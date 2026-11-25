#pragma once

#include "geo.h"
#include "pictures.h"

#include <map>

namespace goldbox {
bool is_wall_archive(const std::filesystem::path& path);
std::vector<Frame> read_wall_pieces(const std::filesystem::path& path);

// The three banks are laid out in the same order as the map's wall types.
using WallBanks = std::array<std::vector<Frame>, 3>;
WallBanks load_map_walls(const std::filesystem::path& geo_path, unsigned map_id);
Frame wall_perspective(const GeoMap& map, const WallBanks& banks, int x, int y, int direction);
}
