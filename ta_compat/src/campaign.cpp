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
    read_mission_goals(*gh, st, out.goals);
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
                std::int32_t id = what.size() > 8 && what[8] >= '0' && what[8] <= '9' ? msvc_atoi(what.substr(8)) : ++running;
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

BattleRequest make_mission_request(Vfs& vfs, const MissionInfo& mission, const MissionRequest& rq) {
    BattleRequest req;
    // The CD layer (archives mounted on `vfs` beyond the game directory's own).
    for (std::size_t k = 0; k < vfs.archives().size(); ++k) req.extra_archives.push_back(vfs.archives()[k]->path());
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
    // The mission's unit list (camps\useonly), read now.
    std::vector<std::string> use_only;
    if (!mission.use_only.empty())
        if (const std::unique_ptr<TdfSection> list = parse_file(vfs, "camps\\useonly\\" + strip_ext(mission.use_only) + ".tdf"))
            for (const auto& sec : list->children()) use_only.push_back(sec->name());
    const bool limit_types = !mission.use_only.empty() && (!use_only.empty() || vfs.size_or_zero("camps\\useonly\\" + strip_ext(mission.use_only) + ".tdf"));
    req.prepare_start = [mission, use_only, limit_types](LoadedBattle& lb, sim::BattleStart& start) {
        start.mission.units = mission.units;
        for (sim::MissionUnit& u : start.mission.units) u.type = lb.type_index(u.unit_name);
        // The order parser keeps its scratch between units, as the original's stack does.
        auto state = std::make_shared<MissionParseState>();
        LoadedBattle* plb = &lb;
        start.mission.parse_orders = [plb, state](const std::string& text) {
            return parse_initial_orders(text, [plb](const std::string& n) { return plb->type_index(n); }, *state);
        };
        start.mission.restrict_types = limit_types;
        for (const std::string& n : use_only)
            if (const std::uint16_t t = lb.type_index(n)) start.mission.allowed_types.push_back(t);
    };
    return req;
}

std::unique_ptr<LoadedBattle> load_mission_battle(Vfs& vfs, const MissionInfo& mission, const MissionRequest& rq, sim::BattleOutside& outside,
                                                  std::string& error) {
    return load_battle(vfs, make_mission_request(vfs, mission, rq), outside, error);
}

}  // namespace ta
