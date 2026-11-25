#include "amiga_pictures.h"
#include "ecl_containers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Palette = std::array<unsigned, 16>;
constexpr Palette portrait{0x000,0xdb9,0xb97,0x964,0x521,0x742,0xa84,0xcb6,0xff9,0x007,0x44b,0x444,0x555,0x777,0xaaa,0xfff};
constexpr Palette scene{0x000,0x06b,0x096,0x0aa,0x000,0x940,0xb44,0xfaa,0xfe9,0x59f,0x0d8,0x5ff,0x555,0x777,0xaaa,0xfff};
constexpr Palette picture{0x000,0x124,0x953,0xb97,0xecb,0x710,0xa42,0xdb0,0xff5,0x074,0x053,0x369,0x555,0x777,0xaaa,0xfff};

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
}

std::optional<PictureArchive> try_amiga_pictures(const std::filesystem::path& path)
{
    auto title = path.stem().string();
    for (auto& ch : title)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
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
