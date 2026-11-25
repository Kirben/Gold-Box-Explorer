#pragma once

#include "records.h"

namespace goldbox {
std::vector<RecordRow> read_xmi_rows(const std::filesystem::path& path);
std::vector<std::uint8_t> xmi_sequence_midi(const std::filesystem::path& path, unsigned index);
}
