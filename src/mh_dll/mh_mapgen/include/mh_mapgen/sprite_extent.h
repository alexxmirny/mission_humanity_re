// mh_mapgen -- how far each object sprite draws beyond its anchor, per tileset tree bank (generated from the unpacked game
// banks 50-56: the opaque bounding box of each kind's sprite.bmp against its hotspot from meta.bin). Pixels; a tile is 32 px.
// Object art is anchored at (tile * 32 + px_x/px_y) and extends up/left of it by far more than a tile, so a placed object can
// be drawn over ground it does not stand on.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace mh_mapgen::detail {

struct SpriteExtent {
    std::int16_t up, left, right, down; // pixels beyond the anchor pixel
};

// diuna256 (bank 50): kinds 1..23
inline constexpr SpriteExtent EXT_50[23] = {
    {107, 25, 6, 0},
    {107, 23, 3, 2},
    {75, 37, 5, 3},
    {94, 10, 7, 4},
    {53, 48, 3, 6},
    {101, 42, 2, 2},
    {23, 13, 2, 2},
    {27, 9, 3, 1},
    {25, 12, 2, 0},
    {18, 7, 3, 1},
    {21, 7, 1, -1},
    {35, 20, 8, 5},
    {37, 20, 5, 3},
    {43, 14, 4, 3},
    {-15, -12, 40, 76},
    {48, 34, 15, 5},
    {22, 15, 4, 2},
    {48, 21, 2, 3},
    {37, 12, 2, 1},
    {44, 9, 2, 3},
    {38, 8, 3, 4},
    {-22, -8, 42, 85},
    {104, 25, 6, 3},
};

// jungle (bank 51): kinds 1..42
inline constexpr SpriteExtent EXT_51[42] = {
    {49, 18, 26, 2},
    {52, 26, 25, 2},
    {47, 23, 20, 1},
    {34, 16, 14, 2},
    {67, 30, 26, 1},
    {115, 66, 31, 0},
    {79, 34, 33, 2},
    {70, 30, 30, 2},
    {61, 16, 20, 1},
    {67, 30, 27, -1},
    {59, 30, 28, 2},
    {70, 9, 38, -2},
    {90, 35, 87, 3},
    {93, 35, 87, 3},
    {93, 35, 87, 3},
    {64, 37, 16, 2},
    {56, 34, 22, 1},
    {60, 28, 26, 2},
    {43, 17, 25, 1},
    {45, 15, 25, 2},
    {62, 29, 20, 2},
    {9, 3, 5, 1},
    {45, 14, 10, 2},
    {16, 4, 30, 2},
    {19, 7, 9, 2},
    {20, 22, 5, 0},
    {3, 2, 5, 3},
    {7, 17, 4, 3},
    {10, 17, 8, 0},
    {10, 27, 9, 2},
    {64, 73, -2, 5},
    {128, 49, 61, -1},
    {51, 31, 8, 0},
    {112, 76, 25, 7},
    {105, 20, 99, 1},
    {125, 180, 30, 18},
    {142, 24, 149, 153},
    {132, 161, 34, 0},
    {50, 10, 12, 0},
    {37, 6, 14, 2},
    {52, 28, 7, 1},
    {96, 12, 52, 1},
};

// ksie256 (bank 52): kinds 1..27
inline constexpr SpriteExtent EXT_52[27] = {
    {70, 58, 7, 10},
    {70, 37, 44, 2},
    {25, 9, 13, 1},
    {41, 55, 6, 2},
    {23, 25, 5, 4},
    {45, 27, 7, 2},
    {16, 19, 7, 3},
    {12, 21, 7, 6},
    {13, 12, 10, 2},
    {22, 14, 13, 2},
    {40, 47, 14, 1},
    {43, 48, 18, 4},
    {74, 42, 14, 4},
    {54, 29, 11, 1},
    {58, 45, 6, 3},
    {91, 58, 18, 1},
    {33, 38, 5, 3},
    {26, 17, 8, 1},
    {15, 15, 4, 2},
    {36, 17, 14, 0},
    {30, 18, 11, 2},
    {22, 10, 8, 2},
    {14, 6, 6, 3},
    {18, 16, 8, 2},
    {18, 5, 11, 2},
    {17, 7, 13, 1},
    {79, 21, 44, 2},
};

// lod256 (bank 53): kinds 1..13
inline constexpr SpriteExtent EXT_53[13] = {
    {84, 52, 4, 6},
    {76, 55, 2, 5},
    {52, 54, 16, 0},
    {106, 74, 11, -2},
    {87, 113, 9, 5},
    {67, 105, 18, -1},
    {72, 126, 42, 29},
    {85, 50, 41, 0},
    {88, 26, 30, 2},
    {144, 121, 13, 2},
    {69, 66, 13, 4},
    {57, 111, 2, 8},
    {77, 76, 9, 3},
};

// ruins256 (bank 54): kinds 1..17
inline constexpr SpriteExtent EXT_54[17] = {
    {32, 18, 17, -3},
    {39, 17, 22, 1},
    {79, 18, 21, 2},
    {35, 17, 18, -1},
    {65, 30, 33, 1},
    {28, 20, 12, 1},
    {29, 20, 14, -1},
    {39, 21, 20, -1},
    {39, 21, 14, -1},
    {33, 17, 22, -1},
    {45, 22, 14, -1},
    {88, 51, 0, -2},
    {182, 85, 5, 3},
    {172, 96, 92, 0},
    {68, 52, 44, -2},
    {168, 44, 83, 2},
    {217, 86, 4, 20},
};

// metal256 (bank 55): kinds 1..22
inline constexpr SpriteExtent EXT_55[22] = {
    {97, 34, 47, 1},
    {114, 49, 40, 5},
    {115, 50, 23, 2},
    {103, 43, 53, 0},
    {114, 53, 48, 4},
    {44, 12, 9, 2},
    {114, 52, 28, 2},
    {105, 47, 11, 2},
    {114, 137, 6, 9},
    {96, 114, 30, 15},
    {94, 60, 6, 3},
    {20, 5, 9, 3},
    {18, 2, 10, 1},
    {142, 129, 15, 1},
    {55, 51, 6, 2},
    {59, 46, 13, 12},
    {28, 26, 6, 2},
    {20, 20, 8, 2},
    {29, 13, 9, 3},
    {19, 29, 3, 1},
    {21, 21, 6, 5},
    {109, 148, 8, 12},
};

// skaly256 (bank 56): kinds 1..24
inline constexpr SpriteExtent EXT_56[24] = {
    {46, 23, 25, 1},
    {121, 39, 69, 1},
    {118, 53, 35, 3},
    {88, 42, 39, 2},
    {93, 49, 39, 0},
    {104, 47, 16, 3},
    {106, 76, 9, 2},
    {112, 70, 24, 6},
    {66, 48, 29, 5},
    {69, 45, 27, 2},
    {93, 45, 23, 4},
    {27, 16, 14, 2},
    {118, 61, 22, 4},
    {198, 94, 111, 2},
    {198, 94, 111, 2},
    {171, 44, 77, 3},
    {82, 37, 32, 2},
    {33, 16, 8, 2},
    {15, 15, 8, 2},
    {51, 21, 21, 3},
    {42, 17, 13, 1},
    {141, 73, 107, 2},
    {133, 171, 23, 4},
    {103, 69, 15, -1},
};

// Extent of `kind` of tileset `stem`; a kind past the table (or an unknown tileset) gets the bank maximum / false.
inline bool sprite_extent(const std::string &stem, std::int32_t kind, SpriteExtent &e) {
    if (stem == "diuna256") {
        e = kind >= 1 && kind <= 23 ? EXT_50[kind - 1] : SpriteExtent{107, 48, 42, 85};
        return true;
    }
    if (stem == "jungle") {
        e = kind >= 1 && kind <= 42 ? EXT_51[kind - 1] : SpriteExtent{142, 180, 149, 153};
        return true;
    }
    if (stem == "ksie256") {
        e = kind >= 1 && kind <= 27 ? EXT_52[kind - 1] : SpriteExtent{91, 58, 44, 10};
        return true;
    }
    if (stem == "lod256") {
        e = kind >= 1 && kind <= 13 ? EXT_53[kind - 1] : SpriteExtent{144, 126, 42, 29};
        return true;
    }
    if (stem == "ruins256") {
        e = kind >= 1 && kind <= 17 ? EXT_54[kind - 1] : SpriteExtent{217, 96, 92, 20};
        return true;
    }
    if (stem == "metal256") {
        e = kind >= 1 && kind <= 22 ? EXT_55[kind - 1] : SpriteExtent{142, 148, 53, 15};
        return true;
    }
    if (stem == "skaly256") {
        e = kind >= 1 && kind <= 24 ? EXT_56[kind - 1] : SpriteExtent{198, 171, 111, 6};
        return true;
    }
    return false;
}

} // namespace mh_mapgen::detail
