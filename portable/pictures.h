#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
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
        bool whole_archive = false;
        bool realtime = false;
        std::string label;
    };
    std::vector<Clip> clips;
    std::size_t playback_only_frames = 0; // Synthetic frames appended after the gallery frames.
    std::vector<unsigned> partial_ids;
    std::vector<std::string> frame_labels;
    std::size_t gallery_count() const { return frames.size() - playback_only_frames; }
};

bool is_picture_archive(const std::filesystem::path& path);
PictureArchive read_pictures(const std::filesystem::path& path);
std::vector<Frame> decode_picture_record(const std::vector<std::uint8_t>& bytes, unsigned id);
}
