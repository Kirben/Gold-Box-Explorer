#pragma once

#include <array>
#include <string_view>

namespace goldbox {
namespace ecl {
struct Opcode {
    const char* name = nullptr;
    unsigned count = 0;
    bool list = false;
};
using Opcodes = std::array<Opcode, 256>;

// Counts follow the original VM handlers, using the legacy Explorer mnemonics.
inline Opcodes opcodes(std::string_view profile)
{
    constexpr Opcode early[]{
        {"EXIT",0},{"GOTO",1},{"GOSUB",1},{"COMPARE",2},{"ADD",3},{"SUBTRACT",3},{"DIVIDE",3},{"MULTIPLY",3},
        {"RANDOM",2},{"SAVE",2},{"LOAD_CHARACTER",1},{"LOAD_MONSTER",3},{"SETUP_MONSTER",3},{"APPROACH",0},
        {"PICTURE",1},{"INPUT_NUMBER",2},{"INPUT_STRING",2},{"PRINT",1},{"PRINT_CLEAR",1},{"RETURN",0},
        {"COMPARE_AND",4},{"VERTICAL_MENU",3,true},{"IF_EQ",0},{"IF_NE",0},{"IF_LT",0},{"IF_GT",0},
        {"IF_LE",0},{"IF_GE",0},{"CLEAR_MONSTERS",0},{"PARTY_STRENGTH",1},{"CHECK_PARTY",6},{"RESERVED_1F",2},
        {"NEW_ECL",1},{"LOAD_FILES",3},{"PARTY_SURPRISE",2},{"SURPRISE",4},{"COMBAT",0},
        {"ON_GOTO",2,true},{"ON_GOSUB",2,true},{"TREASURE",8},{"ROB",3},{"ENCOUNTER_MENU",14},
        {"GET_TABLE",3},{"HORIZONTAL_MENU",2,true},{"PARLAY",6},{"CALL",1},{"DAMAGE",5},{"AND",3},{"OR",3},
        {"SPRITE_OFF",0},{"FIND_ITEM",1},{"PRINT_RETURN",0},{"CLOCK",1},{"SAVE_TABLE",3},{"ADD_NPC",2},
        {"LOAD_PIECES",3},{"PROGRAM",1},{"WHO",1},{"DELAY",0},{"SPELL",3},{"PROTECTION",1},{"CLEAR_BOX",0}
    };
    Opcodes result{};
    for (unsigned i = 0; i < sizeof early / sizeof *early; ++i)
        result[i] = early[i];
    if (profile == "por" || profile == "amiga" || profile == "c64") {
        if (profile == "por")
            result[0x80] = {"USE_UP_ITEM",1}; // PC port extension.
        return result;
    }
    result[0x1f] = {};
    result[0x34].count = 2;
    result[0x3e] = {"DUMP",0};
    result[0x3f] = {"FIND_SPECIAL",1};
    result[0x40] = {"DESTROY_ITEMS",1};
    if (profile != "curse")
        result[0x41] = {"AWARD_EXPERIENCE",2};
    if (profile == "champions" || profile == "secret")
        result[0x3c].count = 0;
    if (profile == "gateway") {
        result[0x32] = {"FIND_MATCHING_ITEM",4};
        result[0x40] = {"TAKE_ITEM",4};
    }
    if (profile == "dk" || profile == "pod" || profile == "treasures" || profile == "dqk") {
        const bool dk = profile == "dk";
        result[0x0c].count = 4;
        result[0x1d] = {"CONTINUE_PROMPT",0};
        result[0x1f] = {"JOURNAL_ENTRY",1};
        result[0x21].count = dk ? 3 : 2;
        result[0x22] = {"YES_NO",0};
        result[0x23] = {"REDRAW_EXIT",0};
        result[0x27] = {"TREASURE",4,true};
        result[0x29] = {"NOOP",0};
        result[0x2c] = {"SOUND",1};
        result[0x31] = {"HORIZONTAL_MENU_WITH_PROMPT",2,true};
        if (!dk)
            result[0x37] = {"FIND_NPC",2};
        result[0x3c] = {"SHOP",1};
        if (profile == "dqk") {
            result[0x3e] = {"REMOVE_CHARACTER",0};
            result[0x3f] = {"HAS_EFFECT",1};
        }
        if (profile == "treasures") {
            result[0x42] = {"LOAD_WALL_SETS",4};
            result[0x43] = {"FIND_MATCHING_ITEM",4};
            result[0x44] = {"TAKE_ITEM",4};
        }
    }
    if (profile == "countdown" || profile == "matrix") {
        result[0x06].name = "MULTIPLY";
        result[0x07].name = "DIVIDE";
        result[0x0c].count = 4;
        result[0x1d] = {"SET_SPEAKER",2};
        result[0x1f] = {"SHIP_COMBAT",4}; // Operands are read in ShipCbt.ShipCombat.
        result[0x22] = {"SKILL_CHECK",3};
        result[0x23] = {"SKILL_CHECK_TEXT",3};
        result[0x27] = {"TREASURE",2,true};
        result[0x29] = {"CONTINUE_PROMPT",0};
        result[0x2c] = {"YES_NO",0};
        result[0x31] = {"HORIZONTAL_MENU_WITH_PROMPT",2,true};
        result[0x3b] = {};
        result[0x3c] = {"NOOP",0};
        result[0x3f] = {"LOGBOOK_ENTRY",2};
        result[0x40].count = 2;
        constexpr Opcode extra[]{
            {"CLEAR_AND_STOP",0},{"PLAY_SOUND",1},{"SKIP_OPCODE",0},{"DISTANCE_AHEAD",2},
            {"LOOP_INITIALIZE",2},{"LOOP_ADVANCE",0},{"TRANSFER_CHARACTER_LISTS",1},
            {"SKILL_DAMAGE",6},{"SOLO_COMBAT",0},{"SHOP",1},{"SET_PICTURE_MODE",2}
        };
        for (unsigned i = 0; i < sizeof extra / sizeof *extra; ++i)
            result[0x42 + i] = extra[i];
    }
    return result;
}
}
}
