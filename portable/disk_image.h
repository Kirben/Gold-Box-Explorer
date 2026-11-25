#pragma once

#include <filesystem>

namespace goldbox {
bool is_disk_image(const std::filesystem::path& path);

// Writes the contents into a new, empty private directory. The caller owns cleanup.
void extract_disk_image(const std::filesystem::path& image, const std::filesystem::path& destination);
}
