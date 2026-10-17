// The original's typed commands (docs/RE_MEMORY.md, "Console commands"): the command table
// (names, permission bits, effects) on top of the engine's registry (sim/console.h).

#include "ta/console_commands.h"

#include <algorithm>

#include "client/game_client.h"
#include "sim/battle.h"
#include "sim/combat.h"
#include "sim/construction.h"
#include "sim/economy.h"
#include "sim/victory.h"
#include "sim/x87.h"
#include "ta/ai_profile.h"
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

    // The share switches of the local player's side record (network games): flip, announce.
    void toggle_share(std::uint8_t bit, const char* what) {
        if (!b().settings.net_game) return;
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
        const std::int32_t row_end = (bt.map.width * 16 - 32) * 0x10000;
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

    void include_file(const std::string& name, const Words& parent) {
        std::vector<std::uint8_t> text;
        if (!w.vfs_.read("debugdat\\" + name + ".txt", text)) return;
        const auto saved = cursor;
        reg.run_text(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), parent, 0xFFFFFFFFu);
        cursor = saved;
    }

    void build();
};

void ConsoleCommands::Impl::build() {
    auto add = [this](const char* name, sim::console::Handler h) { reg.add(name, std::move(h), console_table_bits(name)); };
    
    // ---- options (bit 1): always allowed ----
    add("NoShake", [this](const Words&) { b().settings.shake_disabled = !b().settings.shake_disabled; });
    add("Contour", [this](const Words& a) {
        st.contour[0] = sim::x87::ftol32(a.count() > 1 ? crt_atof(a.text(1)) : 0.0);
        st.contour[1] = sim::x87::ftol32(a.count() > 2 ? crt_atof(a.text(2)) : static_cast<double>(0.75f));
        not_built("Contour");
    });
    add("ScrollSpeed", [this](const Words& a) { c.options_mut().scroll_speed = static_cast<std::uint8_t>(a.number(1, 0)); });
    add("IFace", [this](const Words& a) { c.options_mut().right_click_orders = a.number(1, 0) == 1; });
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
    add("CDPlay", [this](const Words&) { not_built("CDPlay"); });
    add("CDStop", [this](const Words&) { not_built("CDStop"); });
    add("Sound3D", [this](const Words&) {
        auto& o = c.options_mut();
        o.sound_mode = o.sound_mode == 2 ? 1 : 2;
    });
    add("Shading", [this](const Words&) { c.options_mut().shading = !c.options().shading; });
    add("AntiAlias", [this](const Words&) { c.options_mut().antialias = !c.options().antialias; });
    add("Shadow", [this](const Words&) { c.options_mut().shadows = !c.options().shadows; });
    add("Dither", [this](const Words&) { c.options_mut().dithered_fog = !c.options().dithered_fog; });
    add("SwitchAlt", [this](const Words& a) {
        auto& o = c.options_mut();
        o.alt_groups = a.count() < 2 ? !o.alt_groups : (a.number(1, 0) & 1) != 0;
    });
    add("TShadow", [this](const Words&) { c.options_mut().unit_shadows = !c.options().unit_shadows; });
    add("FShadow", [this](const Words&) { c.options_mut().feature_shadows = !c.options().feature_shadows; });
    add("LOSType", [this](const Words&) {
        b().settings.true_sight = !b().settings.true_sight;
        rebuild_sight(false);
    });
    add("Light", [this](const Words& a) {
        st.light[0] = a.number(1, 0);
        st.light[1] = a.number(2, 0);
        st.light[2] = a.number(3, 0);
        not_built("Light");
    });
    add("RCache", [](const Words&) {});  // the original rebuilds its drawing caches
    add("Selectable", [this](const Words&) {
        // Every live unit becomes selectable (flag 0x20).
        for (std::size_t i = 1; i < b().units.size(); ++i)
            if (b().units[i].flags & sim::unit_flags::kAlive) b().units[i].flags |= sim::unit_tick_flags::kSelectable;
    });
    add("MusicMode", [this](const Words&) { not_built("MusicMode"); });
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
    });
    add("Gamma", [this](const Words& a) {
        st.gamma = a.number(1, 0);
        not_built("Gamma");
    });
    add("Clock", [this](const Words&) { c.options_mut().clock = !c.options().clock; });
    add("NetStats", [](const Words&) {});  // resets the network statistics display
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
    add("BigBrother", [this](const Words&) { st.big_brother = !st.big_brother; });
    add("Now", [this](const Words& a) {
        // Unlocks the debug commands with exactly these five words (compared case-sensitively);
        // any other "Now" line locks them again.
        c.options_mut().debug_commands = a.count() == 6 && a.text(1) == "Film" && a.text(2) == "Chris" && a.text(3) == "Include" &&
                                        a.text(4) == "Reload" && a.text(5) == "Assert";
    });
    add("Drop", [this](const Words& a) { st.net_drop_off = a.number(1, 0) == 0; });
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
        if (!b().settings.net_game) return;
        st.compression = !st.compression;
        say(std::string("Ok.  Outgoing packet compression turned ") + (st.compression ? "ON" : "OFF"));
    });
    add("BPS", [this](const Words&) { st.bps = !st.bps; });
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
        sight_view_changed();
        rebuild_sight(false);
    });
    add("Mapping", [this](const Words&) {
        b().settings.explore = !b().settings.explore;
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
    add("MakePoster", [this](const Words&) { not_built("MakePoster"); });

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
            return;
        }
        const std::uint32_t slot = static_cast<std::uint32_t>(a.number(1, 0));
        if (slot < bt.players.size()) bt.kill_team(static_cast<int>(slot), st.kill_word);
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
        st.film_dir = a.text(1);
        if (!st.film_dir.empty() && (st.film_dir.back() == '\\' || st.film_dir.back() == '/')) st.film_dir.pop_back();
    });
    add("FilmSpeed", [this](const Words& a) {
        if (a.count() > 1 && a.number(1, 0) > 0) st.film_speed = a.number(1, 0);
    });
    add("Assert", [](const Words&) {});
    add("Assign", [this](const Words&) { not_built("Assign"); });
    add("BurnAll", [this](const Words&) { not_built("BurnAll"); });
    add("BurnOne", [this](const Words&) { not_built("BurnOne"); });
    add("DebugBreak", [](const Words&) {});  // needs a debug build's second switch
    add("DPrint", [](const Words&) {});
    add("Edge", [this](const Words&) { not_built("Edge"); });
    add("Include", [this](const Words& a) { include_file(a.text(0), a); });
    add("Mem", [](const Words&) {});
    add("MemDump", [](const Words&) {});
    add("Move", [this](const Words& a) {
        if (sim::console::c_stricmp(a.text(0), "move") != 0) return;
        cursor[0] = static_cast<std::int32_t>(static_cast<std::uint32_t>(cursor[0]) + static_cast<std::uint32_t>(a.number(1, 0)) * 0x100000u);
        cursor[2] = static_cast<std::int32_t>(static_cast<std::uint32_t>(cursor[2]) + static_cast<std::uint32_t>(a.number(2, 0)) * 0x100000u);
    });
    add("PrintWeights", [](const Words&) {});  // writes a file
    add("Profile", [this](const Words&) { st.profile = !st.profile; });
    add("Reload", [this](const Words&) { not_built("Reload"); });
    add("ReloadAIProfiles", [this](const Words&) { not_built("ReloadAIProfiles"); });
    add("Save", [this](const Words&) { not_built("Save"); });
    add("SeaLevel", [this](const Words& a) { b().settings.sea_level = static_cast<std::uint8_t>(a.number(1, 0)); });
    add("Search", [this](const Words&) { not_built("Search"); });
    add("SelBoxes", [this](const Words&) { c.options_mut().selection_boxes = !c.options().selection_boxes; });
    add("Senderror", [this](const Words& a) {
        if (a.count() != 2) return;
        const std::int32_t v = a.number(1, 0);
        st.senderror = v < 0 || v > 100 ? 0 : v;
    });
    add("TreeDeath", [this](const Words&) { b().settings.damage_options ^= 8; });
    add("Feature", [this](const Words&) { not_built("Feature"); });
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
