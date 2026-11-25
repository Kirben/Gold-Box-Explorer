#pragma once

#include "pictures.h"
#include <optional>

namespace goldbox {
std::optional<PictureArchive> try_amiga_pictures(const std::filesystem::path& path);
}
