#include "records.h"
#include "item_names.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
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

RecordRow item(unsigned id, unsigned slot, const std::uint8_t* b, const std::string* owner)
{
    RecordRow row{id, slot, name_at(b, 40), {}};
    std::string resolved;
    for (unsigned offset : {49, 48, 47}) {
        const auto component = item_components[b[offset]];
        if (!component.empty()) {
            if (!resolved.empty())
                resolved += ' ';
            resolved += component;
        }
    }
    const auto base = b[46] < std::size(item_templates) ? item_templates[b[46]] : std::string_view{};
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
    constexpr unsigned effect_size = 9;
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
        RecordRow row{b[0], slot, "Effect " + std::to_string(b[0]), {}};
        if (!owner.empty())
            row.fields.emplace_back("Owner", owner);
        field(row, "Effect ID", b[0]);
        field(row, "Duration (stored)", word(b, 1));
        row.fields.emplace_back("Timing", word(b, 1) ? "Timed or effect-specific" : "Permanent");
        field(row, "Strength / effect data", b[3]);
        field(row, "Run handler on removal", b[4]);
        rows.push_back(std::move(row));
    }
    return rows;
}
}

RecordKind record_kind(const std::filesystem::path& path)
{
    const auto name = upper(path.filename().string());
    if (name == "MONCHA.DAX") return RecordKind::monsters;
    if (name == "MONITM.DAX") return RecordKind::inventory;
    if (name == "ITEM.DAX") return RecordKind::items;
    const auto extension = upper(path.extension().string());
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
    if (kind == RecordKind::saved_character || kind == RecordKind::saved_inventory ||
        kind == RecordKind::saved_effects)
        return read_save_rows(path, kind);
    const auto records = read_dax(path);
    const bool trailer = merged(records);
    std::map<unsigned, std::string> owners;
    if (kind == RecordKind::inventory) {
        const auto folder = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (record_kind(entry.path()) != RecordKind::monsters)
                continue;
            try {
                for (const auto& record : read_dax(entry.path())) {
                    if (record.bytes.size() == monster_size)
                        owners.emplace(record.id, name_at(record.bytes.data(), 15));
                }
            } catch (const std::exception&) {
                // The inventory remains readable when its companion is unavailable.
            }
            break;
        }
    }
    std::vector<RecordRow> rows;
    for (const auto& record : records) {
        if (trailer && record.id >= 253)
            continue;
        if (kind == RecordKind::monsters) {
            if (record.bytes.size() != monster_size)
                throw std::runtime_error("Unexpected DOS character record size");
            rows.push_back(monster(record.id, record.bytes.data()));
        } else {
            if (record.bytes.size() % item_size)
                throw std::runtime_error("Unexpected DOS item record size");
            const auto owner = owners.find(record.id);
            for (unsigned slot = 0; slot < record.bytes.size() / item_size; ++slot)
                rows.push_back(item(record.id, slot, record.bytes.data() + slot * item_size,
                                    owner == owners.end() ? nullptr : &owner->second));
        }
    }
    return rows;
}
}
