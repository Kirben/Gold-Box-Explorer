#include "amiga_pictures.h"
#include "ecl_containers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Palette = std::array<unsigned, 16>;
constexpr Palette portrait{0x000,0xdb9,0xb97,0x964,0x521,0x742,0xa84,0xcb6,0xff9,0x007,0x44b,0x444,0x555,0x777,0xaaa,0xfff};
constexpr Palette scene{0x000,0x06b,0x096,0x0aa,0x000,0x940,0xb44,0xfaa,0xfe9,0x59f,0x0d8,0x5ff,0x555,0x777,0xaaa,0xfff};
constexpr Palette picture{0x000,0x124,0x953,0xb97,0xecb,0x710,0xa42,0xdb0,0xff5,0x074,0x053,0x369,0x555,0x777,0xaaa,0xfff};
constexpr Palette daa_shared{0x000,0x00a,0x0a0,0x0aa,0xa00,0xa0a,0xa50,0xaaa,0x555,0x55f,0x5f5,0x5ff,0xf55,0xf5f,0xff5,0xfff};

unsigned word(const Bytes& bytes, std::size_t at)
{
    if (at > bytes.size() || bytes.size() - at < 2)
        throw std::runtime_error("Truncated Amiga picture word");
    return unsigned(bytes[at]) << 8 | bytes[at + 1];
}

std::uint32_t dword(const Bytes& bytes, std::size_t at)
{
    return std::uint32_t(word(bytes, at)) << 16 | word(bytes, at + 2);
}

struct Image {
    unsigned height, width_bytes, count, plane_bytes, stride;
    std::size_t first, end;
};

Image header(const Bytes& bytes, std::size_t offset)
{
    Image image{word(bytes, offset), word(bytes, offset + 2), word(bytes, offset + 8),
                word(bytes, offset + 10), 0, offset + 12, 0};
    image.stride = (image.width_bytes + 1) & ~1u;
    image.end = image.first + 4 * image.plane_bytes * image.count;
    if (!image.height || image.height > 512 || !image.width_bytes || image.width_bytes > 80 ||
        !image.count || image.count > 255 || image.plane_bytes != image.height * image.stride ||
        image.end > bytes.size())
        throw std::runtime_error("Invalid Amiga planar picture header");
    return image;
}

Frame render(const DaxRecord& record, const Image& image, unsigned index, unsigned frame,
             unsigned piece, unsigned width_bytes, const Image* delta, const Palette& palette,
             bool transparent)
{
    const unsigned width = width_bytes * 8;
    Frame result{record.id, index, width, image.height, Bytes(width * image.height * 4)};
    const auto plane_span = image.plane_bytes * image.count;
    for (unsigned y = 0; y < image.height; ++y) {
        for (unsigned byte = 0; byte < width_bytes; ++byte) {
            const auto column = piece * width_bytes + byte;
            const auto at = image.first + frame * image.plane_bytes + y * image.stride + column;
            std::array<unsigned, 4> planes{};
            for (unsigned plane = 0; plane < planes.size(); ++plane) {
                planes[plane] = record.bytes[at + plane * plane_span];
                if (delta)
                    planes[plane] ^= record.bytes[delta->first + plane * delta->plane_bytes +
                                                  y * delta->stride + column];
            }
            for (unsigned bit = 0; bit < 8; ++bit) {
                unsigned color = 0;
                for (unsigned plane = 0; plane < planes.size(); ++plane)
                    color |= ((planes[plane] >> (7 - bit)) & 1) << plane;
                if (transparent && !color)
                    continue;
                const auto rgb = palette[color];
                auto* pixel = result.rgba.data() + 4 * (y * width + byte * 8 + bit);
                pixel[0] = static_cast<std::uint8_t>(((rgb >> 8) & 15) * 17);
                pixel[1] = static_cast<std::uint8_t>(((rgb >> 4) & 15) * 17);
                pixel[2] = static_cast<std::uint8_t>((rgb & 15) * 17);
                pixel[3] = 255;
            }
        }
    }
    return result;
}

enum class Layout { image, tiles, animation, distance };
struct Format {
    const char* name;
    Layout layout;
    bool transparent;
};

constexpr Format formats[]{
    {"HEAD",Layout::image,false}, {"BODY",Layout::image,false},
    {"CHEAD",Layout::image,true}, {"CBODY",Layout::image,true},
    {"CPIC",Layout::image,true}, {"COMSPR",Layout::image,true}, {"ICON",Layout::image,true},
    {"PIC",Layout::animation,false}, {"FINAL",Layout::animation,false}, {"SPRIT",Layout::distance,true},
    {"8X8D",Layout::tiles,true}, {"BACPAC",Layout::tiles,false}, {"DUNGCOM",Layout::tiles,false},
    {"WILDCOM",Layout::tiles,false}, {"RANDCOM",Layout::tiles,false}, {"SQRPACI",Layout::tiles,false}
};

void append_record(const DaxRecord& record, const Format& format, PictureArchive& archive)
{
    const std::string_view name(format.name);
    const auto& palette = name == "HEAD" || name == "BODY" ? portrait :
        name == "PIC" || (name == "FINAL" && record.id == 2) ? picture : scene;
    if (format.layout == Layout::animation || format.layout == Layout::distance) {
        const auto count = dword(record.bytes, 0);
        if (!count || count > 255)
            throw std::runtime_error("Invalid Amiga picture frame count");
        Image first{};
        std::size_t offset = 4;
        PictureArchive::Clip clip{record.id, {}};
        for (unsigned index = 0; index < count; ++index) {
            const auto delay = dword(record.bytes, offset);
            const auto image = header(record.bytes, offset + 4);
            const auto* delta = index && format.layout == Layout::animation ? &first : nullptr;
            if (image.count != 1 || (delta && (image.width_bytes != delta->width_bytes ||
                                              image.height != delta->height)))
                throw std::runtime_error("Invalid Amiga delta frame");
            if (!index)
                first = image;
            if (name == "PIC" && count > 1) {
                const int ms = record.id == 1 || record.id == 29 ? 200 :
                    static_cast<int>(std::clamp<std::uint64_t>(std::uint64_t(delay) * 100, 20, 10000));
                clip.steps.push_back({archive.frames.size(), ms});
            }
            archive.frames.push_back(render(record, image, index, 0, 0, image.width_bytes,
                                            delta, palette, format.transparent));
            offset = image.end;
        }
        if (offset != record.bytes.size())
            throw std::runtime_error("Trailing Amiga animation data");
        if (!clip.steps.empty())
            archive.clips.push_back(std::move(clip));
        return;
    }
    const auto image = header(record.bytes, 0);
    const unsigned pieces = format.layout == Layout::tiles ? 2 : 1;
    if (image.end != record.bytes.size() || image.width_bytes % pieces)
        throw std::runtime_error("Invalid Amiga picture data size");
    const auto count = image.count * pieces - (name == "8X8D" && (record.id == 202 || record.id == 203));
    for (unsigned index = 0; index < count; ++index)
        archive.frames.push_back(render(record, image, index, index / pieces, index % pieces,
                                        image.width_bytes / pieces, nullptr, palette, format.transparent));
}

void append_amiga_runs(const Bytes& file, std::size_t& begin, std::size_t end,
                       std::size_t limit, bool packbits, bool stop_at_limit, Bytes& bytes)
{
    while (begin < end && (!stop_at_limit || bytes.size() < limit)) {
        const auto command = file[begin++];
        if (packbits && command == 128)
            continue;
        const bool literal = command < (packbits ? 128 : 127);
        const unsigned count = literal ? command + 1 : (packbits ? 257 : 256) - command;
        const unsigned stored = literal ? count : 1;
        if (stored > end - begin || count > limit - bytes.size())
            throw std::runtime_error("Invalid Amiga picture run");
        if (literal)
            bytes.insert(bytes.end(), file.begin() + begin, file.begin() + begin + count);
        else
            bytes.insert(bytes.end(), count, file[begin]);
        begin += stored;
    }
}

Bytes expand_daa(const Bytes& file, std::size_t begin, std::size_t end, std::size_t limit,
                 bool packbits)
{
    Bytes bytes;
    append_amiga_runs(file, begin, end, limit, packbits, false, bytes);
    return bytes;
}

void append_daa(PictureArchive& archive, const Bytes& bytes, unsigned id, const std::string& stem)
{
    const unsigned height = word(bytes, 0), width = word(bytes, 2) * 8;
    const bool tiles = stem.compare(0, 4, "8X8D") == 0 || stem == "SPACETVL";
    const unsigned max_width = tiles && height == 8 ? 1024 : 640;
    if (!width || width > max_width || !height || height > 512 || bytes.size() < 9 || !bytes[8])
        throw std::runtime_error("Invalid Amiga DAA image dimensions");
    unsigned count = bytes[8], image_width = width;
    if (tiles && height == 8) {
        count *= width / 8;
        image_width = 8;
    }
    // The native loader aligns the complete horizontal strip to a 16-bit boundary.
    const unsigned stride = ((count * image_width + 15) / 16) * 2;
    const unsigned plane_size = height * stride;
    const unsigned planes = tiles && width == 8 && bytes.size() < 73 + 5 * plane_size ? 4 : 5;
    const unsigned first = planes == 4 ? 9 : 73;
    if (first + planes * plane_size > bytes.size() || bytes.size() - first - planes * plane_size > 4)
        throw std::runtime_error("Invalid Amiga DAA planar strip size");
    std::array<unsigned, 32> colors{};
    if (planes == 5)
        for (unsigned n = 0; n < colors.size(); ++n)
            colors[n] = word(bytes, 9 + n * 2);
    if (tiles && std::all_of(colors.begin(), colors.end(), [](auto rgb) { return (rgb & 4095) == 0; })) {
        std::copy(daa_shared.begin(), daa_shared.end(), colors.begin());
        std::copy(daa_shared.begin(), daa_shared.end(), colors.begin() + daa_shared.size());
    }
    const bool transparent = tiles || stem.compare(0, 4, "CPIC") == 0 || stem == "COMSPR" ||
        stem.compare(0, 5, "SPRIT") == 0;
    for (unsigned n = 0; n < count; ++n) {
        Frame frame{id, n, image_width, height, Bytes(image_width * height * 4)};
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < image_width; ++x) {
                const auto sx = n * image_width + x;
                const auto source = first + y * stride + sx / 8;
                unsigned ink = 0;
                for (unsigned plane = 0; plane < planes; ++plane)
                    ink |= ((bytes[source + plane * plane_size] >> (7 - sx % 8)) & 1) << plane;
                if (!ink && transparent)
                    continue;
                const auto rgb = colors[ink];
                const auto dest = (y * image_width + x) * 4;
                frame.rgba[dest] = (rgb >> 8 & 15) * 17;
                frame.rgba[dest + 1] = (rgb >> 4 & 15) * 17;
                frame.rgba[dest + 2] = (rgb & 15) * 17;
                frame.rgba[dest + 3] = 255;
            }
        archive.frames.push_back(std::move(frame));
    }
}

void append_daa_animation(PictureArchive& archive, const Bytes& file, std::size_t first,
                          std::size_t end, unsigned id, const std::string& stem)
{
    if (end - first < 2)
        throw std::runtime_error("Truncated Amiga DAA animation");
    const auto packed_header = word(file, first);
    first += 2;
    if (packed_header > end - first)
        throw std::runtime_error("Invalid Amiga DAA animation header size");
    const auto header = expand_daa(file, first, first + packed_header, 256, true);
    first += packed_header;
    const unsigned height = word(header, 0), width_bytes = word(header, 2), count = word(header, 8);
    if (height == 0 || height > 512 || width_bytes == 0 || width_bytes > 80 ||
        count == 0 || 74 + count * 2 > header.size())
        throw std::runtime_error("Invalid Amiga DAA animation header");
    const unsigned plane_bytes = height * ((width_bytes + 1) & ~1u);
    Bytes image(header.begin(), header.begin() + 9);
    image[8] = 1;
    image.insert(image.end(), header.begin() + 10, header.begin() + 74);
    PictureArchive::Clip clip{id, {}};
    const bool encounter = stem.compare(0, 5, "SPRIT") == 0;
    Bytes base;
    for (unsigned n = 0; n < count; ++n) {
        const auto packed = word(header, 74 + n * 2);
        if (end - first < 2 || packed > end - first - 2)
            throw std::runtime_error("Invalid Amiga DAA frame size");
        auto pixels = expand_daa(file, first, first + packed, 5 * plane_bytes, true);
        first += packed;
        if (pixels.size() != 5 * plane_bytes)
            throw std::runtime_error("Truncated Amiga DAA frame");
        if (!n)
            base = pixels;
        else if (!encounter)
            for (std::size_t i = 0; i < pixels.size(); ++i)
                pixels[i] ^= base[i];
        if (!encounter && count > 1)
            clip.steps.push_back({archive.frames.size(), static_cast<int>(std::max(1u, word(file, first)) * 100)});
        first += 2;
        image.resize(73);
        image.insert(image.end(), pixels.begin(), pixels.end());
        append_daa(archive, image, id, stem);
        archive.frames.back().index = n;
    }
    if (!clip.steps.empty())
        archive.clips.push_back(std::move(clip));
}

PictureArchive read_daa(const std::filesystem::path& path, const std::string& stem)
{
    std::ifstream stream(path, std::ios::binary);
    const Bytes file{std::istreambuf_iterator<char>(stream), {}};
    const auto start = word(file, 0);
    const bool animation = stem.compare(0, 3, "PIC") == 0 || stem.compare(0, 5, "SPRIT") == 0;
    const unsigned stride = animation ? 6 : 9;
    if (start <= 2 || start > file.size() || (start - 2) % stride)
        throw std::runtime_error("Unsupported Amiga DAA directory");
    PictureArchive archive;
    for (unsigned entry = 2; entry < start; entry += stride) {
        const unsigned id = file[entry];
        const auto before = archive.frames.size();
        try {
            const auto offset = dword(file, entry + (animation ? 2 : 1));
            if (offset > file.size() - start)
                throw std::runtime_error("Amiga DAA directory entry outside archive");
            if (animation) {
                const auto next = entry + stride < start ? dword(file, entry + stride + 2) : file.size() - start;
                if (next < offset || next > file.size() - start)
                    throw std::runtime_error("Invalid Amiga DAA frame directory");
                append_daa_animation(archive, file, start + offset, start + next, id, stem);
            } else {
                const unsigned raw = word(file, entry + 5), stored = word(file, entry + 7);
                if (stored > file.size() - start - offset)
                    throw std::runtime_error("Amiga DAA directory entry outside archive");
                // Champions includes an unused, zero-filled slot before its tile banks.
                if (id == 201 && stem.compare(0, 4, "8X8D") == 0 &&
                    std::all_of(file.begin() + start + offset, file.begin() + start + offset + stored,
                                [](auto byte) { return byte == 0; }))
                    continue;
                auto bytes = expand_daa(file, start + offset, start + offset + stored, raw, false);
                if (bytes.size() != raw)
                    throw std::runtime_error("Incorrect Amiga DAA expanded size");
                append_daa(archive, bytes, id, stem);
            }
        } catch (const std::runtime_error&) {
            archive.frames.resize(before);
            archive.skipped_ids.push_back(id);
        }
    }
    return archive;
}
}

bool is_amiga_ilbm(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    for (auto& c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension != ".lbm" && extension != ".iff" && extension != ".ilbm")
        return false;
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 12> header{};
    return stream.read(header.data(), header.size()) &&
        std::string_view(header.data(), 4) == "FORM" &&
        std::string_view(header.data() + 8, 4) == "ILBM";
}

PictureArchive read_amiga_ilbm(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    const Bytes file{std::istreambuf_iterator<char>(stream), {}};
    if (file.size() < 12)
        throw std::runtime_error("Truncated Amiga ILBM FORM");
    const auto form_size = dword(file, 4);
    if (form_size < 4 || form_size > file.size() - 8)
        throw std::runtime_error("Invalid Amiga ILBM FORM size");
    const auto form_end = std::size_t(8) + form_size;
    std::size_t header = 0, body = 0, body_end = 0, palette = 0, palette_size = 0;
    std::uint32_t mode = 0;
    for (std::size_t at = 12; at < form_end;) {
        if (form_end - at < 8)
            throw std::runtime_error("Truncated Amiga ILBM chunk header");
        const std::string_view tag(reinterpret_cast<const char*>(file.data() + at), 4);
        const auto size = std::size_t(dword(file, at + 4));
        at += 8;
        if (size > form_end - at || (size & 1) > form_end - at - size)
            throw std::runtime_error("Amiga ILBM chunk extends beyond FORM");
        if (tag == "BMHD" && size >= 20)
            header = at;
        else if (tag == "CMAP" && size % 3 == 0) {
            palette = at;
            palette_size = size;
        } else if (tag == "CAMG" && size == 4)
            mode = dword(file, at);
        else if (tag == "BODY") {
            body = at;
            body_end = at + size;
            break;
        }
        at += size + (size & 1);
    }
    if (!header || !body)
        throw std::runtime_error("Amiga ILBM requires BMHD and BODY chunks");
    const unsigned width = word(file, header), height = word(file, header + 2);
    const unsigned planes = file[header + 8], masking = file[header + 9], compression = file[header + 10];
    if (!width || !height || std::uint64_t(width) * height > 16 * 1024 * 1024 ||
        !planes || planes > 8 || masking > 2 || compression > 1 ||
        ((planes == 6 || planes == 8) && (mode & 0x0800)))
        throw std::runtime_error("Unsupported Amiga ILBM dimensions, masking, compression or display mode");

    const unsigned colors = 1u << planes;
    const bool half_brite = planes == 6 && (mode & 0x0080);
    const unsigned base_colors = half_brite ? colors / 2 : colors;
    // Early Deluxe Paint writes Amiga's 4-bit channels in the high nibble.
    const bool four_bit = palette_size && std::all_of(file.begin() + palette,
        file.begin() + palette + palette_size, [](auto channel) { return (channel & 15) == 0; });
    std::array<std::array<std::uint8_t, 3>, 256> rgb{};
    for (unsigned index = 0; index < base_colors; ++index)
        for (unsigned c = 0; c < 3; ++c) {
            const unsigned offset = index * 3 + c;
            const unsigned channel = offset < palette_size ? file[palette + offset] :
                (palette_size ? 0 : index * 255 / (base_colors - 1));
            rgb[index][c] = static_cast<std::uint8_t>(four_bit ? channel | (channel >> 4) : channel);
            if (half_brite)
                rgb[index + base_colors][c] = static_cast<std::uint8_t>(
                    four_bit ? (channel >> 5) * 17 : channel / 2);
        }

    const unsigned stride = ((width + 15) / 16) * 2;
    const unsigned scanline_size = stride * (planes + (masking == 1));
    const unsigned transparent = word(file, header + 12);
    Frame frame{0, 0, width, height, Bytes(std::size_t(width) * height * 4)};
    auto at = body;
    Bytes row;
    row.reserve(scanline_size);
    for (unsigned y = 0; y < height; ++y) {
        row.clear();
        if (compression) {
            for (unsigned limit = stride; limit <= scanline_size; limit += stride)
                append_amiga_runs(file, at, body_end, limit, true, true, row);
        } else if (scanline_size <= body_end - at) {
            row.assign(file.begin() + at, file.begin() + at + scanline_size);
            at += scanline_size;
        }
        if (row.size() != scanline_size)
            throw std::runtime_error("Truncated Amiga ILBM scanline");
        for (unsigned x = 0; x < width; ++x) {
            const unsigned bit = 0x80 >> (x % 8);
            unsigned index = 0;
            for (unsigned plane = 0; plane < planes; ++plane)
                if (row[plane * stride + x / 8] & bit)
                    index |= 1u << plane;
            auto* pixel = frame.rgba.data() + (std::size_t(y) * width + x) * 4;
            std::copy(rgb[index].begin(), rgb[index].end(), pixel);
            const bool visible = (masking != 1 || (row[planes * stride + x / 8] & bit)) &&
                                 (masking != 2 || index != transparent);
            pixel[3] = visible ? 255 : 0;
        }
    }
    PictureArchive archive;
    archive.frames.push_back(std::move(frame));
    return archive;
}

namespace {
constexpr std::size_t intro_pack_start = 0x238;
constexpr std::size_t intro_raw_size = 135036;
constexpr std::size_t intro_data_start = 6240;

bool amiga_intro_header(const Bytes& bytes)
{
    return bytes.size() >= 36 && dword(bytes, 0) == 0x3f3 && dword(bytes, 8) == 2 &&
        dword(bytes, 28) == 0x3e9 && dword(bytes, 32) == 0x83;
}

Bytes expand_amiga_intro(const Bytes& packed)
{
    if (!amiga_intro_header(packed) || packed.size() < intro_pack_start + 12 ||
        dword(packed, 0x228) != 0x09090909 || dword(packed, 0x230) != 0x3ea)
        throw std::runtime_error("Invalid Pool of Radiance Amiga intro header");
    const auto length = dword(packed, 0x17c);
    if (length < 8 || length % 4 || length > packed.size() - intro_pack_start - 4)
        throw std::runtime_error("Invalid Amiga intro packed length");
    auto cursor = intro_pack_start + length;
    const auto previous_word = [&] {
        if (cursor == intro_pack_start)
            throw std::runtime_error("Truncated Amiga intro bitstream");
        cursor -= 4;
        return dword(packed, cursor);
    };
    const auto trailer = previous_word();
    const unsigned skip = trailer & 255;
    if ((trailer >> 8) != intro_raw_size || skip >= 32)
        throw std::runtime_error("Invalid Amiga intro compression trailer");
    auto bits = previous_word() >> skip;
    unsigned available = 32 - skip;
    const auto read_bits = [&](unsigned count) {
        unsigned value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!available) {
                bits = previous_word();
                available = 32;
            }
            value = (value << 1) | (bits & 1);
            bits >>= 1;
            --available;
        }
        return value;
    };
    Bytes raw(intro_raw_size);
    auto dest = raw.size();
    const auto run_length = [&](std::size_t size, unsigned extension_bits) {
        unsigned extra;
        do {
            extra = read_bits(extension_bits);
            size += extra;
            if (size > dest)
                throw std::runtime_error("Amiga intro run exceeds unpacked size");
        } while (extra == (1u << extension_bits) - 1);
        return size;
    };
    while (dest) {
        if (!read_bits(1)) {
            const auto count = run_length(1, 2);
            for (std::size_t i = 0; i < count; ++i)
                raw[--dest] = static_cast<std::uint8_t>(read_bits(8));
            if (!dest)
                break;
        }
        const auto tag = read_bits(2);
        const auto offset = read_bits(tag == 3 && !read_bits(1) ? 7 : 9);
        const auto count = tag == 3 ? run_length(5, 3) : tag + 2;
        if (count > dest || offset >= raw.size() - dest)
            throw std::runtime_error("Invalid Amiga intro back-reference");
        for (std::size_t i = 0; i < count; ++i) {
            const auto byte = raw[dest + offset];
            raw[--dest] = byte;
        }
    }
    // These image offsets are specific to the original English executable layout.
    unsigned a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    if (a != 0x0428 || b != 0x875c)
        throw std::runtime_error("Unsupported or corrupt Pool of Radiance Amiga intro");
    return raw;
}

Frame intro_frame(const Bytes& raw, unsigned screen)
{
    struct Strip { unsigned source, destination, height; };
    constexpr Strip strips[]{
        {93,30,48}, {455,60,16}, {398,30,56}, {474,60,19},
        {141,20,146}, {0,20,92}, {288,20,110}, {499,20,100},
    };
    Frame frame{screen, 0, 320, 200, Bytes(320 * 200 * 4)};
    const auto cmap = intro_data_start + 128302 + (screen == 5 ? 32 : 0);
    std::array<std::array<std::uint8_t, 4>, 16> palette{};
    for (unsigned i = 0; i < palette.size(); ++i) {
        const auto rgb = word(raw, cmap + i * 2);
        palette[i] = {static_cast<std::uint8_t>(((rgb >> 8) & 15) * 17),
                      static_cast<std::uint8_t>(((rgb >> 4) & 15) * 17),
                      static_cast<std::uint8_t>((rgb & 15) * 17), 255};
    }
    const bool title = screen == std::size(strips);
    const unsigned first = title ? 0 : strips[screen].destination;
    const unsigned end = title ? frame.height : first + strips[screen].height;
    const unsigned span = title ? 8000 : 24000;
    const auto source = title ? intro_data_start : intro_data_start + 32032 + strips[screen].source * 40;
    for (unsigned y = 0; y < frame.height; ++y)
        for (unsigned x = 0; x < frame.width; ++x) {
            unsigned index = 0;
            if (y >= first && y < end) {
                const auto at = source + (y - first) * 40 + x / 8;
                for (unsigned plane = 0; plane < 4; ++plane)
                    index |= ((raw[at + plane * span] >> (7 - x % 8)) & 1) << plane;
            }
            std::copy(palette[index].begin(), palette[index].end(),
                      frame.rgba.data() + (y * frame.width + x) * 4);
        }
    return frame;
}
}

bool is_amiga_intro(const std::filesystem::path& path)
{
    auto name = path.filename().string();
    for (auto& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name != "intro")
        return false;
    std::ifstream stream(path, std::ios::binary);
    Bytes header(36);
    return stream.read(reinterpret_cast<char*>(header.data()), header.size()) && amiga_intro_header(header);
}

PictureArchive read_amiga_intro(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    const Bytes packed{std::istreambuf_iterator<char>(stream), {}};
    const auto raw = expand_amiga_intro(packed);
    PictureArchive archive;
    archive.frame_labels = {"SSI", "And...", "Ubi Soft", "Present", "AD&D and copyright",
                            "Pool of Radiance", "Game credits", "Amiga credits", "Title artwork"};
    const auto count = archive.frame_labels.size();
    for (unsigned screen = 0; screen < count; ++screen)
        archive.frames.push_back(intro_frame(raw, screen));
    PictureArchive::Clip clip{0, {}, true, true, "Introduction"};
    // Use the portable game port's pacing for the executable's RGB4 fades.
    constexpr int fade_ms = 40, hold_ms = 5000, gap_ms = 400;
    Frame black{0, 16, 320, 200, Bytes(320 * 200 * 4)};
    for (std::size_t i = 3; i < black.rgba.size(); i += 4)
        black.rgba[i] = 255;
    const auto black_index = archive.frames.size();
    archive.frames.push_back(std::move(black));
    for (std::size_t screen = 0; screen < count; ++screen) {
        std::array<std::size_t, 17> fade{};
        fade[1] = screen;
        fade[16] = black_index;
        for (unsigned divisor = 2; divisor < 16; ++divisor) {
            auto frame = archive.frames[screen];
            frame.index = divisor - 1;
            for (std::size_t i = 0; i < frame.rgba.size(); i += 4)
                for (unsigned c = 0; c < 3; ++c)
                    frame.rgba[i + c] = static_cast<std::uint8_t>((frame.rgba[i + c] / 17 / divisor) * 17);
            fade[divisor] = archive.frames.size();
            archive.frames.push_back(std::move(frame));
        }
        for (unsigned divisor = 16; divisor > 1; --divisor)
            clip.steps.push_back({fade[divisor], fade_ms});
        clip.steps.push_back({screen, 2 * fade_ms + hold_ms});
        for (unsigned divisor = 2; divisor <= 16; ++divisor)
            clip.steps.push_back({fade[divisor], fade_ms});
        clip.steps.back().duration_ms += gap_ms;
    }
    archive.playback_only_frames = archive.frames.size() - count;
    archive.clips.push_back(std::move(clip));
    return archive;
}

std::optional<PictureArchive> try_amiga_pictures(const std::filesystem::path& path)
{
    auto title = path.stem().string();
    for (auto& ch : title)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    auto ext = path.extension().string();
    for (auto& ch : ext)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (ext == ".DAA")
        return read_daa(path, title);
    const auto match = std::find_if(std::begin(formats), std::end(formats),
                                    [&](const auto& format) { return title == format.name; });
    if (match == std::end(formats))
        return std::nullopt;
    const auto records = try_amiga_dax(path);
    if (records.empty())
        return std::nullopt;
    PictureArchive archive;
    for (const auto& record : records) {
        const auto first = archive.frames.size();
        try {
            append_record(record, *match, archive);
        } catch (const std::runtime_error&) {
            archive.frames.resize(first);
            archive.skipped_ids.push_back(record.id);
        }
    }
    return archive;
}
}
