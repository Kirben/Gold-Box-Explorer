#pragma once

#include "dax.h"
#include "pictures.h"

#include <optional>
#include <string>
#include <string_view>

namespace goldbox {
std::optional<std::vector<std::uint8_t>> expand_pc98(const std::uint8_t* packed, std::size_t length,
                                                     unsigned size);
std::string pc98_text(std::string_view text);
bool is_pc98_title(const std::filesystem::path& path);
PictureArchive read_pc98_title(const std::filesystem::path& path);
PictureArchive read_pc98_pictures(const std::filesystem::path& path, const std::vector<DaxRecord>& records);
std::vector<Frame> decode_pc98_wall_tiles(const DaxRecord& record);
}
