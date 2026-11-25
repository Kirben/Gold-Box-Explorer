#include "apng.h"

#include <wx/mstream.h>
#include <wx/zstream.h>

#include <algorithm>
#include <numeric>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;

void append_integer(Bytes& bytes, std::uint32_t value, int width = 4)
{
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool write_chunk(wxOutputStream& output, const char* type, const Bytes& data)
{
    Bytes header;
    append_integer(header, static_cast<std::uint32_t>(data.size()));
    header.insert(header.end(), type, type + 4);
    std::uint32_t crc = 0xffffffff;
    const auto update = [&](std::uint8_t byte) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    };
    for (int i = 0; i < 4; ++i)
        update(static_cast<std::uint8_t>(type[i]));
    for (auto byte : data)
        update(byte);
    Bytes checksum;
    append_integer(checksum, crc ^ 0xffffffffu);
    output.Write(header.data(), header.size());
    if (!data.empty())
        output.Write(data.data(), data.size());
    output.Write(checksum.data(), checksum.size());
    return output.IsOk();
}

struct TimedFrame {
    std::size_t index;
    unsigned numerator;
    unsigned denominator;
};

std::vector<TimedFrame> timed_frames(const PictureArchive::Clip& clip)
{
    std::vector<TimedFrame> result;
    for (const auto& step : clip.steps) {
        unsigned milliseconds = step.duration_ms;
        const auto divisor = std::gcd(milliseconds, 1000u);
        if (milliseconds / divisor <= 65535) {
            result.push_back({step.frame, milliseconds / divisor, 1000 / divisor});
            continue;
        }
        // APNG stores the delay as two 16-bit integers. Repeat the same
        // image in whole-second spans to preserve unusually long delays.
        while (milliseconds >= 1000) {
            const auto seconds = std::min(milliseconds / 1000, 65535u);
            result.push_back({step.frame, seconds, 1});
            milliseconds -= seconds * 1000;
        }
        if (milliseconds)
            result.push_back({step.frame, milliseconds, 1000});
    }
    return result;
}

bool compress_frame(const Frame& frame, unsigned width, unsigned height, Bytes& data)
{
    wxMemoryOutputStream buffer;
    wxZlibOutputStream compressed(buffer);
    // Filter byte 0 and transparent padding to the shared canvas size.
    Bytes row(1 + 4 * width, 0);
    for (unsigned y = 0; y < height; ++y) {
        if (y < frame.height)
            std::copy_n(frame.rgba.data() + 4 * frame.width * y, 4 * frame.width, row.data() + 1);
        else if (y == frame.height)
            std::fill(row.begin(), row.end(), 0);
        compressed.Write(row.data(), row.size());
    }
    if (!compressed.IsOk() || !compressed.Close() || !buffer.IsOk())
        return false;
    data.resize(buffer.GetSize());
    return buffer.CopyTo(data.data(), data.size()) == data.size();
}
}

bool write_apng(wxOutputStream& output, const std::vector<Frame>& frames,
                const PictureArchive::Clip& clip)
{
    unsigned width = 0;
    unsigned height = 0;
    for (const auto& step : clip.steps) {
        const auto& frame = frames.at(step.frame);
        width = std::max(width, frame.width);
        height = std::max(height, frame.height);
    }
    if (!width || !height)
        return false;
    constexpr std::uint8_t signature[]{137, 'P', 'N', 'G', 13, 10, 26, 10};
    output.Write(signature, sizeof signature);
    Bytes header;
    append_integer(header, width);
    append_integer(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
    const auto timing = timed_frames(clip);
    Bytes animation;
    append_integer(animation, static_cast<std::uint32_t>(timing.size()));
    append_integer(animation, 1); // Play once and leave the final frame visible.
    if (!write_chunk(output, "IHDR", header) || !write_chunk(output, "acTL", animation))
        return false;

    std::uint32_t sequence = 0;
    for (const auto& step : timing) {
        const bool first = sequence == 0;
        Bytes control;
        append_integer(control, sequence++);
        append_integer(control, width);
        append_integer(control, height);
        append_integer(control, 0); // x offset
        append_integer(control, 0); // y offset
        append_integer(control, step.numerator, 2);
        append_integer(control, step.denominator, 2);
        control.insert(control.end(), {0, 0}); // No disposal, replace canvas including alpha.
        Bytes pixels;
        if (!write_chunk(output, "fcTL", control) ||
            !compress_frame(frames.at(step.index), width, height, pixels))
            return false;
        if (first) {
            if (!write_chunk(output, "IDAT", pixels))
                return false;
        } else {
            Bytes data;
            append_integer(data, sequence++);
            data.insert(data.end(), pixels.begin(), pixels.end());
            if (!write_chunk(output, "fdAT", data))
                return false;
        }
    }
    return write_chunk(output, "IEND", {});
}
}
