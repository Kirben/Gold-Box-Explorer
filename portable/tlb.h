#pragma once

#include "pictures.h"
#include "dax.h"

namespace goldbox {
bool is_tlb_archive(const std::filesystem::path& path);
PictureArchive read_tlb(const std::filesystem::path& path);
bool is_geo_glb(const std::filesystem::path& path);
PictureArchive read_geo_glb(const std::filesystem::path& path);
std::vector<DaxRecord> read_hlib_data(const std::filesystem::path& path);
}
