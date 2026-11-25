#include "ecl_containers.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace goldbox {
namespace {
using Bytes = std::vector<std::uint8_t>;

unsigned be16(const Bytes& bytes, std::size_t at)
{
    return (unsigned(bytes.at(at)) << 8) | bytes.at(at + 1);
}

std::uint32_t be32(const Bytes& bytes, std::size_t at)
{
    return (std::uint32_t(be16(bytes, at)) << 16) | be16(bytes, at + 2);
}

// Backwards ByteKiller stream, as decoded by the legacy Explorer Amiga reader.
class ByteKiller {
public:
    ByteKiller(const Bytes& bytes, std::size_t start, unsigned packed, unsigned raw)
        : bytes_(bytes), start_(start), input_(start + packed), output_(raw), remaining_(raw)
    {
        if (word() != raw)
            throw std::runtime_error("Amiga DAX output size disagrees with its ByteKiller trailer");
        checksum_ = word();
        buffer_ = word();
        checksum_ ^= buffer_;
    }

    Bytes decode()
    {
        while (remaining_) {
            if (!bit()) {
                if (!bit())
                    literal(bits(3) + 1);
                else
                    repeat(2, bits(8));
            } else {
                switch (bits(2)) {
                case 0: repeat(3, bits(9)); break;
                case 1: repeat(4, bits(10)); break;
                case 2: {
                    const auto count = bits(8) + 1;
                    repeat(count, bits(12));
                    break;
                }
                case 3: literal(bits(8) + 9); break;
                }
            }
        }
        if (checksum_)
            throw std::runtime_error("Amiga DAX ByteKiller checksum mismatch");
        return std::move(output_);
    }

private:
    std::uint32_t word()
    {
        if (input_ - start_ < 4)
            throw std::runtime_error("Truncated ByteKiller data");
        input_ -= 4;
        return be32(bytes_, input_);
    }

    unsigned bit()
    {
        auto result = buffer_ & 1;
        buffer_ >>= 1;
        if (!buffer_) {
            buffer_ = word();
            checksum_ ^= buffer_;
            result = buffer_ & 1;
            buffer_ = (buffer_ >> 1) | 0x80000000u;
        }
        return result;
    }

    unsigned bits(unsigned count)
    {
        unsigned value = 0;
        while (count--)
            value = value * 2 + bit();
        return value;
    }

    void literal(unsigned count)
    {
        if (count > remaining_)
            throw std::runtime_error("ByteKiller literal exceeds output size");
        while (count--)
            output_[--remaining_] = static_cast<std::uint8_t>(bits(8));
    }

    void repeat(unsigned count, unsigned distance)
    {
        if (count > remaining_ || !distance || distance > output_.size() - remaining_)
            throw std::runtime_error("Invalid ByteKiller back-reference");
        while (count--) {
            --remaining_;
            output_[remaining_] = output_[remaining_ + distance];
        }
    }

    const Bytes& bytes_;
    std::size_t start_, input_;
    Bytes output_;
    std::size_t remaining_;
    std::uint32_t checksum_ = 0, buffer_ = 0;
};
}

std::vector<DaxRecord> try_amiga_ecl(const Bytes& file)
{
    if (file.size() < 12)
        return {};
    const auto size = be16(file, 0);
    const auto start = size + 2;
    if (!size || size % 10 || start > file.size())
        return {};
    std::size_t previous = start;
    std::set<unsigned> ids;
    for (unsigned at = 2; at < start; at += 10) {
        const auto id = be16(file, at);
        const auto offset = be32(file, at + 2);
        const auto packed = be16(file, at + 6);
        const auto raw = be16(file, at + 8);
        if (!ids.insert(id).second || offset != previous - start || packed < 12 ||
            packed % 4 || packed > file.size() - previous || !raw)
            return {};
        previous += packed;
    }
    if (previous != file.size())
        return {};
    std::vector<DaxRecord> records;
    for (unsigned at = 2; at < start; at += 10) {
        const auto packed = be16(file, at + 6);
        records.push_back({be16(file, at), packed,
                           ByteKiller(file, start + be32(file, at + 2), packed, be16(file, at + 8)).decode()});
    }
    return records;
}

std::vector<DaxRecord> try_amiga_glb(const Bytes& file)
{
    if (file.size() < 16 || !std::equal(file.begin(), file.begin() + 4, "GLIB"))
        return {};
    const auto count = be16(file, 8);
    const auto table_end = 16 + 4 * (count + 1);
    if (file.size() != be32(file, 4) || be16(file, 10) != 1 ||
        !std::equal(file.begin() + 12, file.begin() + 16, "DATA") ||
        !count || table_end > file.size())
        throw std::runtime_error("Invalid Amiga GLIB DATA header");
    std::vector<std::uint32_t> offsets;
    for (unsigned i = 0; i <= count; ++i) {
        const auto offset = be32(file, 16 + 4 * i);
        if (offset < (i ? offsets.back() : table_end) || offset > file.size())
            throw std::runtime_error("Invalid Amiga GLIB record offset");
        offsets.push_back(offset);
    }
    if (offsets[1] - offsets[0] < 2)
        throw std::runtime_error("Missing Amiga GLIB index");
    const auto entries = be16(file, offsets[0]);
    if (2 + 4 * entries > offsets[1] - offsets[0])
        throw std::runtime_error("Truncated Amiga GLIB index");
    std::vector<DaxRecord> records;
    for (unsigned i = 0; i < entries; ++i) {
        const auto at = offsets[0] + 2 + 4 * i;
        const auto index = be16(file, at + 2);
        if (!index || index >= count)
            throw std::runtime_error("Invalid Amiga GLIB record index");
        const auto first = offsets[index];
        const auto last = offsets[index + 1];
        records.push_back({be16(file, at), last - first, Bytes(file.begin() + first, file.begin() + last)});
    }
    return records;
}
}
