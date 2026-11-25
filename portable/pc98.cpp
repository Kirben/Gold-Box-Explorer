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

bool pc98_picture_sequence(const std::string& family, const Bytes& bytes)
{
    return family == "PIC" ||
        (family == "FINAL" && bytes.size() >= 3 && word(bytes, 0) > 640);
}

constexpr unsigned pc98_first_bank_frames = 3;
constexpr unsigned pc98_max_frames = 6;
// MEGDOS.SYS $1225 returns the CLOCK device time. The supplied IO98.SYS
// driver zeros the hundredths field at $205B, so consecutive values of
// GetTicks (GAME.EXE $5332) are one second apart.
constexpr unsigned pc98_clock_step_ms = 1000;
constexpr unsigned pc98_default_speed = 5;

using PictureBlocks = std::array<std::vector<std::size_t>, 256>;

void draw_pc98_ending_layer(Frame& destination, const Frame& source, unsigned x, unsigned y)
{
    const auto width = std::min(source.width, destination.width - x);
    const auto height = std::min(source.height, destination.height - y);
    for (unsigned row = 0; row < height; ++row)
        std::copy_n(source.rgba.data() + row * source.width * 4, width * 4,
                    destination.rgba.data() + ((y + row) * destination.width + x) * 4);
}

void add_pc98_ending(const PictureBlocks& blocks, PictureArchive& archive)
{
    constexpr unsigned scene_size = 240;
    struct Scene {
        unsigned picture, background, top, transitions;
    };
    constexpr unsigned first_scenes_transitions = (10 - pc98_default_speed) * 5 / 2;
    constexpr Scene scenes[]{
        {2, 1, 64, first_scenes_transitions},
        {7, 3, 0, first_scenes_transitions},
        {8, 3, 0, 6},
    };
    if (std::any_of(blocks.begin() + 1, blocks.begin() + 9,
                    [](const auto& block) { return block.empty(); }))
        return;
    for (const auto& scene : scenes) {
        const auto& background = archive.frames[blocks[scene.background].front()];
        if (background.width != scene_size || background.height != scene_size ||
            blocks[scene.picture].size() != (scene.picture == 2 ? 4 : 6))
            return;
    }

    const auto gallery_count = archive.frames.size();
    PictureArchive::Clip clip{0, {}, true, true};
    // GAME.OVR $2254E checks for a changed clock value after drawing each
    // frame. $2290E and $22B28 pass (10 - speed) * 5 div 2 (speed defaults
    // to 5 at $ED54), and $22BBF passes 6. Dialogue waits are omitted.
    for (const auto& scene : scenes) {
        auto background = archive.frames[blocks[scene.background].front()];
        if (scene.background == 3) {
            draw_pc98_ending_layer(background, archive.frames[blocks[4].front()], 0, 176);
            draw_pc98_ending_layer(background, archive.frames[blocks[5].front()], 208, 144);
            draw_pc98_ending_layer(background, archive.frames[blocks[6].front()], 0, 144);
        }
        const auto& pictures = blocks[scene.picture];
        const auto first = archive.frames.size();
        for (std::size_t i = 0; i < pictures.size(); ++i) {
            auto frame = background;
            frame.id = scene.picture;
            frame.index = static_cast<unsigned>(i);
            draw_pc98_ending_layer(frame, archive.frames[pictures[i]], 32, scene.top);
            archive.frames.push_back(std::move(frame));
        }
        for (unsigned i = 0; i < scene.transitions; ++i)
            clip.steps.push_back({first + i % pictures.size(), pc98_clock_step_ms});
    }
    // GAME.OVR $22BC2 calls the speed-scaled runtime busy wait three
    // times. Use its nominal duration in the portable preview.
    clip.steps.back().duration_ms += 3 * pc98_default_speed * 200;
    archive.playback_only_frames = archive.frames.size() - gallery_count;
    archive.clips.push_back(std::move(clip));
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
    std::array<const DaxRecord*, 256> by_id{};
    PictureBlocks blocks;
    std::array<bool, 256> sequences{};
    for (const auto& record : records)
        by_id[record.id] = &record;
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
            const bool animated = pc98_picture_sequence(family, bytes);
            const bool sequence = animated || encounter;
            std::size_t at = sequence ? 1 : 0;
            const std::uint8_t* base = nullptr;
            unsigned base_width = 0, base_height = 0;
            const auto* primary = record.id >= 128 ? by_id[record.id - 128] : nullptr;
            // GAME.OVR $33E2C opens id + 128; $34016 XORs those frames
            // against the base retained from the first record.
            if (animated && primary && primary->bytes.size() >= 10 &&
                pc98_picture_sequence(family, primary->bytes) &&
                primary->bytes.front() > pc98_first_bank_frames &&
                primary->bytes.front() == bytes.front()) {
                base_width = word(primary->bytes, 1);
                base_height = word(primary->bytes, 3);
                if (base_width / 8 * base_height * planes <= primary->bytes.size() - 10)
                    base = primary->bytes.data() + 10;
            }
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
                    if (!index && animated && !base) {
                        base = bytes.data() + at;
                        base_width = width;
                        base_height = height;
                    }
                    at += size;
                } while (tile_bank && at < bytes.size());
            }
            for (auto i = first; i < result.frames.size(); ++i)
                blocks[record.id].push_back(i);
            sequences[record.id] = animated;
        } catch (const std::runtime_error&) {
            result.frames.resize(first);
            result.skipped_ids.push_back(record.id);
        }
    }
    if (result.frames.empty())
        throw std::runtime_error("No supported PC-98 pictures in archive");
    if (family == "PIC" || family == "FINAL") {
        for (unsigned id = 0; id < 128; ++id) {
            if (!sequences[id] || blocks[id].empty())
                continue;
            const auto count = std::min<unsigned>(by_id[id]->bytes.front(), pc98_max_frames);
            auto& block = blocks[id];
            const auto& continuation = blocks[id + 128];
            const bool split = count > pc98_first_bank_frames && sequences[id + 128] &&
                by_id[id + 128]->bytes.front() == by_id[id]->bytes.front();
            if (split)
                block.insert(block.end(), continuation.begin(), continuation.end());
            if (block.size() > count)
                block.resize(count);
            if (family == "PIC" && block.size() > 1) {
                PictureArchive::Clip clip{id, {}};
                // GAME.OVR $32656 advances when elapsed centiseconds exceed three
                // times the delay of five loaded at $33BAA and $33EF7.
                // A single clock change already exceeds that threshold.
                for (const auto frame : block)
                    clip.steps.push_back({frame, pc98_clock_step_ms});
                result.clips.push_back(clip);
                if (split && !continuation.empty()) {
                    clip.id = id + 128;
                    result.clips.push_back(std::move(clip));
                }
            }
        }
        if (family == "FINAL")
            add_pc98_ending(blocks, result);
    }
    return result;
}
}
