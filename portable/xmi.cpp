#include "xmi.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Chunk = std::pair<std::size_t, std::size_t>;
constexpr unsigned vlq_limit = 0x0fffffff;

void find_sequences(const Bytes& bytes, std::size_t at, std::size_t end,
                    unsigned depth, bool xmid, std::vector<Chunk>& sequences)
{
    if (depth > 8)
        throw std::runtime_error("XMIDI container nesting is too deep");
    const auto tag = [&](std::size_t pos) {
        return std::string_view(reinterpret_cast<const char*>(bytes.data() + pos), 4);
    };
    while (at < end) {
        if (end - at < 8)
            throw std::runtime_error("Truncated XMIDI chunk header");
        const auto id = tag(at);
        std::size_t size = 0;
        for (unsigned i = 4; i < 8; ++i)
            size = size * 256 + bytes[at + i];
        at += 8;
        if (size > end - at)
            throw std::runtime_error("XMIDI chunk exceeds its container");
        const auto next = at + size;
        if ((id == "FORM" || id == "CAT " || id == "LIST") && size >= 4 && tag(at) == "XMID")
            find_sequences(bytes, at + 4, next, depth + 1, true, sequences);
        else if (xmid && id == "EVNT")
            sequences.emplace_back(at, next);
        at = next + (next < end ? (size & 1) : 0);
    }
}

struct Archive {
    Bytes bytes;
    std::vector<Chunk> sequences;
};

Archive read_xmi(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open XMIDI file");
    Archive archive{Bytes(std::istreambuf_iterator<char>(stream), {}), {}};
    if (archive.bytes.size() < 12 ||
        !std::equal(archive.bytes.begin(), archive.bytes.begin() + 4, "FORM"))
        throw std::runtime_error("Expected a FORM XMIDI file");
    find_sequences(archive.bytes, 0, archive.bytes.size(), 0, false, archive.sequences);
    if (archive.sequences.empty())
        throw std::runtime_error("No XMIDI event sequences found");
    return archive;
}

void append_unsigned(Bytes& bytes, std::uint32_t value, unsigned count)
{
    for (unsigned i = count; i > 0; --i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * (i - 1))));
}

void append_vlq(Bytes& bytes, std::uint64_t value)
{
    if (value > vlq_limit)
        throw std::runtime_error("MIDI delta exceeds the variable-length quantity limit");
    unsigned shift = 0;
    while ((value >> shift) >= 128)
        shift += 7;
    while (shift) {
        bytes.push_back(static_cast<std::uint8_t>(((value >> shift) & 127) | 128));
        shift -= 7;
    }
    bytes.push_back(static_cast<std::uint8_t>(value & 127));
}

Bytes convert_sequence(const Bytes& source, Chunk chunk)
{
    auto [at, end] = chunk;
    const auto read = [&]() {
        if (at == end)
            throw std::runtime_error("Truncated XMIDI event");
        return source[at++];
    };
    const auto read_data = [&]() {
        const auto value = read();
        if (value >= 128)
            throw std::runtime_error("Invalid XMIDI channel event operand");
        return value;
    };
    const auto read_vlq = [&]() {
        unsigned value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const auto byte = read();
            value = value * 128 + (byte & 127);
            if (!(byte & 128))
                return value;
        }
        throw std::runtime_error("Invalid XMIDI variable-length quantity");
    };
    struct Event {
        std::uint64_t tick;
        Bytes bytes;
    };
    std::vector<Event> events;
    std::uint64_t tick = 0;
    while (at < end) {
        // XMIDI delays add the literal bytes before the next status byte.
        while (at < end && source[at] < 128)
            tick += source[at++];
        const auto status = read();
        if (status < 0xf0) {
            const auto kind = status >> 4;
            const auto first = read_data();
            Bytes message{status, first};
            if (kind != 0xc && kind != 0xd)
                message.push_back(read_data());
            if (kind == 9) {
                const auto duration = read_vlq();
                if (message[2])
                    events.push_back({tick + std::max(1u, duration),
                                      {static_cast<std::uint8_t>(0x80 | (status & 15)), first, 0}});
            }
            events.push_back({tick, std::move(message)});
            continue;
        }
        if (status != 0xff && status != 0xf0 && status != 0xf7)
            throw std::runtime_error("Unsupported XMIDI system event");
        Bytes message{status};
        const auto type = status == 0xff ? read() : 0;
        if (status == 0xff)
            message.push_back(static_cast<std::uint8_t>(type));
        const auto length = read_vlq();
        if (length > end - at)
            throw std::runtime_error("XMIDI event payload exceeds its chunk");
        if (status == 0xff && type == 0x2f)
            break;
        // The AIL XMIDI scheduler runs at 120 Hz. SMF uses 60 PPQN and the
        // default 500,000 us tempo, so source ticks carry across directly.
        if (status != 0xff || type != 0x51) {
            append_vlq(message, length);
            message.insert(message.end(), source.begin() + at, source.begin() + at + length);
            events.push_back({tick, std::move(message)});
        }
        at += length;
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.tick < b.tick;
    });
    Bytes track;
    std::uint64_t previous = 0;
    for (const auto& event : events) {
        append_vlq(track, event.tick - previous);
        track.insert(track.end(), event.bytes.begin(), event.bytes.end());
        previous = event.tick;
    }
    append_vlq(track, std::max(tick, previous) - previous);
    track.insert(track.end(), {0xff, 0x2f, 0});
    if (track.size() > 0xffffffffu)
        throw std::runtime_error("MIDI track is too large");
    Bytes file{'M','T','h','d',0,0,0,6,0,0,0,1,0,60,'M','T','r','k'};
    append_unsigned(file, static_cast<std::uint32_t>(track.size()), 4);
    file.insert(file.end(), track.begin(), track.end());
    return file;
}
}

std::vector<RecordRow> read_xmi_rows(const std::filesystem::path& path)
{
    const auto archive = read_xmi(path);
    std::vector<RecordRow> rows;
    for (unsigned i = 0; i < archive.sequences.size(); ++i) {
        const auto [start, end] = archive.sequences[i];
        rows.push_back({i, 0, "Sequence " + std::to_string(i + 1), {
            {"Event bytes", std::to_string(end - start)}, {"Clock", "120 ticks per second"},
        }});
    }
    return rows;
}

std::vector<std::uint8_t> xmi_sequence_midi(const std::filesystem::path& path, unsigned index)
{
    const auto archive = read_xmi(path);
    if (index >= archive.sequences.size())
        throw std::runtime_error("XMIDI sequence index is out of range");
    return convert_sequence(archive.bytes, archive.sequences[index]);
}
}
