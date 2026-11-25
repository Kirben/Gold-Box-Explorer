#include "vace.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace goldbox {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
constexpr unsigned width = 320;
constexpr unsigned height = 200;
constexpr unsigned max_frames = 512;
using Pixels = std::array<std::uint8_t, width * height>;
using Color = std::array<std::uint8_t, 4>;
using Palette = std::array<Color, 256>;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string upper(std::string name)
{
    for (auto& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

fs::path companion(const fs::path& selected, const char* extension)
{
    if (upper(selected.extension().string()) == extension)
        return selected;
    const auto name = upper(selected.stem().string()) + extension;
    const auto folder = selected.has_parent_path() ? selected.parent_path() : fs::path(".");
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (upper(entry.path().filename().string()) == name && entry.is_regular_file())
            return entry.path();
    }
    throw std::runtime_error("VACE graphics require the matching " + name + " file in the same folder");
}

Bytes read_file(const fs::path& path, std::size_t limit)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.is_open(), "Unable to open VACE file");
    const auto length = file.tellg();
    require(length > 0 && length <= static_cast<std::streamoff>(limit), "Invalid VACE file size");
    Bytes bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    require(bool(file.read(reinterpret_cast<char*>(bytes.data()), length)), "Unable to read VACE file");
    return bytes;
}

unsigned word(const Bytes& bytes, std::size_t at)
{
    return bytes[at] | (unsigned(bytes[at + 1]) << 8);
}

std::uint32_t dword(const Bytes& bytes, std::size_t at)
{
    return word(bytes, at) | (std::uint32_t(word(bytes, at + 2)) << 16);
}

struct Group {
    unsigned length;
    std::vector<std::size_t> offsets;
};

std::vector<Group> read_groups(const Bytes& bytes)
{
    std::vector<Group> groups;
    unsigned frames = 0;
    for (std::size_t start = 0; start < bytes.size();) {
        require(bytes.size() - start >= 6, "Truncated BMA group");
        const auto length = dword(bytes, start);
        const auto count = word(bytes, start + 4);
        require(count && count <= max_frames - frames && length <= bytes.size() - start &&
                length <= 65535 && length > 6 + count * 4, "Invalid BMA group header");
        Group group{length, {}};
        auto previous = 5 + count * 4;
        for (unsigned i = 0; i < count; ++i) {
            const auto offset = dword(bytes, start + 6 + i * 4);
            require(offset > previous && offset <= length - 5, "Invalid BMA frame offset");
            group.offsets.push_back(start + offset);
            previous = offset;
        }
        group.offsets.push_back(start + length);
        groups.push_back(std::move(group));
        frames += count;
        start += length;
    }
    return groups;
}

void draw_frame(const Bytes& bytes, std::size_t at, std::size_t end, Pixels& pixels)
{
    require(end - at >= 5 && word(bytes, at) == width && word(bytes, at + 2) == height,
            "Invalid BMA frame header");
    at += 4;
    while (at < end) {
        const unsigned row = bytes[at++];
        if (row == 255) {
            require(at == end, "Unexpected bytes after BMA frame");
            return;
        }
        require(row < height, "Invalid BMA row");
        bool last;
        do {
            require(end - at >= 4, "Truncated BMA span");
            const auto header = word(bytes, at);
            last = (header & 0x8000) != 0;
            const auto column = header & 0x7fff;
            const unsigned count = bytes[at + 2];
            const unsigned packed = bytes[at + 3];
            at += 4;
            require(count && column + count <= width && packed <= end - at, "Invalid BMA span bounds");
            const auto packed_end = at + packed;
            auto output = row * width + column;
            const auto output_end = output + count;
            while (at < packed_end) {
                const auto token = bytes[at++];
                const unsigned run = (token >> 1) + 1;
                require(run <= output_end - output, "BMA run exceeds span");
                if (token & 1) {
                    require(at < packed_end, "Truncated BMA repeat");
                    std::fill_n(pixels.data() + output, run, bytes[at++]);
                } else {
                    require(run <= packed_end - at, "Truncated BMA literal");
                    std::copy_n(bytes.data() + at, run, pixels.data() + output);
                    at += run;
                }
                output += run;
            }
            require(output == output_end, "BMA span length mismatch");
        } while (!last);
    }
    throw std::runtime_error("BMA frame has no terminator");
}

std::uint8_t dac(unsigned value)
{
    return static_cast<std::uint8_t>((value & 63) * 255 / 63);
}

Frame render(const Pixels& pixels, const Palette& palette, unsigned index)
{
    Frame frame{0, index, width, height, Bytes(pixels.size() * 4)};
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const auto& color = palette[pixels[i]];
        std::copy(color.begin(), color.end(), frame.rgba.data() + 4 * i);
    }
    return frame;
}
}

bool is_vace_archive(const fs::path& path)
{
    const auto stem = upper(path.stem().string());
    const auto extension = upper(path.extension().string());
    return (stem == "INTRO" || stem == "FINAL") && (extension == ".BMA" || extension == ".ACF");
}

PictureArchive read_vace(const fs::path& path)
{
    const auto data = read_file(companion(path, ".BMA"), 16 * 1024 * 1024);
    const auto control = read_file(companion(path, ".ACF"), 128 * 1024);
    const auto groups = read_groups(data);
    PictureArchive archive;
    PictureArchive::Clip clip{0, {}};
    Pixels pixels{};
    Palette palette;
    palette.fill({0, 0, 0, 255});
    std::vector<std::size_t> clears;
    unsigned group = 0;
    unsigned frame = 0;
    unsigned metadata = 0;
    unsigned speed = 1;
    bool ended = false;
    for (std::size_t at = 0; at < control.size();) {
        require(control.size() - at >= 2, "Truncated ACF command");
        const unsigned length = control[at];
        require(length >= 2 && !(length & 1) && length <= control.size() - at, "Invalid ACF command length");
        switch (control[at + 1]) {
        case 0:
            require(length == 4, "Invalid ACF speed command");
            speed = std::max(1u, word(control, at + 2));
            break;
        case 1:
            require(length == 2 && at + length == control.size(), "Invalid ACF end");
            ended = true;
            break;
        case 2:
            require(length == 6, "Invalid ACF palette command");
            palette[control[at + 3]] = {dac(control[at + 2]), dac(control[at + 5]),
                                       dac(control[at + 4]), 255};
            break;
        case 3: {
            require(length == 2 && group < groups.size(), "ACF requests a frame outside the BMA");
            const auto& offsets = groups[group].offsets;
            draw_frame(data, offsets[frame], offsets[frame + 1], pixels);
            clip.steps.push_back({archive.frames.size(), static_cast<int>((speed * 1000 + 35) / 70)});
            archive.frames.push_back(render(pixels, palette, static_cast<unsigned>(archive.frames.size())));
            if (++frame == offsets.size() - 1) {
                ++group;
                frame = 0;
            }
            break;
        }
        case 5:
            require(length == 8 && metadata < groups.size() && word(control, at + 2) == metadata &&
                    word(control, at + 4) == groups[metadata].offsets.size() - 1 &&
                    word(control, at + 6) == groups[metadata].length, "ACF group table does not match the BMA");
            ++metadata;
            break;
        case 11:
            require(length == 2, "Invalid ACF clear command");
            pixels.fill(0);
            clears.push_back(clip.steps.size());
            clip.steps.push_back({0, 15});
            break;
        case 12: // Preview uses a fixed hold for this scene boundary.
            require(length == 2, "Invalid ACF hold command");
            if (!archive.frames.empty())
                clip.steps.push_back({archive.frames.size() - 1, 2000});
            break;
        case 8: // Music and driver commands have no gallery output.
        case 9:
        case 13:
        case 14:
            break;
        default:
            throw std::runtime_error("Unsupported VACE ACF opcode " + std::to_string(control[at + 1]));
        }
        at += length;
    }
    require(ended && !archive.frames.empty() && group == groups.size() && metadata == groups.size(),
            "Incomplete ACF frame sequence or group table");
    if (!clears.empty()) {
        const auto index = archive.frames.size();
        for (auto step : clears)
            clip.steps[step].frame = index;
        Pixels black{};
        Palette black_palette{};
        black_palette[0][3] = 255;
        archive.frames.push_back(render(black, black_palette, static_cast<unsigned>(index)));
        archive.playback_only_frames = 1;
    }
    archive.clips.push_back(std::move(clip));
    return archive;
}
}
