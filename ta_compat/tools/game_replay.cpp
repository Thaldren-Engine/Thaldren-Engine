// Whole-game replay: a game of the original captured with `ta_capture --game` (see
// ta_compat/tools/capture) is rebuilt with ta::load_battle from the captured start, the
// interface's orders are applied between steps at the steps they were given, the stale stack
// words the original read are fed in where it read them, and after every step the engine's
// state is compared with the captured digest. The first divergence is reported in full.
//
// Usage: game_replay <game_dir> <capture.jsonl> [--steps N] [--continue] [--no-draw-effects]
//                    [--ignore-cells] [--verbose]
//
// Frames: the capture records the end of every frame's steps; the replay calls end_frame()
// there. The C runtime rand() draws the original makes between frames (drawing lightning,
// the CD audio sync, the interface) are taken from the capture: at the first step of a frame
// the engine's rand() state is set to the captured one and the number of draws counted.

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/commands.h"
#include "ta/battle_loader.h"
#include "ta/hpi.h"
#include "ta/tdf.h"

namespace {

// ---- A small JSON reader (the capture's own output: objects, arrays, integers, strings) ----
struct Json {
    enum Type { Null, Num, Str, Arr, Obj } type = Null;
    long long num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* k) const {
        for (const auto& [key, v] : obj)
            if (key == k) return &v;
        return nullptr;
    }
    long long i(const char* k, long long def = 0) const {
        const Json* v = get(k);
        return v && v->type == Num ? v->num : def;
    }
    std::string s(const char* k) const {
        const Json* v = get(k);
        return v && v->type == Str ? v->str : std::string();
    }
};

struct JsonParser {
    const char* p;
    void ws() {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    }
    bool parse(Json& out) {
        ws();
        if (*p == '{') {
            out.type = Json::Obj;
            ++p;
            ws();
            if (*p == '}') return ++p, true;
            for (;;) {
                Json key;
                if (!parse(key) || key.type != Json::Str) return false;
                ws();
                if (*p++ != ':') return false;
                Json v;
                if (!parse(v)) return false;
                out.obj.emplace_back(key.str, std::move(v));
                ws();
                if (*p == ',') {
                    ++p;
                    continue;
                }
                if (*p == '}') return ++p, true;
                return false;
            }
        }
        if (*p == '[') {
            out.type = Json::Arr;
            ++p;
            ws();
            if (*p == ']') return ++p, true;
            for (;;) {
                Json v;
                if (!parse(v)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (*p == ',') {
                    ++p;
                    continue;
                }
                if (*p == ']') return ++p, true;
                return false;
            }
        }
        if (*p == '"') {
            out.type = Json::Str;
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) ++p;
                out.str += *p++;
            }
            if (*p != '"') return false;
            return ++p, true;
        }
        if (*p == 'n' && std::strncmp(p, "null", 4) == 0) {
            p += 4;
            return true;
        }
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            out.type = Json::Num;
            char* end = nullptr;
            out.num = std::strtoll(p, &end, 10);
            p = end;
            return true;
        }
        if (*p == '"' || *p == 0) return false;
        return false;
    }
};

std::vector<std::string> g_type_names;  // unit names by battle type index (for reports)
const char* tname(long long type) {
    return type >= 0 && static_cast<std::size_t>(type) < g_type_names.size() ? g_type_names[static_cast<std::size_t>(type)].c_str() : "?";
}

std::uint32_t fbits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Steps of the C runtime generator from `from` to `to` (-1: not within `limit`).
long long lcg_distance(std::uint32_t from, std::uint32_t to, long long limit = 2000000) {
    std::uint32_t s = from;
    for (long long n = 0; n <= limit; ++n) {
        if (s == to) return n;
        s = s * 214013u + 2531011u;
    }
    return -1;
}

// Stale stack words fed in per step, in the order the original read them.
struct ReplayOutside : sim::QuietBattleOutside {
    sim::Battle* battle = nullptr;
    std::map<std::uint32_t, std::deque<std::int32_t>> site_y, reclaim_y;
    std::vector<std::string> notes;
    std::int32_t ai_site_height_word(int player, std::int32_t fallback) override {
        auto& q = site_y[battle ? battle->game_tick() : 0];
        if (q.empty()) {
            notes.push_back("step " + std::to_string(battle ? battle->game_tick() : 0) + ": the engine ran a constructor role for player " +
                            std::to_string(player) + " the original did not (site height word missing)");
            return fallback;
        }
        const std::int32_t v = q.front();
        q.pop_front();
        return v;
    }
    // The hover bob's wall clock (0x4B6340), per call; the air repair patrol's site record
    // address per step (a stack address: the same for every call of a step).
    std::map<std::uint32_t, std::deque<std::uint32_t>> clock;
    std::map<std::uint32_t, std::uint32_t> air_repair_site;
    std::uint32_t last_clock = 0;
    std::uint32_t clock_ticks() override {
        auto& q = clock[battle ? battle->game_tick() : 0];
        if (q.empty()) {
            notes.push_back("step " + std::to_string(battle ? battle->game_tick() : 0) +
                            ": the engine read the hover clock more often than the original");
            return last_clock;
        }
        last_clock = q.front();
        q.pop_front();
        return last_clock;
    }
    std::int32_t reclaim_site_y(std::int32_t fallback) override {
        auto& q = reclaim_y[battle ? battle->game_tick() : 0];
        if (q.empty()) {
            notes.push_back("step " + std::to_string(battle ? battle->game_tick() : 0) +
                            ": the engine picked reclaim sites the original did not (site y missing)");
            return fallback;
        }
        const std::int32_t v = q.front();
        q.pop_front();
        return v;
    }
};

// The engine's state in the capture's digest layout.
struct UnitRow {
    std::vector<long long> v;  // the scalar fields
    std::vector<long long> primary, secondary;
    // Motion: speed, turn, flags & 7, then for a route goal: (vtable), count, search tick,
    // flags & 15, points.
    std::vector<long long> motion;
    bool has_motion_row = false;
    std::vector<long long> vel_tilt;  // motion velocity xyz, tilt xyz (newer captures)
    bool has_vel_tilt = false;
};
const char* const kUnitFields[] = {"id",     "type",  "owner",  "x",     "y",        "z",     "pitch",   "heading",
                                   "bank",   "hp",    "build_left_bits", "activate", "script_flags", "flags", "health_pct",
                                   "kills",  "reload0", "reload1", "reload2"};
constexpr int kUnitScalars = 19;
// Flag bits only the interface writes (selected, on-screen marks, build menu page).
constexpr long long kInterfaceFlags = 0x10 | 0x40 | 0x80 | 0x03C00000;

std::map<int, UnitRow> engine_units(sim::Battle& b) {
    std::map<int, UnitRow> out;
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        const sim::UnitTickState& t = b.ticks[i];
        if (!t.type_index) continue;
        const sim::Unit& u = b.units[i];
        UnitRow r;
        r.v = {static_cast<long long>(i), t.type_index, t.owner, u.pos[0], u.pos[1], u.pos[2], u.pitch, u.heading, u.bank,
               u.hit_points, fbits(u.build_fraction_left), u.activate_flags, u.script_flags, u.flags, t.health_percent,
               t.kill_count, u.weapons[0].reload, u.weapons[1].reload, u.weapons[2].reload};
        for (const sim::MissionOrder* o = u.orders; o && r.primary.size() < 32; o = o->next) r.primary.push_back(o->kind);
        for (const sim::MissionOrder* o = u.secondary_orders; o && r.secondary.size() < 32; o = o->next) r.secondary.push_back(o->kind);
        if (t.has_motion) {
            r.vel_tilt = {t.velocity[0], t.velocity[1], t.velocity[2], t.motion.tilt[0], t.motion.tilt[1], t.motion.tilt[2]};
            r.motion = {t.motion.speed, t.motion.turn, t.motion.flags & 7};
            if (const auto* rg = dynamic_cast<const sim::RouteGoal*>(t.motion.goal.get())) {
                r.motion.push_back(-1);  // (the original's vtable is not compared)
                r.motion.push_back(rg->count);
                r.motion.push_back(rg->search_tick);
                r.motion.push_back(rg->flags & 0xF);
                for (std::int32_t k = 0; k < rg->count && k < 20; ++k) r.motion.push_back(rg->points[static_cast<std::size_t>(k)]);
            } else if (t.motion.goal) {
                r.motion.push_back(-2);  // another goal kind
            }
        }
        out[static_cast<int>(i)] = std::move(r);
    }
    return out;
}

std::uint32_t fnv(std::uint32_t h, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

std::string list(const std::vector<long long>& v) {
    std::string s = "[";
    for (std::size_t k = 0; k < v.size(); ++k) s += (k ? "," : "") + std::to_string(v[k]);
    return s + "]";
}

// Compares the engine with one captured digest; returns the differences (empty: identical).
std::vector<std::string> compare(sim::Battle& b, const Json& d) {
    std::vector<std::string> diff;
    char m[512];
    const auto rnd = static_cast<std::uint32_t>(d.i("rand")), brng = static_cast<std::uint32_t>(d.i("brng"));
    if (b.crt_rand.state() != rnd) {
        std::snprintf(m, sizeof m, "rand() state: original %08x, engine %08x (engine is %lld draws behind)", rnd, b.crt_rand.state(),
                      lcg_distance(b.crt_rand.state(), rnd, 100000));
        diff.push_back(m);
    }
    if (b.battle_rng().state() != brng) {
        std::snprintf(m, sizeof m, "battle generator: original %08x, engine %08x", brng, b.battle_rng().state());
        diff.push_back(m);
    }
    if (const Json* w = d.get("wind"); w && w->arr.size() == 2) {
        if (w->arr[0].num != b.wind.heading || w->arr[1].num != b.wind.speed) {
            std::snprintf(m, sizeof m, "wind: original heading %lld speed %lld, engine %u %d", w->arr[0].num, w->arr[1].num, b.wind.heading,
                          b.wind.speed);
            diff.push_back(m);
        }
    }
    if (const Json* ps = d.get("players"))
        for (const Json& p : ps->arr) {
            if (p.arr.size() < 5) continue;
            const auto slot = static_cast<std::size_t>(p.arr[0].num);
            if (slot >= 10) continue;
            const sim::PlayerSlot& ps2 = b.players[slot];
            const sim::PlayerEconomy& pe = b.player_economy[slot];
            const long long ours[4] = {fbits(ps2.energy_stored), fbits(ps2.metal_stored), fbits(pe.energy_storage), fbits(pe.metal_storage)};
            const char* names[4] = {"energy", "metal", "energy storage", "metal storage"};
            for (int k = 0; k < 4; ++k)
                if (p.arr[static_cast<std::size_t>(k + 1)].num != ours[k]) {
                    float a, e;
                    const auto ab = static_cast<std::uint32_t>(p.arr[static_cast<std::size_t>(k + 1)].num);
                    const auto eb = static_cast<std::uint32_t>(ours[k]);
                    std::memcpy(&a, &ab, 4);
                    std::memcpy(&e, &eb, 4);
                    std::snprintf(m, sizeof m, "player %zu %s: original %.9g, engine %.9g", slot, names[k], static_cast<double>(a),
                                  static_cast<double>(e));
                    diff.push_back(m);
                }
        }
    // Units.
    std::map<int, UnitRow> theirs;
    if (const Json* us = d.get("units"))
        for (const Json& u : us->arr) {
            if (u.arr.size() < kUnitScalars + 2 || u.arr[1].num == 0) continue;  // (older captures listed empty entries)
            UnitRow r;
            for (int k = 0; k < kUnitScalars; ++k) r.v.push_back(u.arr[static_cast<std::size_t>(k)].num);
            for (const Json& o : u.arr[kUnitScalars].arr) r.primary.push_back(o.num);
            for (const Json& o : u.arr[kUnitScalars + 1].arr) r.secondary.push_back(o.num);
            if (u.arr.size() > kUnitScalars + 2) {
                r.has_motion_row = true;
                for (const Json& o : u.arr[kUnitScalars + 2].arr) r.motion.push_back(o.num);
            }
            if (u.arr.size() > kUnitScalars + 3) {
                r.has_vel_tilt = true;
                for (const Json& o : u.arr[kUnitScalars + 3].arr) r.vel_tilt.push_back(o.num);
            }
            theirs[static_cast<int>(r.v[0])] = std::move(r);
        }
    const std::map<int, UnitRow> ours = engine_units(b);
    for (const auto& [id, r] : theirs) {
        auto it = ours.find(id);
        if (it == ours.end()) {
            std::snprintf(m, sizeof m, "unit %d (type %lld %s, player %lld): exists in the original only, at %lld,%lld,%lld", id, r.v[1], tname(r.v[1]), r.v[2],
                          r.v[3], r.v[4], r.v[5]);
            diff.push_back(m);
            continue;
        }
        const UnitRow& e = it->second;
        for (int k = 1; k < kUnitScalars; ++k) {
            long long a = r.v[static_cast<std::size_t>(k)], c = e.v[static_cast<std::size_t>(k)];
            if (k == 13) {
                a &= ~kInterfaceFlags;
                c &= ~kInterfaceFlags;
            }
            if (a != c) {
                std::snprintf(m, sizeof m, "unit %d (type %lld %s, player %lld) %s: original %lld (0x%llx), engine %lld (0x%llx)", id, r.v[1], tname(r.v[1]), r.v[2],
                              kUnitFields[k], a, static_cast<unsigned long long>(a), c, static_cast<unsigned long long>(c));
                diff.push_back(m);
            }
        }
        if (r.primary != e.primary) diff.push_back("unit " + std::to_string(id) + " primary orders: original " + list(r.primary) + ", engine " + list(e.primary));
        if (r.has_motion_row) {
            const std::vector<long long>& a = r.motion;
            const std::vector<long long>& c = e.motion;
            const bool head_differs = a.size() >= 3 && c.size() >= 3 ? !std::equal(a.begin(), a.begin() + 3, c.begin()) : (a.size() >= 3) != (c.size() >= 3);
            // Route goals: count, search tick, flags, points (the original's goal vtable is element 3).
            bool route_differs = false;
            // (Route flag bit 2 comes from uninitialised heap memory in the original, the goal's
            // constructor keeps it, and nothing reads it: not compared.)
            if (c.size() > 6 && c[3] == -1 && a.size() > 6) {
                std::vector<long long> a2(a.begin() + 4, a.end()), c2(c.begin() + 4, c.end());
                a2[2] &= 0xB;
                c2[2] &= 0xB;
                route_differs = a2 != c2;
            }
            if (head_differs || route_differs)
                diff.push_back("unit " + std::to_string(id) + " motion [speed, turn, flags, goal, route count, search tick, route flags, points]: original " +
                               list(a) + ", engine " + list(c));
        }
        if (r.has_vel_tilt && r.vel_tilt != e.vel_tilt)
            diff.push_back("unit " + std::to_string(id) + " motion [velocity xyz, tilt xyz]: original " + list(r.vel_tilt) + ", engine " +
                           list(e.vel_tilt));
        if (r.secondary != e.secondary)
            diff.push_back("unit " + std::to_string(id) + " secondary orders: original " + list(r.secondary) + ", engine " + list(e.secondary));
    }
    for (const auto& [id, e] : ours)
        if (!theirs.count(id)) {
            std::snprintf(m, sizeof m, "unit %d (type %lld %s, player %lld): exists in the engine only, at %lld,%lld,%lld", id, e.v[1], tname(e.v[1]), e.v[2], e.v[3],
                          e.v[4], e.v[5]);
            diff.push_back(m);
        }
    // Projectiles by slot.
    std::map<int, std::vector<long long>> tp, op;
    if (const Json* ps = d.get("proj"))
        for (const Json& p : ps->arr) {
            std::vector<long long> v;
            for (const Json& x : p.arr) v.push_back(x.num);
            if (!v.empty()) tp[static_cast<int>(v[0])] = v;
        }
    for (int s = 0; s < sim::kMaxProjectiles; ++s) {
        const sim::Projectile& p = b.projectiles.slots[static_cast<std::size_t>(s)];
        if (!p.weapon) continue;
        op[s] = {s, p.weapon->index, p.pos[0], p.pos[1], p.pos[2], p.vel[0], p.vel[1], p.vel[2], p.owner ? p.owner->id : 0,
                 p.target ? p.target->id : 0};
    }
    for (const auto& [s, v] : tp) {
        auto it = op.find(s);
        if (it == op.end()) diff.push_back("projectile slot " + std::to_string(s) + ": original only " + list(v));
        else if (it->second != v) diff.push_back("projectile slot " + std::to_string(s) + ": original " + list(v) + ", engine " + list(it->second));
    }
    for (const auto& [s, v] : op)
        if (!tp.count(s)) diff.push_back("projectile slot " + std::to_string(s) + ": engine only " + list(v));
    // Map cells: features (type, aux, flags) and occupancy.
    if (const Json* c = d.get("cells"); c && c->arr.size() == 2) {
        std::uint32_t fh = 2166136261u, uh = 2166136261u;
        for (const sim::MapCell& cell : b.map.cells) {
            std::uint8_t u[4], f[5];
            std::memcpy(u, &cell.unit, 2);
            std::memcpy(u + 2, &cell.unit2, 2);
            std::memcpy(f, &cell.feature, 2);
            std::memcpy(f + 2, &cell.feature_aux, 2);
            f[4] = cell.flags;
            uh = fnv(uh, u, 4);
            fh = fnv(fh, f, 5);
        }
        if (static_cast<std::uint32_t>(c->arr[0].num) != fh) diff.push_back("map cells: features differ (hash)");
        if (static_cast<std::uint32_t>(c->arr[1].num) != uh) diff.push_back("map cells: unit occupancy differs (hash)");
    }
    return diff;
}

std::string side_of_commander(ta::Vfs& vfs, const std::string& commander) {
    std::vector<std::uint8_t> bytes;
    if (!vfs.read("gamedata\\sidedata.tdf", bytes)) return {};
    const auto r = ta::tdf_parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!r.root) return {};
    for (const auto& s : r.root->children())
        if (ta::stricmp_c(s->get_string("commander", 32, ""), commander) == 0) return s->get_string("name", 32, "");
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: game_replay <game_dir> <capture.jsonl> [--steps N] [--continue] [--no-draw-effects] [--verbose]\n");
        return 2;
    }
    std::uint32_t max_steps = 0xFFFFFFFFu;
    bool keep_going = false, draw_effects = true, verbose = false, ignore_cells = false;
    int trace_unit = 0;  // print this unit's row (original and engine) after every step
    bool shuffle_from_capture = false;  // take the shuffle's rand() state from the capture
    for (int k = 3; k < argc; ++k) {
        const std::string a = argv[k];
        if (a == "--steps" && k + 1 < argc) max_steps = static_cast<std::uint32_t>(std::strtoul(argv[++k], nullptr, 0));
        else if (a == "--continue") keep_going = true;
        else if (a == "--no-draw-effects") draw_effects = false;
        else if (a == "--verbose") verbose = true;
        else if (a == "--ignore-cells") ignore_cells = true;
        else if (a == "--shuffle-from-capture") shuffle_from_capture = true;
        else if (a == "--trace-unit" && k + 1 < argc) trace_unit = std::atoi(argv[++k]);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Read the capture.
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) {
        std::printf("cannot open %s\n", argv[2]);
        return 1;
    }
    std::vector<Json> events;
    {
        std::string line;
        std::vector<char> buf(1 << 20);
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), f)) {
            line += buf.data();
            if (line.empty() || line.back() != '\n') continue;
            if (line.find("\"ev\":\"archive_open\"") == std::string::npos && line.find("\"ev\":\"rand\"") == std::string::npos) {
                Json j;
                JsonParser p{line.c_str()};
                if (p.parse(j)) events.push_back(std::move(j));
            }
            line.clear();
        }
    }
    std::fclose(f);

    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    ReplayOutside out;
    const Json* start = nullptr;
    std::string map;
    std::uint32_t seed = 0, shuffle = 0, sim_rand = 0;
    bool have_seed = false, have_shuffle = false, have_loading_srand = false;
    long long loading_tid = -1;
    std::uint32_t loading_srand = 0;
    for (const Json& e : events) {
        const std::string ev = e.s("ev");
        if (ev == "battle_seed" && !have_seed) {
            seed = static_cast<std::uint32_t>(e.get("override") ? e.i("override") : e.i("seed"));
            have_seed = true;
            loading_tid = e.i("tid");
        } else if (ev == "srand" && have_seed && !have_loading_srand && e.i("tid") == loading_tid) {
            loading_srand = static_cast<std::uint32_t>(e.i("seed"));  // srand(time()) of the loading thread
            have_loading_srand = true;
        } else if (ev == "shuffle_rand" && !have_shuffle) {
            shuffle = static_cast<std::uint32_t>(e.i("state"));
            have_shuffle = true;
        } else if (ev == "map") {
            map = e.s("path");
        } else if (ev == "start" && !start) {
            start = &e;
            sim_rand = static_cast<std::uint32_t>(e.i("sim_rand"));
        } else if (ev == "stale") {
            const auto t = static_cast<std::uint32_t>(e.i("tick"));
            const std::string what = e.s("what");
            if (what == "ai_site_y") out.site_y[t].push_back(static_cast<std::int32_t>(e.i("value")));
            else if (what == "reclaim_site_y") out.reclaim_y[t].push_back(static_cast<std::int32_t>(e.i("value")));
            else if (what == "clock") out.clock[t].push_back(static_cast<std::uint32_t>(e.i("value")));
            else if (what == "air_repair_site" && !out.air_repair_site.count(t))
                out.air_repair_site[t] = static_cast<std::uint32_t>(e.i("value"));
        }
    }
    if (!start || !have_seed || map.empty()) {
        std::printf("capture incomplete: start %d, battle seed %d, map '%s'\n", start != nullptr, have_seed, map.c_str());
        return 1;
    }
    // "Maps\Name.TNT" -> "Name".
    if (const auto slash = map.find_last_of("\\/"); slash != std::string::npos) map = map.substr(slash + 1);
    if (map.size() > 4 && ta::stricmp_c(map.substr(map.size() - 4), ".tnt") == 0) map.resize(map.size() - 4);

    ta::BattleRequest req;
    req.map = map;
    sim::BattleStart& bs = req.start;
    const auto map_flags = start->i("map_flags");
    bs.explore = (map_flags & 1) != 0;
    bs.fog_of_war = (map_flags & 2) != 0;
    bs.true_sight = (map_flags & 4) != 0;
    bs.unit_limit = static_cast<std::uint16_t>(start->i("unit_limit", 250));
    bs.ai_difficulty = static_cast<std::int32_t>(start->i("ai_difficulty", 2));
    bs.commander_mode = static_cast<std::int32_t>(start->i("game_mode"));
    bs.local_player = static_cast<std::uint8_t>(start->i("local_player"));
    bs.provider = 2;
    bs.battle_rng_seed = seed;
    bs.fixed_locations = !have_shuffle;
    bs.loading_rand_seed = shuffle;
    // From the loading thread's srand(time()) instead (the map's vents and the explosion tables
    // drawn by the loader): the shuffle's state is then derived, not taken from the capture.
    if (have_loading_srand && !shuffle_from_capture) {
        req.loading_srand_given = true;
        req.loading_srand = loading_srand;
    }
    bs.crt_rand_state = sim_rand;
    if (const Json* ps = start->get("players"))
        for (const Json& p : ps->arr) {
            const auto slot = static_cast<std::size_t>(p.i("slot"));
            const auto controller = p.i("controller");
            // Records without a controller (the original marks unused skirmish slots active with
            // controller 0) take no part.
            if (slot >= 10 || !p.i("active") || controller < 1 || controller > 3) continue;
            sim::StartPlayer& sp = bs.players[slot];
            sp.kind = controller == 1 ? sim::PlayerKind::Local : controller == 2 ? sim::PlayerKind::Computer : sim::PlayerKind::Remote;
            sp.net_id = static_cast<std::int32_t>(p.i("net_id"));
            float e, mt;
            const auto eb = static_cast<std::uint32_t>(p.i("energy")), mb = static_cast<std::uint32_t>(p.i("metal"));
            std::memcpy(&e, &eb, 4);
            std::memcpy(&mt, &mb, 4);
            sp.energy = static_cast<std::int32_t>(e);
            sp.metal = static_cast<std::int32_t>(mt);
            req.sides[slot] = side_of_commander(vfs, p.s("commander"));
            // Alliances: players allied with each other share a group (the lowest such slot).
            if (const Json* al = p.get("allied"))
                for (std::size_t o = 0; o < al->arr.size() && o < 10; ++o)
                    if (o != slot && al->arr[o].num) sp.ally_group = static_cast<std::uint8_t>(std::min(slot, o));
        }
    const auto gs = static_cast<std::uint32_t>(start->i("game_state")), table = static_cast<std::uint32_t>(start->i("unit_table"));
    std::printf("capture: %s, %zu events; map %s, battle seed %08x, shuffle rand %08x, simulation rand %08x, game state %08x, unit table %08x\n",
                argv[2], events.size(), map.c_str(), seed, shuffle, sim_rand, gs, table);
    if (have_loading_srand && have_shuffle)
        std::printf("start: the loading thread drew rand() %lld times between srand(time()) and the shuffle (map load and explosion tables)\n",
                    lcg_distance(loading_srand, shuffle, 4000000));

    std::string err;
    auto lb = ta::load_battle(vfs, req, out, err, [&](sim::Battle& b) {
        out.battle = &b;
        b.settings.addresses.game_state = gs;
        b.settings.addresses.unit_table = table;
    });
    if (!lb) {
        std::printf("load: %s\n", err.c_str());
        return 1;
    }
    sim::Battle& b = *lb->battle;
    g_type_names.assign(lb->type_defs.size(), std::string());
    for (std::size_t k = 1; k < lb->type_defs.size(); ++k)
        if (lb->type_defs[k]) g_type_names[k] = lb->type_defs[k]->unit_name;
    b.settings.local_view = static_cast<std::uint8_t>(start->i("local_view"));
    b.settings.draw_side_effects = draw_effects;
    if (verbose)
        for (int s = 0; s < 10; ++s) {
            const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(s)];
            if (!p.active) continue;
            std::printf("player %d: controller %u, side %s, units %d..%d", s, p.controller, req.sides[static_cast<std::size_t>(s)].c_str(),
                        p.first_unit, p.last_unit);
            for (int id = p.first_unit; id <= p.last_unit && id > 0; ++id)
                if (b.ticks[static_cast<std::size_t>(id)].type_index) {
                    const sim::Unit& u = b.units[static_cast<std::size_t>(id)];
                    std::printf(", unit %d type %u at %d,%d", id, b.ticks[static_cast<std::size_t>(id)].type_index, u.pos[0] >> 16, u.pos[2] >> 16);
                }
            std::printf("\n");
        }

    // The map cells at the start (when the capture has them).
    for (const Json& e : events) {
        if (e.s("ev") != "cells") continue;
        std::map<long long, std::vector<long long>> want;
        if (const Json* cs = e.get("cells"))
            for (const Json& c : cs->arr) {
                std::vector<long long> v;
                for (const Json& x : c.arr) v.push_back(x.num);
                if (v.size() == 7) want[v[0]] = v;
            }
        int shown = 0, differing = 0;
        for (std::size_t i = 0; i < b.map.cells.size(); ++i) {
            const sim::MapCell& c = b.map.cells[i];
            std::vector<long long> ours = {static_cast<long long>(i), c.unit, c.unit2, c.feature, c.feature_aux, c.flags, c.metal};
            const bool empty = !c.unit && !c.unit2 && c.feature == 0xFFFF && !c.feature_aux && !c.flags && !c.metal;
            auto it = want.find(static_cast<long long>(i));
            const std::vector<long long> theirs = it != want.end() ? it->second : std::vector<long long>{static_cast<long long>(i), 0, 0, 0xFFFF, 0, 0, 0};
            if (empty && it == want.end()) continue;
            if (ours != theirs) {
                ++differing;
                if (shown++ < 20)
                    std::printf("start: cell %zu (x %zu, z %zu) [unit, unit2, feature, aux, flags, metal]: original %s, engine %s\n", i,
                                i % static_cast<std::size_t>(b.map.width), i / static_cast<std::size_t>(b.map.width), list(theirs).c_str(),
                                list(ours).c_str());
            }
        }
        std::printf("start: %d map cells differ (%zu listed by the original)\n", differing, want.size());
        break;
    }

    // The route search's scheduling state at the start.
    for (const Json& e : events) {
        if (e.s("ev") != "route_state") continue;
        const sim::RouteSearch& rs = b.route_search;
        auto arr = [&](const char* k) {
            std::vector<long long> v;
            if (const Json* a = e.get(k))
                for (const Json& x : a->arr) v.push_back(x.num);
            return v;
        };
        std::vector<long long> cur, cred, sc, tu;
        for (int p = 0; p < 10; ++p) {
            cur.push_back(rs.unit_cursor[static_cast<std::size_t>(p)]);
            cred.push_back(rs.credit[static_cast<std::size_t>(p)]);
            sc.push_back(rs.estimate_scale[static_cast<std::size_t>(p)]);
            tu.push_back(rs.load[static_cast<std::size_t>(p)]);
        }
        const bool same = e.i("player_count") == b.settings.player_count && e.i("units_per_player") == b.settings.units_per_player &&
                          e.i("refresh") == rs.refresh_counter && e.i("player") == rs.player &&
                          e.i("active") == (rs.active ? rs.active->id : 0) && arr("cursor") == cur && arr("credit") == cred &&
                          arr("scale") == sc && arr("turns") == tu;
        std::printf("start: route search state %s%s%s\n", same ? "identical" : "DIFFERENT",
                    b.route_context().fault.empty() ? "" : "; route search fault: ", b.route_context().fault.c_str());
        if (!same) {
            std::printf("  original: players %lld, units per player %lld, refresh %lld, player %lld, serving %lld, cursors %s, credits %s, scales %s, turns %s\n",
                        e.i("player_count"), e.i("units_per_player"), e.i("refresh"), e.i("player"), e.i("active"), list(arr("cursor")).c_str(),
                        list(arr("credit")).c_str(), list(arr("scale")).c_str(), list(arr("turns")).c_str());
            std::printf("  engine:   players %u, units per player %u, refresh %d, player %u, serving %d, cursors %s, credits %s, scales %s, turns %s\n",
                        b.settings.player_count, b.settings.units_per_player, rs.refresh_counter, rs.player, rs.active ? rs.active->id : 0,
                        list(cur).c_str(), list(cred).c_str(), list(sc).c_str(), list(tu).c_str());
        }
        break;
    }

    // Replay.
    std::uint32_t steps = 0, frames = 0, ui_orders = 0, compared = 0, first_div = 0;
    long long between_frame_draws = 0;
    int unknown_gaps = 0;
    bool frame_start = true;  // the next step is the first of a frame
    std::map<int, int> frame_sizes;
    int in_frame = 0;
    for (const Json& e : events) {
        if (b.game_tick() >= max_steps) break;
        const std::string ev = e.s("ev");
        if (ev == "step") {
            const auto t = static_cast<std::uint32_t>(e.i("tick"));
            if (t <= 1 && b.game_tick() == 0 && t == 0) continue;
            if (b.game_tick() + 1 != t) {
                std::printf("step order broken: engine at %u, capture step %u\n", b.game_tick(), t);
                return 1;
            }
            const Json* rs = e.get("rand_state");
            if (rs && rs->type == Json::Num) {
                const auto want = static_cast<std::uint32_t>(rs->num);
                if (b.crt_rand.state() != want) {
                    const long long n = lcg_distance(b.crt_rand.state(), want);
                    if (!frame_start && !first_div)
                        std::printf("step %u: rand() changed between steps of one frame (%lld draws)\n", t, n);
                    if (n < 0) ++unknown_gaps;
                    else between_frame_draws += n;
                    if (verbose) std::printf("step %u: %lld rand() draws between frames taken from the capture\n", t, n);
                    b.crt_rand.set_state(want);
                }
            }
            const auto want_rng = static_cast<std::uint32_t>(e.i("battle_rng"));
            if (b.battle_rng().state() != want_rng && !first_div) {
                std::printf("step %u: battle generator differs before the step: original %08x, engine %08x\n", t, want_rng, b.battle_rng().state());
            }
            frame_start = false;
            if (auto it = out.air_repair_site.find(t); it != out.air_repair_site.end())
                b.settings.air_repair_patrol_site_address = it->second;
            b.step();
            ++steps;
            ++in_frame;
        } else if (ev == "digest") {
            if (static_cast<std::uint32_t>(e.i("tick")) != b.game_tick()) continue;
            ++compared;
            if (trace_unit > 0 && static_cast<std::size_t>(trace_unit) < b.units.size()) {
                const std::map<int, UnitRow> ours = engine_units(b);
                std::string theirs = "-";
                if (const Json* us = e.get("units"))
                    for (const Json& u : us->arr)
                        if (u.arr.size() > kUnitScalars && u.arr[0].num == trace_unit) {
                            std::vector<long long> v;
                            for (std::size_t k = 0; k < kUnitScalars; ++k) v.push_back(u.arr[k].num);
                            theirs = list(v);
                        }
                auto it = ours.find(trace_unit);
                const sim::UnitTickState& t = b.ticks[static_cast<std::size_t>(trace_unit)];
                std::printf("trace %u (%s): original %s\n          engine   %s  velocity %d,%d,%d\n", b.game_tick(), tname(t.type_index), theirs.c_str(),
                            it != ours.end() ? list(it->second.v).c_str() : "-", t.velocity[0], t.velocity[1], t.velocity[2]);
            }
            std::vector<std::string> diff = compare(b, e);
            if (ignore_cells)
                diff.erase(std::remove_if(diff.begin(), diff.end(), [](const std::string& d) { return d.rfind("map cells", 0) == 0; }), diff.end());
            if (!diff.empty() && !first_div) {
                first_div = b.game_tick();
                std::printf("FIRST DIVERGENCE after step %u (%zu differences):\n", first_div, diff.size());
                for (std::size_t k = 0; k < diff.size() && k < 60; ++k) std::printf("  %s\n", diff[k].c_str());
                for (const std::string& n : out.notes) std::printf("  note: %s\n", n.c_str());
                if (!keep_going) break;
            } else if (!diff.empty() && verbose) {
                std::printf("step %u: %zu differences (first: %s)\n", b.game_tick(), diff.size(), diff[0].c_str());
            }
        } else if (ev == "frame") {
            if (static_cast<std::uint32_t>(e.i("tick")) != b.game_tick()) continue;
            const auto view = static_cast<std::uint8_t>(e.i("view"));
            b.end_frame();
            b.settings.local_view = view;
            // Line-of-sight options changed (console toggles; the original then rebuilds sight).
            if (const Json* mf = e.get("map_flags"); mf && mf->type == Json::Num) {
                const bool explore = (mf->num & 1) != 0, fog = (mf->num & 2) != 0, true_sight = (mf->num & 4) != 0;
                if (explore != b.settings.explore || fog != b.settings.fog_of_war || true_sight != b.settings.true_sight) {
                    std::printf("step %u: line-of-sight options changed by the player (map flags %lld): sight rebuilt\n", b.game_tick(), mf->num);
                    b.settings.explore = explore;
                    b.settings.fog_of_war = fog;
                    b.settings.true_sight = true_sight;
                    b.rebuild_sight(false);
                }
            }
            ++frames;
            ++frame_sizes[in_frame];
            in_frame = 0;
            frame_start = true;
        } else if (ev == "order" && e.i("in_step") == 0 && b.game_tick() > 0) {
            const std::string fn = e.s("fn");
            const int id = static_cast<int>(e.i("unit"));
            if (id <= 0 || static_cast<std::size_t>(id) >= b.units.size()) continue;
            sim::Unit& u = b.units[static_cast<std::size_t>(id)];
            if (fn == "issue_or_cancel") {
                const int tid = static_cast<int>(e.i("target"));
                sim::Unit* target = tid > 0 && static_cast<std::size_t>(tid) < b.units.size() ? &b.units[static_cast<std::size_t>(tid)] : nullptr;
                std::array<std::int32_t, 3> pos{};
                const Json* pj = e.get("pos");
                const bool has_pos = pj && pj->type == Json::Arr && pj->arr.size() == 3;
                if (has_pos)
                    for (int k = 0; k < 3; ++k) pos[static_cast<std::size_t>(k)] = static_cast<std::int32_t>(pj->arr[static_cast<std::size_t>(k)].num);
                sim::command::issue_or_cancel(b, static_cast<std::uint8_t>(e.i("kind")), e.i("queued") != 0, u, target, has_pos ? &pos : nullptr,
                                              static_cast<std::int32_t>(e.i("p0")), static_cast<std::int32_t>(e.i("p1")));
                ++ui_orders;
            } else if (fn == "build_count") {
                b.order_build_count(u, static_cast<std::uint16_t>(e.i("type")), static_cast<std::int32_t>(e.i("delta")));
                ++ui_orders;
            }
        }
    }
    std::printf("replayed %u steps in %u frames (steps per frame:", steps, frames);
    for (const auto& [n, c] : frame_sizes) std::printf(" %d x%d", n, c);
    std::printf("), %u interface orders, %lld rand() draws between frames taken from the capture%s, %u digests compared\n", ui_orders,
                between_frame_draws, unknown_gaps ? " (some gaps not found)" : "", compared);
    if (first_div) std::printf("result: DIVERGED after step %u\n", first_div);
    else std::printf("result: identical for all %u compared steps\n", compared);
    return first_div ? 1 : 0;
}
