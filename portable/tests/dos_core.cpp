#include "dax.h"
#include "geo.h"
#include "pictures.h"
#include "records.h"
#include "save_map.h"
#include "walls.h"
#include "tlb.h"
#include "vace.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace goldbox;
using Bytes = std::vector<std::uint8_t>;

void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void write(const fs::path& path, const Bytes& bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.close();
    check(bool(stream), "Could not write fixture");
}

void dax(const fs::path& path, std::uint8_t id, const Bytes& data)
{
    Bytes bytes{9, 0, id, 0, 0, 0, 0, 0, 0,
                static_cast<std::uint8_t>(data.size()), static_cast<std::uint8_t>(data.size() >> 8)};
    bytes.insert(bytes.end(), data.begin(), data.end());
    write(path, bytes);
}

void pictures(const fs::path& dir)
{
    Bytes data{2};
    for (unsigned i = 0; i < 2; ++i) {
        Bytes frame(25);
        frame[0] = i ? 4 : 2;
        frame[4] = frame[6] = 1;
        frame[21] = i ? 0x14 : 0x4f;
        data.insert(data.end(), frame.begin(), frame.end());
    }
    const auto path = dir / "PIC.DAX";
    dax(path, 7, data);
    const auto archive = read_pictures(path);
    check(archive.frames.size() == 2 && archive.clips.size() == 1, "Missing animation frames or clip");
    const auto& first = archive.frames[0];
    const auto& second = archive.frames[1];
    check(first.id == 7 && first.width == 8 && first.height == 1, "Unexpected frame geometry");
    check(first.rgba.at(0) == 170 && first.rgba.at(1) == 0 && first.rgba.at(3) == 255,
          "Unexpected EGA red pixel");
    check(second.rgba.at(0) == 170 && second.rgba.at(2) == 170, "Animation delta was not applied");
    const auto& steps = archive.clips[0].steps;
    check(steps.size() == 2 && steps[0].frame == 0 && steps[1].frame == 1 &&
          steps[0].duration_ms == 220 && steps[1].duration_ms == 439, "Unexpected DOS playback timing");

    const auto sprite = dir / "SPRIT1.DAX";
    dax(sprite, 7, data);
    const auto variants = read_pictures(sprite);
    check(variants.frames.size() == 2 && variants.clips.empty() &&
          variants.frames[1].rgba[2] == 170 && variants.frames[1].rgba[4] == 170 &&
          variants.frames[1].rgba[7] == 255 && variants.frames[1].rgba[11] == 0,
          "Encounter sprites should show independent, transparent distance frames");

    constexpr unsigned directory = 11 + 80 * 3 + 40 + 6;
    Bytes extended(directory + 6);
    extended[0] = extended[2] = extended[8] = 1;
    extended[9] = 176;
    extended[10] = 80;
    extended[11] = 40;
    extended[directory - 2] = 1;
    extended[directory] = extended[directory + 3] = 2;
    extended.insert(extended.end(), {249, 176, 249, 176});
    dax(sprite, 8, extended);
    const auto vga_sprite = read_pictures(sprite);
    check(vga_sprite.frames.size() == 2 && vga_sprite.frames[0].rgba[3] == 0 &&
          vga_sprite.frames[1].rgba[0] == 160 && vga_sprite.frames[1].rgba[3] == 255,
          "Treasures VGA sprite key-frame, transparency or palette mismatch");

    const auto body = dir / "CBODY.DAX";
    Bytes chunky{1, 1, 0, 0, 0, 0, 1, 0, 32, 0, 63, 32, 1,
                 32, 4, 4, 4, 4, 4, 4, 4};
    dax(body, 3, chunky);
    const auto colored = read_pictures(body);
    check(colored.frames.size() == 1 && colored.frames[0].width == 8 &&
          colored.frames[0].rgba[0] == 252 && colored.frames[0].rgba[1] == 128 &&
          colored.frames[0].rgba[2] == 4 && colored.frames[0].rgba[4] == 170,
          "Indexed DAX palette or pixels decoded incorrectly");

    const auto tiles = dir / "8x8d1.dax";
    dax(tiles, 4, {0x81, 0, 0, 0, 0, 0, 0, 0x18});
    const auto mono = read_pictures(tiles);
    check(mono.frames.size() == 1 && mono.frames[0].height == 8 &&
          mono.frames[0].rgba[0] == 255 && mono.frames[0].rgba[4] == 0 &&
          mono.frames[0].rgba[28] == 255, "Monochrome tile bit order is incorrect");
    check(is_picture_archive(body) && is_picture_archive(tiles) && is_picture_archive("FINAL5.DAX") &&
          !is_picture_archive("MONCHA.DAX") && !is_picture_archive("ECL2.DAX"),
          "DAX graphics routing must retain distinct data handlers");
}

void ending(const fs::path& dir)
{
    const auto still = [](unsigned side, std::uint8_t color) {
        Bytes bytes(17 + side * side / 2, color);
        std::fill_n(bytes.begin(), 17, 0);
        bytes[0] = static_cast<std::uint8_t>(side);
        bytes[2] = static_cast<std::uint8_t>(side / 8);
        bytes[8] = 1;
        return bytes;
    };
    const auto animated = [](std::uint8_t master, std::uint8_t delta) {
        Bytes bytes{2};
        for (auto color : {master, delta}) {
            Bytes frame(21 + 88 * 44, color);
            std::fill_n(frame.begin(), 21, 0);
            frame[0] = 5;
            frame[4] = 88;
            frame[6] = 11;
            bytes.insert(bytes.end(), frame.begin(), frame.end());
        }
        return bytes;
    };
    const std::vector<Bytes> blocks{still(120, 0x11), animated(0x44, 0x40),
        still(120, 0x22), still(8, 0x33), still(8, 0x55), still(8, 0x66),
        animated(0x77, 0x07), animated(0x99, 0x09)};
    Bytes archive{72, 0};
    unsigned offset = 0;
    for (unsigned i = 0; i < blocks.size(); ++i) {
        const auto length = blocks[i].size();
        archive.push_back(static_cast<std::uint8_t>(i + 1));
        for (unsigned shift = 0; shift < 32; shift += 8)
            archive.push_back(static_cast<std::uint8_t>(offset >> shift));
        archive.insert(archive.end(), {0, 0, static_cast<std::uint8_t>(length),
                                      static_cast<std::uint8_t>(length >> 8)});
        offset += static_cast<unsigned>(length);
    }
    for (const auto& block : blocks)
        archive.insert(archive.end(), block.begin(), block.end());
    const auto path = dir / "FINAL5.DAX";
    write(path, archive);
    const auto ending = read_pictures(path);
    check(ending.gallery_count() == 11 && ending.playback_only_frames == 10 &&
          ending.clips.size() == 1 && ending.clips[0].whole_archive,
          "FINAL gallery frames and assembled frames were not separated");
    const auto& steps = ending.clips[0].steps;
    check(steps.size() == 91 && steps[0].duration_ms == 220 &&
          ending.frames.at(steps[0].frame).id == 2 && ending.frames.at(steps[44].frame).id == 7 &&
          ending.frames.at(steps[88].frame).id == 8 && steps[90].frame == steps[89].frame &&
          steps[90].duration_ms == 675, "FINAL scene order, repeats or timing mismatch");
    const auto& first = ending.frames.at(steps[1].frame);
    const auto& later = ending.frames.at(steps[44].frame);
    check(first.width == 120 && first.height == 120 && first.rgba[2] == 170 &&
          first.rgba[(32 * 120 + 16) * 4] == 170 && later.rgba[(72 * 120 + 104) * 4] == 170,
          "FINAL background, black-key overlay or party layer mismatch");

    dax(path, 2, blocks[1]);
    const auto partial = read_pictures(path);
    check(partial.gallery_count() == 2 && partial.clips.empty(), "Incomplete FINAL must stay browsable");
}

Bytes tlb_library(const char* type, const std::vector<Bytes>& records, bool has_ids = false)
{
    Bytes bytes(16 + 4 * (records.size() + 1));
    std::copy_n("HLIB", 4, bytes.begin());
    std::copy_n(type, 4, bytes.begin() + 12);
    bytes[8] = static_cast<std::uint8_t>(records.size());
    bytes[11] = has_ids;
    const auto set_dword = [&](std::size_t offset, std::size_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    for (std::size_t i = 0; i < records.size(); ++i) {
        set_dword(16 + 4 * i, bytes.size());
        bytes.insert(bytes.end(), records[i].begin(), records[i].end());
    }
    set_dword(16 + 4 * records.size(), bytes.size());
    set_dword(4, bytes.size());
    return bytes;
}

Bytes tlb_image(std::uint8_t method, const Bytes& pixels)
{
    Bytes bytes{1, 0, 0, 0, 0, 0, 1, method}; // 4 by 1 pixels
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    return bytes;
}

void tlb(const fs::path& dir)
{
    const Bytes palette{5, 0, 32, 0, 1, 0, 0, 24, 10, 20, 30};
    const auto tile = tlb_library("TILE", {palette, tlb_image(16, {32, 0, 32, 0}),
        tlb_image(17, {255, 0, 0, 0, 32, 32, 32, 32}), tlb_image(18, {253, 32}),
        tlb_image(21, {32, 255, 32, 255}), tlb_image(23, {255, 0, 1, 32, 0, 0, 1, 32, 0}),
        tlb_image(25, {1, 2, 0, 0, 0, 0, 4, 2, 0, 0, 4, 0})});
    const auto path = dir / "PIC.tlb";
    write(path, tlb_library("HLIB", {{1, 0, 42, 0, 1, 0}, tile}, true));
    check(is_tlb_archive(path), "Mixed case TLB extension not recognized");
    const auto images = read_tlb(path);
    check(images.frames.size() == 6 && images.skipped_ids.empty(), "Unexpected TLB frame count");
    check(images.frames[0].id == 42 && images.frames[0].rgba == Bytes{
        10,20,30,255, 0,0,0,255, 10,20,30,255, 0,0,0,255}, "Wrong TLB IDs, palette or pixels");
    check(images.frames[1].rgba[3] == 0 && images.frames[1].rgba[7] == 255 &&
          images.frames[2].rgba[12] == 10 && images.frames[2].rgba[15] == 255 &&
          images.frames[3].rgba[7] == 0, "Wrong TLB mask, repetition or transparent index");
    check(images.frames[4].rgba[3] == 0 && images.frames[4].rgba[4] == 10 &&
          images.frames[4].rgba[11] == 0, "Wrong TLB sparse row");
    check(images.frames[5].width == 8 && images.frames[5].rgba[16] == 10 &&
          images.frames[5].rgba[23] == 0, "Wrong TLB composite placement");

    write(dir / "dungcom.tlb", tile);
    write(dir / "CPIC0001.TLB", tlb_library("TILE", {tlb_image(21, {32, 32, 32, 32})}));
    check(read_tlb(dir / "CPIC0001.TLB").frames[0].rgba[0] == 10,
          "FRUA combat icon did not load the companion palette");
}

void geo_glb(const fs::path& dir)
{
    const auto disk1 = dir / "disk1";
    const auto disk2 = dir / "Disk2";
    const auto disk3 = dir / "DISK3";
    for (const auto& disk : {disk1, disk2, disk3})
        fs::create_directories(disk);

    Bytes system_palette{5, 0, 32, 0, 16, 0, 0, 24};
    std::vector<Bytes> tiles{{1, 0, 0, 0, 0, 0, 0, 24}};
    for (unsigned i = 0; i < 16; ++i) {
        system_palette.insert(system_palette.end(), {static_cast<std::uint8_t>(i * 16), 64, 0});
        Bytes pixels(64, static_cast<std::uint8_t>(32 + i));
        auto tile = tlb_image(16, pixels);
        tile[0] = 8;
        tile[6] = 2;
        tiles.push_back(std::move(tile));
    }
    write(disk1 / "always.tlb", tlb_library("TILE", {system_palette}));
    write(disk2 / "Topview.tlb", tlb_library("TILE", tiles));

    Bytes area(8 + 4 * 4);
    area[0] = area[1] = 2;
    // N, E, S+W, and all walls.
    area[8] = 0x10;
    area[9] = 0x01;
    area[11] = 0x11;
    area[14] = 0x12;
    area[15] = 0x34;
    const auto path = disk3 / "geo.glb";
    write(path, tlb_library("DATA", {{2, 0, 42, 0, 1, 0, 7, 0, 1, 0}, area}, true));
    check(is_geo_glb(path) && !is_geo_glb("MONCHA.GLB"), "Unexpected GLB map routing");
    const auto maps = read_geo_glb(path);
    check(maps.gallery_count() == 2 && maps.frames[0].id == 42 && maps.frames[1].id == 7 &&
          maps.frames[0].rgba == maps.frames[1].rgba, "GEO.GLB IDs or their order changed");
    const auto& map = maps.frames[0];
    check(map.width == 16 && map.height == 16, "GEO.GLB map dimensions are wrong");
    check(map.rgba[0] == 16 && map.rgba[1] == 64 && map.rgba[3] == 255 &&
          map.rgba[8 * 4] == 32 && map.rgba[8 * 16 * 4] == 192 &&
          map.rgba[(8 * 16 + 8) * 4] == 240, "GEO.GLB tile order, wall mask or palette mismatch");
}

void vace(const fs::path& dir)
{
    const Bytes first{64, 1, 200, 0, 0, 0, 128, 2, 3, 2, 1, 2, 255};
    const Bytes delta{64, 1, 200, 0, 0, 1, 128, 1, 2, 0, 3, 255};
    const auto size = static_cast<std::uint8_t>(14 + first.size() + delta.size());
    Bytes bma{size, 0, 0, 0, 2, 0, 14, 0, 0, 0,
              static_cast<std::uint8_t>(14 + first.size()), 0, 0, 0};
    bma.insert(bma.end(), first.begin(), first.end());
    bma.insert(bma.end(), delta.begin(), delta.end());
    const Bytes acf{6, 2, 63, 1, 0, 0, 6, 2, 0, 2, 63, 0, 6, 2, 0, 3, 0, 63,
                    2, 3, 2, 12, 2, 11, 4, 0, 16, 0, 2, 3, 8, 5, 0, 0, 2, 0, size, 0, 2, 1};
    const auto picture = dir / "intro.bma";
    const auto control = dir / "INTRO.ACF";
    write(picture, bma);
    write(control, acf);
    check(is_vace_archive(picture) && is_vace_archive(control), "VACE filename recognition failed");
    const auto archive = read_vace(control);
    check(archive.gallery_count() == 2 && archive.frames.size() == 3 && archive.clips.size() == 1,
          "VACE gallery or playback frame count is wrong");
    const auto& a = archive.frames[0];
    const auto& b = archive.frames[1];
    const Bytes colors{255, 0, 0, 255, 0, 0, 255, 255};
    const Bytes next{0, 0, 0, 255, 0, 255, 0, 255};
    check(a.width == 320 && a.height == 200 && std::equal(colors.begin(), colors.end(), a.rgba.begin()) &&
          std::equal(next.begin(), next.end(), b.rgba.begin()), "VACE palette, span or clear mismatch");
    const auto& steps = archive.clips[0].steps;
    check(steps.size() == 4 && steps[0].frame == 0 && steps[0].duration_ms == 14 &&
          steps[1].frame == 0 && steps[1].duration_ms == 2000 &&
          steps[2].frame == 2 && steps[2].duration_ms == 15 &&
          steps[3].frame == 1 && steps[3].duration_ms == 229, "VACE scene order or preview timing mismatch");
    check(read_vace(picture).frames[1].rgba == b.rgba, "Both halves of a VACE pair should open the same gallery");
    auto corrupt = acf;
    corrupt[corrupt.size() - 4] = size - 1;
    write(control, corrupt);
    try {
        read_vace(picture);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("VACE accepted a mismatched group table");
}

void geo(const fs::path& dir)
{
    constexpr unsigned index = 3 * 16 + 2;
    Bytes data(1026);
    data[2 + index] = 0x12;
    data[258 + index] = 0x34;
    data[514 + index] = 0x85;
    data[770 + index] = 0xe4;
    const auto path = dir / "GEO5.DAX";
    dax(path, 15, data);
    const auto records = read_dax(path);
    check(records.size() == 1 && records[0].id == 15, "Unexpected GEO directory");
    const auto map = read_geo_map(records[0]);
    const auto& cell = map[index];
    check(cell.walls == std::array<unsigned, 4>{1, 2, 3, 4}, "Wall order changed");
    check(cell.doors == std::array<unsigned, 4>{0, 1, 2, 3}, "Door order changed");
    check(cell.event == 5 && cell.event_flag && map[index + 1].event == 0, "Unexpected event plane");
}

void walls(const fs::path& dir)
{
    Bytes tile(17 + 32);
    tile[0] = 8;
    tile[2] = tile[8] = 1;
    std::fill(tile.begin() + 17, tile.end(), 0x44);
    dax(dir / "8X8D.DAX", 1, tile);
    const auto path = dir / "WALLDEF.DAX";
    dax(path, 1, Bytes(780, 1));
    const auto frames = read_wall_pieces(path);
    check(frames.size() == 50 && frames[0].width == 8 && frames[0].height == 16,
          "Unexpected wall piece count or geometry");
    check(frames[0].rgba.at(0) == 173 && frames[0].rgba.at(3) == 255,
          "Wall piece did not use the companion tile");
    GeoMap map{};
    map[8 * 16 + 8].walls[0] = 1;
    WallBanks banks;
    banks[0] = frames;
    const auto view = wall_perspective(map, banks, 8, 8, 0);
    check(view.width == 88 && view.height == 88, "Unexpected wall perspective geometry");
    const auto pixel = (20 * 88 + 40) * 4;
    check(view.rgba.at(pixel) == 173 && view.rgba.at(pixel + 1) == 0,
          "Front wall is missing from the perspective");
}

Bytes character()
{
    Bytes bytes(285);
    bytes[0] = 3;
    bytes[1] = 'O';
    bytes[2] = 'r';
    bytes[3] = 'c';
    bytes[45] = 41;
    bytes[50] = 8;
    bytes[283] = 6;
    return bytes;
}

Bytes item()
{
    Bytes bytes(63);
    bytes[46] = bytes[49] = 37;
    bytes[50] = 255;
    return bytes;
}

bool has_field(const RecordRow& row, const Field& field)
{
    return std::find(row.fields.begin(), row.fields.end(), field) != row.fields.end();
}

void records(const fs::path& dir)
{
    dax(dir / "MONCHA.DAX", 2, character());
    dax(dir / "ITEM.DAX", 2, item());
    dax(dir / "MONITM.DAX", 2, item());
    const auto monsters = read_record_rows(dir / "MONCHA.DAX");
    check(monsters.size() == 1 && monsters[0].name == "Orc" &&
          has_field(monsters[0], {"Base THAC0", "19"}) && has_field(monsters[0], {"Current HP", "6"}),
          "Unexpected MONCHA fields");
    const auto items = read_record_rows(dir / "ITEM.DAX");
    check(items.size() == 1 && has_field(items[0], {"Magic bonus", "-1"}), "Item bonus lost its sign");
    const auto inventory = read_record_rows(dir / "MONITM.DAX");
    check(inventory.size() == 1 && has_field(inventory[0], {"Owner", "Orc"}), "Missing inventory owner");
}

void saves(const fs::path& dir)
{
    const auto folder = dir / "SAVE";
    fs::create_directories(folder);
    write(folder / "ORC.SAV", character());
    write(folder / "ORC.ITM", item());
    write(folder / "ORC.SPC", {38, 0x34, 0x12, 1, 1, 0, 0, 0, 0});
    const auto saved = read_record_rows(folder / "ORC.SAV");
    const auto inventory = read_record_rows(folder / "ORC.ITM");
    const auto effects = read_record_rows(folder / "ORC.SPC");
    check(saved.size() == 1 && saved[0].name == "Orc", "Missing saved character");
    check(inventory.size() == 1 && has_field(inventory[0], {"Owner", "Orc"}), "Missing saved item owner");
    check(effects.size() == 1 && effects[0].id == 38 &&
          has_field(effects[0], {"Duration (stored)", "4660"}), "Unexpected saved effect");

    Bytes world(13137);
    world[0] = 2;
    world[0x18b] = world[0x1e5] = 15;
    world[12801] = 8;
    world[12802] = 5;
    world[12803] = 2;
    world[12806] = 1;
    write(folder / "SAVGAMA.DAT", world);
    constexpr unsigned position = 5 * 16 + 8;
    Bytes map(1026);
    dax(dir / "GEO2.DAX", 15, map);
    map[2 + position] = 0x10;
    dax(folder / "GEO2.DAX", 15, map);
    Bytes explored{4, 'E', 'X', 'P', 'L', 1, 1, 15, 15};
    explored.resize(41);
    explored[7 + 2 + 2 * 5 + 1] = 1;
    write(folder / "SAVGAMA.MAP", explored);
    const auto area = read_saved_map(folder / "SAVGAMA.DAT");
    check(area.disk == 2 && area.script == 15 && area.map_id == 15 && area.x == 8 &&
          area.y == 5 && area.facing == 1, "Unexpected saved party position");
    check(area.original && area.copied && area.changed.count() == 1 && area.changed.test(position),
          "Unexpected saved map changes");
    check(area.visited && area.visited->count() == 1 && area.visited->test(position),
          "Unexpected explored cells");
}
}

int main(int argc, char** argv)
{
    try {
        check(argc == 3, "Expected a test name and a fixture directory");
        const std::string_view name(argv[1]);
        const fs::path dir(argv[2]);
        fs::create_directories(dir);
        if (name == "pictures") pictures(dir);
        else if (name == "ending") ending(dir);
        else if (name == "tlb") tlb(dir);
        else if (name == "geo-glb") geo_glb(dir);
        else if (name == "vace") vace(dir);
        else if (name == "geo") geo(dir);
        else if (name == "walls") walls(dir);
        else if (name == "records") records(dir);
        else if (name == "saves") saves(dir);
        else throw std::runtime_error("Unknown DOS regression test");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
