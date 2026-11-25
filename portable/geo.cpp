#include "geo.h"

#include <algorithm>
#include <regex>
#include <stdexcept>

namespace goldbox {
bool is_geo_archive(const std::filesystem::path& path)
{
    static const std::regex name(R"(GEO[0-9]*\.DAX)", std::regex::icase);
    return std::regex_match(path.filename().string(), name);
}

bool has_geo_trailer(const std::vector<DaxRecord>& records)
{
    return std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 255 && record.bytes.size() == 5;
    }) && std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 253 && !record.bytes.empty() && record.bytes.size() % 9 == 0;
    });
}

GeoMap read_geo_map(const DaxRecord& record)
{
    constexpr auto plane_size = map_side * map_side;
    if (record.bytes.size() < 2 + plane_size * 4)
        throw std::runtime_error("GEO record is too short for a 16x16 map");

    GeoMap map{};
    for (std::size_t i = 0; i < map.size(); ++i) {
        const unsigned ne = record.bytes[2 + i];
        const unsigned sw = record.bytes[2 + plane_size + i];
        const unsigned event = record.bytes[2 + 2 * plane_size + i];
        const unsigned door = record.bytes[2 + 3 * plane_size + i];
        map[i] = {{ne >> 4, ne & 15, sw >> 4, sw & 15},
                  {door & 3, (door >> 2) & 3, (door >> 4) & 3, door >> 6},
                  event & 127, (event & 128) != 0};
    }
    return map;
}
}
