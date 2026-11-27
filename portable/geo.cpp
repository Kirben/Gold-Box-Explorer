#include "geo.h"

#include <algorithm>
#include <regex>
#include <stdexcept>

namespace goldbox {
bool is_geo_archive(const std::filesystem::path& path)
{
    static const std::regex name(R"(GEO[0-9]*\.DAX)", std::regex::icase);
    return std::regex_match(path.filename().string(), name);
}

bool has_geo_trailer(const std::vector<DaxRecord>& records)
{
    return std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 255 && record.bytes.size() == 5;
    }) && std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.id == 253 && !record.bytes.empty() && record.bytes.size() % 9 == 0;
    });
}

std::string_view geo_map_name(const DaxRecord& record)
{
    if (record.bytes.size() < 2)
        return {};
    return geo_map_name(record.id, record.bytes.data() + 2, record.bytes.size() - 2);
}

std::string_view geo_map_name(unsigned id, const std::uint8_t* planes, std::size_t size)
{
    // Key names to a map ID and the layout from the shipped data.
    // LOAD_FILES sends its map operand to View3D.Load3DMap. For the later
    // titles below, location dialogue in the associated ECL scripts
    // identifies the named maps.
    struct NamedMap {
        unsigned id;
        std::uint64_t geometry;
        std::string_view name;
    };
    static constexpr NamedMap names[]{
        // Pool of Radiance AreaName and ClueBook. Scripts 3, 16, 24 and 29
        // each cover two maps. These layouts match DOS, Amiga and PC-98.
        {0, 0x2efd1a5dde31b976ull, "New Phlan"},
        {1, 0x93d7956353ca12c0ull, "Buccaneers' Base"},
        {2, 0x87df1a042cb833d2ull, "Cadorna Textile House"},
        {3, 0xa52de94f53e4d994ull, "Valjevo Castle, Northwest"},
        {4, 0x586bf2a2dd42a9f3ull, "Valjevo Castle, Northeast"},
        {5, 0x08f8a2fa59966399ull, "Valjevo Castle, Southeast"},
        {6, 0x26a5afd8e4d3ec3bull, "Valjevo Castle, Southwest"},
        {7, 0xfad79045c6ae97ccull, "Valjevo Castle, Inner Tower"},
        {9, 0x855db2927dd14651ull, "Stojanow Gate"},
        {10, 0xa9e82fee745c40d6ull, "Valhingen Graveyard"},
        {13, 0x3d61c277645a5fa4ull, "Kobold Caves"},
        {14, 0xa0fedb57d8deb6c2ull, "Kovel Mansion"},
        {15, 0xba8033a357df7e5full, "Mendor's Library"},
        {16, 0x9ff818a6538c9a39ull, "Lizard Man Keep"},
        {17, 0x40bb5a951b5ff964ull, "Nomad Camp"},
        {18, 0xc8cfa133cb073e28ull, "Podal Plaza"},
        {20, 0x08735a43f5a7be3full, "Slums"},
        {21, 0xf46c681bb87d8884ull, "Sokal Keep"},
        {22, 0x600552efd8ed1ac3ull, "Sorcerer's Island, Level 1"},
        {23, 0x0fc68310390607f6ull, "Yarash's Pyramid"},
        {24, 0xf26f2e8df1aeccddull, "Temple of Bane"},
        {25, 0x6f612bc06fd86e68ull, "Western Wilderness"},
        {26, 0x4181e35a867f1fd2ull, "Central Wilderness"},
        {27, 0xa61d1d2d2ed9079bull, "Eastern Wilderness"},
        {28, 0x6ed970d77bd22bf2ull, "Outpost of Zhentil Keep"},
        {29, 0xb0d589e112d40017ull, "Kuto's Well"},
        {30, 0x88b39060cfd7cf80ull, "Lizard Man Catacombs"},
        {31, 0xf3085dda40f421ddull, "Wealthy Area"},
        {32, 0xdee6476b0531e73aull, "Kuto's Well Catacombs"},
        // Curse of the Azure Bonds (original ECL map loads and location dialogue).
        {1, 0x183a7f4c1add85bdull, "Tilverton and the Thieves' Guild"},
        {3, 0xd027a6d105782001ull, "Tilverton Sewers"},
        {4, 0x09cc8bfaf37d836cull, "Fire Knife Hideout"},
        {16, 0x42c43f2eb03766f3ull, "Yulash"},
        {32, 0xfd16106a6615cf79ull, "Temple of Bane, Zhentil Keep"},
        // Additional maps verified against the SSI clue-book layouts.
        {33, 0xa6ebb7c16bb5e9aeull, "Cave of the Beholder"},
        // Scripts 17/18 share the pit, and 49/50 share the village
        // and lava tubes where the party meets Crimdrac.
        {17, 0x1c5c4977764c0785ull, "Pit of Moander"},
        {37, 0xb57ab8bf3f665171ull, "Oxam's Tower"},
        {50, 0xb96670e005ad4d0cull, "Haptooth Village and the Cave of the Dracolich"},
        {51, 0xf0e429cac2e5ffe7ull, "The Wizard's Tower"},
        {64, 0x562133c9ce5422abull, "The Burial Glen"},
        // The overland scripts route several destinations to each of
        // these encounter maps. Use the setting described on entry.
        {21, 0xf7c3e38f71fe37b2ull, "War-Blasted City (Shared Encounter)"},
        {53, 0x88c72c5d07c126beull, "Caverns and Dark Pit (Shared Encounter)"},
        {69, 0x1ff7ce1a9da2fcdbull, "Ruins and River Pirates' Treasure (Shared Encounter)"},
        {66, 0x2449f20abdaec310ull, "Ruins of Myth Drannor"},
        {67, 0xdb987552e6010a12ull, "Grand Ruined Temple"},
        // Secret of the Silver Blades (original ECL map loads and location dialogue).
        {16, 0x90bd5a7a6400dd65ull, "New Verdigris"},
        {32, 0x9e3dc386026a4970ull, "Old Verdigris Ruins"},
        {33, 0x71265372116e16a7ull, "Well of Knowledge"},
        {34, 0x378b3918b766827eull, "Black Circle Headquarters"},
        // ECL 48 calls $8268 before starting ECL 49 or 50. The helper
        // loads GEO 48 at $826E. The two scripts dispatch depths 1-4 and 5-8.
        {48, 0x22745d479811291aull, "The Mines (Levels 1-8)"},
        // ECL 48 and 51 load GEO 49 and display the depth from $4C69.
        {49, 0x71548d0ac2015fc9ull, "The Mines (Wheel Lift and Temple Approach)"},
        {50, 0x207a679aadbfb477ull, "Temple of Tyr"},
        {64, 0x848ad9843ef2ae46ull, "Dungeon (Entrance from the Mines)"},
        {65, 0x222c4a160c40b35cull, "Dungeon (Middle Section)"},
        {66, 0x601e3f566d0192b7ull, "Dungeon (Exit to the Crevasses)"},
        {80, 0x441e472e7db58ab7ull, "The Crevasses"},
        {82, 0xbd404c2217eeced2ull, "The Crevasses (Castle Gates)"},
        {98, 0x194084309064acf6ull, "Dreadlord's Sanctum"},
        // Additional maps verified against the SSI clue-book layouts.
        {68, 0x9cba7b1cefc6b72bull, "Drider Base"},
        {81, 0x5d13a16665aacd63ull, "Frost Giant Village"},
        {96, 0xdb259b71879dfd14ull, "Castle Entrance"},
        {97, 0xa5689aebb134fca7ull, "Middle Level of the Castle"},
        // Champions of Krynn (original ECL map loads and location dialogue).
        {32, 0x56f2f87e73bbcc51ull, "Throtl"},
        {34, 0x10405be1e59f2d7dull, "Throtl Catacombs"},
        {48, 0x581a539013ff5e80ull, "Gargath"},
        {49, 0x63e936794c907776ull, "Gargath Keep"},
        {80, 0x46dcb71fcd0c64c3ull, "Sanction"},
        {81, 0x8017f4e3f386f4bfull, "Temple of Huerzyd"},
        {82, 0xa85fff379c42ae69ull, "Temple of Duerghast"},
        {96, 0xfe1a105350b49609ull, "Citadels"},
        {97, 0x435c93de974277fbull, "Kernen"},
        // Additional maps verified against the SSI clue-book layouts.
        {50, 0xa524e12a2d7239deull, "Jelek"},
        {64, 0x3180e68affe1005bull, "Neraka City and Base"},
        {66, 0x2198d66f9678eb64ull, "Neraka Prison"},
        {67, 0xfe613dd354328d18ull, "Tomb of Sir Dargaard"},
        {68, 0x2de9588039a26a96ull, "Southern Outpost"},
        {99, 0xf00c8f81e9ca0a9bull, "Ogre Base"},
        // Death Knights of Krynn (original ECL map loads and location dialogue).
        {32, 0x9aca38adb1b1b418ull, "Kalaman"},
        {33, 0x4cee9fc9f31952c5ull, "Vingaard"},
        {33, 0x60b4ede10691f94cull, "Vingaard"}, // English Amiga GEO1, ECL 33
        {34, 0xdae7ebfd747ccd7bull, "Cekos"},
        {50, 0x416dd6184b822aeaull, "Death Dragon's Lair"},
        {51, 0xfa56855ef45c1806ull, "Throtl Keep"},
        {64, 0xa6b18ce73c315770ull, "Graveyard"},
        {67, 0x112e4a845f33dcefull, "Turef"},
        {82, 0xe26489a514ce151bull, "Voice Wood"},
        {96, 0x913062671206a00dull, "Dargaard Keep, First Floor"},
        {98, 0xf700a37410831e08ull, "Dargaard Keep, Top Floor"},
        {99, 0xf92707e4d1e62d12ull, "Takhisis' Challenge"},
        // Additional maps verified against the SSI clue-book layouts.
        {36, 0xe5135cd9ca0b4c77ull, "Gargath Outpost"},
        {48, 0xabfcf7fedb8e0fa8ull, "High Clerist's Tower"},
        {49, 0x049eea4e1d76b276ull, "Tunnels Beneath the Tower"},
        {65, 0xb3714ff17dcb5f31ull, "Kuo-Toa Slave Ship"},
        {66, 0x57b1bc6ef5430fc8ull, "Gnome Village (Quazle)"},
        // Scripts 80, 81 and 84 load the same town geometry. Script 83
        // places four separate wilderness encounters in one GEO record.
        {80, 0x0e78b762241b0429ull, "Cerberus and Dulcimer"},
        {83, 0xc48de1904e9ca004ull, "Shipwreck, Cursed Village, Fun House and Father of Trees"},
        {97, 0xc66924fa957ba94dull, "Dargaard Keep, Second Floor"},
        // Gateway to the Savage Frontier (original ECL map loads and location dialogue).
        {1, 0x6ed0bd57056e8fd5ull, "Yartar"},
        {2, 0x53caec55061ebf83ull, "Kraken Complex"},
        {3, 0x617497f2dd54aad9ull, "Nesme"},
        {4, 0xaf52098d183fde90ull, "Silverymoon"},
        {5, 0x20428fdec60d513aull, "Everlund"},
        {6, 0xb75d604ef509a117ull, "Neverwinter"},
        {7, 0xa655ab3148f6389full, "Port Llast and the Gallant Prince"},
        {8, 0xeb0505f925bc9a29ull, "Luskan"},
        {9, 0xd0bde5059537f857ull, "Hosttower of the Arcane"},
        {10, 0x0bd904723589ab91ull, "Tuern"},
        {11, 0x2ad53b5fe2df2d76ull, "Gundarlun"},
        {13, 0x6bc9b8af1c8bcb41ull, "Kraken Headquarters"},
        {14, 0x165a74ca186ae566ull, "Secomber"},
        {15, 0x75cb1f5023fb6cddull, "Loudwater"},
        {16, 0x987e9f6f32df1921ull, "Llorkh"},
        {17, 0x608fee8c601e828dull, "Arena Beneath Llorkh"},
        {19, 0xdaa1ad7ec72969c5ull, "Sundabar"},
        {20, 0x2dd648a2f14ce2b8ull, "Ascore"},
        // ECL 21 $893D-$8B3F switches overland sectors at the edges.
        // The southern pair starts four columns east of the second northern sector.
        {21, 0xb99594e005e8b3d9ull, "Savage Frontier Overland (North, Far West)"},
        {22, 0x0ca52a4d2f4eaeccull, "Savage Frontier Overland (North, West Central)"},
        {23, 0x5d6b21e4136149b7ull, "Savage Frontier Overland (North, East Central)"},
        {24, 0xa051a3ec1fca7dd0ull, "Savage Frontier Overland (North, Far East)"},
        {25, 0x6c96bf4737033f56ull, "Savage Frontier Overland (Southwest)"},
        {26, 0x3e59c0377a52e770ull, "Savage Frontier Overland (Southeast)"},
        // ECL 27 loads each layout at two entry points on the way to Ascore's plaza.
        // The numbers distinguish GEO layouts in script order.
        {27, 0xee90d0553524dc77ull, "Ascore Maze (Layout 1)"},
        {28, 0xc4d32566a89d6e28ull, "Ascore Maze (Layout 2)"},
        {29, 0x488f2724d25f66ffull, "Ascore Maze (Layout 3)"},
        {30, 0x3008435be8de0e53ull, "Ascore Maze (Layout 4)"},
        // Additional maps verified against the SSI clue-book layouts.
        {12, 0x21d812d7be0d4075ull, "Purple Rocks"},
        {18, 0x8792a04bff1cb4e0ull, "The Star Mounts"},
        // Pools of Darkness (original ECL map loads and location dialogue).
        {18, 0x9fa9e040e0eedf25ull, "Limbo"},
        {19, 0x51ea78f24be1ef19ull, "Zhentil Keep and Mulmaster"},
        {21, 0x540fb492e97d1b1bull, "Hill Giant Steading"},
        {32, 0xc880dd675dc9d868ull, "Fire Giant Cave"},
        {33, 0x9f6a0b942abca154ull, "Dragons' Aerie"},
        {36, 0xf203283a14f63a52ull, "Beholder Cave"},
        {48, 0x157eac64c64c9deaull, "Manshoon's Tower"},
        {53, 0xfb22617e75b570e4ull, "Moander's Heart"},
        {64, 0xf597c511ffdd4318ull, "Dark Phlan"},
        {65, 0x1160df1efcfdfac9ull, "Palace of Gothmenes"},
        {81, 0xeaf4774d2c51f91eull, "Drow Testing Ground"},
        // Additional maps verified against the SSI clue-book layouts.
        {16, 0x8721258892d6b63aull, "Phlan (East)"},
        {17, 0xdc90b3727924f575ull, "Phlan (West)"},
        {22, 0x63a7017f6585daa5ull, "Sasha and the Lands of Thar"},
        {34, 0x9dd03e7bb84a98b7ull, "Thorne's Cave"},
        {37, 0x168dd7c4c173e57full, "Vala vs. Vaasa"},
        {66, 0x035e8de18e327258ull, "Nacacia and Myth Drannor"},
        {68, 0xb703a19a0ece4cceull, "Silk and the Slaves of the Drow"},
        {69, 0x11d812e5363a58b4ull, "Kalistes' Temple"},
        {82, 0x92400f60321ca335ull, "Kalistes' Parlor, Upper Level"},
        {84, 0xcc871f4486d62b08ull, "Kalistes' Parlor, Lower Level"},
        // ECL 1's jumper loads a test area with store, XP and combat selectors.
        {1, 0x6500d2c030596d86ull, "Playtester Town"},
        {20, 0x5197cedbab9d0484ull, "Overland Encounters"},
        {35, 0x5033cdc30a3d1910ull, "Descent to the Depths"},
        {39, 0x38e4ca671b18e1a9ull, "Dave's Challenge"},
        // GEO 49 shares 506 of its 512 wall bytes with GEO 33.
        {49, 0x17801d7a48d9ed1cull, "Dragons' Aerie (Alternate Layout)"},
        {50, 0x27efec8cf2d2714cull, "Temple of Tyr"},
        {52, 0x6f8c3f7442ba7e55ull, "Moander's Body"},
        // ECL 70 switches from GEO 70 to 71 when the level counter
        // reaches 6, and switches back when descending to 5.
        {70, 0x851a6b0d70d76c9aull, "Tower of Marcus (Lower Levels)"},
        {71, 0xb34bdab2cea8302cull, "Tower of Marcus (Middle Levels)"},
        {67, 0xdc46fb9840d5d74cull, "Tower of Marcus (Upper Levels)"},
        {83, 0x56d9170da7e37b83ull, "Web Dimension"},
        // Treasures of the Savage Frontier (original ECL map loads and location dialogue).
        {20, 0xc2b5d39f821162d5ull, "Leilon"},
        {24, 0x0ca1d29e73893d86ull, "Triboar"},
        {25, 0x0c6a03f8ee9b05c5ull, "Longsaddle"},
        {27, 0xe5b7098b3ae85147ull, "Orlumbor"},
        {33, 0x6ff359d52f451339ull, "Fireshear"},
        {34, 0x4c50325747d5f5a4ull, "Fireshear Mines"},
        // Additional maps verified against the SSI clue-book layouts.
        {16, 0x200bae319bc40efaull, "Llorkh"},
        {18, 0xfe5298ec9c5418e8ull, "Loudwater"},
        {19, 0x0014a115e3295c07ull, "Secomber"},
        {21, 0x05768bfd8c8c4e45ull, "Smuggler's Dock Near Waterdeep"},
        {22, 0xd863296d6cf9ad3cull, "Waterdeep and the Caverns"},
        {23, 0x8f1d3ffd5b4b2329ull, "Yartar"},
        {26, 0xb3726e8730bc9deeull, "Mintarn"},
        {28, 0xa451e7b96bbe62d8ull, "Neverwinter"},
        {29, 0xc38ae7a0c185ff66ull, "Port Llast"},
        {30, 0xd395c3ed58f75518ull, "Mirabar"},
        {31, 0x7e73aaf6cf95fe62ull, "Luskan"},
        {32, 0xcea99961fc10c6a6ull, "Ruathym"},
        {35, 0x1b921a87fcdd427dull, "Ice Peak, Aurilssbaarg"},
        {36, 0x5183f93ef84ba85bull, "Ice Peak, Bjorn's Hold"},
        {37, 0xd691bf9b63144cccull, "Ice Peak, Icewolf"},
        {38, 0xfc7333f1907fe9fdull, "Ice Peak, Freezefire's Lair"},
        {39, 0xe5a85db1fb411964ull, "Daggerford"},
        {40, 0xf8f00adf015e28feull, "Way Inn"},
        {47, 0xe6d7792a339c457full, "Farr Windward"},
        {48, 0xc3852ae40e96162aull, "Trackless Sea Tours"},
        // ECL 17 ($805B), 49 ($803A) and 50 ($804C) select these
        // geometries through LOAD_WALL_SETS during area initialization.
        {17, 0xd3dbce6b751e208eull, "Lord Geildarr's Keep"},
        {49, 0xc8d7ceffd02e944eull, "Tower of Twilight"},
        {50, 0x8abe4ffe20d3361eull, "The Crossroads"},
        // Town exits store the overland GEO ID at $02EE before starting
        // ECL 51, which passes it to LOAD_FILES at $809A.
        {51, 0xbf4f3ce4026f6a0aull, "Overland: Luskan and Port Llast"},
        {52, 0xa91ec9f95b5a3463ull, "Overland: Mirabar and Longsaddle"},
        {53, 0xe1cd1ce41655e670ull, "Overland: Neverwinter and Leilon"},
        {54, 0x1fbd648ee5433890ull, "Overland: Triboar and Yartar"},
        {55, 0xebff5d3807d2849aull, "Overland: Waterdeep Area"},
        {56, 0xde5693e7ee39948bull, "Overland: Secomber, Daggerford and Way Inn"},
        {57, 0xee32a46309a51e38ull, "Overland: Loudwater and Llorkh"},
        // ShowItemMap loads the effect byte of type $10 items. Names come
        // from ITEM0 slots 172, 173, 171, 174 and 185 and itemNameParts.
        {58, 0xaaffcc6f30300f2eull, "Firedock Map"},
        {59, 0x9e349820ae636bbaull, "Treasure Map"},
        {60, 0x88d9324db1e6402bull, "Tunnels of Orlumbor"},
        {61, 0x2030acbbb8c1c08bull, "Luskan High Captains"},
        {62, 0x5fb58f094981fa19ull, "Farms of Longsaddle"},
        // Buck Rogers: Countdown to Doomsday (English Amiga ECL and GEO).
        {16, 0x4a5c6f46c5a801b3ull, "Chicagorg Spaceport"},
        // SSI clue book maps B-E and the corresponding ECL area entries.
        {32, 0x7ac9be42f88b2ed4ull, "Spyship"},
        {35, 0x8e7b0da78624e442ull, "Damaged Asteroid Installation"},
        {48, 0xcaa0d26b930edf05ull, "Asteroid Base (Level One)"},
        {49, 0x81c257f5c7600e82ull, "Asteroid Base (Level Two)"},
        // ECL 50 loads $32 during boarding and $33 for the captivity sequence.
        {50, 0xf5a5caf0a90165d7ull, "Talon's Pirate Ship (Boarding)"},
        {51, 0xca5da028670e9365ull, "Talon's Pirate Ship (Holding Cell)"},
        {52, 0xc6cf090f3e44231full, "Ship Boarding Encounter"},
        {65, 0x99c64fc119e44b6aull, "Desert Runner Village"},
        {66, 0x76421452b7a36e1aull, "Gradivus Mons RAM Base"},
        {67, 0x7b53c5b0eafcd9e1ull, "Talon's Secret Asteroid Base"},
        // ECL 81 and 83 both load GEO 81 for clue book areas J and K.
        {81, 0x660fd84de1c78c10ull, "Lowlander Village and Venusian Space Elevator Ruins"},
        {82, 0x31f87e6812730dacull, "Venus RAM Base"},
        // ECL 95 and 99 load this layout immediately before combat.
        {99, 0x9cd6d5bc2f6b0aeaull, "Shared Combat Map"},
        {96, 0x8099832734583d5bull, "Mariposa Embarkation Section"},
        {97, 0x2d20e3238e60a845ull, "Mariposa"},
        {98, 0x156934b12a8107a8ull, "Mariposa Weapons Control"},
        // Matrix Cubed, DOS GEO1.DAX. ECL LOAD_FILES selects each layout.
        // The Venus lab elevator switches 50/51 and the rig hatch switches 114/115.
        {1, 0x83926fcf196e780dull, "Playtest Jumper"},
        {17, 0x8f4a3304e619037dull, "Caloris Space Port"},
        {18, 0x99771f4da5b02ceeull, "NEO Mission Briefing"},
        {21, 0xc23e6391cab164fbull, "Ship Boarding Encounter"},
        {33, 0xe9aae649ba83643eull, "Losangelorg Sprawls (The Warren)"},
        {34, 0xcb16e3e8097cd5e2ull, "Historical Museum"},
        {35, 0xbcbce28fad0fa9c9ull, "Ceres Asteroid Base"},
        {38, 0x54e8f08309859ca7ull, "Tower Isle and Radio Station"},
        {49, 0x9c29c1a8f5e1afc7ull, "Lowlander Village"},
        {50, 0x7569c413477f5231ull, "Venus Laboratory"},
        {51, 0x6950fd9348a4e99bull, "Venus Laboratory (Lower Level)"},
        {52, 0xcecefa5976aeef40ull, "Lowlander Mines"},
        {64, 0x83926fcf196e780dull, "Luna Base"},
        {65, 0x23a994915c4c378full, "Tsai Weaponry Labs"},
        {66, 0x07fa9ff33a4a3ac8ull, "Killer Kane's Rogue and the RAM Battler Deimos"},
        {80, 0xea8b01db4a7830e8ull, "Mars Prison (Surface)"},
        {81, 0xf0fc162e4ebf1a08ull, "PURGE Headquarters"},
        {82, 0x9b525967f77054eaull, "NEO Installation"},
        {84, 0x96a0846ea8d4c135ull, "Mars Prison (Interior)"},
        {96, 0xc7eddff0d70d8a24ull, "Living Ship (First Layout)"},
        {97, 0x22590a839ae93ac1ull, "Living Ship (Second Layout)"},
        {112, 0x2a7a3472907c6472ull, "Stormrider University"},
        {113, 0xb6b25cb434026e53ull, "Genetics Foundation Building"},
        {114, 0x46ae25826312a1b4ull, "Jupiter Mining Rig"},
        {115, 0x20a8e9461f90ebf2ull, "Jupiter Mining Rig (Aircar)"},
    };
    constexpr unsigned plane_size = map_side * map_side;
    if (size != 4 * plane_size)
        return {};
    // Event flags and header bytes can change independently of the map layout.
    std::uint64_t geometry = 14695981039346656037ull;
    for (unsigned i = 0; i < 4 * plane_size; ++i) {
        if (i / plane_size == 2)
            continue;
        geometry ^= planes[i];
        geometry *= 1099511628211ull;
    }
    const auto candidate = std::find_if(std::begin(names), std::end(names), [&](const auto& map) {
        return map.id == id && map.geometry == geometry;
    });
    return candidate == std::end(names) ? std::string_view{} : candidate->name;
}

GeoMap read_geo_map(const DaxRecord& record)
{
    constexpr auto plane_size = map_side * map_side;
    if (record.bytes.size() < 2 + plane_size * 4)
        throw std::runtime_error("GEO record is too short for a 16x16 map");

    GeoMap map{};
    for (std::size_t i = 0; i < map.size(); ++i) {
        const unsigned ne = record.bytes[2 + i];
        const unsigned sw = record.bytes[2 + plane_size + i];
        const unsigned event = record.bytes[2 + 2 * plane_size + i];
        const unsigned door = record.bytes[2 + 3 * plane_size + i];
        map[i] = {{ne >> 4, ne & 15, sw >> 4, sw & 15},
                  {door & 3, (door >> 2) & 3, (door >> 4) & 3, door >> 6},
                  event & 127, (event & 128) != 0};
    }
    return map;
}
}
