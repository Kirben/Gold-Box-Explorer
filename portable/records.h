#pragma once

#include "dax.h"

#include <string>
#include <utility>

namespace goldbox {
enum class RecordKind { none, monsters, items, base_items, inventory, monster_effects, saved_character,
    saved_inventory, saved_effects, vault, ecl, amiga_text, amiga_sound, c64,
    frua_items, frua_design, frua_geo, frua_save };
using Field = std::pair<std::string, std::string>;

struct RecordRow {
    unsigned id;
    unsigned slot;
    std::string name;
    std::vector<Field> fields;
};

RecordKind record_kind(const std::filesystem::path& path);
std::vector<RecordRow> read_record_rows(const std::filesystem::path& path);
}
