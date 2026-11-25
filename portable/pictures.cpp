#include "pictures.h"
#include "amiga_pictures.h"
#include "dax.h"
#include "pc98.h"

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
    const bool custom_palette = (flags & 0xcc) != 0;
    const auto planar_size = height * columns * 5;
    const bool standard_strata = !header && count && !custom_palette &&
        data.size() == pos + 8 + planar_size * count &&
        data.size() != pos + 8 + packed_size * (count + 1);
    const bool planar = standard_strata || (custom_palette &&
        (header || data.size() != pos + 8 + packed_size * count));
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
        pos += custom_palette ? 1 : 8;
        if (custom_palette && (flags & 1)) pos += 8;
        if (custom_palette && (flags & 2)) pos += 16;
        if (custom_palette && (flags & 8)) {
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
        if (!count || pos + planar_size * count + (header ? 14 * (count - 1) : 0) > data.size())
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

bool decode_vga_indexed(const Bytes& data, unsigned id, std::vector<Frame>& out)
{
    if (data.size() < 20)
        return false;
    const unsigned height = data[0];
    const unsigned width = data[1] * 8;
    const auto count = word(data, 6);
    const unsigned first_color = data[8];
    const unsigned colors = data[9] + 1;
    const auto frame_size = width * height;
    if (!width || width > 320 || !height || height > 200 || !count ||
        first_color + colors > 256 || count > data.size() / frame_size)
        return false;
    const auto start = data.size() - frame_size * count;
    const auto palette_end = 10 + colors * 3;
    if (start < palette_end ||
        !std::all_of(data.begin() + 10, data.begin() + palette_end, [](auto value) { return value < 64; }))
        return false;
    std::array<Color, 256> palette;
    for (unsigned i = 0; i < palette.size(); ++i)
        palette[i] = ega[i % ega.size()];
    for (unsigned i = 0; i < colors; ++i) {
        const auto* rgb = data.data() + 10 + i * 3;
        for (unsigned channel = 0; channel < 3; ++channel)
            palette[first_color + i][channel] = static_cast<std::uint8_t>(rgb[channel] * 4);
    }
    for (unsigned i = 0; i < count; ++i) {
        Frame frame{id, i, width, height, Bytes(frame_size * 4)};
        for (unsigned pixel = 0; pixel < frame_size; ++pixel) {
            const auto& color = palette[data[start + i * frame_size + pixel]];
            std::copy(color.begin(), color.end(), frame.rgba.data() + pixel * 4);
        }
        out.push_back(std::move(frame));
    }
    return true;
}

bool decode_mono_tiles(const Bytes& data, unsigned id, std::vector<Frame>& out)
{
    if (data.empty() || data.size() % 8)
        return false;
    for (unsigned i = 0; i < data.size() / 8; ++i) {
        Frame frame{id, i, 8, 8, Bytes(8 * 8 * 4)};
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                const auto& color = ega[data[i * 8 + y] & (0x80 >> x) ? 15 : 0];
                std::copy(color.begin(), color.end(), frame.rgba.data() + (y * 8 + x) * 4);
            }
        }
        out.push_back(std::move(frame));
    }
    return true;
}

bool unpack_vga_runs(const Bytes& data, std::size_t pos, std::size_t end,
                     std::size_t limit, Bytes& pixels)
{
    while (pos < end) {
        const auto control = data[pos++];
        const unsigned count = control < 128 ? control + 1 : 257 - control;
        if (count > limit - pixels.size())
            return false;
        if (control < 128) {
            if (count > end - pos)
                return false;
            pixels.insert(pixels.end(), data.begin() + pos, data.begin() + pos + count);
            pos += count;
        } else {
            if (pos == end)
                return false;
            pixels.insert(pixels.end(), count, data[pos++]);
        }
    }
    return true;
}

bool decode_vga_sprite(const Bytes& data, unsigned id, std::vector<Frame>& out, bool encounter = false)
{
    if (data.size() < 132 || data[1] || !data[0] || data[0] > 200 ||
        !data[2] || data[2] > 40 || !data[9] || !data[10] || data[9] + data[10] > 256)
        return false;
    const unsigned width = data[2] * 8;
    const unsigned height = data[0];
    const unsigned frame_size = width * height;
    // Treasures stores a nibble map and variable-sized frame directory after the palette.
    const bool extended = encounter && data[9] == 176 && data[10] == 80 && data[8] < 9;
    const unsigned header = extended ? 17 + 3 * data[10] + data[10] / 2 : 129 + 3 * data[10];
    const unsigned declared_count = data[8] + 1;
    std::size_t pos = header + 3 * declared_count;
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
    if (extended) {
        for (unsigned frame = 0; frame < declared_count; ++frame) {
            const auto length = word(data, header + 3 * frame);
            if (length > data.size() - pos ||
                !unpack_vga_runs(data, pos, pos + length, (frame + 1) * frame_size, pixels) ||
                pixels.size() != (frame + 1) * frame_size)
                return false;
            pos += length;
        }
        if (pos != data.size())
            return false;
    } else if (!unpack_vga_runs(data, pos, data.size(), 256 * frame_size, pixels)) {
        return false;
    }
    const auto count = pixels.size() / frame_size;
    if (!count)
        return false;
    const auto master = encounter && !extended ? count - 1 : data[header - 2];
    if (pixels.size() % frame_size || master >= count)
        return false;
    if (encounter) {
        palette[0] = {0, 0, 0, 0};
        palette[13] = ega[0];
    }
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

int dos_interval(std::uint64_t hundredths, std::uint64_t& tick)
{
    constexpr std::uint64_t numerator = 54925, denominator = 10000;
    const auto target = tick * numerator / denominator + hundredths;
    const auto next = std::max<std::uint64_t>(tick + 1,
        (target * denominator + numerator - 1) / numerator);
    const auto scaled = (next - tick) * numerator;
    tick = next;
    const auto whole = scaled / 1000;
    const auto remainder = scaled % 1000;
    const auto rounded = whole + (remainder > 500 || (remainder == 500 && (whole & 1)));
    return static_cast<int>(std::min<std::uint64_t>(rounded, std::numeric_limits<int>::max()));
}

int dos_delay(unsigned raw, std::uint64_t& tick)
{
    return dos_interval(10ull * raw, tick);
}

bool decode_ega_animation(const Bytes& data, unsigned id, PictureArchive& archive,
                          bool xor_frames, bool add_clip, bool encounter)
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
        if (size > data.size() - position)
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
                const auto packed = data[position + at] ^
                    (xor_frames && index && at < master_size ? data[master + at] : 0);
                const auto pixel = (packed >> (col % 2 ? 0 : 4)) & 15;
                if (encounter && pixel == 0)
                    continue;
                const auto& color = ega[encounter && pixel == 13 ? 0 : pixel];
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
    if (add_clip && clip.steps.size() > 1)
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

constexpr unsigned ending_size = 120;

// Draw into the game's ending window, whose top-left screen coordinate is (8, 8).
void draw_ending_layer(Frame& canvas, const Frame& layer, unsigned x, unsigned y, bool black_key = false)
{
    for (unsigned row = 0; row < layer.height && y + row < canvas.height; ++row) {
        for (unsigned col = 0; col < layer.width && x + col < canvas.width; ++col) {
            const auto* source = layer.rgba.data() + (row * layer.width + col) * 4;
            if (source[3] == 0 || (black_key && !(source[0] | source[1] | source[2])))
                continue;
            auto* target = canvas.rgba.data() + ((y + row) * canvas.width + x + col) * 4;
            std::copy_n(source, 4, target);
        }
    }
}

void add_final_clip(const std::vector<DaxRecord>& records, PictureArchive& archive)
{
    constexpr unsigned scene_ids[]{2, 7, 8};
    std::array<std::vector<std::size_t>, 9> blocks;
    for (std::size_t i = 0; i < archive.frames.size(); ++i) {
        const auto id = archive.frames[i].id;
        if (id > 0 && id < blocks.size())
            blocks[id].push_back(i);
    }
    if (std::any_of(blocks.begin() + 1, blocks.end(), [](const auto& block) { return block.empty(); }))
        return;
    for (const auto id : {1, 3}) {
        const auto& frame = archive.frames[blocks[id].front()];
        if (frame.width != ending_size || frame.height != ending_size)
            return;
    }
    std::array<std::vector<unsigned>, 3> delays;
    constexpr unsigned stride = 21 + 88 * 11 * 4;
    for (unsigned scene = 0; scene < delays.size(); ++scene) {
        const auto id = scene_ids[scene];
        const auto& block = blocks[id];
        const auto record = std::find_if(records.begin(), records.end(), [id](const auto& r) { return r.id == id; });
        if (block.size() < 2 || record == records.end() || record->bytes.size() != 1 + stride * block.size() ||
            record->bytes[0] != block.size())
            return;
        for (std::size_t i = 0; i < block.size(); ++i) {
            const auto& frame = archive.frames[block[i]];
            if (frame.width != 88 || frame.height != 88)
                return;
            const auto offset = 1 + stride * i;
            delays[scene].push_back(word(record->bytes, offset) | (word(record->bytes, offset + 2) << 16));
        }
    }

    const auto gallery_count = archive.frames.size();
    PictureArchive::Clip clip{0, {}, true};
    std::uint64_t bios_tick = 0;
    for (unsigned scene = 0; scene < delays.size(); ++scene) {
        const auto id = scene_ids[scene];
        const auto& block = blocks[id];
        const unsigned top = scene == 0 ? 32 : 0;
        // WinGame uses (10 - gameDelay) * 5 div 2 passes for the first two scenes.
        constexpr unsigned default_game_delay = 1;
        const unsigned passes = scene == 2 ? 1 : (10 - default_game_delay) * 5 / 2;
        Frame canvas{id, 0, ending_size, ending_size, Bytes(ending_size * ending_size * 4)};
        draw_ending_layer(canvas, archive.frames[blocks[scene == 0 ? 1 : 3].front()], 0, 0);
        if (scene != 0) {
            draw_ending_layer(canvas, archive.frames[blocks[4].front()], 0, 88);
            draw_ending_layer(canvas, archive.frames[blocks[5].front()], 104, 72);
            draw_ending_layer(canvas, archive.frames[blocks[6].front()], 0, 72);
        }
        draw_ending_layer(canvas, archive.frames[block.front()], 16, top);
        std::vector<std::size_t> loop(block.size());
        for (unsigned pass = 0; pass < passes; ++pass) {
            for (std::size_t i = 0; i < block.size(); ++i) {
                if (pass < 2) {
                    draw_ending_layer(canvas, archive.frames[block[i]], 16, top, true);
                    loop[i] = archive.frames.size();
                    canvas.index = static_cast<unsigned>(pass * block.size() + i);
                    archive.frames.push_back(canvas);
                }
                // PlayFinalAnim advances only after the elapsed centiseconds exceed this delay.
                const auto delay = (default_game_delay + 3ull) * delays[scene][i] + 1;
                clip.steps.push_back({loop[i], dos_interval(delay, bios_tick)});
            }
        }
    }
    clip.steps.push_back({clip.steps.back().frame, 3 * 225});
    archive.playback_only_frames = archive.frames.size() - gallery_count;
    archive.clips.push_back(std::move(clip));
}
}

bool is_picture_archive(const std::filesystem::path& path)
{
    if (is_pc98_title(path) || is_amiga_ilbm(path) || is_amiga_intro(path))
        return true;
    static const std::regex daa(
        R"((8X8D|BACK|BIGPIC|BODY|BORDERS?|COMSPR|CPIC|DUNGCOM|HEAD|HITEK|LOTEK|MARSCOM|PIC|RANDCOM|SHIPS|SKY|SPACETVL|SPRIT|VENUSCOM|WILDCOM)[0-9]*\.DAA)",
        std::regex::icase);
    static const std::regex name(
        R"(((PIC|SPRIT)[0-9]*[A-Z]?|(8X8D|BACK|BACPAC|BIGPIC|BODY|BORD(ERS?)?|CBODY|CHEAD|COMSPR|CPIC|CURSOR|DUNGCOM|FINAL|HEAD|ICON|OVER|RANDCOM|SKY|SKYGRND|SQRPACI|TILES|TITLE|WILDCOM)[0-9]*)\.DAX)",
        std::regex::icase);
    const auto file = path.filename().string();
    return std::regex_match(file, name) || std::regex_match(file, daa);
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

    struct Entry {
        unsigned id, raw, packed;
        std::size_t offset;
    };
    std::vector<Entry> directory;
    bool pc98 = true, saw_type5 = false;
    for (std::size_t entry = 2; entry < start; entry += 9) {
        const std::size_t offset = word(file, entry + 1) | (word(file, entry + 3) << 16);
        const auto raw = word(file, entry + 5);
        const auto packed = word(file, entry + 7);
        if (offset > file.size() - start || packed > file.size() - start - offset)
            throw std::runtime_error("DAX directory entry points outside the archive");
        directory.push_back({file[entry], raw, packed, start + offset});
        pc98 &= packed && (file[start + offset] == 5 || file[start + offset] == 0xff);
        saw_type5 |= packed && file[start + offset] == 5;
    }
    std::vector<DaxRecord> records;
    records.reserve(directory.size());
    if (pc98 && saw_type5) {
        for (const auto& entry : directory) {
            auto raw = expand_pc98(file.data() + entry.offset, entry.packed, entry.raw);
            if (!raw)
                break;
            records.push_back({entry.id, entry.packed, std::move(*raw), true});
        }
        if (records.size() == directory.size())
            return records;
        records.clear();
    }
    for (const auto& entry : directory)
        records.push_back({entry.id, entry.packed,
            entry.packed ? unpack(file, entry.offset, entry.packed, entry.raw) : Bytes{}});
    return records;
}

PictureArchive read_pictures(const std::filesystem::path& path)
{
    if (is_amiga_intro(path))
        return read_amiga_intro(path);
    if (is_amiga_ilbm(path))
        return read_amiga_ilbm(path);
    if (is_pc98_title(path))
        return read_pc98_title(path);
    if (auto amiga = try_amiga_pictures(path))
        return std::move(*amiga);
    PictureArchive archive;
    const auto filename = path.filename().string();
    static const std::regex animated_pictures(R"(PIC[0-9]*[A-Z]?\.DAX)", std::regex::icase);
    static const std::regex endings(R"(FINAL[0-9]*\.DAX)", std::regex::icase);
    static const std::regex sprites(R"(SPRIT[0-9]*[A-Z]?\.DAX)", std::regex::icase);
    static const std::regex tiles(R"(8X8D[0-9]*\.DAX)", std::regex::icase);
    const bool play_pictures = std::regex_match(filename, animated_pictures);
    const bool ending = std::regex_match(filename, endings);
    const bool xor_frames = play_pictures || ending;
    const bool encounter = std::regex_match(filename, sprites);
    const bool mono_tiles = std::regex_match(filename, tiles);
    const auto records = read_dax(path);
    if (!records.empty() && records.front().pc98)
        return read_pc98_pictures(path, records);
    for (const auto& record : records) {
        if (!record.stored_size)
            continue;
        const auto& block = record.bytes;
        if (decode_ega_animation(block, record.id, archive, xor_frames, play_pictures, encounter))
            continue;
        const auto first = archive.frames.size();
        if (decode_vga_sprite(block, record.id, archive.frames, encounter)) {
            if (play_pictures)
                add_vga_clip(block, record.id, first, archive);
            continue;
        }
        if (!decode_picture(block, record.id, 5, archive.frames) &&
            !decode_vga_indexed(block, record.id, archive.frames) &&
            !decode_picture(block, record.id, 0, archive.frames) &&
            !(mono_tiles && decode_mono_tiles(block, record.id, archive.frames)))
            archive.skipped_ids.push_back(record.id);
    }
    if (archive.frames.empty())
        throw std::runtime_error("The archive contains no supported picture frames");
    if (ending)
        add_final_clip(records, archive);
    return archive;
}

std::vector<Frame> decode_picture_record(const Bytes& bytes, unsigned id)
{
    std::vector<Frame> frames;
    if (!decode_vga_sprite(bytes, id, frames) && !decode_picture(bytes, id, 5, frames) &&
        !decode_vga_indexed(bytes, id, frames))
        decode_picture(bytes, id, 0, frames);
    return frames;
}
}
