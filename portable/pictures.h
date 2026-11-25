#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace goldbox {
struct Frame {
    unsigned id;
    unsigned index;
    unsigned width;
    unsigned height;
    std::vector<std::uint8_t> rgba;
};

struct PictureArchive {
    std::vector<Frame> frames;
    std::vector<unsigned> skipped_ids;
    struct Step {
        std::size_t frame;
        int duration_ms;
    };
    struct Clip {
        unsigned id;
        std::vector<Step> steps;
    };
    std::vector<Clip> clips;
};

bool is_picture_archive(const std::filesystem::path& path);
PictureArchive read_pictures(const std::filesystem::path& path);
std::vector<Frame> decode_picture_record(const std::vector<std::uint8_t>& bytes, unsigned id);
}
