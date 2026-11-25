#pragma once

#include "pictures.h"

namespace goldbox {
bool is_cmp_archive(const std::filesystem::path& path);
PictureArchive read_cmp(const std::filesystem::path& path);
bool is_anm_archive(const std::filesystem::path& path);
PictureArchive read_anm(const std::filesystem::path& path);
bool is_packwall_archive(const std::filesystem::path& path);
PictureArchive read_packwall(const std::filesystem::path& path);
}
