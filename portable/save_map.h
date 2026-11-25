#pragma once

#include "geo.h"

#include <bitset>
#include <optional>

namespace goldbox {
using MapMask = std::bitset<map_side * map_side>;

struct SavedMap {
    unsigned disk;
    unsigned script;
    unsigned map_id;
    unsigned location;
    unsigned x;
    unsigned y;
    unsigned facing;
    std::optional<GeoMap> original;
    std::optional<GeoMap> copied;
    std::optional<MapMask> visited;
    MapMask changed;
};

bool is_saved_world(const std::filesystem::path& path);
SavedMap read_saved_map(const std::filesystem::path& path);
}
