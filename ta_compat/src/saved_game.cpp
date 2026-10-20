// Saved games: the bank container, the Summary, reading saves and starting their battle
// (docs/RE_MEMORY.md, "Saved games"). The battle sections are in save_sections.cpp.

#include "ta/saved_game.h"

#include <cstring>

#include "net/codec.h"
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

// One SQSH chunk with the dictionary coder (0x4D1820, method 1, not encrypted): "SQSH",
// version 2, method, encryption flag, compressed size, original size, the byte sum of the
// compressed data. `bound` is the output buffer the original allows (larger: it fails).
bool squash(const std::vector<std::uint8_t>& in, std::uint32_t bound, std::uint8_t stale, std::vector<std::uint8_t>& out) {
    const net::CompressResult c = net::compress(in.data(), in.size(), stale);
    if (bound < c.data.size() + 0x13) return false;
    out.clear();
    out.insert(out.end(), {'S', 'Q', 'S', 'H', 2, 1, 0});
    wr32(out, static_cast<std::uint32_t>(c.data.size()));
    wr32(out, static_cast<std::uint32_t>(in.size()));
    std::uint32_t sum = 0;
    for (std::uint8_t b : c.data) sum += b;
    wr32(out, sum);
    out.insert(out.end(), c.data.begin(), c.data.end());
    return true;
}

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
double BankSection::get_double(const std::string& k, double def) const {
    for (const auto& e : doubles)
        if (stricmp_c(e.first, k) == 0) return e.second;
    return def;
}
std::string BankSection::get_string(const std::string& k, const std::string& def) const {
    const std::string* s = find_string(k);
    return s ? *s : def;
}
const std::string* BankSection::find_string(const std::string& k) const {
    for (const auto& e : strings)
        if (stricmp_c(e.first, k) == 0) return &e.second;
    return nullptr;
}
BankSection::Blob& BankSection::blob(const std::string& n) {
    for (Blob& b : blobs)
        if (b.named && stricmp_c(b.name, n) == 0) return b;
    blobs.push_back(Blob{true, n, 0, {}});
    return blobs.back();
}
BankSection::Blob& BankSection::numbered(std::int32_t index) {
    for (Blob& b : blobs)
        if (!b.named && b.index == index) return b;
    blobs.push_back(Blob{false, {}, index, {}});
    return blobs.back();
}
const BankSection::Blob* BankSection::find_blob(const std::string& n) const {
    for (const Blob& b : blobs)
        if (b.named && stricmp_c(b.name, n) == 0) return &b;
    return nullptr;
}
const BankSection::Blob* BankSection::find_numbered(std::int32_t index) const {
    for (const Blob& b : blobs)
        if (!b.named && b.index == index) return &b;
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
            BankSection::Blob* b =
                noff < 0 ? &s.numbered(static_cast<std::int32_t>(rd32(payload, at + 4))) : &s.blob(cstr(dir, static_cast<std::uint32_t>(noff)));
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

void write_bank(const Bank& bank, std::vector<std::uint8_t>& out, std::vector<std::size_t>* stale, const BankWriteOptions& opt) {
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
    // The descriptor word of named blobs persists over the accounts of one write.
    std::uint32_t index_word = opt.named_blob_word;
    bool index_known = false;
    for (const BankSection& s : bank.sections) {
        // An account is written when it has entries or blobs (empty blobs count).
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
        std::vector<std::size_t> stale_here;
        for (const auto& b : s.blobs) {
            if (b.data.empty()) continue;
            if (b.named) {
                wr32(out, add(b.name));
                if (!index_known) stale_here.push_back(out.size());
                wr32(out, index_word);
            } else {
                wr32(out, 0xFFFFFFFFu);
                index_word = static_cast<std::uint32_t>(b.index);
                index_known = true;
                wr32(out, index_word);
            }
            wr32(out, static_cast<std::uint32_t>(data_at));
            wr32(out, static_cast<std::uint32_t>(b.data.size()));
            data_at += b.data.size();
        }
        for (const auto& b : s.blobs) out.insert(out.end(), b.data.begin(), b.data.end());
        std::uint32_t size = static_cast<std::uint32_t>(out.size() - start), packed = 0;
        if (opt.squash) {
            // HapiBank_WriteAccount: the entries and bytes squashed when that is smaller.
            const std::vector<std::uint8_t> body(out.begin() + static_cast<std::ptrdiff_t>(start + 0x20), out.end());
            const std::uint32_t n = static_cast<std::uint32_t>(body.size());
            std::vector<std::uint8_t> chunk;
            if (squash(body, n * 120u / 100u + 0x77u, opt.squash_stale_byte, chunk) && chunk.size() < n) {
                out.resize(start + 0x20);
                out.insert(out.end(), chunk.begin(), chunk.end());
                size = static_cast<std::uint32_t>(chunk.size() + 0x20);
                packed = 1;
                stale_here.clear();
            }
        }
        if (stale) stale->insert(stale->end(), stale_here.begin(), stale_here.end());
        set32(out, start, size);
        set32(out, start + 4, sec_name);
        set32(out, start + 8, static_cast<std::uint32_t>(s.ints.size()));
        set32(out, start + 12, static_cast<std::uint32_t>(s.doubles.size()));
        set32(out, start + 16, static_cast<std::uint32_t>(s.strings.size()));
        set32(out, start + 20, nb);
        set32(out, start + 24, packed);
    }
    const std::uint32_t dir_off = static_cast<std::uint32_t>(out.size());
    std::vector<std::uint8_t> chunk;
    if (opt.squash && squash(dir, static_cast<std::uint32_t>(dir.size()) * 110u / 100u + 0x77u, opt.squash_stale_byte, chunk) &&
        chunk.size() < dir.size()) {
        out.insert(out.end(), chunk.begin(), chunk.end());
        out[0x18] = 1;
    } else {
        out.insert(out.end(), dir.begin(), dir.end());
    }
    set32(out, 0x08, name_off);
    set32(out, 0x0C, dir_off);
    set32(out, 0x10, 0x22);
    set32(out, 0x14, 1);
}

// --- The Summary (GameSave_SaveAll 0x4326B0; read by the load menu 0x492360).

void read_summary(const BankSection& s, SaveSummary& m) {
    m.max_units = s.get_int("maxunits", 0);
    m.has_campaign = s.find_string("Campaign") != nullptr;
    m.campaign = s.get_string("Campaign", "");
    m.mission = s.get_string("Mission", "");
    m.map = s.get_string("Map", "");
    m.thumbs = s.get_string("Thumbs", "");
    m.has_description = s.find_string("Description") != nullptr;
    m.description = s.get_string("Description", "");
    m.difficulty = s.get_int("Difficulty", 0);
    m.side = s.get_int("Side", 0);
    m.players = s.get_int("Players", 0);
    m.game_type = s.get_int("Gametype", 0);
    m.skirmish_keys = s.has("CommanderDeath");
    // The load menu's defaults for a skirmish summary without the keys are 1.
    m.commander_death = s.get_int("CommanderDeath", 1);
    m.location = s.get_int("Location", 1);
    m.mapping = s.get_int("Mapping", 1);
    m.line_of_sight = s.get_int("LineOfSight", 1);
    m.line_of_sight_type = s.get_int("LineOfSightType", 1);
    m.between_missions = s.has("BetweenMissions");
    m.game_id = s.get_int("Game ID", 0);
    m.game_time = static_cast<std::uint32_t>(s.get_int("Game Time", 0));
}

namespace {

// The Summary keys in the original's order. `in_battle`: the front end's state is the battle
// (GS+0x391F1 = 6); otherwise the save is between missions.
void put_summary(Bank& bank, const SaveContext& c, std::uint32_t game_time) {
    BankSection& s = bank.section("Summary");
    s.set_int("BUILD DATE: Jul 30 1998", 0);  // "BUILD DATE: %s" with the executable's stamps
    s.set_int("BUILD TIME: 11:16:36", 0);
    s.set_int("maxunits", static_cast<std::int32_t>(c.max_units));
    if (!c.campaign.empty()) s.set_string("Campaign", c.campaign);  // a null name writes no key
    s.set_string("Mission", c.mission);
    s.set_string("Map", c.mission);
    s.set_int("Difficulty", c.difficulty);
    s.set_int("Side", c.side);
    s.set_int("Players", c.players);
    s.set_int("Gametype", c.game_type);
    s.set_string("Thumbs", c.thumbs);
    if (c.game_type == 2) {
        s.set_int("CommanderDeath", c.commander_death);
        s.set_int("Location", c.location);
        s.set_int("Mapping", c.mapping);
        s.set_int("LineOfSight", c.line_of_sight);
        s.set_int("LineOfSightType", c.line_of_sight_type);
    }
    if (c.between_missions) s.set_int("BetweenMissions", 1);
    s.set_string("Description", c.description);
    s.set_int("Game ID", c.game_id);
    s.set_int("Game Time", static_cast<std::int32_t>(game_time));
}

}  // namespace

Bank make_between_missions_bank(const SaveContext& ctx) {
    Bank bank;
    bank.name = kBankName;
    SaveContext c = ctx;
    c.between_missions = true;
    put_summary(bank, c, 0);
    return bank;
}

// Defined in save_sections.cpp.
void write_battle_sections(const LoadedBattle& lb, Bank& bank, const SaveContext& ctx, std::vector<std::string>* notes);

Bank make_save_bank(const LoadedBattle& lb, const SaveContext& ctx, std::vector<std::string>* notes) {
    Bank bank;
    bank.name = kBankName;
    put_summary(bank, ctx, lb.battle->game_tick());
    if (!ctx.between_missions) write_battle_sections(lb, bank, ctx, notes);
    return bank;
}

bool write_save_file(const LoadedBattle& lb, const SaveContext& ctx, std::vector<std::uint8_t>& out) {
    const Bank bank = make_save_bank(lb, ctx);
    write_bank(bank, out, nullptr, ctx.bank);
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
    read_summary(*s, out);
    out.has_battle = bank.find("Units") || bank.find("Players");
    return true;
}

bool read_saved_game(const std::vector<std::uint8_t>& bytes, SavedGame& out, std::string& error) {
    out = SavedGame{};
    if (!read_bank(bytes, out.bank, error)) return false;
    const BankSection* s = out.bank.find("Summary");
    if (!s) {
        error = "Invalid savegame file";
        return false;
    }
    read_summary(*s, out.summary);
    out.summary.has_battle = out.bank.find("Units") || out.bank.find("Players");
    return true;
}

// --- Starting a saved game (the load menu 0x492360, then the battle start 0x497180 with the
// bank).

namespace {

std::unique_ptr<TdfSection> parse_vfs(Vfs& vfs, const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!vfs.read(path, bytes)) return nullptr;
    return std::move(tdf_parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).root);
}

// The side names by side index (gamedata\sidedata.tdf [SIDEn] name).
std::string side_name(Vfs& vfs, int index) {
    if (const std::unique_ptr<TdfSection> sides = parse_vfs(vfs, "gamedata\\sidedata.tdf"))
        if (const TdfSection* s = sides->child("SIDE" + std::to_string(index))) return s->get_string("name", 32, "");
    return index == 1 ? "CORE" : "ARM";
}

}  // namespace

bool make_saved_game_request(Vfs& vfs, const SavedGame& save, const LoadInputs& in, BattleRequest& req, LoadedView& view, std::string& error) {
    const SaveSummary& m = save.summary;
    if (m.between_missions) {
        error = "a save between missions (it opens the mission's briefing)";
        return false;
    }
    if (m.game_type == 1) {
        // A campaign: the mission by its name in the campaign's list, the difficulty and side.
        Campaign c;
        if (!m.has_campaign || !load_campaign(vfs, m.campaign, c, error)) {
            if (error.empty()) error = "Invalid savegame file";
            return false;
        }
        const CampaignMission* found = nullptr;
        for (const CampaignMission& cm : c.missions)
            if (stricmp_c(cm.name, m.mission) == 0) {
                found = &cm;
                break;
            }
        if (!found) {
            error = "Invalid savegame file";
            return false;
        }
        MissionInfo info;
        if (!load_mission(vfs, found->file, m.difficulty, info, error)) return false;
        MissionRequest rq;
        rq.campaign_side = side_name(vfs, m.side == 0 ? 0 : 1);
        rq.other_side = side_name(vfs, m.side == 0 ? 1 : 0);
        rq.difficulty = m.difficulty;
        rq.battle_rng_seed = in.battle_rng_seed;
        rq.loading_rand_seed = in.loading_rand_seed;
        rq.crt_rand_state = in.crt_rand_state;
        req = make_mission_request(vfs, info, rq);
    } else if (m.game_type == 2) {
        // A skirmish: the map, the players by the saved controllers and sides, the options.
        req = BattleRequest{};
        req.map = m.mission;
        sim::BattleStart& s = req.start;
        s.provider = 2;
        s.unit_limit = in.unit_limit;
        s.commander_mode = m.commander_death;
        s.fixed_locations = m.location != 0;
        s.explore = m.mapping != 0;
        s.fog_of_war = m.line_of_sight != 0;
        s.true_sight = m.line_of_sight_type != 0;
        s.ai_difficulty = m.difficulty;
        s.battle_rng_seed = in.battle_rng_seed;
        s.loading_rand_seed = in.loading_rand_seed;
        s.crt_rand_state = in.crt_rand_state;
        for (int k = 0; k < 10; ++k) {
            const BankSection* ps = save.bank.find("Player " + std::to_string(k));
            if (!ps) continue;
            const std::int32_t ctl = ps->get_int("Controller", 0);
            if (ctl != 1 && ctl != 2) continue;
            sim::StartPlayer& sp = s.players[static_cast<std::size_t>(k)];
            sp.kind = ctl == 1 ? sim::PlayerKind::Local : sim::PlayerKind::Computer;
            sp.net_id = k + 1;
            sp.colour = static_cast<std::uint8_t>(ps->get_int("Logo", k));
            req.sides[static_cast<std::size_t>(k)] = side_name(vfs, ps->get_int("Side", 0));
            sp.name = start_player_name(ctl != 1, ps->get_int("Side", 0));
            if (ctl == 1) s.local_player = static_cast<std::uint8_t>(k);  // the last human slot (0x47A760)
        }
    } else {
        error = "Invalid savegame file";
        return false;
    }
    req.saved_game = true;
    const Bank* bank = &save.bank;
    LoadedView* v = &view;
    auto previous = req.prepare_start;
    req.prepare_start = [previous, bank, v](LoadedBattle& lb, sim::BattleStart& start) {
        if (previous) previous(lb, start);
        LoadedBattle* plb = &lb;
        start.restore = [plb, bank, v](sim::Battle&, std::string& err) { return load_save_sections(*plb, *bank, *v, err); };
    };
    return true;
}

std::unique_ptr<LoadedBattle> load_saved_game(Vfs& vfs, const SavedGame& save, const LoadInputs& in, sim::BattleOutside& outside,
                                              LoadedView& view, std::string& error) {
    BattleRequest req;
    if (!make_saved_game_request(vfs, save, in, req, view, error)) return nullptr;
    return load_battle(vfs, req, outside, error);
}

}  // namespace ta
