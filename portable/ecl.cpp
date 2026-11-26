#include "ecl.h"
#include "ecl_containers.h"
#include "ecl_ops.h"
#include "pc98.h"
#include "tlb.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;

std::string upper(std::string value)
{
    for (auto& c : value)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

std::string hex(unsigned value, int digits = 4)
{
    std::ostringstream out;
    out << '$' << std::uppercase << std::hex << std::setw(digits) << std::setfill('0') << value;
    return out.str();
}

unsigned little(const Bytes& bytes, std::size_t offset, unsigned width)
{
    if (offset > bytes.size() || width > bytes.size() - offset)
        throw std::runtime_error("truncated operand");
    unsigned value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= static_cast<unsigned>(bytes[offset + i]) << (8 * i);
    return value;
}

struct Game {
    const char* key;
    const char* title;
    const char* marker;
};

constexpr Game games[]{
    {"por", "POOL OF RADIANCE", "POOL.CFG"},
    {"curse", "CURSE OF THE AZURE BONDS", "CURSE.CFG"},
    {"champions", "CHAMPIONS OF KRYNN", "KRYNN.CFG"},
    {"secret", "SECRET OF THE SILVER BLADES", "BLADES.CFG"},
    {"gateway", "GATEWAY TO THE SAVAGE FRONTIER", "8X8D6.DAX"},
    {"dk", "DEATH KNIGHTS OF KRYNN", "DKK.CFG"},
    {"pod", "POOLS OF DARKNESS", "POOL4.CFG"},
    {"treasures", "TREASURES OF THE SAVAGE FRONTIER", "TREASURE.CFG"},
    {"dqk", "DARK QUEEN OF KRYNN", "DQK.CFG"},
    {"countdown", "COUNTDOWN TO DOOMSDAY", "BUCK.CFG"},
    {"matrix", "MATRIX CUBED", "MATRIX.CFG"},
};

std::string detect_game(const std::filesystem::path& path)
{
    for (auto dir = path.parent_path(); !dir.empty();) {
        auto name = upper(dir.filename().string());
        std::replace(name.begin(), name.end(), '_', ' ');
        std::replace(name.begin(), name.end(), '-', ' ');
        for (const auto& game : games)
            if (name.find(game.title) != std::string::npos)
                return game.key;
        if (name == "SECRET SILVER BLADES")
            return "secret";
        if (name == "BUCK ROGERS")
            return "countdown";
        const auto parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(
             path.has_parent_path() ? path.parent_path() : ".", error)) {
        const auto filename = upper(entry.path().filename().string());
        for (const auto& game : games)
            if (filename == game.marker)
                return game.key;
    }
    return {};
}

struct Operand {
    unsigned tag{};
    unsigned value{};
    std::string text;
};

Operand operand(const Bytes& bytes, std::size_t& offset, bool buck, bool pc98)
{
    Operand result;
    result.tag = little(bytes, offset++, 1);
    if (result.tag == 0x80) {
        if (pc98) {
            const auto start = bytes.begin() + offset;
            const auto end = std::find(start, bytes.end(), 0);
            if (end == bytes.end())
                throw std::runtime_error("unterminated PC-98 string");
            result.text = "\"";
            for (char c : pc98_text({reinterpret_cast<const char*>(bytes.data() + offset),
                                     static_cast<std::size_t>(end - start)})) {
                if (c == '"' || c == '\\')
                    result.text += '\\';
                if (c == '\n')
                    result.text += "\\n";
                else if (c == '\r')
                    result.text += "\\r";
                else
                    result.text += c;
            }
            result.text += '"';
            offset += end - start + 1;
            return result;
        }
        const unsigned length = little(bytes, offset++, 1);
        if (length > bytes.size() - offset)
            throw std::runtime_error("truncated packed string");
        result.text = "\"";
        unsigned bits = 0;
        unsigned count = 0;
        for (unsigned i = 0; i < length; ++i) {
            bits = (bits << 8) | bytes[offset++];
            count += 8;
            while (count >= 6) {
                count -= 6;
                const auto value = (bits >> count) & 63;
                if (value) {
                    const char c = static_cast<char>(value < 32 ? value + 64 : value);
                    if (c == '"' || c == '\\')
                        result.text += '\\';
                    result.text += c;
                }
            }
        }
        result.text += '"';
        return result;
    }
    unsigned width = 2;
    switch (result.tag) {
    case 0: width = 1; break;
    case 1: case 2: case 3: case 0x81: break;
    case 4: if (buck) { width = 4; break; } [[fallthrough]];
    case 5: if (buck && result.tag == 5) break; [[fallthrough]];
    default: throw std::runtime_error("unknown operand type " + hex(result.tag, 2));
    }
    result.value = little(bytes, offset, width);
    offset += width;
    result.text = hex(result.value, width * 2);
    switch (result.tag) {
    case 1: result.text = "byte[" + result.text + "]"; break;
    case 3: result.text = "word[" + result.text + "]"; break;
    case 5: result.text = "long[" + result.text + "]"; break;
    case 0x81: result.text = "string[" + result.text + "]"; break;
    }
    return result;
}

const char* native_call(std::string_view game, unsigned script, unsigned address)
{
    if (game == "c64") {
        const bool wilderness = script >= 25 && script <= 27;
        switch (address) {
        case 0x0806: return "Redraw view frame";
        case 0x2c4e: return "Bank out KERNAL and I/O";
        case 0x2c51: return "Restore KERNAL and I/O";
        case 0x2c90: return "Refresh map and view";
        case 0x8403: return "Update picture animation";
        case 0xba03: return "Play sound effect";
        case 0xc000: return wilderness ? "Draw wilderness view" : nullptr;
        case 0xc003: return wilderness ? "Scroll down" : "Draw scene";
        case 0xc009: return wilderness ? "Scroll right" : "Bank out I/O";
        case 0xc018: return wilderness ? "Write terrain tile" : "Read wall attributes";
        case 0xc01b: return wilderness ? "Read terrain tile" : "Clear 3D work buffers";
        case 0xc01e: return wilderness ? nullptr : "Move forward";
        }
    }
    if (game == "por" || game == "amiga") {
        switch (address) {
        case 0x2c90: return "Refresh map and view";
        case 0x8000: return "Mirror duel";
        case 0x8001: return "Selected character fights alone";
        case 0xba03: return "Play sound cue";
        case 0xc018: return "Read wall ahead";
        case 0xc01e: return "Move forward";
        }
    }
    return nullptr;
}

struct Instruction {
    std::size_t end{};
    unsigned code{};
    std::vector<Operand> args;
};

Instruction decode(const Bytes& bytes, std::size_t offset, const ecl::Opcodes& ops, bool buck, bool pc98)
{
    Instruction result;
    result.code = little(bytes, offset++, 1);
    const auto& op = ops[result.code];
    if (!op.name)
        throw std::runtime_error("unknown opcode " + hex(result.code, 2));
    for (unsigned i = 0; i < op.count; ++i)
        result.args.push_back(operand(bytes, offset, buck, pc98));
    if (op.list) {
        const auto count = result.args.back();
        if (count.tag != 0 && count.tag != 2 && count.tag != 4)
            throw std::runtime_error("operand list has a runtime-dependent length");
        for (unsigned i = 0; i < count.value; ++i)
            result.args.push_back(operand(bytes, offset, buck, pc98));
    }
    result.end = offset;
    return result;
}

struct MapReferences {
    std::set<unsigned> maps;
    struct Dispatch {
        unsigned address;
        Operand selector;
        std::vector<unsigned> targets;
    };
    std::vector<Dispatch> dispatches;
    std::map<unsigned, unsigned> cell_event_copies;
};

RecordRow read_script(DaxRecord record, unsigned slot, std::string game, bool prefixed,
                      bool c64_file, bool amiga_file, MapReferences* references)
{
    RecordRow row{record.id, slot, "Script " + std::to_string(record.id), {}};
    auto& bytes = record.bytes;
    if (record.pc98 && game.empty())
        game = "por";
    if ((c64_file || prefixed) && bytes.size() >= 2 &&
        (c64_file || little(bytes, 0, 2) == 0x1388))
        bytes.erase(bytes.begin(), bytes.begin() + 2);
    if (game.empty() && bytes.size() >= 20) {
        unsigned first = 0xffff;
        for (unsigned i = 0; i < 20; i += 4)
            first = std::min(first, little(bytes, i + 2, 2));
        if (first >= 0x9914 && first < 0xa000)
            game = amiga_file ? "amiga" : "por";
    }
    const unsigned base = (game == "por" || game == "amiga" || game == "c64") ? 0x9900 : 0x8000;
    row.fields.emplace_back("Profile", game.empty() ? "Unknown game (common opcodes only)" : game);
    if (c64_file && game != "c64")
        row.fields.emplace_back("Platform", "Commodore 64");
    if (amiga_file && game != "amiga")
        row.fields.emplace_back("Platform", "Amiga");
    if (record.pc98)
        row.fields.emplace_back("Platform", "PC-98");
    const bool buck = game == "countdown" || game == "matrix";
    auto ops = ecl::opcodes(game);
    if (game.empty()) {
        for (std::string_view profile : {"curse","champions","secret","gateway","dk","pod","treasures","dqk","countdown","matrix"}) {
            const auto candidate = ecl::opcodes(profile);
            for (unsigned i = 0; i < ops.size(); ++i)
                if (ops[i].name && (!candidate[i].name ||
                    std::string_view(ops[i].name) != candidate[i].name ||
                    ops[i].count != candidate[i].count || ops[i].list != candidate[i].list))
                    ops[i] = {};
        }
    }
    std::map<std::size_t, Field> lines;
    std::set<std::size_t> tables;
    std::vector<std::size_t> pending;
    std::vector<bool> occupied(bytes.size());
    for (unsigned i = 0; i < 20 && i + 4 <= bytes.size(); i += 4)
        if (bytes[i] == 1 && bytes[i + 1] == 1)
            pending.push_back(i);
    if (pending.empty())
        row.fields.emplace_back("Warning", "Script has no recognized entry jumps");
    while (!pending.empty()) {
        const auto offset = pending.back();
        pending.pop_back();
        if (offset >= bytes.size() || occupied[offset] || lines.count(offset) || tables.count(offset))
            continue;
        const auto address = hex(base + static_cast<unsigned>(offset));
        try {
            const auto ins = decode(bytes, offset, ops, buck, record.pc98);
            if (std::find(occupied.begin() + offset, occupied.begin() + ins.end, true) != occupied.begin() + ins.end)
                throw std::runtime_error("instruction overlaps previously decoded bytes");
            std::fill(occupied.begin() + offset, occupied.begin() + ins.end, true);
            const auto& op = ops[ins.code];
            std::string line = hex(ins.code, 2) + "  " + op.name;
            if (offset < 20)
                line += " [entry " + std::to_string(offset / 4 + 1) + "]";
            for (unsigned i = 0; i < ins.args.size(); ++i) {
                const auto& arg = ins.args[i];
                const bool jump = ins.code == 1 || ins.code == 2 ||
                                  ((ins.code == 0x25 || ins.code == 0x26) && i >= 2);
                line += i == 0 ? " " : ", ";
                line += jump ? hex(arg.value) : arg.text;
                if (jump) {
                    if (arg.value >= base + 20 && arg.value - base < bytes.size())
                        pending.push_back(arg.value - base);
                    else
                        line += " [outside script]";
                }
                if (ins.code == 0x2d) {
                    if (!c64_file || game == "c64")
                        if (const auto name = native_call(game, record.id, arg.value))
                            line += " [" + std::string(name) + "]";
                }
            }
            if (ins.code == 0x2a || ins.code == 0x35) {
                const auto table = ins.args[ins.code == 0x2a ? 0 : 1].value;
                line += ins.code == 0x2a ? " [table, index, destination]" : " [value, table, index]";
                if (table >= base + 20 && table - base < bytes.size())
                    tables.insert(table - base);
            }
            if (references && ins.code == 0x21 && ins.args[0].tag == 0 &&
                ins.args[0].value < 127)
                references->maps.insert(ins.args[0].value);
            if (references && (game == "por" || game == "amiga" || record.pc98)) {
                constexpr unsigned cell_event = 0xc04f;
                if (ins.code == 0x2f && ins.args[0].tag == 1 && ins.args[0].value == cell_event &&
                    ins.args[1].tag == 0 && ins.args[1].value == 127 && ins.args[2].tag == 1)
                    references->cell_event_copies.emplace(base + ins.end, ins.args[2].value);
            }
            if (references && (ins.code == 0x25 || ins.code == 0x26) &&
                ins.args.size() > 2 && (ins.args[0].tag == 1 || ins.args[0].tag == 3)) {
                MapReferences::Dispatch dispatch{base + static_cast<unsigned>(offset), ins.args[0], {}};
                for (std::size_t i = 2; i < ins.args.size(); ++i) {
                    const auto& arg = ins.args[i];
                    dispatch.targets.push_back(arg.tag == 1 || arg.tag == 2 ? arg.value : 0);
                }
                references->dispatches.push_back(std::move(dispatch));
            }
            const bool skip = buck && ins.code == 0x44;
            const bool conditional = ins.code >= 0x16 && ins.code <= 0x1b;
            if (conditional || skip) {
                try {
                    pending.push_back(decode(bytes, ins.end, ops, buck, record.pc98).end);
                } catch (const std::runtime_error&) {
                    line += " [could not decode skipped instruction]";
                }
            }
            lines.emplace(offset, Field{address, std::move(line)});
            if (!skip && ins.code != 0 && ins.code != 1 && ins.code != 0x13 &&
                ins.code != 0x20 && ins.code != 0x33 &&
                !(ins.code == 0x23 && std::string_view(op.name) == "REDRAW_EXIT") &&
                !(buck && ins.code == 0x42))
                pending.push_back(ins.end);
        } catch (const std::runtime_error& error) {
            lines.emplace(offset, Field{address, std::string("Decode stopped: ") + error.what()});
        }
    }
    for (std::size_t offset = 0; offset < bytes.size();) {
        if (const auto line = lines.find(offset); line != lines.end())
            row.fields.push_back(std::move(line->second));
        if (occupied[offset]) {
            ++offset;
            continue;
        }
        auto end = offset + 1;
        while (end < bytes.size() && !occupied[end] && !lines.count(end) &&
               !tables.count(end) && end - offset < 16)
            ++end;
        std::string data = tables.count(offset) ? "TABLE DATA" : "DATA";
        for (auto i = offset; i < end; ++i)
            data += " " + hex(bytes[i], 2).substr(1);
        row.fields.emplace_back(hex(base + static_cast<unsigned>(offset)), std::move(data));
        offset = end;
    }
    return row;
}
}

bool is_ecl_archive(const std::filesystem::path& path)
{
    const auto stem = upper(path.stem().string());
    const auto extension = upper(path.extension().string());
    if (stem.compare(0, 3, "ECL") != 0)
        return false;
    const auto suffix = stem.substr(3);
    if (extension == ".PRG")
        return suffix.size() == 2 && suffix.find_first_not_of("0123456789ABCDEF") == std::string::npos;
    if (extension == ".DAX" && suffix == "DEMO")
        return true;
    return (extension == ".DAX" || extension == ".GLB") &&
           suffix.find_first_not_of("0123456789") == std::string::npos;
}

static std::vector<RecordRow> read_ecl(const std::filesystem::path& path, std::string_view profile,
                                       std::vector<MapReferences>* references)
{
    const auto extension = upper(path.extension().string());
    std::string game(profile);
    std::vector<DaxRecord> records;
    bool amiga_file = false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open ECL file");
    Bytes file(std::istreambuf_iterator<char>(stream), {});
    if (extension == ".GLB") {
        records = try_amiga_glb(file);
        amiga_file = !records.empty();
        if (amiga_file) {
            if (game.empty())
                game = detect_game(path);
        } else {
            records = read_hlib_data(path);
            game = "dqk";
        }
    } else {
        if (extension == ".PRG") {
            if (file.size() < 22)
                throw std::runtime_error("not a C64 ECL script");
            const unsigned load = little(file, 0, 2);
            if (load != 0x1388 && load != 0x1000 && load != 0x3000 && load != 0x4000)
                throw std::runtime_error("unrecognized C64 ECL load address");
            unsigned base = 0x9900;
            for (unsigned i = 2; i < 22; i += 4)
                if (little(file, i + 2, 2) < base)
                    base = 0x8000;
            for (unsigned i = 2; i < 22; i += 4)
                if (file[i] != 1 || file[i + 1] != 1 || little(file, i + 2, 2) < base + 20 ||
                    little(file, i + 2, 2) - base >= file.size() - 2)
                    throw std::runtime_error("invalid C64 ECL entry table");
            if (base == 0x9900)
                game = "c64";
            else if (game.empty())
                game = detect_game(path);
            if (base == 0x8000 && game == "por")
                game.clear();
            records.push_back({static_cast<unsigned>(std::stoul(path.stem().string().substr(3), nullptr, 16)),
                               static_cast<unsigned>(file.size()), std::move(file)});
        } else {
            records = try_amiga_dax(file);
            amiga_file = !records.empty();
            if (amiga_file) {
                if (game.empty())
                    game = detect_game(path);
                if (game == "por")
                    game = "amiga";
            } else {
                records = read_dax(path);
                if (game.empty())
                    game = detect_game(path);
            }
        }
    }
    if (!game.empty() && game != "amiga" && game != "c64" &&
        std::none_of(std::begin(games), std::end(games), [&](const auto& item) { return game == item.key; }))
        throw std::runtime_error("unknown ECL profile: " + game);
    std::vector<RecordRow> rows;
    if (references)
        references->resize(records.size());
    for (unsigned i = 0; i < records.size(); ++i)
        rows.push_back(read_script(std::move(records[i]), i, game, extension != ".GLB", extension == ".PRG",
                                   amiga_file, references ? &(*references)[i] : nullptr));
    return rows;
}

std::vector<RecordRow> read_ecl_rows(const std::filesystem::path& path, std::string_view profile)
{
    return read_ecl(path, profile, nullptr);
}

std::optional<EclEventLink> find_ecl_event(const std::filesystem::path& archive, unsigned map_id,
                                          unsigned event)
{
    std::vector<MapReferences> references;
    const auto rows = read_ecl(archive, {}, &references);
    std::optional<EclEventLink> best;
    std::tuple<bool, bool, bool, std::size_t> best_rank{};
    bool ambiguous = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& ref = references[i];
        const bool loads_map = ref.maps.count(map_id) != 0;
        const bool matching_id = rows[i].id == map_id;
        if (!loads_map && !matching_id)
            continue;
        for (const auto& dispatch : ref.dispatches) {
            const auto copied = ref.cell_event_copies.find(dispatch.address);
            const bool exact = copied != ref.cell_event_copies.end() && copied->second == dispatch.selector.value;
            if (event >= dispatch.targets.size() || (!exact && dispatch.targets.size() <= 4))
                continue;
            const auto target = dispatch.targets[event];
            if (!target)
                continue;
            const auto address = hex(target);
            const auto& fields = rows[i].fields;
            const auto line = std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
                return field.first == address && !field.second.empty() && field.second.front() == '$';
            });
            if (line == fields.end())
                continue;
            const auto rank = std::make_tuple(exact, loads_map, matching_id, dispatch.targets.size());
            const auto instruction = static_cast<std::size_t>(std::distance(fields.begin(), line));
            if (!best || rank > best_rank) {
                best = EclEventLink{i, instruction, !exact};
                best_rank = rank;
                ambiguous = false;
            } else if (rank == best_rank && (best->script != i || best->instruction != instruction)) {
                ambiguous = true;
            }
        }
    }
    if (best && !ambiguous)
        return best;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].id == map_id)
            return EclEventLink{i, {}, true};
    return {};
}

void write_ecl_listing(std::ostream& output, const RecordRow& script)
{
    output << script.name << '\n';
    for (const auto& [address, text] : script.fields)
        output << address << '\t' << text << '\n';
}
}
