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

Bytes unpack_cmp(const Bytes& source)
{
    Bytes packed(screen_width * screen_height / 2);
    std::size_t input = 3;
    std::size_t output = 0;
    while (output < packed.size()) {
        if (input == source.size())
            throw std::runtime_error("Truncated Hillsfar CMP run");
        const auto token = source[input++];
        const bool repeat = token == 0 || token >= 128;
        unsigned count = token < 128 ? token : 256 - token;
        if (!token) {
            if (source.size() - input < 2)
                throw std::runtime_error("Truncated Hillsfar CMP long run");
            count = word(source, input);
            input += 2;
        }
        const auto consumed = repeat ? 1u : count;
        if (!count || count > packed.size() - output || consumed > source.size() - input)
            throw std::runtime_error("Invalid Hillsfar CMP run length");
        if (repeat)
            std::fill_n(packed.data() + output, count, source[input]);
        else
            std::copy_n(source.data() + input, count, packed.data() + output);
        input += consumed;
        output += count;
    }
    if (input != source.size())
        throw std::runtime_error("Unexpected bytes after the Hillsfar CMP raster");
    return packed;
}

Frame render(const std::uint8_t* packed, unsigned id, unsigned width, unsigned height, bool columns)
{
    Frame frame{id, 0, width, height, Bytes(width * height * 4)};
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width / 2; ++x) {
            const auto pair = packed[columns ? x * height + y : y * (width / 2) + x];
            for (unsigned pixel = 0; pixel < 2; ++pixel) {
                const auto color = palette[pixel ? pair & 15 : pair >> 4];
                auto* rgba = frame.rgba.data() + (y * width + 2 * x + pixel) * 4;
                rgba[0] = static_cast<std::uint8_t>(color >> 16);
                rgba[1] = static_cast<std::uint8_t>(color >> 8);
                rgba[2] = static_cast<std::uint8_t>(color);
                rgba[3] = 255;
            }
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

    const auto packed = unpack_cmp(source);
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
}
