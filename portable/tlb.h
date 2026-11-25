#pragma once

#include "pictures.h"

namespace goldbox {
bool is_tlb_archive(const std::filesystem::path& path);
PictureArchive read_tlb(const std::filesystem::path& path);
}
