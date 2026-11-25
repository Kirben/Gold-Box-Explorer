#pragma once

#include "pictures.h"
#include <optional>

namespace goldbox {
bool is_amiga_ilbm(const std::filesystem::path& path);
PictureArchive read_amiga_ilbm(const std::filesystem::path& path);
std::optional<PictureArchive> try_amiga_pictures(const std::filesystem::path& path);
}
