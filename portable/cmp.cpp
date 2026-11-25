#include "cmp.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr unsigned screen_width = 320;
constexpr unsigned screen_height = 200;
constexpr unsigned tile_size = 24;
constexpr unsigned tile_count = 48;
constexpr unsigned wall_count = 25;
constexpr unsigned wall_record_size = 0x10ec;
constexpr unsigned palette[]{
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0x000000, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff
};

std::string upper(std::string value)
{
    for (auto& c : value)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

bool is_maze(std::string_view name)
{
    return name == "MAZE0" || name == "MAZE1" || name == "MAZE2";
}

unsigned word(const Bytes& bytes, std::size_t offset)
{
    return bytes[offset] | (unsigned(bytes[offset + 1]) << 8);
}

void expand_runs(const Bytes& source, std::size_t& input, Bytes& packed, bool delta)
{
    std::size_t output = 0;
    while (output < packed.size()) {
        if (input == source.size())
            throw std::runtime_error("Truncated Hillsfar graphics run");
        const auto token = source[input++];
        const bool repeat = token == 0 || token >= 128;
        unsigned count = token < 128 ? token : 256 - token;
        if (!token) {
            if (source.size() - input < 2)
                throw std::runtime_error("Truncated Hillsfar graphics long run");
            // ANM stores extended run counts in the opposite byte order from CMP.
            count = delta ? (unsigned(source[input]) << 8) | source[input + 1] : word(source, input);
            input += 2;
        }
        const auto consumed = repeat ? 1u : count;
        if (!count || count > packed.size() - output || consumed > source.size() - input)
            throw std::runtime_error("Invalid Hillsfar graphics run length");
        if (delta) {
            for (unsigned i = 0; i < count; ++i)
                packed[output + i] ^= source[input + (repeat ? 0 : i)];
        } else if (repeat)
            std::fill_n(packed.data() + output, count, source[input]);
        else
            std::copy_n(source.data() + input, count, packed.data() + output);
        input += consumed;
        output += count;
    }
}

void put_pixel(std::uint8_t* rgba, unsigned index)
{
    const auto color = palette[index];
    rgba[0] = static_cast<std::uint8_t>(color >> 16);
    rgba[1] = static_cast<std::uint8_t>(color >> 8);
    rgba[2] = static_cast<std::uint8_t>(color);
    rgba[3] = 255;
}

Frame render(const std::uint8_t* packed, unsigned id, unsigned width, unsigned height, bool columns)
{
    Frame frame{id, 0, width, height, Bytes(width * height * 4)};
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width / 2; ++x) {
            const auto pair = packed[columns ? x * height + y : y * (width / 2) + x];
            auto* rgba = frame.rgba.data() + (y * width + 2 * x) * 4;
            put_pixel(rgba, pair >> 4);
            put_pixel(rgba + 4, pair & 15);
        }
    }
    return frame;
}
}

bool is_cmp_archive(const std::filesystem::path& path)
{
    if (upper(path.extension().string()) != ".CMP")
        return false;
    constexpr std::string_view names[]{
        "AGUY0", "AGUY1", "AGUY2", "AGUY3", "ARENA", "CAMP", "CRED", "EQBDR",
        "EQOUT", "EQPATH", "EQSCRN", "EQUIPTR", "EQUISMAP", "ETHEQ", "GUILDS", "HERO",
        "HFBORDER", "ICONS", "LCSCR", "MAP", "MAZE0", "MAZE1", "MAZE2", "MAZEGUYS",
        "NPC", "SHOPS", "SKYFLOOR", "SYMBOLS", "TARGOBJS", "TARGSCRN", "TITLE", "TOOLS"
    };
    const auto name = upper(path.stem().string());
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

PictureArchive read_cmp(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Unable to open the Hillsfar CMP file");
    const auto length = file.tellg();
    if (length < 3 || length > 65537)
        throw std::runtime_error("Invalid Hillsfar CMP file size");
    Bytes source(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(source.data()), length))
        throw std::runtime_error("Unable to read the Hillsfar CMP file");
    if (word(source, 0) != source.size() - 2 || (source[2] != 1 && source[2] != 2))
        throw std::runtime_error("Invalid Hillsfar CMP header");

    Bytes packed(screen_width * screen_height / 2);
    std::size_t input = 3;
    expand_runs(source, input, packed, false);
    if (input != source.size())
        throw std::runtime_error("Unexpected bytes after the Hillsfar CMP raster");
    PictureArchive result;
    if (is_maze(upper(path.stem().string()))) {
        if (source[2] != 1)
            throw std::runtime_error("Invalid Hillsfar maze tile storage order");
        // The DOS loader walks 48 tiles at a 0x120-byte stride.
        for (unsigned i = 0; i < tile_count; ++i)
            result.frames.push_back(render(packed.data() + i * tile_size * tile_size / 2,
                                           i, tile_size, tile_size, false));
    } else {
        result.frames.push_back(render(packed.data(), 1, screen_width, screen_height, source[2] == 2));
    }
    return result;
}

bool is_anm_archive(const std::filesystem::path& path)
{
    if (upper(path.extension().string()) != ".ANM")
        return false;
    const auto name = upper(path.stem().string());
    return name == "CLERIC" || name == "FIGHTER" || name == "MU" || name == "THIEF";
}

PictureArchive read_anm(const std::filesystem::path& path)
{
    constexpr unsigned header_size = 0x3d;
    constexpr unsigned sequence_size = 32;
    constexpr unsigned width = 160;
    constexpr unsigned height = 72;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Unable to open the Hillsfar ANM file");
    const auto length = file.tellg();
    if (length <= header_size || length > 10000)
        throw std::runtime_error("Invalid Hillsfar ANM file size");
    Bytes source(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(source.data()), length))
        throw std::runtime_error("Unable to read the Hillsfar ANM file");
    const auto end = std::find(source.begin(), source.begin() + sequence_size, 0);
    if (end == source.begin() || end == source.begin() + sequence_size)
        throw std::runtime_error("Invalid Hillsfar ANM sequence");

    Bytes packed(width * height / 2);
    std::size_t input = header_size;
    PictureArchive result;
    const auto count = static_cast<unsigned>(end - source.begin());
    for (unsigned i = 0; i < count; ++i) {
        expand_runs(source, input, packed, true);
        result.frames.push_back(render(packed.data(), i + 1, width, height, false));
    }
    if (input != source.size())
        throw std::runtime_error("Unexpected bytes after the Hillsfar ANM frames");
    return result;
}

bool is_packwall_archive(const std::filesystem::path& path)
{
    return upper(path.filename().string()) == "PACKWALL.OUT";
}

PictureArchive read_packwall(const std::filesystem::path& path)
{
    constexpr unsigned sheet_width = 272;
    constexpr unsigned sheet_height = 96;
    constexpr unsigned perspectives = 11;
    constexpr auto archive_size = wall_count * wall_record_size;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != archive_size)
        throw std::runtime_error("Expected a 108300-byte Hillsfar PACKWALL.OUT archive");
    Bytes source(archive_size);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(source.data()), source.size()))
        throw std::runtime_error("Unable to read Hillsfar PACKWALL.OUT");

    PictureArchive result;
    result.frames.reserve(wall_count);
    for (unsigned entry = 0; entry < wall_count; ++entry) {
        auto offset = entry * wall_record_size;
        const auto end = offset + wall_record_size;
        Frame sheet{entry + 1, 0, sheet_width, sheet_height, Bytes(sheet_width * sheet_height * 4)};
        unsigned left = 0;
        for (unsigned piece = 0; piece < perspectives; ++piece) {
            if (end - offset < 4)
                throw std::runtime_error("Truncated Hillsfar wall perspective header");
            const unsigned height = source[offset + 1] + 1;
            const unsigned width = source[offset + 2] * 8;
            offset += 4;
            const auto length = width / 2 * height;
            if (!width || left + width > sheet_width || height > sheet_height || length > end - offset)
                throw std::runtime_error("Invalid Hillsfar wall perspective dimensions");
            for (unsigned y = 0; y < height; ++y) {
                for (unsigned x = 0; x < width / 2; ++x) {
                    const auto pair = source[offset + y * (width / 2) + x];
                    auto* target = sheet.rgba.data() + (y * sheet_width + left + 2 * x) * 4;
                    if (const auto index = pair >> 4)
                        put_pixel(target, index);
                    if (const auto index = pair & 15)
                        put_pixel(target + 4, index);
                }
            }
            left += width + 4;
            offset += length;
        }
        if (offset != end)
            throw std::runtime_error("Unexpected bytes after Hillsfar wall perspectives");
        result.frames.push_back(std::move(sheet));
    }
    return result;
}
}
