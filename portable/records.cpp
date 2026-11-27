#include "records.h"
#include "c64.h"
#include "ecl.h"
#include "ecl_containers.h"
#include "item_names.h"
#include "frua_item_names.h"
#include "pc98.h"
#include "tlb.h"
#include "xmi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
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

std::string name_at(const std::uint8_t* data, unsigned capacity, bool pc98 = false)
{
    const auto length = std::min<unsigned>(data[0], capacity);
    if (pc98)
        return pc98_text({reinterpret_cast<const char*>(data + 1), length});
    std::string name;
    for (unsigned i = 1; i <= length; ++i) {
        const auto c = data[i];
        name += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    return name;
}

std::vector<std::uint8_t> read_record_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Unable to open record file");
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    if (file.bad())
        throw std::runtime_error("Unable to read record file");
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

std::string amiga_name(const std::uint8_t* bytes, unsigned count)
{
    std::string name;
    for (unsigned i = 0; i < count && bytes[i]; ++i) {
        const auto c = bytes[i];
        if (c >= 128) {
            name += static_cast<char>(0xc0 | (c >> 6));
            name += static_cast<char>(0x80 | (c & 63));
        } else {
            name += c >= 32 ? static_cast<char>(c) : '?';
        }
    }
    return name;
}

unsigned amiga_word(const std::uint8_t* bytes, unsigned at)
{
    return unsigned(bytes[at]) << 8 | bytes[at + 1];
}

std::string raw_bytes(const std::uint8_t* bytes, unsigned at, unsigned size)
{
    std::ostringstream text;
    text << std::uppercase << std::hex << std::setfill('0');
    for (unsigned i = 0; i < size; ++i) {
        if (i)
            text << ' ';
        text << std::setw(2) << unsigned(bytes[at + i]);
    }
    return text.str();
}

void amiga_field(RecordRow& row, unsigned offset, std::string_view name, std::string value)
{
    std::ostringstream label;
    label << '$' << std::uppercase << std::hex << std::setfill('0') << std::setw(4) << offset << "  " << name;
    row.fields.emplace_back(label.str(), std::move(value));
}

bool amiga_character_size(std::size_t size)
{
    return size == 287 || size == 288;
}

RecordRow amiga_monster(const DaxRecord& record)
{
    if (!amiga_character_size(record.bytes.size()))
        throw std::runtime_error("Unsupported Amiga MONCHA character size");
    const auto* b = record.bytes.data();
    RecordRow row{record.id, 0, amiga_name(b, 16), {{"Format", "Amiga character"}}};
    amiga_field(row, 0, "Name", row.name);
    if (row.name.empty())
        row.name = "Monster " + std::to_string(record.id);
    // Offsets in SSI's PC character schema, adjusted for both native alignment bytes.
    constexpr unsigned offsets[]{
        16,17,18,19,20,21,22,23,44,45,46,47,48,50,51,107,108,109,114,115,116,117,118,119,
        127,131,132,133,134,135,136,150,158,159,160,161,162,163,165,167,169,170,171,172,
        176,177,178,181,184,186,187,188,189,190,191,192,193,199,200,204,256,257,258,
        260,264,268,269,270,271,272,273,274,275,276,277,279,281,283,284,285
    };
    constexpr std::string_view names[]{
        "Strength","Intelligence","Wisdom","Dexterity","Constitution","Charisma","Exceptional strength",
        "Memorized spells","Spell preparation time","Base THAC0","Race","Class","Age","Maximum HP",
        "Known spells","Attack level","Icon dimensions","Saving throws","Base movement","Highest level",
        "Drained levels","Drained HP","Undead level","Thief skills","Effects pointer","Unknown 083",
        "NPC","NPC treasure share","Unknown 086","Unknown 087","Coins","Class levels","Gender",
        "Creature type","Alignment","Attacks","Secondary attacks","Unarmed rolls","Unarmed dice",
        "Unarmed modifier","Base AC","Use strength","Random byte","Experience","Item limits","Rolled HP",
        "Cleric spell slots","Mage spell slots","XP award","XP bonus per HP","Portrait head",
        "Portrait body","Icon head","Icon body","Party order","Icon size","Icon colors","Item count",
        "Items pointer","Equipped pointers","Hands equipped","Save bonus","Encumbrance",
        "Next character pointer","Combat pointer","Status","Enabled","Hostile","Quick fight","Current THAC0",
        "Current AC","Rear AC","Current attacks","Secondary current attacks","Current damage rolls",
        "Current damage dice","Current damage modifier","Current HP","Current movement"
    };
    static_assert(std::size(offsets) == std::size(names) + 1);
    for (unsigned i = 0; i < std::size(names); ++i) {
        const auto pc = offsets[i];
        const auto at = pc + (pc >= 127) + (pc >= 136);
        const auto size = offsets[i + 1] - pc;
        std::string value;
        if (pc == 48 || pc == 184 || pc == 258)
            value = std::to_string(amiga_word(b, at));
        else if (pc == 172)
            value = std::to_string(std::uint32_t(amiga_word(b, at)) << 16 | amiga_word(b, at + 2));
        else if (pc == 136) {
            for (unsigned coin = 0; coin < 7; ++coin) {
                if (coin)
                    value += ", ";
                value += std::to_string(amiga_word(b, at + 2 * coin));
            }
        } else if (size == 1) {
            const bool signed_value = pc == 46 || pc == 47 || pc == 115 || pc == 118 || pc == 270;
            value = std::to_string(signed_value ? signed_byte(b[at]) : int(b[at]));
        } else {
            value = raw_bytes(b, at, size);
        }
        if (at == 128 || at == 138)
            amiga_field(row, at - 1, "Alignment padding", std::to_string(b[at - 1]));
        amiga_field(row, at, names[i], std::move(value));
    }
    if (record.bytes.size() == 288)
        amiga_field(row, 287, "Tail padding", std::to_string(b[287]));
    return row;
}

RecordRow amiga_item(unsigned id, unsigned slot, const std::uint8_t* b)
{
    RecordRow row{id, slot, amiga_name(b, 41), {{"Format", "Amiga item"}}};
    amiga_field(row, 0, "Name", row.name);
    if (row.name.empty())
        row.name = "Item type " + std::to_string(b[46]);
    amiga_field(row, 41, "In use", std::to_string(b[41]));
    amiga_field(row, 42, "Runtime next pointer", raw_bytes(b, 42, 4));
    constexpr std::string_view names[]{
        "Type","Name component 3","Name component 2","Name component 1","Magic bonus",
        "Save bonus","Readied","Unidentified mask","Cursed","Alignment padding"
    };
    for (unsigned i = 0; i < std::size(names); ++i)
        amiga_field(row, 46 + i, names[i],
                    std::to_string(i == 4 || i == 5 ? signed_byte(b[46 + i]) : int(b[46 + i])));
    amiga_field(row, 56, "Weight", std::to_string(amiga_word(b, 56)));
    amiga_field(row, 58, "Quantity", std::to_string(b[58]));
    amiga_field(row, 59, "Alignment padding", std::to_string(b[59]));
    amiga_field(row, 60, "Value", std::to_string(amiga_word(b, 60)));
    amiga_field(row, 62, "Charges / scroll spells", std::to_string(b[62]));
    amiga_field(row, 63, "Effect", std::to_string(b[63]));
    amiga_field(row, 64, "Special", std::to_string(b[64]));
    return row;
}

RecordRow amiga_effect(unsigned id, unsigned slot, const std::uint8_t* b)
{
    RecordRow row{id, slot, "Effect " + std::to_string(b[0]), {{"Format", "Amiga effect"}}};
    amiga_field(row, 0, "Effect ID", std::to_string(b[0]));
    amiga_field(row, 1, "Padding", std::to_string(b[1]));
    amiga_field(row, 2, "Duration in minutes (0 permanent)", std::to_string(amiga_word(b, 2)));
    amiga_field(row, 4, "Strength", std::to_string(b[4]));
    amiga_field(row, 5, "Removal callback", b[5] ? "Yes" : "No");
    amiga_field(row, 6, "Runtime next pointer", raw_bytes(b, 6, 4));
    return row;
}

std::vector<RecordRow> amiga_record_rows(const std::vector<DaxRecord>& records, RecordKind kind,
                                         const std::map<unsigned, std::string>& owners)
{
    std::vector<RecordRow> rows;
    const bool effects = kind == RecordKind::monster_effects;
    const unsigned stride = effects ? 10 : 65;
    for (const auto& record : records) {
        if (record.bytes.empty())
            continue;
        if (kind == RecordKind::monsters) {
            rows.push_back(amiga_monster(record));
            continue;
        }
        if (record.bytes.size() % stride)
            throw std::runtime_error("Unsupported Amiga item or effect record size");
        const auto owner = owners.find(record.id);
        for (unsigned slot = 0; slot < record.bytes.size() / stride; ++slot) {
            const auto* b = record.bytes.data() + slot * stride;
            auto row = effects ? amiga_effect(record.id, slot, b) : amiga_item(record.id, slot, b);
            if (owner != owners.end() && !owner->second.empty())
                row.fields.emplace(row.fields.begin() + 1, "Owner", owner->second);
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::vector<std::uint8_t> unpack_monster_packbits(const std::vector<std::uint8_t>& packed, unsigned start)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t at = start; at < packed.size();) {
        const auto code = packed[at++];
        if (code == 128)
            continue;
        const unsigned count = code < 128 ? code + 1 : 257 - code;
        const unsigned input = code < 128 ? count : 1;
        if (input > packed.size() - at || count > 4096 - bytes.size())
            return {};
        if (code < 128)
            bytes.insert(bytes.end(), packed.begin() + at, packed.begin() + at + input);
        else
            bytes.insert(bytes.end(), count, packed[at]);
        at += input;
    }
    return bytes;
}

RecordRow later_amiga_monster(const DaxRecord& record)
{
    const auto& original = record.bytes;
    std::vector<std::uint8_t> expanded;
    unsigned name_offset = 0;
    if (original.size() != 422 && original.size() != 440) {
        if (original.size() >= 2 && original[0] >= 1 && original[0] <= 4) {
            expanded = unpack_monster_packbits(original, 2);
            if (expanded.size() == amiga_word(original.data(), 0) && expanded.size() >= 124)
                name_offset = 96;
            else
                expanded.clear();
        }
        if (expanded.empty()) {
            expanded = unpack_monster_packbits(original, 0);
            if (expanded.size() != 422 && expanded.size() != 440)
                throw std::runtime_error("Unsupported packed Amiga monster record");
        }
    }
    const auto& bytes = expanded.empty() ? original : expanded;
    const auto* b = bytes.data();
    auto name = name_offset ? amiga_name(b + name_offset, 16) : amiga_name(b + 1, std::min<unsigned>(b[0], 15));
    RecordRow row{record.id, 0, std::move(name), {{"Format", "Amiga GLIB creature"}}};
    if (row.name.empty())
        row.name = "Monster " + std::to_string(record.id);
    field(row, "Stored bytes", original.size());
    field(row, "Decoded bytes", bytes.size());
    constexpr const char* stats[]{"Strength", "Intelligence", "Wisdom", "Dexterity", "Constitution", "Charisma"};
    for (unsigned i = 0; i < std::size(stats); ++i) {
        const auto at = name_offset + 16 + 2 * i;
        amiga_field(row, at, std::string(stats[i]) + " (base)", std::to_string(b[at]));
        amiga_field(row, at + 1, std::string(stats[i]) + " (current)", std::to_string(b[at + 1]));
    }
    for (unsigned at = 0; at < bytes.size(); at += 16)
        amiga_field(row, at, "Decoded bytes", raw_bytes(b, at, std::min<unsigned>(16, bytes.size() - at)));
    return row;
}

std::vector<RecordRow> dark_queen_monsters(const std::filesystem::path& path)
{
    constexpr unsigned character_size = 0x19e, effect_width = 5, item_width = 17;
    std::vector<RecordRow> rows;
    for (const auto& record : read_hlib_data(path)) {
        if (record.bytes.empty())
            continue;
        if (record.bytes.size() < 2)
            throw std::runtime_error("Truncated Dark Queen monster record");
        const auto decoded = unpack_monster_packbits(record.bytes, 2);
        const auto* b = decoded.data();
        if (decoded.size() != amiga_word(record.bytes.data(), 0) || decoded.size() < character_size + 2)
            throw std::runtime_error("Invalid Dark Queen monster record length for ID " + std::to_string(record.id));
        const auto effects = b[character_size];
        const auto items = b[character_size + 1];
        if (decoded.size() < character_size + 2 + effects * effect_width + items * item_width)
            throw std::runtime_error("Truncated Dark Queen monster inventory or effects");
        const auto* name = reinterpret_cast<const char*>(b + 0x68);
        RecordRow row{record.id, 0, std::string(name, std::find(name, name + 16, '\0')), {}};
        if (row.name.empty())
            row.name = "Monster " + std::to_string(record.id);
        field(row, "Record ID", record.id);
        field(row, "Record size", character_size);
        constexpr const char* stats[]{"Strength", "Intelligence", "Wisdom", "Dexterity", "Constitution", "Charisma"};
        for (unsigned i = 0; i < std::size(stats); ++i) {
            field(row, (std::string(stats[i]) + " (base)").c_str(), b[0x78 + 2 * i]);
            field(row, (std::string(stats[i]) + " (current)").c_str(), b[0x79 + 2 * i]);
        }
        field(row, "Race code", word(b, 0x58));
        field(row, "Class code", word(b, 0x5a));
        field(row, "Age", word(b, 0x52));
        field(row, "Maximum HP", b[0x89]);
        field(row, "Current HP", b[0x198]);
        field(row, "Base THAC0", 60 - b[0x87]);
        field(row, "Base AC", 60 - b[0xbb]);
        field(row, "Base movement", b[0x90]);
        field(row, "Alignment code", word(b, 0x62));
        field(row, "Primary attacks (stored)", b[0xb3]);
        field(row, "XP award", word(b, 0x54));
        field(row, "Inventory count", items);
        field(row, "Effect count", effects);
        for (unsigned i = 0; i < effects; ++i)
            field(row, ("Effect ID " + std::to_string(i + 1)).c_str(), b[character_size + 2 + i * effect_width]);
        for (unsigned i = 0; i < items; ++i)
            field(row, ("Item type " + std::to_string(i + 1)).c_str(),
                b[character_size + 2 + effects * effect_width + i * item_width]);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<RecordRow> later_amiga_records(const std::filesystem::path& path, RecordKind kind)
{
    const auto records = read_amiga_glb(path, nullptr);
    std::map<unsigned, std::string> owners;
    if (kind == RecordKind::inventory || kind == RecordKind::monster_effects) {
        const auto folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (upper(entry.path().filename().string()) != "MONCHA.GLB")
                continue;
            for (const auto& owner : read_amiga_glb(entry.path(), nullptr)) {
                if (!owner.bytes.empty()) {
                    try {
                        owners.emplace(owner.id, later_amiga_monster(owner).name);
                    } catch (const std::runtime_error&) {
                        // An inventory remains readable when its owner record is unknown.
                    }
                }
            }
            break;
        }
    }
    std::vector<RecordRow> rows;
    for (const auto& record : records) {
        if (record.bytes.empty())
            continue;
        if (kind == RecordKind::monsters) {
            rows.push_back(later_amiga_monster(record));
            continue;
        }
        const bool effect_record = kind == RecordKind::monster_effects;
        const unsigned stride = effect_record ? 9 : 63;
        const auto count = record.bytes.size() / stride;
        const auto trailing = record.bytes.size() % stride;
        if (trailing && (trailing != 1 || !(count * stride & 1)))
            throw std::runtime_error("Unexpected Amiga GLIB record stride");
        for (unsigned slot = 0; slot < count; ++slot) {
            const auto* b = record.bytes.data() + slot * stride;
            RecordRow row{record.id, slot, {}, {{"Format", effect_record ? "Amiga GLIB effect" : "Amiga GLIB item"}}};
            if (const auto owner = owners.find(record.id); owner != owners.end())
                row.fields.emplace_back("Owner", owner->second);
            if (effect_record) {
                row.name = "Effect " + std::to_string(b[0]);
                field(row, "Effect ID", b[0]);
                field(row, "Stored duration", amiga_word(b, 1));
                field(row, "Strength / effect data", b[3]);
                field(row, "Removal callback", b[4]);
                row.fields.emplace_back("Runtime next pointer", raw_bytes(b, 5, 4));
            } else {
                row.name = amiga_name(b + 1, std::min<unsigned>(b[0], 40));
                if (row.name.empty())
                    row.name = "Item type " + std::to_string(b[46]);
                field(row, "Type", b[46]);
                row.fields.emplace_back("Name component IDs", std::to_string(b[49]) + ", " +
                                        std::to_string(b[48]) + ", " + std::to_string(b[47]));
                field(row, "Magic bonus", signed_byte(b[50]));
                field(row, "Save bonus", signed_byte(b[51]));
                field(row, "Readied", b[52]);
                field(row, "Unidentified mask", b[53]);
                field(row, "Cursed", b[54]);
                field(row, "Weight", amiga_word(b, 55));
                field(row, "Quantity", b[57]);
                field(row, "Value", amiga_word(b, 58));
                row.fields.emplace_back("Charges / effect / special", raw_bytes(b, 60, 3));
            }
            row.fields.emplace_back("Stored record bytes", raw_bytes(b, 0, stride));
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

bool merged(const std::vector<DaxRecord>& records)
{
    return std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 253 && !record.bytes.empty() && record.bytes.size() % 9 == 0;
    }) && std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 255 && record.bytes.size() == 5;
    });
}

RecordRow monster(unsigned id, const std::uint8_t* b, bool pc98 = false)
{
    RecordRow row{id, 0, name_at(b, 15, pc98), {}};
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
    {216, 31, 32, 38, 40, 47, 86, 87, 95, 109, 124, 203},
    {409, 89, 90, 96, 98, 213, 266, 267, 275, 304, 322, 396},
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
        return monster(record.id, b, record.pc98);
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

RecordRow death_knights_spells(const DaxRecord& record, const std::string* owner)
{
    constexpr unsigned memorized_count = 74, known_count = 114, slot_bytes = 32;
    if (record.bytes.size() != memorized_count + known_count + slot_bytes)
        throw std::runtime_error("Unexpected Death Knights spell record size");
    // Spell IDs and names from Death Knights of Krynn's gdconsts.pas.
    constexpr std::string_view names[]{
        "", "Bless", "Barkskin", "Cure Light Wounds", "Charm Person/Mammal",
        "Detect Magic", "Protection From Evil", "Ressurection", "Resist Cold", "Burning Hands",
        "Charm Person", "Detect Magic", "Enlarge", "Reduce", "Friends",
        "Magic Missile", "Protection From Evil", "Restoration", "Read Magic", "Shield",
        "Shocking Grasp", "Sleep", "Find Traps", "Hold Person", "Resist Fire",
        "Silence, 15' Radius", "Slow Poison", "Snake Charm", "Spiritual Hammer", "Detect Invisibility",
        "Invisibility", "Knock", "Mirror Image", "Ray of Enfeeblement", "Stinking Cloud",
        "Strength", "", "Cure Blindness", "Cause Blindness", "Cure Disease",
        "Harm", "Dispel Magic", "Prayer", "Remove Curse", "Bestow Curse",
        "Blink", "Dispel Magic", "Fireball", "Haste", "Hold Person",
        "Invisibility, 10' Radius", "Lightning Bolt", "Protection From Evil, 10' Radius", "Heal", "Protection From Normal Missiles",
        "Slow", "Death Spell", "", "Cure Serious Wounds", "",
        "Disentegrate", "", "", "Globe Of Invulnerability", "",
        "", "Stone to Flesh", "Neutralize Poison", "Poison", "Protection Evil, 10' Radius",
        "Sticks to Snakes", "Cure Critical Wounds", "Flesh to Stone", "Dispel Evil", "Flame Strike",
        "Raise Dead", "Slay Living", "Detect Magic", "Entangle", "Faerie Fire",
        "Invisibility to Animals", "Charm Monsters", "Confusion", "Dimension Door", "Fear",
        "Fire Shield", "Fumble", "Ice Storm", "Minor Globe Of Invulnerability", "Remove Curse",
        "Iron Skin", "Cloud Kill", "Cone of Cold", "Feeblemind", "Hold Monsters",
        "", "", "", "Fire Touch", "",
        "Bestow Curse", "Protection From Evil 10' Radius", "Silence 15' radius", "Detect Magic", "Remove Curse",
        "Bless", "Charm Person", "Burning Hands", "Delay Blast Fireball", "Mass Invisibility",
        "Power Word Stun", "Mass Charm", "Mind Blank", "Otto's Irresitable Dance", "Power Word Blind",
    };
    const auto describe = [&](unsigned id) {
        auto name = "Spell " + std::to_string(id);
        if (id < std::size(names) && !names[id].empty())
            name += " - " + std::string(names[id]);
        return name;
    };
    RecordRow row{record.id, 0, owner ? *owner : "Monster " + std::to_string(record.id), {}};
    field(row, "Record ID", record.id);
    const auto& bytes = record.bytes;
    for (unsigned i = 0; i < memorized_count; ++i) {
        const auto id = bytes[i] & 0x7f;
        if (id)
            row.fields.emplace_back("Memorization slot " + std::to_string(i + 1),
                describe(id) + (bytes[i] & 0x80 ? " (pending rest)" : " (ready)"));
    }
    for (unsigned id = 1; id <= known_count; ++id)
        if (bytes[memorized_count + id - 1])
            row.fields.emplace_back("Known spell", describe(id));
    // Several shipped monster blocks have leftover data in the slot table.
    row.fields.emplace_back("Spell slot bytes (stored)",
        raw_bytes(bytes.data(), memorized_count + known_count, slot_bytes));
    return row;
}

RecordRow item(unsigned id, unsigned slot, const std::uint8_t* b, const std::string* owner,
               bool pool_names = true, bool pc98 = false, std::size_t owner_size = 0)
{
    const bool silver_blades = owner_size == 439;
    const bool death_knights = owner_size == 216;
    const bool champions = owner_size == 409;
    const bool generated_name = silver_blades || death_knights || champions;
    RecordRow row{id, slot, generated_name ? std::string{} : name_at(b, 40, pc98), {}};
    std::string resolved;
    for (unsigned offset : {49, 48, 47}) {
        const auto component_id = b[offset];
        std::string_view component;
        if (silver_blades && component_id < std::size(silver_blades_item_components))
            component = silver_blades_item_components[component_id];
        else if (death_knights && component_id < std::size(death_knights_item_components))
            component = death_knights_item_components[component_id];
        else if (champions && component_id < std::size(champions_item_components))
            component = champions_item_components[component_id];
        else if (pool_names && !generated_name)
            component = item_components[component_id];
        if (!component.empty()) {
            if (!resolved.empty())
                resolved += ' ';
            resolved += component;
        }
    }
    const auto base = pool_names && !generated_name && b[46] < std::size(item_templates) ? item_templates[b[46]] : std::string_view{};
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

void base_item_fields(RecordRow& row, const std::uint8_t* item)
{
    field(row, "Equipment slot", item[0]);
    field(row, "Hands required", item[1]);
    field(row, "Large target damage dice count", item[2]);
    field(row, "Large target damage die sides", item[3]);
    field(row, "Large target damage modifier", signed_byte(item[4]));
    field(row, "Rate of fire (doubled)", item[5]);
    field(row, "Armour class descriptor", item[6]);
    field(row, "Weapon class / damage type flags", item[7]);
    field(row, "Small or medium target damage dice count", item[9]);
    field(row, "Small or medium target damage die sides", item[10]);
    field(row, "Small or medium target damage modifier", signed_byte(item[11]));
    field(row, "Range (stored)", item[12]);
    field(row, "Class usability mask", item[13]);
    field(row, "Weapon property flags", item[14]);
}

std::vector<RecordRow> read_base_items(const std::filesystem::path& path)
{
    const auto bytes = read_record_file(path);
    constexpr unsigned gold_box_count = 128, buck_rogers_count = 39;
    constexpr unsigned gold_box_stride = 16, gateway_stride = 24, buck_rogers_stride = 9;
    const bool gateway = bytes.size() == 2 + gold_box_count * gateway_stride;
    const bool buck_rogers = bytes.size() == buck_rogers_count * buck_rogers_stride;
    const bool has_header = gateway || bytes.size() == 2 + gold_box_count * gold_box_stride;
    if (!has_header && !buck_rogers && bytes.size() != gold_box_count * gold_box_stride)
        throw std::runtime_error("Unrecognized standalone ITEMS table size");
    const unsigned stride = gateway ? gateway_stride : buck_rogers ? buck_rogers_stride : gold_box_stride;
    const unsigned count = buck_rogers ? buck_rogers_count : gold_box_count;
    std::vector<RecordRow> rows;
    rows.reserve(count);
    for (unsigned id = 0; id < count; ++id) {
        const unsigned offset = (has_header ? 2 : 0) + id * stride;
        const auto* item = bytes.data() + offset;
        RecordRow row{id, 0, "Base item " + std::to_string(id), {}};
        field(row, "Base item type", id);
        field(row, "File offset", offset);
        if (buck_rogers) {
            field(row, "Equipment slot", item[0]);
            field(row, "Attacks per round (doubled)", item[1]);
            field(row, "Weapon class / armour class code", item[2]);
            field(row, "Heavy equipment", item[3]);
            field(row, "Damage dice count", item[4]);
            field(row, "Damage die sides", item[5]);
            field(row, "Damage modifier", signed_byte(item[6]));
            field(row, "Range", item[7]);
            field(row, "Consumable", item[8]);
        } else {
            base_item_fields(row, item);
        }
        row.fields.emplace_back("Stored bytes", raw_bytes(item, 0, stride));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<RecordRow> read_frua_items(const std::filesystem::path& path)
{
    const auto directory = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::filesystem::path definitions, base;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file())
            continue;
        const auto name = upper(entry.path().filename().string());
        if (name == "ITEM.DAT") definitions = entry.path();
        if (name == "ITEMS.DAT") base = entry.path();
    }
    if (upper(path.filename().string()) == "ITEM.DAT") definitions = path;
    else base = path;
    if (definitions.empty())
        return read_base_items(base);
    const auto items = read_record_file(definitions);
    const auto base_items = base.empty() ? std::vector<std::uint8_t>{} : read_record_file(base);
    constexpr unsigned count = 254, base_stride = 16;
    const auto stride = items.size() / count;
    if ((stride != 17 && stride != 18) || items.size() != count * stride)
        throw std::runtime_error("Expected 254 DOS ITEM.DAT records");
    if (!base_items.empty() && base_items.size() != 128 * base_stride)
        throw std::runtime_error("Expected 128 DOS ITEMS.DAT base records");
    std::vector<RecordRow> rows;
    rows.reserve(count);
    for (unsigned id = 0; id < count; ++id) {
        const auto* b = items.data() + id * stride;
        RecordRow row{id, 0, {}, {}};
        for (const auto component : {b[3], b[2], b[1]}) {
            if (!component)
                continue;
            if (!row.name.empty())
                row.name += ' ';
            if (component < std::size(frua_item_components))
                row.name += frua_item_components[component];
            else
                row.name += '[' + std::to_string(component) + ']';
        }
        if (row.name.empty())
            row.name = "Item " + std::to_string(id);
        field(row, "Item ID", id);
        field(row, "Base item type", b[0]);
        row.fields.emplace_back("Name component IDs", std::to_string(b[3]) + ", " +
                                std::to_string(b[2]) + ", " + std::to_string(b[1]));
        field(row, "Weight", signed_word(word(b, 4)));
        field(row, "Price", signed_word(word(b, 6)));
        field(row, "Magic bonus", signed_byte(b[8]));
        field(row, "Secondary code", b[9]);
        field(row, "Readied", b[10]);
        field(row, "Identification flags", b[11]);
        field(row, "Cursed", b[12]);
        field(row, "Quantity", b[13]);
        field(row, "Charges", b[14]);
        field(row, "Magical effect code", b[15]);
        field(row, "Special code", b[16]);
        if (std::size_t(b[0] + 1) * base_stride <= base_items.size())
            base_item_fields(row, base_items.data() + b[0] * base_stride);
        row.fields.emplace_back("Stored bytes", raw_bytes(b, 0, static_cast<unsigned>(stride)));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string fixed_text(const std::uint8_t* b, unsigned length)
{
    const auto end = std::find(b, b + length, 0);
    std::string result(b, end);
    const auto last = result.find_last_not_of(' ');
    result.resize(last == std::string::npos ? 0 : last + 1);
    return result;
}

std::vector<RecordRow> read_frua_design(const std::filesystem::path& path)
{
    const auto bytes = read_record_file(path);
    if (bytes.size() < 387)
        throw std::runtime_error("Truncated FRUA GAME001.DAT");
    const auto* b = bytes.data();
    auto name = fixed_text(b, 32);
    RecordRow row{0, 0, name.empty() ? "Adventure design" : std::move(name), {}};
    constexpr const char* resources[]{"Starting experience", "Starting platinum", "Starting gems", "Starting jewellery"};
    for (unsigned i = 0; i < 4; ++i) {
        const auto at = 32 + i * 4;
        const auto value = word(b, at) | (std::uint32_t(word(b, at + 2)) << 16);
        field(row, resources[i], value);
    }
    field(row, "Starting module", b[48]);
    field(row, "Starting entry point", b[49]);
    field(row, "Starting equipment", b[50]);
    for (unsigned i = 0; i < 20; ++i) {
        auto label = (i < 8 ? "Key " : "Special item ") + std::to_string(i < 8 ? i + 1 : i - 7);
        row.fields.emplace_back(std::move(label), fixed_text(b + 52 + i * 16, 16));
    }
    row.fields.emplace_back("Design password", fixed_text(b + 372, 15));
    return {std::move(row)};
}

std::vector<RecordRow> read_frua_geo(const std::filesystem::path& path)
{
    const auto bytes = read_record_file(path);
    constexpr unsigned grid = 322, stored_cells = 576, cell_stride = 6;
    if (bytes.size() < grid + stored_cells * cell_stride)
        throw std::runtime_error("Truncated FRUA GEO map");
    const auto* b = bytes.data();
    const unsigned height = b[26], width = b[27];
    const auto cells = width * height;
    if (cells > stored_cells)
        throw std::runtime_error("Invalid FRUA GEO map dimensions");
    auto name = fixed_text(b + 142, 16);
    RecordRow summary{0, 0, name.empty() ? "Map summary" : std::move(name), {}};
    field(summary, "Width", width);
    field(summary, "Height", height);
    const bool overland = b[28] == 255 && b[29] == 255 && b[30] == 255;
    summary.fields.emplace_back("Module type", overland ? "Overland" : "Dungeon");
    summary.fields.emplace_back("Wall art slots", raw_bytes(b, 28, 3));
    field(summary, "Area view", b[31]);
    summary.fields.emplace_back("Backdrop slots", raw_bytes(b, 32, 4));
    field(summary, "Dungeon combat art", b[36]);
    field(summary, "Wilderness combat art", b[37]);
    field(summary, "Summoned monster", b[286]);
    field(summary, "Undead turning difficulty", b[287]);
    field(summary, "Camp picture", b[312]);
    field(summary, "Treasure picture", b[313]);
    for (unsigned i = 0; i < 8; ++i) {
        const auto slot = std::to_string(i + 1);
        const auto* entry = b + 38 + i * 4;
        summary.fields.emplace_back("Entry point " + slot, std::to_string(entry[1]) + ", " +
                                    std::to_string(entry[0]) + " (facing " + std::to_string(entry[2]) + ')');
        summary.fields.emplace_back("Zone " + slot, fixed_text(b + 158 + i * 16, 16));
        const auto* rest = b + 70 + i * 4;
        summary.fields.emplace_back("Rest event " + slot, std::to_string(rest[2]) + " (" +
                                    std::to_string(rest[3]) + "% every " + std::to_string(rest[0]) + " minutes)");
        const auto* step = b + 102 + i * 4;
        summary.fields.emplace_back("Step event " + slot, std::to_string(step[2]) + " (every " +
                                    std::to_string(step[0]) + " steps, zone mask " + std::to_string(step[3]) + ')');
    }
    std::vector<RecordRow> rows;
    rows.reserve(1 + cells);
    rows.push_back(std::move(summary));
    for (unsigned i = 0; i < cells; ++i) {
        const auto* cell = b + grid + i * cell_stride;
        RecordRow row{i + 1, 0, "Cell " + std::to_string(i % width) + ", " + std::to_string(i / width), {}};
        constexpr const char* edges[]{"North", "East", "South", "West"};
        for (unsigned direction = 0; direction < 4; ++direction)
            field(row, edges[direction], cell[direction]);
        field(row, "Event", cell[4]);
        field(row, "Backdrop / zone byte", cell[5]);
        rows.push_back(std::move(row));
    }
    constexpr unsigned encounter_tag = grid + stored_cells * cell_stride;
    constexpr unsigned encounters = encounter_tag + 8, encounter_count = 100, encounter_stride = 20;
    if (bytes.size() >= encounters + encounter_count * encounter_stride &&
        upper(std::string(reinterpret_cast<const char*>(b + encounter_tag), 4)) == "ENCR") {
        for (unsigned i = 0; i < encounter_count; ++i) {
            const auto* event = b + encounters + i * encounter_stride;
            if (!event[0])
                continue;
            RecordRow row{stored_cells + i + 1, 0, "Event " + std::to_string(i + 1), {}};
            field(row, "Event type", event[0]);
            field(row, "Do flag", event[1]);
            field(row, "Condition", event[2]);
            field(row, "Chained event", event[3]);
            row.fields.emplace_back("Event parameters", raw_bytes(event, 4, 16));
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::vector<RecordRow> read_frua_save(const std::filesystem::path& path)
{
    const auto b = read_record_file(path);
    if (b.size() < 131)
        throw std::runtime_error("Truncated FRUA SAVGAM CSV data");
    RecordRow row{0, 0, "Saved party", {}};
    constexpr std::pair<unsigned, const char*> fields[]{
        {1,"No magic"}, {5,"Rounds"}, {6,"Turns"}, {7,"Hours"}, {8,"Days"},
        {9,"Months"}, {10,"Years"}, {16,"Facing direction"}, {17,"Game speed"},
        {18,"Current module"}, {21,"Initiated"}, {22,"Previous column"}, {23,"Previous row"},
        {24,"Search mode"}, {25,"Overland"}, {28,"Party never dies"}, {29,"No treasure"},
        {30,"No combat XP"}, {31,"Party size"}, {33,"Dungeon"}, {35,"Second overland"},
        {36,"Overland column"}, {37,"Overland row"}, {38,"Combat level"}, {43,"No rest zone"},
        {47,"Training allowed"}, {54,"Initial opponent distance"}, {57,"Friendly opponents"},
        {59,"Underwater"}, {65,"Dungeon column"}, {66,"Dungeon row"},
    };
    for (const auto& [offset, label] : fields)
        field(row, label, b[offset]);
    row.fields.emplace_back("Key bytes", raw_bytes(b.data(), 67, 8));
    row.fields.emplace_back("Special item bytes", raw_bytes(b.data(), 75, 12));
    row.fields.emplace_back("Quest bytes", raw_bytes(b.data(), 87, 44));
    return {std::move(row)};
}

std::vector<RecordRow> read_luck(const std::filesystem::path& path)
{
    auto records = upper(path.extension().string()) == ".GLB" ? read_amiga_glb(path, "TILE")
                                                               : try_amiga_dax(path);
    if (records.empty() && upper(path.extension().string()) == ".DAX")
        records = read_dax(path);
    if (records.empty())
        throw std::runtime_error("No dialogue records in LUCK archive");
    constexpr const char* substitutions[]{
        "{stronger name}", "{weaker name}", "{NPC name}", "{party member name}",
        "{other party member name}", "{stronger pronoun}", "{weaker pronoun}",
    };
    std::vector<RecordRow> rows;
    rows.reserve(records.size());
    for (const auto& record : records) {
        const auto id = record.id;
        const bool romance = id >= 101;
        const char* group = romance ? "Romance" : id >= 51 ? "Lucky paper" : "Crown hint";
        RecordRow row{id, 0, std::string(group) + " " + std::to_string(id), {}};
        field(row, "Record ID", id);
        row.fields.emplace_back("Category", group);
        std::string line;
        unsigned number = 0;
        auto append_line = [&] {
            row.fields.emplace_back("Line " + std::to_string(++number), std::move(line));
            line.clear();
        };
        for (std::size_t i = 0; i < record.bytes.size(); ++i) {
            const auto byte = record.bytes[i];
            if (byte == 26)
                break;
            if (byte == 12) {
                if (!line.empty())
                    append_line();
                row.fields.emplace_back("Page break", "Wait for Continue");
                // The source usually puts a line ending immediately after the page marker.
                if (i + 1 < record.bytes.size() && record.bytes[i + 1] == 13)
                    ++i;
                if (i + 1 < record.bytes.size() && record.bytes[i + 1] == 10)
                    ++i;
            } else if (byte == 10 || byte == 13) {
                if (byte == 13 && i + 1 < record.bytes.size() && record.bytes[i + 1] == 10)
                    ++i;
                append_line();
            } else if (romance && byte >= 2 && byte <= 8) {
                line += substitutions[byte - 2];
            } else if (byte >= 32 && byte <= 126) {
                line += static_cast<char>(byte);
            } else {
                line += "{$" + raw_bytes(&byte, 0, 1) + '}';
            }
        }
        if (!line.empty())
            append_line();
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<RecordRow> read_countdown_ships(const std::filesystem::path& path)
{
    constexpr const char* systems[]{"Hull", "Sensors", "Control systems", "Life support", "Fuel", "Engines"};
    constexpr const char* types[]{"", "Fighter", "Asterover", "Scout cruiser", "Lt freighter", "Hv freighter",
        "Md cruiser", "Lt transport", "Hv transport", "Hv cruiser", "Space liner", "Battler"};
    constexpr const char* races[]{"Monster", "Terran", "Martian", "Venusian", "Mercurian",
        "Tinker", "Desert Runner", "Lunarian", "Lowlander"};
    constexpr const char* weapons[]{"None", "Beam laser", "Pumped laser", "Gyrocannon",
        "Missile mount", "Heavy missile mount", "K-cannon", "Light acceleration gun", "Heavy acceleration gun"};
    const auto records = read_dax(path);
    if (records.empty())
        throw std::runtime_error("No Countdown ship records in SHIPS2.DAX");
    std::vector<RecordRow> rows;
    rows.reserve(records.size());
    for (const auto& record : records) {
        if (record.bytes.size() != 108 || record.bytes.front() > 15)
            throw std::runtime_error("Invalid 108-byte Countdown ship record");
        // The Amiga SHIPS2 archive preserves both the DOS directory and little-endian ship words.
        const auto* b = record.bytes.data();
        auto name = name_at(b, 15);
        RecordRow row{record.id, 0, name.empty() ? "Ship " + std::to_string(record.id) : std::move(name), {}};
        field(row, "Record ID", record.id);
        field(row, "Tonnage", signed_word(word(b, 16)));
        field(row, "Length", signed_word(word(b, 18)));
        field(row, "Width", signed_word(word(b, 20)));
        for (unsigned i = 0; i < std::size(systems); ++i)
            row.fields.emplace_back(std::string(systems[i]) + " (current / max)",
                std::to_string(signed_word(word(b, 22 + i * 2))) + " / " +
                std::to_string(signed_word(word(b, 34 + i * 2))));
        field(row, "Cargo", signed_word(word(b, 46)));
        field(row, "Crew", signed_word(word(b, 48)));
        field(row, "Armour class", signed_byte(b[50]));
        field(row, "Pilot skill", b[51]);
        field(row, "System skill", b[52]);
        field(row, "Gunnery skill", b[53]);
        field(row, "Base speed", signed_byte(b[54]));
        field(row, "Reaction", signed_byte(b[55]));
        field(row, "Manoeuvre", b[56]);
        field(row, "Ship type ID", b[57]);
        if (b[57] > 0 && b[57] < std::size(types))
            row.fields.emplace_back("Ship type", types[b[57]]);
        field(row, "Race ID", b[58]);
        if (b[58] < std::size(races))
            row.fields.emplace_back("Race", races[b[58]]);

        for (unsigned i = 0; i < 8; ++i) {
            const auto* mount = b + 60 + i * 6;
            const unsigned kind = mount[0] & 0x7f;
            if (!kind)
                continue;
            const auto label = "Mount " + std::to_string(i + 1);
            const auto weapon = kind < std::size(weapons) ? std::string(weapons[kind]) :
                "Weapon type " + std::to_string(kind);
            row.fields.emplace_back(label, weapon + ((mount[0] & 0x80) ? " (crew)" : " (pilot)"));
            row.fields.emplace_back(label + " (working / max)",
                std::to_string(mount[2]) + " / " + std::to_string(mount[1]));
            row.fields.emplace_back(label + " ammunition", std::to_string(mount[4]));
            row.fields.emplace_back(label + " reloads", std::to_string(mount[5]));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<RecordRow> read_vault(const std::filesystem::path& path)
{
    constexpr unsigned header_size = 16;
    constexpr unsigned padded_records = 50;
    const auto bytes = read_record_file(path);
    const auto* data = bytes.data();
    // The small vault headers are identical in size on DOS and Amiga.
    // Use the matching party format to choose byte order and item stride.
    if ((bytes.size() >= 12 && (bytes.size() - 12) % 72 == 0) ||
        (bytes.size() >= 28 && (bytes.size() - 28) % 70 == 0)) {
        const auto folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
        static const std::regex slot(R"(SAVGAM[A-J]\.(SAV|PTY))");
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!std::regex_match(upper(entry.path().filename().string()), slot) || !entry.is_regular_file()) continue;
            const bool knights = upper(entry.path().extension().string()) == ".PTY";
            if (knights ? entry.file_size() != 2925 : entry.file_size() < 5143) continue;
            const unsigned header = knights ? 12 : 28;
            const unsigned width = knights ? 72 : 70;
            if (bytes.size() < header || (bytes.size() - header) % width) continue;
            std::vector<RecordRow> rows{{0, 0, "Vault contents", {{"Format", "Amiga"}}}};
            constexpr const char* labels[]{"Copper", "Silver", "Electrum", "Gold", "Platinum", "Gems", "Jewels"};
            for (unsigned at = 0; at < header; at += 4) {
                const auto label = knights && !at ? "Steel" : labels[(knights ? 4 : 0) + at / 4];
                field(rows.front(), label, (std::uint32_t(amiga_word(data, at)) << 16) | amiga_word(data, at + 2));
            }
            for (unsigned at = header; at < bytes.size(); at += width)
                rows.push_back({0, (at - header) / width, amiga_name(data + at, 41), {}});
            return rows;
        }
    }
    // Amiga Dark Queen and Pools reserve 200 item slots; the FFFF tag
    // precedes a big-endian root count. Unused slots contain template data.
    if ((bytes.size() == 3616 || bytes.size() == 4016) && word(data, 12) == 0xffff) {
        const unsigned width = bytes.size() == 3616 ? 18 : 20;
        const auto roots = amiga_word(data, 14);
        if (roots <= 200) {
            std::vector<RecordRow> rows{{0, 0, "Vault contents", {{"Format", "Amiga"}}}};
            for (unsigned i = 0; i < 3; ++i)
                field(rows.front(), i == 0 ? "Coins" : i == 1 ? "Gems" : "Jewels",
                      (std::uint32_t(amiga_word(data, i * 4)) << 16) | amiga_word(data, i * 4 + 2));
            field(rows.front(), "Stored items and bundles", roots);
            unsigned slot = 0;
            for (unsigned i = 0; i < roots; ++i) {
                if (slot >= 200) throw std::runtime_error("Truncated Amiga vault");
                const auto* p = data + 16 + width * slot;
                RecordRow row{i + 1, slot++, "Item type " + std::to_string(p[0]), {}};
                const unsigned quantity = p[width == 18 ? 13 : 12];
                field(row, "Quantity", quantity);
                if (p[0] == 'I') slot += quantity;
                if (slot > 200) throw std::runtime_error("Truncated Amiga vault bundle");
                rows.push_back(std::move(row));
            }
            return rows;
        }
    }
    // Pascal SaveVault writes three long currency totals followed by TItem records.
    const bool gateway = bytes.size() >= 28 && (bytes.size() - 28) % item_size == 0;
    const unsigned money_bytes = gateway ? 28 : 12;
    const bool tagged = bytes.size() >= 866 && word(data, 12) == 0xffff;
    if (!tagged && bytes.size() >= money_bytes && (bytes.size() - money_bytes) % item_size == 0) {
        std::vector<RecordRow> rows{{0, 0, "Vault contents", {}}};
        constexpr const char* currencies[]{"Copper", "Silver", "Electrum", "Gold", "Platinum", "Gems", "Jewels"};
        for (unsigned i = 0; i < money_bytes / 4; ++i)
            field(rows.front(), gateway ? currencies[i] : i == 0 ? "Coin total" : currencies[4 + i],
                  word(data, i * 4) | (std::uint32_t(word(data, i * 4 + 2)) << 16));
        for (unsigned offset = money_bytes; offset < bytes.size(); offset += item_size)
            rows.push_back(item(0, (offset - money_bytes) / item_size, data + offset, nullptr, false));
        return rows;
    }
    // Dark Queen uses 17-byte ItemRec and FRUA uses 18. Beyond the
    // mandatory padding, use the corresponding save slot to disambiguate.
    const bool queen = bytes.size() == 866 ||
        (bytes.size() > 866 && (bytes.size() - header_size) % 17 == 0 &&
         ((bytes.size() - header_size) % 18 != 0 ||
          std::filesystem::exists(path.parent_path() / ("SAVGAM" + upper(path.stem().string()).substr(5) + ".QSV"))));
    const unsigned record_size = queen ? 17 : 18;
    if (bytes.size() < header_size + padded_records * record_size ||
        (bytes.size() - header_size) % record_size != 0 || word(data, 12) != 0xffff)
        throw std::runtime_error("Expected a Forgotten Realms Unlimited Adventures DOS vault");
    const auto available = (bytes.size() - header_size) / record_size;
    const auto roots = word(data, 14);
    if (roots > available)
        throw std::runtime_error("Invalid FRUA vault item count");

    RecordRow summary{0, 0, "Vault contents", {}};
    field(summary, queen ? "Steel pieces" : "Platinum pieces", word(data, 0) | (std::uint32_t(word(data, 2)) << 16));
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
            if (!queen && id < std::size(frua_item_components))
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

void position_fields(RecordRow& row, const std::uint8_t* p)
{
    field(row, "Map X", signed_byte(p[0]));
    field(row, "Map Y", signed_byte(p[1]));
    field(row, "Facing", p[2]);
    field(row, "Wall ahead", p[3]);
    field(row, "Cell event", p[4]);
}

// Dark Queen saveload.c: game_write_cb embeds each hero_write_cb stream.
// The item pointer holds a count. Effect pointers are serialized chain markers.
std::vector<RecordRow> read_queen_save(const std::vector<std::uint8_t>& b, bool party)
{
    if (party && b.size() < 0x1f1d)
        throw std::runtime_error("Truncated Dark Queen party save");
    auto count = party ? word(b.data(), 0x40d) : 1u;
    if (count > 8)
        throw std::runtime_error("Invalid Dark Queen party count");
    std::vector<RecordRow> rows;
    if (party) {
        rows.push_back({0, 0, "Party", {}});
        auto& summary = rows.back();
        field(summary, "Map ID", b[0x12]);
        field(summary, "Script", b[0x15]);
        field(summary, "Loaded map", b[0xc5]);
        position_fields(summary, b.data() + 0x400);
        field(summary, "Party members", count);
    }
    std::size_t cursor = party ? 0x40f : 0;
    auto take = [&](std::size_t length) {
        if (length > b.size() - cursor)
            throw std::runtime_error("Truncated Dark Queen character data");
        const auto* p = b.data() + cursor;
        cursor += length;
        return p;
    };
    for (unsigned i = 0; i < count; ++i) {
        const auto* p = take(0x19e);
        RecordRow member{i + 1, 0, amiga_name(p + 0x68, 16), {}};
        field(member, "Current HP", p[0x198]);
        field(member, "Age", word(p, 0x52));
        field(member, "Class", word(p, 0x5a));
        field(member, "Status", word(p, 0x64));
        const auto items = word(p, 8);
        field(member, "Inventory entries", items);
        for (unsigned item_index = 0; item_index < items; ++item_index) {
            const auto* item = take(17);
            if (item[0] == 'I')
                take(17u * item[13]);
        }
        const auto* link = p + 4;
        while (word(link, 0) || word(link, 2)) {
            const auto* effect = take(9);
            member.fields.emplace_back("Effect " + std::to_string(effect[0]),
                                       std::to_string(word(effect, 1)) + " minutes");
            link = effect + 5;
        }
        rows.push_back(std::move(member));
    }
    return rows;
}

// Save streams contain pointer values from the Amiga heap. Only their
// presence matters: the next serialized node follows the preceding record.
struct AmigaPartyLayout {
    unsigned character, item, item_pointer, effect_pointer, name;
    bool counted_items;
};

bool read_amiga_members(const std::vector<std::uint8_t>& b, std::size_t& at, unsigned count,
                        const AmigaPartyLayout& l, std::vector<RecordRow>& rows)
{
    auto take = [&](unsigned length) -> const std::uint8_t* {
        if (at > b.size() || length > b.size() - at) return nullptr;
        const auto* p = b.data() + at;
        at += length;
        return p;
    };
    auto linked = [](const std::uint8_t* p) { return amiga_word(p, 0) || amiga_word(p, 2); };
    for (unsigned i = 0; i < count; ++i) {
        const auto* p = take(l.character);
        if (!p || p[l.name] < 32 || !std::memchr(p + l.name, 0, 16)) return false;
        RecordRow row{i + 1, 0, amiga_name(p + l.name, 16), {}};
        auto remaining = l.counted_items ? amiga_word(p, l.item_pointer + 2) : unsigned(linked(p + l.item_pointer));
        unsigned items = 0;
        while (remaining) {
            const auto* item = take(l.item);
            if (!item) return false;
            ++items;
            if (l.counted_items) {
                --remaining;
                const auto quantity = item[l.item == 18 ? 13 : 12];
                row.fields.emplace_back("Item type " + std::to_string(item[0]), std::to_string(quantity));
                if (item[0] == 'I' && !take(quantity * l.item)) return false;
            } else {
                row.fields.emplace_back("Item " + std::to_string(items), amiga_name(item, 41));
                remaining = linked(item + 42);
                if (l.item == 70 && item[46] == 'I' && !take(item[58] * l.item)) return false;
            }
        }
        field(row, "Inventory entries", items);
        const auto* link = p + l.effect_pointer;
        while (linked(link)) {
            const auto* effect = take(10);
            if (!effect) return false;
            row.fields.emplace_back("Effect " + std::to_string(effect[0]), std::to_string(amiga_word(effect, 2)));
            link = effect + 6;
        }
        rows.push_back(std::move(row));
    }
    return true;
}

std::vector<RecordRow> amiga_inline_party(const std::vector<std::uint8_t>& b, unsigned start,
                                         const AmigaPartyLayout& layout, bool padded, bool single = false)
{
    if (start > b.size()) return {};
    unsigned count = single ? 1 : amiga_word(b.data(), start - 2);
    if (!count || count > 8) return {};
    std::vector<RecordRow> rows;
    if (!single) {
        rows.push_back({0, 0, "Party", {{"Format", "Amiga"}}});
        field(rows.front(), "Party members", count);
        if (start == 12825 || start == 5765) {
            const unsigned pos = start - 24;
            field(rows.front(), "Map X", amiga_word(b.data(), pos));
            field(rows.front(), "Map Y", amiga_word(b.data(), pos + 2));
            field(rows.front(), "Facing", b[pos + 4]);
        } else if (start == 5143) {
            position_fields(rows.front(), b.data() + 5121);
        }
    }
    std::size_t at = start;
    if (!read_amiga_members(b, at, count, layout, rows) || (!padded && at != b.size())) return {};
    return rows;
}

std::vector<RecordRow> read_party_save(const std::filesystem::path& path)
{
    const auto bytes = read_record_file(path);
    const auto ext = upper(path.extension().string());
    if (ext == ".DAA" && bytes.size() >= 2633) {
        // Countdown writer $2A252: 402-byte character, two lists of
        // 70-byte items, then 10-byte effects. The slot header stores the
        // counts in three eight-word arrays, just as the native reader uses.
        const unsigned count = amiga_word(bytes.data(), 2583);
        if (count > 8) throw std::runtime_error("Invalid Countdown party count");
        std::vector<RecordRow> rows{{0, 0, "Party", {{"Format", "Amiga Countdown to Doomsday"}}}};
        field(rows.front(), "Party members", count);
        field(rows.front(), "Map X", amiga_word(bytes.data(), 2561));
        field(rows.front(), "Map Y", amiga_word(bytes.data(), 2563));
        std::size_t at = 2633;
        for (unsigned i = 0; i < count; ++i) {
            const auto items = amiga_word(bytes.data(), 2585 + 2 * i);
            const auto hidden = amiga_word(bytes.data(), 2601 + 2 * i);
            const auto effects = amiga_word(bytes.data(), 2617 + 2 * i);
            const auto length = 402u + 70u * (items + hidden) + 10u * effects;
            if (length > bytes.size() - at) throw std::runtime_error("Truncated Countdown party member");
            RecordRow row{i + 1, 0, amiga_name(bytes.data() + at, 16), {}};
            at += 402;
            for (unsigned j = 0; j < items + hidden; ++j, at += 70)
                row.fields.emplace_back(j < items ? "Item" : "Hidden item", amiga_name(bytes.data() + at, 41));
            for (unsigned j = 0; j < effects; ++j, at += 10)
                field(row, "Effect ID", bytes[at]);
            rows.push_back(std::move(row));
        }
        if (at != bytes.size()) throw std::runtime_error("Invalid Countdown save length");
        return rows;
    }
    // Curse writer at executable offset $260CC, Gateway at $230FE.
    // Both follow item links at +$2A and effect links at +6.
    if (ext == ".DAT" || ext == ".GUY") {
        for (unsigned width : {428u, 434u}) {
            const AmigaPartyLayout layout{width, 66, 0x152, 0xf2, 0, false};
            auto rows = amiga_inline_party(bytes, ext == ".GUY" ? 0 : 12825, layout, false, ext == ".GUY");
            if (!rows.empty()) return rows;
            // Treasures writes smaller state banks; its slot writer is at $1C344.
            if (ext == ".DAT" && width == 434) {
                rows = amiga_inline_party(bytes, 5765, layout, false);
                if (!rows.empty()) return rows;
            }
        }
    }
    if (ext == ".SAV") {
        // Secret's writer at $27144 uses 70-byte items with bundle children.
        auto rows = amiga_inline_party(bytes, 5143, {340, 70, 0xfe, 0x96, 0, false}, false);
        if (!rows.empty()) return rows;
    }
    if (ext == ".QCH") {
        auto rows = amiga_inline_party(bytes, 0, {406, 18, 8, 4, 96, true}, false, true);
        if (!rows.empty()) return rows;
    }
    if (ext == ".QSV" || ext == ".PTY") {
        const bool queen = ext == ".QSV";
        auto rows = amiga_inline_party(bytes, 1038,
            {queen ? 406u : 404u, queen ? 18u : 20u, 8, 4, 96, true}, true);
        if (!rows.empty()) {
            position_fields(rows.front(), bytes.data() + 1024);
            return rows;
        }
    }
    if (ext == ".QSV" || ext == ".QCH")
        return read_queen_save(bytes, ext == ".QSV");
    // Pool's Amiga writer omits the disk byte and writes ten position
    // bytes followed by the interface state and C strings (not String[40]).
    const bool pool = ext == ".DAT" && bytes.size() == 13141 && bytes[0] == 0;
    const bool champions = ext == ".DAT" && bytes.size() == 5481;
    const bool knights = ext == ".PTY" && bytes.size() == 2925;
    if (pool || champions || knights) {
        const unsigned stride = pool ? 41 : 42;
        const unsigned roster = static_cast<unsigned>(bytes.size()) - 8 * stride;
        const auto count = bytes[roster - 1];
        if (count > 8) throw std::runtime_error("Invalid Amiga party count");
        std::vector<RecordRow> rows{{0, 0, "Party", {{"Format", pool ? "Amiga Pool of Radiance" :
            champions ? "Amiga Champions of Krynn" : "Amiga Death Knights of Krynn"}}}};
        if (pool) {
            position_fields(rows.front(), bytes.data() + 0x3200);
            field(rows.front(), "Map ID", amiga_word(bytes.data(), 2 * 0xc5));
            field(rows.front(), "Script", amiga_word(bytes.data(), 2 * 0xf2));
            field(rows.front(), "Location type", bytes[0x320a]);
            field(rows.front(), "Interface", bytes[0x320b]);
        } else {
            const unsigned pos = champions ? 5121 : 2561;
            field(rows.front(), "Map X", amiga_word(bytes.data(), pos));
            field(rows.front(), "Map Y", amiga_word(bytes.data(), pos + 2));
            field(rows.front(), "Facing", bytes[pos + 4]);
        }
        field(rows.front(), "Party members", count);
        const auto folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
        for (unsigned i = 0; i < count; ++i) {
            const auto* p = bytes.data() + roster + i * stride;
            const auto stem = amiga_name(p, stride);
            if (stem.empty() || stem.size() > 8)
                throw std::runtime_error("Invalid Amiga party member filename");
            RecordRow member{i + 1, 0, stem, {}};
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (upper(entry.path().filename().string()) != upper(stem) + (knights ? ".GAM" : ".SAV")) continue;
                auto data = read_record_file(entry.path());
                if (pool && data.size() == 288) member = amiga_monster({i + 1, 288, std::move(data)});
                if (!pool && data.size() == (knights ? 232u : 426u)) member.name = amiga_name(data.data(), 16);
                break;
            }
            member.fields.emplace_back("Member file", stem + (knights ? ".gam" : ".sav"));
            rows.push_back(std::move(member));
        }
        return rows;
    }
    // SaveLoad in each Pascal game writes the VM banks, five position bytes,
    // interface states and wall cache, then a byte count and eight String[40]s.
    unsigned position = 0;
    if (ext == ".DAT") {
        switch (bytes.size()) {
        case 13149: position = 12801; break; // Curse, Gateway
        case 5469: position = 5121; break;   // Champions, Silver Blades
        case 2901: position = 2561; break;   // both Buck Rogers games
        }
    } else if (ext == ".PTY") {
        if (bytes.size() == 1364) position = 1024; // Pools, Treasures
        if (bytes.size() == 2913) position = 2561; // Death Knights
    }
    if (!position)
        throw std::runtime_error("Unsupported DOS party save layout");
    const auto roster = bytes.size() - 8 * 41;
    const auto count = bytes[roster - 1];
    if (count > 8)
        throw std::runtime_error("Invalid DOS party count");
    std::vector<RecordRow> rows{{0, 0, "Party", {}}};
    position_fields(rows.front(), bytes.data() + position);
    field(rows.front(), "Previous interface", bytes[position + 5]);
    field(rows.front(), "Current interface", bytes[position + 6]);
    field(rows.front(), "Party members", count);
    for (unsigned i = 0; i < count; ++i) {
        const auto* p = bytes.data() + roster + i * 41;
        if (!p[0] || p[0] > 40)
            throw std::runtime_error("Invalid party member filename");
        const auto stem = name_at(p, 40);
        RecordRow member{i + 1, 0, stem, {}};
        const auto filename = upper(stem) + (bytes.size() == 2913 ? ".GAM" : ".SAV");
        const auto folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (upper(entry.path().filename().string()) != filename || !entry.is_regular_file()) continue;
            auto data = read_record_file(entry.path());
            if (known_monster_size(data.size())) {
                const auto length = static_cast<unsigned>(data.size());
                member = read_monster({i + 1, length, std::move(data)});
            }
            break;
        }
        member.fields.emplace_back("Member file", filename);
        rows.push_back(std::move(member));
    }
    return rows;
}

std::vector<RecordRow> read_save_rows(const std::filesystem::path& path, RecordKind kind)
{
    const auto bytes = read_record_file(path);
    if (kind == RecordKind::saved_character) {
        if (upper(path.extension().string()) == ".GUY" && bytes.size() != 422)
            return read_party_save(path);
        if ((bytes.size() == 232 || bytes.size() == 426) && bytes.front() >= 32) {
            RecordRow row{0, 0, amiga_name(bytes.data(), 16), {{"Format", "Amiga character"}}};
            field(row, "Strength", bytes[16]);
            field(row, "Intelligence", bytes[18]);
            return {row};
        }
        if (bytes.size() == 288 && bytes.front() >= 32)
            return {amiga_monster({0, 288, bytes})};
        if (!known_monster_size(bytes.size()))
            throw std::runtime_error("Unsupported DOS character save layout");
        auto row = read_monster({0, static_cast<unsigned>(bytes.size()), bytes});
        row.fields.front() = {"Save file", path.filename().u8string()};
        return {std::move(row)};
    }
    const bool effects = kind == RecordKind::saved_effects;
    const auto extension = upper(path.extension().string());
    if (extension == ".WIZ")
        return {death_knights_spells({0, static_cast<unsigned>(bytes.size()), bytes}, nullptr)};
    std::string owner;
    std::size_t owner_size = 0;
    const auto folder = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        const auto ext = upper(entry.path().extension().string());
        if (upper(entry.path().stem().string()) != upper(path.stem().string()) ||
            (ext != ".SAV" && ext != ".GUY" && ext != ".WHO" && ext != ".GAM" && ext != ".PC"))
            continue;
        try {
            const auto character = read_record_file(entry.path());
            if (character.size() == 288 && character.front() >= 32) {
                owner_size = 288;
                owner = amiga_name(character.data(), 16);
            }
            if ((character.size() == 426 || character.size() == 232) && character.front() >= 32) {
                owner_size = character.size();
                owner = amiga_name(character.data(), 16);
            }
            if (known_monster_size(character.size())) {
                owner_size = character.size();
                owner = name_at(character.data(), 15);
            }
        } catch (const std::exception&) {
            // Inventory can be inspected on its own.
        }
        break;
    }
    if ((extension == ".ITM" || extension == ".SPC") && owner_size == 288) {
        const auto stride = effects ? 10u : 65u;
        if (bytes.size() % stride) throw std::runtime_error("Truncated Amiga saved item or effect");
        std::vector<RecordRow> rows;
        for (unsigned offset = 0; offset < bytes.size(); offset += stride) {
            const auto* p = bytes.data() + offset;
            auto row = effects ? amiga_effect(p[0], offset / stride, p) : amiga_item(0, offset / stride, p);
            row.fields.emplace_back("Owner", owner);
            rows.push_back(std::move(row));
        }
        return rows;
    }
    if (owner_size == 232 || owner_size == 426) {
        const unsigned width = effects ? 10 : 72;
        if (bytes.size() % width) throw std::runtime_error("Truncated Amiga character companion");
        std::vector<RecordRow> rows;
        for (unsigned at = 0; at < bytes.size(); at += width) {
            const auto* p = bytes.data() + at;
            // These 72-byte inventory nodes differ from Pool's 65-byte nodes.
            auto row = effects ? amiga_effect(p[0], at / width, p) :
                RecordRow{0, at / width, amiga_name(p, 41), {}};
            if (row.name.empty()) row.name = "Inventory entry " + std::to_string(at / width + 1);
            row.fields.emplace_back("Owner", owner);
            rows.push_back(std::move(row));
        }
        return rows;
    }
    const bool buck = extension == ".HID" || (extension == ".STF" &&
        (owner_size == 259 || owner_size == 263 || (!owner_size && bytes.size() % item_size != 0)));
    const auto stride = effects ? effect_size : buck ? 62u : item_size;
    if (bytes.size() % stride)
        throw std::runtime_error("Truncated DOS saved item or effect");
    std::vector<RecordRow> rows;
    for (unsigned slot = 0; slot < bytes.size() / stride; ++slot) {
        const auto* b = bytes.data() + slot * stride;
        if (!effects) {
            if (buck) rows.push_back(compact_item(0, slot, b, stride, owner.empty() ? nullptr : &owner));
            else rows.push_back(item(0, slot, b, owner.empty() ? nullptr : &owner,
                                     extension == ".ITM", false, owner_size));
            continue;
        }
        rows.push_back(effect(b[0], slot, b, owner.empty() ? nullptr : &owner));
    }
    return rows;
}
}

namespace {
struct SoundBank {
    bool packed;
    std::vector<DaxRecord> samples;
};

SoundBank read_sound_bank(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    char signature[4]{};
    if (!stream.read(signature, sizeof(signature)))
        throw std::runtime_error("Cannot read sound bank signature");
    if (std::string_view(signature, sizeof(signature)) == "HLIB")
        return {true, read_dos_sound_records(path)};
    auto records = read_amiga_glb(path, "DIG8");
    if (records.empty())
        throw std::runtime_error("Expected an Amiga DIG8 or DOS DIG4 sample bank");
    return {false, std::move(records)};
}
}

RecordKind record_kind(const std::filesystem::path& path)
{
    if (is_ecl_archive(path))
        return RecordKind::ecl;
    if (is_c64_record(path))
        return RecordKind::c64;
    const auto name = upper(path.filename().string());
    if (name == "LUCK.DAX" || name == "LUCK.GLB") return RecordKind::luck;
    if (name == "SHIPS2.DAX") return RecordKind::countdown_ships;
    if (name == "ITEMS" || name == "ITEMS.1") return RecordKind::base_items;
    if (name == "ITEM.DAT" || name == "ITEMS.DAT") return RecordKind::frua_items;
    if (name == "GAME001.DAT") return RecordKind::frua_design;
    if (name.compare(0, 3, "GEO") == 0 && upper(path.extension().string()) == ".DAT")
        return RecordKind::frua_geo;
    if (name.compare(0, 6, "SAVGAM") == 0 && upper(path.extension().string()) == ".CSV")
        return RecordKind::frua_save;
    if (name == "STRINGS.GLB") return RecordKind::amiga_text;
    if (name == "MONCHA.LIB") return RecordKind::monsters;
    if (name == "SOUNDS.GLB") return RecordKind::sound_samples;
    static const std::regex monster_name(R"(MON[0-9]*(CHA|ITM|SPC)\.(DAX|GLB))");
    std::smatch match;
    static const std::regex dat_monster(R"(MON[0-9]+(CHA|ITM|SPC)\.DAT)");
    if (std::regex_match(name, match, monster_name) || std::regex_match(name, match, dat_monster)) {
        if (match[1] == "CHA") return RecordKind::monsters;
        if (match[1] == "ITM") return RecordKind::inventory;
        return RecordKind::monster_effects;
    }
    static const std::regex spell_archive(R"(MON[0-9]+WIZ\.(DAX|DAT))");
    if (std::regex_match(name, spell_archive)) return RecordKind::monster_spells;
    static const std::regex item_name(R"(ITEM[0-9]*\.(DAX|GLB))");
    if (std::regex_match(name, item_name)) return RecordKind::items;
    const auto extension = upper(path.extension().string());
    if (extension == ".XMI") return RecordKind::xmi;
    static const std::regex party_name(R"(SAVGAM[A-JZ]\.(DAT|PTY|QSV|SAV|DAA))");
    if (std::regex_match(name, party_name)) return RecordKind::dos_party;
    if (extension == ".QCH") return RecordKind::dos_party;
    if (extension == ".GUY" || extension == ".PC" || extension == ".WHO")
        return RecordKind::saved_character;
    if (extension == ".SWG" || extension == ".THG" || extension == ".JNK" ||
        extension == ".STF" || extension == ".HID") return RecordKind::saved_inventory;
    if (extension == ".FX" || extension == ".EFX" || extension == ".EFF" ||
        extension == ".SFX" || extension == ".WIZ") return RecordKind::saved_effects;
    if (extension == ".HIL") return RecordKind::hillsfar_character;
    if (extension == ".GAM" && name.compare(0, 6, "CHRDAT") == 0)
        return RecordKind::saved_character;
    const auto stem = upper(path.stem().string());
    if (stem == "VAULT" && extension.size() == 2 && extension[1] >= 'A' && extension[1] <= 'J')
        return RecordKind::vault;
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
    if (kind == RecordKind::dos_party)
        return read_party_save(path);
    if (kind == RecordKind::hillsfar_character) {
        // Hillsfar hfchar.h starts at the name, four bytes into the .HIL.
        const auto bytes = read_record_file(path);
        if (bytes.size() != 188) throw std::runtime_error("Invalid Hillsfar character size");
        const auto* p = bytes.data() + 4;
        RecordRow row{0, 0, amiga_name(p, 16), {}};
        // Amiga Hillsfar $626C/$6306 writes the same 188-byte block. The
        // name starts four bytes in and $5B9E stores the age as a native word.
        // Character generation uses a byte-range age. Other byte fields below
        // have the same offsets and need no platform inference.
        const auto age = word(p, 0x1a);
        const auto amiga_age = amiga_word(p, 0x1a);
        if (age > 255 && amiga_age <= 255)
            field(row, "Age", amiga_age);
        else if (age <= 255 || age == amiga_age)
            field(row, "Age", age);
        else {
            field(row, "Age (DOS byte order)", age);
            field(row, "Age (Amiga byte order)", amiga_age);
        }
        field(row, "Current HP", p[0x1c]);
        field(row, "Maximum HP", p[0x1d]);
        field(row, "Quest state", p[0x99]);
        field(row, "Completed quests", p[0xb1]);
        return {std::move(row)};
    }
    if (kind == RecordKind::none)
        throw std::runtime_error("Unrecognized DOS record archive");
    if (kind == RecordKind::vault)
        return read_vault(path);
    if (kind == RecordKind::base_items)
        return read_base_items(path);
    if (kind == RecordKind::frua_items)
        return read_frua_items(path);
    if (kind == RecordKind::frua_design)
        return read_frua_design(path);
    if (kind == RecordKind::frua_geo)
        return read_frua_geo(path);
    if (kind == RecordKind::frua_save)
        return read_frua_save(path);
    if (kind == RecordKind::luck)
        return read_luck(path);
    if (kind == RecordKind::countdown_ships)
        return read_countdown_ships(path);
    if (kind == RecordKind::ecl)
        return read_ecl_rows(path);
    if (kind == RecordKind::c64)
        return read_c64_records(path);
    if (kind == RecordKind::xmi)
        return read_xmi_rows(path);
    if (kind == RecordKind::sound_samples) {
        const auto bank = read_sound_bank(path);
        std::vector<RecordRow> rows;
        for (const auto& entry : bank.samples) {
            const auto* b = entry.bytes.data();
            if (entry.bytes.size() < 6)
                throw std::runtime_error("Truncated sound sample header");
            RecordRow row{entry.id, 0, "Sample " + std::to_string(entry.id), {}};
            row.fields.emplace_back("Encoding", bank.packed ? "DOS DIG4" : "Amiga DIG8");
            field(row, "Sample rate (Hz)", amiga_word(b, 0));
            field(row, "Declared sample bytes", std::uint32_t(amiga_word(b, 2)) << 16 | amiga_word(b, 4));
            field(row, "Stored sample bytes", entry.bytes.size() - 6);
            rows.push_back(std::move(row));
        }
        return rows;
    }
    if (kind == RecordKind::amiga_text) {
        std::vector<RecordRow> rows;
        for (const auto& entry : read_amiga_glb(path, "TEXT")) {
            const auto text = amiga_name(entry.bytes.data(), static_cast<unsigned>(entry.bytes.size()));
            rows.push_back({entry.id, 0, text.empty() ? "String " + std::to_string(entry.id) : text,
                {{"Text", text}}});
        }
        return rows;
    }
    if (kind == RecordKind::saved_character || kind == RecordKind::saved_inventory ||
        kind == RecordKind::saved_effects)
        return read_save_rows(path, kind);
    const auto extension = upper(path.extension().string());
    if (kind == RecordKind::monsters && (extension == ".GLB" || extension == ".LIB")) {
        std::ifstream stream(path, std::ios::binary);
        char signature[4]{};
        if (stream.read(signature, sizeof(signature)) && std::string_view(signature, 4) == "HLIB")
            return dark_queen_monsters(path);
        if (extension == ".LIB")
            throw std::runtime_error("Unsupported MONCHA.LIB container");
    }
    if (extension == ".GLB")
        return later_amiga_records(path, kind);
    auto records = try_amiga_dax(path);
    const bool amiga = !records.empty();
    if (!amiga)
        records = read_dax(path);
    const bool trailer = !amiga && merged(records);
    const auto filename = upper(path.filename().string());
    std::map<unsigned, std::string> owners;
    std::size_t owner_size = 0;
    if (kind == RecordKind::inventory || kind == RecordKind::monster_effects ||
        kind == RecordKind::monster_spells || (kind == RecordKind::items && filename != "ITEM.DAX" && filename != "ITEM0.DAX")) {
        const auto folder = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
        const auto companion = kind == RecordKind::items
            ? "MON" + filename.substr(4, filename.size() - 8) + "CHA.DAX"
            : filename.substr(0, filename.size() - 7) + "CHA.DAX";
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            const auto candidate = upper(entry.path().filename().string());
            if (candidate != companion && !(!amiga && upper(path.extension().string()) == ".DAT" &&
                candidate == companion.substr(0, companion.size() - 3) + "DAT"))
                continue;
            try {
                const auto companions = amiga ? try_amiga_dax(entry.path()) : read_dax(entry.path());
                for (const auto& record : companions) {
                    if (amiga && amiga_character_size(record.bytes.size())) {
                        if (kind != RecordKind::items)
                            owners.emplace(record.id, amiga_name(record.bytes.data(), 16));
                    } else if (!amiga && known_monster_size(record.bytes.size())) {
                        owner_size = record.bytes.size();
                        if (kind != RecordKind::items)
                            owners.emplace(record.id, name_at(record.bytes.data(), 15, record.pc98));
                    }
                }
            } catch (const std::exception&) {
                // The inventory remains readable when its companion is unavailable.
            }
            break;
        }
    }
    if (kind == RecordKind::monster_spells) {
        std::vector<RecordRow> rows;
        for (const auto& record : records) {
            if (record.bytes.empty() || (trailer && record.id >= 253))
                continue;
            const auto owner = owners.find(record.id);
            rows.push_back(death_knights_spells(record,
                owner == owners.end() || owner->second.empty() ? nullptr : &owner->second));
        }
        return rows;
    }
    if (amiga)
        return amiga_record_rows(records, kind, owners);
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
                        (!owner_size && (kind != RecordKind::items || filename == "ITEM.DAX")), record.pc98,
                        kind == RecordKind::inventory ? owner_size : 0));
            }
        }
    }
    return rows;
}

std::vector<std::uint8_t> sample_wav(const std::filesystem::path& path, unsigned id)
{
    const auto bank = read_sound_bank(path);
    const auto& records = bank.samples;
    const auto sample = std::find_if(records.begin(), records.end(),
        [id](const auto& record) { return record.id == id; });
    constexpr unsigned header = 6;
    if (sample == records.end() || sample->bytes.size() < header)
        throw std::runtime_error("Sound sample is missing or truncated");
    const auto* bytes = sample->bytes.data();
    const auto rate = amiga_word(bytes, 0);
    const auto stored = std::uint32_t(amiga_word(bytes, 2)) << 16 | amiga_word(bytes, 4);
    const auto decoded = std::uint64_t(stored) * (bank.packed ? 2 : 1);
    if (!rate || !stored || stored > sample->bytes.size() - header || decoded > 0xffffffffu - 37)
        throw std::runtime_error("Invalid sound sample length or rate");
    const auto length = static_cast<std::uint32_t>(decoded);
    const auto padding = length & 1;

    std::vector<std::uint8_t> wave{'R','I','F','F'};
    wave.reserve(44ull + length + padding);
    auto integer = [&](std::uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            wave.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    };
    integer(36 + length + padding, 4);
    wave.insert(wave.end(), {'W','A','V','E','f','m','t',' '});
    integer(16, 4);
    integer(1, 2); // PCM
    integer(1, 2); // Mono
    integer(rate, 4);
    integer(rate, 4); // Bytes per second
    integer(1, 2); // One byte per sample frame
    integer(8, 2);
    wave.insert(wave.end(), {'d','a','t','a'});
    integer(length, 4);
    for (std::uint32_t i = 0; i < stored; ++i) {
        const auto sample_byte = bytes[header + i];
        if (bank.packed) {
            // The DOS speaker driver dispatches the low nibble first.
            wave.push_back((sample_byte & 15) * 17);
            wave.push_back((sample_byte >> 4) * 17);
        } else {
            wave.push_back(sample_byte ^ 0x80);
        }
    }
    if (padding)
        wave.push_back(0);
    return wave;
}
}
