#pragma once

#include "pictures.h"
#include "records.h"

namespace goldbox {
bool is_c64_picture(const std::filesystem::path& path);
PictureArchive read_c64_picture(const std::filesystem::path& path);
bool is_c64_record(const std::filesystem::path& path);
std::vector<RecordRow> read_c64_records(const std::filesystem::path& path);
}
