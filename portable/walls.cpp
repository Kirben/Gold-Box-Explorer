#include "walls.h"
#include "amiga_pictures.h"
#include "ecl_containers.h"
#include "pc98.h"
#include "tlb.h"

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

std::vector<Frame> compose_pieces(const DaxRecord& record, const std::vector<const Frame*>& bank, bool amiga)
{
    constexpr unsigned offsets[] = {0, 2, 6, 10, 22, 38, 54, 110, 132, 154};
    constexpr unsigned widths[] = {1, 1, 1, 3, 2, 2, 7, 2, 2, 1};
    constexpr unsigned heights[] = {2, 4, 4, 4, 8, 8, 8, 11, 11, 2};
    const auto first_tile = std::find_if(bank.begin(), bank.end(), [](const auto* tile) { return tile; });
    const auto tile_size = first_tile == bank.end() ? 8u : (*first_tile)->width;
    std::vector<Frame> pieces;
    for (std::size_t start = 0; start < record.bytes.size(); start += 156) {
        for (unsigned view = 0; view < 10; ++view) {
            Frame frame{record.id, static_cast<unsigned>(pieces.size()), widths[view] * tile_size,
                        heights[view] * tile_size, {}};
            frame.rgba.resize(frame.width * frame.height * 4);
            for (unsigned row = 0; row < heights[view]; ++row) {
                for (unsigned col = 0; col < widths[view]; ++col) {
                    const auto index = record.bytes[start + offsets[view] + row * widths[view] + col];
                    for (unsigned y = 0; y < tile_size; ++y) {
                        auto dest = frame.rgba.begin() + 4 * ((row * tile_size + y) * frame.width + col * tile_size);
                        if (index && index < bank.size() && bank[index])
                            std::copy_n(bank[index]->rgba.begin() + y * tile_size * 4, tile_size * 4, dest);
                        else if (amiga && index) {
                            for (unsigned x = 0; x < tile_size; ++x) {
                                const bool light = ((x / 2) + (y / 2)) % 2;
                                *dest++ = light ? 255 : 80;
                                *dest++ = 0;
                                *dest++ = light ? 255 : 80;
                                *dest++ = 255;
                            }
                        }
                        else if (!index && !amiga) {
                            for (unsigned x = 0; x < tile_size; ++x) {
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

std::vector<Frame> amiga_walls(const fs::path& path, const std::vector<DaxRecord>& records)
{
    const auto folder = path.has_parent_path() ? path.parent_path() : fs::path(".");
    const auto name = "8X8D" + upper(path.filename().string()).substr(7);
    for (const auto& file : fs::directory_iterator(folder)) {
        if (upper(file.path().filename().string()) != name)
            continue;
        const auto pictures = try_amiga_pictures(file.path());
        if (!pictures)
            break;
        std::map<unsigned, std::vector<const Frame*>> tiles;
        for (const auto& tile : pictures->frames) {
            if (tile.width != 8 || tile.height != 8)
                throw std::runtime_error("Amiga wall tile is not 8x8");
            tiles[tile.id].push_back(&tile);
        }
        std::vector<Frame> frames;
        for (const auto& record : records) {
            if (record.bytes.empty() || record.bytes.size() % 780)
                throw std::runtime_error("Invalid Amiga wall definition size");
            std::vector<const Frame*> bank{nullptr};
            auto append = [&](unsigned id) {
                const auto found = tiles.find(id);
                if (found == tiles.end())
                    throw std::runtime_error("Missing Amiga wall tile bank " + std::to_string(id));
                bank.insert(bank.end(), found->second.begin(), found->second.end());
            };
            append(203);
            const auto sets = record.bytes.size() / 780;
            for (unsigned part = 0; part < sets; ++part)
                append(sets == 1 ? record.id : (record.id ? record.id : 10) * 10 + part + 1);
            auto pieces = compose_pieces(record, bank, true);
            frames.insert(frames.end(), std::make_move_iterator(pieces.begin()), std::make_move_iterator(pieces.end()));
        }
        return frames;
    }
    throw std::runtime_error("Missing companion Amiga " + name);
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
            if (record.pc98) {
                auto frames = decode_pc98_wall_tiles(record);
                result.insert(result.end(), std::make_move_iterator(frames.begin()),
                              std::make_move_iterator(frames.end()));
                continue;
            }
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

        return compose_pieces(record, bank, record.pc98);
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
    static const std::regex name(R"(WALLDEF[0-9]*\.(DAX|GLB))", std::regex::icase);
    return std::regex_match(path.filename().string(), name);
}

PictureArchive read_wall_archive(const fs::path& path)
{
    if (upper(path.extension().string()) != ".GLB")
        return {read_wall_pieces(path), {}};

    const auto folder = fs::absolute(path).parent_path();
    std::vector<fs::path> folders{folder, folder.parent_path()};
    auto is_disk = [](const fs::path& dir) {
        const auto name = upper(dir.filename().string());
        return name.size() >= 5 && name.compare(0, 4, "DISK") == 0 &&
               std::all_of(name.begin() + 4, name.end(), [](unsigned char c) { return std::isalnum(c); });
    };
    auto add_disks = [&](const fs::path& root) {
        for (const auto& entry : fs::directory_iterator(root))
            if (entry.is_directory() && is_disk(entry.path()) &&
                std::find(folders.begin(), folders.end(), entry.path()) == folders.end())
                folders.push_back(entry.path());
    };
    add_disks(folder.parent_path());
    if (is_disk(folder) && is_disk(folder.parent_path())) {
        add_disks(folder.parent_path().parent_path());
        const auto count = folders.size();
        for (std::size_t i = 2; i < count; ++i) {
            const auto dir = folders[i];
            add_disks(dir);
        }
    }

    std::map<unsigned, std::vector<Frame>> tiles;
    bool gateway = false;
    for (const auto& dir : folders) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            const auto name = upper(entry.path().filename().string());
            if (name != "WALLS.TLB" && name != "8X8D.TLB" && name != "8X8DA.TLB" &&
                name != "8X8DB.TLB" && name != "8X8DC.TLB")
                continue;
            gateway |= name == "WALLS.TLB";
            try {
                auto archive = read_tlb(entry.path());
                for (auto& frame : archive.frames) {
                    if (frame.width != 8 || frame.height != 8)
                        continue;
                    auto& bank = tiles[frame.id];
                    if (bank.size() <= frame.index)
                        bank.resize(frame.index + 1);
                    if (bank[frame.index].rgba.empty())
                        bank[frame.index] = std::move(frame);
                }
            } catch (const std::runtime_error&) {
                // Some titles use separate shared and area-specific banks.
            }
        }
    }
    PictureArchive result;
    const auto records = read_amiga_glb(path, "DATA");
    for (const auto& record : records) {
        if (record.bytes.empty() || record.bytes.size() % 780) {
            result.skipped_ids.push_back(record.id);
            continue;
        }
        auto append = [&](std::vector<const Frame*>& bank, unsigned id) {
            const auto found = tiles.find(id);
            if (found != tiles.end())
                for (const auto& tile : found->second)
                    bank.push_back(tile.rgba.empty() ? nullptr : &tile);
        };
        auto fits = [&](const std::vector<const Frame*>& bank) {
            return std::all_of(record.bytes.begin(), record.bytes.end(), [&](auto index) {
                return index == 0 || (index < bank.size() && bank[index]);
            });
        };
        std::vector<const Frame*> bank{nullptr};
        unsigned bank_id = record.id;
        if (gateway && record.id == 32) {
            const auto alias = std::find_if(records.begin(), records.end(), [&](const auto& other) {
                return other.id == 15 && other.bytes == record.bytes;
            });
            if (alias != records.end())
                bank_id = alias->id;
        }
        append(bank, bank_id);
        if (bank.size() == 257)
            bank.erase(bank.begin() + 1);
        if ((bank.size() < 255 && !(gateway && bank.size() == 212 && fits(bank))) || !fits(bank)) {
            bank.resize(1);
            append(bank, 203);
            const auto sets = record.bytes.size() / 780;
            if (tiles.count(bank_id))
                append(bank, bank_id);
            else
                for (unsigned part = 1; part <= sets; ++part)
                    append(bank, bank_id * 10 + part);
            // Secret's LOAD_PIECES pairs wall definitions 7 and 15 with
            // these third-slot sets. The first two slots come from the multipart record.
            if (!gateway && sets == 2 && bank.size() == 186) {
                if (bank_id == 7 && tiles.count(71) && tiles.count(72))
                    append(bank, 19);
                else if (bank_id == 15 && tiles.count(151) && tiles.count(152))
                    append(bank, 9);
            }
        }
        if (!fits(bank))
            result.partial_ids.push_back(record.id);
        auto pieces = compose_pieces(record, bank, true);
        result.frames.insert(result.frames.end(), std::make_move_iterator(pieces.begin()),
                             std::make_move_iterator(pieces.end()));
    }
    return result;
}

std::vector<Frame> read_wall_pieces(const fs::path& path)
{
    if (const auto amiga = try_amiga_dax(path); !amiga.empty())
        return amiga_walls(path, amiga);
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
            for (unsigned i = 0; i < result.size();) {
                const auto value = bytes[pos + 2 + 2 * i];
                if (script.pc98 && value == 255) {
                    ++i;
                    continue;
                }
                std::string name;
                const DaxRecord* record = nullptr;
                const auto id = value == 127 || value == 255 ? (script.pc98 ? 0u : map_id) : value;
                if (!resources.wall_location(id, name, record)) {
                    found = false;
                    break;
                }
                result[i] = resources.assemble(*record, name);
                // One multipart PC-98 definition can supply consecutive wall slots.
                if (script.pc98 && value == 127)
                    break;
                i += script.pc98 ? std::max<std::size_t>(1, record->bytes.size() / 780) : 1;
            }
            if (found && std::any_of(result.begin(), result.end(), [](const auto& bank) { return !bank.empty(); }))
                return result;
        }
    }
    throw std::runtime_error("No matching DOS wall sets found in the companion ECL archive");
}

Frame wall_perspective(const GeoMap& map, const WallBanks& banks, int x, int y, int direction)
{
    const auto nonempty = std::find_if(banks.begin(), banks.end(), [](const auto& bank) { return !bank.empty(); });
    const auto scale = nonempty == banks.end() ? 1 : static_cast<int>(nonempty->front().width / 8);
    const auto size = 88 * scale;
    Frame result{0, 0, static_cast<unsigned>(size), static_cast<unsigned>(size),
                 std::vector<std::uint8_t>(size * size * 4, 255)};
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
        px *= scale;
        py *= scale;
        for (int row = 0; row < static_cast<int>(piece->height); ++row) {
            for (int col = 0; col < static_cast<int>(piece->width); ++col) {
                if (row + py < 0 || row + py >= size || col + px < 0 || col + px >= size)
                    continue;
                const auto pixel = piece->rgba.data() + 4 * (row * piece->width + col);
                if (!pixel[3] || (pixel[0] == 255 && pixel[1] == 82 && pixel[2] == 255))
                    continue;
                std::copy_n(pixel, 4, result.rgba.data() + 4 * ((row + py) * size + col + px));
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
