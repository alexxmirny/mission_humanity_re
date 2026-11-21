#include "mh_cfg_patch.h"
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace {
using namespace mh::mods::cfg;
int  checks = 0, failures = 0;
void ck(bool b, const char *s) {
    ++checks;
    if (!b) {
        ++failures;
        std::printf("  FAIL: %s\n", s);
    }
}
std::vector<std::uint8_t> wide(std::string_view s) {
    std::vector<std::uint8_t> v{0xff, 0xfe};
    for (unsigned char c : s) {
        v.push_back(c);
        v.push_back(0);
    }
    return v;
}
std::vector<std::uint8_t> wide_base(std::string_view s) {
    std::string t = "DEFINE \"L73\" 73\nDEFINE \"L74\" 74\nDEFINE \"L75\" 75\nDEFINE \"L76\" 76\nTEXT \"L73\" \"A\"\nTEXT \"L74\" \"B\"\nTEXT \"L75\" \"C\"\nTEXT \"L76\" \"D\"\n";
    t += s;
    return wide(t);
}
std::string ascii(std::span<const std::uint8_t> s) {
    std::string r;
    if (s.size() < 2 || s[0] != 0xff || s[1] != 0xfe) return {};
    for (std::size_t i = 2; i + 1 < s.size(); i += 2) {
        if (s[i + 1]) return {};
        r.push_back(static_cast<char>(s[i]));
    }
    return r;
}
std::span<const std::uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()}; }
Input                         input(std::string_view path, std::string_view contents, std::uint32_t mod) { return {path, path, bytes(contents), mod}; }
} // namespace
int run_cfgpatchtest() {
    auto               base = wide_base("DEFINE \"SPRITE_ZWYKLY\" 1\nDEFINE \"SOLDIERTYPE_UNKNOWN\" 0\nDEFINE \"metal\" 1\nDEFINE \"unitcode\" 1\nDEFINE \"T\" 12\nUNIT \"u\"\n TYPE \"unitcode\"\n ENERGY 11\n RESOURCE 5 \"metal\"\nEND\nUNIT \"u\"\n TYPE \"unitcode\"\n ENERGY 13\nEND\nPROGRESS \"p\"\nEND\n");
    auto               lang = wide("TEXT \"T\" \"base\"\n");
    std::string        p1   = "@patch UNIT \"u\" INDEX 1 ENERGY 20 RESOURCE 7 \"metal\" RESOURCE 8 \"metal\" END\n";
    std::string        p2   = "@patch UNIT \"u\" INDEX 1 ENERGY 30 END\n";
    std::string        en = "TEXT \"T\" \"english\"\n", ru = "TEXT \"T\" \"russian\"\n";
    std::vector<Input> in{input("cfg\\patch.cfg", p2, 1), input("lang\\ru.cfg", ru, 0), input("cfg\\patch.cfg", p1, 0), input("lang\\en.cfg", en, 0)};
    auto               r = prepare(base, lang, in, "ru");
    auto               a = ascii(r.init), t = ascii(r.lang);
    ck(r.ok && !r.passthrough, "two ordered patches accepted");
    ck(a.find("ENERGY 30\r\n") != std::string::npos && a.find("ENERGY 13\r\n") != std::string::npos, "mod order wins and duplicate index isolates record");
    ck(a.find("RESOURCE 7 \"metal\"") != std::string::npos && a.find("RESOURCE 8 \"metal\"") != std::string::npos && a.find("RESOURCE 5 \"metal\"") == std::string::npos, "first RESOURCE replaces inherited list, then appends");
    ck(t.find("TEXT \"T\" \"russian\"") != std::string::npos, "selected locale overrides same-mod English");
    ck(r.init.size() >= 4 && r.init[0] == 255 && r.init[1] == 254 && a.ends_with("\r\n\r\n") && t.starts_with("\r\n"), "emitted BOM and seam");
    std::string        high_en = "TEXT \"T\" \"later-english\"\n";
    std::vector<Input> fallback{input("lang\\en.cfg", en, 0), input("lang\\ru.cfg", ru, 0), input("lang\\en.cfg", high_en, 1)};
    r = prepare(base, lang, fallback, "ru");
    ck(r.ok && ascii(r.lang).find("TEXT \"T\" \"later-english\"") != std::string::npos, "later mod English fallback overrides earlier selected locale");
    std::string broken       = "@patch UNIT \"u\" INDEX 1 ENERGY 2147483648 END\n";
    auto        overflow_int = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", broken, 0)}, "en");
    ck(!overflow_int.ok && overflow_int.init.empty(), "int32 overflow is rejected");
    std::string nonascii_bare   = "@patch UNIT \"u\" INDEX 1 ENERGY 17\xD0\x96 END\n";
    auto        nonascii_result = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", nonascii_bare, 0)}, "en");
    ck(!nonascii_result.ok && nonascii_result.init.empty(), "non-ASCII bare numeric suffix is rejected");
    std::string unicode_comment = "@patch UNIT \"u\" INDEX 1 ENERGY 17 END # \xD0\x96\n";
    auto        comment_result  = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", unicode_comment, 0)}, "en");
    ck(comment_result.ok, "Unicode in a comment does not become a bare token");
    std::string ambiguous = "@patch UNIT \"u\" ENERGY 9 END\n";
    auto        bad       = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", ambiguous, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "duplicate selector rejected without partial output");
    std::string late = "@patch UNIT \"u\" INDEX 1 WEAPON 100 \"none\" END\n";
    in.push_back(input("cfg\\patch.cfg", late, 2));
    bad = prepare(base, lang, in, "ru");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "invalid later patch discards all output");
    std::string clear = "@patch UNIT \"u\" INDEX 1 @clear RESOURCE END\n";
    r                 = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", clear, 0)}, "en");
    ck(r.ok && ascii(r.init).find("RESOURCE 5 \"metal\"") == std::string::npos, "clear restores empty resource group");
    std::string replace = "@replace UNIT \"u\" INDEX 1 TYPE \"unitcode\" END\n";
    r                   = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", replace, 0)}, "en");
    ck(r.ok && ascii(r.init).find("ENERGY 11") == std::string::npos && ascii(r.init).find("RESOURCE 5 \"metal\"") == std::string::npos, "replace starts from ObjectInit defaults");
    std::string unknown = "@patch UNIT \"u\" INDEX 1 ENERGY 99 END\n", invalid = "TEXT \"missing\" \"bad\"\n";
    bad = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", unknown, 0), input("lang\\zz.cfg", invalid, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "unselected invalid translation rejects whole transaction");
    auto        labelbase = wide_base("DEFINE \"resource\" 73\nTEXT \"resource\" \"base\"\n");
    std::string format    = "TEXT \"resource\" \"%s\"\n";
    bad                   = prepare(labelbase, wide(""), std::vector<Input>{input("lang\\en.cfg", format, 0)}, "en");
    ck(!bad.ok && bad.lang.empty(), "resource label rejects unsafe printf conversions");
    std::string literal = "TEXT \"resource\" \"ok %%\"\n";
    r                   = prepare(labelbase, wide(""), std::vector<Input>{input("lang\\en.cfg", literal, 0)}, "en");
    ck(r.ok && ascii(r.lang).find("ok %%") != std::string::npos, "resource label permits escaped percent");
    std::string unicode = "TEXT \"T\" \"\xD0\x96\"\n";
    r                   = prepare(base, lang, std::vector<Input>{input("lang\\en.cfg", unicode, 0)}, "en");
    bool found_wide     = false;
    for (std::size_t i = 2; i + 1 < r.lang.size(); i += 2)
        if (r.lang[i] == 0x16 && r.lang[i + 1] == 0x04) found_wide = true;
    ck(r.ok && found_wide, "UTF-8 translation emits UTF-16LE characters");
    auto        wbase = wide_base("WEAPON \"w\" PROBABILYTY 0 33 END\n");
    std::string prob  = "@patch WEAPON \"w\" @clear PROBABILYTY END\n";
    r                 = prepare(wbase, wide(""), std::vector<Input>{input("cfg\\patch.cfg", prob, 0)}, "en");
    ck(r.ok && ascii(r.init).find("PROBABILYTY") == std::string::npos, "clear probability restores ObjectInit slot zero default");
    std::string tabbed = "@patch UNIT \"u\" INDEX 1 ENERGY 17\tgarbage END\n";
    r                  = prepare(base, lang, std::vector<Input>{input("cfg\\patch.cfg", tabbed, 0)}, "en");
    ck(r.ok && ascii(r.init).find("ENERGY 17\r\n") != std::string::npos, "numeric prefix accepts trailing tab junk");
    std::string large_info = "DEFINE \"unitcode\" 1\nDEFINE \"SPRITE_ZWYKLY\" 1\nDEFINE \"SOLDIERTYPE_UNKNOWN\" 0\nDEFINE \"ore\" 1\nDEFINE \"header\" 736\nTEXT \"header\" \"" + std::string(119, 'H') + "\"\n";
    for (int id = 73; id <= 76; ++id) large_info += "TEXT \"L" + std::to_string(id) + "\" \"" + std::string(19, 'x') + "\"\n";
    large_info += "UNIT \"v\" TYPE \"unitcode\" RESOURCE 2147483647 \"ore\" RESOURCE 2147483647 \"ore\" RESOURCE 2147483647 \"ore\" RESOURCE 2147483647 \"ore\" END\n";
    auto        info_base  = wide_base(large_info);
    std::string info_patch = "@patch UNIT \"v\" ENERGY 12 END\n";
    bad                    = prepare(info_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", info_patch, 0)}, "en");
    ck(!bad.ok && bad.init.empty(), "info-panel combination cannot overflow shared text scratch");
    auto        planet_base = wide_base("PLANET \"p\" MAP \"old.mp\" END\n");
    std::string name31 = std::string(28, 'a') + ".mp", name32 = std::string(29, 'a') + ".mp";
    std::string map_ok = "@patch PLANET \"p\" MAP \"" + name31 + "\" END\n";
    r                  = prepare(planet_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", map_ok, 0)}, "en");
    ck(r.ok && r.changed_planet_maps.size() == 1 && r.changed_planet_maps[0] == name31, "31-byte planet map name accepted and exposed to adapter");
    std::string map_bad = "@patch PLANET \"p\" MAP \"" + name32 + "\" END\n";
    bad                 = prepare(planet_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", map_bad, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "32-byte planet map name rejected before publication");
    auto        bbase = wide_base("DEFINE \"buildingtype\" 1\nBUILDING_BUILD_TIME_MUL 1\nANIM \"a\" LENGTH 2 TIME 1 END\nBUILDING \"b\" TYPE \"buildingtype\" SPRITE_QUANTITY 1 BUILD_TIME 5 ANIM 0 \"a\" AREA 0 1 2 AREA 1 3 4 END\n");
    std::string area  = "@patch BUILDING \"b\" AREA 1 9 END\n";
    r                 = prepare(bbase, wide(""), std::vector<Input>{input("cfg\\patch.cfg", area, 0)}, "en");
    a                 = ascii(r.init);
    ck(r.ok && a.find("AREA 0 1 2\r\n") != std::string::npos && a.find("AREA 1 9\r\n") != std::string::npos && a.find("AREA 1 9 4") == std::string::npos, "AREA replaces selected column and preserves other columns");
    std::string fractional = "@patch ANIM \"a\" TIME 0.2 END\n";
    r                      = prepare(bbase, wide(""), std::vector<Input>{input("cfg\\patch.cfg", fractional, 0)}, "en");
    ck(r.ok && ascii(r.init).find("TIME 0.2\r\n") != std::string::npos, "binary32 shortest float emission matches oracle choice");
    std::string cyclic = "@patch ANIM \"a\" CYCLIC END\n";
    bad                = prepare(bbase, wide(""), std::vector<Input>{input("cfg\\patch.cfg", cyclic, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "cyclic construction animation rejected before emission");
    std::string overflow = "@patch ANIM \"a\" LENGTH 2000 REPEAT 2 END\n";
    bad                  = prepare(bbase, wide(""), std::vector<Input>{input("cfg\\patch.cfg", overflow, 0)}, "en");
    ck(!bad.ok && bad.init.empty(), "animation LENGTH times REPEAT respects shared frame pool");
    auto        static_base  = wide_base("DEFINE \"static_type\" 34\nANIM \"static_anim\" LENGTH 1 TIME 1 END\nBUILDING \"static\" TYPE \"static_type\" SPRITE_QUANTITY 1 ANIM 0 \"static_anim\" ANIM 1 \"static_anim\" END\n");
    std::string static_patch = "@patch BUILDING \"static\" SIGHT 1 END\n";
    r                        = prepare(static_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", static_patch, 0)}, "en");
    ck(r.ok, "nonconstructible online-toggle building permits a zero-time static animation");
    std::string negative_static = "@patch BUILDING \"static\" BUILD_TIME -1 END\n";
    bad                         = prepare(static_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", negative_static, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "negative static construction duration is rejected");
    auto        mother_base  = wide_base("DEFINE \"mother_type\" 6\nANIM \"build\" TIME 1 END\nBUILDING \"mother\" TYPE \"mother_type\" SPRITE_QUANTITY 1 ANIM 0 \"build\" END\n");
    std::string mother_patch = "@patch BUILDING \"mother\" SIGHT 1 END\n";
    bad                      = prepare(mother_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", mother_patch, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "mother landing can reach construction progress and requires a positive total");
    auto        guarded_base = wide_base("DEFINE \"static_type\" 34\nANIM \"build\" TIME 1 END\nANIM \"guarded\" TIME 0 END\nBUILDING \"static\" TYPE \"static_type\" SPRITE_QUANTITY 1 ANIM 0 \"build\" ANIM 1 \"guarded\" END\n");
    std::string guarded_zero = "@patch BUILDING \"static\" SIGHT 1 END\n";
    r                        = prepare(guarded_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", guarded_zero, 0)}, "en");
    ck(r.ok, "zero-time online-toggle frame is skipped by the guarded handler");
    std::string guarded_negative = "@patch ANIM \"guarded\" TIME -1 END\n";
    bad                          = prepare(guarded_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", guarded_negative, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "negative guarded online frame is rejected");
    auto        sentinel_base  = wide_base("DEFINE \"airfield\" 29\nANIM \"build\" TIME 1 END\nANIM \"sentinel_anim\" TIME -1 END\nBUILDING \"air\" TYPE \"airfield\" SPRITE_QUANTITY 3 BUILD_TIME 1 ANIM 0 \"build\" ANIM 3 \"sentinel_anim\" END\n");
    std::string sentinel_patch = "@patch BUILDING \"air\" SIGHT 1 END\n";
    r                          = prepare(sentinel_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", sentinel_patch, 0)}, "en");
    ck(r.ok, "airfield sentinel slot does not enter the strict callback loop");
    auto        helipad_base = wide_base("DEFINE \"helipad\" 10\nANIM \"build\" TIME 1 END\nANIM \"guarded\" TIME -1 END\nBUILDING \"heli\" TYPE \"helipad\" SPRITE_QUANTITY 2 BUILD_TIME 1 ANIM 0 \"build\" ANIM 3 \"guarded\" END\n");
    std::string heli_two     = "@patch BUILDING \"heli\" SIGHT 1 END\n";
    r                        = prepare(helipad_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", heli_two, 0)}, "en");
    ck(r.ok, "helipad animation beyond sprite count is not ticked");
    std::string heli_three = "@patch BUILDING \"heli\" SPRITE_QUANTITY 3 END\n";
    bad                    = prepare(helipad_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", heli_three, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "reachable guarded helipad frame rejects negative time");
    auto        online_base = wide_base("DEFINE \"generic_type\" 1\nANIM \"build\" LENGTH 2 TIME 1 END\nANIM \"online\" LENGTH 2 TIME 1 END\nBUILDING \"factory\" TYPE \"generic_type\" SPRITE_QUANTITY 1 BUILD_TIME 5 ANIM 0 \"build\" ANIM 1 \"online\" END\n");
    std::string zero_online = "@patch ANIM \"online\" TIME 0 END\n";
    bad                     = prepare(online_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", zero_online, 0)}, "en");
    ck(!bad.ok && bad.init.empty() && bad.lang.empty(), "generic online animation with zero frame time is rejected");
    std::string cyclic_online = "@patch ANIM \"online\" CYCLIC END\n";
    r                         = prepare(online_base, wide(""), std::vector<Input>{input("cfg\\patch.cfg", cyclic_online, 0)}, "en");
    ck(r.ok, "positive-duration cyclic online animation is accepted");
    std::printf("cfgpatchtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
