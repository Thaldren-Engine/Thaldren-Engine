// Mission text parsers: placed units' order strings and the goal keys, with the original's
// C runtime conversions (docs/RE_MEMORY.md, "Campaign").

#include <algorithm>
#include <cstring>

#include "ta/campaign.h"
#include "ta/tdf.h"

namespace ta {

namespace {

// --- The C runtime's sscanf conversions used by the mission parsers (enough of them).

bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

struct Scanner {
    const char* p;
    void skip_ws() {
        while (*p && is_space(*p)) ++p;
    }
    // %[set]: at least one character of the set.
    bool set(bool (*in)(char), std::string& out) {
        const char* s = p;
        while (*p && in(*p)) ++p;
        if (p == s) return false;
        out.assign(s, p);
        return true;
    }
    bool lit(char c) {
        if (*p != c) return false;
        ++p;
        return true;
    }
    // %d: decimal; %i: base from the prefix (0x hex, 0 octal). 32-bit wrap.
    bool integer(std::int32_t& out, bool auto_base) {
        skip_ws();
        const char* s = p;
        bool neg = false;
        if (*p == '+' || *p == '-') neg = *p++ == '-';
        int base = 10;
        if (auto_base && *p == '0') {
            if (p[1] == 'x' || p[1] == 'X') {
                base = 16;
                p += 2;
            } else {
                base = 8;
            }
        }
        std::uint32_t v = 0;
        const char* d = p;
        for (;; ++p) {
            int digit;
            if (is_digit(*p)) digit = *p - '0';
            else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
            else break;
            if (digit >= base) break;
            v = v * static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(digit);
        }
        if (p == d && base != 8) {
            p = s;
            return false;
        }
        out = static_cast<std::int32_t>(neg ? 0u - v : v);
        return true;
    }
    // %f into a float.
    bool real(float& out) {
        skip_ws();
        const char* s = p;
        if (*p == '+' || *p == '-') ++p;
        int digits = 0;
        while (is_digit(*p)) ++p, ++digits;
        if (*p == '.') {
            ++p;
            while (is_digit(*p)) ++p, ++digits;
        }
        if (!digits) {
            p = s;
            return false;
        }
        if (*p == 'e' || *p == 'E') {
            const char* e = p++;
            if (*p == '+' || *p == '-') ++p;
            if (!is_digit(*p)) p = e;
            else
                while (is_digit(*p)) ++p;
        }
        out = static_cast<float>(msvc_atof(std::string(s, p)));
        return true;
    }
};

bool name_chars(char c) { return is_alpha(c) || is_digit(c) || c == '_' || c == '.'; }
bool name_chars_no_underscore(char c) { return is_alpha(c) || is_digit(c) || c == '.'; }

// The C runtime's float-to-integer conversion of a product computed at double precision
// (values out of the 64-bit range give the low word of 0x8000000000000000).
std::int32_t ftol(double v) {
    if (!(v > -9.2233720368547758e18 && v < 9.2233720368547758e18)) return 0;
    return static_cast<std::int32_t>(static_cast<std::int64_t>(v));
}

// The order letters of InitialMission (0x487BF0).
void parse_orders(const std::string& text, const std::function<std::uint16_t(const std::string&)>& type_index, MissionParseState& st,
                  std::vector<sim::MissionUnitOrder>& out) {
    using K = sim::MissionUnitOrder::Kind;
    const char* p = text.c_str();
    while (*p) {
        while (*p && is_space(*p)) ++p;
        const std::size_t n = std::strcspn(p, ",");
        std::string tok(p, n);
        p += n;
        if (*p == ',') ++p;
        if (tok.empty()) continue;
        auto pos = [&] {
            return std::array<std::int32_t, 3>{ftol(static_cast<double>(st.order_x) * 65536.0), 0, ftol(static_cast<double>(st.order_z) * 65536.0)};
        };
        // A name read into the token's own buffer: on failure the buffer keeps the token.
        auto name_from = [&](std::size_t at, bool (*in)(char), bool& got) {
            Scanner s{tok.c_str() + std::min(at, tok.size())};
            s.skip_ws();
            std::string name;
            got = s.set(in, name);
            return got ? name : tok;
        };
        sim::MissionUnitOrder o;
        switch (tok[0]) {
        case 'A':
        case 'a': {
            Scanner s{tok.c_str() + 1};
            float x = 0, z = 0;
            int got = 0;
            if (s.real(x)) {
                st.order_x = x;
                ++got;
                if (s.real(z)) {
                    st.order_z = z;
                    ++got;
                }
            }
            if (got == 2) {
                o.kind = K::AttackAt;
                o.pos = pos();
                o.final_order = true;
                out.push_back(o);
            } else {
                bool ok;
                const std::string name = name_from(1, name_chars, ok);
                if (const std::uint16_t t = type_index(name)) {
                    o.kind = K::AttackType;
                    o.type = t;
                    out.push_back(o);
                }
            }
            break;
        }
        case 'B':
        case 'b': {
            std::int32_t count = 1;
            if (tok.size() > 1 && (tok[1] == 'w' || tok[1] == 'W')) {
                Scanner s{tok.c_str() + 2};
                s.integer(count, false);
                o.kind = K::BuildWeapon;
                o.count = count;
                out.push_back(o);
                break;
            }
            Scanner s{tok.c_str() + 1};
            s.skip_ws();
            std::string name;
            if (s.set(name_chars, name)) {
                if (s.integer(count, false)) {
                    float x, z;
                    if (s.real(x)) {
                        st.order_x = x;
                        if (s.real(z)) st.order_z = z;
                    }
                }
            } else {
                name = tok;
            }
            if (const std::uint16_t t = type_index(name)) {
                o.kind = K::Build;
                o.type = t;
                o.count = count;
                o.pos = pos();
                out.push_back(o);
            }
            break;
        }
        case 'D':
        case 'd':
            o.kind = K::SelfDestruct;
            o.final_order = true;
            out.push_back(o);
            break;
        case 'G':
        case 'g':
        case 'I':
        case 'i': {
            bool ok;
            o.kind = (tok[0] == 'G' || tok[0] == 'g') ? K::Guard : K::LoadInto;
            o.ref = name_from(1, name_chars, ok);
            o.ref_given = true;
            out.push_back(o);
            break;
        }
        case 'M':
        case 'm':
        case 'U':
        case 'u': {
            Scanner s{tok.c_str() + 1};
            float x, z;
            if (s.real(x)) {
                st.order_x = x;
                if (s.real(z)) st.order_z = z;
            }
            o.kind = (tok[0] == 'M' || tok[0] == 'm') ? K::Move : K::Unload;
            o.pos = pos();
            out.push_back(o);
            break;
        }
        case 'O':
        case 'o': {
            Scanner s{tok.c_str() + 1};
            std::int32_t move = -1, fire = -1;
            if (s.integer(move, false)) s.integer(fire, false);
            o.kind = K::StandingOrders;
            o.count = move;  // -1: unchanged
            o.param = fire;
            out.push_back(o);
            break;
        }
        case 'P':
        case 'p': {
            Scanner s{tok.c_str() + 1};
            float x, z, t = 0.0f;
            if (s.real(x)) {
                st.order_x = x;
                if (s.real(z)) {
                    st.order_z = z;
                    s.real(t);
                }
            }
            o.kind = K::Patrol;
            o.pos = pos();
            o.param = ftol(static_cast<double>(t) * 30.0);
            o.final_order = true;
            out.push_back(o);
            break;
        }
        case 'S':
        case 's':
            o.kind = K::MakeSelectable;
            o.final_order = true;
            out.push_back(o);
            break;
        case 'W':
        case 'w':
            if (tok.size() > 1 && (tok[1] == 'a' || tok[1] == 'A')) {
                bool ok;
                o.kind = K::WaitForAttack;
                o.ref = name_from(2, name_chars_no_underscore, ok);
                o.ref_given = ok;
                out.push_back(o);
            } else {
                Scanner s{tok.c_str() + 1};
                float t = 0.0f;
                std::int32_t v = 0;
                if (s.real(t)) s.integer(v, false);
                o.kind = K::Wait;
                o.param = ftol(static_cast<double>(t) * 30.0);
                o.count = v;
                out.push_back(o);
            }
            break;
        default:
            break;
        }
    }
}

// Win and loss goal keys of [GlobalHeader] (0x48E010) with the parser's shared scratch.
}  // namespace

void read_mission_goals(const TdfSection& gh, MissionParseState& st, sim::MissionGoals& g) {
    using GK = sim::GoalKind;
    std::int32_t& v10 = st.goal_words[0];
    std::int32_t& v14 = st.goal_words[1];
    std::int32_t& v24 = st.goal_words[2];
    std::int32_t& v28 = st.goal_words[3];
    std::int32_t v18 = 0, v20 = 0;
    (void)v18;
    (void)v20;
    const std::int32_t rec = static_cast<std::int32_t>(st.goal_record_address);
    auto str = [&](const char* key, std::string& out) {
        bool found = false;
        out = gh.get_string(key, 0x100, "", &found);
        return found;
    };
    // "%[a-zA-Z],%i[,%i,%i]" into the name buffer and the given slots.
    auto scan = [&](const std::string& text, std::initializer_list<std::int32_t*> ints) {
        Scanner s{text.c_str()};
        std::string name;
        if (!s.set(is_alpha, name)) return;
        st.goal_name = name;
        for (std::int32_t* v : ints) {
            if (!s.lit(',')) return;
            std::int32_t x;
            if (!s.integer(x, true)) return;
            *v = x;
        }
    };
    auto any_type = [](const std::string& n) { return stricmp_c(n, "ANYTYPE") == 0 ? std::string() : n; };
    std::string text;
    if (gh.get_int("KillEnemyCommander", 0)) g.win.push_back({GK::KillEnemyCommander});
    if (gh.get_int("DestroyAllUnits", 0)) g.win.push_back({GK::DestroyAllUnits});
    if (gh.get_int("KillAllMobileUnits", 0)) g.win.push_back({GK::KillAllMobileUnits});
    if (str("BuildUnitType", text)) {
        sim::MissionGoal m{GK::BuildUnitType};
        m.type_name = text;
        g.win.push_back(m);
        v10 = rec + 0x10;
    }
    if (str("CaptureUnitType", text)) {
        sim::MissionGoal m{GK::CaptureUnitType};
        m.type_name = text;
        g.win.push_back(m);
        v10 = rec + 0x0C;
    }
    if (str("KillAllOfType", text)) {
        sim::MissionGoal m{GK::KillAllOfType};
        m.type_name = text;
        g.win.push_back(m);
        v10 = rec + 0x10;
    }
    if (str("KillUnitType", text)) {
        scan(text, {&v10});
        v28 = v10;
        v24 = rec + 0x0C;
        sim::MissionGoal m{GK::KillUnitType};
        m.type_name = st.goal_name;
        m.count = v28;
        g.win.push_back(m);
    }
    if (str("MoveUnitToRadius", text)) {
        scan(text, {&v10, &v24, &v28});
        v20 = v28;
        v14 = v24;
        v18 = v10;
        sim::MissionGoal m{GK::MoveUnitToRadius};
        m.type_name = any_type(st.goal_name);
        m.pos = {v18, 0, v14};
        m.radius = static_cast<std::int32_t>(static_cast<std::uint32_t>(v20) << 16);
        g.win.push_back(m);
    }
    for (const char* key : {"UnitTypePassesX", "UnitTypePassesZ"})
        if (str(key, text)) {
            scan(text, {&v14});
            v18 = v14;
            sim::MissionGoal m{key[13] == 'X' ? GK::UnitTypeNearX : GK::UnitTypeNearZ};
            m.type_name = any_type(st.goal_name);
            m.line = v14 >> 4;
            g.win.push_back(m);
        }
    if (const std::int32_t t = gh.get_int("VictoryTimerRunsOut", 0); t > 0) {
        sim::MissionGoal m{GK::VictoryTimer};
        m.deadline = static_cast<std::uint32_t>(t * 30);
        g.win.push_back(m);
    }
    if (gh.get_int("CommanderKilled", 0)) g.loss.push_back({GK::CommanderKilled});
    if (gh.get_int("AllUnitsKilled", 0)) g.loss.push_back({GK::AllUnitsKilled});
    if (str("AllUnitsKilledOfType", text)) {
        sim::MissionGoal m{GK::AllUnitsKilledOfType};
        m.type_name = text;
        g.loss.push_back(m);
        v14 = rec + 0x10;
    }
    if (str("UnitTypeKilled", text)) {
        scan(text, {&v14});
        v20 = v14;
        v18 = rec + 0x0C;
        sim::MissionGoal m{GK::UnitTypeKilled};
        m.type_name = st.goal_name;
        m.count = v20;
        g.loss.push_back(m);
    }
    if (const std::int32_t t = gh.get_int("DeathTimerRunsOut", 0); t > 0) {
        sim::MissionGoal m{GK::DeathTimer};
        m.deadline = static_cast<std::uint32_t>(t * 30);
        g.loss.push_back(m);
    }
    for (const char* key : {"AnyUnitPassesX", "AnyUnitPassesZ"})
        if (const std::int32_t v = gh.get_int(key, -1); v > -1) {
            sim::MissionGoal m{key[13] == 'X' ? GK::AnyUnitNearX : GK::AnyUnitNearZ};
            m.line = v >> 4;
            g.loss.push_back(m);
        }
    if (g.win.empty()) g.win.push_back({GK::DestroyAllUnits});
    if (g.loss.empty()) g.loss.push_back({GK::AllUnitsKilled});
}


std::vector<sim::MissionUnitOrder> parse_initial_orders(const std::string& text, const std::function<std::uint16_t(const std::string&)>& type_index,
                                                        MissionParseState& state) {
    std::vector<sim::MissionUnitOrder> out;
    parse_orders(text, type_index, state, out);
    return out;
}

}  // namespace ta
