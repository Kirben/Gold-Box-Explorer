#pragma once

#include "pictures.h"

namespace goldbox {
bool is_vace_archive(const std::filesystem::path& path);
PictureArchive read_vace(const std::filesystem::path& path);
}
