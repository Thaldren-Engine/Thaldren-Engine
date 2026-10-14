// Offline battle view: loads a map and a few units of real types from an
// installed copy of the original game, runs their scripts for a while and
// writes 640x480 8-bit BMP frames of the battle view.
//
// Usage: battle_view <game_dir> <out_prefix> [--gpu] [--window S] [--map NAME] [--scroll X Y] [--frames N] [--step K] [--battle]
//                   [--player N [--true-los]]
//   --player N with --battle: line of sight for player N: fog of war (unexplored black, unseen
//              grey), only player N's units and what it sees drawn, shots in the fog hidden,
//              cloaked units translucent. Default: no fog, everything drawn.
//   --gpu      also render through the GPU backend (if built) and compare with the CPU reference.
//   --window S afterwards show the view live in a window for S seconds (GPU backend).
//   --battle   run a real battle instead of animating placed units: two players' units made
//              by the engine's unit creation, every step run by sim::Battle; units find their
//              targets themselves (no line of sight yet: everything is in sight) and route
//              with the route search. The map's features are the battle's: they burn, break
//              and reproduce; dead units leave wrecks, and explosions, debris and shards are drawn.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "render/battle_view.h"
#include "render/presenter.h"
#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/rng.h"
#include "sim/unit.h"
#include "ta/battle_features.h"
#include "ta/cob.h"
#include "ta/featuredefs.h"
#include "ta/battle_types.h"
#include "ta/sight_tables.h"
#include "ta/gaf.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/render_assets.h"
#include "ta/tnt.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"

#ifdef RENDER_HAVE_GPU
#include <SDL3/SDL_timer.h>

#include "render/gpu_renderer.h"
#endif

namespace {

struct World : sim::UnitWorld {
    std::vector<std::unique_ptr<sim::Unit>>* units = nullptr;
    const sim::HeightMap* hm = nullptr;
    sim::BattleRng rng;
    sim::Unit* unit_by_id(std::uint16_t id) override {
        for (auto& u : *units)
            if (u->id == id) return u.get();
        return nullptr;
    }
    const sim::HeightMap& heightmap() override { return *hm; }
    bool yard_cells_free(sim::Unit&, std::int32_t) override { return true; }
    void claim_yard_cells(sim::Unit&) override {}
    void on_activate_flags_changed(sim::Unit&, std::uint8_t, std::uint8_t) override {}
    std::uint32_t random_mod(std::uint32_t n) override { return rng.next_mod(n); }
    void emit_sfx(sim::Unit&, std::int32_t, std::int32_t) override {}
    void explode_piece(sim::Unit&, std::int32_t, std::int32_t) override {}
    void attach_unit(sim::Unit&, std::int32_t, std::int32_t, std::int32_t) override {}
    void drop_unit(sim::Unit&, std::int32_t) override {}
};

struct UnitType {
    const ta::UnitDefRecord* def = nullptr;
    std::vector<std::uint8_t> model_file;
    std::unique_ptr<ta::Model3doPiece> root;
    sim::PoseModel pose_model;
    render::Model model;
    ta::CobFile cob;
    sim::UnitTypeInfo info;
};

struct Placement {
    const char* name;
    int x, z;            // world units
    std::uint16_t heading;
    std::uint8_t colour; // team colour index
    const char* script;  // extra script function to start ("" = none)
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: battle_view <game_dir> <out_prefix> [--gpu] [--map NAME] [--scroll X Y] [--frames N] [--step K]\n");
        return 2;
    }
    const std::string game = argv[1], prefix = argv[2];
    std::string map_name = "Moon Quartet";
    int scroll_x = 1200, scroll_y = 1600, frames = 3, step = 15;
    bool use_gpu = false;
    bool battle_mode = false;
    int view_player = -1;   // --player N: fog of war, line of sight and radar for player N
    bool true_los = false;  // --true-los: true line of sight (else circular)
    int window_seconds = 0;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gpu") use_gpu = true;
        else if (a == "--window" && i + 1 < argc) window_seconds = std::atoi(argv[++i]);
        else if (a == "--map" && i + 1 < argc) map_name = argv[++i];
        else if (a == "--scroll" && i + 2 < argc) {
            scroll_x = std::atoi(argv[++i]);
            scroll_y = std::atoi(argv[++i]);
        } else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--step" && i + 1 < argc) step = std::atoi(argv[++i]);
        else if (a == "--battle") battle_mode = true;
        else if (a == "--player" && i + 1 < argc) view_player = std::atoi(argv[++i]);
        else if (a == "--true-los") true_los = true;
    }

    ta::Vfs vfs;
    vfs.mount_game_dir(game);
    render::Palette palette{};
    render::BlendTable blend;
    if (!ta::load_palette(vfs, palette) || !ta::load_blend_table(vfs, blend)) {
        std::fprintf(stderr, "cannot load the palette / blend table\n");
        return 1;
    }

    // Map: tiles, heights, features.
    std::vector<std::uint8_t> tnt;
    if (!vfs.read("maps\\" + map_name + ".tnt", tnt)) {
        std::fprintf(stderr, "map not found: %s\n", map_name.c_str());
        return 1;
    }
    ta::TntTiles tiles;
    sim::HeightMap hm;
    ta::TntInfo info;
    if (!ta::tnt_load_tiles(tnt, tiles) || !ta::tnt_load_heights(tnt, hm, &info)) {
        std::fprintf(stderr, "bad map file\n");
        return 1;
    }
    std::printf("map %s: %dx%d tiles, %zu tile graphics\n", map_name.c_str(), tiles.width, tiles.height, tiles.tiles.size());

    const ta::WeaponDefTable weapons = ta::load_weapon_defs(vfs, ta::WeaponDefOptions{});
    std::array<std::string, 256> weapon_names;
    for (std::size_t i = 0; i < 256; ++i) weapon_names[i] = weapons.slots[i].name;
    ta::FeatureDefTable fdefs;
    fdefs.load_library(vfs, weapon_names);
    std::vector<std::string> tnt_names;
    ta::tnt_feature_names(tnt, tnt_names);
    fdefs.load_map_features(tnt_names);

    // Feature images (frame 0 of the sequence and of its shadow).
    struct FeatureLook {
        std::vector<render::Sprite8> image, shadow;
    };
    std::map<int, FeatureLook> looks;
    std::map<std::string, std::vector<std::uint8_t>> gaf_files;
    auto feature_look = [&](int index) -> FeatureLook* {
        auto it = looks.find(index);
        if (it != looks.end()) return &it->second;
        FeatureLook& lk = looks[index];
        const auto& recs = fdefs.records();
        if (index < 0 || static_cast<std::size_t>(index) >= recs.size()) return &lk;
        const auto& r = recs[static_cast<std::size_t>(index)];
        if (!(r.flags & ta::feature_flags::kGaf) || r.gaf_owner < 0) return &lk;
        const std::string file = recs[static_cast<std::size_t>(r.gaf_owner)].gaf_file;
        auto& data = gaf_files[file];
        if (data.empty()) vfs.read("anims\\" + file + ".GAF", data);
        ta::GafDirectory dir;
        if (!ta::gaf_read_directory(data, dir)) return &lk;
        const int s = ta::gaf_find_sequence(dir, r.sequences[ta::kSeq]);
        if (!r.sequences[ta::kSeq].empty() && s >= 0) ta::gaf_sequence_frames(data, s, lk.image);
        const int sh = ta::gaf_find_sequence(dir, r.sequences[ta::kSeqShadow]);
        if (!r.sequences[ta::kSeqShadow].empty() && sh >= 0) ta::gaf_sequence_frames(data, sh, lk.shadow);
        return &lk;
    };
    std::vector<ta::TntFeatureCell> cells;
    ta::tnt_feature_cells(tnt, cells);

    // Units.
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    // With the feature table, as battle loading does: unit corpses are looked up (and appended),
    // then the dead / burnt / reclaimed chains are resolved.
    const ta::UnitDefTable defs = ta::load_unit_defs(vfs, ctx, &fdefs);
    fdefs.resolve_chains();
    ta::TextureLibrary textures;
    textures.load(vfs);
    std::map<std::string, std::unique_ptr<UnitType>> types;
    auto type_of = [&](const std::string& name) -> UnitType* {
        auto it = types.find(name);
        if (it != types.end()) return it->second.get();
        for (const auto& d : defs.units) {
            if (ta::stricmp_c(d.unit_name, name) != 0) continue;
            auto t = std::make_unique<UnitType>();
            t->def = &d;
            std::vector<std::uint8_t> cob;
            std::string err;
            if (!vfs.read("objects3d\\" + d.object_name + ".3DO", t->model_file) || !vfs.read("scripts\\" + d.unit_name + ".COB", cob))
                return nullptr;
            auto m = ta::load_3do(t->model_file, true);
            if (!m.root || !ta::load_cob(cob, t->cob, err)) return nullptr;
            t->root = std::move(m.root);
            t->pose_model = ta::to_pose_model(t->root.get());
            t->model = ta::build_render_model(t->root.get(), textures);
            t->info.max_damage = d.max_damage ? d.max_damage : 1;
            t->info.model_max_y = d.model_max_y;
            return (types[name] = std::move(t)).get();
        }
        return nullptr;
    };

    const Placement placements[] = {
        {"ARMCOM", 1420, 1800, 0x0000, 0, "StartMoving"},
        {"CORCOM", 1500, 1800, 0x8000, 1, ""},
        {"ARMSOLAR", 1340, 1880, 0x0000, 0, "Activate"},
        {"ARMLLT", 1560, 1880, 0x4000, 0, ""},
        {"CORAK", 1460, 1940, 0xC000, 1, "StartMoving"},
        {"ARMSTUMP", 1620, 1780, 0x2000, 0, ""},
        {"CORRAID", 1380, 1740, 0x6000, 1, ""},
        {"ARMMEX", 1640, 1980, 0x0000, 0, "Activate"},
        {"ARMBRAWL", 1520, 1700, 0x3000, 0, "Activate"},
    };
    std::vector<std::unique_ptr<sim::Unit>> units;
    std::vector<std::unique_ptr<sim::UnitScriptHost>> hosts;
    std::vector<std::pair<sim::Unit*, std::pair<UnitType*, std::uint8_t>>> placed;
    World world;
    world.units = &units;
    world.hm = &hm;
    world.rng.seed(0x12345678u);
    std::uint16_t next_id = 1;
    for (const Placement& p : placements) {
        if (battle_mode) break;
        UnitType* t = type_of(p.name);
        if (!t) {
            std::printf("unit type %s unavailable\n", p.name);
            continue;
        }
        auto u = std::make_unique<sim::Unit>();
        u->id = next_id++;
        u->type = &t->info;
        u->flags = sim::unit_flags::kAlive;
        u->hit_points = static_cast<std::int16_t>(t->info.max_damage);
        const std::int32_t gy = hm.sample(static_cast<std::int16_t>(p.x), static_cast<std::int16_t>(p.z));
        const bool air = (t->def->type_flags & (1u << 11)) != 0;
        u->pos = {p.x << 16, (std::max(gy, 0) + (air ? 60 : 0)) << 16, p.z << 16};
        u->heading = p.heading;
        u->pose.build(t->pose_model, t->cob.program.piece_names);
        hosts.push_back(std::make_unique<sim::UnitScriptHost>(*u, world));
        u->script = std::make_unique<sim::ScriptInstance>(t->cob.program, *hosts.back(), 30);
        u->script->set_step_limit(5000000);
        placed.push_back({u.get(), {t, p.colour}});
        units.push_back(std::move(u));
    }
    for (std::size_t i = 0; i < placed.size(); ++i) {
        sim::Unit* u = placed[i].first;
        u->script->call("Create", nullptr, true);
        const char* extra = placements[i].script;
        if (*extra) u->script->call(extra, nullptr, true);
    }

    // Battle mode: types converted from the definitions, units made by the engine's creation routine.
    struct BattleType {
        const ta::UnitDefRecord* def = nullptr;
        ta::UnitTypeAssets assets;
        std::unique_ptr<ta::Model3doPiece> root;
        render::Model model;
    };
    std::vector<std::unique_ptr<BattleType>> btypes(1);  // index = battle type index (0 = blank)
    // Behaviours not built yet (movement, chasing) wait 30 steps at a time instead of looping.
    struct ViewOutside : sim::QuietBattleOutside {
        const sim::Battle* battle = nullptr;
        std::int32_t run_mission(sim::Unit&, sim::MissionOrder& order, std::uint32_t&) override {
            order.wait |= sim::order_wait::kTimer;
            order.wake_tick = battle->game_tick() + 30;
            return sim::mission_result::kContinue;
        }
    } outside;
    sim::BattleRng battle_rng;
    battle_rng.seed(0x12345678u);
    std::vector<sim::WeaponType> weapon_types;
    std::unique_ptr<sim::Battle> battle;
    std::unique_ptr<ta::AnimImporter> anim_import;
    std::vector<std::string> feature_models;  // 3D feature model names (FeatureTypeInfo::model - 1)
    if (battle_mode) {
        weapon_types = ta::to_weapon_types(weapons);
        battle = std::make_unique<sim::Battle>(outside, hm, battle_rng);
        outside.battle = battle.get();
        sim::Battle& b = *battle;
        anim_import = std::make_unique<ta::AnimImporter>(vfs, b.anims);
        ta::bind_weapon_explosions(weapons, weapon_types, *anim_import);  // before anything points into weapon_types
        b.features = ta::to_feature_types(fdefs, weapon_types, *anim_import, &feature_models);
        ta::load_effect_art(*anim_import, b.effect_art);
        b.settings.sea_level = static_cast<std::uint8_t>(info.sea_level);
        b.settings.gravity = 112;
        b.settings.damage_options = 8;  // weapons damage features
        b.init_unit_table(101);
        for (int p = 0; p < 2; ++p) {
            sim::PlayerSlot& s = b.players[static_cast<std::size_t>(p)];
            s.active = true;
            s.controller = static_cast<std::uint8_t>(p == 0 ? 1 : 2);
            s.net_id = p + 1;
            s.state = static_cast<std::uint8_t>(p);
            b.alliances[static_cast<std::size_t>(p)][static_cast<std::size_t>(p)] = 1;  // allied with itself only
            s.energy_stored = 1.0e6f;
            s.metal_stored = 1.0e6f;
            b.assign_unit_range(p, 1 + 50 * p, 50 + 50 * p);
        }
        b.map.width = hm.width();
        b.map.height = hm.height();
        b.map.cells.assign(static_cast<std::size_t>(b.map.width) * static_cast<std::size_t>(b.map.height), sim::MapCell{});
        for (std::int32_t z = 0; z < b.map.height; ++z)
            for (std::int32_t x = 0; x < b.map.width; ++x)
                b.map.cells[static_cast<std::size_t>(z * b.map.width + x)].height = hm.at(x, z);
        sim::refresh_height_range(b.map, 0, 0, static_cast<std::int16_t>(b.map.width), static_cast<std::int16_t>(b.map.height));
        b.init_buckets();
        b.reset_features_and_effects();
        ta::place_tnt_features(b, tnt);
        b.settings.player_count = 2;
        b.settings.units_per_player = 50;
        b.move_classes = ta::to_move_classes(ctx);
        if (view_player >= 0) {
            // Line of sight: fog, the map explored by the units, the shapes from the game files.
            b.settings.fog_of_war = true;
            b.settings.explore = true;
            b.settings.true_sight = true_los;
            b.settings.local_view = static_cast<std::uint8_t>(view_player);
            std::string err;
            if (!ta::load_sight_tables(vfs, b.sight_tables, err)) std::printf("sight tables: %s\n", err.c_str());
            b.init_sight();
        }
        b.init_routes();
        auto battle_type = [&](const std::string& name) -> std::uint16_t {
            for (std::size_t k = 1; k < btypes.size(); ++k)
                if (ta::stricmp_c(btypes[k]->def->unit_name, name) == 0) return static_cast<std::uint16_t>(k);
            for (const auto& d : defs.units) {
                if (ta::stricmp_c(d.unit_name, name) != 0) continue;
                auto t = std::make_unique<BattleType>();
                t->def = &d;
                std::string err;
                if (!ta::load_unit_assets(vfs, d, t->assets, err)) return 0;
                auto m = ta::load_3do(t->assets.model_file, true);
                if (!m.root) return 0;
                t->root = std::move(m.root);
                t->model = ta::build_render_model(t->root.get(), textures);
                ta::bind_pose_primitives(t->assets.pose_model, t->model);
                b.types.emplace_back();
                ta::to_battle_unit_type(d, t->assets, weapon_types, b.types.back());
                ta::to_targeting_type(d, defs.categories, weapon_types, b.types.back().targeting);
                btypes.push_back(std::move(t));
                return static_cast<std::uint16_t>(btypes.size() - 1);
            }
            return 0;
        };
        struct Spawn {
            const char* name;
            int player, x, z;
        };
        const Spawn spawns[] = {
            {"ARMCOM", 0, 1380, 1760}, {"ARMLLT", 0, 1460, 1720}, {"ARMLLT", 0, 1560, 1720}, {"ARMSTUMP", 0, 1340, 1680},
            {"ARMHAM", 0, 1620, 1760}, {"ARMSOLAR", 0, 1300, 1800},
            {"CORCOM", 1, 1520, 1980}, {"CORLLT", 1, 1420, 2000}, {"CORAK", 1, 1580, 1940}, {"CORRAID", 1, 1680, 1980},
            {"CORSTORM", 1, 1460, 1940},
        };
        for (const Spawn& s : spawns) battle_type(s.name);  // all types first: the type table must not grow under live units
        for (const Spawn& s : spawns) {
            const std::uint16_t type = battle_type(s.name);
            if (!type) {
                std::printf("unit type %s unavailable\n", s.name);
                continue;
            }
            const std::int32_t gy = std::max(hm.sample(static_cast<std::int16_t>(s.x), static_cast<std::int16_t>(s.z)), 0);
            if (!b.create_unit(s.player, type, s.x << 16, gy << 16, s.z << 16, true, 1))
                std::printf("could not create %s\n", s.name);
        }
        b.start_players();  // the players' weapon-target controllers and enemy lists
        if (view_player >= 0) b.rebuild_sight(true);  // the battle start
        // Routing: the raider drives over to the far side of the other camp (route search + motion).
        for (std::size_t i = 1; i < b.units.size(); ++i) {
            if (b.ticks[i].type_index == 0 || !b.ticks[i].has_motion || b.ticks[i].type_index >= btypes.size()) continue;
            if (ta::stricmp_c(btypes[b.ticks[i].type_index]->def->unit_name, "CORRAID") != 0) continue;
            const std::int32_t to[3] = {1250 << 16, 0, 1640 << 16};
            sim::enqueue_order(b.order_context(), b.units[i], sim::mission::kMoveGround, false, nullptr, to, 0, 0);
        }
    }

    render::BattleScene scene;
    scene.camera.scroll_x = scroll_x;
    scene.camera.scroll_y = scroll_y;
    scene.terrain.width = tiles.width;
    scene.terrain.height = tiles.height;
    scene.terrain.map = &tiles.map;
    scene.terrain.tiles = &tiles.tiles;
    scene.heights = &hm;
    for (const auto& c : cells) {
        if (battle_mode) break;  // the battle draws its own features
        if (c.feature >= fdefs.records().size()) continue;
        const auto& r = fdefs.records()[c.feature];
        FeatureLook* lk = feature_look(c.feature);
        render::FeatureInstance f;
        f.cell_x = c.x;
        f.cell_z = c.z;
        f.footprint_x = r.footprint_x;
        f.footprint_z = r.footprint_z;
        f.height = r.height;
        f.image = lk->image.empty() ? nullptr : &lk->image[0];
        f.shadow = lk->shadow.empty() ? nullptr : &lk->shadow[0];
        f.image_blended = (r.flags & ta::feature_flags::kAnimTrans) != 0;
        f.shadow_blended = (r.flags & ta::feature_flags::kShadTrans) != 0;
        if (f.image || f.shadow) scene.features.push_back(f);
    }

#ifdef RENDER_HAVE_GPU
    std::unique_ptr<render::GpuRenderer> gpu;
    if (use_gpu) {
        gpu = render::GpuRenderer::create(blend);
        if (!gpu) std::printf("GPU backend unavailable: %s\n", render::GpuRenderer::last_error().c_str());
    }
#else
    if (use_gpu) std::printf("built without the GPU backend\n");
#endif

    // Battle-mode drawing data: decoded frames by sequence, 3D feature models, glows.
    std::map<std::uint32_t, std::vector<render::Sprite8>> seq_frames;
    auto frames_of = [&](std::uint32_t seq) -> const std::vector<render::Sprite8>* {
        if (!seq || !anim_import) return nullptr;
        auto it = seq_frames.find(seq);
        if (it != seq_frames.end()) return &it->second;
        std::vector<render::Sprite8>& v = seq_frames[seq];
        const auto o = anim_import->origins().find(seq);
        if (o != anim_import->origins().end())
            if (const std::vector<std::uint8_t>* data = anim_import->file(o->second.first)) ta::gaf_sequence_frames(*data, o->second.second, v);
        return &v;
    };
    auto frame_at = [&](const sim::AnimCursor& c) -> const render::Sprite8* {
        const std::vector<render::Sprite8>* f = frames_of(c.seq);
        return f && c.frame < f->size() ? &(*f)[c.frame] : nullptr;
    };
    struct FeatureModel {
        std::vector<std::uint8_t> file;
        std::unique_ptr<ta::Model3doPiece> root;
        sim::PoseModel pose;
        render::Model model;
        bool ok = false;
    };
    std::map<std::uint32_t, FeatureModel> feature_model_cache;
    auto feature_model = [&](std::uint32_t id) -> const FeatureModel* {
        if (id == 0 || id > feature_models.size()) return nullptr;
        FeatureModel& fm = feature_model_cache[id];
        if (!fm.ok && fm.file.empty() && vfs.read("objects3d\\" + feature_models[id - 1] + ".3DO", fm.file)) {
            auto m = ta::load_3do(fm.file, true);
            if (m.root) {
                fm.root = std::move(m.root);
                fm.pose = ta::to_pose_model(fm.root.get());
                fm.model = ta::build_render_model(fm.root.get(), textures);
                fm.ok = true;
            }
        }
        return fm.ok ? &fm : nullptr;
    };
    std::array<std::vector<render::Sprite8>, 3> glows;
    for (int k = 0; k < 3; ++k) glows[static_cast<std::size_t>(k)] = render::glow_frames(k, 0xC0FFEEu + static_cast<std::uint32_t>(k));
    render::BlendTable light_table;
    {
        std::vector<std::uint8_t> lht;
        if (vfs.read("palettes\\PALETTE.LHT", lht)) light_table = render::light_blend_table(lht);
    }
    render::FogOfWar fog;
    std::array<std::vector<render::Sprite8>, 4> fog_black, fog_gray;
    render::BlendTable gray_table;
    if (battle && view_player >= 0) {
        std::vector<std::uint8_t> gaf;
        ta::GafDirectory dir;
        if (vfs.read("anims\\fog.gaf", gaf) && ta::gaf_read_directory(gaf, dir))
            for (int k = 0; k < 4; ++k) {
                ta::gaf_sequence_frames(gaf, ta::gaf_find_sequence(dir, "Black" + std::to_string(k + 1)), fog_black[static_cast<std::size_t>(k)]);
                ta::gaf_sequence_frames(gaf, ta::gaf_find_sequence(dir, "Gray" + std::to_string(k + 1)), fog_gray[static_cast<std::size_t>(k)]);
                fog.black[static_cast<std::size_t>(k)] = &fog_black[static_cast<std::size_t>(k)];
                fog.gray[static_cast<std::size_t>(k)] = &fog_gray[static_cast<std::size_t>(k)];
            }
        gray_table = render::gray_remap_table(palette);
        fog.gray_table = &gray_table;
    }
    std::map<const sim::PoseModel*, const render::Model*> model_of_pose;
    for (const auto& bt : btypes)
        if (bt) model_of_pose[&bt->assets.pose_model] = &bt->model;

    auto build_frame = [&](std::uint32_t at_tick) {
        scene.units.clear();
        if (battle) {
            sim::Battle& b = *battle;
            // Features as the battle holds them.
            scene.features.clear();
            for (std::int32_t z = 0; z < b.map.height; ++z)
                for (std::int32_t x = 0; x < b.map.width; ++x) {
                    const sim::MapCell& cell = b.map.cells[static_cast<std::size_t>(z * b.map.width + x)];
                    if (cell.feature >= sim::kFirstSpecialFeature || cell.feature >= b.features.size()) continue;
                    const sim::FeatureTypeInfo& t = b.features[cell.feature];
                    const auto& rec = fdefs.records()[cell.feature];
                    render::FeatureInstance f;
                    f.cell_x = x;
                    f.cell_z = z;
                    f.footprint_x = t.footprint_x;
                    f.footprint_z = t.footprint_z;
                    f.height = t.height;
                    if (cell.flags & 1) {
                        const sim::FeatureSlot& sl = b.feature_state.slots[cell.feature_aux % sim::kFeatureSlots];
                        if (!(t.flags & sim::feature_type_flags::kFlat)) {
                            const FeatureModel* fm = feature_model(t.model);
                            if (!fm) continue;
                            f.model = &fm->model;
                            f.pose_model = &fm->pose;
                            f.pos = sl.pos();
                            f.pitch = sl.get<std::int16_t>(sim::FeatureSlot::kAngles);
                            f.heading = sl.get<std::uint16_t>(sim::FeatureSlot::kAngles + 2);
                            f.bank = sl.get<std::int16_t>(sim::FeatureSlot::kAngles + 4);
                        } else {
                            f.image = frame_at(sl.cursor(sim::FeatureSlot::kCursorA));
                            if (sl.get<std::uint8_t>(sim::FeatureSlot::kFlags) & sim::feature_slot_flags::kHasShadow)
                                f.shadow = frame_at(sl.cursor(sim::FeatureSlot::kCursorB));
                        }
                    } else if (t.flags & sim::feature_type_flags::kFlat) {
                        if (t.flags & sim::feature_type_flags::kAnimating) {
                            const auto& tc = b.feature_state.type_cursors[cell.feature];
                            f.image = t.sequences[sim::kSeqNormal] ? frame_at(tc[0]) : nullptr;
                            f.shadow = t.sequences[sim::kSeqNormalShadow] ? frame_at(tc[1]) : nullptr;
                        } else {
                            const auto* im = frames_of(t.sequences[sim::kSeqNormal]);
                            const auto* sh = frames_of(t.sequences[sim::kSeqNormalShadow]);
                            f.image = im && !im->empty() ? &(*im)[0] : nullptr;
                            f.shadow = sh && !sh->empty() ? &(*sh)[0] : nullptr;
                        }
                        f.image_blended = (rec.flags & ta::feature_flags::kAnimTrans) != 0;
                        f.shadow_blended = (rec.flags & ta::feature_flags::kShadTrans) != 0;
                    }
                    if (f.image || f.shadow || f.model) scene.features.push_back(f);
                }
            // Debris pieces and world effects.
            scene.debris.clear();
            for (int k = 0; k < sim::kMaxDebris; ++k) {
                if (b.effects.debris_block[static_cast<std::size_t>(k)] == -1) continue;
                const sim::DebrisPiece& d = b.effects.debris[static_cast<std::size_t>(k)];
                const auto m = model_of_pose.find(d.model);
                if (m == model_of_pose.end()) continue;
                render::DebrisInstance di;
                di.model = m->second;
                di.pose_model = d.model;
                di.node = d.node;
                di.pos = d.pos;
                di.turn = d.turn;
                di.team_colour = static_cast<std::uint8_t>(d.owner);
                scene.debris.push_back(di);
            }
            scene.effects.clear();
            for (std::int32_t k = 0; k < b.effects.count; ++k) {
                const sim::WorldEffect& e = b.effects.effects[static_cast<std::size_t>(k)];
                render::EffectInstance ei;
                ei.pos = e.pos;
                if (e.art.seq) ei.art = frame_at(e.art);
                for (int g = 0; g < 3; ++g)
                    if (e.glow.seq && e.glow.seq == b.effect_art.glow[static_cast<std::size_t>(g)] &&
                        e.glow.frame < glows[static_cast<std::size_t>(g)].size())
                        ei.glow = &glows[static_cast<std::size_t>(g)][e.glow.frame];
                if (e.shard >= 0 && e.shard < sim::kMaxShards) {
                    const sim::Shard& sh = b.effects.shards[static_cast<std::size_t>(e.shard)];
                    const auto m = model_of_pose.find(sh.model);
                    if (m != model_of_pose.end() && sh.node >= 0 && static_cast<std::size_t>(sh.node) < m->second->pieces.size()) {
                        const auto& prims = m->second->pieces[static_cast<std::size_t>(sh.node)].primitives;
                        if (sh.primitive >= 0 && static_cast<std::size_t>(sh.primitive) < prims.size()) {
                            ei.shard = true;
                            ei.points = sh.points;
                            ei.angle = e.angle;
                            ei.look = &prims[static_cast<std::size_t>(sh.primitive)];
                            ei.team_colour = static_cast<std::uint8_t>(sh.owner);
                        }
                    }
                }
                scene.effects.push_back(ei);
            }
            scene.light = light_table.empty() ? nullptr : &light_table;
            for (std::size_t i = 1; i < b.units.size(); ++i) {
                const sim::UnitTickState& t = b.ticks[i];
                if (t.type_index == 0 || t.type_index >= btypes.size()) continue;
                const sim::Unit& u = b.units[i];
                // The viewed player sees its own units and those in its sight (0x48BAE0).
                if (view_player >= 0 && t.owner != view_player && !b.unit_seen_by(view_player, u)) continue;
                const BattleType& bt = *btypes[t.type_index];
                render::UnitInstance ui;
                ui.unit = &u;
                ui.model = &bt.model;
                ui.params.team_colour = static_cast<std::uint8_t>(t.owner);
                ui.params.animation_frame = at_tick;
                ui.ground = (bt.def->type_flags & (1u << 11)) == 0;
                ui.ground_height = std::max(0, hm.sample(static_cast<std::int16_t>(u.pos[0] >> 16), static_cast<std::int16_t>(u.pos[2] >> 16)));
                ui.shadow = (bt.def->type_flags & (1u << 25)) == 0;
                ui.translucent = (u.activate_flags & sim::unit_flags::kCloaked) != 0;
                scene.units.push_back(ui);
            }
            scene.fog = nullptr;
            if (view_player >= 0) {
                // The viewed player's fog: unexplored blocks black, unseen ones grey.
                fog.blocks_x = b.sight.width;
                fog.blocks_z = b.sight.height;
                const std::size_t n = static_cast<std::size_t>(fog.blocks_x) * static_cast<std::size_t>(fog.blocks_z);
                fog.unexplored.assign(n, 0);
                fog.unseen.assign(n, 0);
                const auto& counts = b.sight.players[static_cast<std::size_t>(view_player)].counts;
                for (std::size_t k = 0; k < n; ++k) {
                    fog.unexplored[k] = k < b.explored.size() && !(b.explored[k] & (1u << view_player));
                    fog.unseen[k] = k < counts.size() && counts[k] == 0;
                }
                if (!b.settings.fog_of_war) fog.unseen.clear();
                scene.fog = &fog;
            }
            scene.projectiles.clear();
            for (std::int32_t k = 0; k < b.projectiles.count; ++k) {
                const sim::Projectile& p = b.projectiles.slots[static_cast<std::size_t>(k)];
                if (p.flags & sim::projectile_flags::kDeleted) continue;
                if (view_player >= 0 && !b.point_seen_by(view_player, p.pos)) continue;  // shots in the fog are not drawn
                render::ProjectileInstance pi;
                pi.look = render::ProjectileLook::Line;
                pi.pos = p.pos;
                // Beams draw from their tail; other shots as a short streak behind them.
                for (int a = 0; a < 3; ++a) pi.tail[static_cast<std::size_t>(a)] = p.pos[static_cast<std::size_t>(a)] - p.vel[static_cast<std::size_t>(a)] * 2;
                if (p.weapon && (p.weapon->flags & sim::weapon_flags::kBeam)) pi.tail = p.trail;
                pi.colour = p.owner_slot == 0 ? 0xE0 : 0xF4;
                scene.projectiles.push_back(pi);
            }
            return render::build_draw_list(scene);
        }
        for (const auto& [u, tc] : placed) {
            render::UnitInstance ui;
            ui.unit = u;
            ui.model = &tc.first->model;
            ui.params.team_colour = tc.second;
            ui.params.animation_frame = at_tick;
            ui.ground = (tc.first->def->type_flags & (1u << 11)) == 0;  // canfly
            ui.ground_height = std::max(0, hm.sample(static_cast<std::int16_t>(u->pos[0] >> 16), static_cast<std::int16_t>(u->pos[2] >> 16)));
            ui.shadow = (tc.first->def->type_flags & (1u << 25)) == 0;  // noshadow
            scene.units.push_back(ui);
        }
        // A sample beam so the projectile path shows up.
        scene.projectiles.clear();
        render::ProjectileInstance beam;
        beam.look = render::ProjectileLook::Line;
        beam.pos = {1560 << 16, 40 << 16, 1870 << 16};
        beam.tail = {1500 << 16, 50 << 16, 1820 << 16};
        beam.colour = 0xE0;
        scene.projectiles.push_back(beam);
        return render::build_draw_list(scene);
    };

    render::BmpPresenter out(prefix);
    std::uint32_t tick = 0;
    for (int f = 0; f < frames; ++f) {
        if (f > 0)
            for (int k = 0; k < step; ++k, ++tick) {
                if (battle) {
                    battle->advance_tick();
                    battle->step_units_and_projectiles();
                    battle->step_effects();   // stage 5
                    battle->players_stage();  // stage 6
                    battle->step_features();  // stage 7
                } else {
                    for (auto& u : units) u->script->tick(1);
                }
            }
        if (battle) battle->expire_lingering_sight();  // once per frame (the original's 0x482130)
        render::DrawList list = build_frame(tick);
        const render::Image8 frame = render::execute(list, blend);
        out.present(frame, palette);
        std::printf("frame %d (tick %u): %zu draw commands, %zu transient sprites\n", f, tick, list.cmds.size(), list.transient.size());
        if (battle) {
            int live = 0, aimed = 0;
            for (std::size_t i = 1; i < battle->units.size(); ++i) {
                if (battle->ticks[i].type_index == 0) continue;
                ++live;
                for (const auto& slot : battle->units[i].weapons) aimed += slot.target_z == sim::kUnitTarget && slot.target_x != 0;
            }
            std::printf("  live units %d, weapon slots aimed at a unit %d, shots in flight %d\n", live, aimed, battle->projectiles.count);
            int alive = 0, wrecks = 0, burning = 0;
            for (std::size_t i = 1; i < battle->units.size(); ++i) alive += battle->ticks[i].type_index != 0;
            for (const auto& c : battle->map.cells)
                if (c.feature < battle->features.size() && (c.flags & 1)) {
                    const auto& t = battle->features[c.feature];
                    if (!(t.flags & sim::feature_type_flags::kFlat)) ++wrecks;
                    else if (battle->feature_state.slots[c.feature_aux % sim::kFeatureSlots].get<std::uint8_t>(sim::FeatureSlot::kFlags) & 1) ++burning;
                }
            int debris = 0;
            for (auto d : battle->effects.debris_block) debris += d != -1;
            std::printf("  battle: %d units, %d 3D features, %d burning, %d world effects, %d debris pieces%s%s\n", alive, wrecks, burning,
                        battle->effects.count, debris, battle->feature_state.fault.empty() ? "" : " FEATURE FAULT: ",
                        battle->feature_state.fault.c_str());
        }
#ifdef RENDER_HAVE_GPU
        if (gpu) {
            render::Image8 g;
            render::lines_to_sprites(list);
            if (!gpu->render(list, g)) {
                std::printf("GPU render failed: %s\n", render::GpuRenderer::last_error().c_str());
            } else {
                std::size_t diff = 0;
                for (std::size_t i = 0; i < g.pixels.size() && i < frame.pixels.size(); ++i) diff += g.pixels[i] != frame.pixels[i];
                std::printf("  GPU (%s) vs CPU reference: %zu differing pixels\n", gpu->driver().c_str(), diff);
                std::vector<std::uint8_t> rgba;
                if (gpu->render_rgba(list, palette, rgba)) {
                    std::size_t rdiff = 0;
                    for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
                        const render::Rgb& c = palette[frame.pixels[i]];
                        rdiff += rgba[i * 4] != c.r || rgba[i * 4 + 1] != c.g || rgba[i * 4 + 2] != c.b;
                    }
                    std::printf("  GPU palette resolve vs CPU reference: %zu differing pixels\n", rdiff);
                }
                char name[32];
                std::snprintf(name, sizeof name, "gpu_%04d.bmp", f);
                render::write_bmp(prefix + name, g, palette);
            }
        }
#endif
    }
#ifdef RENDER_HAVE_GPU
    // Live view: run the scripts at 30 steps per second and show every step.
    if (window_seconds > 0) {
        if (!gpu) gpu = render::GpuRenderer::create(blend);
        if (!gpu) {
            std::printf("GPU backend unavailable: %s\n", render::GpuRenderer::last_error().c_str());
            return 1;
        }
        const std::uint64_t start = SDL_GetTicks();
        for (int n = 0; SDL_GetTicks() - start < static_cast<std::uint64_t>(window_seconds) * 1000u; ++n) {
            if (battle) {
                battle->advance_tick();
                battle->step_units_and_projectiles();
                battle->step_effects();   // stage 5
                battle->players_stage();  // stage 6
                battle->step_features();  // stage 7
            } else {
                for (auto& u : units) u->script->tick(1);
            }
            ++tick;
            if (battle) battle->expire_lingering_sight();
            render::DrawList list = build_frame(tick);
            render::lines_to_sprites(list);
            if (!gpu->present_window(list, palette, 2, "battle_view")) break;
            const std::uint64_t due = start + static_cast<std::uint64_t>(n + 1) * 1000u / 30u;
            const std::uint64_t now = SDL_GetTicks();
            if (now < due) SDL_Delay(static_cast<std::uint32_t>(due - now));
        }
    }
#else
    if (window_seconds > 0) std::printf("built without the GPU backend (no window output)\n");
#endif
    for (const auto& [u, tc] : placed)
        if (!u->script->fault().empty()) std::printf("script fault (%s): %s\n", tc.first->def->unit_name.c_str(), u->script->fault().c_str());
    return 0;
}
