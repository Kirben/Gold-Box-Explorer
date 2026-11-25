#include "disk_image.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace goldbox {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

std::string upper(std::string text)
{
    for (auto& c : text)
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
    return text;
}

std::string host_name(const std::uint8_t* data, std::size_t size, bool petscii)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        auto c = data[i];
        if (petscii && c >= 0xc1 && c <= 0xda)
            c -= 0x80;
        if (c < 32 || c >= 127 || std::string_view("<>:\"/\\|?*%").find(c) != std::string_view::npos ||
            (i + 1 == size && (c == '.' || c == ' '))) {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    if (result.empty())
        result = "%00";
    const auto stem = upper(result.substr(0, result.find('.')));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT") &&
         stem[3] >= '1' && stem[3] <= '9'))
        result.insert(0, "%5F");
    return result;
}

fs::path unique_path(const fs::path& directory, std::string name, std::set<std::string>& used)
{
    auto candidate = fs::path(name);
    for (unsigned i = 2; !used.insert(upper(candidate.string())).second; ++i)
        candidate = fs::path(name).stem().string() + "~" + std::to_string(i) + fs::path(name).extension().string();
    return directory / candidate;
}

void write_file(const fs::path& path, const Bytes& bytes)
{
    std::ofstream stream(path, std::ios::binary);
    if (!bytes.empty())
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.close();
    if (!stream)
        throw std::runtime_error("Could not cache an extracted disk file");
}

constexpr unsigned d64_size = 174848;

unsigned d64_offset(unsigned track, unsigned sector)
{
    const auto sectors = [](unsigned t) { return t <= 17 ? 21u : t <= 24 ? 19u : t <= 30 ? 18u : 17u; };
    if (!track || track > 35 || sector >= sectors(track))
        throw std::runtime_error("Invalid D64 track or sector");
    unsigned block = sector;
    for (unsigned t = 1; t < track; ++t)
        block += sectors(t);
    return 256 * block;
}

void extract_d64(const Bytes& image, const fs::path& destination)
{
    if (image.size() != d64_size && image.size() != d64_size + 683)
        throw std::runtime_error("Expected a standard 35-track D64 image");
    std::set<unsigned> directory_chain;
    std::set<std::string> names;
    unsigned track = 18, sector = 1;
    while (track) {
        const auto offset = d64_offset(track, sector);
        if (!directory_chain.insert(offset).second)
            throw std::runtime_error("Circular D64 directory");
        const auto* directory = image.data() + offset;
        for (unsigned i = 0; i < 8; ++i) {
            const auto* entry = directory + 32 * i;
            const auto type = entry[2] & 7;
            if (type < 1 || type > 3 || !entry[3])
                continue;
            auto length = 16u;
            while (length && entry[4 + length] == 0xa0)
                --length;
            auto name = host_name(entry + 5, length, true);
            if (type == 2 && upper(fs::path(name).extension().string()) != ".PRG")
                name += ".PRG";
            Bytes content;
            std::set<unsigned> chain;
            unsigned file_track = entry[3], file_sector = entry[4];
            while (file_track) {
                const auto block = d64_offset(file_track, file_sector);
                if (!chain.insert(block).second)
                    throw std::runtime_error("Circular D64 file chain");
                const auto* data = image.data() + block;
                file_track = data[0];
                file_sector = data[1];
                if (!file_track && !file_sector)
                    throw std::runtime_error("Invalid D64 final sector length");
                const auto count = file_track ? 254 : file_sector - 1;
                content.insert(content.end(), data + 2, data + 2 + count);
            }
            write_file(unique_path(destination, name, names), content);
        }
        track = directory[0];
        sector = directory[1];
    }
}

std::uint32_t be32(const std::uint8_t* data, unsigned at)
{
    return std::uint32_t(data[at]) << 24 | std::uint32_t(data[at + 1]) << 16 |
        std::uint32_t(data[at + 2]) << 8 | data[at + 3];
}

class Adf {
public:
    explicit Adf(const Bytes& image) : image_(image) {}

    void extract(const fs::path& destination)
    {
        if ((image_.size() != 901120 && image_.size() != 1802240) ||
            image_[0] != 'D' || image_[1] != 'O' || image_[2] != 'S' || image_[3] > 5)
            throw std::runtime_error("Expected a standard AmigaDOS OFS or FFS ADF image");
        fast_ = image_[3] & 1;
        const auto stored_root = be32(image_.data(), 8);
        const auto root = stored_root >= 2 && stored_root < image_.size() / 512 &&
            be32(block(stored_root), 508) == 1 ? stored_root : image_.size() / 1024;
        if (be32(block(root), 508) != 1)
            throw std::runtime_error("Invalid ADF root block");
        directory(root, destination, 0);
    }

private:
    const std::uint8_t* block(unsigned index) const
    {
        if (index < 2 || index >= image_.size() / 512)
            throw std::runtime_error("Invalid ADF block pointer");
        return image_.data() + index * 512;
    }

    void directory(unsigned index, const fs::path& path, unsigned depth)
    {
        if (depth >= 64 || be32(block(index), 0) != 2)
            throw std::runtime_error("Invalid ADF directory block");
        const auto* header = block(index);
        std::set<std::string> names;
        for (unsigned slot = 0; slot < 72; ++slot) {
            auto child = be32(header, 24 + slot * 4);
            while (child) {
                if (!entries_.insert(child).second)
                    throw std::runtime_error("Circular or repeated ADF directory entry");
                const auto* entry = block(child);
                const auto type = be32(entry, 508);
                const auto name_size = entry[432];
                if (name_size > 30 || be32(entry, 0) != 2)
                    throw std::runtime_error("Invalid ADF directory entry");
                if (type == 2 || type == 0xfffffffdu) {
                    const auto child_path = unique_path(path, host_name(entry + 433, name_size, false), names);
                    if (type == 2) {
                        fs::create_directory(child_path);
                        directory(child, child_path, depth + 1);
                    } else {
                        write_file(child_path, file(child));
                    }
                }
                child = be32(entry, 496);
            }
        }
    }

    Bytes file(unsigned index) const
    {
        const auto length = be32(block(index), 324);
        if (length > image_.size())
            throw std::runtime_error("Invalid ADF file length");
        Bytes content;
        content.reserve(length);
        std::set<unsigned> chain;
        while (content.size() < length) {
            if (!chain.insert(index).second)
                throw std::runtime_error("Circular ADF file extension chain");
            const auto* header = block(index);
            for (unsigned i = 0; i < 72 && content.size() < length; ++i) {
                const auto* data = block(be32(header, 308 - 4 * i));
                const auto remaining = length - content.size();
                const auto count = fast_ ? std::min<std::size_t>(512, remaining) : be32(data, 12);
                if (!count || count > remaining || (!fast_ && (count > 488 || be32(data, 0) != 8)))
                    throw std::runtime_error("Invalid ADF file data block");
                const auto start = fast_ ? 0 : 24;
                content.insert(content.end(), data + start, data + start + count);
            }
            if (content.size() < length) {
                index = be32(header, 504);
                if (be32(block(index), 0) != 16)
                    throw std::runtime_error("Invalid ADF file extension block");
            }
        }
        return content;
    }

    const Bytes& image_;
    bool fast_ = false;
    std::set<unsigned> entries_;
};
}

bool is_disk_image(const fs::path& path)
{
    const auto extension = upper(path.extension().string());
    return extension == ".ADF" || extension == ".D64";
}

void extract_disk_image(const fs::path& image, const fs::path& destination)
{
    if (!is_disk_image(image) || !fs::is_directory(destination) || !fs::is_empty(destination))
        throw std::runtime_error("A disk image and an empty cache directory are required");
    const auto size = fs::file_size(image);
    if (size > 1802240)
        throw std::runtime_error("Unsupported disk image size");
    Bytes bytes(static_cast<std::size_t>(size));
    std::ifstream stream(image, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Could not read the disk image");
    if (upper(image.extension().string()) == ".D64")
        extract_d64(bytes, destination);
    else
        Adf(bytes).extract(destination);
}
}
