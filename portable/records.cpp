#include "records.h"
#include "ecl.h"
#include "item_names.h"
#include "frua_item_names.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <stdexcept>

namespace goldbox {
namespace {
constexpr unsigned monster_size = 285;
constexpr unsigned item_size = 63;

std::string upper(std::string name)
{
    for (auto& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

std::string name_at(const std::uint8_t* data, unsigned capacity)
{
    std::string name;
    for (unsigned i = 1; i <= std::min<unsigned>(data[0], capacity); ++i) {
        const auto c = data[i];
        name += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    return name;
}

std::vector<std::uint8_t> read_save(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Unable to open DOS save component");
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    if (file.bad())
        throw std::runtime_error("Unable to read DOS save component");
    return bytes;
}

unsigned word(const std::uint8_t* data, unsigned offset)
{
    return data[offset] | (unsigned(data[offset + 1]) << 8);
}

int signed_byte(unsigned byte) { return byte < 128 ? byte : static_cast<int>(byte) - 256; }
int signed_word(unsigned value) { return static_cast<int>(value) - (value >= 0x8000 ? 0x10000 : 0); }

void field(RecordRow& row, const char* label, long long value)
{
    row.fields.emplace_back(label, std::to_string(value));
}

bool merged(const std::vector<DaxRecord>& records)
{
    return std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 253 && !record.bytes.empty() && record.bytes.size() % 9 == 0;
    }) && std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 255 && record.bytes.size() == 5;
    });
}

RecordRow monster(unsigned id, const std::uint8_t* b)
{
    RecordRow row{id, 0, name_at(b, 15), {}};
    if (row.name.empty())
        row.name = "Monster " + std::to_string(id);
    field(row, "Record ID", id);
    constexpr const char* stats[] = {"Strength", "Intelligence", "Wisdom",
                                     "Dexterity", "Constitution", "Charisma"};
    for (unsigned i = 0; i < 6; ++i)
        field(row, stats[i], b[16 + i]);
    field(row, "Exceptional strength", b[22]);
    field(row, "Race code", signed_byte(b[46]));
    field(row, "Class code", signed_byte(b[47]));
    field(row, "Age", word(b, 48));
    field(row, "Maximum HP", b[50]);
    field(row, "Current HP", b[283]);
    field(row, "Base THAC0", 60 - b[45]);
    field(row, "Current THAC0", 60 - b[272]);
    field(row, "Base AC", 60 - b[169]);
    field(row, "Current AC", 60 - b[273]);
    field(row, "Base movement", b[114]);
    field(row, "Current movement", b[284]);
    field(row, "Highest class level", signed_byte(b[115]));
    field(row, "Undead level", signed_byte(b[118]));
    field(row, "Alignment code", b[160]);
    field(row, "Attack level", b[107]);
    field(row, "Primary attacks per two rounds", b[161]);
    field(row, "Secondary attacks per two rounds", b[162]);
    for (unsigned i = 0; i < 2; ++i) {
        auto damage = std::to_string(b[163 + i]) + "d" + std::to_string(b[165 + i]);
        const auto modifier = signed_byte(b[167 + i]);
        if (modifier)
            damage += (modifier > 0 ? "+" : "") + std::to_string(modifier);
        row.fields.emplace_back("Unarmed damage " + std::to_string(i + 1), std::move(damage));
    }
    field(row, "XP award", word(b, 184));
    field(row, "XP bonus per HP", b[186]);
    field(row, "Inventory count (stored)", b[199]);
    return row;
}

struct MonsterLayout {
    unsigned size, thac0, race, age, max_hp, movement, alignment, attacks, ac, xp, items, current_thac0;
};

constexpr MonsterLayout later_monsters[]{
    {422, 115, 116, 118, 120, 228, 283, 284, 292, 316, 332, 409},
    {439, 106, 107, 110, 112, 237, 288, 289, 297, 334, 352, 426},
    {510, 172, 173, 176, 178, 311, 359, 360, 368, 408, 422, 497},
};

bool known_monster_size(std::size_t size)
{
    return size == monster_size || size == 259 || size == 263 ||
        std::any_of(std::begin(later_monsters), std::end(later_monsters),
                    [size](const auto& layout) { return layout.size == size; });
}

RecordRow read_monster(const DaxRecord& record)
{
    const auto size = record.bytes.size();
    const auto* b = record.bytes.data();
    if (size == monster_size)
        return monster(record.id, b);
    if (!known_monster_size(size))
        throw std::runtime_error("Unsupported DOS monster record size: " + std::to_string(size));
    RecordRow row{record.id, 0, name_at(b, 15), {}};
    if (row.name.empty())
        row.name = "Monster " + std::to_string(record.id);
    field(row, "Record ID", record.id);
    field(row, "Record size", size);
    if (size == 259 || size == 263) {
        constexpr const char* stats[]{"Strength", "Dexterity", "Constitution", "Intelligence",
                                     "Wisdom", "Charisma", "Tech"};
        for (unsigned i = 0; i < 7; ++i) {
            row.fields.emplace_back(std::string(stats[i]) + " (base)", std::to_string(b[16 + i]));
            row.fields.emplace_back(std::string(stats[i]) + " (current)", std::to_string(b[23 + i]));
        }
        field(row, "Race code", signed_byte(b[0x27]));
        field(row, "Career code", signed_byte(b[0x28]));
        field(row, "Level", b[0x29]);
        field(row, "Maximum HP", word(b, 0x45));
        field(row, "Current HP", word(b, 0xe3));
        field(row, "Base movement", b[0x36]);
        field(row, "Current movement", b[0xe5]);
        field(row, "XP award", word(b, 0xb2));
        field(row, "Combat icon ID", b[0xb8]);
        return row;
    }
    const auto& layout = *std::find_if(std::begin(later_monsters), std::end(later_monsters),
                                     [size](const auto& entry) { return entry.size == size; });
    constexpr const char* stats[]{"Strength", "Intelligence", "Wisdom", "Dexterity", "Constitution", "Charisma"};
    for (unsigned i = 0; i < 6; ++i) {
        row.fields.emplace_back(std::string(stats[i]) + " (base)", std::to_string(b[16 + 2 * i]));
        row.fields.emplace_back(std::string(stats[i]) + " (current)", std::to_string(b[17 + 2 * i]));
    }
    field(row, "Exceptional strength", b[28]);
    field(row, "Race code", signed_byte(b[layout.race]));
    field(row, "Class code", signed_byte(b[layout.race + 1]));
    field(row, "Age", word(b, layout.age));
    field(row, "Maximum HP", b[layout.max_hp]);
    field(row, "Current HP", b[size - 2]);
    field(row, "Base THAC0", 60 - b[layout.thac0]);
    field(row, "Current THAC0", 60 - b[layout.current_thac0]);
    field(row, "Base AC", 60 - b[layout.ac]);
    field(row, "Current AC", 60 - b[layout.current_thac0 + 1]);
    field(row, "Base movement", b[layout.movement]);
    field(row, "Current movement", b[size - 1]);
    field(row, "Alignment code", b[layout.alignment]);
    field(row, "Primary attacks (stored)", b[layout.attacks]);
    field(row, "XP award", word(b, layout.xp));
    field(row, "Inventory count (stored)", b[layout.items]);
    return row;
}

RecordRow item(unsigned id, unsigned slot, const std::uint8_t* b, const std::string* owner, bool pool_names = true)
{
    RecordRow row{id, slot, name_at(b, 40), {}};
    std::string resolved;
    for (unsigned offset : {49, 48, 47}) {
        const auto component = pool_names ? item_components[b[offset]] : std::string_view{};
        if (!component.empty()) {
            if (!resolved.empty())
                resolved += ' ';
            resolved += component;
        }
    }
    const auto base = pool_names && b[46] < std::size(item_templates) ? item_templates[b[46]] : std::string_view{};
    if (row.name.empty()) {
        row.name = resolved;
        if (row.name.empty())
            row.name = base.empty() ? "Item type " + std::to_string(b[46]) : std::string(base);
    }
    if (owner)
        row.fields.emplace_back("Owner", *owner);
    if (!resolved.empty())
        row.fields.emplace_back("Full name from components", resolved);
    field(row, "Record ID", id);
    field(row, "Item slot", slot + 1);
    field(row, "Base item type", b[46]);
    if (!base.empty())
        row.fields.emplace_back("Base item name", base);
    row.fields.emplace_back("Name component IDs", std::to_string(b[49]) + ", "
        + std::to_string(b[48]) + ", " + std::to_string(b[47]));
    field(row, "Magic bonus", signed_byte(b[50]));
    field(row, "Saving throw bonus", signed_byte(b[51]));
    field(row, "Equipped", b[52]);
    field(row, "Unidentified name mask", b[53]);
    field(row, "Cursed", b[54]);
    field(row, "Weight (coins)", word(b, 55));
    field(row, "Quantity (stored)", b[57]);
    field(row, "Value (gold)", word(b, 58));
    field(row, "Charges / first spell", b[60]);
    field(row, "Effect / second spell", b[61]);
    field(row, "Special ID / third spell", b[62]);
    return row;
}

RecordRow compact_item(unsigned id, unsigned slot, const std::uint8_t* b, unsigned stride,
                       const std::string* owner)
{
    const bool buck = stride == 62 || stride == 16;
    RecordRow row{id, slot, stride == 62 ? name_at(b, 40) : std::string{}, {}};
    if (stride == 62)
        b += 46;
    if (stride <= 17 && b[0] == 255) {
        row.name = "Empty template";
        field(row, "Record ID", id);
        field(row, "Template index", slot + 1);
        field(row, "Base item type", b[0]);
        return row;
    }
    if (!buck) {
        for (unsigned offset : {3u, 2u, 1u}) {
            const auto code = b[offset];
            if (!code)
                continue;
            if (!row.name.empty())
                row.name += ' ';
            // The two 510-byte engines share these words. ID 115 is game-specific.
            if (code <= 125 && code != 115)
                row.name += frua_item_components[code];
            else
                row.name += '[' + std::to_string(code) + ']';
        }
    }
    if (row.name.empty())
        row.name = "Item type " + std::to_string(b[0]);
    if (owner)
        row.fields.emplace_back("Owner", *owner);
    field(row, "Record ID", id);
    field(row, "Item slot", slot + 1);
    field(row, "Base item type", b[0]);
    row.fields.emplace_back("Name component IDs", std::to_string(b[3]) + ", " +
                            std::to_string(b[2]) + ", " + std::to_string(b[1]));
    field(row, "Magic bonus", signed_byte(b[4]));
    field(row, "Saving throw bonus", signed_byte(b[5]));
    field(row, "Equipped", buck ? (b[6] & 128) != 0 : b[6]);
    field(row, "Unidentified name mask", b[7]);
    if (!buck)
        field(row, "Cursed", b[8]);
    field(row, "Weight", word(b, buck ? 8 : 9));
    field(row, "Quantity", b[buck ? 10 : 11]);
    field(row, buck ? "Value (credits)" : "Value", word(b, buck ? 11 : 12));
    for (unsigned i = 0; i < 3; ++i)
        row.fields.emplace_back("Special byte " + std::to_string(i + 1), std::to_string(b[(buck ? 13 : 14) + i]));
    return row;
}

constexpr unsigned effect_size = 9;

RecordRow effect(unsigned id, unsigned slot, const std::uint8_t* b, const std::string* owner)
{
    RecordRow row{id, slot, "Effect " + std::to_string(b[0]), {}};
    if (owner)
        row.fields.emplace_back("Owner", *owner);
    field(row, "Effect ID", b[0]);
    field(row, "Duration (stored)", word(b, 1));
    row.fields.emplace_back("Timing", word(b, 1) ? "Timed or effect-specific" : "Permanent");
    field(row, "Strength / effect data", b[3]);
    field(row, "Run handler on removal", b[4]);
    return row;
}

std::vector<RecordRow> read_vault(const std::filesystem::path& path)
{
    constexpr unsigned header_size = 16;
    constexpr unsigned record_size = 18;
    constexpr unsigned padded_records = 50;
    const auto bytes = read_save(path);
    const auto* data = bytes.data();
    if (bytes.size() < header_size + padded_records * record_size ||
        (bytes.size() - header_size) % record_size != 0 || word(data, 12) != 0xffff)
        throw std::runtime_error("Expected a Forgotten Realms Unlimited Adventures DOS vault");
    const auto available = (bytes.size() - header_size) / record_size;
    const auto roots = word(data, 14);
    if (roots > available)
        throw std::runtime_error("Invalid FRUA vault item count");

    RecordRow summary{0, 0, "Vault contents", {}};
    field(summary, "Platinum pieces", word(data, 0) | (std::uint32_t(word(data, 2)) << 16));
    field(summary, "Gems", word(data, 4) | (std::uint32_t(word(data, 6)) << 16));
    field(summary, "Jewels", word(data, 8) | (std::uint32_t(word(data, 10)) << 16));
    field(summary, "Stored items and bundles", roots);
    std::vector<RecordRow> rows;
    rows.push_back(std::move(summary));

    std::size_t current = 0;
    auto next_item = [&](unsigned parent) {
        if (current >= available)
            throw std::runtime_error("Truncated FRUA vault item or bundle");
        const auto* item = data + header_size + record_size * current++;
        const auto slot = static_cast<unsigned>(current);
        RecordRow row{item[0], slot - 1, {}, {}};
        // The game concatenates the saved component IDs in reverse byte order.
        for (unsigned byte : {3u, 2u, 1u}) {
            const auto id = item[byte];
            if (!id)
                continue;
            if (!row.name.empty())
                row.name += ' ';
            if (id < std::size(frua_item_components))
                row.name += frua_item_components[id];
            else
                row.name += '[' + std::to_string(id) + ']';
        }
        if (row.name.empty())
            row.name = item[0] == 'I' && !parent ? "Bundle" : "Item " + std::to_string(slot);
        if (parent)
            field(row, "Part of bundle record", parent);
        field(row, "Stored record", slot);
        field(row, "Item type", item[0]);
        row.fields.emplace_back("Name component IDs", std::to_string(item[3]) + ", " +
            std::to_string(item[2]) + ", " + std::to_string(item[1]));
        field(row, "Weight", signed_word(word(item, 4)));
        field(row, "Value", signed_word(word(item, 6)));
        field(row, "Magic bonus", signed_byte(item[8]));
        field(row, "Saving throw bonus", signed_byte(item[9]));
        field(row, "Readied", item[10]);
        field(row, "Unidentified name mask", item[11]);
        field(row, "Cursed", item[12]);
        field(row, item[0] == 'I' && !parent ? "Bundle contents" : "Quantity", item[13]);
        rows.push_back(std::move(row));
        return item;
    };
    for (unsigned i = 0; i < roots; ++i) {
        const auto* root = next_item(0);
        const auto parent = static_cast<unsigned>(current);
        if (root[0] == 'I') {
            for (unsigned child = 0; child < root[13]; ++child)
                next_item(parent);
        }
    }
    if (available != std::max<std::size_t>(current, padded_records))
        throw std::runtime_error("FRUA vault size does not match its stored records");
    field(rows.front(), "Records including bundle contents", current);
    return rows;
}

std::vector<RecordRow> read_save_rows(const std::filesystem::path& path, RecordKind kind)
{
    const auto bytes = read_save(path);
    if (kind == RecordKind::saved_character) {
        if (bytes.size() != monster_size)
            throw std::runtime_error("Expected a 285-byte Pool of Radiance DOS character save");
        auto row = monster(0, bytes.data());
        row.fields.front() = {"Save file", path.filename().u8string()};
        return {std::move(row)};
    }
    const bool effects = kind == RecordKind::saved_effects;
    const auto stride = effects ? effect_size : item_size;
    if (bytes.size() % stride)
        throw std::runtime_error(effects ? "Expected 9-byte Pool of Radiance DOS effect entries"
                                        : "Expected 63-byte Pool of Radiance DOS inventory entries");
    std::string owner;
    const auto folder = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const auto companion = upper(path.stem().string()) + ".SAV";
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (upper(entry.path().filename().string()) != companion)
            continue;
        try {
            const auto character = read_save(entry.path());
            if (character.size() == monster_size)
                owner = name_at(character.data(), 15);
        } catch (const std::exception&) {
            // Inventory can be inspected on its own.
        }
        break;
    }
    std::vector<RecordRow> rows;
    for (unsigned slot = 0; slot < bytes.size() / stride; ++slot) {
        const auto* b = bytes.data() + slot * stride;
        if (!effects) {
            rows.push_back(item(0, slot, b, owner.empty() ? nullptr : &owner));
            continue;
        }
        rows.push_back(effect(b[0], slot, b, owner.empty() ? nullptr : &owner));
    }
    return rows;
}
}

RecordKind record_kind(const std::filesystem::path& path)
{
    if (is_ecl_archive(path))
        return RecordKind::ecl;
    const auto name = upper(path.filename().string());
    static const std::regex monster_name(R"(MON[0-9]*(CHA|ITM|SPC)\.DAX)");
    std::smatch match;
    if (std::regex_match(name, match, monster_name)) {
        if (match[1] == "CHA") return RecordKind::monsters;
        if (match[1] == "ITM") return RecordKind::inventory;
        return RecordKind::monster_effects;
    }
    static const std::regex item_name(R"(ITEM[0-9]*\.DAX)");
    if (std::regex_match(name, item_name)) return RecordKind::items;
    const auto extension = upper(path.extension().string());
    const auto stem = upper(path.stem().string());
    if (extension == ".DAT" && (stem == "VAULT" ||
        (stem.size() == 6 && stem.compare(0, 5, "VAULT") == 0 && stem.back() >= 'A' && stem.back() <= 'Z')))
        return RecordKind::vault;
    if (extension == ".SAV") return RecordKind::saved_character;
    if (extension == ".ITM") return RecordKind::saved_inventory;
    if (extension == ".SPC") return RecordKind::saved_effects;
    return RecordKind::none;
}

std::vector<RecordRow> read_record_rows(const std::filesystem::path& path)
{
    const auto kind = record_kind(path);
    if (kind == RecordKind::none)
        throw std::runtime_error("Unrecognized DOS record archive");
    if (kind == RecordKind::vault)
        return read_vault(path);
    if (kind == RecordKind::ecl)
        return read_ecl_rows(path);
    if (kind == RecordKind::saved_character || kind == RecordKind::saved_inventory ||
        kind == RecordKind::saved_effects)
        return read_save_rows(path, kind);
    const auto records = read_dax(path);
    const bool trailer = merged(records);
    const auto filename = upper(path.filename().string());
    std::map<unsigned, std::string> owners;
    std::size_t owner_size = 0;
    if (kind == RecordKind::inventory || kind == RecordKind::monster_effects ||
        (kind == RecordKind::items && filename != "ITEM.DAX" && filename != "ITEM0.DAX")) {
        const auto folder = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
        const auto companion = kind == RecordKind::items
            ? "MON" + filename.substr(4, filename.size() - 8) + "CHA.DAX"
            : filename.substr(0, filename.size() - 7) + "CHA.DAX";
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (upper(entry.path().filename().string()) != companion)
                continue;
            try {
                for (const auto& record : read_dax(entry.path())) {
                    if (known_monster_size(record.bytes.size())) {
                        owner_size = record.bytes.size();
                        if (kind != RecordKind::items)
                            owners.emplace(record.id, name_at(record.bytes.data(), 15));
                    }
                }
            } catch (const std::exception&) {
                // The inventory remains readable when its companion is unavailable.
            }
            break;
        }
    }
    unsigned stride = item_size;
    const auto fits = [&](unsigned width) {
        return std::all_of(records.begin(), records.end(), [&](const auto& record) {
            return (trailer && record.id >= 253) || record.bytes.size() % width == 0;
        });
    };
    if (kind == RecordKind::monster_effects) {
        stride = effect_size;
    } else if (kind == RecordKind::items && filename == "ITEM0.DAX") {
        // Later games store an ITEM0 template table rather than named item instances.
        const auto block = std::find_if(records.begin(), records.end(), [](const auto& record) {
            return record.id == 1 && !record.bytes.empty();
        });
        if (block != records.end() && block->bytes.size() == 127 * 16 && fits(16))
            stride = 16;
        else if (block != records.end() && (block->bytes.size() == 127 * 17 ||
                 block->bytes.size() == 254 * 17) && fits(17))
            stride = 17;
        else if (!fits(item_size)) {
            if (fits(17) == fits(16))
                throw std::runtime_error("Unrecognized DOS ITEM0 template record size");
            stride = fits(17) ? 17 : 16;
        }
    } else if (kind == RecordKind::inventory) {
        if (owner_size == 510) {
            stride = 17;
        } else if (owner_size == 259 || owner_size == 263 || filename == "MON0ITM.DAX") {
            stride = 62;
        } else if (!owner_size && filename != "MONITM.DAX") {
            if (!fits(item_size) && fits(17))
                stride = 17;
        }
    }
    std::vector<RecordRow> rows;
    for (const auto& record : records) {
        if (record.bytes.empty() || (trailer && record.id >= 253))
            continue;
        if (kind == RecordKind::monsters) {
            rows.push_back(read_monster(record));
        } else {
            if (record.bytes.size() % stride)
                throw std::runtime_error("Unexpected DOS item or effect record size");
            const auto found = owners.find(record.id);
            const auto* owner = found == owners.end() || found->second.empty() ? nullptr : &found->second;
            for (unsigned slot = 0; slot < record.bytes.size() / stride; ++slot) {
                const auto* b = record.bytes.data() + slot * stride;
                if (kind == RecordKind::monster_effects)
                    rows.push_back(effect(record.id, slot, b, owner));
                else if (stride != item_size)
                    rows.push_back(compact_item(record.id, slot, b, stride, owner));
                else
                    rows.push_back(item(record.id, slot, b, owner, owner_size == monster_size ||
                        (!owner_size && (kind != RecordKind::items || filename == "ITEM.DAX"))));
            }
        }
    }
    return rows;
}
}
