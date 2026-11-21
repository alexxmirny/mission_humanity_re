#include "include/mh_cfg_patch.h"
#include "include/mh_cfg_schema.gen.h"
#include "include/mh_mod_catalog.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mh::mods::cfg {
namespace {
using schema::Kind;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
using Str = std::u16string;
struct Token {
    std::u16string_view s;
    bool                quoted;
    bool                closed = true;
};
using Line  = std::vector<Token>;
using Lines = std::vector<Line>;
struct Atom {
    Str          s;
    std::int32_t i                              = 0;
    float        f                              = 0;
    bool         operator==(const Atom &) const = default;
};
struct Field {
    Atom                                scalar;
    std::map<int, Atom>                 slots;
    std::vector<Atom>                   list;
    std::array<std::array<int, 10>, 10> area{};
    std::set<int>                       banks;
    std::array<int, 4>                  source{};
    bool                                operator==(const Field &) const = default;
};
struct Record {
    Str                          name;
    std::map<std::string, Field> fields;
};
struct Pair {
    Str          name;
    std::int32_t value;
};
struct Text {
    Str name, value;
};
struct Bank {
    Str               name;
    std::vector<Pair> sprites;
};
struct Model {
    std::vector<Pair>                          defines, def_banks, icons;
    std::vector<Text>                          texts;
    std::vector<Bank>                          banks;
    std::vector<std::pair<std::string, Atom>>  globals;
    std::map<std::string, std::vector<Record>> classes;
};
std::string narrow_ascii(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (auto c : s) {
        if (c > 0x7f) throw Error("non-ASCII narrow config token is unsupported when patches are active");
        out.push_back(static_cast<char>(c));
    }
    return out;
}
std::string number_ascii(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}
Str u16ascii(std::string_view s) {
    Str r;
    r.reserve(s.size());
    for (unsigned char c : s) r.push_back(c);
    return r;
}
Str decode_utf8(std::span<const std::uint8_t> b) {
    Str out;
    out.reserve(b.size());
    std::size_t i = 0;
    if (b.size() >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) i = 3;
    while (i < b.size()) {
        unsigned c = b[i++], cp;
        unsigned n;
        if (c < 0x80) {
            cp = c;
            n  = 0;
        } else if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 31;
            n  = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 15;
            n  = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 7;
            n  = 3;
        } else throw Error("invalid UTF-8");
        if (n > b.size() - i) throw Error("truncated UTF-8");
        for (unsigned k = 0; k < n; ++k) {
            unsigned d = b[i++];
            if ((d & 0xc0) != 0x80) throw Error("invalid UTF-8 continuation");
            cp = (cp << 6) | (d & 63);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || !cp) throw Error("invalid UTF-8 code point or NUL");
        if (cp < 0x10000) out.push_back(static_cast<char16_t>(cp));
        else {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (cp & 1023)));
        }
    }
    return out;
}
Str decode_utf16(std::span<const std::uint8_t> b) {
    if (b.size() % 2) throw Error("odd UTF-16LE byte count");
    Str r;
    r.reserve(b.size() / 2);
    for (std::size_t i = 0; i < b.size(); i += 2) {
        char16_t c = static_cast<char16_t>(b[i] | (unsigned(b[i + 1]) << 8));
        if (i == 0 && c == 0xfeff) continue;
        if (!c) throw Error("embedded NUL");
        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 3 >= b.size()) throw Error("unpaired UTF-16 surrogate");
            auto d = static_cast<char16_t>(b[i + 2] | (unsigned(b[i + 3]) << 8));
            if (d < 0xdc00 || d > 0xdfff) throw Error("unpaired UTF-16 surrogate");
            r.push_back(c);
            r.push_back(d);
            i += 2;
        } else if (c >= 0xdc00 && c <= 0xdfff) throw Error("unpaired UTF-16 surrogate");
        else r.push_back(c);
    }
    return r;
}
Lines lex(const Str &s) {
    Lines       out;
    Line        line;
    std::size_t i = 0, tokens = 0;
    while (i < s.size()) {
        char16_t c = s[i];
        if (c == '\r' || c == '\n') {
            out.push_back(std::move(line));
            line = {};
            ++i;
            if (c == '\r' && i < s.size() && s[i] == '\n') ++i;
            continue;
        }
        if (c == ' ') {
            ++i;
            continue;
        }
        if (c == '"') {
            ++i;
            const auto begin = i;
            while (i < s.size() && s[i] != '"' && s[i] != '\r' && s[i] != '\n') ++i;
            const auto finish = i;
            bool       closed = i < s.size() && s[i] == '"';
            if (closed) ++i;
            line.push_back({std::u16string_view(s).substr(begin, finish - begin), true, closed});
        } else {
            const auto begin = i;
            while (i < s.size() && s[i] != ' ' && s[i] != '\r' && s[i] != '\n') ++i;
            line.push_back({std::u16string_view(s).substr(begin, i - begin), false});
        }
        if (line.size() > 65536 || ++tokens > 2000000) throw Error("config token limit exceeded");
    }
    out.push_back(std::move(line));
    return out;
}
bool starts(std::u16string_view s, std::u16string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool line_comment(const Token &t) { return !t.quoted && (starts(t.s, u"//") || starts(t.s, u";") || starts(t.s, u"#") || t.s == u"rem" || t.s == u"REM"); }
int  integer(const Token &t, bool ascii_only = false) {
    if (t.quoted) throw Error("expected integer");
    auto        s = ascii_only ? narrow_ascii(t.s) : number_ascii(t.s);
    std::size_t p = 0;
    while (p < s.size() && (s[p] == '\t' || s[p] == '\v' || s[p] == '\f')) ++p;
    bool minus = false;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) minus = s[p++] == '-';
    auto          begin = p;
    std::uint64_t v     = 0;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        unsigned d = s[p++] - '0';
        if (v > (2147483648ull - d) / 10) throw Error("integer out of int32 range");
        v = v * 10 + d;
    }
    if (p == begin || (!minus && v > 2147483647ull)) throw Error("invalid int32");
    if (minus && v == 2147483648ull) return std::numeric_limits<std::int32_t>::min();
    return minus ? -static_cast<int>(v) : static_cast<int>(v);
}
float real(const Token &t, bool ascii_only = false) {
    if (t.quoted) throw Error("expected float");
    auto        s   = ascii_only ? narrow_ascii(t.s) : number_ascii(t.s);
    const char *p   = s.data();
    const char *end = p + s.size();
    while (p < end && (*p == '\t' || *p == '\v' || *p == '\f')) ++p;
    if (p < end && *p == '+') ++p;
    double d      = 0;
    auto   result = std::from_chars(p, end, d, std::chars_format::general);
    if (result.ptr == p) throw Error("invalid float");
    if (result.ec == std::errc::result_out_of_range) {
        // A decimal below binary64's minimum rounds to signed zero in Python's float().
        // Distinguish underflow from overflow by its decimal exponent.
        const char *q   = p;
        bool        neg = false;
        if (q < result.ptr && *q == '-') {
            neg = true;
            ++q;
        }
        int         before = 0, first = -1, digits = 0;
        bool        dot = false;
        const char *e   = q;
        for (; e < result.ptr && *e != 'e' && *e != 'E'; ++e) {
            if (*e == '.') {
                dot = true;
                continue;
            }
            if (*e >= '0' && *e <= '9') {
                if (!dot) ++before;
                if (first < 0 && *e != '0') first = digits;
                ++digits;
            }
        }
        int exp = 0;
        if (e < result.ptr) {
            ++e;
            bool eneg = false;
            if (e < result.ptr && (*e == '+' || *e == '-')) eneg = *e++ == '-';
            while (e < result.ptr && *e >= '0' && *e <= '9') {
                if (exp < 100000) exp = exp * 10 + (*e - '0');
                ++e;
            }
            if (eneg) exp = -exp;
        }
        if (first >= 0 && static_cast<long long>(before) - first - 1 + exp < 0) d = neg ? -0.0 : 0.0;
        else throw Error("float overflow");
    } else if (result.ec != std::errc{}) throw Error("invalid float");
    if (!std::isfinite(d) || std::abs(d) >= static_cast<double>(std::numeric_limits<float>::max()) + std::ldexp(1.0, 103)) throw Error("float32 overflow");
    float f = static_cast<float>(d);
    if (!std::isfinite(f)) throw Error("float32 overflow");
    return f;
}
const Token &arg(const Line &l, std::size_t i) {
    if (i >= l.size()) throw Error("missing argument");
    return l[i];
}
Str strarg(const Line &l, std::size_t i) {
    const auto &t = arg(l, i);
    if (!t.quoted) throw Error("expected quoted string");
    if (!t.closed) throw Error("unterminated quoted string");
    return Str(t.s);
}
const schema::Keyword *keyword(std::string_view cls, std::string_view kw) {
    for (const auto &k : schema::keywords)
        if (cls == k.cls && kw == k.keyword) return &k;
    return nullptr;
}
Record fresh(std::string_view cls, Str name) {
    Record r;
    r.name = std::move(name);
    for (const auto &k : schema::keywords)
        if (cls == k.cls && k.kind != Kind::BankSprite) r.fields.try_emplace(k.field);
    for (const auto &s : schema::seeds)
        if (cls == s.cls) {
            const schema::Keyword *k = nullptr;
            for (const auto &t : schema::keywords)
                if (cls == t.cls && std::string_view(t.field) == s.field) {
                    k = &t;
                    break;
                }
            if (!k) throw Error("invalid generated seed");
            auto &f         = r.fields[s.field];
            Str   seed_text = u16ascii(s.value);
            Token t{seed_text, false};
            Atom  a;
            if (k->kind == Kind::Str || k->kind == Kind::SlotStr) a.s = seed_text;
            else if (k->kind == Kind::F32 || k->kind == Kind::SlotF32) a.f = real(t);
            else a.i = integer(t);
            if (k->kind == Kind::Source4) {
                if (s.slot < 0 || s.slot >= 4) throw Error("invalid source default");
                f.source[s.slot] = a.i;
            } else if (s.slot >= 0) f.slots[s.slot] = a;
            else f.scalar = a;
        }
    return r;
}

std::size_t apply(const schema::Keyword &k, const Line &l, std::size_t i, Record &rec, Bank *bank, bool patch, std::set<std::string> &groups) {
    auto &f           = rec.fields[k.field];
    auto  check_slot  = [&](int slot) { for(const auto& c:schema::caps) if(std::string_view(k.cls)==c.cls&&std::string_view(k.field)==c.field&&(std::string_view(c.mode)=="slot"||std::string_view(c.mode)=="slot1") && (slot<(std::string_view(c.mode)=="slot1"?1:0)||slot>=c.limit)) throw Error(std::string(k.keyword)+": slot out of range"); };
    auto  check_count = [&]() { for(const auto& c:schema::caps) if(std::string_view(k.cls)==c.cls&&std::string_view(k.field)==c.field&&std::string_view(c.mode)=="count"&&f.list.size()>static_cast<std::size_t>(c.limit)) throw Error(std::string(k.keyword)+": too many entries"); };
    auto  as          = [&](std::size_t n) { auto s=strarg(l,i+n); if(patch) (void)narrow_ascii(s); return s; };
    auto  ai          = [&](std::size_t n) { return integer(arg(l, i + n), patch); };
    auto  af          = [&](std::size_t n) { return real(arg(l, i + n), patch); };
    Atom  a;
    int   slot;
    switch (k.kind) {
        case Kind::Int: f.scalar.i = ai(1); return i + 2;
        case Kind::F32: f.scalar.f = af(1); return i + 2;
        case Kind::Str: f.scalar.s = as(1); return i + 2;
        case Kind::Flag: f.scalar.i = 1; return i + 1;
        case Kind::SlotStr:
            slot = ai(1);
            check_slot(slot);
            a.s           = as(2);
            f.slots[slot] = a;
            return i + 3;
        case Kind::SlotInt:
            slot = ai(1);
            check_slot(slot);
            a.i           = ai(2);
            f.slots[slot] = a;
            return i + 3;
        case Kind::SlotF32:
            slot = ai(1);
            check_slot(slot);
            a.f           = af(2);
            f.slots[slot] = a;
            return i + 3;
        case Kind::Cap:
            slot = ai(1);
            check_slot(slot);
            a.s           = as(2);
            a.f           = af(3);
            f.slots[slot] = a;
            return i + 4;
        case Kind::Res:
            if (patch && groups.insert(k.field).second) f.list.clear();
            a.i = ai(1);
            a.s = as(2);
            f.list.push_back(a);
            check_count();
            return i + 3;
        case Kind::Depend:
            if (patch && groups.insert(k.field).second) f.list.clear();
            a.s = as(1);
            f.list.push_back(a);
            check_count();
            return i + 2;
        case Kind::Area: {
            int col = ai(1);
            if (patch && (col < 0 || col > 9)) throw Error("AREA column outside 0..9");
            col = std::clamp(col, 0, 9);
            if (patch)
                for (auto &row : f.area) row[col] = 0;
            std::size_t j   = i + 2;
            int         row = 0;
            while (j < l.size() && !l[j].quoted) {
                int v;
                try {
                    v = integer(l[j], patch);
                } catch (const Error &) { break; }
                if (row >= 10) throw Error("AREA has more than ten rows");
                f.area[row++][col] = v;
                ++j;
            }
            return j;
        }
        case Kind::BankRange: {
            int lo = ai(1), hi = ai(2);
            if (lo < 0 || hi > 99 || hi < lo) throw Error("BANK range outside 0..99 or reversed");
            if (patch && groups.insert(k.field).second) f.banks.clear();
            for (int n = lo; n <= hi; ++n) f.banks.insert(n);
            return i + 3;
        }
        case Kind::Source4:
            for (int n = 0; n < 4; ++n) f.source[n] = ai(n + 1);
            return i + 5;
        case Kind::BankSprite:
            if (!bank) throw Error("SPRITE outside BANK");
            if (patch && groups.insert(k.field).second) bank->sprites.clear();
            bank->sprites.push_back({as(1), ai(2)});
            return i + 3;
    }
    throw Error("unknown generated argument kind");
}
struct Reader {
    Model      &m;
    std::string source;
    bool        block = false;
    void        base(const Str &text) {
        std::string           section;
        Record                rec;
        Bank                  bank;
        std::set<std::string> groups;
        std::size_t           lineno = 0;
        for (const auto &line : lex(text)) {
            ++lineno;
            std::size_t i = 0;
            try {
                while (i < line.size()) {
                    const auto &t = line[i];
                    if (block) {
                        if (!t.quoted && starts(t.s, u"*/")) block = false;
                        ++i;
                        continue;
                    }
                    if (!t.quoted && starts(t.s, u"/*")) {
                        block = true;
                        ++i;
                        continue;
                    }
                    if (line_comment(t)) break;
                    if (t.quoted) throw Error("unexpected string at dispatch");
                    auto kw = narrow_ascii(t.s);
                    if (kw.empty() || kw.find_first_not_of("\t\v\f") == std::string::npos) {
                        ++i;
                        continue;
                    }
                    if (!section.empty()) {
                        if (kw == "END") {
                            if (section == "BANK") m.banks.push_back(std::move(bank));
                            else {
                                auto &dst = m.classes[section];
                                for (const auto &c : schema::caps)
                                    if (section == c.cls && std::string_view(c.field) == "__count__" && std::string_view(c.mode) == "count" && dst.size() >= static_cast<std::size_t>(c.limit)) throw Error(section + ": class count exceeds constructor limit");
                                dst.push_back(std::move(rec));
                            }
                            section.clear();
                            ++i;
                            continue;
                        }
                        auto k = keyword(section, kw);
                        if (!k) throw Error("unknown " + section + " keyword " + kw);
                        i = apply(*k, line, i, rec, &bank, false, groups);
                        continue;
                    }
                    if (kw == "END") {
                        ++i;
                        continue;
                    }
                    if (kw == "DEFINE" || kw == "DEF_BANK" || kw == "ICON") {
                        Pair p{strarg(line, i + 1), integer(arg(line, i + 2))};
                        (kw == "DEFINE" ? m.defines : kw == "DEF_BANK" ? m.def_banks
                                                                              : m.icons)
                            .push_back(std::move(p));
                        i += 3;
                        continue;
                    }
                    if (kw == "TEXT") {
                        m.texts.push_back({strarg(line, i + 1), strarg(line, i + 2)});
                        i += 3;
                        continue;
                    }
                    bool modifier = false;
                    for (const auto &x : schema::modifiers)
                        if (kw == x.keyword) {
                            Atom a;
                            if (x.kind == Kind::F32) a.f = real(arg(line, i + 1));
                            else a.i = integer(arg(line, i + 1));
                            auto found = std::find_if(m.globals.begin(), m.globals.end(), [&](const auto &p) { return p.first == kw; });
                            if (found == m.globals.end()) m.globals.emplace_back(kw, a);
                            else found->second = a;
                            modifier = true;
                            break;
                        }
                    if (modifier) {
                        i += 2;
                        continue;
                    }
                    bool cls = false;
                    for (const auto &k : schema::keywords)
                        if (kw == k.cls) {
                            cls = true;
                            break;
                        }
                    if (!cls) throw Error("unknown top-level keyword " + kw);
                    section  = kw;
                    Str name = strarg(line, i + 1);
                    if (kw == "BANK") {
                        bank      = {};
                        bank.name = std::move(name);
                        rec       = {};
                    } else rec = fresh(kw, std::move(name));
                    groups.clear();
                    i += 2;
                }
            } catch (const Error &e) { throw Error(source + ":" + std::to_string(lineno) + ": " + e.what()); }
        }
        if (!section.empty()) throw Error(source + ": unclosed " + section + " section");
        if (block) throw Error(source + ": unclosed block comment");
    }
};
Record &select(Model &m, const std::string &cls, const Str &name, int index) {
    auto it = m.classes.find(cls);
    if (it == m.classes.end()) throw Error("unknown class or target " + cls);
    auto &v = it->second;
    if (index) {
        if (index < 1 || static_cast<std::size_t>(index) > v.size() || v[index - 1].name != name) throw Error("INDEX does not match original " + cls + " target");
        return v[index - 1];
    }
    Record *found = nullptr;
    for (auto &r : v)
        if (r.name == name) {
            if (found) throw Error("ambiguous " + cls + " target; INDEX required");
            found = &r;
        }
    if (!found) throw Error("unknown " + cls + " target");
    return *found;
}
std::size_t select_text(const Model &m, const Str &name, int index) {
    if (index) {
        if (index < 1 || static_cast<std::size_t>(index) > m.texts.size() || m.texts[index - 1].name != name) throw Error("TEXT INDEX does not match original target");
        return index - 1;
    }
    std::size_t found = m.texts.size();
    for (std::size_t i = 0; i < m.texts.size(); ++i)
        if (m.texts[i].name == name) {
            if (found != m.texts.size()) throw Error("ambiguous TEXT target; INDEX required");
            found = i;
        }
    if (found == m.texts.size()) throw Error("unknown TEXT target");
    return found;
}
int optional_index(const Line &l, std::size_t &i) {
    if (i < l.size() && !l[i].quoted && l[i].s == u"INDEX") {
        int n = integer(arg(l, i + 1), true);
        if (n <= 0) throw Error("INDEX must be positive");
        i += 2;
        return n;
    }
    return 0;
}
void patch(Model &m, const Str &text, const std::string &source) {
    std::string           cls;
    Record               *rec = nullptr;
    Bank                  tempbank;
    Bank                 *targetbank = nullptr;
    bool                  block      = false;
    std::set<std::string> groups;
    std::size_t           ln = 0;
    for (const auto &line : lex(text)) {
        ++ln;
        std::size_t i = 0;
        try {
            while (i < line.size()) {
                const auto &t = line[i];
                if (block) {
                    if (!t.quoted && starts(t.s, u"*/")) block = false;
                    ++i;
                    continue;
                }
                if (!t.quoted && starts(t.s, u"/*")) {
                    block = true;
                    ++i;
                    continue;
                }
                if (line_comment(t)) break;
                if (t.quoted) throw Error("unexpected quoted token");
                auto kw = narrow_ascii(t.s);
                if (kw.empty() || kw.find_first_not_of("\t\v\f") == std::string::npos) {
                    ++i;
                    continue;
                }
                if (cls.empty()) {
                    if (kw != "@patch" && kw != "@replace") throw Error("expected @patch or @replace");
                    auto c = narrow_ascii(arg(line, i + 1).s);
                    if (arg(line, i + 1).quoted) throw Error("expected class");
                    auto name = strarg(line, i + 2);
                    (void)narrow_ascii(name);
                    i += 3;
                    int idx = optional_index(line, i);
                    cls     = c;
                    groups.clear();
                    if (cls == "BANK") {
                        std::vector<Bank *> matches;
                        for (auto &b : m.banks)
                            if (b.name == name) matches.push_back(&b);
                        if (idx) {
                            if (idx < 1 || static_cast<std::size_t>(idx) > m.banks.size() || m.banks[idx - 1].name != name) throw Error("BANK INDEX mismatch");
                            targetbank = &m.banks[idx - 1];
                        } else {
                            if (matches.size() != 1) throw Error("BANK target missing or ambiguous");
                            targetbank = matches.front();
                        }
                        tempbank = kw == "@replace" ? Bank{name, {}} : *targetbank;
                        rec      = nullptr;
                    } else {
                        rec = &select(m, cls, name, idx);
                        if (kw == "@replace") *rec = fresh(cls, name);
                    }
                    continue;
                }
                if (kw == "END") {
                    if (targetbank) {
                        if (tempbank.name != targetbank->name || tempbank.sprites.size() != targetbank->sprites.size()) throw Error("BANK sprite identity changed");
                        for (std::size_t n = 0; n < tempbank.sprites.size(); ++n)
                            if (tempbank.sprites[n].name != targetbank->sprites[n].name || tempbank.sprites[n].value != targetbank->sprites[n].value) throw Error("BANK sprite identity changed");
                        targetbank = nullptr;
                    }
                    cls.clear();
                    rec = nullptr;
                    ++i;
                    continue;
                }
                if (kw == "@clear") {
                    if (!rec) throw Error("BANK cannot be cleared");
                    auto key = narrow_ascii(arg(line, i + 1).s);
                    auto k   = keyword(cls, key);
                    if (!k || arg(line, i + 1).quoted) throw Error("unknown @clear field " + key);
                    auto d                = fresh(cls, rec->name);
                    rec->fields[k->field] = d.fields[k->field];
                    groups.insert(k->field);
                    i += 2;
                    continue;
                }
                auto k = keyword(cls, kw);
                if (!k) throw Error("unknown " + cls + " keyword " + kw);
                Record dummy;
                i = apply(*k, line, i, rec ? *rec : dummy, targetbank ? &tempbank : nullptr, true, groups);
            }
        } catch (const Error &e) { throw Error(source + ":" + std::to_string(ln) + ": " + e.what()); }
    }
    if (!cls.empty() || block) throw Error(source + ": unterminated patch or comment");
}
using TextChanges = std::map<std::size_t, Str>;
void        validate_text_entry(const Model &, std::size_t, const Str &);
TextChanges text_patch(const Model &m, const Str &text, const std::string &source) {
    TextChanges out;
    bool        block = false;
    std::size_t ln    = 0;
    for (const auto &line : lex(text)) {
        ++ln;
        std::size_t i = 0;
        try {
            while (i < line.size()) {
                const auto &t = line[i];
                if (block) {
                    if (!t.quoted && starts(t.s, u"*/")) block = false;
                    ++i;
                    continue;
                }
                if (!t.quoted && starts(t.s, u"/*")) {
                    block = true;
                    ++i;
                    continue;
                }
                if (line_comment(t)) break;
                if (t.quoted || t.s != u"TEXT") throw Error("only TEXT is allowed in language patch");
                Str name = strarg(line, i + 1), value = strarg(line, i + 2);
                (void)narrow_ascii(name);
                i += 3;
                int  idx    = optional_index(line, i);
                auto target = select_text(m, name, idx);
                validate_text_entry(m, target, value);
                out[target] = std::move(value);
                if (i < line.size() && !line_comment(line[i]) && !(!line[i].quoted && starts(line[i].s, u"/*"))) throw Error("one TEXT statement per line");
            }
        } catch (const Error &e) { throw Error(source + ":" + std::to_string(ln) + ": " + e.what()); }
    }
    if (block) throw Error(source + ": unterminated comment");
    return out;
}

std::string fmt_float(float f) {
    double d = f;
    if (std::trunc(d) == d && std::abs(d) < 1e15) return std::to_string(static_cast<std::int64_t>(d));
    for (int prec : {6, 7, 8, 9, 17}) {
        char buf[128];
        auto r = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general, prec);
        if (r.ec != std::errc{}) throw Error("float formatting failed");
        double v = 0;
        auto   z = std::from_chars(buf, r.ptr, v, std::chars_format::general);
        if (z.ec == std::errc{} && z.ptr == r.ptr && static_cast<float>(v) == f) return {buf, r.ptr};
    }
    throw Error("float formatting failed");
}
void append(Str &s, std::string_view a) {
    for (unsigned char c : a) s.push_back(c);
}
void quoted(Str &s, const Str &v) {
    if (v.find(u'"') != Str::npos || v.find(u'\r') != Str::npos || v.find(u'\n') != Str::npos || v.find(u'\0') != Str::npos) throw Error("unemittable string");
    s.push_back(u'"');
    s += v;
    s.push_back(u'"');
}
void line_start(Str &out, std::string_view kw) {
    out += u"   ";
    append(out, kw);
}
void endline(Str &out) { out += u"\r\n"; }
void value_int(Str &out, int i) {
    out.push_back(u' ');
    append(out, std::to_string(i));
}
void value_float(Str &out, float f) {
    out.push_back(u' ');
    append(out, fmt_float(f));
}
void value_str(Str &out, const Str &s) {
    out.push_back(u' ');
    quoted(out, s);
}
void emit_field(Str &out, const schema::Keyword &k, const Field &f) {
    auto start = [&]() { line_start(out, k.keyword); };
    switch (k.kind) {
        case Kind::Int:
            start();
            value_int(out, f.scalar.i);
            endline(out);
            break;
        case Kind::F32:
            start();
            value_float(out, f.scalar.f);
            endline(out);
            break;
        case Kind::Str:
            start();
            value_str(out, f.scalar.s);
            endline(out);
            break;
        case Kind::Flag:
            if (f.scalar.i) {
                start();
                endline(out);
            }
            break;
        case Kind::SlotStr:
        case Kind::SlotInt:
        case Kind::SlotF32:
        case Kind::Cap:
            for (const auto &[slot, a] : f.slots) {
                start();
                value_int(out, slot);
                if (k.kind == Kind::SlotStr || k.kind == Kind::Cap) value_str(out, a.s);
                if (k.kind == Kind::SlotInt) value_int(out, a.i);
                if (k.kind == Kind::SlotF32 || k.kind == Kind::Cap) value_float(out, a.f);
                endline(out);
            }
            break;
        case Kind::Res:
        case Kind::Depend:
            for (const auto &a : f.list) {
                start();
                if (k.kind == Kind::Res) value_int(out, a.i);
                value_str(out, a.s);
                endline(out);
            }
            break;
        case Kind::Area:
            for (int c = 0; c < 10; ++c) {
                int last = 9;
                while (last >= 0 && !f.area[last][c]) --last;
                if (last < 0) continue;
                start();
                value_int(out, c);
                for (int row = 0; row <= last; ++row) value_int(out, f.area[row][c]);
                endline(out);
            }
            break;
        case Kind::BankRange:
            for (auto it = f.banks.begin(); it != f.banks.end();) {
                int lo = *it, hi = lo;
                ++it;
                while (it != f.banks.end() && *it == hi + 1) hi = *it++;
                start();
                value_int(out, lo);
                value_int(out, hi);
                endline(out);
            }
            break;
        case Kind::Source4:
            start();
            for (int v : f.source) value_int(out, v);
            endline(out);
            break;
        case Kind::BankSprite: throw Error("invalid BANK field");
    }
}
std::vector<std::uint8_t> utf16_bytes(const Str &s) {
    if (s.size() > (mh::mods::SET_BYTES_MAX - 2) / 2) throw Error("emitted config exceeds limit");
    std::vector<std::uint8_t> out;
    out.reserve(2 + s.size() * 2);
    out.push_back(0xff);
    out.push_back(0xfe);
    for (char16_t c : s) {
        out.push_back(static_cast<std::uint8_t>(c));
        out.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    return out;
}
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> emit(const Model &m) {
    Str out, lang;
    for (const auto &p : m.defines) {
        out += u"DEFINE ";
        quoted(out, p.name);
        value_int(out, p.value);
        endline(out);
    }
    for (const auto &[name, v] : m.globals) {
        append(out, name);
        bool isfloat = false;
        for (const auto &x : schema::modifiers)
            if (name == x.keyword) {
                isfloat = x.kind == Kind::F32;
                break;
            }
        if (isfloat) value_float(out, v.f);
        else value_int(out, v.i);
        endline(out);
    }
    for (const auto &p : m.def_banks) {
        out += u"DEF_BANK ";
        quoted(out, p.name);
        value_int(out, p.value);
        endline(out);
    }
    for (const auto &p : m.icons) {
        out += u"ICON ";
        quoted(out, p.name);
        value_int(out, p.value);
        endline(out);
    }
    for (const auto &b : m.banks) {
        out += u"BANK ";
        quoted(out, b.name);
        endline(out);
        for (const auto &p : b.sprites) {
            out += u"   SPRITE ";
            quoted(out, p.name);
            value_int(out, p.value);
            endline(out);
        }
        out += u"END\r\n";
    }
    for (const auto *cls : schema::class_order) {
        auto it = m.classes.find(cls);
        if (it == m.classes.end()) continue;
        for (const auto &r : it->second) {
            append(out, cls);
            out.push_back(u' ');
            quoted(out, r.name);
            endline(out);
            auto                       d = fresh(cls, r.name);
            std::set<std::string_view> emitted;
            for (const auto &k : schema::keywords)
                if (std::string_view(k.cls) == cls && emitted.insert(k.field).second) {
                    auto v   = r.fields.find(k.field);
                    auto def = d.fields.find(k.field);
                    if (v == r.fields.end() || def == d.fields.end()) throw Error("inconsistent schema field");
                    if (!(v->second == def->second)) emit_field(out, k, v->second);
                }
            out += u"END\r\n";
        }
    }
    endline(out);
    endline(lang);
    for (const auto &t : m.texts) {
        lang += u"TEXT ";
        quoted(lang, t.name);
        value_str(lang, t.value);
        endline(lang);
    }
    return {utf16_bytes(out), utf16_bytes(lang)};
}

std::string label(const Str &s) {
    std::string out;
    for (char16_t c : s) {
        if (c >= 0x20 && c <= 0x7e) out.push_back(static_cast<char>(c));
        else out.push_back('?');
    }
    return out;
}

std::string narrow_engine(const Str &text) { return narrow_ascii(text); }

std::int32_t lookup_define(const Model &m, const Str &s, bool &found) {
    for (const auto &x : m.defines)
        if (narrow_engine(x.name) == narrow_engine(s)) {
            found = true;
            return x.value;
        }
    found = false;
    return 0;
}
bool exists(const Model &m, std::string_view uni, const Str &name, const Record &r) {
    if (name.empty()) return true;
    auto in_pairs = [&](const auto &pairs) { return std::any_of(pairs.begin(), pairs.end(), [&](const auto &p) { return narrow_engine(p.name) == narrow_engine(name); }); };
    if (uni == "define" || uni == "resource_define") return in_pairs(m.defines);
    if (uni == "icon") return in_pairs(m.icons);
    if (uni == "sprite") {
        for (const auto &b : m.banks)
            if (in_pairs(b.sprites)) return true;
        return false;
    }
    std::string cls;
    if (uni == "anim") cls = "ANIM";
    else if (uni == "progress") cls = "PROGRESS";
    else if (uni == "weapon") cls = "WEAPON";
    else if (uni == "building") cls = "BUILDING";
    else if (uni == "unit") cls = "UNIT";
    else if (uni == "planet") cls = "PLANET";
    else if (uni == "upgrade_object") {
        bool found = false;
        auto type  = r.fields.find("type");
        auto id    = type == r.fields.end() ? 0 : lookup_define(m, type->second.scalar.s, found);
        cls        = id == 1 ? "UNIT" : "WEAPON";
    } else throw Error("unknown generated reference universe");
    auto it = m.classes.find(cls);
    return it != m.classes.end() && std::any_of(it->second.begin(), it->second.end(), [&](const Record &x) { return narrow_engine(x.name) == narrow_engine(name); });
}
void valid_string(const Str &s, std::size_t limit, std::string_view context, bool text = false) {
    if ((text ? s.size() : narrow_engine(s).size()) > limit) throw Error(std::string(context) + ": string exceeds safe destination");
    if (s.find(u'"') != Str::npos || s.find(u'\r') != Str::npos || s.find(u'\n') != Str::npos || s.find(u'\0') != Str::npos) throw Error(std::string(context) + ": unemittable string");
}
int cap_limit(std::string_view cls, std::string_view field, std::string_view mode) {
    for (const auto &c : schema::caps)
        if (cls == c.cls && field == c.field && mode == c.mode) return c.limit;
    throw Error("missing generated constructor limit");
}
double global_value(const Model &m, std::string_view name) {
    auto i = std::find_if(m.globals.begin(), m.globals.end(), [&](const auto &p) { return p.first == name; });
    if (i == m.globals.end()) {
        for (const auto &mod : schema::modifiers)
            if (name == mod.keyword) return mod.initial;
        throw Error("unknown modifier default");
    }
    for (const auto &mod : schema::modifiers)
        if (name == mod.keyword) return mod.kind == Kind::F32 ? i->second.f : i->second.i;
    throw Error("unknown modifier in generated transform");
}
double transformed(const Model &m, const schema::Transform &rule, double x, const std::string &where) {
    double result = x * global_value(m, rule.mul) + global_value(m, rule.add);
    if (!std::isfinite(result)) throw Error(where + ": non-finite modified value");
    if (rule.integer) {
        double t = std::trunc(result);
        if (t < std::numeric_limits<std::int32_t>::min() || t > std::numeric_limits<std::int32_t>::max()) throw Error(where + ": modified int32 overflow");
    }
    return result;
}
void validate_text_entry(const Model &m, std::size_t index, const Str &value) {
    valid_string(value, static_cast<std::size_t>(cap_limit("TEXT", "value", "utf16")), "TEXT value", true);
    bool found = false;
    int  id    = lookup_define(m, m.texts[index].name, found);
    if (!found) return;
    for (const auto &d : schema::domains)
        if (std::string_view(d.cls) == "TEXT" && std::string_view(d.field) == "__resource_label_id__" && id >= d.min && id <= d.max) {
            std::size_t n = 0;
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == u'%') {
                    if (i + 1 == value.size() || value[i + 1] != u'%') throw Error("TEXT resource label contains unsafe format conversion");
                    ++i;
                }
                ++n;
            }
            if (n > static_cast<std::size_t>(cap_limit("TEXT", "__resource_label__", "utf16"))) throw Error("TEXT resource label exceeds menu buffer");
        }
}
void validate(const Model &m) {
    const auto common    = static_cast<std::size_t>(cap_limit("*", "__string__", "bytes"));
    const auto text_max  = static_cast<std::size_t>(cap_limit("TEXT", "value", "utf16"));
    const auto text_pool = static_cast<std::size_t>(cap_limit("TEXT", "__pool__", "utf16"));
    for (const auto &p : m.defines) valid_string(p.name, common, "DEFINE");
    for (const auto &p : m.def_banks) valid_string(p.name, common, "DEF_BANK");
    for (const auto &p : m.icons) valid_string(p.name, common, "ICON");
    std::size_t pool = 0;
    for (const auto &t : m.texts) {
        valid_string(t.name, common, "TEXT key");
        valid_string(t.value, text_max, "TEXT value", true);
        pool += t.value.size() + 1;
        if (pool > text_pool) throw Error("TEXT pool overflow");
        bool found     = false;
        int  id        = lookup_define(m, t.name, found);
        bool in_domain = false;
        for (const auto &d : schema::domains)
            if (std::string_view(d.cls) == "TEXT" && std::string_view(d.field) == "__id__") in_domain = id >= d.min && id <= d.max;
        if (!found || !in_domain) throw Error("TEXT key has invalid resolved id: " + label(t.name));
    }
    for (const auto &b : m.banks) {
        valid_string(b.name, common, "BANK");
        auto base = std::find_if(m.def_banks.begin(), m.def_banks.end(), [&](const Pair &p) { return narrow_engine(p.name) == narrow_engine(b.name); });
        if (base == m.def_banks.end()) throw Error("BANK has no DEF_BANK: " + label(b.name));
        std::int64_t cursor = base->value, cap = cap_limit("SPRITE", "__id__", "count");
        if (cursor < 0 || cursor > cap) throw Error("BANK base outside sprite ID space");
        for (const auto &p : b.sprites) {
            valid_string(p.name, common, "SPRITE");
            if (p.value < 0 || p.value > cap - cursor) throw Error("SPRITE frame range outside ID space: " + label(p.name));
            cursor += p.value;
        }
    }
    for (const auto &c : schema::caps)
        if (std::string_view(c.field) == "__count__" && std::string_view(c.mode) == "count") {
            auto it = m.classes.find(c.cls);
            if (it != m.classes.end() && it->second.size() > static_cast<std::size_t>(c.limit)) throw Error(std::string(c.cls) + ": class count exceeds constructor limit");
        }
    for (const auto &[cls, recs] : m.classes)
        for (const auto &r : recs) {
            std::string where = cls + " \"" + label(r.name) + "\"";
            valid_string(r.name, common, where);
            std::set<std::string_view> visited;
            for (const auto &k : schema::keywords)
                if (cls == k.cls && visited.insert(k.field).second) {
                    const auto &f     = r.fields.at(k.field);
                    std::string w     = where + "." + k.keyword;
                    std::size_t limit = common;
                    for (const auto &c : schema::caps)
                        if (cls == c.cls && std::string_view(k.field) == c.field && std::string_view(c.mode) == "bytes") limit = static_cast<std::size_t>(c.limit);
                    if (k.kind == Kind::Str) valid_string(f.scalar.s, limit, w);
                    for (const auto &[slot, a] : f.slots)
                        if (k.kind == Kind::SlotStr || k.kind == Kind::Cap) valid_string(a.s, limit, w);
                    for (const auto &a : f.list) valid_string(a.s, limit, w);
                    for (const auto &cap : schema::caps)
                        if (cls == cap.cls && std::string_view(k.field) == cap.field) {
                            std::string_view mode = cap.mode;
                            if (mode == "count") {
                                if (f.list.size() > static_cast<std::size_t>(cap.limit)) throw Error(w + ": too many entries");
                            } else if (mode == "slot" || mode == "slot1")
                                for (const auto &[slot, a] : f.slots)
                                    if (slot < (mode == "slot1" ? 1 : 0) || slot >= cap.limit) throw Error(w + ": slot out of range");
                        }
                    for (const auto &rule : schema::refs)
                        if (cls == rule.cls && std::string_view(k.field) == rule.field) {
                            auto check = [&](const Str &name) {
                                if (!exists(m, rule.universe, name, r)) throw Error(w + ": unresolved reference " + label(name));
                                if (std::string_view(rule.universe) == "resource_define" && !name.empty()) {
                                    bool found = false;
                                    int  id    = lookup_define(m, name, found);
                                    for (const auto &d : schema::domains)
                                        if (cls == d.cls && std::string_view(k.field) == d.field && (id < d.min || id > d.max)) throw Error(w + ": resolved resource ID outside constructor domain");
                                }
                            };
                            if (k.kind == Kind::Str) check(f.scalar.s);
                            if (k.kind == Kind::SlotStr || k.kind == Kind::Cap)
                                for (const auto &[slot, a] : f.slots) check(a.s);
                            if (k.kind == Kind::Depend || k.kind == Kind::Res)
                                for (const auto &a : f.list) check(a.s);
                        }
                    auto check_trunc = [&](double v) { double t=std::trunc(v); if(!std::isfinite(v)||t<std::numeric_limits<std::int32_t>::min()||t>std::numeric_limits<std::int32_t>::max()) throw Error(w+": integer conversion overflow"); };
                    if (cls == "UPGRADE" && k.kind == Kind::F32 && (std::string_view(k.field) == "step_speed" || std::string_view(k.field) == "turn_speed")) check_trunc(f.scalar.f);
                    for (const auto &rule : schema::transforms)
                        if (cls == rule.cls && std::string_view(k.field) == rule.field) {
                            if (k.kind == Kind::Int) (void)transformed(m, rule, f.scalar.i, w);
                            else if (k.kind == Kind::F32) (void)transformed(m, rule, f.scalar.f, w);
                            else if (k.kind == Kind::Res)
                                for (const auto &a : f.list) (void)transformed(m, rule, a.i, w);
                            else throw Error("invalid generated transform kind");
                        }
                    for (const auto &rule : schema::domains)
                        if (cls == rule.cls && std::string_view(k.field) == rule.field) {
                            auto check = [&](int v) {if(v<rule.min||v>rule.max) throw Error(w+": value outside constructor domain"); };
                            if (k.kind == Kind::Int) check(f.scalar.i);
                            else if (k.kind == Kind::Str) {
                                bool found = f.scalar.s.empty();
                                int  v     = f.scalar.s.empty() ? 0 : lookup_define(m, f.scalar.s, found);
                                if (!found) throw Error(w + ": unknown DEFINE");
                                check(v);
                            } else if (k.kind == Kind::SlotInt)
                                for (const auto &[slot, a] : f.slots) check(a.i);
                        }
                }
        }
}


bool is_building_kind(std::string_view kind, int type) {
    for (const auto &k : schema::building_kinds)
        if (kind == k.name && type == k.value) return true;
    return false;
}
void validate_building_values(const Model &m) {
    auto buildings = m.classes.find("BUILDING");
    if (buildings == m.classes.end()) return;
    for (const auto &b : buildings->second) {
        bool        found   = false;
        const auto &rawtype = b.fields.at("type").scalar.s;
        int         type    = (rawtype.empty() ? 0 : lookup_define(m, rawtype, found)) & 255;
        auto        check   = [&](double v) -> std::int32_t { double t=std::trunc(v); if(!std::isfinite(v)||t<std::numeric_limits<std::int32_t>::min()||t>std::numeric_limits<std::int32_t>::max()) throw Error("BUILDING "+label(b.name)+": resource conversion overflow"); return static_cast<std::int32_t>(t); };
        const auto &slots   = b.fields.at("extract_fuel").slots;
        if (is_building_kind("mine", type)) {
            std::array<std::int32_t, 10> sums{};
            int                          used = 0;
            for (const auto &[slot, a] : slots)
                if (a.f != 0 && used < 4) {
                    bool present = false;
                    int  id      = a.s.empty() ? 0 : lookup_define(m, a.s, present);
                    if (id < 0 || id >= static_cast<int>(sums.size())) throw Error("BUILDING " + label(b.name) + ": extraction resource ID outside array");
                    (void)check(a.f);
                    sums[id] = check(static_cast<double>(sums[id]) + a.f);
                    ++used;
                }
        } else if (is_building_kind("mother", type) || is_building_kind("port", type) || is_building_kind("shuttle", type)) {
            for (int slot : {0, 1, 2, 4}) {
                auto it = slots.find(slot);
                if (it != slots.end()) (void)check(it->second.f);
            }
        }
        const auto &capacity = b.fields.at("capacity").slots;
        bool        all      = is_building_kind("mine", type) || is_building_kind("silo", type);
        bool        prefix   = is_building_kind("mother", type) || is_building_kind("port", type);
        if (all || prefix)
            for (const auto &[slot, a] : capacity)
                if ((all || slot < 8) && a.f != 0) (void)check(a.f);
    }
}

void validate_localized(const Model &m, const TextChanges &overrides) {
    std::map<int, const Str *> last;
    std::size_t                pool = 0;
    for (std::size_t i = 0; i < m.texts.size(); ++i) {
        auto       it    = overrides.find(i);
        const Str &value = it == overrides.end() ? m.texts[i].value : it->second;
        valid_string(value, static_cast<std::size_t>(cap_limit("TEXT", "value", "utf16")), "TEXT value", true);
        pool += value.size() + 1;
        bool found = false;
        int  id    = lookup_define(m, m.texts[i].name, found);
        if (found) last[id] = &value;
    }
    if (pool > static_cast<std::size_t>(cap_limit("TEXT", "__pool__", "utf16"))) throw Error("TEXT pool overflow");
    int label_lo = 0, label_hi = -1, header_id = 0;
    for (const auto &d : schema::domains)
        if (std::string_view(d.cls) == "TEXT") {
            if (std::string_view(d.field) == "__resource_label_id__") {
                label_lo = d.min;
                label_hi = d.max;
            }
            if (std::string_view(d.field) == "__info_header_id__") header_id = d.min;
        }
    auto        size_at     = [&](int id) -> std::size_t { auto it=last.find(id); return it==last.end()?0:it->second->size(); };
    std::size_t labels_size = 0;
    for (int id = label_lo; id <= label_hi; ++id) {
        auto it = last.find(id);
        if (it == last.end()) throw Error("TEXT resource label missing: " + std::to_string(id));
        const auto &text     = *it->second;
        std::size_t rendered = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == u'%') {
                if (i + 1 == text.size() || text[i + 1] != u'%') throw Error("TEXT resource label contains unsafe format conversion");
                ++i;
            }
            ++rendered;
        }
        if (rendered > static_cast<std::size_t>(cap_limit("TEXT", "__resource_label__", "utf16"))) throw Error("TEXT resource label exceeds menu buffer");
        labels_size += rendered;
    }
    const std::size_t info_fixed = static_cast<std::size_t>(cap_limit("TEXT", "__info_fixed__", "utf16"));
    const std::size_t info_cap   = static_cast<std::size_t>(cap_limit("TEXT", "__info_scratch__", "utf16"));
    for (const auto &[cls, records] : m.classes)
        if (cls == "UNIT" || cls == "BUILDING" || cls == "PROJECT")
            for (const auto &r : records) {
                std::array<std::int32_t, 4> costs{};
                const auto                 &list = r.fields.at("resources").list;
                for (std::size_t i = 0; i < costs.size() && i < list.size(); ++i) {
                    double v = list[i].i;
                    if (cls == "UNIT") {
                        auto rule = std::find_if(std::begin(schema::transforms), std::end(schema::transforms), [](const schema::Transform &t) { return std::string_view(t.cls) == "UNIT" && std::string_view(t.field) == "resources"; });
                        if (rule == std::end(schema::transforms)) throw Error("missing UNIT resource transform");
                        v = transformed(m, *rule, v, "UNIT RESOURCE");
                    }
                    costs[i] = static_cast<std::int32_t>(std::trunc(v));
                }
                std::size_t sum = size_at(header_id) + labels_size + info_fixed;
                for (auto v : costs) sum += std::to_string(v).size();
                if (sum > info_cap) throw Error(cls + " " + label(r.name) + ": info cost text exceeds scratch buffer");
            }
    auto buildings = m.classes.find("BUILDING");
    if (buildings != m.classes.end()) {
        auto              building_text = [&](const Record &r) -> std::size_t { bool found=false; int id=lookup_define(m,r.name,found); return size_at(found?id:0); };
        auto              housing       = [&](int wanted) -> const Record                    *{ if(wanted<0) return nullptr; for(const auto& r:buildings->second) { const Str& s=r.fields.at("type").scalar.s; bool found=false; int type=s.empty()?0:lookup_define(m,s,found); if((type&255)==wanted) return &r; } return nullptr; };
        const std::size_t cap           = static_cast<std::size_t>(cap_limit("TEXT", "__scratch__", "utf16"));
        for (const auto &producer : buildings->second)
            for (const auto &reason : schema::housing_reasons)
                for (int target : {reason.alien, reason.human}) {
                    const Record *home  = housing(target);
                    std::size_t   total = building_text(producer) + size_at(reason.reason) + (home ? building_text(*home) + 6 : 3);
                    if (total > cap) throw Error("BUILDING " + label(producer.name) + ": production message exceeds text scratch buffer");
                }
    }
}

void validate_animations(const Model &m) {
    struct Anim {
        std::string name;
        std::size_t start;
        int         length;
        bool        cyclic;
    };
    std::vector<Anim>                   anims;
    std::vector<double>                 times;
    std::map<std::string, std::int64_t> sprites;
    for (const auto &bank : m.banks) {
        auto b = std::find_if(m.def_banks.begin(), m.def_banks.end(), [&](const Pair &p) { return narrow_engine(p.name) == narrow_engine(bank.name); });
        if (b == m.def_banks.end()) throw Error("BANK missing DEF_BANK");
        std::int64_t id = b->value;
        for (const auto &sprite : bank.sprites) {
            sprites.try_emplace(narrow_engine(sprite.name), id);
            id += sprite.value;
        }
    }
    auto a = m.classes.find("ANIM");
    if (a != m.classes.end())
        for (const auto &r : a->second) {
            const auto &f      = r.fields;
            int         length = f.at("length").scalar.i, repeat = f.at("repeat").scalar.i, step = f.at("step").scalar.i;
            if (length <= 0 || repeat <= 0) throw Error("ANIM " + label(r.name) + ": nonpositive LENGTH or REPEAT");
            std::int64_t count = std::int64_t(length) * repeat, budget = cap_limit("ANIM", "__pool__", "frames");
            if (count > budget || static_cast<std::int64_t>(times.size()) > budget - count) throw Error("ANIM " + label(r.name) + ": frame pool overflow");
            auto         sprite = narrow_engine(f.at("sprite").scalar.s);
            std::int64_t id     = 0;
            if (!sprite.empty()) {
                auto it = sprites.find(sprite);
                if (it == sprites.end()) throw Error("ANIM " + label(r.name) + ": unknown sprite");
                id = it->second;
            }
            std::int64_t end = id + std::int64_t(step) * (count - 1), cap = cap_limit("SPRITE", "__id__", "count");
            if (id < 0 || id >= cap || end < 0 || end >= cap) throw Error("ANIM " + label(r.name) + ": generated sprite ID outside pool");
            anims.push_back({narrow_engine(r.name), times.size(), length, f.at("cyclic").scalar.i != 0});
            times.insert(times.end(), static_cast<std::size_t>(count), static_cast<double>(f.at("time").scalar.f));
        }
    auto buildings = m.classes.find("BUILDING");
    if (buildings == m.classes.end()) return;
    for (const auto &b : buildings->second) {
        auto at = b.fields.at("anims").slots.find(0);
        if (at == b.fields.at("anims").slots.end() || at->second.s.empty()) throw Error("BUILDING " + label(b.name) + ": missing construction ANIM[0]");
        const auto name = narrow_engine(at->second.s);
        auto       it   = std::find_if(anims.begin(), anims.end(), [&](const Anim &x) { return x.name == name; });
        if (it == anims.end()) throw Error("BUILDING " + label(b.name) + ": unresolved construction animation");
        if (it->cyclic && it->length > 1) throw Error("BUILDING " + label(b.name) + ": cyclic construction animation");
        double duration = 0;
        for (int j = 0; j < it->length; ++j) duration += times[it->start + static_cast<std::size_t>(j)];
        if (!std::isfinite(duration) || duration <= 0) throw Error("BUILDING " + label(b.name) + ": invalid construction animation duration");
        auto tr = std::find_if(std::begin(schema::transforms), std::end(schema::transforms), [](const schema::Transform &t) { return std::string_view(t.cls) == "BUILDING" && std::string_view(t.field) == "build_time"; });
        if (tr == std::end(schema::transforms)) throw Error("missing BUILDING build_time transform");
        double adjusted = transformed(m, *tr, b.fields.at("build_time").scalar.f, "BUILDING build_time");
        double factor   = adjusted / duration;
        if (!std::isfinite(factor)) throw Error("BUILDING " + label(b.name) + ": invalid animation retiming");
        for (int j = 0; j < it->length; ++j) {
            double &t = times[it->start + static_cast<std::size_t>(j)];
            t *= factor;
            if (!std::isfinite(t)) throw Error("BUILDING " + label(b.name) + ": animation retiming overflow");
        }
    }
    // A later constructor can retime a chain shared with an earlier building.
    // MAIN_BASE and CIVIL types cannot be constructed and their online
    // toggle callback deliberately permits zero-time static animations. The
    // selector for an actual construction needs a positive total denominator;
    // individual zero-time frames are harmless in its finite, acyclic walk.
    for (const auto &b : buildings->second) {
        const auto &slots     = b.fields.at("anims").slots;
        auto        at        = slots.find(0);
        auto        name      = narrow_engine(at->second.s);
        auto        it        = std::find_if(anims.begin(), anims.end(), [&](const Anim &x) { return x.name == name; });
        const Str  &type_name = b.fields.at("type").scalar.s;
        bool        found     = false;
        int         type      = (type_name.empty() ? 0 : lookup_define(m, type_name, found)) & 255;
        double      duration  = 0;
        for (int j = 0; j < it->length; ++j) duration += times[it->start + static_cast<std::size_t>(j)];
        if (!std::isfinite(duration) || duration < 0 ||
            (duration == 0 && !is_building_kind("nonconstructible", type)))
            throw Error("BUILDING " + label(b.name) + ": invalid retimed construction duration");

        // Default and human-airfield callbacks tick configured online slots
        // 1..sprite_quantity with no zero-time guard. Their terminal restart
        // and cyclic paths stay within the first repetition of the animation.
        if (is_building_kind("tick_strict", type)) {
            int count = b.fields.at("sprite_quantity").scalar.i;
            for (int slot = 1; slot <= count; ++slot) {
                auto s = slots.find(slot);
                if (s == slots.end() || s->second.s.empty()) continue;
                auto online_name = narrow_engine(s->second.s);
                auto online      = std::find_if(anims.begin(), anims.end(), [&](const Anim &x) { return x.name == online_name; });
                if (online == anims.end()) throw Error("BUILDING " + label(b.name) + ": unresolved online animation");
                for (int j = 0; j < online->length; ++j) {
                    double t = times[online->start + static_cast<std::size_t>(j)];
                    if (!std::isfinite(t) || t <= 0)
                        throw Error("BUILDING " + label(b.name) + ": invalid online animation frame time");
                }
            }
        }
        // Special handlers either guard zero time or have a sentinel slot.
        // Generated role names identify the proven reachable cfg slots.
        for (const auto &rule : schema::building_kinds) {
            if (rule.value != type) continue;
            std::string_view role   = rule.name, prefix;
            bool             strict = false, counted = false;
            if (role.starts_with("guarded_slot_")) prefix = "guarded_slot_";
            else if (role.starts_with("guarded_count_slot_")) {
                prefix  = "guarded_count_slot_";
                counted = true;
            } else if (role.starts_with("strict_count_slot_")) {
                prefix  = "strict_count_slot_";
                counted = true;
                strict  = true;
            } else continue;
            role.remove_prefix(prefix.size());
            int  slot   = 0;
            auto parsed = std::from_chars(role.data(), role.data() + role.size(), slot);
            if (parsed.ec != std::errc{} || parsed.ptr != role.data() + role.size() || slot < 1 || slot > 10)
                throw Error("invalid special animation schema");
            if (counted && slot > b.fields.at("sprite_quantity").scalar.i) continue;
            auto s = slots.find(slot);
            if (s == slots.end() || s->second.s.empty()) continue;
            auto online_name = narrow_engine(s->second.s);
            auto online      = std::find_if(anims.begin(), anims.end(), [&](const Anim &x) { return x.name == online_name; });
            if (online == anims.end()) throw Error("BUILDING " + label(b.name) + ": unresolved special animation");
            for (int j = 0; j < online->length; ++j) {
                double t = times[online->start + static_cast<std::size_t>(j)];
                if (!std::isfinite(t) || t < 0 || (strict && t == 0))
                    throw Error("BUILDING " + label(b.name) + ": invalid special animation frame time");
            }
        }
    }
}

struct Layer {
    const Input                         *game = nullptr;
    std::map<std::string, const Input *> langs;
};
} // namespace
Result prepare(std::span<const std::uint8_t> base_init, std::span<const std::uint8_t> base_lang, std::span<const Input> inputs, std::string_view locale) {
    Result result;
    if (inputs.empty()) {
        result.ok          = true;
        result.passthrough = true;
        return result;
    }
    try {
        if (inputs.size() > mh::mods::SET_PAYLOADS_MAX || base_init.size() > mh::mods::PAYLOAD_BYTES_MAX || base_lang.size() > mh::mods::PAYLOAD_BYTES_MAX) throw Error("config input limit exceeded");
        std::size_t                    total = base_init.size() + base_lang.size();
        std::map<std::uint32_t, Layer> layers;
        for (const auto &in : inputs) {
            if (in.bytes.size() > mh::mods::PAYLOAD_BYTES_MAX || in.bytes.size() > mh::mods::SET_BYTES_MAX - total) throw Error(std::string(in.source) + ": config input limit exceeded");
            total += in.bytes.size();
            auto &layer = layers[in.mod_index];
            if (in.path == "cfg\\patch.cfg") {
                if (layer.game) throw Error(std::string(in.source) + ": duplicate cfg patch");
                layer.game = &in;
            } else {
                if (locale.empty()) throw Error(std::string(in.source) + ": unknown locale with language patches");
                auto p = in.path;
                if (!p.starts_with("lang\\") || !p.ends_with(".cfg") || p.size() <= 9) throw Error(std::string(in.source) + ": invalid config input path");
                auto language = p.substr(5, p.size() - 9);
                if (language.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string_view::npos) throw Error(std::string(in.source) + ": invalid locale filename");
                if (!layer.langs.emplace(std::string(language), &in).second) throw Error(std::string(in.source) + ": duplicate language patch");
            }
        }
        Model m;
        Reader{m, "INIT.CFG"}.base(decode_utf16(base_init));
        Reader{m, "INITLANG.CFG"}.base(decode_utf16(base_lang));
        std::vector<Str> original_maps;
        auto             planets = m.classes.find("PLANET");
        if (planets != m.classes.end())
            for (const auto &p : planets->second) original_maps.push_back(p.fields.at("map").scalar.s);
        std::map<std::uint32_t, std::map<std::string, TextChanges>> translations;
        std::set<std::string>                                       languages;
        for (const auto &[mod, layer] : layers) {
            if (layer.game) patch(m, decode_utf8(layer.game->bytes), std::string(layer.game->source));
            for (const auto &[language, input] : layer.langs) {
                auto c = text_patch(m, decode_utf8(input->bytes), std::string(input->source));
                for (const auto &[index, value] : c) valid_string(value, static_cast<std::size_t>(cap_limit("TEXT", "value", "utf16")), std::string(input->source) + ": TEXT", true);
                translations[mod].emplace(language, std::move(c));
                languages.insert(language);
            }
        }
        if (!locale.empty()) languages.insert(std::string(locale));
        TextChanges selected;
        for (const auto &language : languages) {
            TextChanges effective;
            for (const auto &[mod, by_language] : translations) {
                auto merge = [&](std::string_view lang) { auto it=by_language.find(std::string(lang)); if(it!=by_language.end()) for(const auto& [i,v]:it->second) effective[i]=v; };
                merge("en");
                if (language != "en") merge(language);
            }
            validate_localized(m, effective);
            if (language == locale) selected = std::move(effective);
        }
        for (const auto &[i, v] : selected) m.texts[i].value = v;
        validate(m);
        validate_building_values(m);
        validate_localized(m, {});
        validate_animations(m);
        std::vector<std::string> maps;
        planets = m.classes.find("PLANET");
        if (planets != m.classes.end()) {
            std::set<std::string> seen;
            std::size_t           i = 0;
            for (const auto &p : planets->second) {
                const Str &name = p.fields.at("map").scalar.s;
                if (name.empty() && i < original_maps.size() && !original_maps[i].empty()) throw Error("PLANET \"" + label(p.name) + "\": MAP cannot be cleared");
                if (i < original_maps.size() && name != original_maps[i] && !name.empty()) {
                    auto n = narrow_engine(name);
                    if (seen.insert(n).second) maps.push_back(std::move(n));
                }
                ++i;
            }
        }
        auto output                = emit(m);
        result.init                = std::move(output.first);
        result.lang                = std::move(output.second);
        result.changed_planet_maps = std::move(maps);
        result.ok                  = true;
        return result;
    } catch (const std::exception &e) {
        result.error = e.what();
        return result;
    }
}
} // namespace mh::mods::cfg
