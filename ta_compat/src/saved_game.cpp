// Saved games: the bank container, the engine's sections, load by replay
// (docs/RE_MEMORY.md, "Saved games").

#include "ta/saved_game.h"

#include <cstring>

#include "sim/lockstep.h"
#include "ta/tdf.h"

namespace ta {

namespace {

std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
           static_cast<std::uint32_t>(b[at + 3]) << 24;
}
void wr32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
void set32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
}
std::string cstr(const std::vector<std::uint8_t>& d, std::uint32_t at) {
    std::string s;
    for (std::size_t i = at; i < d.size() && d[i]; ++i) s.push_back(static_cast<char>(d[i]));
    return s;
}
bool unsquash(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> chunk(p, p + n);
    out.clear();
    return unpack_sqsh(chunk, out) == HpiError::None;
}

template <class V> typename V::value_type* find_key(V& v, const std::string& k) {
    for (auto& e : v)
        if (stricmp_c(e.first, k) == 0) return &e;
    return nullptr;
}

constexpr const char* kBankName = "Total Annihilation 3.0";

}  // namespace

// --- Bank sections.

void BankSection::set_int(const std::string& k, std::int32_t v) {
    if (auto* e = find_key(ints, k)) e->second = v;
    else ints.emplace_back(k, v);
}
void BankSection::set_string(const std::string& k, const std::string& v) {
    if (auto* e = find_key(strings, k)) e->second = v;
    else strings.emplace_back(k, v);
}
void BankSection::set_double(const std::string& k, double v) {
    if (auto* e = find_key(doubles, k)) e->second = v;
    else doubles.emplace_back(k, v);
}
bool BankSection::has(const std::string& k) const {
    auto self = const_cast<BankSection*>(this);
    return find_key(self->ints, k) || find_key(self->strings, k) || find_key(self->doubles, k);
}
std::int32_t BankSection::get_int(const std::string& k, std::int32_t def) const {
    for (const auto& e : ints)
        if (stricmp_c(e.first, k) == 0) return e.second;
    return def;
}
std::string BankSection::get_string(const std::string& k, const std::string& def) const {
    for (const auto& e : strings)
        if (stricmp_c(e.first, k) == 0) return e.second;
    return def;
}
BankSection::Blob& BankSection::blob(const std::string& n) {
    for (Blob& b : blobs)
        if (b.named && stricmp_c(b.name, n) == 0) return b;
    blobs.push_back(Blob{true, n, 0, {}});
    return blobs.back();
}
const BankSection::Blob* BankSection::find_blob(const std::string& n) const {
    for (const Blob& b : blobs)
        if (b.named && stricmp_c(b.name, n) == 0) return &b;
    return nullptr;
}
BankSection& Bank::section(const std::string& n) {
    for (BankSection& s : sections)
        if (stricmp_c(s.name, n) == 0) return s;
    sections.push_back(BankSection{});
    sections.back().name = n;
    return sections.back();
}
const BankSection* Bank::find(const std::string& n) const {
    for (const BankSection& s : sections)
        if (stricmp_c(s.name, n) == 0) return &s;
    return nullptr;
}

bool read_bank(const std::vector<std::uint8_t>& bytes, Bank& out, std::string& error, const char* only) {
    out = Bank{};
    if (bytes.size() < 0x22 || std::memcmp(bytes.data(), "HAPIBANK", 8) != 0) {
        error = "not a bank file";
        return false;
    }
    if (rd32(bytes, 0x14) != 1) {
        error = "unknown bank version";
        return false;
    }
    const std::uint32_t name_off = rd32(bytes, 0x08), dir_off = rd32(bytes, 0x0C), first = rd32(bytes, 0x10);
    if (dir_off > bytes.size() || first > dir_off) {
        error = "bad bank header";
        return false;
    }
    std::vector<std::uint8_t> dir(bytes.begin() + dir_off, bytes.end());
    if (bytes[0x18]) {
        std::vector<std::uint8_t> d;
        if (!unsquash(dir.data(), dir.size(), d)) {
            error = "bank directory does not decompress";
            return false;
        }
        dir = std::move(d);
    }
    out.name = cstr(dir, name_off);
    std::size_t pos = first;
    while (pos + 0x20 <= dir_off) {
        const std::uint32_t size = rd32(bytes, pos);
        if (size < 0x20 || pos + size > dir_off) {
            error = "bad account";
            return false;
        }
        const std::string name = cstr(dir, rd32(bytes, pos + 4));
        const std::int32_t ni = static_cast<std::int32_t>(rd32(bytes, pos + 8)), nd = static_cast<std::int32_t>(rd32(bytes, pos + 12)),
                           ns = static_cast<std::int32_t>(rd32(bytes, pos + 16)), nb = static_cast<std::int32_t>(rd32(bytes, pos + 20));
        const std::uint32_t packed = rd32(bytes, pos + 24);
        if (only && stricmp_c(name, only) != 0) {
            pos += size;
            continue;
        }
        std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(pos + 0x20), bytes.begin() + static_cast<std::ptrdiff_t>(pos + size));
        if (packed == 1) {
            std::vector<std::uint8_t> d;
            if (!unsquash(payload.data(), payload.size(), d)) {
                error = "account " + name + " does not decompress";
                return false;
            }
            payload = std::move(d);
        }
        BankSection& s = out.section(name);
        std::size_t at = 0;
        for (std::int32_t k = 0; k < ni; ++k, at += 8) s.set_int(cstr(dir, rd32(payload, at)), static_cast<std::int32_t>(rd32(payload, at + 4)));
        for (std::int32_t k = 0; k < nd; ++k, at += 12) {
            double v = 0;
            if (at + 12 <= payload.size()) std::memcpy(&v, payload.data() + at + 4, 8);
            s.set_double(cstr(dir, rd32(payload, at)), v);
        }
        for (std::int32_t k = 0; k < ns; ++k, at += 8) s.set_string(cstr(dir, rd32(payload, at)), cstr(dir, rd32(payload, at + 4)));
        for (std::int32_t k = 0; k < nb; ++k, at += 16) {
            const std::int32_t noff = static_cast<std::int32_t>(rd32(payload, at));
            const std::uint32_t data_at = rd32(payload, at + 8), n = rd32(payload, at + 12);
            BankSection::Blob* b = nullptr;
            if (noff < 0) {
                const std::int32_t idx = static_cast<std::int32_t>(rd32(payload, at + 4));
                for (BankSection::Blob& x : s.blobs)
                    if (!x.named && x.index == idx) b = &x;
                if (!b) {
                    s.blobs.push_back(BankSection::Blob{false, {}, idx, {}});
                    b = &s.blobs.back();
                }
            } else {
                b = &s.blob(cstr(dir, static_cast<std::uint32_t>(noff)));
            }
            // Blob bytes: at their file offset relative to the account's payload; repeated
            // entries of one blob are appended.
            const std::size_t rel = static_cast<std::size_t>(data_at) - pos - 0x20;
            if (rel + n <= payload.size()) b->data.insert(b->data.end(), payload.begin() + static_cast<std::ptrdiff_t>(rel),
                                                          payload.begin() + static_cast<std::ptrdiff_t>(rel + n));
        }
        pos += size;
    }
    return true;
}

void write_bank(const Bank& bank, std::vector<std::uint8_t>& out) {
    out.assign(0x22, 0);
    std::memcpy(out.data(), "HAPIBANK", 8);
    std::vector<std::uint8_t> dir;
    auto add = [&](const std::string& s) {
        const std::uint32_t at = static_cast<std::uint32_t>(dir.size());
        dir.insert(dir.end(), s.begin(), s.end());
        dir.push_back(0);
        return at;
    };
    const std::uint32_t name_off = add(bank.name);
    for (const BankSection& s : bank.sections) {
        if (s.ints.empty() && s.doubles.empty() && s.strings.empty() && s.blobs.empty()) continue;
        const std::size_t start = out.size();
        out.resize(start + 0x20, 0);
        const std::uint32_t sec_name = add(s.name);
        for (const auto& [k, v] : s.ints) {
            wr32(out, add(k));
            wr32(out, static_cast<std::uint32_t>(v));
        }
        for (const auto& [k, v] : s.doubles) {
            wr32(out, add(k));
            std::uint8_t b[8];
            std::memcpy(b, &v, 8);
            out.insert(out.end(), b, b + 8);
        }
        for (const auto& [k, v] : s.strings) {
            wr32(out, add(k));
            wr32(out, add(v));
        }
        std::uint32_t nb = 0;
        for (const auto& b : s.blobs) nb += !b.data.empty();
        std::size_t data_at = out.size() + nb * 16u;
        std::int32_t last_index = 0;
        for (const auto& b : s.blobs) {
            if (b.data.empty()) continue;
            if (b.named) {
                wr32(out, add(b.name));
                wr32(out, static_cast<std::uint32_t>(last_index));  // the original leaves a stale word here
            } else {
                wr32(out, 0xFFFFFFFFu);
                wr32(out, static_cast<std::uint32_t>(b.index));
                last_index = b.index;
            }
            wr32(out, static_cast<std::uint32_t>(data_at));
            wr32(out, static_cast<std::uint32_t>(b.data.size()));
            data_at += b.data.size();
        }
        for (const auto& b : s.blobs) out.insert(out.end(), b.data.begin(), b.data.end());
        set32(out, start, static_cast<std::uint32_t>(out.size() - start));
        set32(out, start + 4, sec_name);
        set32(out, start + 8, static_cast<std::uint32_t>(s.ints.size()));
        set32(out, start + 12, static_cast<std::uint32_t>(s.doubles.size()));
        set32(out, start + 16, static_cast<std::uint32_t>(s.strings.size()));
        set32(out, start + 20, nb);
    }
    const std::uint32_t dir_off = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), dir.begin(), dir.end());
    set32(out, 0x08, name_off);
    set32(out, 0x0C, dir_off);
    set32(out, 0x10, 0x22);
    set32(out, 0x14, 1);
}

// --- Game starts.

std::unique_ptr<LoadedBattle> start_game(Vfs& vfs, const GameStart& g, sim::BattleOutside& outside, std::string& error) {
    if (g.kind == GameStart::Kind::Mission) {
        MissionInfo info;
        if (!load_mission(vfs, g.mission_file, g.mission.difficulty, info, error)) return nullptr;
        return load_mission_battle(vfs, info, g.mission, outside, error);
    }
    return load_battle(vfs, g.battle, outside, error);
}

namespace {

std::int32_t bits(float f) {
    std::int32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
float from_bits(std::int32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void put_start(BankSection& s, const GameStart& g) {
    s.set_int("Kind", static_cast<std::int32_t>(g.kind));
    if (g.kind == GameStart::Kind::Mission) {
        s.set_string("Campaign", g.campaign);
        s.set_int("MissionIndex", g.mission_index);
        s.set_string("MissionFile", g.mission_file);
        s.set_string("CampaignSide", g.mission.campaign_side);
        s.set_string("OtherSide", g.mission.other_side);
        s.set_int("Difficulty", g.mission.difficulty);
        s.set_int("BattleRngSeed", static_cast<std::int32_t>(g.mission.battle_rng_seed));
        s.set_int("LoadingRandSeed", static_cast<std::int32_t>(g.mission.loading_rand_seed));
        s.set_int("CrtRandState", static_cast<std::int32_t>(g.mission.crt_rand_state));
        return;
    }
    const BattleRequest& r = g.battle;
    const sim::BattleStart& b = r.start;
    s.set_string("Map", r.map);
    s.set_int("Schema", r.schema);
    s.set_int("Provider", b.provider);
    s.set_int("StartEnergy", bits(b.start_energy));
    s.set_int("StartMetal", bits(b.start_metal));
    s.set_int("CommanderMode", b.commander_mode);
    s.set_int("UnitLimit", b.unit_limit);
    s.set_int("FogOfWar", b.fog_of_war);
    s.set_int("Explore", b.explore);
    s.set_int("TrueSight", b.true_sight);
    s.set_int("TargetEverything", b.target_everything);
    s.set_int("CommandMode", b.command_mode);
    s.set_int("AiDifficulty", b.ai_difficulty);
    s.set_int("LocalPlayer", b.local_player);
    s.set_int("NetGame", b.net_game);
    s.set_int("FixedLocations", b.fixed_locations);
    s.set_int("BattleRngSeed", static_cast<std::int32_t>(b.battle_rng_seed));
    s.set_int("LoadingRandSeed", static_cast<std::int32_t>(b.loading_rand_seed));
    s.set_int("CrtRandState", static_cast<std::int32_t>(b.crt_rand_state));
    s.set_int("StartPositionCount", b.start_position_count);
    for (int k = 0; k < 10; ++k) {
        const std::string p = "StartPos" + std::to_string(k) + " ";
        s.set_int(p + "X", b.start_positions[static_cast<std::size_t>(k)][0]);
        s.set_int(p + "Z", b.start_positions[static_cast<std::size_t>(k)][1]);
    }
    s.set_int("RuleCount", static_cast<std::int32_t>(b.start_rules.size()));
    for (std::size_t k = 0; k < b.start_rules.size(); ++k) {
        const std::string p = "Rule" + std::to_string(k) + " ";
        s.set_int(p + "Id", b.start_rules[k].id);
        s.set_int(p + "X", b.start_rules[k].x);
        s.set_int(p + "Z", b.start_rules[k].z);
    }
    for (int k = 0; k < 10; ++k) {
        const sim::StartPlayer& sp = b.players[static_cast<std::size_t>(k)];
        const std::string p = "Player" + std::to_string(k) + " ";
        s.set_int(p + "Kind", static_cast<std::int32_t>(sp.kind));
        s.set_int(p + "NetId", sp.net_id);
        s.set_int(p + "CommanderType", sp.commander_type);
        s.set_int(p + "StartPosition", sp.start_position);
        s.set_int(p + "Colour", sp.colour);
        s.set_int(p + "AllyGroup", sp.ally_group);
        s.set_int(p + "Energy", sp.energy);
        s.set_int(p + "Metal", sp.metal);
        s.set_int(p + "Watcher", sp.watcher);
        s.set_int(p + "AlliedVictory", sp.allied_victory);
        s.set_string(p + "Name", sp.name);
        s.set_string(p + "Commander", sp.commander_name);
        s.set_string(p + "Side", r.sides[static_cast<std::size_t>(k)]);
    }
}

bool get_start(const BankSection& s, GameStart& g) {
    g = GameStart{};
    g.kind = static_cast<GameStart::Kind>(s.get_int("Kind", 0));
    if (g.kind == GameStart::Kind::Mission) {
        g.campaign = s.get_string("Campaign", "");
        g.mission_index = s.get_int("MissionIndex", -1);
        g.mission_file = s.get_string("MissionFile", "");
        g.mission.campaign_side = s.get_string("CampaignSide", "ARM");
        g.mission.other_side = s.get_string("OtherSide", "");
        g.mission.difficulty = s.get_int("Difficulty", 1);
        g.mission.battle_rng_seed = static_cast<std::uint32_t>(s.get_int("BattleRngSeed", 0));
        g.mission.loading_rand_seed = static_cast<std::uint32_t>(s.get_int("LoadingRandSeed", 0));
        g.mission.crt_rand_state = static_cast<std::uint32_t>(s.get_int("CrtRandState", 0));
        return !g.mission_file.empty();
    }
    if (g.kind != GameStart::Kind::Battle) return false;
    BattleRequest& r = g.battle;
    sim::BattleStart& b = r.start;
    r.map = s.get_string("Map", r.map);
    r.schema = s.get_int("Schema", -1);
    b.provider = s.get_int("Provider", 2);
    b.start_energy = from_bits(s.get_int("StartEnergy", bits(1000.0f)));
    b.start_metal = from_bits(s.get_int("StartMetal", bits(1000.0f)));
    b.commander_mode = s.get_int("CommanderMode", 0);
    b.unit_limit = static_cast<std::uint16_t>(s.get_int("UnitLimit", 250));
    b.fog_of_war = s.get_int("FogOfWar", 0) != 0;
    b.explore = s.get_int("Explore", 0) != 0;
    b.true_sight = s.get_int("TrueSight", 0) != 0;
    b.target_everything = s.get_int("TargetEverything", 0) != 0;
    b.command_mode = s.get_int("CommandMode", 0);
    b.ai_difficulty = s.get_int("AiDifficulty", 2);
    b.local_player = static_cast<std::uint8_t>(s.get_int("LocalPlayer", 0));
    b.net_game = s.get_int("NetGame", 0) != 0;
    b.fixed_locations = s.get_int("FixedLocations", 0) != 0;
    b.battle_rng_seed = static_cast<std::uint32_t>(s.get_int("BattleRngSeed", 0));
    b.loading_rand_seed = static_cast<std::uint32_t>(s.get_int("LoadingRandSeed", 0));
    b.crt_rand_state = static_cast<std::uint32_t>(s.get_int("CrtRandState", 0));
    b.start_position_count = s.get_int("StartPositionCount", 0);
    for (int k = 0; k < 10; ++k) {
        const std::string p = "StartPos" + std::to_string(k) + " ";
        b.start_positions[static_cast<std::size_t>(k)] = {s.get_int(p + "X", 0), s.get_int(p + "Z", 0)};
    }
    const std::int32_t rules = s.get_int("RuleCount", 0);
    for (std::int32_t k = 0; k < rules; ++k) {
        const std::string p = "Rule" + std::to_string(k) + " ";
        sim::StartRule rule;
        rule.id = s.get_int(p + "Id", 0);
        rule.x = static_cast<std::int16_t>(s.get_int(p + "X", 0));
        rule.z = static_cast<std::int16_t>(s.get_int(p + "Z", 0));
        b.start_rules.push_back(rule);
    }
    for (int k = 0; k < 10; ++k) {
        sim::StartPlayer& sp = b.players[static_cast<std::size_t>(k)];
        const std::string p = "Player" + std::to_string(k) + " ";
        sp.kind = static_cast<sim::PlayerKind>(s.get_int(p + "Kind", 0));
        sp.net_id = s.get_int(p + "NetId", 0);
        sp.commander_type = static_cast<std::uint16_t>(s.get_int(p + "CommanderType", 0));
        sp.start_position = s.get_int(p + "StartPosition", -1);
        sp.colour = static_cast<std::uint8_t>(s.get_int(p + "Colour", 0));
        sp.ally_group = static_cast<std::uint8_t>(s.get_int(p + "AllyGroup", 0xFF));
        sp.energy = s.get_int(p + "Energy", -1);
        sp.metal = s.get_int(p + "Metal", -1);
        sp.watcher = s.get_int(p + "Watcher", 0) != 0;
        sp.allied_victory = s.get_int(p + "AlliedVictory", 0) != 0;
        sp.name = s.get_string(p + "Name", "");
        sp.commander_name = s.get_string(p + "Commander", "");
        r.sides[static_cast<std::size_t>(k)] = s.get_string(p + "Side", "");
    }
    return true;
}

void put_summary(BankSection& s, const SaveSummary& m) {
    // The original's keys (GameSave_SaveAll 0x4326B0); its build stamp (assumed: the stamp
    // strings of the executable) as two zero-valued integer keys.
    s.set_int("BUILD DATE: Jul 30 1998", 0);
    s.set_int("BUILD TIME: 11:16:36", 0);
    s.set_int("maxunits", m.max_units);
    s.set_int("Difficulty", m.difficulty);
    s.set_int("Side", m.side);
    s.set_int("Players", m.players);
    s.set_int("Gametype", m.game_type);
    if (m.game_type == 2) {
        s.set_int("CommanderDeath", m.commander_death);
        s.set_int("Location", m.location);
        s.set_int("Mapping", m.mapping);
        s.set_int("LineOfSight", m.line_of_sight);
        s.set_int("LineOfSightType", m.line_of_sight_type);
    }
    if (m.between_missions) s.set_int("BetweenMissions", 1);
    s.set_int("Game ID", m.game_id);
    s.set_int("Game Time", static_cast<std::int32_t>(m.game_time));
    s.set_string("Campaign", m.campaign);
    s.set_string("Mission", m.mission);
    s.set_string("Map", m.map);
    s.set_string("Thumbs", "");
    if (!m.description.empty()) s.set_string("Description", m.description);
}

void get_summary(const BankSection& s, SaveSummary& m) {
    m.max_units = s.get_int("maxunits", 0);
    m.campaign = s.get_string("Campaign", "");
    m.mission = s.get_string("Mission", "");
    m.map = s.get_string("Map", "");
    m.description = s.get_string("Description", "");
    m.difficulty = s.get_int("Difficulty", 0);
    m.side = s.get_int("Side", 0);
    m.players = s.get_int("Players", 0);
    m.game_type = s.get_int("Gametype", 0);
    m.commander_death = s.get_int("CommanderDeath", 0);
    m.location = s.get_int("Location", 0);
    m.mapping = s.get_int("Mapping", 0);
    m.line_of_sight = s.get_int("LineOfSight", 0);
    m.line_of_sight_type = s.get_int("LineOfSightType", 0);
    m.between_missions = s.has("BetweenMissions");
    m.game_id = s.get_int("Game ID", 0);
    m.game_time = static_cast<std::uint32_t>(s.get_int("Game Time", 0));
}

}  // namespace

SaveSummary make_summary(const sim::Battle& b, const GameStart& g, const std::string& description) {
    SaveSummary m;
    m.max_units = b.settings.unit_limit;
    m.description = description;
    m.game_type = b.settings.provider;
    m.players = b.settings.player_count;
    m.game_time = b.game_tick();
    if (g.kind == GameStart::Kind::Mission) {
        m.campaign = g.campaign;
        m.mission = g.mission_file;
        m.map = g.mission_file;
        m.difficulty = g.mission.difficulty;
        m.side = stricmp_c(g.mission.campaign_side, "CORE") == 0 ? 1 : 0;
    } else {
        m.map = g.battle.map;
        m.difficulty = g.battle.start.ai_difficulty;
        m.side = stricmp_c(g.battle.sides[g.battle.start.local_player % 10], "CORE") == 0 ? 1 : 0;
        m.commander_death = g.battle.start.commander_mode;
        m.location = g.battle.start.fixed_locations;
        m.mapping = g.battle.start.explore;
        m.line_of_sight = g.battle.start.fog_of_war;
        m.line_of_sight_type = g.battle.start.true_sight;
    }
    m.engine = true;
    return m;
}

bool write_saved_game(const SavedGame& save, std::vector<std::uint8_t>& out) {
    Bank bank;
    bank.name = kBankName;
    put_summary(bank.section("Summary"), save.summary);
    put_start(bank.section("Engine Start"), save.start);
    BankSection& check = bank.section("Engine Check");
    check.set_int("Version", 1);
    check.set_int("Tick", static_cast<std::int32_t>(save.tick));
    check.set_int("Checksum", static_cast<std::int32_t>(save.checksum));
    check.set_int("BattleRng", static_cast<std::int32_t>(save.battle_rng));
    check.set_int("CrtRand", static_cast<std::int32_t>(save.crt_rand));
    save.journal.encode(bank.section("Engine Journal").blob("Journal").data);
    write_bank(bank, out);
    return true;
}

bool read_save_summary(const std::vector<std::uint8_t>& bytes, SaveSummary& out, std::string& error) {
    Bank bank;
    if (!read_bank(bytes, bank, error)) return false;
    if (stricmp_c(bank.name, kBankName) != 0) {
        error = "not a saved game";
        return false;
    }
    const BankSection* s = bank.find("Summary");
    if (!s) {
        error = "no summary";
        return false;
    }
    out = SaveSummary{};
    get_summary(*s, out);
    out.engine = bank.find("Engine Journal") != nullptr;
    return true;
}

bool read_saved_game(const std::vector<std::uint8_t>& bytes, SavedGame& out, std::string& error) {
    Bank bank;
    if (!read_bank(bytes, bank, error)) return false;
    out = SavedGame{};
    const BankSection* sum = bank.find("Summary");
    const BankSection* start = bank.find("Engine Start");
    const BankSection* check = bank.find("Engine Check");
    const BankSection* journal = bank.find("Engine Journal");
    if (sum) get_summary(*sum, out.summary);
    if (!start || !check || !journal) {
        error = "a saved game of the original (its own sections are not restored by the engine)";
        return false;
    }
    out.summary.engine = true;
    if (!get_start(*start, out.start)) {
        error = "bad start description";
        return false;
    }
    out.tick = static_cast<std::uint32_t>(check->get_int("Tick", 0));
    out.checksum = static_cast<std::uint32_t>(check->get_int("Checksum", 0));
    out.battle_rng = static_cast<std::uint32_t>(check->get_int("BattleRng", 0));
    out.crt_rand = static_cast<std::uint32_t>(check->get_int("CrtRand", 0));
    const BankSection::Blob* jb = journal->find_blob("Journal");
    if (!jb || !out.journal.decode(jb->data)) {
        error = "bad journal";
        return false;
    }
    return true;
}

std::unique_ptr<LoadedBattle> load_saved_game(Vfs& vfs, const SavedGame& save, sim::BattleOutside& outside, std::string& error) {
    auto lb = start_game(vfs, save.start, outside, error);
    if (!lb) return nullptr;
    sim::Battle& b = *lb->battle;
    sim::JournalPlayer player(save.journal);
    if (!player.run_to(b, save.tick)) {
        error = "the replay differs from the recording at step " + std::to_string(player.mismatch_tick);
        return nullptr;
    }
    player.apply_rest(b);  // commands given after the last step, before the save
    if (sim::state_checksum(b) != save.checksum || b.battle_rng().state() != save.battle_rng || b.crt_rand.state() != save.crt_rand) {
        error = "the replayed state differs from the saved one";
        return nullptr;
    }
    return lb;
}

}  // namespace ta
