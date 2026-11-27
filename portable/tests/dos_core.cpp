#include "dax.h"
#include "disk_image.h"
#include "geo.h"
#include "pictures.h"
#include "records.h"
#include "ecl.h"
#include "c64.h"
#include "save_map.h"
#include "walls.h"
#include "tlb.h"
#include "vace.h"
#include "cmp.h"
#include "xmi.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
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
    auto repeat = [](Bytes& packed, unsigned count, unsigned value) {
        packed.push_back(static_cast<std::uint8_t>(256 - count));
        packed.push_back(static_cast<std::uint8_t>(value));
    };
    const auto static_daa = dir / "CPIC1.DAA";
    Bytes header(73);
    header[1] = header[3] = header[8] = 1;
    header[11] = 0xf;
    Bytes packed{72};
    packed.insert(packed.end(), header.begin(), header.end());
    repeat(packed, 1, 0x80);
    repeat(packed, 9, 0);
    Bytes daa{0,11,3,0,0,0,0,0,83,0,static_cast<std::uint8_t>(packed.size())};
    daa.insert(daa.end(), packed.begin(), packed.end());
    write(static_daa, daa);
    const auto static_art = read_pictures(static_daa);
    check(is_picture_archive(static_daa) && static_art.frames.size() == 1 &&
          static_art.frames.front().rgba[0] == 255 && static_art.frames.front().rgba[7] == 0,
          "Amiga DAA directory, one-byte repeat, planar layout or transparency changed");

    const auto animated_daa = dir / "PIC1.DAA";
    Bytes animation_header(78);
    animation_header[1] = animation_header[3] = 1;
    animation_header[9] = 2;
    animation_header[12] = 0xf;
    animation_header[75] = animation_header[77] = 11;
    Bytes animation{0,8,4,0,0,0,0,0,0,79,77};
    animation.insert(animation.end(), animation_header.begin(), animation_header.end());
    for (auto bit : {0x80, 0xc0}) {
        animation.push_back(9);
        animation.push_back(bit);
        animation.insert(animation.end(), 9, 0);
        animation.insert(animation.end(), {0, 2});
    }
    write(animated_daa, animation);
    const auto animated_art = read_pictures(animated_daa);
    check(animated_art.frames.size() == 2 && animated_art.clips.size() == 1 &&
          animated_art.frames[0].rgba[0] == 255 && animated_art.frames[1].rgba[0] == 0 &&
          animated_art.frames[1].rgba[4] == 255 && animated_art.clips[0].steps[1].duration_ms == 200,
          "Amiga DAA PackBits frame sizes, XOR delta or stored delay changed");

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

Bytes tlb_library(const char* type, const std::vector<Bytes>& records, bool has_ids = false, bool amiga = false)
{
    Bytes bytes(16 + 4 * (records.size() + 1));
    std::copy_n(amiga ? "GLIB" : "HLIB", 4, bytes.begin());
    std::copy_n(type, 4, bytes.begin() + 12);
    bytes[amiga ? 9 : 8] = static_cast<std::uint8_t>(records.size());
    bytes[11] = has_ids;
    const auto set_dword = [&](std::size_t offset, std::size_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(value >> ((amiga ? 3 - i : i) * 8));
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
    Bytes amiga_palette(104);
    amiga_palette[7] = 232;
    amiga_palette[8 + 16 * 3] = 160;
    const Bytes masked{0,1,0,0,0,0,1,225, 0x80,0,0,0,0,0x60};
    const Bytes compressed{0,1,0,0,0,0,1,226, 253,0,0,0x40};
    const auto amiga_path = dir / "amiga.tlb";
    write(amiga_path, tlb_library("GLIB", {{0,1,0,42,0,1},
        tlb_library("TILE", {amiga_palette, masked, compressed}, false, true)}, true, true));
    const auto amiga = read_tlb(amiga_path);
    check(amiga.frames.size() == 2 && amiga.skipped_ids.empty() && amiga.frames[0].id == 42 &&
          amiga.frames[0].width == 8 && amiga.frames[0].rgba[3] == 0 &&
          amiga.frames[0].rgba[4] == 160 && amiga.frames[0].rgba[7] == 255 &&
          amiga.frames[1].rgba[4] == 160, "Amiga TILE endian, palette, mask or RLE mismatch");

    Bytes amiga_map(1024);
    amiga_map[0] = 0x10;
    amiga_map[512] = 1;
    const auto amiga_map_path = dir / "GEO.GLB";
    write(amiga_map_path, tlb_library("DATA", {{0,1,0,42,0,1}, amiga_map}, true, true));
    const auto maps = read_geo_glb(amiga_map_path);
    check(maps.frames.size() == 1 && maps.frames[0].id == 42 && maps.frames[0].width == 193 &&
          maps.frames[0].rgba[0] == 235 && maps.frames[0].rgba[(5 * 193 + 5) * 4] == 94,
          "Amiga fixed GEO layout mismatch");
    const auto strings = dir / "STRINGS.GLB";
    write(strings, tlb_library("TEXT", {{'C','l','e','r','i','c'}, {'T','h','i','e','f'}}, false, true));
    const auto labels = read_record_rows(strings);
    check(labels.size() == 2 && labels[0].id == 0 && labels[0].name == "Cleric" &&
          labels[1].id == 1 && labels[1].name == "Thief", "Amiga GLIB string IDs or payload mismatch");
    const auto sounds = dir / "SOUNDS.GLB";
    write(sounds, tlb_library("DIG8", {{0x1c,0xfa,0,0,0,2,0x7f,0x80}}, false, true));
    const auto samples = read_record_rows(sounds);
    check(samples.size() == 1 && samples[0].fields.back().second == "2",
          "Amiga GLIB sample metadata mismatch");

    const Bytes span_tile{0,1,0,0,0,0,1,3, 0,3,0,1, 0x41,0x80,0, 0x80,0,0,0};
    const auto plain = tlb_library("TILE", {amiga_palette, span_tile}, false, true);
    Bytes packed(plain.begin(), plain.begin() + 16);
    packed[10] = 5;
    packed.insert(packed.end(), {0, 0});
    for (std::size_t at = 16; at < plain.size();) {
        const auto count = std::min<std::size_t>(plain.size() - at, 15);
        packed.push_back(static_cast<std::uint8_t>(0xd0 + count));
        packed.insert(packed.end(), plain.begin() + at, plain.begin() + at + count);
        at += count;
    }
    packed.push_back(255);
    write(amiga_path, packed);
    const auto spans = read_tlb(amiga_path);
    check(spans.frames.size() == 1 && spans.frames[0].rgba[3] == 255 &&
          spans.frames[0].rgba[7] == 0, "Amiga LZ library or blended span decode mismatch");

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

    const auto scene_archive = [](unsigned sets) {
        Bytes ids{static_cast<std::uint8_t>(sets), 0};
        std::vector<Bytes> members;
        for (unsigned i = 1; i <= sets; ++i) {
            const auto id = static_cast<std::uint8_t>(i);
            ids.insert(ids.end(), {id, 0, id, 0});
            const Bytes palette{5, 0, 32, 0, 1, 0, 0, 24, id, 0, 0};
            auto overlay = tlb_image(21, {255, 4, 255, 255});
            overlay[4] = overlay[5] = 255; // Signed X origin of -1.
            members.push_back(tlb_library("TILE", {palette, tlb_image(16, {32, 32, 32, 32}), overlay}));
        }
        members.insert(members.begin(), std::move(ids));
        return tlb_library("HLIB", members, true);
    };
    write(dir / "DQK.EXE", {});
    write(dir / "TITLE.TLB", scene_archive(10));
    const auto titles = read_tlb(dir / "TITLE.TLB");
    check(titles.frames.size() == 8 && titles.frames[0].width == 320 &&
          titles.frames[0].height == 200 && titles.frames[0].rgba[0] == 2 &&
          titles.frames[0].rgba[8] == 171 && titles.frames[0].rgba[12] == 2,
          "Dark Queen title did not compose the placed overlay with its palette");
    write(dir / "FINAL.TLB", scene_archive(3));
    const auto endings = read_tlb(dir / "FINAL.TLB");
    check(endings.frames.size() == 6 && endings.frames[0].rgba[0] == 1 &&
          endings.frames[1].rgba[8] == 171 && endings.frames[2].rgba[0] == 3 &&
          endings.frames[4].rgba[0] == 2, "Dark Queen ending scene order or layer composition mismatch");
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

void cmp(const fs::path& dir)
{
    const auto file = [](unsigned order, Bytes runs) {
        const auto length = runs.size() + 1;
        Bytes result{static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
                     static_cast<std::uint8_t>(order)};
        result.insert(result.end(), runs.begin(), runs.end());
        return result;
    };
    const auto title = dir / "title.cmp";
    write(title, file(2, {1, 0x51, 255, 0x2c, 0, 0xfe, 0x7c, 0}));
    check(is_cmp_archive(title) && !is_cmp_archive("unknown.cmp"), "Hillsfar CMP routing mismatch");
    const auto picture = read_cmp(title);
    const auto& frame = picture.frames.at(0);
    check(frame.width == 320 && frame.height == 200 && frame.rgba[0] == 0 &&
          frame.rgba[3] == 255 && frame.rgba[6] == 170 && frame.rgba[8 * 160 + 1] == 170 &&
          frame.rgba[8] == 0, "CMP palette, run types or column order mismatch");

    const auto maze = dir / "MAZE2.CMP";
    write(maze, file(1, {0, 0x20, 0x01, 0x11, 0, 0xe0, 0x7b, 0x22}));
    const auto tiles = read_cmp(maze);
    check(tiles.gallery_count() == 48 && tiles.frames.front().width == 24 &&
          tiles.frames.back().height == 24 && tiles.frames.back().id == 47 &&
          tiles.frames[0].rgba[2] == 170 && tiles.frames[1].rgba[1] == 170,
          "CMP maze tile count, stride or order mismatch");

    Bytes amiga(35);
    amiga[3] = 0xf0; // Index 1 is bright green.
    amiga[34] = 2;
    amiga.insert(amiga.end(), {0,0,1,0, 1,0x80, 0,0x7c,0xfe,0});
    amiga[33] = static_cast<std::uint8_t>(amiga.size() - 34);
    write(title, amiga);
    const auto amiga_picture = read_cmp(title);
    check(amiga_picture.frames[0].rgba[(320 * 4) + 1] == 255 &&
          amiga_picture.frames[0].rgba[1] == 0, "Amiga CMP RGB4 or column-major planes changed");

    write(title, file(2, {0, 0x01, 0x7d, 0}));
    try {
        read_cmp(title);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("CMP accepted a run beyond the raster");
}

void anm(const fs::path& dir)
{
    Bytes source(0x3d);
    source[0] = 4;
    source[1] = 2;
    source.insert(source.end(), {0, 0x16, 0x80, 0x41,
                                 2, 0x10, 0x01, 0, 0x16, 0x7e, 0});
    const auto path = dir / "cleric.anm";
    write(path, source);
    check(is_anm_archive(path) && !is_anm_archive("UNKNOWN.ANM"), "ANM routing mismatch");
    const auto archive = read_anm(path);
    check(archive.gallery_count() == 2 && archive.frames[0].id == 1 && archive.frames[1].id == 2 &&
          archive.frames[0].width == 160 && archive.frames[0].height == 72 &&
          archive.frames[0].rgba[0] == 170 && archive.frames[1].rgba[0] == 0 &&
          archive.frames[1].rgba[6] == 170 && archive.frames[1].rgba[8] == 170 &&
          archive.frames[1].rgba[14] == 0 && archive.frames[1].rgba[18] == 0,
          "ANM frame order, run byte order, or XOR delta mismatch");
    source.pop_back();
    write(path, source);
    try {
        read_anm(path);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("ANM accepted a truncated delta");
}

void packwall(const fs::path& dir)
{
    constexpr unsigned widths[]{16, 16, 8, 8, 16, 24, 8, 16, 32, 56, 24};
    constexpr unsigned heights[]{88, 56, 32, 16, 16, 32, 8, 16, 32, 56, 16};
    Bytes record;
    for (unsigned i = 0; i < 11; ++i) {
        record.insert(record.end(), {0, static_cast<std::uint8_t>(heights[i] - 1),
                                     static_cast<std::uint8_t>(widths[i] / 8), 0});
        record.insert(record.end(), widths[i] * heights[i] / 2, i == 0 ? 0x40 : 0x20);
    }
    Bytes bytes;
    for (unsigned i = 0; i < 25; ++i)
        bytes.insert(bytes.end(), record.begin(), record.end());
    const auto path = dir / "packwall.out";
    write(path, bytes);
    check(is_packwall_archive(path) && !is_packwall_archive("OTHER.OUT"), "PACKWALL routing mismatch");
    const auto archive = read_packwall(path);
    check(archive.gallery_count() == 25 && archive.frames.back().id == 25,
          "PACKWALL wall set IDs or count are incorrect");
    const auto& sheet = archive.frames.front();
    check(sheet.width == 272 && sheet.height == 96 && sheet.rgba[0] == 170 &&
          sheet.rgba[3] == 255 && sheet.rgba[7] == 0 && sheet.rgba[16 * 4 + 3] == 0 &&
          sheet.rgba[20 * 4 + 1] == 170, "PACKWALL sheet layout or transparency mismatch");
    auto amiga = bytes;
    for (unsigned set = 0; set < 25; ++set) {
        unsigned offset = set * static_cast<unsigned>(record.size());
        for (unsigned piece = 0; piece < 11; ++piece) {
            const unsigned length = widths[piece] * heights[piece] / 2;
            offset += 4;
            for (unsigned i = 0; i < length; i += 4) {
                std::fill_n(amiga.begin() + offset + i, 4, 0);
                amiga[offset + i + (piece == 0 ? 2 : 1)] = 0xaa;
            }
            offset += length;
        }
    }
    const auto planar = dir / "PACKWALL.OST";
    write(planar, amiga);
    check(is_packwall_archive(planar) && read_packwall(planar).frames.front().rgba == sheet.rgba,
          "Amiga wall bitplanes differ from the same DOS indices");
    bytes[2] = 0;
    write(path, bytes);
    try {
        read_packwall(path);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("PACKWALL accepted a zero-width perspective");
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
    check(geo_map_name(records[0]).empty(), "A reused map ID inherited a Pool of Radiance name");
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
    check(view.rgba[0] < 128 && view.rgba[0] != view.rgba[(87 * 88) * 4] &&
          view.rgba[3] == 255 && view.rgba[(87 * 88) * 4 + 3] == 255,
          "Perspective backdrop is missing distinct opaque ceiling and floor bands");
    const auto pixel = (20 * 88 + 40) * 4;
    check(view.rgba.at(pixel) == 173 && view.rgba.at(pixel + 1) == 0,
          "Front wall is missing from the perspective");

    WallBanks pc98;
    pc98[2].resize(50);
    for (auto& piece : pc98[2])
        piece = {3, 0, 16, 16, Bytes(16 * 16 * 4, 255)};
    auto& front = pc98[2][6];
    front.width = 112;
    front.height = 128;
    front.rgba.resize(front.width * front.height * 4);
    for (std::size_t at = 0; at < front.rgba.size(); at += 4) {
        front.rgba[at + 1] = 0;
        front.rgba[at + 2] = 0;
        front.rgba[at + 3] = 255;
    }
    map[8 * 16 + 8].walls[0] = 11;
    const auto slot_three = wall_perspective(map, pc98, 8, 8, 0);
    check(slot_three.width == 176 && slot_three.rgba.at((40 * 176 + 80) * 4 + 1) == 0,
          "PC-98 map preview collapsed empty wall slots");
    dax(dir / "8X8D5.DAX", 24, tile);
    dax(dir / "WALLDEF5.DAX", 24, Bytes(1560, 1));
    dax(dir / "ECL5.DAX", 3, {0x37, 0, 24, 0, 25, 0, 1});
    const auto castle = load_map_walls(dir / "GEO5.DAX", 4);
    check(castle[0].size() == 50 && castle[1].size() == 50 && castle[2].size() == 50,
          "Castle wall definitions did not fill successive slots");

    dax(dir / "ECL8.DAX", 16, {0x21, 0, 30, 0, 2, 0, 255,
                                0x37, 0, 1, 0, 255, 0, 255});
    const auto alternate = load_map_walls(dir / "GEO8.DAX", 30);
    check(alternate[0].size() == 50 && alternate[1].empty() && alternate[2].empty(),
          "A map loaded from a different script lost its wall bank or empty slots");

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

    const auto sounds = dir / "SOUNDS.GLB";
    write(sounds, Bytes{'G','L','I','B',0,0,0,34,0,1,0,0,'D','I','G','8',
                        0,0,0,24,0,0,0,34,0x1c,0xfa,0,0,0,3,0x80,0,0x7f,0xff});
    const auto audio = read_record_rows(sounds);
    check(audio.size() == 1 && has_field(audio[0], {"Sample rate (Hz)", "7418"}), "Lost Amiga sample rate");
    const auto wave = sample_wav(sounds, 0);
    check(wave.size() == 48 && wave[4] == 40 && wave[22] == 1 &&
          wave[24] == 0xfa && wave[25] == 0x1c && wave[34] == 8 && wave[40] == 3 &&
          wave[44] == 0 && wave[45] == 128 && wave[46] == 255 && wave[47] == 0,
          "DIG8 WAV PCM sign, length, rate, or padding changed");

    write(sounds, Bytes{'H','L','I','B',32,0,0,0,1,0,0,0,'D','I','G','4',
                        24,0,0,0,32,0,0,0,0x2b,0x77,0,0,0,1,0xf1,0xcc});
    const auto packed_audio = read_record_rows(sounds);
    const auto packed_wave = sample_wav(sounds, 0);
    check(packed_audio.size() == 1 && has_field(packed_audio[0], {"Encoding", "DOS DIG4"}) &&
          has_field(packed_audio[0], {"Sample rate (Hz)", "11127"}) &&
          packed_wave.size() == 46 && packed_wave[44] == 17 && packed_wave[45] == 255,
          "DOS DIG4 WAV must decode low nibble first and omit archive padding");

    const auto xmi = dir / "music.xmi";
    write(xmi, Bytes{'F','O','R','M',0,0,0,24,'X','M','I','D','E','V','N','T',0,0,0,11,
                     0xc0,4,0x90,60,100,10,6,4,0xff,0x2f,0,0});
    const auto sequences = read_record_rows(xmi);
    const auto midi = xmi_sequence_midi(xmi, 0);
    const Bytes expected{'M','T','h','d',0,0,0,6,0,0,0,1,0,60,'M','T','r','k',0,0,0,15,
                         0,0xc0,4,0,0x90,60,100,10,0x80,60,0,0,0xff,0x2f,0};
    check(sequences.size() == 1 && midi == expected, "XMIDI note duration, delta or MIDI header changed");

    const auto ships = dir / "ships2.dax";
    Bytes ship(108);
    ship[0] = 5;
    std::copy_n("Adder", 5, ship.begin() + 1);
    ship[16] = 150;
    ship[22] = 0x58;
    ship[23] = 2;
    ship[34] = 0xe8;
    ship[35] = 3;
    ship[55] = 255;
    ship[57] = 6;
    ship[60] = 0x83;
    ship[61] = 3;
    ship[62] = 2;
    ship[64] = 50;
    dax(ships, 41, ship);
    const auto fleet = read_record_rows(ships);
    check(fleet.size() == 1 && fleet[0].id == 41 && fleet[0].name == "Adder" &&
          has_field(fleet[0], {"Hull (current / max)", "600 / 1000"}) &&
          has_field(fleet[0], {"Reaction", "-1"}) &&
          has_field(fleet[0], {"Ship type", "Md cruiser"}) &&
          has_field(fleet[0], {"Mount 1", "Gyrocannon (crew)"}) &&
          has_field(fleet[0], {"Mount 1 (working / max)", "2 / 3"}) &&
          has_field(fleet[0], {"Mount 1 ammunition", "50"}), "SHIPS2.DAX ship fields changed");
}

void item_archives(const fs::path& dir)
{
    auto named = item();
    named[0] = 3;
    named[1] = 'A';
    named[2] = 'x';
    named[3] = 'e';
    dax(dir / "item3.dax", 4, named);
    const auto loose = read_record_rows(dir / "item3.dax");
    check(loose.size() == 1 && loose[0].id == 4 && loose[0].name == "Axe" &&
          has_field(loose[0], {"Magic bonus", "-1"}) &&
          record_kind("ITEMS.DAX") == RecordKind::none, "Wrong numbered item routing or fields");

    const auto path = dir / "ITEM0.DAX";
    Bytes compact(254 * 17);
    compact[0] = 1;
    compact[3] = 18;
    compact[9] = 21;
    compact[17] = 255;
    dax(path, 1, compact);
    const auto templates = read_record_rows(path);
    check(templates.size() == 254 && templates[0].name == "Long Sword" &&
          has_field(templates[0], {"Weight", "21"}) && templates[1].name == "Empty template",
          "Wrong late Gold Box item template layout");

    Bytes buck(127 * 16);
    buck[0] = 2;
    buck[6] = 128;
    buck[8] = 34;
    buck[11] = 5;
    dax(path, 1, buck);
    const auto gear = read_record_rows(path);
    check(gear.size() == 127 && has_field(gear[0], {"Equipped", "1"}) &&
          has_field(gear[0], {"Weight", "34"}) && has_field(gear[0], {"Value (credits)", "5"}),
          "Wrong Buck Rogers item template layout");

    const auto base_path = dir / "ITEMS";
    for (const unsigned header : {0, 2}) {
        Bytes base(header + 128 * 16);
        base[header + 16 + 4] = 0xfd;
        base[header + 16 + 13] = 0x40;
        write(base_path, base);
        const auto rows = read_record_rows(base_path);
        check(rows.size() == 128 && rows[1].id == 1 &&
              has_field(rows[1], {"File offset", std::to_string(header + 16)}) &&
              has_field(rows[1], {"Large target damage modifier", "-3"}) &&
              has_field(rows[1], {"Class usability mask", "64"}), "Wrong standalone Gold Box ITEMS layout");
    }
    const auto gateway_path = dir / "ITEMS.1";
    write(gateway_path, Bytes(2 + 128 * 24));
    const auto gateway = read_record_rows(gateway_path);
    check(gateway.size() == 128 && has_field(gateway[1], {"File offset", "26"}),
          "Wrong Gateway ITEMS.1 stride");
    Bytes buck_types(39 * 9);
    buck_types[9 + 6] = 0xfe;
    write(base_path, buck_types);
    const auto types = read_record_rows(base_path);
    check(types.size() == 39 && has_field(types[1], {"File offset", "9"}) &&
          has_field(types[1], {"Damage modifier", "-2"}), "Wrong Buck Rogers ITEMS layout");
}

void numbered_monsters(const fs::path& dir)
{
    auto older = character();
    older.resize(422);
    older[120] = 23;
    older[420] = 19;
    dax(dir / "MON1CHA.DAX", 2, older);
    auto later = character();
    later.resize(510);
    later[1] = 'O';
    later[2] = 'g';
    later[3] = 'r';
    later[178] = 44;
    later[508] = 33;
    dax(dir / "mon2cha.dax", 2, later);
    const auto old = read_record_rows(dir / "MON1CHA.DAX");
    const auto modern = read_record_rows(dir / "mon2cha.dax");
    check(old.size() == 1 && has_field(old[0], {"Maximum HP", "23"}) &&
          has_field(old[0], {"Current HP", "19"}) && modern[0].name == "Ogr" &&
          has_field(modern[0], {"Maximum HP", "44"}) && has_field(modern[0], {"Current HP", "33"}),
          "Wrong numbered monster record offsets");
    dax(dir / "MON2ITM.DAX", 2, {4, 0, 0, 18, 255, 0, 1, 0, 0, 7, 0, 2, 9, 0, 0, 0, 0});
    const auto items = read_record_rows(dir / "MON2ITM.DAX");
    check(items.size() == 1 && items[0].name == "Long Sword" &&
          has_field(items[0], {"Owner", "Ogr"}) && has_field(items[0], {"Magic bonus", "-1"}),
          "Wrong compact item layout or numbered companion lookup");
    dax(dir / "MON1SPC.DAX", 2, {51, 0x34, 0x12, 4, 1, 0, 0, 0, 0});
    const auto effects = read_record_rows(dir / "MON1SPC.DAX");
    check(effects.size() == 1 && effects[0].name == "Effect 51" &&
          has_field(effects[0], {"Owner", "Orc"}) && has_field(effects[0], {"Duration (stored)", "4660"}),
          "Wrong monster effect or numbered companion");
    auto silver = character();
    silver.resize(439);
    silver[112] = 34;
    silver[437] = 29;
    dax(dir / "MON3CHA.DAX", 2, silver);
    const auto blades = read_record_rows(dir / "MON3CHA.DAX");
    check(has_field(blades.at(0), {"Maximum HP", "34"}) &&
          has_field(blades.at(0), {"Current HP", "29"}), "Wrong Silver Blades monster offsets");
    auto inventory = item();
    inventory[46] = 50;
    inventory[47] = 108;
    inventory[48] = 0;
    inventory[49] = 50;
    dax(dir / "MON3ITM.DAT", 2, inventory);
    const auto silver_items = read_record_rows(dir / "MON3ITM.DAT");
    check(silver_items.size() == 1 && silver_items[0].name == "Bracers AC 4" &&
          has_field(silver_items[0], {"Owner", "Orc"}),
          "Wrong Silver Blades inventory name or DAT archive routing");

    auto krynn = character();
    krynn.resize(216);
    krynn[40] = 40;
    krynn[214] = 38;
    dax(dir / "MON4CHA.DAX", 2, krynn);
    const auto krynn_creatures = read_record_rows(dir / "MON4CHA.DAX");
    check(krynn_creatures.size() == 1 && has_field(krynn_creatures[0], {"Maximum HP", "40"}) &&
          has_field(krynn_creatures[0], {"Current HP", "38"}), "Wrong Death Knights monster layout");
    std::vector<std::uint8_t> spellbook(220);
    spellbook[0] = 15;
    spellbook[1] = 0x80 | 47;
    spellbook[74 + 46] = 1;
    dax(dir / "MON4WIZ.DAX", 2, spellbook);
    const auto spell_rows = read_record_rows(dir / "MON4WIZ.DAX");
    check(spell_rows.size() == 1 && spell_rows[0].name == "Orc" &&
          has_field(spell_rows[0], {"Memorization slot 1", "Spell 15 - Magic Missile (ready)"}) &&
          has_field(spell_rows[0], {"Memorization slot 2", "Spell 47 - Fireball (pending rest)"}) &&
          has_field(spell_rows[0], {"Known spell", "Spell 47 - Fireball"}), "Wrong Death Knights spell data");
    inventory[46] = 68;
    inventory[47] = 112;
    inventory[49] = 68;
    dax(dir / "MON4ITM.DAX", 2, inventory);
    const auto krynn_items = read_record_rows(dir / "MON4ITM.DAX");
    check(krynn_items.size() == 1 && krynn_items[0].name == "Solamnic Plate +2" &&
          has_field(krynn_items[0], {"Owner", "Orc"}),
          "Wrong Death Knights inventory name or owner");

    auto champions = character();
    champions.resize(409);
    champions[98] = 52;
    champions[407] = 49;
    dax(dir / "MON5CHA.DAT", 2, champions);
    const auto champions_rows = read_record_rows(dir / "MON5CHA.DAT");
    check(has_field(champions_rows.at(0), {"Maximum HP", "52"}) &&
          has_field(champions_rows.at(0), {"Current HP", "49"}),
          "Wrong Champions of Krynn monster fields");
    inventory[46] = 75;
    inventory[47] = 76;
    inventory[49] = 75;
    dax(dir / "MON5ITM.DAT", 2, inventory);
    const auto champions_items = read_record_rows(dir / "MON5ITM.DAT");
    check(champions_items.size() == 1 && champions_items[0].name == "Dust Disappearance" &&
          has_field(champions_items[0], {"Owner", "Orc"}),
          "Wrong Champions of Krynn inventory name or DAT companion");
    dax(dir / "MON5SPC.DAT", 2, {51, 0x34, 0x12, 4, 1, 0, 0, 0, 0});
    const auto champions_effects = read_record_rows(dir / "MON5SPC.DAT");
    check(champions_effects.size() == 1 && has_field(champions_effects[0], {"Owner", "Orc"}),
          "Wrong Champions of Krynn effect archive routing");

    std::vector<std::uint8_t> dark_queen(0x19e + 2 + 5 + 17);
    std::copy_n("Wyvern", 7, dark_queen.begin() + 0x68);
    dark_queen[0x89] = 33;
    dark_queen[0x19e] = 1;
    dark_queen[0x19f] = 1;
    dark_queen[0x1a0] = 51;
    dark_queen[0x1a5] = 18;
    std::vector<std::uint8_t> packed{static_cast<std::uint8_t>(dark_queen.size() >> 8),
                                     static_cast<std::uint8_t>(dark_queen.size())};
    for (std::size_t at = 0; at < dark_queen.size(); at += 128) {
        const auto count = std::min<std::size_t>(128, dark_queen.size() - at);
        packed.push_back(static_cast<std::uint8_t>(count - 1));
        packed.insert(packed.end(), dark_queen.begin() + at, dark_queen.begin() + at + count);
    }
    auto library = tlb_library("HLIB", {{1, 0, 42, 0, 1, 0}, packed}, true);
    std::copy_n("DATA", 4, library.begin() + 12);
    write(dir / "MONCHA.LIB", library);
    const auto queen_rows = read_record_rows(dir / "MONCHA.LIB");
    check(queen_rows.size() == 1 && queen_rows[0].id == 42 && queen_rows[0].name == "Wyvern" &&
          has_field(queen_rows[0], {"Maximum HP", "33"}) &&
          has_field(queen_rows[0], {"Effect ID 1", "51"}) &&
          has_field(queen_rows[0], {"Item type 1", "18"}), "Wrong Dark Queen HLIB monster data");

    auto buck = character();
    buck.resize(263);
    buck[0x45] = buck[0xe3] = 44;
    buck[0x46] = buck[0xe4] = 1;
    dax(dir / "MON0CHA.DAX", 2, buck);
    const auto rogers = read_record_rows(dir / "MON0CHA.DAX");
    check(has_field(rogers.at(0), {"Maximum HP", "300"}) &&
          has_field(rogers.at(0), {"Current HP", "300"}), "Wrong Buck Rogers monster HP width");
}

void vault(const fs::path& dir)
{
    Bytes bytes(16 + 50 * 18, 255);
    std::fill_n(bytes.begin(), 16 + 3 * 18, 0);
    bytes[0] = 2;
    bytes[2] = 1;
    bytes[4] = 9;
    bytes[8] = 7;
    bytes[12] = bytes[13] = 255;
    bytes[14] = 2;
    bytes[16] = 1;
    bytes[18] = 18;
    bytes[19] = 26;
    bytes[22] = bytes[23] = 255;
    bytes[24] = 255;
    bytes[34] = 'I';
    bytes[47] = 1;
    bytes[52] = 2;
    bytes[55] = 240;
    const auto path = dir / "vaulta.dat";
    write(path, bytes);
    check(record_kind(path) == RecordKind::vault && record_kind("VAULT.DAT") == RecordKind::vault,
          "Vault file routing mismatch");
    const auto rows = read_record_rows(path);
    check(rows.size() == 4 && has_field(rows[0], {"Platinum pieces", "65538"}) &&
          has_field(rows[0], {"Gems", "9"}) && has_field(rows[0], {"Jewels", "7"}),
          "Wrong vault currency or record count");
    check(rows[1].name == "Fine Long Sword" && has_field(rows[1], {"Magic bonus", "-1"}) &&
          has_field(rows[1], {"Value", "-1"}) &&
          rows[2].name == "Bundle" && rows[3].name == "[240]" &&
          has_field(rows[3], {"Part of bundle record", "2"}), "Wrong FRUA item names or bundle layout");

    bytes[14] = 0;
    write(path, bytes);
    check(read_record_rows(path).size() == 1, "Empty vault should still show money and ignore padding");
    bytes[14] = 51;
    write(path, bytes);
    try {
        read_record_rows(path);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("Accepted a FRUA vault with too many items");
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

    // The two Buck Rogers SaveLoad units serialize byte banks; Gateway and
    // Curse serialize word banks plus the active ECL. Both end in String[40]s.
    for (unsigned size : {2901u, 13149u}) {
        Bytes party(size);
        party[size == 2901 ? 2561 : 12801] = 11;
        const auto roster = size - 328;
        party[roster - 1] = 1;
        party[roster] = 3;
        party[roster + 1] = 'O';
        party[roster + 2] = 'R';
        party[roster + 3] = 'C';
        const auto slot = folder / "SAVGAMB.DAT";
        write(slot, party);
        check(!is_saved_world(slot), "Later DOS save sent to Pool map decoder");
        const auto rows = read_record_rows(slot);
        check(rows.size() == 2 && rows[1].name == "Orc" &&
              has_field(rows[0], {"Map X", "11"}), "Later DOS party roster or position lost");
    }
}
}

namespace {
void ecl_scripts(const fs::path& dir)
{
    Bytes script{0x88, 0x13};
    for (unsigned i = 0; i < 5; ++i)
        script.insert(script.end(), {1, 1, 0x14, 0x99});
    script.insert(script.end(), {0x16, 0x2d, 1, 6, 8, 0x11, 0x80, 3, 0x04, 0x20, 0xc4, 0, 0xff});
    const auto c64 = dir / "ECL1E.PRG";
    write(c64, script);
    const auto rows = read_record_rows(c64);
    check(rows.size() == 1 && rows[0].id == 30 && has_field(rows[0], {"Profile", "c64"}),
          "C64 PRG lost hexadecimal script ID or dialect");
    check(has_field(rows[0], {"$9915", "$2D  CALL byte[$0806] [Redraw view frame]"}) &&
          has_field(rows[0], {"$9919", "$11  PRINT \"ABCD\""}) &&
          has_field(rows[0], {"$9920", "DATA FF"}), "C64 VM decoding or branch traversal disagrees with fixture");
    std::ostringstream listing;
    write_ecl_listing(listing, rows.front());
    const auto text = listing.str();
    check(text.find("Script 30\n") == 0 &&
          text.find("$9915\t$2D  CALL byte[$0806] [Redraw view frame]\n") != std::string::npos &&
          text.find("$9919\t$11  PRINT \"ABCD\"\n") != std::string::npos &&
          text.find("$9920\tDATA FF\n") != std::string::npos, "ECL text listing dropped script content");
    const auto por = dir / "ECL1.DAX";
    dax(por, 30, script);
    const auto pc = read_ecl_rows(por, "por");
    check(pc.size() == 1 && has_field(pc[0], {"$9915", "$2D  CALL byte[$0806]"}),
          "A C64 native call name leaked into the DOS dialect");
    for (unsigned i = 0; i < 5; ++i)
        script[5 + 4 * i] = 0x80;
    script.resize(22);
    script.insert(script.end(), {0x08, 4, 0x78, 0x56, 0x34, 0x12, 5, 0, 0x90, 0});
    dax(por, 7, script);
    for (auto dialect : {"countdown", "matrix"}) {
        const auto buck = read_ecl_rows(por, dialect);
        check(has_field(buck[0], {"$8014", "$08  RANDOM $12345678, long[$9000]"}),
              "Buck Rogers long operands were not decoded");
    }
    Bytes dispatch;
    for (unsigned i = 0; i < 5; ++i)
        dispatch.insert(dispatch.end(), {1, 1, 0x14, 0x99});
    dispatch.insert(dispatch.end(), {0x21, 0, 42, 0, 2, 0, 255,
        0x2f, 1, 0x4f, 0xc0, 0, 127, 1, 0x79, 0x6e,
        0x25, 1, 0x79, 0x6e, 0, 2, 1, 0x30, 0x99, 1, 0x31, 0x99,
        0, 0});
    const auto mapped = dir / "ECL42.DAX";
    dax(mapped, 7, dispatch);
    const auto link = find_ecl_event(mapped, 42, 1);
    const auto mapped_rows = read_ecl_rows(mapped);
    check(link && link->script == 0 && !link->inferred && link->instruction &&
          mapped_rows[0].fields.at(*link->instruction).first == "$9931",
          "Map link did not follow the masked event table in a differently numbered script");
    check(!find_ecl_event(mapped, 9, 1), "Map link guessed an unrelated script");
    const auto buck_dir = dir / "buck_rogers";
    fs::create_directories(buck_dir);
    script[0] = 0;
    script[1] = 0x40;
    script.resize(22);
    script.insert(script.end(), {0x1f, 1, 0x61, 0x4c, 0, 1, 0, 0x32, 0, 0x12, 0});
    const auto buck_file = buck_dir / "ECL22.PRG";
    write(buck_file, script);
    const auto buck = read_record_rows(buck_file);
    check(has_field(buck[0], {"Profile", "countdown"}) &&
          has_field(buck[0], {"$8014", "$1F  SHIP_COMBAT byte[$4C61], $01, $32, $12"}) &&
          has_field(buck[0], {"$801E", "$00  EXIT"}), "C64 Countdown ship combat lost operand alignment");
    script[0] = 0x88;
    script[1] = 0x13;
    dax(por, 34, script);
    for (auto dialect : {"countdown", "matrix"})
        check(has_field(read_ecl_rows(por, dialect)[0], {"$801E", "$00  EXIT"}),
              "DOS ship combat lost operand alignment");
    const auto amiga_path = buck_dir / "ECL.GLB";
    const auto glib_size = static_cast<std::uint8_t>(34 + script.size() - 2);
    Bytes amiga{'G','L','I','B', 0,0,0,glib_size, 0,2,0,1, 'D','A','T','A',
                0,0,0,28, 0,0,0,34, 0,0,0,glib_size, 0,1,0,34,0,1};
    amiga.insert(amiga.end(), script.begin() + 2, script.end());
    write(amiga_path, amiga);
    const auto amiga_rows = read_record_rows(amiga_path);
    check(amiga_rows.size() == 1 && amiga_rows[0].id == 34 &&
          has_field(amiga_rows[0], {"Platform", "Amiga"}) &&
          has_field(amiga_rows[0], {"$801E", "$00  EXIT"}), "Amiga GLIB directory or script offsets disagree");

    Bytes japanese{0x88, 0x13};
    for (unsigned i = 0; i < 5; ++i)
        japanese.insert(japanese.end(), {1, 1, 0x14, 0x99});
    japanese.insert(japanese.end(), {0x11, 0x80, 0x82, 0xa0, 0x6f, 0, 0});
    const auto pc98 = dir / "ECL2.DAX";
    Bytes packed{18,0,3,0,0,0,0,29,0,32,0,4,32,0,0,0,29,0,30,0,5,0,29};
    packed.insert(packed.end(), japanese.begin(), japanese.end());
    packed.push_back(0xff);
    packed.insert(packed.end(), japanese.begin(), japanese.end());
    write(pc98, packed);
    const auto localized = read_ecl_rows(pc98);
    check(localized.size() == 2 && has_field(localized[0], {"Platform", "PC-98"}) &&
          has_field(localized[0], {"$9914", "$11  PRINT \"\xe3\x81\x82\xe3\x83\x89\""}) &&
          has_field(localized[1], {"$991A", "$00  EXIT"}),
          "PC-98 DAX literal/raw records or zero-terminated Shift-JIS broke ECL alignment");
}
}

namespace {
void c64_assets(const fs::path& dir)
{
    Bytes bitmap(8002), colors(2002);
    bitmap[1] = colors[1] = 0x10;
    bitmap[2] = 0x1b; // Four selector values in one multicolour bitmap byte.
    colors[2] = 7;
    colors[1002] = 0x12;
    const auto title_bits = dir / "T0";
    const auto title_colors = dir / "T1.PRG";
    write(title_bits, bitmap);
    write(title_colors, colors);
    check(is_c64_picture(title_bits) && is_c64_picture(title_colors), "Missing C64 Hillsfar title routing");
    const auto title = read_c64_picture(title_bits);
    const auto& pixels = title.frames.at(0).rgba;
    check(title.frames[0].width == 320 && title.frames[0].height == 200 &&
          pixels[0] == 46 && pixels[8] == 0 && pixels[16] == 129 && pixels[24] == 237 &&
          pixels == read_c64_picture(title_colors).frames[0].rgba, "C64 Hillsfar tile or title color order changed");
    Bytes credit_data(6221);
    credit_data[1] = 0x20;
    credit_data[2] = 0x1b;
    credit_data[3050] = 15;
    const auto credits = dir / "CR";
    write(credits, credit_data);
    const auto credit = read_c64_picture(credits);
    const auto& colors_out = credit.frames.at(0).rgba;
    check(is_c64_picture(credits) && colors_out[0] == 46 && colors_out[8] == 196 &&
          colors_out[16] == 129 && colors_out[24] == 237, "C64 Hillsfar credits colour order changed");

    Bytes picture(58);
    picture[1] = 0x50;
    picture[2] = 'A';
    picture[3] = 'B';
    picture[34] = 6;
    picture[35] = 12;
    picture[47] = picture[48] = picture[51] = 1;
    picture[49] = 8;
    picture[55] = 5;
    picture.insert(picture.end(), {1, 0x40, 0, 1, 8, 0, 1, 0x40, 0, 0});
    const auto pic = dir / "PIC02.PRG";
    write(pic, picture);
    const auto gallery = read_c64_picture(pic);
    check(gallery.frames.size() == 2 && gallery.clips.size() == 1 &&
          gallery.frames[0].rgba[0] == 86 && gallery.frames[0].rgba[4] == 86 &&
          gallery.frames[1].rgba[0] == 0 && gallery.frames[1].rgba[3] == 255 &&
          gallery.clips[0].steps[1].duration_ms == 200,
          "C64 multicolour or XOR animation regression");

    Bytes item{0x10,0x27, 1,0,0,0, 0xff,0x80,0x87,0x80, 0x34,0x12,1,0,0,0,0,0};
    const auto items = dir / "ITEMFILE02.PRG";
    write(items, item);
    const auto rows = read_record_rows(items);
    check(rows.size() == 1 && rows[0].id == 2 && has_field(rows[0], {"Weight","4660"}) &&
          has_field(rows[0], {"Magic bonus","-1"}) && has_field(rows[0], {"Hidden name mask","7"}),
          "C64 native item record offsets or flags disagree");
    const auto numbered_items = dir / "item10.prg";
    item[0] = 0;
    item[1] = 0x40;
    write(numbered_items, item);
    const auto numbered_rows = read_record_rows(numbered_items);
    check(numbered_rows.size() == 1 && numbered_rows[0].id == 0x10 &&
          numbered_rows[0].fields == rows[0].fields, "C64 numbered item bank routing");

    Bytes map(1026);
    map[1] = 4;
    map[2] = 0x10;
    const auto alternate = dir / "GE031.PRG";
    write(alternate, map);
    const auto view = read_c64_picture(alternate);
    check(is_c64_picture(alternate) && view.frames.size() == 1 &&
          view.frames[0].id == 0x31 && view.frames[0].width == 193 &&
          view.frames[0].rgba[0] == 255, "Gateway alternate map routing or wall pixel");

    picture[1] = 0x40;
    write(pic, picture);
    check(read_c64_picture(pic).frames[0].rgba == gallery.frames[0].rgba,
          "Relocated C64 picture changed original pixels");
    Bytes names(516);
    names[1] = 0x40;
    names[258] = 0xa0;
    names[514] = 'X';
    const auto names_path = dir / "ITEMNAMES.PRG";
    write(names_path, names);
    const auto components = read_record_rows(names_path);
    check(components.size() == 256 && components.front().name == "X", "Relocated C64 name pointer mismatch");

    Bytes walls{0, 0x40};
    for (unsigned value : {32u, 1u}) {
        for (unsigned left = 780; left;) {
            const unsigned count = std::min(left, 127u);
            walls.push_back(static_cast<std::uint8_t>(128 + count));
            walls.push_back(static_cast<std::uint8_t>(value));
            left -= count;
        }
        walls.push_back(0);
    }
    const auto wall_path = dir / "WALLDEF07.PRG";
    write(wall_path, walls);
    const auto views = read_c64_picture(wall_path);
    check(views.frames.size() == 50 && views.frames.front().id == 7 &&
          views.frames.front().rgba[3] == 255, "Later C64 wall planes did not assemble");

    Bytes encounter(850);
    encounter[1] = 0x40;
    encounter[2] = encounter[66] = 0x80;
    for (unsigned part = 0; part < 12; ++part) {
        encounter[770 + part * 6] = 60;
        encounter[771 + part * 6] = 70;
    }
    const std::array<std::uint8_t, 12> placement{60, 70, 1, 1, 0, 2, 61, 71, 0, 0, 0, 7};
    std::copy(placement.begin(), placement.end(), encounter.begin() + 770);
    const auto encounter_path = dir / "SPRITE01.PRG";
    write(encounter_path, encounter);
    const auto sprites = read_c64_picture(encounter_path);
    const auto& far = sprites.frames.at(0);
    check(sprites.frames.size() == 15 && far.width == 48 && far.height == 42 &&
          far.rgba[0] == 129 && far.rgba[(far.width + 1) * 4] == 129 && far.rgba[11] == 0,
          "Later C64 encounter placement, expansion or hardware sprite priority changed");

    Bytes spell_art(1994);
    spell_art[1] = 0x40;
    for (unsigned tile = 250; tile < 394; tile += 18)
        std::fill_n(spell_art.begin() + tile, 9, 32);
    spell_art[394 + 7 * 64] = 0x80;
    spell_art[394 + 8 * 64] = 0x40;
    spell_art[394 + 10 * 64] = 0x80;
    const auto spell_bank = dir / "SPELLE66.PRG";
    const auto spell_overlay = dir / "SPELLE65.PRG";
    write(spell_bank, spell_art);
    write(spell_overlay, Bytes{0,0x40,0xad,0,0x7d,0x29,7,0xc9,1});
    const auto spell = read_c64_picture(spell_overlay);
    const auto& sparkle = spell.clips.at(3);
    const auto& bolt = spell.clips.at(4);
    const auto& first_sparkle = spell.frames.at(sparkle.steps.at(0).frame);
    const auto& second_sparkle = spell.frames.at(sparkle.steps.at(1).frame);
    const auto& first_bolt = spell.frames.at(bolt.steps.at(0).frame);
    const auto& second_bolt = spell.frames.at(bolt.steps.at(1).frame);
    int duration = 0;
    for (const auto& step : bolt.steps)
        duration += step.duration_ms;
    check(sparkle.steps.size() == 32 && bolt.steps.size() == 32 && bolt.realtime &&
          first_sparkle.width == 48 && first_sparkle.rgba[4] == 117 &&
          second_sparkle.rgba[4 + 3] == 0 && second_sparkle.rgba[12] == 117 &&
          first_bolt.rgba[4] == 255 && second_bolt.rgba[8] == 129 &&
          duration >= 92 && duration <= 95 &&
          read_c64_picture(spell_bank).clips.size() == spell.clips.size(),
          "C64 animated spell order, palette cycling or PAL flight timing changed");

    Bytes buck_art(3198);
    buck_art[1] = 0x40;
    for (unsigned tile = 250; tile < 430; tile += 18)
        std::fill_n(buck_art.begin() + tile, 9, 32);
    buck_art[736] = 0x80;
    write(spell_bank, buck_art);
    const auto primary = read_c64_picture(spell_bank);
    for (const auto* name : {"SPELLE67.PRG", "SPELLE68.PRG", "SPELLE69.PRG"}) {
        const auto path = dir / name;
        write(path, buck_art);
        const auto gallery = read_c64_picture(path);
        check(is_c64_picture(path) && gallery.gallery_count() == 37 && gallery.clips.empty() &&
              gallery.frames[10].rgba == primary.frames[10].rgba, "Buck Rogers alternate artwork bank");
    }
}
}

static void ilbm(const fs::path& dir)
{
    auto fixture = [](unsigned masking, bool packed, const Bytes& pixels) {
        Bytes file{'F','O','R','M',0,0,0,0,'I','L','B','M'};
        auto chunk = [&](std::string_view id, const Bytes& data) {
            file.insert(file.end(), id.begin(), id.end());
            const auto n = static_cast<unsigned>(data.size());
            for (unsigned shift : {24, 16, 8, 0})
                file.push_back(static_cast<std::uint8_t>(n >> shift));
            file.insert(file.end(), data.begin(), data.end());
            if (n & 1)
                file.push_back(0);
        };
        Bytes header{0,3,0,1,0,0,0,0,1,0,0,0,0,0,1,1,0,3,0,1};
        header[9] = static_cast<std::uint8_t>(masking);
        header[10] = packed;
        chunk("BMHD", header);
        chunk("ANNO", {'x'});
        chunk("CMAP", {0,0,0,0xf0,0x90,0x80});
        chunk("BODY", pixels);
        const auto n = static_cast<unsigned>(file.size() - 8);
        for (unsigned i = 0; i < 4; ++i)
            file[4 + i] = static_cast<std::uint8_t>(n >> (24 - 8 * i));
        return file;
    };
    const auto masked = dir / "masked.LBM";
    write(masked, fixture(1, true, {128,1,0xa0,0, 255,0x60}));
    check(is_picture_archive(masked), "ILBM was not routed to the picture gallery");
    const auto image = read_pictures(masked);
    const auto& frame = image.frames.at(0);
    check(frame.width == 3 && frame.height == 1 &&
          frame.rgba == Bytes{255,153,136,0, 0,0,0,255, 255,153,136,255},
          "ILBM scanline padding, palette, or mask changed");
    const auto keyed = dir / "keyed.iff";
    write(keyed, fixture(2, false, {0xa0,0}));
    check(read_pictures(keyed).frames.at(0).rgba ==
          Bytes{255,153,136,255, 0,0,0,0, 255,153,136,255},
          "ILBM transparent colour was not applied");
    write(masked, fixture(1, true, {253,0}));
    bool rejected = false;
    try {
        read_pictures(masked);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "ILBM accepted a ByteRun1 run crossing bitplane rows");
}

static void frua_records(const fs::path& dir)
{
    const auto items = dir / "item.dat";
    const auto base = dir / "ITEMS.DAT";
    Bytes base_data(128 * 16);
    base_data[16 + 1] = 2;
    const auto standalone_base = dir / "standalone" / "ITEMS.DAT";
    fs::create_directories(standalone_base.parent_path());
    write(standalone_base, base_data);
    check(read_record_rows(standalone_base).size() == 128, "FRUA base table must open independently");
    write(base, base_data);
    for (const unsigned stride : {17, 18}) {
        Bytes data(254 * stride);
        auto start = data.begin() + stride;
        start[0] = 1;
        start[3] = 18;
        start[4] = 44;
        start[6] = 75;
        start[8] = 255;
        write(items, data);
        for (const auto& path : {items, base}) {
            const auto rows = read_record_rows(path);
            check(rows.size() == 254 && rows[1].name == "Long Sword" &&
                  has_field(rows[1], {"Price", "75"}) &&
                  has_field(rows[1], {"Magic bonus", "-1"}) &&
                  has_field(rows[1], {"Hands required", "2"}), "FRUA item companion or stride failed");
        }
    }

    const auto design = dir / "GAME001.DAT";
    Bytes settings(387);
    std::copy_n("Design", 6, settings.begin());
    settings[34] = 1;
    settings[48] = 4;
    std::copy_n("Silver Key", 10, settings.begin() + 52);
    write(design, settings);
    const auto campaign = read_record_rows(design);
    check(campaign.size() == 1 && campaign[0].name == "Design" &&
          has_field(campaign[0], {"Starting experience", "65536"}) &&
          has_field(campaign[0], {"Key 1", "Silver Key"}), "FRUA design offsets failed");

    const auto geo_dat = dir / "GEO001.DAT";
    constexpr unsigned grid = 322, encounter_tag = grid + 24 * 24 * 6;
    Bytes dungeon(encounter_tag + 8 + 100 * 20);
    dungeon[26] = 15;
    dungeon[27] = 38;
    std::copy_n("Keep", 4, dungeon.begin() + 142);
    dungeon[grid + 6 * 38 + 4] = 5;
    std::copy_n("ENCR", 4, dungeon.begin() + encounter_tag);
    dungeon[encounter_tag + 8 + 4 * 20] = 38;
    write(geo_dat, dungeon);
    const auto map = read_record_rows(geo_dat);
    check(map.size() == 572 && map.front().name == "Keep" && map[39].name == "Cell 0, 1" &&
          has_field(map[39], {"Event", "5"}) && map.back().name == "Event 5" &&
          has_field(map.back(), {"Event type", "38"}), "FRUA map offsets failed");

    const auto save = dir / "SAVGAMA.CSV";
    Bytes saved(131);
    saved[31] = 6;
    saved[65] = 12;
    write(save, saved);
    const auto party = read_record_rows(save);
    check(party.size() == 1 && has_field(party[0], {"Party size", "6"}) &&
          has_field(party[0], {"Dungeon column", "12"}), "FRUA binary saved party offsets failed");
}

static void luck_records(const fs::path& dir)
{
    const Bytes text{2,' ',3,13,10,12,13,10,5,' ',8,26,0};
    const auto dos = dir / "LUCK.DAX";
    dax(dos, 101, text);
    const auto size = static_cast<std::uint8_t>(34 + text.size());
    Bytes library{'G','L','I','B',0,0,0,size,0,2,0,1,'T','I','L','E',
                  0,0,0,28,0,0,0,34,0,0,0,size,0,1,0,101,0,1};
    library.insert(library.end(), text.begin(), text.end());
    const auto amiga = dir / "Luck.glb";
    write(amiga, library);
    for (const auto& path : {dos, amiga}) {
        const auto rows = read_record_rows(path);
        check(rows.size() == 1 && rows[0].id == 101 && rows[0].name == "Romance 101" &&
              has_field(rows[0], {"Line 1", "{stronger name} {weaker name}"}) &&
              has_field(rows[0], {"Page break", "Wait for Continue"}) &&
              has_field(rows[0], {"Line 2", "{party member name} {weaker pronoun}"}) &&
              rows[0].fields.size() == 5, "LUCK dialogue, page break or substitutions changed");
    }
}

void disk_images(const fs::path& dir)
{
    const auto contents = [](const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{});
    };
    const auto extract = [&](const char* filename, const Bytes& bytes, const char* folder) {
        const auto path = dir / filename;
        write(path, bytes);
        const auto destination = dir / folder;
        fs::create_directories(destination);
        // Existing matching cache contents can be left behind when the test is rerun.
        for (const auto& entry : fs::directory_iterator(destination))
            fs::remove_all(entry.path());
        extract_disk_image(path, destination);
        return destination;
    };

    Bytes d64(174848 + 683);
    constexpr unsigned directory = (17 * 21 + 1) * 256;
    d64[directory + 1] = 255;
    d64[directory + 2] = 0x82;
    d64[directory + 3] = 1;
    std::fill_n(d64.begin() + directory + 5, 16, 0xa0);
    std::copy_n("TITLEPG", 7, d64.begin() + directory + 5);
    d64[0] = 1;
    d64[1] = 1;
    std::fill_n(d64.begin() + 2, 254, 0x43);
    d64[257] = 3;
    d64[258] = 0x31;
    d64[259] = 0x32;
    auto expected = Bytes(254, 0x43);
    expected.insert(expected.end(), {0x31, 0x32});
    check(contents(extract("test.D64", d64, "commodore") / "TITLEPG.PRG") == expected,
          "D64 file chain, PRG naming or final sector length is incorrect");

    for (const bool fast : {false, true}) {
        Bytes adf(901120);
        const auto set = [&](unsigned block, unsigned offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i)
                adf[block * 512 + offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
        };
        const auto name = [&](unsigned block, std::string_view text) {
            adf[block * 512 + 432] = static_cast<std::uint8_t>(text.size());
            std::copy(text.begin(), text.end(), adf.begin() + block * 512 + 433);
        };
        set(0, 0, fast ? 0x444f5301 : 0x444f5300);
        set(0, 8, 880);
        set(880, 0, 2);
        set(880, 24, 881);
        set(880, 508, 1);
        set(881, 0, 2);
        set(881, 24, 882);
        set(881, 508, 2);
        name(881, "Data");
        set(882, 0, 2);
        set(882, 308, 883);
        set(882, 324, 3);
        set(882, 508, 0xfffffffdu);
        name(882, "ECL.DAX");
        if (!fast) {
            set(883, 0, 8);
            set(883, 12, 3);
        }
        const auto start = 883 * 512 + (fast ? 0 : 24);
        std::copy_n("ECL", 3, adf.begin() + start);
        const auto destination = fast ? extract("ffs.adf", adf, "ffs") : extract("ofs.adf", adf, "ofs");
        check(contents(destination / "Data" / "ECL.DAX") == Bytes{'E', 'C', 'L'},
              "ADF file data or subdirectory is incorrect");
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
        else if (name == "cmp") cmp(dir);
        else if (name == "packwall") packwall(dir);
        else if (name == "anm") anm(dir);
        else if (name == "vault") vault(dir);
        else if (name == "monsters") numbered_monsters(dir);
        else if (name == "items") item_archives(dir);
        else if (name == "ecl") ecl_scripts(dir);
        else if (name == "c64") c64_assets(dir);
        else if (name == "ilbm") ilbm(dir);
        else if (name == "frua-records") frua_records(dir);
        else if (name == "luck") luck_records(dir);
        else if (name == "disk-images") disk_images(dir);
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
