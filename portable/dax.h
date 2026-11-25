#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace goldbox {
struct DaxRecord {
    unsigned id;
    unsigned stored_size;
    std::vector<std::uint8_t> bytes;
    bool pc98 = false;
};

std::vector<DaxRecord> read_dax(const std::filesystem::path& path);
}
