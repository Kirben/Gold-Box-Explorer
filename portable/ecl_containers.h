#pragma once

#include "dax.h"

namespace goldbox {
// An empty result means the file did not have an Amiga DAX directory.
std::vector<DaxRecord> try_amiga_dax(const std::vector<std::uint8_t>& file);
std::vector<DaxRecord> try_amiga_dax(const std::filesystem::path& path);
std::vector<DaxRecord> try_amiga_glb(const std::vector<std::uint8_t>& file);
}
