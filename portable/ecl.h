#pragma once

#include "records.h"
#include <iosfwd>
#include <optional>
#include <string_view>

namespace goldbox {
struct EclEventLink {
    std::size_t script;
    std::optional<std::size_t> instruction;
    bool inferred = false;
};

std::optional<EclEventLink> find_ecl_event(const std::filesystem::path& archive, unsigned map_id,
                                           unsigned event);
bool is_ecl_archive(const std::filesystem::path& path);
std::vector<RecordRow> read_ecl_rows(const std::filesystem::path& path, std::string_view profile = {});
void write_ecl_listing(std::ostream& output, const RecordRow& script);
}
