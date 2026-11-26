#pragma once

#include "records.h"
#include <iosfwd>
#include <string_view>

namespace goldbox {
bool is_ecl_archive(const std::filesystem::path& path);
std::vector<RecordRow> read_ecl_rows(const std::filesystem::path& path, std::string_view profile = {});
void write_ecl_listing(std::ostream& output, const RecordRow& script);
}
