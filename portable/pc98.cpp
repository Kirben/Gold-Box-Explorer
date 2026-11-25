#include "pc98.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <regex>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;

std::string upper(std::string name)
{
    for (auto& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

unsigned word(const Bytes& bytes, std::size_t at)
{
    return unsigned(bytes.at(at)) | unsigned(bytes.at(at + 1)) << 8;
}

Frame planar(const Bytes& bytes, std::size_t offset, unsigned id, unsigned index,
             unsigned width, unsigned height, unsigned planes, const std::uint8_t* base = nullptr)
{
    const auto stride = width / 8;
    const auto plane_size = stride * height;
    Frame frame{id, index, width, height, Bytes(width * height * 4)};
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const unsigned mask = 0x80 >> (x % 8);
            const auto at = y * stride + x / 8;
            const auto* bits = bytes.data() + offset + at;
            if (planes == 4 && (bits[plane_size * 3] & mask))
                continue;
            auto* pixel = frame.rgba.data() + (y * width + x) * 4;
            // The file stores the VRAM planes in blue, red, green order.
            constexpr unsigned channels[]{1,2,0};
            for (unsigned c = 0; c < 3; ++c) {
                const auto p = channels[c] * plane_size;
                const unsigned value = bits[p] ^ (base ? base[at + p] : 0);
                pixel[c] = (value & mask) ? 255 : 0;
            }
            pixel[3] = 255;
        }
    return frame;
}
}

std::optional<Bytes> expand_pc98(const std::uint8_t* packed, std::size_t length, unsigned size)
{
    if (!length)
        return {};
    if (packed[0] == 0xff && length == std::size_t(size) + 1)
        return Bytes(packed + 1, packed + length);
    if (packed[0] != 5 || length < 2 || !size)
        return {};
    Bytes bytes;
    bytes.reserve(size);
    for (std::size_t i = 2; i < length;) {
        const unsigned command = packed[i++];
        const unsigned count = command < 128 ? command : command < 192 ? command - 125 : command - 190;
        const unsigned stored = command < 128 ? count : command < 192 ? 1 : 0;
        if (count > size - bytes.size() || stored > length - i)
            return {};
        if (command < 128)
            bytes.insert(bytes.end(), packed + i, packed + i + count);
        else
            bytes.insert(bytes.end(), count, stored ? packed[i] : packed[1]);
        i += stored;
    }
    if (bytes.size() != size)
        return {};
    return bytes;
}

std::string pc98_text(std::string_view text)
{
    if (text.empty())
        return {};
    // The game uses single-byte glyphs $61..$79 for voiced katakana.
    // Keep a Shift-JIS trail byte intact even when it falls in that range.
    constexpr unsigned kana[]{
        0x834b,0x834d,0x834f,0x8351,0x8353,0x8355,0x8357,0x8359,0x835b,0x835d,
        0x835f,0x8361,0x8364,0x8366,0x8368,0x836f,0x8372,0x8375,0x8378,0x837b,
        0x8370,0x8373,0x8376,0x8379,0x837c
    };
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool lead = (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
        if (c >= 0x61 && c <= 0x79) {
            const auto value = kana[c - 0x61];
            normalized += static_cast<char>(value >> 8);
            normalized += static_cast<char>(value & 255);
        } else {
            normalized += text[i];
            if (lead && i + 1 < text.size())
                normalized += text[++i];
        }
    }
    text = normalized;
#ifdef _WIN32
    const auto size = static_cast<int>(text.size());
    const auto wide_size = MultiByteToWideChar(932, 0, text.data(), size, nullptr, 0);
    if (wide_size > 0) {
        std::wstring wide(wide_size, L'\0');
        MultiByteToWideChar(932, 0, text.data(), size, wide.data(), wide_size);
        const auto utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
        std::string result(utf8_size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, result.data(), utf8_size, nullptr, nullptr);
        return result;
    }
#else
    auto converter = iconv_open("UTF-8", "CP932");
    if (converter != reinterpret_cast<iconv_t>(-1)) {
        std::string result(text.size() * 4, '\0');
        auto* input = const_cast<char*>(text.data());
        auto* output = result.data();
        auto remaining = text.size(), capacity = result.size();
        while (remaining) {
            if (iconv(converter, &input, &remaining, &output, &capacity) != static_cast<std::size_t>(-1))
                break;
            *output++ = '?';
            --capacity;
            ++input;
            --remaining;
        }
        iconv_close(converter);
        result.resize(result.size() - capacity);
        return result;
    }
#endif
    std::string result(text);
    for (auto& c : result)
        if (static_cast<unsigned char>(c) >= 128)
            c = '?';
    return result;
}

bool is_pc98_title(const std::filesystem::path& path)
{
    static const std::regex name(R"(TITLE[12][BGR]\.DAX)", std::regex::icase);
    return std::regex_match(path.filename().string(), name);
}

PictureArchive read_pc98_title(const std::filesystem::path& path)
{
    const auto prefix = upper(path.stem().string()).substr(0, 6);
    const auto folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    std::array<Bytes, 3> planes;
    const std::string channels = "BRG";
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        const auto name = upper(entry.path().filename().string());
        if (!is_pc98_title(entry.path()) || name.compare(0, 6, prefix) != 0)
            continue;
        auto records = read_dax(entry.path());
        if (records.size() != 1 || !records.front().pc98 || records.front().bytes.size() != 32000)
            throw std::runtime_error("Invalid PC-98 title plane");
        planes[channels.find(name[6])] = std::move(records.front().bytes);
    }
    Bytes combined;
    for (const auto& plane : planes) {
        if (plane.empty())
            throw std::runtime_error("Missing PC-98 title colour plane");
        combined.insert(combined.end(), plane.begin(), plane.end());
    }
    PictureArchive result;
    result.frames.push_back(planar(combined, 0, prefix[5] - '0', 0, 640, 400, 3));
    return result;
}

std::vector<Frame> decode_pc98_wall_tiles(const DaxRecord& record)
{
    const auto& bytes = record.bytes;
    const bool headerless = record.id == 201 && bytes.size() == 5664;
    const unsigned planes = headerless ? 3 : 4, header = headerless ? 0 : 9;
    const unsigned stride = 32 * planes;
    if (bytes.size() < header || (bytes.size() - header) % stride)
        throw std::runtime_error("Invalid PC-98 wall glyph bank");
    std::vector<Frame> frames;
    for (std::size_t at = header; at < bytes.size(); at += stride)
        frames.push_back(planar(bytes, at, record.id, static_cast<unsigned>(frames.size()), 16, 16, planes));
    return frames;
}

PictureArchive read_pc98_pictures(const std::filesystem::path& path, const std::vector<DaxRecord>& records)
{
    const auto family = upper(path.stem().string());
    const bool tile_bank = family == "DUNGCOM" || family == "RANDCOM" || family == "WILDCOM" || family == "BACPAC";
    const bool encounter = family == "SPRIT";
    const bool masked = encounter || family == "CPIC" || family == "COMSPR" ||
        family == "ICON" || family == "CHEAD" || family == "CBODY";
    const unsigned planes = masked ? 4 : 3;
    PictureArchive result;
    for (const auto& record : records) {
        const auto& bytes = record.bytes;
        const auto first = result.frames.size();
        try {
            if (family == "8X8D") {
                auto tiles = decode_pc98_wall_tiles(record);
                result.frames.insert(result.frames.end(), std::make_move_iterator(tiles.begin()),
                                     std::make_move_iterator(tiles.end()));
                continue;
            }
            bool sequence = family == "PIC" || encounter;
            if (family == "FINAL" && bytes.size() >= 3 && word(bytes, 0) > 640)
                sequence = true;
            std::size_t at = sequence ? 1 : 0;
            const std::uint8_t* base = nullptr;
            unsigned base_width = 0, base_height = 0;
            while (at < bytes.size()) {
                if (bytes.size() - at < 9)
                    throw std::runtime_error("Truncated PC-98 picture header");
                const unsigned width = word(bytes, at), height = word(bytes, at + 2);
                if (!width || width > 640 || width % 8 || !height || height > 400)
                    throw std::runtime_error("Invalid PC-98 picture dimensions");
                at += 9;
                const unsigned size = width / 8 * height * planes;
                if (size > bytes.size() - at || (tile_bank && (bytes.size() - at) % size))
                    throw std::runtime_error("Invalid PC-98 picture plane size");
                do {
                    const auto index = static_cast<unsigned>(result.frames.size() - first);
                    result.frames.push_back(planar(bytes, at, record.id, index, width, height, planes,
                        sequence && !encounter && width == base_width && height == base_height ? base : nullptr));
                    if (!index && sequence) {
                        base = bytes.data() + at;
                        base_width = width;
                        base_height = height;
                    }
                    at += size;
                } while (tile_bank && at < bytes.size());
            }
            if (!encounter && result.frames.size() - first > 1 && sequence) {
                PictureArchive::Clip clip{record.id, {}};
                for (auto i = first; i < result.frames.size(); ++i)
                    clip.steps.push_back({i, 200});
                result.clips.push_back(std::move(clip));
            }
        } catch (const std::runtime_error&) {
            result.frames.resize(first);
            result.skipped_ids.push_back(record.id);
        }
    }
    if (result.frames.empty())
        throw std::runtime_error("No supported PC-98 pictures in archive");
    return result;
}
}
