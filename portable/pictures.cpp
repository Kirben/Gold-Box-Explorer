#include "pictures.h"
#include "dax.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <stdexcept>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Color = std::array<std::uint8_t, 4>;
constexpr std::array<Color, 16> ega{{
    {0,0,0,255}, {0,0,170,255}, {0,170,0,255}, {0,170,170,255},
    {170,0,0,255}, {170,0,170,255}, {170,85,0,255}, {170,170,170,255},
    {85,85,85,255}, {85,85,255,255}, {85,255,85,255}, {85,255,255,255},
    {255,85,85,255}, {255,85,255,255}, {255,255,85,255}, {255,255,255,255}
}};

unsigned word(const Bytes& data, std::size_t offset)
{
    return data.at(offset) | (unsigned(data.at(offset + 1)) << 8);
}

Bytes unpack(const Bytes& file, std::size_t start, std::size_t length, unsigned raw_size)
{
    auto end = start + length;
    if (!raw_size)
        return Bytes(file.begin() + start, file.begin() + end);

    Bytes result;
    result.reserve(raw_size);
    while (start < end && result.size() < raw_size) {
        const auto control = file[start++];
        if (control < 128) {
            const auto count = std::min<std::size_t>(control + 1, end - start);
            if (count > raw_size - result.size())
                throw std::runtime_error("DAX literal exceeds the declared block size");
            result.insert(result.end(), file.begin() + start, file.begin() + start + count);
            start += count;
        } else {
            const unsigned count = 256 - control;
            if (start == end || count > raw_size - result.size())
                throw std::runtime_error("Invalid DAX repeat run");
            result.insert(result.end(), count, file[start++]);
        }
    }
    // The reference reader leaves a zero-filled suffix on short literal runs.
    result.resize(raw_size);
    return result;
}

bool decode_picture(const Bytes& data, unsigned id, unsigned header, std::vector<Frame>& out)
{
    if (data.size() < header + 17)
        return false;
    const unsigned height = word(data, header);
    const unsigned columns = word(data, header + 2);
    const unsigned x = word(data, header + 4) * 8;
    const unsigned y = word(data, header + 6) * 8;
    if (!height || height > 200 || !columns || columns > 40 || x > 320 || y > 200)
        return false;

    unsigned count = header ? data[0] : data[8];
    if (header && (!count || count > 8))
        return false;
    std::array<Color, 32> colors{};
    std::copy(ega.begin(), ega.end(), colors.begin());
    std::copy(ega.begin(), ega.end(), colors.begin() + 16);
    std::size_t pos = header + 9;
    const auto flags = data[pos];
    const auto packed_size = height * columns * 4;
    const bool legacy = header && !(flags & 0xcc) &&
        data.size() == pos + 8 + packed_size * count + 21 * (count - 1);
    const bool planar = (flags & 0xcc) &&
        (header || data.size() != pos + 8 + packed_size * count);
    if (!planar) {
        pos += 8;
        const std::size_t size = height * columns * 4;
        if (!header && data.size() == pos + size * (count + 1))
            ++count;
        if (!legacy && (!count || data.size() != pos + size * count + (header ? 1 : 0))) {
            if (!header || !count || data.size() != pos + size * count)
                return false;
        }
    } else {
        ++pos;
        if (flags & 1) pos += 8;
        if (flags & 2) pos += 16;
        if (flags & 8) {
            if (pos + 24 > data.size())
                return false;
            for (unsigned index = 18; index < 32; ++index) {
                const unsigned palette_index = index - 16;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const auto packed = data[pos + 3 * (palette_index % 8) + channel];
                    colors[index][channel] = 16 * ((packed >> (palette_index >= 8 ? 4 : 0)) & 15);
                }
            }
            pos += 24;
        } else {
            std::fill(colors.begin() + 18, colors.end(), Color{0,0,0,255});
        }
        const auto size = height * columns * 5;
        if (!count || pos + size * count + (header ? 14 * (count - 1) : 0) > data.size())
            return false;
    }

    const unsigned width = columns * 8;
    Bytes indices(width * height);
    for (unsigned index = 0; index < count; ++index) {
        Frame frame{id, index, x + width, y + height, {}};
        frame.rgba.resize(frame.width * frame.height * 4);
        for (unsigned row = 0; row < height; ++row) {
            for (unsigned col = 0; col < width; ++col) {
                auto& pixel = indices[row * width + col];
                if (planar) {
                    for (unsigned plane = 0; plane < 5; ++plane) {
                        const auto packed = data[pos + plane * columns + col / 8];
                        if (!plane && !header)
                            pixel = 0;
                        pixel ^= ((packed >> (7 - col % 8)) & 1) << plane;
                    }
                } else {
                    const auto packed = data[pos + col / 2];
                    pixel = (packed >> (col % 2 == 0 ? 4 : 0)) & 15;
                }
                const auto& color = colors[pixel];
                auto dest = frame.rgba.begin() + 4 * ((row + y) * frame.width + col + x);
                std::copy(color.begin(), color.end(), dest);
            }
            pos += columns * (planar ? 5 : 4);
        }
        if (planar && header)
            pos += 14;
        if (legacy)
            pos += 21;
        out.push_back(std::move(frame));
    }
    return true;
}

bool decode_vga_sprite(const Bytes& data, unsigned id, std::vector<Frame>& out)
{
    if (data.size() < 132 || data[1] || !data[0] || data[0] > 200 ||
        !data[2] || data[2] > 40 || !data[9] || !data[10] || data[9] + data[10] > 256)
        return false;
    const unsigned width = data[2] * 8;
    const unsigned height = data[0];
    const unsigned frame_size = width * height;
    const unsigned header = 132 + 3 * data[10];
    std::size_t pos = header + 3 * data[8];
    if (pos >= data.size())
        return false;
    std::array<Color, 256> palette{};
    for (unsigned index = 0; index < palette.size(); ++index)
        palette[index] = ega[index % 16];
    for (unsigned index = 0; index < data[10]; ++index) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            const auto value = data[11 + index * 3 + channel];
            if (value > 63)
                return false;
            palette[data[9] + index][channel] = value * 4;
        }
    }
    Bytes pixels;
    while (pos < data.size()) {
        const auto control = data[pos++];
        if (control < 128) {
            const std::size_t count = control + 1;
            if (count > data.size() - pos)
                return false;
            pixels.insert(pixels.end(), data.begin() + pos, data.begin() + pos + count);
            pos += count;
        } else {
            if (pos == data.size())
                return false;
            pixels.insert(pixels.end(), 257 - control, data[pos++]);
        }
    }
    const auto count = pixels.size() / frame_size;
    const unsigned master = data[header - 5];
    if (pixels.size() % frame_size || master >= count)
        return false;
    for (unsigned index = 0; index < count; ++index) {
        Frame frame{id, index, width, height, Bytes(frame_size * 4)};
        for (unsigned i = 0; i < frame_size; ++i) {
            const auto pixel = pixels[index * frame_size + i] ^
                (index == master ? 0 : pixels[master * frame_size + i]);
            const auto& color = palette[pixel];
            std::copy(color.begin(), color.end(), frame.rgba.begin() + i * 4);
        }
        out.push_back(std::move(frame));
    }
    return true;
}

int dos_delay(unsigned raw, std::uint64_t& tick)
{
    constexpr std::uint64_t numerator = 54925, denominator = 10000;
    const auto target = tick * numerator / denominator + 10ull * raw;
    const auto next = std::max<std::uint64_t>(tick + 1,
        (target * denominator + numerator - 1) / numerator);
    const auto scaled = (next - tick) * numerator;
    tick = next;
    const auto whole = scaled / 1000;
    const auto remainder = scaled % 1000;
    const auto rounded = whole + (remainder > 500 || (remainder == 500 && (whole & 1)));
    return static_cast<int>(std::min<std::uint64_t>(rounded, std::numeric_limits<int>::max()));
}

bool decode_ega_animation(const Bytes& data, unsigned id, PictureArchive& archive)
{
    if (data.empty() || !data[0] || data[0] > 8)
        return false;
    std::vector<Frame> frames;
    PictureArchive::Clip clip{id, {}};
    std::size_t position = 1, master = 0, master_size = 0;
    std::uint64_t tick = 0;
    for (unsigned index = 0; index < data[0]; ++index) {
        if (data.size() - position < 21)
            return false;
        const unsigned delay = word(data, position) | (word(data, position + 2) << 16);
        const unsigned height = word(data, position + 4);
        const unsigned width = word(data, position + 6) * 8;
        const unsigned x = word(data, position + 8) * 8;
        const unsigned y = word(data, position + 10) * 8;
        position += 21;
        if (!width || width > 320 || !height || height > 200 || x > 320 || y > 200)
            return false;
        const auto size = width * height / 2;
        if (size > data.size() - position || (index && size > master_size))
            return false;
        if (!index) {
            master = position;
            master_size = size;
        }
        Frame frame{id, index, x + width, y + height, {}};
        frame.rgba.resize(frame.width * frame.height * 4);
        for (unsigned row = 0; row < height; ++row) {
            for (unsigned col = 0; col < width; ++col) {
                const auto at = (row * width + col) / 2;
                const auto packed = data[position + at] ^ (index ? data[master + at] : 0);
                const auto& color = ega[(packed >> (col % 2 ? 0 : 4)) & 15];
                std::copy(color.begin(), color.end(),
                          frame.rgba.begin() + 4 * ((row + y) * frame.width + col + x));
            }
        }
        clip.steps.push_back({archive.frames.size() + frames.size(), dos_delay(delay, tick)});
        frames.push_back(std::move(frame));
        position += size;
    }
    if (position != data.size())
        return false;
    if (clip.steps.size() > 1)
        archive.clips.push_back(std::move(clip));
    archive.frames.insert(archive.frames.end(), std::make_move_iterator(frames.begin()),
                          std::make_move_iterator(frames.end()));
    return true;
}

void add_vga_clip(const Bytes& data, unsigned id, std::size_t first, PictureArchive& archive)
{
    const unsigned count = data[8] + 1;
    const unsigned table = 129 + data[10] * 3;
    if (count != archive.frames.size() - first || count > 9 || table + count * 3 > data.size())
        return;
    PictureArchive::Clip clip{id, {}};
    std::uint64_t tick = 0;
    for (unsigned i = data[table - 1]; i < count; ++i) {
        const unsigned delay = data[table + i * 3 + 2];
        clip.steps.push_back({first + i, delay == 99 ? 0 : dos_delay(delay, tick)});
        if (delay == 99)
            break;
    }
    if (clip.steps.size() > 1)
        archive.clips.push_back(std::move(clip));
}
}

bool is_picture_archive(const std::filesystem::path& path)
{
    static const std::regex name(R"(PIC[0-9]*[A-Z]?\.DAX)", std::regex::icase);
    return std::regex_match(path.filename().string(), name);
}

std::vector<DaxRecord> read_dax(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Unable to open the archive");
    const Bytes file{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad() || file.size() < 2)
        throw std::runtime_error("Unable to read the DAX header");
    const std::size_t table_size = word(file, 0);
    const auto start = 2 + table_size;
    if (table_size % 9 || start > file.size())
        throw std::runtime_error("Invalid DAX directory");

    std::vector<DaxRecord> records;
    for (std::size_t entry = 2; entry < start; entry += 9) {
        const std::size_t offset = word(file, entry + 1) | (word(file, entry + 3) << 16);
        const auto raw = word(file, entry + 5);
        const auto packed = word(file, entry + 7);
        if (offset > file.size() - start || packed > file.size() - start - offset)
            throw std::runtime_error("DAX directory entry points outside the archive");
        records.push_back({file[entry], packed,
                           packed ? unpack(file, start + offset, packed, raw) : Bytes{}});
    }
    return records;
}

PictureArchive read_pictures(const std::filesystem::path& path)
{
    PictureArchive archive;
    for (const auto& record : read_dax(path)) {
        if (!record.stored_size)
            continue;
        const auto& block = record.bytes;
        if (decode_ega_animation(block, record.id, archive))
            continue;
        const auto first = archive.frames.size();
        if (decode_vga_sprite(block, record.id, archive.frames)) {
            add_vga_clip(block, record.id, first, archive);
            continue;
        }
        if (!decode_picture(block, record.id, 5, archive.frames) &&
            !decode_picture(block, record.id, 0, archive.frames))
            archive.skipped_ids.push_back(record.id);
    }
    if (archive.frames.empty())
        throw std::runtime_error("The archive contains no supported picture frames");
    return archive;
}

std::vector<Frame> decode_picture_record(const Bytes& bytes, unsigned id)
{
    std::vector<Frame> frames;
    if (!decode_vga_sprite(bytes, id, frames) && !decode_picture(bytes, id, 5, frames))
        decode_picture(bytes, id, 0, frames);
    return frames;
}
}
