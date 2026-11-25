#pragma once

#include "dax.h"

namespace goldbox {
// An empty result means the file did not have an Amiga DAX directory.
std::vector<DaxRecord> try_amiga_ecl(const std::vector<std::uint8_t>& file);
std::vector<DaxRecord> try_amiga_glb(const std::vector<std::uint8_t>& file);
}
