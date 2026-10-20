// The original's typed commands (docs/RE_MEMORY.md, "Console commands"): the command table
// (names, permission bits, effects) on top of the engine's registry (sim/console.h).

#include "ta/console_commands.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "client/game_client.h"
#include "sim/ai.h"
#include "sim/battle.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/construction.h"
#include "sim/economy.h"
#include "sim/features.h"
#include "sim/unit_create.h"
#include "sim/victory.h"
#include "sim/x87.h"
#include "ta/ai_profile.h"
#include "ta/battle_loader.h"
#include "ta/battle_types.h"
#include "ta/client_world.h"
#include "ta/crt_strtod.h"

namespace ta {

using sim::console::Words;
namespace cb = console_bits;

struct ConsoleCommands::Impl {
    client::GameClient& c;
    ClientWorld& w;
    sim::console::Registry reg;
    ConsoleState st;
    std::array<std::int32_t, 3> cursor{};  // the pointer's map point (GS+0x2CAA) while a line runs

    Impl(client::GameClient& client, ClientWorld& world) : c(client), w(world) { build(); }

    sim::Battle& b() { return w.battle(); }
    int local() const { return c.options().player; }  // GS+0x2A42
    int view() const { return c.viewed_player(); }    // GS+0x2A43

    // A record index that names a player in the game: below 10, a record, a controller (1 local,
    // 2 computer, 3 remote) and not out of the game (state 10).
    bool playing(std::uint32_t n) {
        n &= 0xFF;
        if (n >= 10 || n >= b().players.size()) return false;
        const sim::PlayerSlot& p = b().players[n];
        return p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10;
    }
    void say(const std::string& text) { c.message(text); }
    void not_built(const char* name) { st.not_built.push_back(name); }
    void sight_view_changed() { c.options_mut().fog = b().settings.fog_of_war || b().settings.explore; }
    // A whole sight rebuild (0x4816A0) ends by composing the minimap's fog picture and drawing
    // its marks at once.
    void rebuild_sight(bool clear_explored) {
        b().rebuild_sight(clear_explored);
        c.minimap_sight_rebuilt();
    }

    // 0x430F00 after the commands that save: every setting the battle holds is written.
    void save() {
        if (c.save_settings) c.save_settings();
    }

    // The share switches of the local player's side record (network games): flip, announce.
    void toggle_share(std::uint8_t bit, const char* what) {
        if (!b().settings.net_game) return;
        if (c.network.toggle_share_flag) c.network.toggle_share_flag(bit);
        sim::Battle& bt = b();
        const int p = local();
        bool on = false;
        if (bit == 0x40) {  // radar: the sight option allies read
            auto& o = bt.settings.player_sight[static_cast<std::size_t>(p) % bt.settings.player_sight.size()];
            o.shares_radar = !o.shares_radar;
            on = o.shares_radar;
        } else {
            sim::PlayerEconomy& e = bt.player_economy[static_cast<std::size_t>(p)];
            e.share_flags = static_cast<std::uint8_t>(e.share_flags ^ bit);
            on = (e.share_flags & bit) != 0;
        }
        say(std::string("Toggled ") + what + " to: " + (on ? "ON" : "OFF"));
    }

    // The default command (0x417890): every unit type whose name matches the first word as a
    // wildcard is created, fully built, for player <word 1> (default 0) in a row from the
    // pointer; when none matches, debugdat\<word>.txt runs as command text.
    void default_command(const Words& words) {
        const std::string pattern = words.text(0);
        std::array<std::int32_t, 3> pos = cursor;
        const UnitDefTable& defs = w.defs();
        sim::Battle& bt = b();
        const std::int32_t row_end = static_cast<std::int32_t>(static_cast<std::uint32_t>(bt.scroll_width()) * 0x10000u);
        const std::int32_t player = words.number(1, 0);
        sim::console::UnitRowHooks hooks;
        hooks.matches = [&](std::uint16_t t) { return t < defs.units.size() && sim::console::wildcard_match(defs.units[t].unit_name, pattern); };
        hooks.extents_x = [&](std::uint16_t t) {
            return t < bt.types.size() ? std::array<std::int32_t, 2>{bt.types[t].info.model_min_x, bt.types[t].info.model_max_x}
                                       : std::array<std::int32_t, 2>{0, 0};
        };
        hooks.snap = [&](std::uint16_t t, std::array<std::int32_t, 3>& p) { sim::snap_to_footprint(bt.construction(), t, p); };
        hooks.create = [&](std::uint16_t t, const std::array<std::int32_t, 3>& p) { bt.create_unit(player, t, p[0], p[1], p[2], true, 1, 0); };
        const int matched = sim::console::place_unit_row(pos, row_end, static_cast<int>(defs.units.size()), hooks);
        if (matched == 0) include_file(pattern, words);
    }

    // The pointer's cell (GS+0x2C8E / 0x2C90: the map point's x and z >> 20 as 16-bit values)
    // as a cell index, -1 off the map (the map point is clamped to the map, so not in practice).
    std::int32_t pointer_cell() {
        const sim::MapGrid& m = b().map;
        const auto x = static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(cursor[0]) >> 20));
        const auto z = static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(cursor[2]) >> 20));
        if (x < 0 || z < 0 || x >= m.width || z >= m.height) return -1;
        return static_cast<std::int32_t>(z) * m.width + x;
    }
    // The feature under a cell (0x421E60, GS+0x2CBC for the pointer's cell): a part gives its
    // anchor's feature, the other special values 0xFFFF.
    std::uint16_t feature_under(std::int32_t cell) {
        const sim::MapGrid& m = b().map;
        const sim::MapCell& c = m.cells[static_cast<std::size_t>(cell)];
        if (c.feature <= 0xFFFA) return c.feature;
        if (c.feature != 0xFFFE) return 0xFFFF;
        const std::int32_t anchor = cell - ((c.feature_aux & 0xFF) * m.width + (c.feature_aux >> 8));
        if (anchor < 0 || static_cast<std::size_t>(anchor) >= m.cells.size()) return 0xFFFF;  // the original reads outside the grid
        return m.cells[static_cast<std::size_t>(anchor)].feature;
    }
    // A mission kind by name (MissionOrder_FindByName: lower bound over the sorted lookup
    // names, case-insensitive; 0 when it is not there).
    static std::uint8_t mission_by_name(const std::string& name) {
        static const std::vector<std::string> names = UnitDefContext::default_mission_names();
        std::size_t lo = 0, hi = names.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (sim::console::c_stricmp(names[mid], name) < 0) lo = mid + 1;
            else hi = mid;
        }
        return lo != names.size() && sim::console::c_stricmp(names[lo], name) == 0 ? static_cast<std::uint8_t>(lo) : 0;
    }

    // "+reload <unit>" (0x417490): every unit of the type is killed through the kill packet
    // (0x486E80, kind 8: every table entry of that type), then its FBI and script are read
    // again (0x42D1F0) when the type can be made.
    void reload_type(const std::string& name) {
        LoadedBattle& lb = w.loaded();
        sim::Battle& bt = b();
        const std::uint16_t t = lb.type_index(name);
        if (t == 0 || t >= bt.types.size()) return;
        for (std::size_t i = 1; i < bt.units.size(); ++i)
            if (bt.ticks[i].type_index == t) sim::kill_unit(bt.combat(), bt.units[i], 8, st.kill_word);
        if (!(bt.types[t].create.type_flags & sim::create_flags::kLoadable)) return;
        std::string error;
        if (!reload_unit_properties(w.vfs_, lb.unit_context, lb.unit_defs, t, lb.feature_defs, st.reload_yard, error)) {
            bt.feature_world().state.fault = error;
            return;
        }
        const UnitDefRecord& d = lb.unit_defs.units[t];
        auto assets = std::make_unique<UnitTypeAssets>();
        if (!load_unit_assets(w.vfs_, d, *assets, error)) return;
        const std::uint32_t loadable = bt.types[t].create.type_flags & sim::create_flags::kLoadable;  // not an FBI key
        to_battle_unit_type(d, *assets, lb.weapons, bt.types[t]);
        to_ai_type(d, lb.weapons, bt.types[t].ai);
        bt.types[t].create.type_flags = (bt.types[t].create.type_flags & ~sim::create_flags::kLoadable) | loadable;
        // Category objects are shared by pointer in the original: every type's masks see the change.
        for (std::size_t k = 1; k < bt.types.size() && k < lb.unit_defs.units.size(); ++k)
            if (lb.type_assets[k] || k == t) to_targeting_type(lb.unit_defs.units[k], lb.unit_defs.categories, lb.weapons, bt.types[k].targeting);
        if (lb.type_assets[t]) lb.retired_assets.push_back(std::move(lb.type_assets[t]));
        lb.type_assets[t] = std::move(assets);
        bt.unit_types_changed();
    }

    void include_file(const std::string& name, const Words& parent) {
        std::vector<std::uint8_t> text;
        if (!w.vfs_.read("debugdat\\" + name + ".txt", text)) return;
        const auto saved = cursor;
        reg.run_text(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), parent, 0xFFFFFFFFu);
        cursor = saved;
    }

    // "+printweights <player> <file>" (0x418BB0, exactly three words): for a player in the
    // game, <file> opened "w+b" gets the computer player's table (0x40C250).
    void print_weights(const Words& a) {
        if (a.count() != 3) return;
        const std::uint32_t p = static_cast<std::uint32_t>(a.number(1, 0));
        if (!playing(p)) return;
        const std::size_t slot = p & 0xFF;
        // (a name without a folder lands in the working folder: the game folder)
        std::string file = a.text(2);
        if (std::filesystem::path(file).is_relative()) file = w.game_dir() + "\\" + file;
        std::FILE* f = std::fopen(file.c_str(), "w+b");
        if (!f) return;
        sim::Battle& bt = b();
        const std::uint32_t tick = bt.game_tick();
        const std::uint32_t hours = tick / 108000u;
        const std::int32_t rest = static_cast<std::int32_t>(tick - hours * 108000u);
        const std::int32_t minutes = rest / 1800, seconds = (rest - minutes * 1800) / 30;
        char t[64];
        std::snprintf(t, sizeof t, "%02d:%02d:%02d", static_cast<int>(hours), minutes, seconds);
        std::fprintf(f, "gametime: '%s'\r\n", t);
        const std::string& name = slot < w.content().player_names.size() ? w.content().player_names[slot] : std::string();
        std::fprintf(f, "player:   '%s' num: %d\r\n", name.c_str(), static_cast<int>(p));
        const std::uint8_t ctl = bt.players[slot].controller;
        std::fprintf(f, "controller: %s\r\n", ctl == 1 ? "HUMAN" : ctl == 2 ? "AI" : "INVALID");
        std::fprintf(f, "terrain:    '%s'\r\n", ("Maps\\" + w.request().map + ".TNT").c_str());
        LoadedBattle& lb = w.loaded();
        std::fprintf(f, "profile:    '%s'\r\n", ai_profile_path(lb.ota, lb.schema < 0 ? 0 : lb.schema).c_str());
        static const char* const kDifficulty[3] = {"EASY", "MEDIUM", "HARD"};
        const std::int32_t d = bt.settings.ai_difficulty;
        std::fprintf(f, "difficulty: '%s'\r\n", d >= 0 && d < 3 ? kDifficulty[d] : "");
        std::fprintf(f, "================================================\r\n");
        std::fprintf(f, "<limit> - <base:baseML:baseEL> : <end result - before economy-based tweaks> - <unit name>\r\n");
        const UnitDefTable& defs = w.defs();
        const sim::AiTable& tb = bt.ai_state.tables[slot % bt.ai_state.tables.size()];
        for (std::size_t k = 1; k < defs.units.size(); ++k) {
            const std::int32_t limit = k < tb.limit.size() ? tb.limit[k] : -1;
            if (limit < 0) std::fprintf(f, "n/a ");
            else std::fprintf(f, "%4d", limit);
            const std::array<std::int8_t, 3> sc = k < tb.build_scores.size() ? tb.build_scores[k] : std::array<std::int8_t, 3>{};
            const int weight = k < tb.weight.size() ? tb.weight[k] : 0;
            std::fprintf(f, " - %3d : %3d : %3d = %3d - '%s\t\t:%s'\r\n", sc[0], sc[1], sc[2], weight, defs.units[k].unit_name.c_str(),
                         defs.units[k].name.c_str());
        }
        std::fclose(f);
    }

    void build();
};

void ConsoleCommands::Impl::build() {
    auto add = [this](const char* name, sim::console::Handler h) { reg.add(name, std::move(h), console_table_bits(name)); };
    
    // ---- options (bit 1): always allowed ----
    add("NoShake", [this](const Words&) { b().settings.shake_disabled = !b().settings.shake_disabled; });
    add("Contour", [this](const Words& a) {
        // (0x416DB0: ftol of atof of words 1 and 2, defaults 0 and 0.75: the interval and offset of
        // the contour lines over the terrain, 0x418310)
        st.contour[0] = sim::x87::ftol32(a.count() > 1 ? crt_atof(a.text(1)) : 0.0);
        st.contour[1] = sim::x87::ftol32(a.count() > 2 ? crt_atof(a.text(2)) : static_cast<double>(0.75f));
        c.options_mut().contour_interval = st.contour[0];
        c.options_mut().contour_offset = st.contour[1];
    });
    add("ScrollSpeed", [this](const Words& a) {
        c.options_mut().scroll_speed = static_cast<std::uint8_t>(a.number(1, 0));
        save();
    });
    add("IFace", [this](const Words& a) {
        c.options_mut().right_click_orders = a.number(1, 0) == 1;
        save();
    });
    add("Give", [this](const Words& a) {
        // Give <player> <amount> metal|energy: a transfer from the local player.
        if (!playing(static_cast<std::uint32_t>(a.number(1, 0)))) return;
        const auto to = static_cast<std::uint8_t>(a.number(1, 0));
        sim::Economy e = b().economy();
        if (sim::console::c_stricmp(a.text(3), "metal") == 0)
            sim::transfer_metal(e, static_cast<std::uint8_t>(local()), to, static_cast<float>(a.number(2, 0)), true);
        if (sim::console::c_stricmp(a.text(3), "energy") == 0)
            sim::transfer_energy(e, static_cast<std::uint8_t>(local()), to, static_cast<float>(a.number(2, 0)), true);
    });
    // The music (0x4167F0: the CD player's play of track n, 0 = a track the mood picks; 0x416810:
    // stop; 0x4175E0: the music mode).
    add("CDPlay", [this](const Words& a) {
        if (c.music_command) c.music_command("play", a.number(1, 0));
    });
    add("CDStop", [this](const Words&) {
        if (c.music_command) c.music_command("stop", 0);
    });
    add("Sound3D", [this](const Words&) {
        auto& o = c.options_mut();
        o.sound_mode = o.sound_mode == 2 ? 1 : 2;
        save();
    });
    add("Shading", [this](const Words&) {
        c.options_mut().shading = !c.options().shading;
        save();
    });
    add("AntiAlias", [this](const Words&) {
        c.options_mut().antialias = !c.options().antialias;
        save();
    });
    add("Shadow", [this](const Words&) {
        c.options_mut().shadows = !c.options().shadows;
        save();
    });
    add("Dither", [this](const Words&) {
        c.options_mut().dithered_fog = !c.options().dithered_fog;
        save();
    });
    add("SwitchAlt", [this](const Words& a) {
        auto& o = c.options_mut();
        if (a.count() < 2) {
            o.alt_groups = !o.alt_groups;
            save();
        } else {
            o.alt_groups = (a.number(1, 0) & 1) != 0;
        }
    });
    add("TShadow", [this](const Words&) { c.options_mut().unit_shadows = !c.options().unit_shadows; });
    add("FShadow", [this](const Words&) { c.options_mut().feature_shadows = !c.options().feature_shadows; });
    add("LOSType", [this](const Words&) {
        b().settings.true_sight = !b().settings.true_sight;
        rebuild_sight(false);
    });
    add("Light", [this](const Words& a) {
        // (0x4166C0 -> 0x4597F0: each word n becomes float(n) x 0.01 of the lit path's light
        // direction; then the drawing caches are made again)
        st.light[0] = a.number(1, 0);
        st.light[1] = a.number(2, 0);
        st.light[2] = a.number(3, 0);
        auto& o = c.options_mut();
        for (int k = 0; k < 3; ++k)
            o.light[static_cast<std::size_t>(k)] = static_cast<float>(static_cast<double>(static_cast<float>(st.light[k])) * 0.01);
    });
    add("RCache", [](const Words&) {});  // the original rebuilds its drawing caches
    add("Selectable", [this](const Words&) {
        // Every live unit becomes selectable (flag 0x20).
        for (std::size_t i = 1; i < b().units.size(); ++i)
            if (b().units[i].flags & sim::unit_flags::kAlive) b().units[i].flags |= sim::unit_tick_flags::kSelectable;
    });
    add("MusicMode", [this](const Words& a) {
        if (c.music_command) c.music_command("mode", a.number(1, 0));
    });
    add("Logo", [this](const Words& a) {
        // Logo <colour> <player>: the player's team colour (its logo frame).
        const std::int32_t logo = a.number(1, 0);
        const std::size_t count = w.content().team_logos ? w.content().team_logos->size() : 0;
        if (logo >= 0 && static_cast<std::size_t>(logo) < count && playing(static_cast<std::uint32_t>(a.number(2, 0)))) {
            const std::size_t p = static_cast<std::size_t>(a.number(2, 0) & 0xFF);
            w.request_.start.players[p].colour = static_cast<std::uint8_t>(logo);
            w.content().player_colour[p] = static_cast<std::uint8_t>(logo);
            return;
        }
        say("Invalid logo setting");
    });
    add("ScreenChat", [this](const Words&) {
        st.screen_chat = !st.screen_chat;
        c.options_mut().screen_chat = st.screen_chat;
        save();
    });
    add("Gamma", [this](const Words& a) {
        // (0x417290: the display gamma n x 0.1, n kept as the Gamma setting and saved)
        st.gamma = a.number(1, 0);
        if (c.on_gamma_command) c.on_gamma_command(st.gamma);
    });
    add("Clock", [this](const Words&) {
        c.options_mut().clock = !c.options().clock;
        save();
    });
    add("NetStats", [this](const Words&) { c.reset_net_stats(); });
    add("Sing", [this](const Words&) {
        st.sing = !st.sing;
        c.options_mut().sing = st.sing;
    });
    add("NoMetal", [this](const Words& a) {
        const std::uint32_t p = a.count() == 1 ? static_cast<std::uint32_t>(local()) : static_cast<std::uint8_t>(a.number(1, 0));
        if (playing(p)) b().players[p & 0xFF].metal_stored = static_cast<float>(a.number(2, 0));
    });
    add("NoEnergy", [this](const Words& a) {
        const std::uint32_t p = a.count() == 1 ? static_cast<std::uint32_t>(local()) : static_cast<std::uint8_t>(a.number(1, 0));
        if (playing(p)) b().players[p & 0xFF].energy_stored = static_cast<float>(a.number(2, 0));
    });
    add("BigBrother", [this](const Words&) {
        st.big_brother = !st.big_brother;
        c.toggle_big_brother();
    });
    add("Now", [this](const Words& a) {
        // Unlocks the debug commands with exactly these five words (compared case-sensitively);
        // any other "Now" line locks them again.
        c.options_mut().debug_commands = a.count() == 6 && a.text(1) == "Film" && a.text(2) == "Chris" && a.text(3) == "Include" &&
                                        a.text(4) == "Reload" && a.text(5) == "Assert";
    });
    add("Drop", [this](const Words& a) {
        st.net_drop_off = a.number(1, 0) == 0;
        if (c.network.set_timeout_off) c.network.set_timeout_off(st.net_drop_off);
    });
    add("ShootAll", [this](const Words&) { b().settings.target_everything = !b().settings.target_everything; });
    add("ShareMetal", [this](const Words&) { toggle_share(2, "ShareMetal"); });
    add("ShareEnergy", [this](const Words&) { toggle_share(4, "ShareEnergy"); });
    add("ShareMapping", [this](const Words&) { toggle_share(0x20, "ShareMapping"); });
    add("ShareRadar", [this](const Words&) { toggle_share(0x40, "ShareRadar"); });
    add("ShareAll", [this](const Words&) {
        if (!b().settings.net_game) return;
        toggle_share(2, "ShareMetal");
        toggle_share(4, "ShareEnergy");
        toggle_share(0x20, "ShareMapping");
        toggle_share(0x40, "ShareRadar");
    });
    add("ShowRanges", [this](const Words&) { c.options_mut().show_ranges = !c.options().show_ranges; });
    auto set_share = [this](const Words& a, bool metal) {
        if (!b().settings.net_game) return;
        sim::PlayerEconomy& e = b().player_economy[static_cast<std::size_t>(local())];
        const float storage = metal ? e.metal_storage : e.energy_storage;
        const float asked = static_cast<float>(a.number(1, 0));
        // The asked amount unless it is above the storage (an unordered storage takes the amount).
        const float v = asked > storage ? storage : asked;
        (metal ? e.share_metal_above : e.share_energy_above) = v;
        say(std::string("OK.  Will share ") + (metal ? "metal" : "energy") + " if above " + std::to_string(a.number(1, 0)));
    };
    add("SetShareMetal", [set_share](const Words& a) { set_share(a, true); });
    add("SetShareEnergy", [set_share](const Words& a) { set_share(a, false); });
    add("Compression", [this](const Words&) {
        // (0x4194D0: GS+0x4ED = (it was 0); the line names the new state, "OFF" while it is set)
        if (!b().settings.net_game) return;
        st.compression_off = !st.compression_off;
        if (c.network.set_compression_off) c.network.set_compression_off(st.compression_off);
        say(std::string("Ok.  Outgoing packet compression turned ") + (st.compression_off ? "OFF" : "ON"));
    });
    add("BPS", [this](const Words&) {
        st.bps = !st.bps;
        c.options_mut().bps = st.bps;
    });
    add("SFX", [this](const Words&) { b().fx_lists.spawning_off = !b().fx_lists.spawning_off; });

    // ---- cheats (bit 2): skirmish, and network games whose host allows them ----
    add("Radar", [this](const Words&) { c.options_mut().radar_everything = !c.options().radar_everything; });
    add("ATM", [this](const Words&) {
        // The viewed player gets 1000 energy and 1000 metal.
        sim::PlayerSlot& p = b().players[static_cast<std::size_t>(view()) % b().players.size()];
        p.energy_stored = static_cast<float>(static_cast<double>(p.energy_stored) - -1000.0);
        p.metal_stored = static_cast<float>(static_cast<double>(p.metal_stored) - -1000.0);
    });
    add("View", [this](const Words& a) {
        if (!playing(static_cast<std::uint32_t>(a.number(1, 0)))) return;
        const auto v = static_cast<std::uint8_t>(a.number(1, 0));
        c.options_mut().view = v;
        b().settings.local_view = v;
    });
    add("LOS", [this](const Words&) {
        b().settings.fog_of_war = !b().settings.fog_of_war;
        save();
        sight_view_changed();
        rebuild_sight(false);
    });
    add("Mapping", [this](const Words&) {
        b().settings.explore = !b().settings.explore;
        save();
        sight_view_changed();
        rebuild_sight(true);
    });
    add("DoubleShot", [this](const Words&) { b().settings.damage_options ^= 0x80; });
    add("HalfShot", [this](const Words&) { b().settings.damage_options ^= 0x100; });
    add("NowISee", [this](const Words&) {
        b().settings.explore = false;
        b().settings.fog_of_war = false;
        rebuild_sight(true);
    });
    add("Meteor", [this](const Words& a) {
        // No argument: a shower starts now; otherwise the map's shower switch.
        if (a.count() < 2) {
            sim::console::start_meteor_shower(b().meteors, b().game_tick(), b().map.width, b().map.height, b().crt_rand);
            return;
        }
        b().meteors.enabled = a.number(1, 0) != 0;
    });
    add("MakePoster", [this](const Words& a) {
        // 0x417600: "<w> <h>" (defaults 3200 x 2400) or "all" (the scroll extent); 0x495A30 draws
        // it with line of sight and mapping off (the sight rebuilt clearing the explored map before
        // and after: the explored map is lost), then both switches as they were.
        const int pw = a.count() > 1 ? a.number(1, 0) : 0xC80;
        const int ph = a.count() > 2 ? a.number(2, 0) : 0x960;
        const bool all = sim::console::c_stricmp(a.text(1), "all") == 0;
        sim::Battle& bt = b();
        const bool explore = bt.settings.explore, fog = bt.settings.fog_of_war;
        bt.settings.explore = false;
        bt.settings.fog_of_war = false;
        sight_view_changed();
        rebuild_sight(true);
        const std::string path = c.make_poster(pw, ph, all);
        bt.settings.explore = explore;
        bt.settings.fog_of_war = fog;
        sight_view_changed();
        rebuild_sight(true);
        std::printf("poster %s\n", path.empty() ? "(not written)" : path.c_str());
    });

    // ---- debug commands (bit 4): after "Now" unlocks them ----
    add("AI", [this](const Words& a) {
        const std::uint32_t p = static_cast<std::uint32_t>(a.number(1, local()));
        if (!playing(p)) return;
        sim::PlayerSlot& s = b().players[p & 0xFF];
        s.controller = s.controller == 1 ? 2 : 1;  // a local human becomes a computer player, anything else local
    });
    add("Control", [this](const Words& a) {
        if (!playing(static_cast<std::uint32_t>(a.number(1, 0)))) return;
        const auto p = static_cast<std::uint8_t>(a.number(1, 0));
        c.options_mut().player = p;
        c.options_mut().view = -1;
        b().settings.local_player = p;
        b().settings.local_view = p;
    });
    add("Kill", [this](const Words& a) {
        sim::Battle& bt = b();
        if (a.count() == 1) {
            // Every unit with a type is killed (kind 8) through the kill routine.
            for (std::size_t i = 1; i < bt.units.size(); ++i)
                if (bt.ticks[i].type_index != 0) sim::kill_unit(bt.combat(), bt.units[i], 8, st.kill_word);
        } else {
            const std::uint32_t slot = static_cast<std::uint32_t>(a.number(1, 0));
            if (slot < bt.players.size()) bt.kill_team(static_cast<int>(slot), st.kill_word);
        }
        // Both forms then switch the map's goals off (0x4904B0 on GS+0x391ED): a mission
        // never ends by them afterwards.
        bt.mission_goals.active = false;
    });
    auto end_game = [this](bool win) {
        // The local player's side picks the team killed: a win kills team (side != 1), a loss
        // team (side); then the end flags.
        const int side = c.options().side;
        const int team = win ? (side != 1 ? 1 : 0) : side;
        if (team >= 0 && static_cast<std::size_t>(team) < b().players.size()) b().kill_team(team, st.kill_word);
        if (win) b().end_game.flags |= sim::end_game_flags::kWon | sim::end_game_flags::kWonMovie | sim::end_game_flags::kEnding;
        else {
            b().end_game.flags &= static_cast<std::uint16_t>(~sim::end_game_flags::kWon);
            b().end_game.flags |= sim::end_game_flags::kLost | sim::end_game_flags::kEnding;
        }
    };
    add("IWin", [end_game](const Words&) { end_game(true); });
    add("ILose", [end_game](const Words&) { end_game(false); });
    add("Film", [this](const Words& a) {
        if (a.count() < 2) return;
        // (0x417330: word 1 becomes the image output folder, one trailing '\' or '/' dropped,
        // saved at once as "Image Output Directory")
        st.film_dir = a.text(1);
        if (!st.film_dir.empty() && (st.film_dir.back() == '\\' || st.film_dir.back() == '/')) st.film_dir.pop_back();
        c.options_mut().image_dir = st.film_dir;
        if (c.save_text_setting) c.save_text_setting("Image Output Directory", st.film_dir);
    });
    add("FilmSpeed", [this](const Words& a) {
        // (0x4173E0: a number above 0 becomes the film's frames a second, saved at once as
        // "Movie Output Rate")
        if (a.count() > 1 && a.number(1, 0) > 0) {
            st.film_speed = a.number(1, 0);
            c.options_mut().film_speed = st.film_speed;
            if (c.save_setting) c.save_setting("Movie Output Rate", st.film_speed);
        }
    });
    add("Assert", [](const Words&) {});
    add("Assign", [this](const Words& a) {
        // "+assign <mission> <a> <b>" (0x416310): the mission kind by name to the selected units
        // through the interface's order routine (0x48CF30) with no intent and no position; the
        // parameters are atol of words 1 and 2 (word 1 is the mission's name, so normally 0).
        // The unit under the pointer is the target when the kind's flags take one. The pointer
        // record's queued bit (Shift) is clear while the chat line has the keyboard.
        const std::uint8_t kind = mission_by_name(a.text(1));
        if (kind == 0) return;
        sim::Battle& bt = b();
        const std::uint16_t h = c.hover_unit();
        sim::Unit* hover = h != 0 && h < bt.units.size() ? &bt.units[h] : nullptr;
        sim::command::issue_to_selected(bt, local(), 0, kind, false, hover, nullptr, a.number(1, 0), a.number(2, 0));
    });
    add("BurnAll", [this](const Words&) {
        // 0x416390 -> 0x424840: every cell holding a feature (or a feature part) loses it, the
        // indestructible ones too (Feature_RemoveFromCell with force).
        sim::Battle& bt = b();
        sim::FeatureWorld& fw = bt.feature_world();
        const std::size_t n = static_cast<std::size_t>(bt.map.width) * static_cast<std::size_t>(bt.map.height);
        for (std::size_t i = 0; i < n && i < bt.map.cells.size(); ++i) {
            const std::uint16_t f = bt.map.cells[i].feature;
            if (f < 0xFFFB || f == 0xFFFE) sim::remove_feature(fw, static_cast<std::int32_t>(i), true);
        }
    });
    add("BurnOne", [this](const Words&) {
        // 0x4163A0: the feature under the pointer, if any, is removed (with force).
        const std::int32_t cell = pointer_cell();
        if (cell < 0) return;
        if (feature_under(cell) < 0xFFFB) sim::remove_feature(b().feature_world(), cell, true);
    });
    add("DebugBreak", [](const Words&) {});  // needs a debug build's second switch
    add("DPrint", [](const Words&) {});
    add("Edge", [this](const Words& a) {
        // (0x416730: the scroll extent = the map's size less word 1 (default 32) across and word 2
        // (default 128) down)
        b().set_scroll_edges(a.number(1, 0x20), a.number(2, 0x80));
    });
    add("Include", [this](const Words& a) { include_file(a.text(0), a); });
    add("Mem", [](const Words&) {});
    add("MemDump", [this](const Words&) {
        // (0x416240: memdump.txt made empty in the working folder; nothing is written into it)
        if (std::FILE* f = std::fopen((w.game_dir() + "\\memdump.txt").c_str(), "wb")) std::fclose(f);
    });
    add("Move", [this](const Words& a) {
        if (sim::console::c_stricmp(a.text(0), "move") != 0) return;
        cursor[0] = static_cast<std::int32_t>(static_cast<std::uint32_t>(cursor[0]) + static_cast<std::uint32_t>(a.number(1, 0)) * 0x100000u);
        cursor[2] = static_cast<std::int32_t>(static_cast<std::uint32_t>(cursor[2]) + static_cast<std::uint32_t>(a.number(2, 0)) * 0x100000u);
    });
    add("PrintWeights", [this](const Words& a) { print_weights(a); });
    add("Profile", [this](const Words&) {
        st.profile = !st.profile;
        c.options_mut().profile = st.profile;
    });
    add("Reload", [this](const Words& a) {
        if (a.count() > 1) reload_type(a.text(1));
    });
    add("ReloadAIProfiles", [this](const Words&) {
        // 0x4174D0 -> 0x40A100: every active computer player's table (by slot) back to the
        // profile defaults (Ai_ResetProfile 0x409470), then the battle's profile load again
        // (0x4648E0: the map's profile or ai\default.txt, then the units' own lines).
        sim::Battle& bt = b();
        sim::Ai& ai = bt.ai();
        for (int s = 0; s < 10 && static_cast<std::size_t>(s) < bt.players.size(); ++s) {
            const sim::PlayerSlot& p = bt.players[static_cast<std::size_t>(s)];
            if (!p.active || p.controller != 2) continue;
            if (!bt.ai_state.tables[static_cast<std::size_t>(s)].exists) {
                if (ai.fault.empty()) ai.fault = "computer player without a unit table (profile reset)";
                continue;
            }
            sim::ai_reset_profile(ai, s);
        }
        LoadedBattle& lb = w.loaded();
        AiProfileReport report;
        load_ai_profile(w.vfs_, ai_profile_path(lb.ota, lb.schema < 0 ? 0 : lb.schema), lb.unit_defs, ai, report);
    });
    add("Save", [this](const Words& a) {
        // (0x417430: with a name, savegame\<name>.sav, the folder made first)
        if (a.count() > 1 && c.save_game_command) c.save_game_command(a.text(1));
    });
    add("SeaLevel", [this](const Words& a) { b().settings.sea_level = static_cast<std::uint8_t>(a.number(1, 0)); });
    add("Search", [this](const Words& a) {
        // 0x416780: the route search's work divisor (+0x48) = word 1 when nonzero; with
        // exactly three words its base estimate scale (+0x54) = ftol(atof(word 2)).
        sim::RouteSearch& rs = b().route_search;
        if (a.number(1, 0) != 0) rs.budget_divisor = a.number(1, 0);
        if (a.count() == 3) rs.base_budget = sim::x87::ftol32(crt_atof(a.text(2)));
    });
    add("SelBoxes", [this](const Words&) { c.options_mut().selection_boxes = !c.options().selection_boxes; });
    add("Senderror", [this](const Words& a) {
        if (a.count() != 2) return;
        const std::int32_t v = a.number(1, 0);
        st.senderror = v < 0 || v > 100 ? 0 : v;
    });
    add("TreeDeath", [this](const Words&) { b().settings.damage_options ^= 8; });
    add("Feature", [this](const Words& a) {
        // 0x4163D0: the first feature type of this battle's table named word 1 (stricmp,
        // 0x422DD0) is placed at the pointer's cell, owner nibble 10.
        const int type = w.loaded().feature_defs.find(a.text(1));
        if (type < 0 || type >= 0xFFFF) return;
        const std::int32_t cell = pointer_cell();
        if (cell < 0) return;
        sim::place_feature(b().feature_world(), cell, static_cast<std::uint16_t>(type), nullptr, nullptr, 10);
    });
    add("ZBuffer", [](const Words&) {});
    // The computer player's profile commands (bits 8: only text run with every bit allowed).
    auto profile = [this](const Words& a) {
        std::string line;
        for (int k = 0; k < a.count(); ++k) line += (k ? " " : "") + a.text(k);
        AiProfileReport rep;
        run_ai_commands(line, w.vfs_, w.defs(), b().ai(), rep);
    };
    add("plan", profile);
    add("weight", profile);
    add("limit", profile);
    reg.set_default([this](const Words& a) { default_command(a); }, cb::kDebug);
}

ConsoleCommands::ConsoleCommands(client::GameClient& client, ClientWorld& world) : impl_(std::make_unique<Impl>(client, world)) {}
ConsoleCommands::~ConsoleCommands() = default;

std::uint32_t ConsoleCommands::run(const std::string& line, std::uint32_t allowed) {
    // The line's words (0x4B7440) go to the registry (0x4B7900) with the pointer's map point as
    // it stands now.
    impl_->cursor = [&] {
        const sim::Vec3i p = impl_->c.cursor_world();
        return std::array<std::int32_t, 3>{p[0], p[1], p[2]};
    }();
    const Words words = sim::console::split(line);
    if (words.endless) return 0;  // the original never finishes such a line
    return impl_->reg.dispatch(words, allowed);
}

ConsoleState& ConsoleCommands::state() { return impl_->st; }
const sim::console::Registry& ConsoleCommands::registry() const { return impl_->reg; }

std::shared_ptr<ConsoleCommands> install_console_commands(client::GameClient& client, ClientWorld& world) {
    auto cmds = std::make_shared<ConsoleCommands>(client, world);
    client.on_command = [cmds](const std::string& line, std::uint32_t allowed) { return cmds->run(line, allowed); };
    return cmds;
}

}  // namespace ta
