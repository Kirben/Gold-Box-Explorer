#include "pictures.h"
#include "tlb.h"
#include "vace.h"
#include "cmp.h"
#include "dax.h"
#include "records.h"
#include "save_map.h"

#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: goldbox-inspect ARCHIVE [ARCHIVE ...]\n";
        return 2;
    }
    int status = 0;
    for (int arg = 1; arg < argc; ++arg) {
        try {
            if (goldbox::is_saved_world(argv[arg])) {
                const auto save = goldbox::read_saved_map(argv[arg]);
                std::cout << argv[arg] << ": disk " << save.disk << ", script " << save.script
                          << ", map " << save.map_id << ", location type " << save.location
                          << ", position " << save.x << ',' << save.y << ", facing NESW index "
                          << save.facing << '\n';
                if (save.original && save.copied)
                    std::cout << save.changed.count() << " changed cells\n";
                if (save.visited)
                    std::cout << save.visited->count() << " visited cells\n";
                continue;
            }
            if (goldbox::record_kind(argv[arg]) != goldbox::RecordKind::none) {
                const auto rows = goldbox::read_record_rows(argv[arg]);
                std::cout << argv[arg] << ": " << rows.size() << " decoded rows\n";
                for (const auto& row : rows) {
                    std::cout << row.id << ':' << row.slot + 1 << '\t' << row.name;
                    for (const auto& field : row.fields)
                        std::cout << '\t' << field.first << '=' << field.second;
                    std::cout << '\n';
                }
                continue;
            }
            const bool tlb = goldbox::is_tlb_archive(argv[arg]);
            const bool glb = goldbox::is_geo_glb(argv[arg]);
            const bool cmp = goldbox::is_cmp_archive(argv[arg]);
            const bool vace = goldbox::is_vace_archive(argv[arg]);
            if (!tlb && !glb && !cmp && !vace && !goldbox::is_picture_archive(argv[arg])) {
                const auto records = goldbox::read_dax(argv[arg]);
                std::cout << argv[arg] << ": " << records.size() << " records\n";
                for (const auto& record : records)
                    std::cout << record.id << ' ' << record.stored_size << " stored, "
                              << record.bytes.size() << " decoded\n";
                continue;
            }
            const auto archive = tlb ? goldbox::read_tlb(argv[arg])
                : glb ? goldbox::read_geo_glb(argv[arg])
                : cmp ? goldbox::read_cmp(argv[arg])
                : vace ? goldbox::read_vace(argv[arg]) : goldbox::read_pictures(argv[arg]);
            std::cout << argv[arg] << ": " << archive.gallery_count() << (glb ? " maps" : " frames");
            if (!archive.skipped_ids.empty()) {
                std::cout << " (skipped record IDs:";
                for (const auto id : archive.skipped_ids)
                    std::cout << ' ' << id;
                std::cout << ')';
            }
            std::cout << '\n';
            for (const auto& clip : archive.clips) {
                if (clip.whole_archive)
                    std::cout << "Ending:";
                else
                    std::cout << "Animation " << clip.id << ':';
                for (const auto& step : clip.steps) {
                    if (clip.whole_archive) {
                        const auto& frame = archive.frames[step.frame];
                        std::cout << ' ' << frame.id << '.' << frame.index + 1;
                    } else if (step.frame < archive.gallery_count())
                        std::cout << ' ' << archive.frames[step.frame].index + 1;
                    else
                        std::cout << " clear";
                    std::cout << '@' << step.duration_ms << "ms";
                }
                std::cout << '\n';
            }
            for (std::size_t i = 0; i < archive.gallery_count(); ++i) {
                const auto& frame = archive.frames[i];
                std::uint64_t hash = 14695981039346656037ull;
                for (auto byte : frame.rgba) {
                    hash ^= byte;
                    hash *= 1099511628211ull;
                }
                std::cout << frame.id << ':' << frame.index << ' ' << frame.width << 'x'
                          << frame.height << ' ' << std::hex << hash << std::dec << '\n';
            }
        } catch (const std::exception& error) {
            std::cerr << argv[arg] << ": " << error.what() << '\n';
            status = 1;
        }
    }
    return status;
}
