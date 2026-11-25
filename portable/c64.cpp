#include "c64.h"
#include "geo.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>

namespace goldbox {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using Color = std::array<std::uint8_t, 4>;
constexpr Color palette[]{
    {0,0,0,255}, {255,255,255,255}, {129,51,56,255}, {117,206,200,255},
    {142,60,151,255}, {86,172,77,255}, {46,44,155,255}, {237,241,113,255},
    {142,80,41,255}, {85,56,0,255}, {196,108,113,255}, {74,74,74,255},
    {123,123,123,255}, {169,255,159,255}, {112,109,235,255}, {178,178,178,255}
};

std::string upper(std::string value)
{
    for (auto& c : value)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

Bytes read_prg(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open C64 resource");
    return {std::istreambuf_iterator<char>(in), {}};
}

unsigned word(const Bytes& b, std::size_t at)
{
    return unsigned(b.at(at)) | unsigned(b.at(at + 1)) << 8;
}

bool relocated(unsigned address)
{
    return address == 0x3000 || address == 0x4000;
}

void require(const Bytes& bytes, std::size_t size, unsigned address)
{
    if (bytes.size() != size || word(bytes, 0) != address)
        throw std::runtime_error("Unexpected C64 resource size or load address");
}

void require_shared(const Bytes& bytes, std::size_t size, unsigned address)
{
    if (bytes.size() != size || (word(bytes, 0) != address && !relocated(word(bytes, 0))))
        throw std::runtime_error("Unexpected C64 resource size or load address");
}

std::optional<fs::path> find_file(const fs::path& folder, const std::string& name)
{
    std::error_code error;
    for (const auto& file : fs::directory_iterator(folder, error))
        if (upper(file.path().filename().string()) == name && file.is_regular_file())
            return file.path();
    return {};
}

bool disk_folder(const fs::path& path)
{
    const auto name = upper(path.filename().string());
    return name.size() == 5 && name.compare(0, 4, "DISK") == 0 && name.back() >= '1' && name.back() <= '8';
}

Bytes companion(const fs::path& path, const std::string& name)
{
    const auto folder = fs::absolute(path).parent_path();
    if (const auto local = find_file(folder, name))
        return read_prg(*local);
    if (!disk_folder(folder))
        return {};
    const auto parent = folder.parent_path();
    if (const auto flat = find_file(parent, name))
        return read_prg(*flat);
    Bytes result;
    for (const auto& disk : fs::directory_iterator(parent)) {
        if (!disk.is_directory() || !disk_folder(disk.path()))
            continue;
        if (const auto file = find_file(disk.path(), name)) {
            auto candidate = read_prg(*file);
            if (!result.empty() && result != candidate)
                return {};
            result = std::move(candidate);
        }
    }
    return result;
}

Frame image(unsigned id, unsigned index, unsigned width, unsigned height)
{
    return {id, index, width, height, Bytes(width * height * 4)};
}

void pixel(Frame& frame, unsigned x, unsigned y, Color color)
{
    std::copy(color.begin(), color.end(), frame.rgba.begin() + (y * frame.width + x) * 4);
}

void cell(Frame& frame, unsigned x, unsigned y, const std::uint8_t* glyph, unsigned color,
          unsigned bg, unsigned shared1, unsigned shared2, bool multi, bool transparent)
{
    for (unsigned dy = 0; dy < 8; ++dy)
        for (unsigned dx = 0; dx < 8; ++dx) {
            const unsigned code = multi ? (glyph[dy] >> (6 - 2 * (dx / 2))) & 3
                                        : (glyph[dy] >> (7 - dx)) & 1;
            if (transparent && !code)
                continue;
            const unsigned ink = !code ? bg : !multi ? color :
                code == 1 ? shared1 : code == 2 ? shared2 : color & 7;
            pixel(frame, x + dx, y + dy, palette[ink & 15]);
        }
}

void xor_plane(const Bytes& bytes, std::size_t& at, Bytes& plane)
{
    std::size_t out = 0;
    while (at < bytes.size()) {
        const auto code = bytes[at++];
        if (!code)
            return;
        const unsigned count = code < 128 ? code : 256 - code;
        const unsigned stored = code < 128 ? count : 1;
        if (count > plane.size() - out || stored > bytes.size() - at)
            throw std::runtime_error("Invalid C64 picture RLE run");
        for (unsigned i = 0; i < count; ++i)
            plane[out++] ^= bytes[at + (code < 128 ? i : 0)];
        at += stored;
    }
    throw std::runtime_error("Missing C64 picture RLE terminator");
}

PictureArchive pictures(const Bytes& bytes, unsigned id, std::optional<unsigned> shared = {})
{
    if (bytes.size() < 60 || (word(bytes, 0) != 0x5000 && !relocated(word(bytes, 0))))
        throw std::runtime_error("Unsupported C64 picture header");
    const unsigned cols = bytes[47], rows = bytes[48], cells = cols * rows;
    if (!cols || cols > 40 || !rows || rows > 25 || word(bytes, 49) != cells * 8 || word(bytes, 51) != cells)
        throw std::runtime_error("Invalid C64 picture dimensions");
    PictureArchive result;
    PictureArchive::Clip clip{id, {}};
    const bool still = id == 0x4f && relocated(word(bytes, 0)) &&
        std::all_of(bytes.begin() + 2, bytes.begin() + 34, [](auto command) { return command == 0; });
    Bytes bitmap(cells * 8), colors(cells);
    std::size_t at = 58;
    while (at < bytes.size()) {
        const auto index = static_cast<unsigned>(result.frames.size());
        if (!still && (index >= 32 || bytes[2 + index] < 'A' || bytes[2 + index] > 'L'))
            throw std::runtime_error("Invalid C64 animation command");
        xor_plane(bytes, at, bitmap);
        xor_plane(bytes, at, colors);
        auto frame = image(id, index, cols * 8, rows * 8);
        for (unsigned i = 0; i < cells; ++i) {
            if (colors[i] > 15)
                throw std::runtime_error("Invalid C64 picture cell color");
            cell(frame, i % cols * 8, i / cols * 8, bitmap.data() + i * 8, colors[i],
                 bytes[54] & 15, shared.value_or(bytes[55] & 15), bytes[56] & 15, colors[i] & 8, false);
        }
        result.frames.push_back(std::move(frame));
        if (still) {
            // Buck Rogers PIC4F stores the same packet twice, followed by zero padding.
            const auto packet_size = at - 58;
            if (packet_size > bytes.size() - at ||
                !std::equal(bytes.begin() + 58, bytes.begin() + at, bytes.begin() + at) ||
                !std::all_of(bytes.begin() + at + packet_size, bytes.end(), [](auto byte) { return byte == 0; }))
                throw std::runtime_error("Invalid C64 still picture packet");
            return result;
        }
        const auto ticks = std::min(255, (bytes[46] + 1) * bytes[34 + bytes[2 + index] - 'A']);
        clip.steps.push_back({index, std::max(1, (ticks * 1000 + 30) / 60)});
    }
    if (result.frames.size() < 32 && bytes[2 + result.frames.size()] != 0)
        throw std::runtime_error("C64 picture command and frame counts disagree");
    if (clip.steps.size() > 1)
        result.clips.push_back(std::move(clip));
    return result;
}

Frame glyph_atlas(const Bytes& bytes, unsigned id, bool combat)
{
    const auto count = static_cast<unsigned>((bytes.size() - 2 + 7) / 8);
    auto frame = image(id, 0, 128, (count + 15) / 16 * 8);
    for (unsigned i = 0; i < frame.width * frame.height / 64; ++i) {
        std::array<std::uint8_t, 8> glyph{};
        if (i < count) {
            const auto at = 2 + i * 8;
            std::copy_n(bytes.data() + at, std::min<std::size_t>(8, bytes.size() - at), glyph.begin());
        }
        cell(frame, i % 16 * 8, i / 16 * 8, glyph.data(), combat ? 15 : 1,
             combat ? 11 : 0, 10, 0, combat, false);
    }
    return frame;
}

Frame map_image(const Bytes& bytes, unsigned id)
{
    if (bytes.size() != 1026 ||
        (word(bytes, 0) != 0x0400 && word(bytes, 0) != 0x0004 && !relocated(word(bytes, 0))))
        throw std::runtime_error("Invalid C64 GEO map");
    const auto map = read_geo_map({id, 1024, bytes});
    constexpr unsigned step = 12;
    auto frame = image(id, 0, 16 * step + 1, 16 * step + 1);
    for (unsigned y = 0; y < frame.height; ++y)
        for (unsigned x = 0; x < frame.width; ++x)
            pixel(frame, x, y, palette[0]);
    constexpr unsigned inks[]{1, 7, 10, 2};
    for (unsigned i = 0; i < map.size(); ++i) {
        const unsigned x = i % 16 * step, y = i / 16 * step;
        const auto& tile = map[i];
        for (unsigned dot = 0; dot <= step; ++dot) {
            if (tile.walls[0]) pixel(frame, x + dot, y, palette[inks[tile.doors[0]]]);
            if (tile.walls[1]) pixel(frame, x + step, y + dot, palette[inks[tile.doors[1]]]);
            if (tile.walls[2]) pixel(frame, x + dot, y + step, palette[inks[tile.doors[2]]]);
            if (tile.walls[3]) pixel(frame, x, y + dot, palette[inks[tile.doors[3]]]);
        }
        if (tile.event)
            for (unsigned dy = 5; dy < 8; ++dy)
                for (unsigned dx = 5; dx < 8; ++dx)
                    pixel(frame, x + dx, y + dy, palette[13]);
    }
    return frame;
}

void sprite(Frame& frame, unsigned left, unsigned top, const std::uint8_t* data, unsigned flags,
            unsigned one, unsigned two, unsigned scale_x = 1, unsigned scale_y = 1)
{
    const bool multi = flags & 128;
    for (unsigned y = 0; y < 21; ++y)
        for (unsigned x = 0; x < 24; ++x) {
            const auto bits = data[y * 3 + x / 8];
            const unsigned code = multi ? (bits >> (6 - 2 * ((x % 8) / 2))) & 3 : (bits >> (7 - x % 8)) & 1;
            if (!code)
                continue;
            const auto ink = palette[multi && code != 2 ? (code == 1 ? one : two) : flags & 15];
            for (unsigned dy = 0; dy < scale_y; ++dy)
                for (unsigned dx = 0; dx < scale_x; ++dx)
                    pixel(frame, left + x * scale_x + dx, top + y * scale_y + dy, ink);
        }
}

PictureArchive encounter_sprites(const Bytes& bytes, unsigned id)
{
    const auto address = word(bytes, 0);
    if (relocated(address) || address == 0x1000) {
        // DUNGEON indexes six-byte descriptors at E300 (E000 in Silver Blades).
        // Distance groups are 0..1, 2..5, and 6..11. VIC $D010 is cleared.
        unsigned first, count, descriptor;
        if (bytes.size() == 850) {
            first = 2;
            count = 12;
            descriptor = 770;
        } else if (bytes.size() == 463 || bytes.size() == 847) {
            first = 79;
            count = (bytes.size() - first) / 64;
            descriptor = 2;
        } else {
            throw std::runtime_error("Invalid later C64 encounter sprite bank");
        }
        PictureArchive result;
        const unsigned one = bytes[descriptor + 72] & 15, two = bytes[descriptor + 73] & 15;
        // Use one crop for all distances so moving between them preserves screen-relative placement.
        unsigned x0 = 256, y0 = 256, x1 = 0, y1 = 0;
        for (unsigned n = 0; n < count; ++n) {
            const auto* pos = bytes.data() + descriptor + n * 6;
            x0 = std::min(x0, unsigned(pos[0]));
            y0 = std::min(y0, unsigned(pos[1]));
            x1 = std::max(x1, pos[0] + (pos[2] ? 48u : 24u));
            y1 = std::max(y1, pos[1] + (pos[3] ? 42u : 21u));
        }
        unsigned start = 0;
        for (unsigned parts : {2u,4u,6u}) {
            if (start + parts > count)
                break;
            auto frame = image(id, static_cast<unsigned>(result.frames.size()), x1 - x0, y1 - y0);
            for (unsigned n = start + parts; n-- > start;) {
                const auto* pos = bytes.data() + descriptor + n * 6;
                const auto* bits = bytes.data() + first + 64 * n;
                sprite(frame, pos[0] - x0, pos[1] - y0, bits, (pos[5] & 15) | (pos[4] ? 128 : 0),
                       one, two, pos[2] ? 2 : 1, pos[3] ? 2 : 1);
            }
            result.frames.push_back(std::move(frame));
            start += parts;
        }
        for (unsigned n = 0; n < count; ++n) {
            auto frame = image(id, static_cast<unsigned>(result.frames.size()), 24, 21);
            const auto* bits = bytes.data() + first + 64 * n;
            const auto* pos = bytes.data() + descriptor + n * 6;
            sprite(frame, 0, 0, bits, (pos[5] & 15) | (pos[4] ? 128 : 0), one, two);
            result.frames.push_back(std::move(frame));
        }
        return result;
    }
    if (bytes.size() < 130 || word(bytes, 0) != 0xd800 || (bytes.size() - 66) % 64)
        throw std::runtime_error("Invalid C64 encounter sprite bank");
    const unsigned count = (bytes.size() - 66) / 64;
    if (bytes[10] > 6 || bytes[11] > 6 || bytes[12] > 6 || unsigned(bytes[10]+bytes[11]+bytes[12]) != count)
        throw std::runtime_error("Invalid C64 encounter sprite count");
    PictureArchive result;
    const unsigned one = bytes[2] & 15, two = bytes[3] & 15;
    unsigned first = 0;
    for (unsigned distance = 0; distance < 3; ++distance) {
        const unsigned parts = bytes[10 + distance];
        std::array<std::pair<unsigned,unsigned>, 6> positions{};
        unsigned x0 = 256, y0 = 256, x1 = 0, y1 = 0;
        for (unsigned part = 0; part < parts; ++part) {
            const auto n = first + part;
            const unsigned x = (302 - ((bytes[18 + 2*n] + bytes[4 + 2*distance]) & 255)) & 255;
            const unsigned y = (328 - ((bytes[19 + 2*n] + bytes[5 + 2*distance]) & 255)) & 255;
            positions[part] = {x, y};
            x0 = std::min(x0, x); y0 = std::min(y0, y);
            x1 = std::max(x1, x + 24); y1 = std::max(y1, y + 21);
        }
        auto frame = image(id, distance, parts ? x1 - x0 : 1, parts ? y1 - y0 : 1);
        for (unsigned part = parts; part > 0; --part) {
            const auto [x,y] = positions[part-1];
            const auto* bits = bytes.data() + 66 + 64 * (first + part - 1);
            sprite(frame, x - x0, y - y0, bits, bits[63], one, two);
        }
        result.frames.push_back(std::move(frame));
        first += parts;
    }
    for (unsigned n = 0; n < count; ++n) {
        auto frame = image(id, n + 3, 24, 21);
        const auto* bits = bytes.data() + 66 + 64 * n;
        sprite(frame, 0, 0, bits, bits[63], one, two);
        result.frames.push_back(std::move(frame));
    }
    return result;
}

PictureArchive combat_pictures(const Bytes& bytes, unsigned id)
{
    const unsigned address = word(bytes,0), count = (bytes.size() - 2) / 162;
    if ((address != 0x2f5c && !relocated(address)) || (count != 1 && count != 2 && count != 4) ||
        (bytes.size() - 2) % 162 > 1 || (address == 0x2f5c && bytes.size() != 164))
        throw std::runtime_error("Invalid C64 combat sprite bank");
    PictureArchive result;
    const std::array<unsigned,2> widths{count == 4 ? 48u : 24u, 48};
    for (unsigned layout = 0; layout < (count == 2 ? 2u : 1u); ++layout) {
        const auto width = widths[layout], height = count * 24 * 24 / width;
        for (unsigned mirror = 0; mirror < 2; ++mirror) {
            PictureArchive::Clip clip{id, {}};
            for (unsigned pose = 0; pose < 2; ++pose) {
                const auto index = static_cast<unsigned>(result.frames.size());
                auto frame = image(id, index, width, height);
                for (unsigned part = 0; part < count; ++part) {
                    const unsigned left = width == 24 ? 0 : (1 - part / (height / 24)) * 24;
                    const unsigned top = (part % (height / 24)) * 24;
                    const auto* bits = bytes.data() + 2 + part * 162;
                    for (unsigned i = 0; i < 9; ++i) {
                        const auto ink = bits[144 + pose * 9 + i] & 15;
                        cell(frame, left + i % 3 * 8, top + i / 3 * 8, bits + pose * 72 + i * 8,
                             ink, 11, 10, 0, ink & 8, true);
                    }
                }
                if (mirror)
                    for (unsigned y = 0; y < height; ++y)
                        for (unsigned x = 0; x < width / 2; ++x)
                            std::swap_ranges(frame.rgba.begin() + 4 * (y * width + x),
                                             frame.rgba.begin() + 4 * (y * width + x + 1),
                                             frame.rgba.begin() + 4 * (y * width + width - 1 - x));
                result.frames.push_back(std::move(frame));
                clip.steps.push_back({index, 200});
            }
            result.clips.push_back(std::move(clip));
        }
    }
    return result;
}

Frame title(const fs::path& path, const Bytes& bytes)
{
    if (bytes.size() < 3 || (word(bytes, 0) != 0x3a98 && !relocated(word(bytes, 0))))
        throw std::runtime_error("Invalid C64 title header");
    const bool pool = bytes.size() == 3933 && word(bytes, 0) == 0x3a98;
    Bytes raw;
    raw.reserve(9216);
    std::size_t at = 2;
    while (at + 1 < bytes.size()) {
        const auto control = bytes[at++];
        const unsigned count = (control & 127) ? control & 127 : 256;
        const unsigned input = control & 128 ? 1 : count;
        if (count > 9216 - raw.size() || input > bytes.size() - 1 - at)
            throw std::runtime_error("Invalid C64 title RLE");
        if (input == 1 && (control & 128))
            raw.insert(raw.end(), count, bytes[at]);
        else
            raw.insert(raw.end(), bytes.begin() + at, bytes.begin() + at + input);
        at += input;
    }
    if ((raw.size() != 9216 && raw.size() != 9024) || at + 1 != bytes.size() ||
        (bytes[at] != 255 && bytes[at] != 0))
        throw std::runtime_error("Incomplete C64 title RLE");
    const auto sprites = pool ? companion(path, "SPR.TP") : Bytes{};
    if (pool)
        require(sprites, 450, 0x0800);
    auto frame = image(0, 0, 320, 200);
    constexpr unsigned starts[]{132,150,173};
    for (unsigned y = 0; y < 200; ++y)
        for (unsigned x = 0; x < 320; ++x) {
            const auto i = y / 8 * 40 + x / 8;
            const bool foreground = raw[1024 + i * 8 + y % 8] & (128 >> (x % 8));
            unsigned ink = foreground ? raw[i] >> 4 : raw[i] & 15;
            if (pool && !foreground && y >= 2 && y < 23)
                for (unsigned s = 0; s < 3; ++s) {
                    const unsigned dx = x - starts[s];
                    if (dx < 24 && (sprites[2 + s * 64 + (y - 2) * 3 + dx / 8] & (128 >> (dx % 8))))
                        ink = 2;
                }
            pixel(frame, x, y, palette[ink]);
        }
    return frame;
}

PictureArchive credits(const fs::path& path, const Bytes& bytes)
{
    const bool original = bytes.size() == 2941 && word(bytes, 0) == 0x1000;
    const auto font = companion(path, "CHARSET.PRG");
    require_shared(font, 514, 0x1000);
    if (!original) {
        // Match the original INIT command reader and its LDA/STA split pointer lookup.
        // The executable is relocated to $0800 regardless of the PRG transfer address.
        constexpr unsigned base = 0x7fe;
        for (unsigned i = 2; i + 20 <= bytes.size(); ++i) {
            if (bytes[i] != 0x8c || bytes[i+3] != 0xbe || bytes[i+6] != 0x30 ||
                bytes[i+8] != 0x20 || bytes[i+11] != 0x4c ||
                bytes[i+14] != 0xe0 || bytes[i+15] != 0xff ||
                bytes[i+18] != 0xe0 || bytes[i+19] != 0xfe)
                continue;
            const auto call = word(bytes, i + 9);
            const auto stream = word(bytes, i + 4);
            if (call < 0x800 || stream < 0x800 || call - base + 13 > bytes.size() ||
                stream - base >= bytes.size())
                continue;
            const auto p = call - base;
            if (bytes[p] != 0xbd || bytes[p+3] != 0x85 || bytes[p+4] != 7 ||
                bytes[p+5] != 0xbd || bytes[p+8] != 0x85 || bytes[p+9] != 8 || bytes[p+10] != 0x4c)
                continue;
            const auto lo = word(bytes, p + 1), hi = word(bytes, p + 6);
            if (lo < 0x800 || hi <= lo || hi - base >= bytes.size())
                continue;
            const bool shifted = bytes.size() == 3368; // Silver Blades starts on row 6.
            auto frame = image(0, 0, 320, 200);
            for (std::size_t at = 3; at < frame.rgba.size(); at += 4)
                frame.rgba[at] = 255;
            unsigned row = shifted ? 6 : 0, col = 0, ink = 5;
            for (auto at = stream - base; at < bytes.size() && at < stream - base + 256; ++at) {
                const auto cmd = bytes[at];
                if (cmd == 255)
                    return {{std::move(frame)}, {}};
                if (cmd == 254) {
                    ++row;
                    col = shifted ? 1 : 0;
                } else if (cmd >= 192) {
                    col = cmd & 63;
                } else if (cmd >= 128) {
                    ink = cmd & ((bytes.size() == 2191 || bytes.size() == 5453) ? 7 : 15);
                } else {
                    if (cmd >= hi - lo || hi - base + cmd >= bytes.size())
                        throw std::runtime_error("C64 credits string ID outside its pointer table");
                    const auto target = unsigned(bytes[lo - base + cmd]) |
                                        unsigned(bytes[hi - base + cmd]) << 8;
                    if (target < 0x800 || target - base >= bytes.size())
                        throw std::runtime_error("C64 credits string pointer outside INIT");
                    auto pos = target - base;
                    while (pos < bytes.size() && bytes[pos]) {
                        if (row < 25 && col < 40)
                            cell(frame, col * 8, row * 8, font.data() + 2 + (bytes[pos] & 63) * 8,
                                 ink, 0, 0, 0, false, false);
                        ++pos;
                        ++col;
                    }
                    if (pos == bytes.size())
                        throw std::runtime_error("Unterminated C64 credits text");
                }
            }
            throw std::runtime_error("Unterminated C64 credits command stream");
        }
        throw std::runtime_error("Unknown C64 INIT credits interpreter");
    }
    std::array<unsigned, 1000> glyphs, inks{};
    glyphs.fill(32);
    for (unsigned row = 0; row < 24; ++row) {
        if (row == 0 || row == 2 || row == 7 || row == 23) {
            for (unsigned col = 1; col < 39; ++col) {
                glyphs[row * 40 + col] = 27;
                inks[row * 40 + col] = 2;
            }
            glyphs[row * 40] = glyphs[row * 40 + 39] = 0;
            inks[row * 40] = inks[row * 40 + 39] = 7;
        } else {
            glyphs[row * 40] = glyphs[row * 40 + 39] = 36;
            inks[row * 40] = inks[row * 40 + 39] = 2;
        }
    }
    unsigned column = 0, line = 0, color = 5;
    std::size_t at = 0x1173 - 0x7fe;
    while (at < bytes.size() && bytes[at] != 255) {
        const auto command = bytes[at++];
        if (command < 29) {
            const unsigned pointer = bytes[0x108d - 0x7fe + command] |
                                     unsigned(bytes[0x10b5 - 0x7fe + command]) << 8;
            if (pointer < 0x800 || pointer - 0x7fe >= bytes.size())
                throw std::runtime_error("Invalid C64 credits pointer");
            const auto start = pointer - 0x7fe;
            unsigned n = 0;
            while (start + n < bytes.size() && bytes[start + n] && n < 40) {
                if (column + n < 40 && line < 25) {
                    glyphs[line * 40 + column + n] = bytes[start + n] & 63;
                    inks[line * 40 + column + n] = color;
                }
                ++n;
            }
            if (start + n >= bytes.size())
                throw std::runtime_error("Truncated C64 credits string");
            column += n;
        } else if (command == 254) {
            ++line;
            column = 0;
        } else if (command < 192) {
            color = command & 7;
        } else {
            column = command & 63;
        }
    }
    if (at == bytes.size())
        throw std::runtime_error("Unterminated C64 credits stream");
    PictureArchive result;
    constexpr std::string_view labels[]{"PLAY GAME", "DEMO"};
    for (unsigned selection = 0; selection < 2; ++selection) {
        for (unsigned option = 0; option < 2; ++option)
            for (unsigned i = 0; i < labels[option].size(); ++i) {
                glyphs[960 + 10 * option + i] = labels[option][i] & 63;
                inks[960 + 10 * option + i] = selection == option ? 1 : 5;
            }
        auto frame = image(0, selection, 320, 200);
        for (unsigned i = 0; i < 1000; ++i)
            cell(frame, i % 40 * 8, i / 40 * 8, font.data() + 2 + glyphs[i] * 8,
                 inks[i], 0, 0, 0, false, false);
        result.frames.push_back(std::move(frame));
    }
    return result;
}

PictureArchive wilderness(const fs::path& path, const Bytes& bytes, unsigned id)
{
    constexpr unsigned width = 18, height = 36, cells = width * height, tile_count = 120;
    constexpr unsigned expected = 2 + cells + tile_count * 18;
    if ((bytes.size() != expected && bytes.size() != expected + 8) ||
        (word(bytes, 0) != 0x8c00 && word(bytes, 0) != 0x1388) ||
        !std::all_of(bytes.begin() + expected, bytes.end(), [](auto pad) { return pad == 255; }))
        throw std::runtime_error("Invalid C64 wilderness data");
    const auto setup = companion(path, "SQRPACI00.PRG");
    require(setup, 642, 0x1388);
    if (word(setup, 0x204) != 0x8c00 || word(setup, 0x202) != 0x8c00 + cells ||
        setup[0x214] != width - 1 || setup[0x215] != height - 1)
        throw std::runtime_error("Unrecognized C64 wilderness configuration");
    const auto font = companion(path, "CHARSET.PRG");
    require(font, 514, 0x1000);
    const auto secondary = companion(path, "SECSET" + upper(path.stem().string()).substr(7) + ".PRG");
    require(secondary, 1538, 0x1588);
    PictureArchive result;
    result.frames.push_back(image(id, 0, width * 24, height * 24));
    for (unsigned tile = 0; tile < tile_count; ++tile) {
        auto frame = image(id, tile + 1, 24, 24);
        const auto* data = bytes.data() + 2 + cells + tile * 18;
        for (unsigned i = 0; i < 9; ++i) {
            const auto code = data[i], ink = data[9 + i];
            const auto* glyph = code < 64 ? font.data() + 2 + code * 8 :
                                           secondary.data() + 2 + (code - 64) * 8;
            cell(frame, i % 3 * 8, i / 3 * 8, glyph, ink, 0, 15, 5, ink & 8, false);
        }
        result.frames.push_back(std::move(frame));
    }
    auto& map = result.frames[0];
    for (unsigned i = 0; i < cells; ++i) {
        const unsigned tile = setup[0x182 + (bytes[2 + i] & 127)];
        if (tile >= tile_count)
            throw std::runtime_error("C64 wilderness tile index outside its bank");
        const auto& frame = result.frames[tile + 1];
        for (unsigned y = 0; y < 24; ++y)
            std::copy_n(frame.rgba.begin() + y * 96, 96, map.rgba.begin() +
                        ((i / width * 24 + y) * map.width + i % width * 24) * 4);
    }
    return result;
}

PictureArchive wall_pictures(const fs::path& path, const Bytes& bytes, unsigned id, bool combined)
{
    const bool later = !combined && relocated(word(bytes, 0));
    unsigned size = combined ? 2340 : 780;
    Bytes expanded;
    if (combined)
        require(bytes, 4682, 0x23d0);
    else if (!later && (bytes.size() < 784 || word(bytes, 0) != 0x1388))
        throw std::runtime_error("Invalid C64 wall definition header");
    const auto* definitions = bytes.data() + 2;
    Bytes colors;
    if (later) {
        std::size_t pos = 2;
        auto plane = [&] {
            Bytes out;
            while (pos < bytes.size()) {
                const auto code = bytes[pos++];
                if (!code)
                    return out;
                const bool repeat = code >= 128;
                const unsigned count = repeat ? (code == 128 ? 128 : code & 127) : code;
                const unsigned stored = repeat ? 1 : count;
                if (stored > bytes.size() - pos || count > 2340 - out.size())
                    throw std::runtime_error("Invalid later C64 wall run");
                if (repeat)
                    out.insert(out.end(), count, bytes[pos]);
                else
                    out.insert(out.end(), bytes.begin() + pos, bytes.begin() + pos + count);
                pos += stored;
            }
            throw std::runtime_error("Missing C64 wall plane terminator");
        };
        expanded = plane();
        colors = plane();
        if ((expanded.size() != 780 && expanded.size() != 2340) ||
            colors.size() != expanded.size() || pos != bytes.size())
            throw std::runtime_error("Invalid later C64 wall planes");
        size = static_cast<unsigned>(expanded.size());
        definitions = expanded.data();
    } else if (combined) {
        colors.assign(bytes.begin() + 2 + size, bytes.end());
    } else {
        std::size_t at = 2 + size;
        while (at < bytes.size() && bytes[at] != 255) {
            const auto code = bytes[at++];
            const unsigned count = code < 128 ? 1 : code == 128 ? 256 : code & 127;
            if (count > size - colors.size() || (code >= 128 && at == bytes.size()))
                throw std::runtime_error("Invalid C64 wall color run");
            colors.insert(colors.end(), count, code < 128 ? code : bytes[at++]);
        }
        if (colors.size() != size || at + 1 != bytes.size())
            throw std::runtime_error("Incomplete C64 wall color plane");
    }
    std::array<std::uint8_t, 2048> glyphs{};
    std::array<bool, 256> loaded{};
    auto load = [&](const Bytes& file, unsigned bank, unsigned count) {
        std::copy_n(file.begin() + 2, count * 8, glyphs.begin() + bank * 8);
        std::fill_n(loaded.begin() + bank, count, true);
    };
    const auto basic = companion(path, "CHARSET.PRG");
    if (basic.size() == 514 && (word(basic, 0) == 0x1000 || relocated(word(basic, 0))))
        load(basic, 0, 64);
    const auto secondary = companion(path, combined ? "SECSET00.PRG" : "SECSET02.PRG");
    const unsigned shared = combined ? 192 : 42;
    if (secondary.size() == shared * 8 + 2 &&
        (word(secondary, 0) == (combined ? 0x3a00 : 0x1388) || relocated(word(secondary, 0))))
        load(secondary, 64, shared);
    loaded[32] = true;
    if (!combined) {
        const auto name = upper(path.stem().string());
        auto matching = companion(path, "WALLSET" + name.substr(7) + ".PRG");
        unsigned placed = 0;
        if (later && matching.size() == 1538) {
            load(matching, 64, 192);
        } else if (later && matching.size() == 1202) {
            load(matching, 106, 150);
        } else if (matching.size() == 402) {
            const auto address = word(matching, 0);
            if (relocated(address) || address == 0x3b50 || address == 0x3ce0 || address == 0x3e70) {
                placed = relocated(address) || std::all_of(definitions, definitions + size, [](auto code) { return code < 156; })
                    ? 106 : (address - 0x3800) / 8;
                load(matching, placed, 50);
            }
        }
        for (unsigned slot : {106u,156u,206u}) {
            if (slot == placed)
                continue;
            Bytes candidate;
            bool ambiguous = false;
            for (const auto& entry : fs::directory_iterator(fs::absolute(path).parent_path())) {
                const auto name = upper(entry.path().filename().string());
                if (name.size() != 13 || name.compare(0, 7, "WALLSET") != 0 ||
                    name.substr(9) != ".PRG" || !entry.is_regular_file())
                    continue;
                auto file = read_prg(entry.path());
                if (file.size() != 402 || word(file, 0) != 0x3800 + slot * 8)
                    continue;
                if (!candidate.empty() && candidate != file)
                    ambiguous = true;
                candidate = std::move(file);
            }
            if (!candidate.empty() && !ambiguous)
                load(candidate, slot, 50);
        }
    }
    constexpr unsigned offsets[]{0,2,6,10,22,38,54,110,132,154};
    constexpr unsigned widths[]{1,1,1,3,2,2,7,2,2,1};
    constexpr unsigned heights[]{2,4,4,4,8,8,8,11,11,2};
    PictureArchive result;
    for (unsigned wall = 0; wall < size / 156; ++wall) {
        for (unsigned view = 0; view < std::size(offsets); ++view) {
            auto frame = image(id, wall * 10 + view, widths[view] * 8, heights[view] * 8);
            for (unsigned y = 0; y < heights[view]; ++y)
                for (unsigned x = 0; x < widths[view]; ++x) {
                    const auto at = wall * 156 + offsets[view] + y * widths[view] + x;
                    const auto code = definitions[at];
                    if (!code)
                        continue;
                    const unsigned ink = colors[at] & 240 ? 14 : colors[at] & 15;
                    if (loaded[code]) {
                        cell(frame, x * 8, y * 8, glyphs.data() + code * 8, ink, 0, 9, 15, ink & 8, false);
                    } else {
                        for (unsigned dy = 0; dy < 8; ++dy)
                            for (unsigned dx = 0; dx < 8; ++dx)
                                pixel(frame, x * 8 + dx, y * 8 + dy, {255,0,255,255});
                    }
                }
            result.frames.push_back(std::move(frame));
        }
    }
    return result;
}

PictureArchive scene_picture(const fs::path& path, const Bytes& bytes, unsigned id)
{
    const auto font = companion(path, "CHARSET.PRG");
    require_shared(font, 514, 0x1000);
    const bool full = bytes.size() >= 2681;
    const unsigned columns = full ? 38 : 22, rows = full ? 15 : bytes.size() == 1778 ? 12 : 22;
    const unsigned cells = columns * rows, glyph_start = 2 + 2 * cells;
    const unsigned colors = glyph_start + 1536;
    const unsigned glyph_bytes = full ? 1536 : bytes.size() == 1778 ? 1248 : 512;
    const auto folder = upper(fs::absolute(path).parent_path().parent_path().filename().string());
    const bool shifted = full && (id == 0x71 || id == 0x72) &&
        (folder.find("BUCK_ROGERS") != std::string::npos || folder.find("BUCK ROGERS") != std::string::npos);
    std::array<std::uint8_t, 2048> glyphs{};
    std::copy(font.begin() + 2, font.end(), glyphs.begin());
    std::copy_n(bytes.begin() + glyph_start, glyph_bytes, glyphs.begin() + 512 - shifted);
    auto frame = image(id, 0, columns * 8, rows * 8);
    for (unsigned i = 0; i < cells; ++i) {
        const unsigned code = bytes[2 + i], ink = bytes[2 + cells + i] & 15;
        cell(frame, i % columns * 8, i / columns * 8, glyphs.data() + code * 8, ink,
             full ? bytes[colors] : 0, full ? bytes[colors + 1] : 12,
             full ? bytes[colors + 2] : 15, ink & 8, false);
    }
    PictureArchive result;
    result.frames.push_back(std::move(frame));
    return result;
}

struct SpellStyle {
    unsigned first;
    unsigned end; // Exclusive shape limit. Bit 7 increments VIC sprite colour.
    unsigned flags;
    unsigned shared;
};

void spell_sprite(Frame& frame, unsigned x, const std::uint8_t* bits,
                  const SpellStyle& style, unsigned phase = 0)
{
    sprite(frame, x, 0, bits, ((style.flags + phase) & 15) | (style.flags & 64 ? 128 : 0),
           style.shared & 15, style.shared >> 4);
}

void spell_flight(PictureArchive& archive, unsigned id, const std::uint8_t* sprites,
                  const SpellStyle& style)
{
    // A fixed one-cell horizontal sample of the original fixed-point flight.
    // The native accumulator starts at $80 and adds $C0 for a 24-pixel leg.
    PictureArchive::Clip clip{id, {}};
    clip.realtime = true;
    unsigned shape = style.first, color_phase = 0;
    unsigned cycles = 0, previous_ms = 0;
    for (unsigned step = 1; step <= 32; ++step) {
        const unsigned x = (128 + step * 192) / 256;
        const unsigned previous_x = (128 + (step - 1) * 192) / 256;
        if (style.end == 128) {
            ++color_phase;
        } else if (style.end && ++shape >= (style.end & 127)) {
            shape = style.first;
        }
        auto frame = image(id, step - 1, 48, 21);
        spell_sprite(frame, x, sprites + shape * 64, style, color_phase);
        // PAL model calibrated against the original combat wait and copy loops.
        cycles += 2822 + (x != previous_x ? 15 : 0) + (step == 32 ? 46 : 28) +
            (style.end == 128 ? 11 : style.end ? 325 : 0);
        const unsigned ms = (cycles * 1000 + 492624) / 985248;
        clip.steps.push_back({archive.frames.size(), static_cast<int>(ms - previous_ms)});
        archive.frames.push_back(std::move(frame));
        previous_ms = ms;
    }
    archive.playback_only_frames += clip.steps.size();
    archive.clips.push_back(std::move(clip));
}

PictureArchive later_spell_art(const Bytes& bytes)
{
    unsigned tile_end, sprite_start;
    const bool buck = bytes.size() == 3198;
    switch (bytes.size()) {
    case 1994: tile_end = sprite_start = 394; break;
    case 2674: tile_end = sprite_start = 430; break;
    case 2746: tile_end = sprite_start = 412; break;
    case 3198: tile_end = 430; sprite_start = 736; break;
    default: throw std::runtime_error("Unsupported C64 spell artwork bank");
    }
    constexpr std::array<std::uint8_t, 8> blank{};
    PictureArchive result;
    for (unsigned offset = 250; offset < tile_end; offset += 18) {
        auto frame = image(0, (offset - 250) / 18, 24, 24);
        for (unsigned i = 0; i < 9; ++i) {
            const unsigned code = bytes[offset + i], ink = bytes[offset + 9 + i] & 15;
            if (code != 32 && (code < 64 || code > 94))
                throw std::runtime_error("Invalid C64 spell tile glyph");
            const auto* glyph = code == 32 ? blank.data() : bytes.data() + 2 + (code - 64) * 8;
            cell(frame, i % 3 * 8, i / 3 * 8, glyph, ink, 11, 10, 0, ink & 8, false);
        }
        result.frames.push_back(std::move(frame));
    }
    // The five fantasy games share the resident ECL64/CODE03 selectors and
    // the VIC pairs in COMBAT2 ($E000 + file offset - 2).
    constexpr SpellStyle fantasy[]{
        {0,0,0x48,0x07}, {0,0,0x43,0x1e}, {3,6,0x41,0x3e}, {6,0x8a,0x03,0},
        {10,0x80,0,0}, {13,0,1,0}, {16,19,0x41,0x1b}, {16,19,0x45,0xb5},
        {19,0,0x40,0x98}, {22,24,0x41,0xd5}, {22,24,0x4b,0xcf}
    };
    // Buck Rogers ECL64 $82B9/$82C4 (shapes) and $9292 (VIC pairs).
    constexpr SpellStyle buck_styles[]{
        {0,0,0x4a,0x47}, {3,0,0x4d,0x33}, {6,0,0x40,0x1a}, {9,0,0x4d,0x6c},
        {12,14,0x4a,0x47}, {14,16,0x41,0x7a}, {16,18,0x0f,0},
        {18,0,0x4a,0x7f}, {21,23,0x41,0xcf}, {23,25,0x40,0x6e}, {25,27,1,0}
    };
    const auto* styles = buck ? buck_styles : fantasy;
    const unsigned style_count = buck ? std::size(buck_styles) : std::size(fantasy);
    const auto* sprites = bytes.data() + sprite_start;
    constexpr unsigned fantasy_counts[]{3,3,3,4,3,3,3,3,1,2,2};
    for (unsigned style = 0; style < style_count; ++style) {
        const unsigned first = styles[style].first;
        const unsigned count = !buck ? fantasy_counts[style] :
            (style + 1 < style_count ? styles[style + 1].first : 27) - first;
        for (unsigned shape = first; shape < first + count; ++shape) {
            auto frame = image(style + 1, shape, 24, 21);
            spell_sprite(frame, 0, sprites + 64 * shape, styles[style]);
            result.frames.push_back(std::move(frame));
        }
    }
    if (!buck) {
        auto skull = image(style_count + 1, 0, 24, 21);
        sprite(skull, 0, 0, sprites + 21 * 64, 0, 0, 0);
        sprite(skull, 0, 0, sprites + 20 * 64, 2, 0, 0);
        result.frames.push_back(std::move(skull));
    }
    for (unsigned style = 0; style < style_count; ++style)
        spell_flight(result, style + 1, sprites, styles[style]);
    return result;
}

PictureArchive combat_terrain(const Bytes& bytes)
{
    if ((bytes.size() != 3064 && bytes.size() != 3798) || word(bytes, 0) != 0x1388)
        throw std::runtime_error("Invalid C64 combat terrain bank");
    PictureArchive result;
    constexpr std::array<std::uint8_t, 8> blank{};
    for (unsigned bank = 0; bank < (bytes.size() == 3798 ? 2u : 1u); ++bank) {
        const unsigned table = bank ? 3064 : 250, glyphs = bank ? 3550 : 2, count = bank ? 26 : 7;
        for (unsigned tile = 0; tile < count; ++tile) {
            auto frame = image(bank, tile, 24, 24);
            for (unsigned i = 0; i < 9; ++i) {
                const auto code = bytes[table + tile * 18 + i];
                if (code != 32 && (code < 64 || code > 93 + bank))
                    throw std::runtime_error("Invalid C64 combat terrain glyph");
                const auto* glyph = code == 32 ? blank.data() : bytes.data() + glyphs + (code - 64) * 8;
                const auto ink = bytes[table + tile * 18 + 9 + i] & 15;
                cell(frame, i % 3 * 8, i / 3 * 8, glyph, ink, 11, 10, 0, ink & 8, false);
            }
            result.frames.push_back(std::move(frame));
        }
    }
    const auto first = result.frames.size();
    for (unsigned style = 0; style < 9; ++style) {
        const auto flags = bytes[1766 + style * 2], shared = bytes[1767 + style * 2];
        for (unsigned shape = 0; shape < 20; ++shape) {
            auto frame = image(style + 2, shape, 24, 24);
            sprite(frame, 0, 0, bytes.data() + 1784 + shape * 64,
                   (flags & 15) | ((flags & 64) ? 128 : 0), shared & 15, shared >> 4);
            result.frames.push_back(std::move(frame));
        }
    }
    auto skull = image(11, 0, 24, 24);
    sprite(skull, 0, 0, bytes.data() + 440, 0, 0, 0);
    sprite(skull, 0, 0, bytes.data() + 376, 2, 0, 0);
    result.frames.push_back(std::move(skull));
    PictureArchive::Clip burst{3, {}};
    for (unsigned shape : {5u, 4u, 3u})
        burst.steps.push_back({first + 20 + shape, 145});
    result.clips.push_back(std::move(burst));
    const auto* sprites = bytes.data() + 1784;
    for (const auto& [style, shape, end] : {std::array{0u,0u,3u}, std::array{2u,6u,10u},
                                           std::array{3u,10u,128u}})
        spell_flight(result, style + 2, sprites,
                     {shape, end, bytes[1766 + style * 2], bytes[1767 + style * 2]});
    return result;
}

PictureArchive character_picker(const fs::path& path, const Bytes& bank)
{
    require_shared(bank, 1884, 0x8800);
    const auto glyphs = companion(path, "CHARPIC00.PRG");
    require_shared(glyphs, 2032, 0x8000);
    auto add_part = [&](std::array<unsigned, 9>& codes, unsigned address, unsigned id, unsigned pose, bool head) {
        const auto at = address - 0xa700 + 2 + id + pose * 40;
        const auto first = bank[at + 80];
        unsigned skip = bank[at + 160];
        std::size_t next = 0xabc0 - 0xa700 + 2;
        if (first >= codes.size())
            throw std::runtime_error("Invalid first C64 character cell");
        while (skip && next < bank.size())
            if (!bank[next++])
                --skip;
        auto put = [&](unsigned position, unsigned code) {
            const auto kind = bank[2 + code] & 127, previous = bank[2 + codes[position]] & 127;
            if (!(head && (kind == 2 || kind == 3) && codes[position] != 32 &&
                  previous != 2 && previous != 3 && position < 2))
                codes[position] = code;
        };
        put(first, bank[at]);
        unsigned position = 0;
        while (next < bank.size()) {
            const auto code = bank[next++];
            if (!code)
                return;
            if (position == first)
                ++position;
            if (position >= codes.size())
                throw std::runtime_error("Invalid C64 character cell sequence");
            put(position++, code);
        }
        throw std::runtime_error("Unterminated C64 character cell sequence");
    };
    PictureArchive result;
    for (unsigned tall = 0; tall < 2; ++tall) {
        for (unsigned weapon = 0; weapon < 2; ++weapon) {
            const unsigned count = weapon ? (tall ? 35 : 28) : (tall ? 23 : 14);
            for (unsigned id = 0; id < count; ++id) {
                const unsigned group = 2 * tall + weapon;
                PictureArchive::Clip clip{group, {}};
                for (unsigned pose = 0; pose < 2; ++pose) {
                    std::array<unsigned, 9> codes;
                    codes.fill(32);
                    add_part(codes, tall ? 0xa800 : 0xa9e0, weapon ? id : 0, pose, false);
                    add_part(codes, tall ? 0xa8f0 : 0xaad0, weapon ? 0 : id, pose, true);
                    auto frame = image(group, id * 2 + pose, 24, 24);
                    for (unsigned i = 0; i < codes.size(); ++i) {
                        const auto code = codes[i], flag = unsigned(bank[2 + code]), kind = flag & 127;
                        if (2 + code * 8 + 8 > glyphs.size())
                            throw std::runtime_error("C64 character glyph outside charset");
                        const unsigned ink = (kind < 7 ? kind + 1 : 0) | ((flag & 128) >> 4);
                        cell(frame, i % 3 * 8, i / 3 * 8, glyphs.data() + 2 + code * 8,
                             ink, 11, 10, 0, ink & 8, true);
                    }
                    clip.steps.push_back({result.frames.size(), 200});
                    result.frames.push_back(std::move(frame));
                }
                result.clips.push_back(std::move(clip));
            }
        }
    }
    return result;
}

PictureArchive combat_cursor(const Bytes& bytes)
{
    constexpr unsigned code = 2 + 0x149b - 0x800, paint = 2 + 0x14c1 - 0x800,
                       outline = 2 + 0x14ef - 0x800;
    if (bytes.size() != 4118 || bytes[code] != 0xa2 || bytes[code + 1] != 0x3e ||
        bytes[paint] != 0xa9 || bytes[paint + 1] != 0x80)
        throw std::runtime_error("Unsupported C64 cursor constructor");
    PictureArchive result;
    auto cursor = image(0, 0, 24, 24);
    auto sample = image(0, 1, 88, 88);
    for (unsigned y = 0; y < 88; ++y)
        for (unsigned x = 0; x < 88; ++x)
            pixel(sample, x, y, palette[11]);
    for (unsigned y = 0; y < 24; ++y)
        for (unsigned x = 0; x < 24; ++x)
            if (y < 2 || y >= 22 || (bytes[outline + x / 8] & (128 >> (x % 8)))) {
                pixel(cursor, x, y, palette[0]);
                pixel(sample, x + 32, y + 32, palette[0]);
            }
    result.frames.push_back(std::move(cursor));
    result.frames.push_back(std::move(sample));
    return result;
}

PictureArchive ending(const fs::path& path, const Bytes& bytes)
{
    require(bytes, 1381, 0x1000);
    const auto map = companion(path, "SPELLE20.PRG"), lair = companion(path, "SECSET08.PRG"),
               party = companion(path, "SECSET07.PRG"), spirit = companion(path, "PIC3A.PRG");
    require(map, 902, 0x1388); require(lair, 506, 0x3a08);
    require(party, 506, 0x3a00); require(spirit, 706, 0x0800);
    std::array<Bytes, 3> banks{
        companion(path, "PIC34.PRG"), companion(path, "PIC35.PRG"), companion(path, "PIC36.PRG")};
    PictureArchive result;
    PictureArchive::Clip clip{0, {}, true};
    std::map<std::pair<unsigned,unsigned>, PictureArchive> photos;
    std::map<std::array<unsigned,11>, std::size_t> scenes;
    unsigned scene = 0, frame = 0, pool = 7, body = 12, one = 11, two = 1, eye = 7, drop = 0;
    bool alternate = false, visible = false, show_picture = false;
    auto emit = [&](unsigned cycles, bool use_ticks) {
        const std::array<unsigned,11> state{scene,frame,pool,body,one,two,eye,drop,alternate,visible,show_picture};
        const auto key = std::make_pair(scene, scene ? pool : 0);
        auto it = photos.find(key);
        if (it == photos.end())
            it = photos.emplace(key, pictures(banks[scene], scene, scene ? std::optional<unsigned>(pool) :
                                                                         std::nullopt)).first;
        const auto& photo = it->second;
        if (photo.frames.size() != (scene == 0 ? 2 : scene == 1 ? 5 : 3) ||
            photo.frames.at(frame).width != 88 || photo.frames.at(frame).height != 88)
            throw std::runtime_error("Unexpected C64 ending picture layout");
        const auto entry = scenes.try_emplace(state, result.frames.size());
        if (entry.second) {
            auto canvas = image(0, static_cast<unsigned>(result.frames.size()), 320, 200);
            for (std::size_t i = 3; i < canvas.rgba.size(); i += 4)
                canvas.rgba[i] = 255;
            const auto& glyphs = scene ? party : lair;
            const unsigned offset = scene ? 452 : 2, charset = word(glyphs, 0);
            for (unsigned i = 0; i < 225; ++i) {
                const auto id = map[offset + i];
                const unsigned ink = map[offset + 225 + i] & 15;
                const int at = id * 8 + 0x3800 - int(charset) + 2;
                std::array<std::uint8_t,8> solid{};
                const std::uint8_t* bits = solid.data();
                if (id != 32) {
                    if (!scene && id == 64)
                        solid.fill(255);
                    else if (at >= 2 && unsigned(at) + 8 <= glyphs.size())
                        bits = glyphs.data() + at;
                    else
                        throw std::runtime_error("C64 ending glyph outside its bank");
                }
                cell(canvas, 24 + i % 15 * 8, 8 + i / 15 * 8, bits, ink,
                     0, scene ? pool : 7, scene ? 9 : 8, ink & 8, false);
            }
            const unsigned left = 40, top = scene ? 8 : 40;
            if (show_picture) {
                const auto& data = photo.frames[frame].rgba;
                for (unsigned y = 0; y < 88; ++y)
                    std::copy_n(data.begin() + y * 352, 352, canvas.rgba.begin() + ((top + y) * 320 + left) * 4);
            }
            if (show_picture && scene && visible) {
                constexpr int xs[]{0x50,0x44,0x5c,0x36,0x6b,0x50}, ys[]{0x4b,0x48,0x48,0x47,0x47,0x5d};
                for (int part = 5; part >= 0; --part) {
                    const auto* bits = spirit.data() + 2 + (part + (alternate && part ? 5 : 0)) * 64;
                    for (int y = 0; y < 21; ++y)
                        for (int x = 0; x < 24; ++x) {
                            const int px = xs[part] - 48 + x, py = ys[part] - 58 + y + (part ? 0 : int(drop));
                            const auto value = bits[y * 3 + x / 8];
                            const unsigned selector = part ? (value >> (6 - x % 8 / 2 * 2)) & 3 :
                                                             (value >> (7 - x % 8)) & 1;
                            if (selector && px >= 0 && px < 88 && py >= 0 && py < 88)
                                pixel(canvas, left + px, top + py,
                                      palette[part ? selector == 1 ? one : selector == 2 ? body : two : eye]);
                        }
                }
            }
            result.frames.push_back(std::move(canvas));
        }
        const auto ticks = use_ticks ? photo.clips.at(0).steps.at(frame).duration_ms : 0;
        clip.steps.push_back({entry.first->second, std::max(1, ticks + int((cycles * 1000ull + 511363) / 1022727))});
    };
    auto step = [&](unsigned target, unsigned picture, unsigned cycles) {
        scene = target;
        frame = picture;
        show_picture = true;
        if (!frame)
            pool = scene == 0 ? 7 : scene == 1 ? 11 : 14;
        emit(cycles, true);
    };
    emit(0, false);
    for (unsigned i = 0; i < 140; ++i)
        step(0, i % 2, 0);
    const auto dragon = clip.steps[1].frame;
    visible = true;
    step(2, 0, 0);
    unsigned next = 1;
    for (unsigned i = 0; i < 28; ++i) {
        for (unsigned color : {1u,15u,12u,15u}) {
            pool = two = color;
            step(2, next++ % 3, 0);
        }
        alternate = !alternate;
        emit(0, false);
    }
    const auto pool_frame = clip.steps[clip.steps.size() - 2].frame;
    for (unsigned i = 0; i < 5; ++i)
        step(1, i, 128112);
    constexpr unsigned bodies[]{12,12,12,11,0}, ones[]{11,11,11,11,0}, twos[]{15,15,12,11,0};
    for (unsigned i = 0; i < 5; ++i) {
        body = bodies[i]; one = ones[i]; two = twos[i];
        emit(128112 + (i == 4 ? 1306664 : 0), false);
    }
    for (unsigned i = 0; i < 25; ++i) {
        drop += 2;
        emit(1281 * (100 - 4 * i) + 12, false);
        if (i == 0) {
            eye = 2;
            emit(0, false);
        }
    }
    visible = false;
    emit(3919976, false);
    const auto last = clip.steps.back().frame;
    result.frames.insert(result.frames.begin(), {result.frames[dragon], result.frames[pool_frame], result.frames[last]});
    for (auto& entry : clip.steps)
        entry.frame += 3;
    result.playback_only_frames = result.frames.size() - 3;
    result.clips.push_back(std::move(clip));
    return result;
}

const std::regex graphics(R"(^(GEO|HEAD|BODY|PIC|SPRITE|COMPIC|CHARPIC|SECSET|WALLSET|WALLDEF|WALLS|SQRDATA)([0-9A-F]{2})\.PRG$)");
const std::regex records(R"(^(MON|ITEMFILE)([0-9A-F]{2})\.PRG$)");
}

bool is_c64_picture(const fs::path& path)
{
    const auto name = upper(path.filename().string());
    return name == "TITLEPG.PRG" || name == "INIT.PRG" || name == "CHARSET.PRG" ||
           name == "SPR.TP" || name == "SPELLE65.PRG" || name == "SPELLE66.PRG" || name == "SPELLE64.PRG" ||
           name == "COM.PREP.PRG" || name == "SQRPACI00.PRG" || name == "FINAL.PRG" ||
           std::regex_match(name, graphics);
}

bool is_c64_record(const fs::path& path)
{
    const auto name = upper(path.filename().string());
    return name == "ITEMS.PRG" || name == "ITEMNAMES.PRG" || std::regex_match(name, records);
}

std::vector<RecordRow> read_c64_records(const fs::path& path)
{
    const auto name = upper(path.filename().string());
    const auto bytes = read_prg(path);
    auto number = [](RecordRow& row, const char* label, int value) {
        row.fields.emplace_back(label, std::to_string(value));
    };
    auto signed_byte = [](unsigned value) { return value < 128 ? int(value) : int(value) - 256; };
    using Names = std::vector<std::pair<unsigned, std::string>>;
    auto read_names = [](const Bytes& file) {
        if (file.size() < 515)
            throw std::runtime_error("Truncated C64 ITEMNAMES header");
        const auto address = word(file, 0);
        if (address != 0x6f00 && address != 0x1517 && !relocated(address))
            throw std::runtime_error("Invalid C64 ITEMNAMES header");
        const bool later = address != 0x6f00;
        const bool buck = file.size() == 2185 && word(file, 0) == 0x4000;
        Names names(buck ? 239 : 256);
        const unsigned base = buck ? 0xa5de : later ? 0xa000 : 0x7100;
        const auto first = 2 + 2 * names.size();
        for (unsigned id = 0; id < names.size(); ++id)
            names[id].first = unsigned(file[2 + id]) | unsigned(file[2 + names.size() + id]) << 8;
        for (unsigned id = 0; id < names.size(); ++id) {
            const auto pointer = names[id].first;
            if (!pointer)
                continue;
            std::size_t at = pointer - base + first;
            auto& text = names[id].second;
            if (later && at == file.size())
                continue; // Tail entries point just past the last stored string.
            if (pointer < base || at >= file.size())
                throw std::runtime_error("Invalid C64 item name pointer");
            auto end = std::find(file.begin() + at, file.end(), 0) - file.begin();
            if (end == static_cast<std::ptrdiff_t>(file.size())) {
                if (!later)
                    throw std::runtime_error("Unterminated C64 item name");
                // Some ports append a table of loader filenames with no NUL separators.
                for (const auto& other : names)
                    if (other.first > pointer)
                        end = std::min(end, static_cast<std::ptrdiff_t>(other.first - base + first));
            }
            while (at < static_cast<std::size_t>(end)) {
                const auto c = file[at++];
                if (c < 32 || c > 126 || text.size() == 40)
                    throw std::runtime_error("Invalid C64 item name text");
                text += static_cast<char>(c);
            }
        }
        return names;
    };
    std::vector<RecordRow> rows;
    if (name == "ITEMNAMES.PRG") {
        const auto names = read_names(bytes);
        for (unsigned id = 0; id < names.size(); ++id) {
            const auto& [pointer,text] = names[id];
            RecordRow row{id, 0, text.empty() ? "Empty name" : text, {{"Name component", text}}};
            number(row, "Pointer", pointer);
            rows.push_back(std::move(row));
        }
        return rows;
    }
    if (name == "ITEMS.PRG") {
        const bool buck = bytes.size() == 353 && word(bytes, 0) == 0x4000;
        if (!buck && !(bytes.size() == 1282 && word(bytes, 0) == 0x4000))
            require_shared(bytes, 2050, 0x7600);
        constexpr const char* fields[]{"Equipment slot", "Hands", "Large-target damage rolls",
            "Large-target damage dice", "Large-target damage bonus", "Rate of fire ×2", "AC descriptor",
            "Weapon class bits", "Reserved 8", "Small-target damage rolls", "Small-target damage dice",
            "Small-target damage bonus", "Range (exclusive)", "Class mask", "Property mask", "Reserved 15"};
        const unsigned count = (bytes.size() - 2) / (buck ? 9 : 16);
        for (unsigned id = 0; id < count; ++id) {
            RecordRow row{id, 0, "Item type " + std::to_string(id), {}};
            for (unsigned i = 0; i < (buck ? 9 : std::size(fields)); ++i) {
                if (buck) {
                    row.fields.emplace_back("Byte " + std::to_string(i), std::to_string(bytes[2 + 9 * id + i]));
                    continue;
                }
                const auto value = bytes[2 + 16 * id + i];
                number(row, fields[i], i == 4 || i == 11 ? signed_byte(value) : int(value));
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }
    std::smatch match;
    if (!std::regex_match(name, match, records) || bytes.size() < 2)
        throw std::runtime_error("Unrecognized C64 record resource");
    const auto id = static_cast<unsigned>(std::stoul(match[2].str(), nullptr, 16));
    const bool monster = match[1] == "MON";
    const auto address = word(bytes, 0);
    const bool compact_buck = monster && bytes.size() == 204 && address == 0x4000;
    const bool long_buck = monster && bytes.size() == 546 && address == 0x4000;
    if (monster) {
        if (!compact_buck && ((bytes.size() != 482 && bytes.size() != 546) ||
            (address != 0x6400 && address != 0 && !relocated(address))))
            throw std::runtime_error("Unsupported C64 monster record");
        RecordRow row{id, 0, {}, {}};
        for (unsigned i = 2; i < 18 && bytes[i]; ++i) {
            auto c = bytes[i];
            if (c >= 0xc1 && c <= 0xda)
                c -= 128;
            if (c < 32 || c > 126)
                throw std::runtime_error("Invalid C64 monster name");
            row.name += static_cast<char>(c);
        }
        if (row.name.empty())
            throw std::runtime_error("Missing C64 monster name");
        if (compact_buck || long_buck) {
            constexpr char hex[] = "0123456789ABCDEF";
            number(row, "Payload bytes", static_cast<int>(bytes.size() - 2));
            for (unsigned i = 2; i < bytes.size(); i += 16) {
                std::string raw;
                for (unsigned j = i; j < bytes.size() && j < i + 16; ++j) {
                    if (!raw.empty())
                        raw += ' ';
                    raw += hex[bytes[j] >> 4];
                    raw += hex[bytes[j] & 15];
                }
                row.fields.emplace_back("Bytes at payload offset " + std::to_string(i - 2), std::move(raw));
            }
            return {std::move(row)};
        }
        constexpr const char* abilities[]{"Strength","Intelligence","Wisdom","Dexterity","Constitution","Charisma","Exceptional strength"};
        for (unsigned i = 0; i < std::size(abilities); ++i)
            number(row, abilities[i], bytes[2 + 0x14 + i]);
        for (const auto& [field, offset] : std::array<std::pair<const char*,unsigned>,5>{{
                {"Age",0x74},{"Maximum HP",0x76},{"XP award",0xf7},{"Current HP",0x119},{"Gold",0xc1}}})
            number(row, field, word(bytes, 2 + offset));
        number(row, "Race", bytes[2+0x72]);
        number(row, "Class", bytes[2+0x73]);
        number(row, "Base THAC0", 60 - bytes[2+0x71]);
        number(row, "Base AC", 60 - bytes[2+0xe1]);
        number(row, "Movement", bytes[2+0x9f]);
        number(row, "Portrait head", bytes[2+0xfe]);
        number(row, "Portrait body", bytes[2+0xff]);
        number(row, "Inventory slots", (bytes.size() - 2 - 0x120) / 16);
        rows.push_back(std::move(row));
    } else if ((address != 0x2710 && address != 0x1710 && !relocated(address)) || (bytes.size() - 2) % 16) {
        throw std::runtime_error("Invalid C64 placed item bank");
    }
    Names names;
    try {
        const auto name_file = companion(path, "ITEMNAMES.PRG");
        if (!name_file.empty())
            names = read_names(name_file);
    } catch (const std::runtime_error&) {
        // Numeric component IDs remain available.
    }
    unsigned slot = 0;
    for (unsigned at = monster ? 2 + 0x120 : 2; at < bytes.size(); at += 16, ++slot) {
        const auto* b = bytes.data() + at;
        if (monster && !b[0])
            continue;
        RecordRow row{id, monster ? slot + 1 : slot, {}, {}};
        auto item_name = [&](bool visible) {
            std::string result;
            for (unsigned i = 0; i < 3; ++i) {
                if (b[3-i] >= names.size())
                    continue;
                const auto& part = names[b[3-i]].second;
                if (part.empty() || (visible && (b[6] & (1 << i))))
                    continue;
                if (!result.empty())
                    result += " ";
                result += part;
            }
            return result;
        };
        row.name = b[0] ? item_name(false) : "Empty slot";
        if (row.name.empty())
            row.name = "Item type " + std::to_string(b[0]);
        if (monster)
            row.fields.emplace_back("Owner", rows.front().name);
        number(row, "Payload offset", at - 2);
        number(row, "File offset", at);
        number(row, "Type", b[0]);
        row.fields.emplace_back("Visible name", item_name(true));
        row.fields.emplace_back("Name component IDs", std::to_string(b[3]) + ", " +
                                std::to_string(b[2]) + ", " + std::to_string(b[1]));
        number(row, "Magic bonus", signed_byte(b[4]));
        number(row, "Save bonus", signed_byte(b[5]));
        number(row, "Readied", bool(b[6] & 128));
        number(row, "Hidden name mask", b[6] & 7);
        number(row, "Curse flags", b[7]);
        number(row, "Weight", word(bytes, at + 8));
        number(row, "Quantity", b[10]);
        number(row, "Value", word(bytes, at + 11));
        number(row, "Charges / scroll spell 1", b[13]);
        number(row, "Effect / scroll spell 2", b[14]);
        number(row, "Power / scroll spell 3", b[15]);
        rows.push_back(std::move(row));
    }
    return rows;
}

PictureArchive read_c64_picture(const fs::path& path)
{
    const auto name = upper(path.filename().string());
    const auto bytes = read_prg(path);
    PictureArchive result;
    if (name == "TITLEPG.PRG") {
        result.frames.push_back(title(path, bytes));
    } else if (name == "INIT.PRG") {
        return credits(path, bytes);
    } else if (name == "FINAL.PRG") {
        return ending(path, bytes);
    } else if (name == "SQRPACI00.PRG") {
        std::optional<fs::path> region;
        for (unsigned id = 4; id <= 6; ++id) {
            if (const auto file = find_file(fs::absolute(path).parent_path(),
                                            "SQRDATA0" + std::to_string(id) + ".PRG")) {
                if (region)
                    throw std::runtime_error("Select a specific SQRDATA region");
                region = file;
            }
        }
        if (!region)
            throw std::runtime_error("Missing C64 SQRDATA companion");
        return read_c64_picture(*region);
    } else if (name == "SPELLE65.PRG") {
        constexpr std::array<std::uint8_t, 7> overlay{0xad, 0, 0x7d, 0x29, 7, 0xc9, 1};
        if (bytes.size() >= 2 + overlay.size() &&
            std::equal(overlay.begin(), overlay.end(), bytes.begin() + 2))
            return later_spell_art(companion(path, "SPELLE66.PRG"));
        return combat_terrain(bytes);
    } else if (name == "SPELLE66.PRG") {
        return later_spell_art(bytes);
    } else if (name == "SPELLE64.PRG") {
        return character_picker(path, bytes);
    } else if (name == "COM.PREP.PRG") {
        return combat_cursor(bytes);
    } else if (name == "CHARSET.PRG") {
        require_shared(bytes, 514, 0x1000);
        result.frames.push_back(glyph_atlas(bytes, 0, false));
    } else if (name == "SPR.TP") {
        require_shared(bytes, 450, 0x0800);
        for (unsigned i = 0; i < 7; ++i) {
            auto frame = image(0, i, 24, 21);
            sprite(frame, 0, 0, bytes.data() + 2 + 64 * i, i < 3 ? 2 : 1, 0, 0);
            result.frames.push_back(std::move(frame));
        }
        PictureArchive::Clip clip{0, {}};
        for (unsigned shape : {6u,5u,5u,4u,4u,4u,3u,3u,3u,4u,4u,4u,5u,5u,6u})
            clip.steps.push_back({shape, 50});
        result.clips.push_back(std::move(clip));
    } else {
        std::smatch match;
        if (!std::regex_match(name, match, graphics) || bytes.size() < 2)
            throw std::runtime_error("Unrecognized C64 graphics resource");
        const auto kind = match[1].str();
        const auto id = static_cast<unsigned>(std::stoul(match[2].str(), nullptr, 16));
        if (kind == "PIC" && (relocated(word(bytes, 0)) || word(bytes, 0) == 0x1000) &&
            ((id >= 0x70 && (bytes.size() == 2681 || bytes.size() == 2682)) ||
             (id >= 0x7a && id <= 0x7e && bytes.size() == 1778) ||
             (id == 0x6d && bytes.size() == 1538)))
            return scene_picture(path, bytes, id);
        if (kind == "PIC" && id == 0x3a && bytes.size() == 706 && word(bytes, 0) == 0x0800) {
            for (unsigned n = 0; n < 11; ++n) {
                auto frame = image(id, n, 24, 21);
                sprite(frame, 0, 0, bytes.data() + 2 + 64 * n, n ? 128 + 12 : 7, 11, 1);
                result.frames.push_back(std::move(frame));
            }
            return result;
        }
        if (kind == "HEAD" || kind == "BODY" || kind == "PIC")
            return pictures(bytes, id);
        if (kind == "SPRITE")
            return encounter_sprites(bytes, id);
        if (kind == "COMPIC")
            return combat_pictures(bytes, id);
        if (kind == "WALLDEF" || kind == "WALLS")
            return wall_pictures(path, bytes, id, kind == "WALLS");
        if (kind == "SQRDATA")
            return wilderness(path, bytes, id);
        if (kind == "GEO") {
            result.frames.push_back(map_image(bytes, id));
        } else {
            const auto size = bytes.size() - 2;
            const auto address = word(bytes, 0);
            if (kind == "CHARPIC") {
                if (bytes.size() != 5834 || word(bytes, 0) != 0x4000)
                    require_shared(bytes, 2032, 0x8000);
                if (id)
                    throw std::runtime_error("Invalid C64 character glyph bank");
            } else if (kind == "WALLSET") {
                if (!(relocated(address) && (size == 400 || size == 1200 || size == 1536)) &&
                    (size != 400 || (address != 0x3b50 && address != 0x3ce0 && address != 0x3e70)))
                    throw std::runtime_error("Invalid C64 wall glyph bank");
            } else if (!((relocated(address) && size >= 336 && size <= 1536) ||
                         (address == 0x1388 && size == 336) || (address == 0x1390 && size == 1500) ||
                         (size && size <= 2048 && size % 8 == 0 &&
                          (address == 0x3a00 || address == 0x3a08 || address == 0x1390 ||
                           address == 0x1588 || address == 0x1000)))) {
                throw std::runtime_error("Invalid C64 secondary glyph bank");
            }
            result.frames.push_back(glyph_atlas(bytes, id, kind == "CHARPIC"));
        }
    }
    return result;
}
}
