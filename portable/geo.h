#pragma once

#include "dax.h"

#include <array>
#include <string_view>

namespace goldbox {
constexpr int map_side = 16;

struct GeoCell {
    std::array<unsigned, 4> walls;
    std::array<unsigned, 4> doors;
    unsigned event;
    bool event_flag;
};

using GeoMap = std::array<GeoCell, map_side * map_side>;

bool is_geo_archive(const std::filesystem::path& path);
bool has_geo_trailer(const std::vector<DaxRecord>& records);
std::string_view geo_map_name(const DaxRecord& record);
std::string_view geo_map_name(unsigned id, const std::uint8_t* planes, std::size_t size);
GeoMap read_geo_map(const DaxRecord& record);
}
