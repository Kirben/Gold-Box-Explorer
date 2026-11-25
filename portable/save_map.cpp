#include "save_map.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>

namespace goldbox {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

std::string upper(std::string name)
{
    for (auto& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

Bytes read_bytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return {std::istreambuf_iterator<char>(file), {}};
}

fs::path companion(const fs::path& folder, const std::string& name)
{
    const auto directory = folder.empty() ? fs::path(".") : folder;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (upper(it->path().filename().string()) == name && it->is_regular_file(file_error))
            return it->path();
    }
    return {};
}

std::optional<GeoMap> find_map(const fs::path& path, unsigned id)
{
    if (path.empty())
        return {};
    try {
        for (const auto& record : read_dax(path)) {
            if (record.id == id && record.bytes.size() == 1026)
                return read_geo_map(record);
        }
    } catch (const std::exception&) {
        // A stale or damaged companion should not prevent inspecting the slot.
    }
    return {};
}

std::optional<MapMask> explored(const Bytes& data, unsigned script, unsigned map_id)
{
    constexpr std::uint8_t signature[] = {4, 'E', 'X', 'P', 'L', 1};
    constexpr unsigned area_size = 2 + 2 * map_side;
    if (data.size() < 7 || !std::equal(std::begin(signature), std::end(signature), data.begin()) ||
        data[6] > 64 || data.size() < 7u + data[6] * area_size)
        return {};
    for (unsigned index = 0; index < data[6]; ++index) {
        const auto* area = data.data() + 7 + index * area_size;
        if (area[0] != script || area[1] != map_id)
            continue;
        MapMask seen;
        for (int y = 0; y < map_side; ++y) {
            const unsigned row = area[2 + y * 2] | (unsigned(area[3 + y * 2]) << 8);
            for (int x = 0; x < map_side; ++x)
                seen.set(y * map_side + x, (row & (1u << x)) != 0);
        }
        return seen;
    }
    return MapMask{};
}
}

bool is_saved_world(const std::filesystem::path& path)
{
    static const std::regex name(R"(SAVGAM[A-Z]\.DAT)", std::regex::icase);
    return std::regex_match(path.filename().string(), name);
}

SavedMap read_saved_map(const std::filesystem::path& path)
{
    const auto data = read_bytes(path);
    // SSI's DOS save stores the disk byte, three VM zones and the 7680-byte
    // script image before the eight location/party bytes and eight Pascal names.
    constexpr unsigned position = 1 + 2048 + 2048 + 1024 + 7680;
    constexpr unsigned map_address = 1 + 2 * (0x49c5 - 0x4900);
    constexpr unsigned script_address = 1 + 2 * (0x49f2 - 0x4900);
    constexpr unsigned disk_address = 1 + 2048 + 2 * (0x6e12 - 0x6b00);
    if (data.size() != position + 8 + 8 * 41 || data[0] < 1 || data[0] > 8)
        throw std::runtime_error("Unsupported Pool of Radiance DOS world save layout");
    const auto disk = data[disk_address] >= 1 && data[disk_address] <= 8
        ? data[disk_address] : data[0];
    SavedMap saved{disk, data[script_address], data[map_address], data[position + 5],
                   data[position], data[position + 1], (data[position + 2] & 6u) / 2u,
                   {}, {}, {}, {}};
    if (saved.location > 1)
        return saved;
    if (saved.x >= map_side || saved.y >= map_side)
        throw std::runtime_error("Saved party coordinates are outside the DOS map");

    const auto folder = fs::absolute(path).parent_path();
    const auto disk_geo = "GEO" + std::to_string(saved.disk) + ".DAX";
    saved.original = find_map(companion(folder.parent_path(), disk_geo), saved.map_id);
    if (!saved.original)
        saved.original = find_map(companion(folder.parent_path(), "GEO.DAX"), saved.map_id);
    saved.copied = find_map(companion(folder, disk_geo), saved.map_id);
    if (!saved.copied)
        saved.copied = find_map(companion(folder, "GEO.DAX"), saved.map_id);
    if (saved.original && saved.copied) {
        for (std::size_t i = 0; i < saved.changed.size(); ++i) {
            const auto& a = (*saved.original)[i];
            const auto& b = (*saved.copied)[i];
            saved.changed.set(i, a.walls != b.walls || a.doors != b.doors ||
                                 a.event != b.event || a.event_flag != b.event_flag);
        }
    }
    saved.visited = explored(read_bytes(companion(folder, upper(path.stem().string()) + ".MAP")),
                             saved.script, saved.map_id);
    return saved;
}
}
