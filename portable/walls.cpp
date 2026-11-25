#include "walls.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <regex>
#include <stdexcept>

namespace goldbox {
namespace {
namespace fs = std::filesystem;

std::string upper(std::string name)
{
    for (auto& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

class WallResources {
public:
    explicit WallResources(const fs::path& folder)
    {
        for (const auto& entry : fs::directory_iterator(folder.empty() ? fs::path(".") : folder))
            if (entry.is_regular_file())
                files_.emplace(upper(entry.path().filename().string()), entry.path());
    }

    const std::vector<DaxRecord>& archive(const std::string& name)
    {
        auto found = archives_.find(name);
        if (found != archives_.end())
            return found->second;
        const auto file = files_.find(name);
        return archives_.emplace(name, file == files_.end() ? std::vector<DaxRecord>{}
                                                           : read_dax(file->second)).first->second;
    }

    const std::vector<Frame>& tiles(const std::string& name, unsigned id)
    {
        const auto key = std::make_pair(name, id);
        const auto cached = tiles_.find(key);
        if (cached != tiles_.end())
            return cached->second;
        std::vector<Frame> result;
        for (const auto& record : archive(name)) {
            if (record.id != id)
                continue;
            for (auto& tile : decode_picture_record(record.bytes, id)) {
                if (tile.width != 8 || tile.height != 8)
                    throw std::runtime_error("Wall tile bank contains a picture larger or smaller than 8x8");
                // The DOS reference wall viewer reads tiles through ARGB1555.
                // Expand the five-bit channels, including its magenta color key.
                for (std::size_t i = 0; i < tile.rgba.size(); ++i)
                    if (i % 4 != 3)
                        tile.rgba[i] = static_cast<std::uint8_t>(((tile.rgba[i] >> 3) * 255 + 15) / 31);
                result.push_back(std::move(tile));
            }
        }
        return tiles_.emplace(key, std::move(result)).first->second;
    }

    std::vector<Frame> assemble(const DaxRecord& record, const std::string& name)
    {
        constexpr int offsets[] = {0, 2, 6, 10, 22, 38, 54, 110, 132, 154};
        constexpr unsigned widths[] = {1, 1, 1, 3, 2, 2, 7, 2, 2, 1};
        constexpr unsigned heights[] = {2, 4, 4, 4, 8, 8, 8, 11, 11, 2};
        constexpr unsigned slice = 156;
        if (record.bytes.empty() || record.bytes.size() % slice)
            throw std::runtime_error("Invalid wall definition record size");
        const auto companion = "8X8D" + name.substr(7);
        const auto& own = tiles(companion, record.id);
        std::vector<const Frame*> bank{nullptr};
        auto append = [&](const auto& source) {
            for (const auto& tile : source)
                bank.push_back(&tile);
        };
        if (own.size() >= 255) {
            append(own);
            if (own.size() == 256)
                bank.erase(bank.begin() + 1);
        } else {
            const auto& universal = tiles("8X8D1.DAX", 203);
            append(universal.empty() ? tiles("8X8D.DAX", 203) : universal);
            append(own);
            const auto sets = record.bytes.size() / 780;
            if (sets > 1 && bank.size() < 255) {
                const auto base = record.id ? record.id * 10 : 100;
                for (unsigned i = 1; i <= std::min<std::size_t>(sets, 3); ++i)
                    append(tiles(companion, base + i));
            }
        }
        if (bank.size() == 1)
            throw std::runtime_error("No matching 8X8D tiles found for the wall definition");

        std::vector<Frame> pieces;
        for (std::size_t start = 0; start < record.bytes.size(); start += slice) {
            for (unsigned view = 0; view < 10; ++view) {
                Frame frame{record.id, static_cast<unsigned>(pieces.size()),
                            widths[view] * 8, heights[view] * 8, {}};
                frame.rgba.resize(frame.width * frame.height * 4);
                for (unsigned row = 0; row < heights[view]; ++row) {
                    for (unsigned col = 0; col < widths[view]; ++col) {
                        const auto index = record.bytes[start + offsets[view] + row * widths[view] + col];
                        for (unsigned y = 0; y < 8; ++y) {
                            auto dest = frame.rgba.begin() + 4 * ((row * 8 + y) * frame.width + col * 8);
                            if (index && index < bank.size())
                                std::copy_n(bank[index]->rgba.begin() + y * 32, 32, dest);
                            else if (!index) {
                                for (unsigned x = 0; x < 8; ++x) {
                                    *dest++ = 255; *dest++ = 82; *dest++ = 255; *dest++ = 255;
                                }
                            }
                        }
                    }
                }
                pieces.push_back(std::move(frame));
            }
        }
        return pieces;
    }

    bool wall_location(unsigned id, std::string& name, const DaxRecord*& record)
    {
        for (const auto& file : files_) {
            if (!is_wall_archive(file.second))
                continue;
            for (const auto& block : archive(file.first)) {
                if (block.id == id) {
                    name = file.first;
                    record = &block;
                    return true;
                }
            }
        }
        return false;
    }

private:
    std::map<std::string, fs::path> files_;
    std::map<std::string, std::vector<DaxRecord>> archives_;
    std::map<std::pair<std::string, unsigned>, std::vector<Frame>> tiles_;
};
}

bool is_wall_archive(const fs::path& path)
{
    static const std::regex name(R"(WALLDEF[0-9]*\.DAX)", std::regex::icase);
    return std::regex_match(path.filename().string(), name);
}

std::vector<Frame> read_wall_pieces(const fs::path& path)
{
    WallResources resources(path.parent_path());
    std::vector<Frame> pieces;
    const auto name = upper(path.filename().string());
    const auto& records = resources.archive(name);
    const bool trailer = has_geo_trailer(records);
    for (const auto& record : records) {
        if (trailer && record.id >= 253)
            continue;
        auto images = resources.assemble(record, name);
        pieces.insert(pieces.end(), std::make_move_iterator(images.begin()),
                      std::make_move_iterator(images.end()));
    }
    return pieces;
}

WallBanks load_map_walls(const fs::path& geo_path, unsigned map_id)
{
    WallResources resources(geo_path.parent_path());
    const auto companion = "ECL" + upper(geo_path.filename().string()).substr(3);
    for (const auto& script : resources.archive(companion)) {
        if (script.id != map_id)
            continue;
        const auto& bytes = script.bytes;
        for (std::size_t pos = 0; pos + 7 <= bytes.size(); ++pos) {
            if (bytes[pos] != 0x37 || bytes[pos + 1] || bytes[pos + 3] || bytes[pos + 5])
                continue;
            WallBanks result;
            bool found = true;
            for (unsigned i = 0; i < result.size(); ++i) {
                const auto value = bytes[pos + 2 + 2 * i];
                std::string name;
                const DaxRecord* record = nullptr;
                if (!resources.wall_location(value == 127 || value == 255 ? map_id : value, name, record)) {
                    found = false;
                    break;
                }
                result[i] = resources.assemble(*record, name);
            }
            if (found)
                return result;
        }
    }
    throw std::runtime_error("No matching DOS wall sets found in the companion ECL archive");
}

Frame wall_perspective(const GeoMap& map, const WallBanks& banks, int x, int y, int direction)
{
    Frame result{0, 0, 88, 88, std::vector<std::uint8_t>(88 * 88 * 4, 255)};
    constexpr int dx[] = {0, 1, 0, -1};
    constexpr int dy[] = {-1, 0, 1, 0};
    const auto right = (direction + 1) % 4;
    const auto left = (direction + 3) % 4;
    auto square = [&](int depth, int lateral) {
        const auto col = x + dx[direction] * depth + dx[right] * lateral;
        const auto row = y + dy[direction] * depth + dy[right] * lateral;
        return col >= 0 && row >= 0 && col < map_side && row < map_side
            ? map[row * map_side + col] : GeoCell{};
    };
    auto draw = [&](unsigned type, unsigned view, int px, int py) {
        if (!type)
            return;
        const Frame* piece = nullptr;
        for (const auto& bank : banks) {
            const auto count = bank.size() / 10;
            if (type <= count) {
                piece = &bank[(type - 1) * 10 + view];
                break;
            }
            type -= count;
        }
        if (!piece)
            return;
        for (int row = 0; row < static_cast<int>(piece->height); ++row) {
            for (int col = 0; col < static_cast<int>(piece->width); ++col) {
                if (row + py < 0 || row + py >= 88 || col + px < 0 || col + px >= 88)
                    continue;
                const auto pixel = piece->rgba.data() + 4 * (row * piece->width + col);
                if (!pixel[3] || (pixel[0] == 255 && pixel[1] == 82 && pixel[2] == 255))
                    continue;
                std::copy_n(pixel, 4, result.rgba.data() + 4 * ((row + py) * 88 + col + px));
            }
        }
    };
    unsigned last = 0, last_right = 0;
    for (int c = 0; c < 7; ++c) {
        const auto w = square(2, c - 3).walls;
        draw(w[direction], 0, c * 16 - 8, 24);
        if ((w[direction] && c > 0 && c <= 3 && (w[left] || last)) ||
            (c > 3 && last && (w[direction] || last_right)))
            draw(c <= 3 ? w[direction] : last, 9, c * 16 - 16, 24);
        last = w[direction];
        last_right = w[right];
    }
    for (int c = 1; c < 6; ++c) {
        const auto w = square(2, c - 3).walls;
        if (c >= 3) draw(w[right], 2, c * 16 + (c == 3 ? 0 : 8), 16);
        if (c <= 3) draw(w[left], 1, c * 16 - (c == 3 ? 16 : 24), 16);
    }
    for (int c = 0; c < 5; ++c)
        draw(square(1, c - 2).walls[direction], 3, c * 24 - 16, 16);
    for (int c = 1; c < 5; ++c) {
        const auto w = square(1, c - 2).walls;
        if (c < 3) draw(w[left], 4, c * 24 - 32, 0);
        if (c > 1) draw(w[right], 5, c * 24 + 8, 0);
    }
    for (int c = 0; c < 3; ++c)
        draw(square(0, c - 1).walls[direction], 6, c * 56 - 40, 0);
    const auto near = square(0, 0).walls;
    draw(near[right], 8, 72, -8);
    draw(near[left], 7, 0, -8);
    return result;
}
}
