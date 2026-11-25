#pragma once

#include "pictures.h"

namespace goldbox {
bool is_cmp_archive(const std::filesystem::path& path);
PictureArchive read_cmp(const std::filesystem::path& path);
}
