// Campaigns and mission files (docs/RE_MEMORY.md, "Campaign").

#include "ta/campaign.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ta/tdf.h"

namespace ta {

namespace {

std::unique_ptr<TdfSection> parse_file(Vfs& vfs, const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!vfs.read(path, bytes)) return nullptr;
    return tdf_parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).root;
}

std::string strip_ext(const std::string& s) {
    const std::size_t dot = s.find_last_of('.');
    const std::size_t slash = s.find_last_of("\\/");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return s;
    return s.substr(0, dot);
}

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
void read_goals(const TdfSection& gh, MissionParseState& st, sim::MissionGoals& g) {
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

}  // namespace

void mount_cd_archives(Vfs& vfs, const std::filesystem::path& game_dir) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(game_dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext != ".HPI") continue;
        bool mounted = false;
        for (const auto& a : vfs.archives())
            if (std::filesystem::equivalent(a->path(), e.path(), ec)) mounted = true;
        if (!mounted) found.push_back(e.path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return stricmp_c(a.filename().string(), b.filename().string()) < 0; });
    for (const auto& p : found) {
        HpiError err;
        vfs.mount(p, err);
    }
}

std::vector<std::string> list_campaigns(Vfs& vfs) {
    std::vector<std::string> out;
    for (const std::string& n : vfs.enumerate("camps\\*.tdf")) out.push_back(strip_ext(n));
    return out;
}

bool load_campaign(Vfs& vfs, const std::string& name, Campaign& out, std::string& error) {
    const std::unique_ptr<TdfSection> root = parse_file(vfs, "camps\\" + name + ".tdf");
    if (!root) {
        error = "The requested campaign file (" + name + ") could not be found";
        return false;
    }
    out = Campaign{};
    out.name = name;
    if (const TdfSection* h = root->child("HEADER")) out.side = h->get_string("campaignside", 0x100, "");
    for (int n = 0;; ++n) {
        const TdfSection* m = root->child("MISSION" + std::to_string(n));
        if (!m) break;
        CampaignMission cm;
        cm.file = m->get_string("missionfile", 0x100, "");
        cm.name = m->get_string("missionname", 0x100, "");
        out.missions.push_back(cm);
    }
    return true;
}

std::vector<sim::MissionUnitOrder> parse_initial_orders(const std::string& text, const std::function<std::uint16_t(const std::string&)>& type_index,
                                                        MissionParseState& state) {
    std::vector<sim::MissionUnitOrder> out;
    parse_orders(text, type_index, state, out);
    return out;
}

bool load_mission(Vfs& vfs, const std::string& file, int difficulty, MissionInfo& out, std::string& error, MissionParseState* parse) {
    MissionParseState local;
    MissionParseState& st = parse ? *parse : local;
    const std::unique_ptr<TdfSection> root = parse_file(vfs, "Maps\\" + file);
    if (!root) {
        error = "There is no mission file " + file;
        return false;
    }
    const TdfSection* gh = root->child("GlobalHeader");
    if (!gh) {
        error = "No GlobalHeader block in mission file " + file;
        return false;
    }
    out = MissionInfo{};
    out.file = file;
    out.map = strip_ext(file);
    out.max_units = gh->get_int("maxunits", 200);
    out.name = gh->get_string("missionname", 0x100, "");
    out.brief = gh->get_string("brief", 0x100, "");
    out.narration = gh->get_string("narration", 0x100, "");
    out.hint = gh->get_string("missionhint", 0x100, "");
    out.glamour = gh->get_string("glamour", 0x100, "");
    out.glamour_sound = gh->get_string("glamoursound", 0x100, "");
    out.use_only = gh->get_string("UseOnlyUnits", 0x100, "");
    out.mapping = gh->get_int("mapping", 0);
    out.line_of_sight = gh->get_int("lineofsight", 0);
    out.memory = gh->get_string("memory", 0x80, "");
    out.numplayers = gh->get_string("numplayers", 0x80, "");
    out.planet = gh->get_string("Planet", 0x80, "");
    out.no_movie = gh->get_int("nomovie", 0) != 0;
    out.description = gh->get_string("missiondescription", 0x80, "No description available");
    out.kill_mul = static_cast<float>(gh->get_float("killmul", 0.0));
    out.time_mul = static_cast<float>(gh->get_float("timemul", 0.0));
    if (!out.brief.empty()) {
        std::vector<std::uint8_t> t;
        if (vfs.read("camps\\briefs\\" + strip_ext(out.brief) + ".txt", t)) out.brief_text.assign(t.begin(), t.end());
    }
    read_goals(*gh, st, out.goals);
    // The schema for the difficulty (0x436860).
    static const char* const kTypes[3] = {"Easy", "Medium", "Hard"};
    static const int kOrder[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}};
    const int d = difficulty < 0 ? 0 : difficulty > 2 ? 2 : difficulty;
    const TdfSection* schema = nullptr;
    for (int k = 0; k < 3 && !schema; ++k)
        for (int n = 0;; ++n) {
            const TdfSection* s = gh->child("Schema " + std::to_string(n));
            if (!s) break;
            if (stricmp_c(s->get_string("Type", 0x20, ""), kTypes[kOrder[d][k]]) == 0) {
                schema = s;
                out.schema = n;
                out.schema_type = kTypes[kOrder[d][k]];
                break;
            }
        }
    if (!schema) {
        error = "No suitable schema type in mission file " + file;
        return false;
    }
    out.human_metal = static_cast<float>(schema->get_int("HumanMetal", 0));
    out.human_energy = static_cast<float>(schema->get_int("HumanEnergy", 0));
    out.computer_metal = static_cast<float>(schema->get_int("ComputerMetal", 0));
    out.computer_energy = static_cast<float>(schema->get_int("ComputerEnergy", 0));
    out.surface_metal = schema->get_int("SurfaceMetal", 0);
    out.ai_profile = schema->get_string("aiprofile", 0x100, "");
    // Placed units (0x436C30).
    if (const TdfSection* units = schema->child("units"))
        for (const auto& u : units->children()) {
            sim::MissionUnit m;
            m.unit_name = u->get_string("Unitname", 0x400, "");
            m.ident = u->get_string("Ident", 0x400, "");
            bool has = false;
            const std::string orders = u->get_string("InitialMission", 0x400, "", &has);
            m.has_orders = has;
            m.pos = {static_cast<std::int32_t>(static_cast<std::uint32_t>(u->get_int("XPos", 0)) << 16),
                     static_cast<std::int32_t>(static_cast<std::uint32_t>(u->get_int("YPos", 0)) << 16),
                     static_cast<std::int32_t>(static_cast<std::uint32_t>(u->get_int("ZPos", 0)) << 16)};
            m.heading = static_cast<std::uint16_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(u->get_int("Angle", 0)) << 16) / 360);
            std::uint8_t player = static_cast<std::uint8_t>(u->get_int("Player", 0));
            if (player == 0) player = 1;
            m.player = static_cast<std::uint8_t>(player - 1);
            m.health_percent = static_cast<std::int16_t>(u->get_int("HealthPercentage", 100));
            m.immune = (u->get_int("Immunity", 0) & 1) != 0;
            // Orders keep their text until the battle's types are known.
            m.order_text = orders;
            out.units.push_back(m);
        }
    if (const TdfSection* specials = schema->child("specials")) {
        std::int32_t running = 0;
        for (const auto& c : specials->children()) {
            MissionSpecial sp;
            bool found = false;
            const std::string what = c->get_string("specialwhat", 0x100, "", &found);
            if (found && what.size() >= 8 && stricmp_c(what.substr(0, 8), "StartPos") == 0) {
                sp.start_pos = true;
                sp.x = static_cast<std::int16_t>(c->get_int("XPos", 0));
                sp.z = static_cast<std::int16_t>(c->get_int("ZPos", 0));
                std::int32_t id = what.size() > 8 && is_digit(what[8]) ? msvc_atoi(what.substr(8)) : ++running;
                sp.id = id > 0 ? id - 1 : id;
            }
            out.specials.push_back(sp);
        }
    }
    if (const TdfSection* features = schema->child("features"))
        for (const auto& f : features->children()) {
            MissionFeature mf;
            mf.name = f->get_string("Featurename", 0x80, "");
            mf.x = f->get_int("XPos", -1);
            mf.z = f->get_int("ZPos", -1);
            out.features.push_back(mf);
        }
    return true;
}

std::unique_ptr<LoadedBattle> load_mission_battle(Vfs& vfs, const MissionInfo& mission, const MissionRequest& rq, sim::BattleOutside& outside,
                                                  std::string& error) {
    BattleRequest req;
    req.map = mission.map;
    req.schema = mission.schema;
    req.sides[0] = rq.campaign_side;
    req.sides[1] = !rq.other_side.empty() ? rq.other_side : (stricmp_c(rq.campaign_side, "ARM") == 0 ? "CORE" : "ARM");
    sim::BattleStart& s = req.start;
    s.provider = 1;
    s.players[0].kind = sim::PlayerKind::Local;
    s.players[0].net_id = 1;
    s.players[0].colour = 0;
    s.players[1].kind = sim::PlayerKind::Computer;
    s.players[1].net_id = 2;
    s.players[1].colour = 1;
    s.unit_limit = static_cast<std::uint16_t>(mission.max_units);
    s.fog_of_war = mission.line_of_sight != 0;
    s.explore = mission.mapping != 0;
    s.true_sight = true;
    s.commander_mode = 0;
    s.ai_difficulty = rq.difficulty;
    s.local_player = 0;
    s.battle_rng_seed = rq.battle_rng_seed;
    s.loading_rand_seed = rq.loading_rand_seed;
    s.crt_rand_state = rq.crt_rand_state;
    s.mission.goals = mission.goals;
    s.mission.metal[0] = mission.human_metal;
    s.mission.energy[0] = mission.human_energy;
    s.mission.metal[1] = mission.computer_metal;
    s.mission.energy[1] = mission.computer_energy;
    const std::string use_only = mission.use_only;
    req.prepare_start = [&vfs, mission, use_only](LoadedBattle& lb, sim::BattleStart& start) {
        start.mission.units = mission.units;
        for (sim::MissionUnit& u : start.mission.units) u.type = lb.type_index(u.unit_name);
        // The order parser keeps its scratch between units, as the original's stack does.
        auto state = std::make_shared<MissionParseState>();
        LoadedBattle* plb = &lb;
        start.mission.parse_orders = [plb, state](const std::string& text) {
            return parse_initial_orders(text, [plb](const std::string& n) { return plb->type_index(n); }, *state);
        };
        if (!use_only.empty()) {
            const std::unique_ptr<TdfSection> list = parse_file(vfs, "camps\\useonly\\" + strip_ext(use_only) + ".tdf");
            if (list) {
                start.mission.restrict_types = true;
                for (const auto& sec : list->children())
                    if (const std::uint16_t t = lb.type_index(sec->name())) start.mission.allowed_types.push_back(t);
            }
        }
    };
    return load_battle(vfs, req, outside, error);
}

}  // namespace ta
