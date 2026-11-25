#include "tlb.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>

namespace goldbox {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using Color = std::array<std::uint8_t, 4>;
using Palette = std::array<Color, 256>;

std::string upper(std::string value)
{
    for (auto& c : value)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

Bytes read_bytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Unable to open the TLB library");
    Bytes bytes{std::istreambuf_iterator<char>(file), {}};
    if (file.bad())
        throw std::runtime_error("Unable to read the TLB library");
    return bytes;
}

struct Slice {
    const std::uint8_t* data;
    std::size_t size;
    bool big_endian = false;

    Slice part(std::size_t start, std::size_t count) const
    {
        if (start > size || count > size - start)
            throw std::runtime_error("TLB record extends beyond its library");
        return {data + start, count, big_endian};
    }

    unsigned byte(std::size_t offset) const { return *part(offset, 1).data; }

    unsigned word(std::size_t offset) const
    {
        const auto* p = part(offset, 2).data;
        return big_endian ? (unsigned(p[0]) << 8 | p[1]) : (unsigned(p[1]) << 8 | p[0]);
    }

    int signed_word(std::size_t offset) const
    {
        const int value = word(offset);
        return value < 0x8000 ? value : value - 0x10000;
    }

    std::uint32_t dword(std::size_t offset) const
    {
        const auto first = std::uint32_t(word(offset));
        const auto second = std::uint32_t(word(offset + 2));
        return big_endian ? (first << 16 | second) : (second << 16 | first);
    }

    bool tag(std::size_t offset, const char* value) const
    {
        const auto* p = part(offset, 4).data;
        return std::equal(p, p + 4, value);
    }
};

struct Library {
    explicit Library(Slice source) : data(source)
    {
        data.big_endian = data.tag(0, "GLIB");
        if ((!data.big_endian && !data.tag(0, "HLIB")) || data.byte(10) != 0)
            throw std::runtime_error("Unsupported TLB library header");
        const auto size = data.dword(4);
        if (size < 16)
            throw std::runtime_error("Invalid TLB library size");
        data = data.part(0, size);
        const auto count = data.word(8);
        const auto directory_end = 16 + 4 * (count + 1);
        auto previous = directory_end;
        for (unsigned i = 0; i <= count; ++i) {
            const auto offset = data.dword(16 + 4 * i);
            if (offset < previous || offset > data.size)
                throw std::runtime_error("Invalid TLB directory offset");
            offsets.push_back(offset);
            previous = offset;
        }
        ids.resize(count);
        if (data.byte(11) == 1 && count) {
            const auto table = item(0);
            const auto entries = table.word(0);
            table.part(2, 4 * entries);
            for (unsigned i = 0; i < entries; ++i) {
                const auto index = table.word(4 + 4 * i);
                const auto id = table.word(2 + 4 * i);
                directory.emplace_back(id, index);
                if (index < ids.size() && !ids[index])
                    ids[index] = id;
            }
        }
    }

    Slice item(std::size_t index) const
    {
        return data.part(offsets.at(index), offsets.at(index + 1) - offsets[index]);
    }

    unsigned first_item() const { return data.byte(11) == 1 ? 1 : 0; }

    Slice data;
    std::vector<std::uint32_t> offsets;
    std::vector<std::optional<unsigned>> ids;
    std::vector<std::pair<unsigned, unsigned>> directory;
};

Palette default_palette()
{
    constexpr Color ega[] = {
        {0,0,0,255}, {0,0,171,255}, {0,171,0,255}, {0,171,171,255},
        {171,0,0,255}, {171,0,171,255}, {172,87,0,255}, {171,171,171,255},
        {87,87,87,255}, {87,87,255,255}, {87,255,87,255}, {87,255,255,255},
        {255,87,87,255}, {255,87,255,255}, {255,255,87,255}, {255,255,255,255}
    };
    Palette result{};
    std::copy(std::begin(ega), std::end(ega), result.begin());
    result[255] = {103, 247, 159, 255};
    return result;
}

void apply_palette(Slice record, Palette& palette)
{
    const auto first = record.word(2);
    const auto count = record.word(4);
    if (first > palette.size() || count > palette.size() - first)
        throw std::runtime_error("Invalid TLB palette range");
    const auto* rgb = record.part(8, 3 * count).data;
    for (unsigned i = 0; i < count; ++i)
        palette[first + i] = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2], 255};
}

std::optional<fs::path> combat_palette_path(const fs::path& archive)
{
    const auto folder = fs::absolute(archive).parent_path();
    for (const auto& directory : {folder, folder / "DEFAULT.DSN", folder / "DISK2",
                                  folder.parent_path() / "DISK2"}) {
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            if (upper(it->path().filename().string()) == "DUNGCOM.TLB" && it->is_regular_file(error))
                return it->path();
        }
    }
    return {};
}

std::optional<fs::path> find_resource(const fs::path& folder, const char* name, bool directory = false)
{
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (upper(it->path().filename().string()) == name &&
            (directory ? it->is_directory(entry_error) : it->is_regular_file(entry_error)))
            return it->path();
    }
    return {};
}

std::optional<fs::path> map_companion(const fs::path& folder, const char* disk, const char* resource)
{
    if (const auto local = find_resource(folder, resource))
        return local;
    const auto name = upper(folder.filename().string());
    const auto root = name == "DISK1" || name == "DISK2" || name == "DISK3"
        ? folder.parent_path() : folder;
    if (root != folder) {
        if (const auto flat = find_resource(root, resource))
            return flat;
    }
    if (const auto directory = find_resource(root, disk, true))
        return find_resource(*directory, resource);
    return {};
}

void load_combat_palette(const fs::path& path, Palette& palette)
{
    const auto bytes = read_bytes(path);
    const Library library({bytes.data(), bytes.size()});
    if (library.data.tag(12, "TILE")) {
        apply_palette(library.item(library.first_item()), palette);
    } else if (library.data.tag(12, "HLIB")) {
        for (std::size_t i = library.first_item(); i < library.ids.size(); ++i) {
            const Library member(library.item(i));
            if (member.data.tag(12, "TILE"))
                apply_palette(member.item(member.first_item()), palette);
        }
    }
}

Frame decode_image(Slice image, unsigned method, unsigned id, unsigned index, const Palette& palette)
{
    const unsigned height = image.word(0);
    const unsigned width = image.byte(6) * 4;
    if (!width || !height || height > 1024)
        throw std::runtime_error("Invalid TLB image dimensions");
    const auto size = width * height;
    const unsigned plane_bytes = width / 4;
    unsigned pos = 8;
    constexpr unsigned transparent = 256;
    std::vector<unsigned short> pixels(size, method == 23 ? transparent : 0);
    if (method == 18) {
        // PackBits traverses four VGA planes for each row.
        unsigned written = 0;
        while (written < size) {
            const auto code = image.byte(pos++);
            const auto count = code < 128 ? code + 1 : 257 - code;
            if (count > size - written)
                throw std::runtime_error("TLB pixel run exceeds image dimensions");
            const bool literal = code < 128;
            const auto* run = image.part(pos, literal ? count : 1).data;
            pos += literal ? count : 1;
            for (unsigned i = 0; i < count; ++i, ++written) {
                const auto in_row = written % width;
                pixels[(written / width) * width + (in_row % plane_bytes) * 4 +
                       in_row / plane_bytes] = run[literal ? i : 0];
            }
        }
    } else if (method == 23) {
        for (unsigned plane = 0; plane < 4; ++plane) {
            for (unsigned y = 0; y < height; ++y) {
                unsigned column = 0;
                for (auto code = image.byte(pos++); code; code = image.byte(pos++)) {
                    const auto count = code < 128 ? code : 256 - code;
                    if (count > plane_bytes - column)
                        throw std::runtime_error("TLB sparse pixel run exceeds row width");
                    if (code < 128) {
                        const auto* run = image.part(pos, count).data;
                        pos += count;
                        for (unsigned i = 0; i < count; ++i)
                            pixels[y * width + (column + i) * 4 + plane] = run[i];
                    }
                    column += count;
                }
            }
        }
    } else {
        const auto* mask = method == 17 ? image.part(pos, size).data : nullptr;
        if (mask)
            pos += size;
        const auto* source = image.part(pos, size).data;
        for (unsigned i = 0; i < size; ++i)
            pixels[(i % (size / 4)) * 4 + i / (size / 4)] = mask && mask[i] == 255 ? 255 : source[i];
    }

    Frame frame{id, index, width, height, Bytes(size * 4)};
    for (unsigned i = 0; i < size; ++i) {
        if (pixels[i] == transparent || ((method == 17 || method == 21) && pixels[i] == 255))
            continue;
        const auto& color = palette[pixels[i]];
        std::copy(color.begin(), color.end(), frame.rgba.data() + 4 * i);
    }
    return frame;
}

void draw_layer(Frame& target, const Frame& layer, int x, int y)
{
    const auto right = std::min(static_cast<int>(layer.width), static_cast<int>(target.width) - x);
    const auto bottom = std::min(static_cast<int>(layer.height), static_cast<int>(target.height) - y);
    for (int row = std::max(0, -y); row < bottom; ++row) {
        for (int col = std::max(0, -x); col < right; ++col) {
            const auto* pixel = layer.rgba.data() + (row * layer.width + col) * 4;
            if (pixel[3])
                std::copy_n(pixel, 4, target.rgba.data() + ((y + row) * target.width + x + col) * 4);
        }
    }
}

struct Component {
    int x;
    int y;
    Frame frame;
};

// Type 25 draws an ordered list of references to other glyphs in the same TILE.
void place_glyph(const Library& library, unsigned item, int x, int y, const Palette& palette,
                 std::vector<Component>& components, unsigned depth)
{
    if (depth > 16 || item >= library.ids.size() || components.size() >= 4096)
        throw std::runtime_error("Invalid TLB composite glyph");
    const auto image = library.item(item);
    x -= image.signed_word(4);
    y -= image.signed_word(2);
    const auto method = image.byte(7);
    switch (method) {
    case 25:
        for (unsigned i = 0, count = 1; i < count; ++i) {
            const auto part = image.part(8 + 6 * i, 6);
            place_glyph(library, part.byte(0) + library.first_item(), x + part.signed_word(4),
                        y + part.signed_word(2), palette, components, depth + 1);
            count = part.byte(1);
        }
        break;
    case 16:
    case 17:
    case 18:
    case 21:
    case 23:
        components.push_back({x, y, decode_image(image, method, 0, 0, palette)});
        break;
    default:
        throw std::runtime_error("Unsupported TLB composite component");
    }
}

Frame compose_glyph(const Library& library, unsigned item, unsigned id, unsigned index,
                    const Palette& palette)
{
    std::vector<Component> components;
    place_glyph(library, item, 0, 0, palette, components, 0);
    int left = components.front().x;
    int top = components.front().y;
    int right = left;
    int bottom = top;
    for (const auto& part : components) {
        left = std::min(left, part.x);
        top = std::min(top, part.y);
        right = std::max(right, part.x + static_cast<int>(part.frame.width));
        bottom = std::max(bottom, part.y + static_cast<int>(part.frame.height));
    }
    const auto width = static_cast<unsigned>(right - left);
    const auto height = static_cast<unsigned>(bottom - top);
    if (width > 1024 || height > 1024)
        throw std::runtime_error("Invalid TLB composite dimensions");
    Frame frame{id, index, width, height, Bytes(width * height * 4)};
    for (const auto& part : components)
        draw_layer(frame, part.frame, part.x - left, part.y - top);
    return frame;
}

void read_tiles(const Library& library, bool external_only, Palette& palette,
                PictureArchive& result, std::optional<unsigned> group = {})
{
    if (!library.data.tag(12, "TILE"))
        throw std::runtime_error("Expected a TILE graphics library");
    const auto palette_index = library.first_item();
    if (palette_index >= library.ids.size())
        return;
    if (!external_only)
        apply_palette(library.item(palette_index), palette);
    const auto first_image = palette_index + (external_only ? 0 : 1);
    for (auto i = first_image; i < library.ids.size(); ++i) {
        const auto image = library.item(i);
        if (!image.size)
            continue;
        const auto index = i - first_image;
        const auto id = group.value_or(library.ids[i].value_or(i));
        const auto method = image.byte(7);
        if (method == 16 || method == 17 || method == 18 || method == 21 || method == 23)
            result.frames.push_back(decode_image(image, method, id, group ? index : 0, palette));
        else if (method == 25)
            result.frames.push_back(compose_glyph(library, i, id, group ? index : 0, palette));
        else if (method && method != 24) // Type 24 is an alternate interface palette.
            result.skipped_ids.push_back(id);
    }
}

bool amiga_palette(Slice record, Palette& palette)
{
    if (record.size < 8 || (record.byte(7) != 8 && record.byte(7) != 232) ||
        (record.size != 56 && record.size != 104))
        return false;
    const auto* rgb = record.part(8, record.size - 8).data;
    for (unsigned i = 0; i < (record.size - 8) / 3; ++i)
        palette[i] = {rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2], 255};
    return true;
}

Frame amiga_tile(Slice record, unsigned id, unsigned index, const Palette& palette)
{
    const auto method = record.byte(7);
    const unsigned stride = record.byte(6);
    const unsigned height = record.word(0);
    if (!stride || !height || height > 1024)
        throw std::runtime_error("Invalid Amiga TILE dimensions");
    const unsigned colors = method >= 224 ? 5 : 4;
    const bool masked = method == 1 || method == 225;
    const auto span = stride * height;
    const auto required = span * (colors + masked);
    Bytes unpacked;
    const std::uint8_t* data;
    if (method == 2 || method == 226) {
        std::size_t at = 8;
        unpacked.reserve(required);
        while (unpacked.size() < required) {
            const auto code = record.byte(at++);
            const auto count = code < 128 ? code + 1 : 257 - code;
            if (count > required - unpacked.size())
                throw std::runtime_error("Amiga TILE run exceeds image dimensions");
            if (code < 128) {
                const auto* source = record.part(at, count).data;
                unpacked.insert(unpacked.end(), source, source + count);
                at += count;
            } else {
                unpacked.insert(unpacked.end(), count, record.byte(at++));
            }
        }
        if (record.size - at > 1)
            throw std::runtime_error("Unexpected data after Amiga TILE runs");
        data = unpacked.data();
    } else {
        if (record.size != 8 + required)
            throw std::runtime_error("Unexpected Amiga planar TILE size");
        data = record.part(8, required).data;
    }
    const auto* mask = masked ? data : nullptr;
    if (mask)
        data += span;
    Frame frame{id, index, stride * 8, height, Bytes(span * 32)};
    for (unsigned offset = 0; offset < span; ++offset) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const unsigned key = 128 >> bit;
            if (mask && (mask[offset] & key))
                continue;
            unsigned color = 0;
            for (unsigned plane = 0; plane < colors; ++plane)
                if (data[plane * span + offset] & key)
                    color |= 1u << plane;
            std::copy(palette[color].begin(), palette[color].end(),
                      frame.rgba.begin() + (offset * 8 + bit) * 4);
        }
    }
    return frame;
}

void append_amiga_tiles(const Library& library, Palette palette, PictureArchive& result,
                        std::optional<unsigned> group = {})
{
    if (!library.data.tag(12, "TILE"))
        throw std::runtime_error("Expected an Amiga TILE library");
    unsigned index = 0;
    for (auto i = library.first_item(); i < library.ids.size(); ++i) {
        const auto tile = library.item(i);
        if (tile.size < 8 || amiga_palette(tile, palette))
            continue;
        const auto id = group.value_or(library.ids[i].value_or(i));
        const auto method = tile.byte(7);
        switch (method) {
        case 0: case 1: case 2: case 224: case 225: case 226:
            try {
                result.frames.push_back(amiga_tile(tile, id, group ? index : 0, palette));
            } catch (const std::runtime_error&) {
                result.skipped_ids.push_back(id);
            }
            break;
        default:
            result.skipped_ids.push_back(id);
            break;
        }
        ++index;
    }
}

PictureArchive read_amiga_tiles(const Library& library, const fs::path& path)
{
    PictureArchive result;
    auto palette = default_palette();
    for (unsigned i = 16; i < 32; ++i)
        palette[i] = palette[i - 16];
    const auto folder = fs::absolute(path).parent_path();
    if (upper(path.filename().string()) != "ALWAYS.TLB") {
        auto companion = map_companion(folder, "DISK1", "ALWAYS.TLB");
        if (!companion)
            companion = find_resource(folder.parent_path(), "ALWAYS.TLB");
        if (companion) {
            try {
                const auto bytes = read_bytes(*companion);
                const Library system({bytes.data(), bytes.size()});
                if (system.data.big_endian && system.ids.size() > system.first_item())
                    amiga_palette(system.item(system.first_item()), palette);
            } catch (const std::runtime_error&) {
                // Embedded palettes still apply when the system palette is compressed.
            }
        }
    }
    if (library.data.tag(12, "TILE")) {
        append_amiga_tiles(library, palette, result);
    } else if (library.data.tag(12, "GLIB") || library.data.tag(12, "HLIB")) {
        for (auto i = library.first_item(); i < library.ids.size(); ++i) {
            const auto id = library.ids[i].value_or(i);
            try {
                append_amiga_tiles(Library(library.item(i)), palette, result, id);
            } catch (const std::runtime_error&) {
                result.skipped_ids.push_back(id);
            }
        }
    } else {
        throw std::runtime_error("Unsupported Amiga GLIB graphics content");
    }
    std::sort(result.skipped_ids.begin(), result.skipped_ids.end());
    result.skipped_ids.erase(std::unique(result.skipped_ids.begin(), result.skipped_ids.end()),
                             result.skipped_ids.end());
    return result;
}

PictureArchive read_amiga_geo(const Library& maps)
{
    PictureArchive result;
    for (const auto& [id, index] : maps.directory) {
        if (!index || index >= maps.ids.size())
            throw std::runtime_error("Invalid Amiga GEO area index");
        const auto area = maps.item(index);
        const bool fixed = area.size == 1024;
        if (!fixed && area.size < 8) {
            result.skipped_ids.push_back(id);
            continue;
        }
        const unsigned rows = fixed ? 16 : area.byte(0);
        const unsigned cols = fixed ? 16 : area.byte(1);
        const unsigned cells = rows * cols;
        const unsigned header = fixed ? 0 : 8;
        if (!cells || cells > 784 || header + 4 * cells > area.size) {
            result.skipped_ids.push_back(id);
            continue;
        }
        constexpr unsigned scale = 12;
        Frame frame{id, 0, cols * scale + 1, rows * scale + 1, {}};
        frame.rgba.resize(frame.width * frame.height * 4);
        auto paint = [&](unsigned x, unsigned y, Color color) {
            std::copy(color.begin(), color.end(), frame.rgba.begin() + (y * frame.width + x) * 4);
        };
        const Color background{24,24,24,255}, wall{235,235,235,255}, event{94,180,130,255};
        for (unsigned y = 0; y < frame.height; ++y)
            for (unsigned x = 0; x < frame.width; ++x)
                paint(x, y, background);
        for (unsigned i = 0; i < cells; ++i) {
            const unsigned x = i % cols * scale, y = i / cols * scale;
            const auto ne = area.byte(header + i), sw = area.byte(header + cells + i);
            for (unsigned dot = 0; dot <= scale; ++dot) {
                if (ne >> 4) paint(x + dot, y, wall);
                if (ne & 15) paint(x + scale, y + dot, wall);
                if (sw >> 4) paint(x + dot, y + scale, wall);
                if (sw & 15) paint(x, y + dot, wall);
            }
            if (area.byte(header + 2 * cells + i) & 127)
                for (unsigned dy = 5; dy < 8; ++dy)
                    for (unsigned dx = 5; dx < 8; ++dx)
                        paint(x + dx, y + dy, event);
        }
        result.frames.push_back(std::move(frame));
    }
    return result;
}

Palette dark_queen_palette(const fs::path& folder)
{
    auto palette = default_palette();
    std::fill(palette.begin() + 16, palette.end(), Color{0, 0, 0, 255});
    if (const auto always = map_companion(folder, "DISK1", "ALWAYS.TLB")) {
        const auto bytes = read_bytes(*always);
        const Library system({bytes.data(), bytes.size()});
        const auto colors = system.item(system.first_item());
        if (!system.data.tag(12, "TILE") || colors.byte(7) != 24)
            throw std::runtime_error("Invalid ALWAYS.TLB system palette");
        apply_palette(colors, palette);
    }
    return palette;
}

Library scene_set(const Library& library, unsigned id)
{
    const auto entry = std::find_if(library.directory.begin(), library.directory.end(),
                                    [id](const auto& item) { return item.first == id; });
    if (entry == library.directory.end() || !entry->second || entry->second >= library.ids.size())
        throw std::runtime_error("Missing Dark Queen TLB scene set");
    Library set(library.item(entry->second));
    if (!set.data.tag(12, "TILE") || set.ids.size() <= set.first_item() + 1)
        throw std::runtime_error("Invalid Dark Queen TLB scene set");
    return set;
}

void draw_scene_glyph(Frame& canvas, const Library& set, unsigned index, const Palette& palette)
{
    if (set.item(index).byte(7) == 24)
        return;
    std::vector<Component> parts;
    place_glyph(set, index, 0, 0, palette, parts, 0);
    for (const auto& part : parts)
        draw_layer(canvas, part.frame, part.x, part.y);
}

PictureArchive read_dark_queen_scenes(const Library& library, const Palette& system, bool title)
{
    PictureArchive result;
    if (title) {
        for (unsigned scene = 1; scene <= 8; ++scene) {
            const auto base = scene_set(library, scene <= 3 ? 1 : 5);
            const auto overlay = scene_set(library, scene + (scene <= 3 ? 1 : 2));
            auto palette = system;
            apply_palette(base.item(base.first_item()), palette);
            apply_palette(overlay.item(overlay.first_item()), palette);
            Frame canvas{scene, 0, 320, 200, Bytes(320 * 200 * 4)};
            draw_scene_glyph(canvas, base, base.first_item() + 1, palette);
            for (unsigned i = overlay.first_item() + 1; i < overlay.ids.size(); ++i)
                draw_scene_glyph(canvas, overlay, i, palette);
            result.frames.push_back(std::move(canvas));
        }
    } else {
        for (unsigned scene : {1u, 3u, 2u}) {
            const auto set = scene_set(library, scene);
            auto palette = system;
            apply_palette(set.item(set.first_item()), palette);
            const auto first = set.first_item() + 1;
            const auto backdrop = set.item(first);
            const auto width = backdrop.byte(6) * 4;
            const auto height = backdrop.word(0);
            if (!width || !height || width > 640 || height > 400)
                throw std::runtime_error("Invalid Dark Queen ending backdrop");
            Frame canvas{0, 0, width, height, Bytes(width * height * 4)};
            draw_scene_glyph(canvas, set, first, palette);
            for (auto i = first; i < set.ids.size(); ++i) {
                auto frame = canvas;
                frame.id = static_cast<unsigned>(result.frames.size() + 1);
                if (i != first)
                    draw_scene_glyph(frame, set, i, palette);
                result.frames.push_back(std::move(frame));
            }
        }
    }
    return result;
}
}

bool is_tlb_archive(const fs::path& path)
{
    return upper(path.extension().string()) == ".TLB";
}

PictureArchive read_tlb(const fs::path& path)
{
    const auto bytes = read_bytes(path);
    const Library library({bytes.data(), bytes.size()});
    if (library.data.big_endian)
        return read_amiga_tiles(library, path);
    auto palette = default_palette();
    const auto name = upper(path.filename().string());
    if ((name == "TITLE.TLB" || name == "FINAL.TLB") && library.data.tag(12, "HLIB")) {
        const auto folder = fs::absolute(path).parent_path();
        const auto disk = upper(folder.filename().string());
        const auto root = disk == "DISK1" || disk == "DISK2" || disk == "DISK3" ? folder.parent_path() : folder;
        if (find_resource(root, "DQK.EXE"))
            return read_dark_queen_scenes(library, dark_queen_palette(folder), name == "TITLE.TLB");
    }
    const bool external_only = name.compare(0, 4, "CPIC") == 0 && name != "CPIC.TLB";
    if (external_only || name == "CBODY.TLB") {
        if (const auto companion = combat_palette_path(path))
            load_combat_palette(*companion, palette);
        else if (external_only)
            throw std::runtime_error("This TLB combat icon requires DUNGCOM.TLB for its palette");
    }
    PictureArchive result;
    if (library.data.tag(12, "TILE")) {
        read_tiles(library, external_only, palette, result);
    } else if (library.data.tag(12, "HLIB")) {
        for (std::size_t i = library.first_item(); i < library.ids.size(); ++i) {
            const Library member(library.item(i));
            read_tiles(member, false, palette, result, library.ids[i].value_or(i));
        }
    } else {
        throw std::runtime_error("Unsupported TLB library content");
    }
    if (result.frames.empty())
        throw std::runtime_error("The TLB library contains no supported graphics");
    return result;
}

bool is_geo_glb(const fs::path& path)
{
    return upper(path.filename().string()) == "GEO.GLB";
}

std::vector<DaxRecord> read_hlib_data(const fs::path& path)
{
    const auto bytes = read_bytes(path);
    const Library library({bytes.data(), bytes.size()});
    if (!library.data.tag(12, "DATA") || library.first_item() != 1 || library.directory.empty())
        throw std::runtime_error("Expected an indexed HLIB DATA library");
    std::vector<DaxRecord> records;
    for (const auto& [id, index] : library.directory) {
        if (!index || index >= library.ids.size())
            throw std::runtime_error("Invalid HLIB DATA entry index");
        const auto item = library.item(index);
        records.push_back({id, static_cast<unsigned>(item.size), Bytes(item.data, item.data + item.size)});
    }
    return records;
}

PictureArchive read_geo_glb(const fs::path& path)
{
    const auto bytes = read_bytes(path);
    const Library maps({bytes.data(), bytes.size()});
    if (!maps.data.tag(12, "DATA") || maps.first_item() != 1 || maps.directory.empty())
        throw std::runtime_error("Expected a GEO.GLB map directory");
    if (maps.data.big_endian)
        return read_amiga_geo(maps);

    const auto folder = fs::absolute(path).parent_path();
    const auto tile_path = map_companion(folder, "DISK2", "TOPVIEW.TLB");
    if (!tile_path)
        throw std::runtime_error("GEO.GLB requires TOPVIEW.TLB in the same folder or on DISK2");
    const auto tile_bytes = read_bytes(*tile_path);
    const Library tile_set({tile_bytes.data(), tile_bytes.size()});
    if (!tile_set.data.tag(12, "TILE") || tile_set.ids.size() < tile_set.first_item() + 17)
        throw std::runtime_error("Invalid TOPVIEW.TLB map tile library");

    const auto palette = dark_queen_palette(folder);

    constexpr unsigned tile_size = 8;
    std::array<Frame, 16> tiles;
    std::array<std::pair<int, int>, 16> offsets;
    for (unsigned i = 0; i < tiles.size(); ++i) {
        const auto glyph = tile_set.item(tile_set.first_item() + 1 + i);
        const auto method = glyph.byte(7);
        if (glyph.word(0) != tile_size || glyph.byte(6) * 4 != tile_size ||
            (method != 16 && method != 17 && method != 18 && method != 21 && method != 23))
            throw std::runtime_error("Invalid TOPVIEW map tile");
        tiles[i] = decode_image(glyph, method, i, 0, palette);
        offsets[i] = {-glyph.signed_word(4), -glyph.signed_word(2)};
    }

    PictureArchive result;
    for (const auto& [id, index] : maps.directory) {
        if (!index || index >= maps.ids.size())
            throw std::runtime_error("Invalid GEO.GLB area index");
        const auto area = maps.item(index);
        const auto rows = area.byte(0);
        const auto cols = area.byte(1);
        const auto cells = rows * cols;
        if (!cells || cells > 0x310 || area.size != 8 + 4 * cells)
            throw std::runtime_error("Invalid GEO.GLB area dimensions");
        const auto* ne = area.data + 8;
        const auto* sw = ne + cells;
        Frame map{id, 0, cols * tile_size, rows * tile_size, Bytes(cells * tile_size * tile_size * 4)};
        for (unsigned cell = 0; cell < cells; ++cell) {
            const auto glyph = (ne[cell] > 15 ? 1 : 0) | ((ne[cell] & 15) ? 2 : 0) |
                               (sw[cell] > 15 ? 4 : 0) | ((sw[cell] & 15) ? 8 : 0);
            const auto [dx, dy] = offsets[glyph];
            draw_layer(map, tiles[glyph], static_cast<int>(cell % cols * tile_size) + dx,
                          static_cast<int>(cell / cols * tile_size) + dy);
        }
        result.frames.push_back(std::move(map));
    }
    return result;
}
}
