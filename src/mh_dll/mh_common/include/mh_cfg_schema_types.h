#pragma once
namespace mh::mods::cfg::schema {
enum class Kind { Int,
                  F32,
                  Str,
                  Flag,
                  SlotStr,
                  SlotInt,
                  SlotF32,
                  Cap,
                  Res,
                  Depend,
                  Area,
                  BankRange,
                  Source4,
                  BankSprite };
struct Keyword {
    const char *cls;
    const char *keyword;
    Kind        kind;
    const char *field;
};
struct Seed {
    const char *cls;
    const char *field;
    int         slot;
    const char *value;
};
struct Cap {
    const char *cls;
    const char *field;
    const char *mode;
    int         limit;
};
struct Ref {
    const char *cls;
    const char *field;
    const char *universe;
};
struct BuildingKind {
    const char *name;
    int         value;
};
struct HousingReason {
    int reason;
    int alien;
    int human;
};
struct Transform {
    const char *cls;
    const char *field;
    const char *mul;
    const char *add;
    bool        integer;
};
struct Modifier {
    const char *keyword;
    Kind        kind;
    double      initial;
};
struct Domain {
    const char *cls;
    const char *field;
    int         min;
    int         max;
};
} // namespace mh::mods::cfg::schema
