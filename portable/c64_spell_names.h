#pragma once
#include <array>
#include <cstdint>

namespace goldbox::c64_spells {
// CAMP indexes split low/high address tables by spell ID.
// PRG file offsets (high/low, including load header), with the overlay at $8000:
// Champions ECL65 $0e8e/$0f3f; Curse and Gateway ECL65 $0fea/$1094;
// Death Knights CODE01 $11f4/$127a; Silver Blades ECL65 $11a6/$1258.
// Empty entries omit blanks and combat messages. Keep native spellings and aliases.
inline constexpr const char* names[]{
    "", "BLESS", "CURSE", "CURE LIGHT WOUNDS",
    "CAUSE LIGHT WOUNDS", "DETECT MAGIC", "PROTECTION FROM EVIL", "PROTECTION FROM GOOD",
    "RESIST COLD", "BURNING HANDS", "CHARM PERSON", "ENLARGE",
    "FRIENDS", "MAGIC MISSILE", "READ MAGIC", "SHIELD",
    "SHOCKING GRASP", "SLEEP", "FIND TRAPS", "HOLD PERSON",
    "RESIST FIRE", "SILENCE 15' RADIUS", "SLOW POISON", "SNAKE CHARM",
    "SPIRITUAL HAMMER", "DETECT INVISIBILITY", "INVISIBILITY", "KNOCK",
    "MIRROR IMAGE", "RAY OF ENFEEBLEMENT", "STINKING CLOUD", "STRENGTH",
    "CURE BLINDNESS", "CAUSE BLINDNESS", "CURE DISEASE", "CAUSE DISEASE",
    "DISPEL MAGIC", "PRAYER", "REMOVE CURSE", "BESTOW CURSE",
    "BLINK", "FIREBALL", "HASTE", "INVISIBILITY 10' RADIUS",
    "LIGHTNING BOLT", "PROTECTION FROM EVIL 10' RADIUS", "PROTECTION FROM GOOD 10' RADIUS", "PROTECTION FROM NORMAL MISSILES",
    "SLOW", "CURE SERIOUS WOUNDS", "CAUSE SERIOUS WOUNDS", "NEUTRALIZE POISON",
    "POISON", "STICKS TO SNAKES", "CHARM MONSTERS", "CONFUSION",
    "DIMENSION DOOR", "FEAR", "FIRE SHIELD", "FUMBLE",
    "ICE STORM", "MINOR GLOBE OF INVULNERABLITY", "BLINDNESS", "DISEASE",
    "PROTECTION FROM DRAGON BREATH", "PROTECTION FROM PARALYSIS", "REDUCE", "CURE CRITICAL WOUNDS",
    "CAUSE CRITICAL WOUNDS", "DISPEL EVIL", "FLAME STRIKE", "RAISE DEAD",
    "SLAY LIVING", "ENTANGLE", "FAERIE FIRE", "INVISIBILITY TO ANIMALS",
    "CLOUD KILL", "CONE OF COLD", "FEEBLEMIND", "HOLD MONSTERS",
    "BARKSKIN", "CHARM PERSON OR MAMMAL", "RESURRECTION", "RESTORATION",
    "HARM", "HEAL", "DEATH SPELL", "GIANT STRENGTH",
    "DISINTEGRATE", "GLOBE OF INVULNERABILITY", "STONE TO FLESH", "FLESH TO STONE",
    "MINOR GLOBE OF INVULNERABILITY", "IRON SKIN", "FIRE TOUCH", "DELAYED BLAST FIREBALL",
    "MASS INVISIBILITY", "POWER WORD STUN", "MASS CHARM", "MIND BLANK",
    "OTTO'S IRRESISTABLE DANCE", "POWER WORD BLIND", "TRIP", "GLOBE OF INVULNERABLITY",
    "MASS INVISIBLITY",
};

inline constexpr std::array<std::uint8_t, 128> champions{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 5, 11, 0, 12, 13,
    6, 7, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 32, 33, 34, 35, 36, 37, 38, 39, 40, 36, 41,
    42, 19, 43, 44, 45, 46, 47, 48, 49, 49, 49, 50, 50, 50, 50, 1,
    1, 1, 50, 51, 52, 45, 53, 54, 54, 54, 54, 54, 54, 5, 54, 54,
    54, 54, 55, 56, 57, 58, 59, 60, 61, 38, 32, 0, 0, 0, 0, 1,
    1, 1, 0, 0, 39, 45, 21, 5, 38, 1, 10, 9, 62, 63, 64, 65,
};

inline constexpr std::array<std::uint8_t, 128> azure{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 5, 11, 66, 12, 13,
    6, 7, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 32, 33, 34, 35, 36, 37, 38, 39, 40, 36, 41,
    42, 19, 43, 44, 45, 46, 47, 48, 49, 49, 49, 50, 50, 50, 50, 1,
    1, 1, 50, 51, 52, 45, 53, 67, 68, 69, 70, 71, 72, 5, 73, 74,
    75, 54, 55, 56, 57, 58, 59, 60, 61, 38, 32, 76, 77, 78, 79, 1,
    1, 1, 0, 0, 39, 62, 63, 64, 65,
};

inline constexpr std::array<std::uint8_t, 128> death{
    0, 1, 80, 3, 81, 5, 6, 82, 8, 9, 10, 5, 11, 12, 12, 13,
    6, 83, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 72, 32, 33, 34, 84, 36, 37, 38, 39, 40, 36, 41,
    42, 19, 43, 44, 45, 85, 47, 48, 86, 62, 49, 87, 88, 64, 1, 89,
    1, 1, 90, 51, 52, 45, 53, 67, 91, 69, 70, 71, 72, 5, 73, 74,
    75, 54, 55, 56, 57, 58, 59, 60, 92, 38, 93, 76, 77, 78, 79, 1,
    1, 1, 94, 1, 39, 45, 21, 5, 38, 1, 10, 9, 95, 96, 97, 98,
    99, 100, 101,
};

inline constexpr std::array<std::uint8_t, 128> blades{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 5, 11, 66, 12, 13,
    6, 7, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 85, 32, 33, 34, 35, 36, 37, 38, 39, 40, 36, 41,
    42, 19, 43, 44, 45, 46, 47, 48, 84, 49, 49, 50, 50, 50, 50, 50,
    50, 50, 50, 51, 52, 45, 53, 67, 68, 69, 70, 71, 72, 5, 73, 74,
    75, 54, 55, 56, 57, 58, 59, 60, 61, 38, 80, 76, 77, 78, 79, 81,
    81, 0, 3, 0, 39, 102, 102, 102, 102, 102, 102, 102, 102, 86, 86, 88,
    103, 90, 91, 95, 104, 97, 62, 63, 64,
};

}
