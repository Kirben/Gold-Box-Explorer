#pragma once

#include "dax.h"

#include <string>
#include <utility>

namespace goldbox {
enum class RecordKind { none, monsters, items, inventory, saved_character, saved_inventory, saved_effects };
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
