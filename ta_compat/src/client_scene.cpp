// The battle view's scene for the playable client: terrain, features, units,
// shots, debris, effects, nano particles and the viewed player's fog, from the
// battle's state (the battle view tool's battle mode, with the viewed player's
// line of sight and the weapons' own looks).

#include <algorithm>

#include "ta/client_world.h"
#include "ta/model3do.h"

namespace ta {

const std::vector<render::Sprite8>* ClientWorld::frames_of(std::uint32_t seq) {
    if (!seq || !lb_->anims) return nullptr;
    auto it = seq_frames_.find(seq);
    if (it != seq_frames_.end()) return &it->second;
    std::vector<render::Sprite8>& v = seq_frames_[seq];
    const auto o = lb_->anims->origins().find(seq);
    if (o != lb_->anims->origins().end())
        if (const std::vector<std::uint8_t>* data = lb_->anims->file(o->second.first)) gaf_sequence_frames(*data, o->second.second, v);
    return &v;
}

void ClientWorld::fill_scene(render::BattleScene& scene, int view, std::uint32_t frame_counter) {
    sim::Battle& b = battle();
    scene.terrain.width = tiles_.width;
    scene.terrain.height = tiles_.height;
    scene.terrain.map = &tiles_.map;
    scene.terrain.tiles = &tiles_.tiles;
    scene.heights = &hm_;
    auto frame_at = [&](const sim::AnimCursor& c) -> const render::Sprite8* {
        const std::vector<render::Sprite8>* f = frames_of(c.seq);
        return f && c.frame < f->size() ? &(*f)[c.frame] : nullptr;
    };
    auto feature_model = [&](std::uint32_t id) -> const FeatureModel* {
        if (id == 0 || id > feature_models_.size()) return nullptr;
        FeatureModel& fm = feature_model_cache_[id];
        if (!fm.tried) {
            fm.tried = true;
            if (vfs_.read("objects3d\\" + feature_models_[id - 1] + ".3DO", fm.file)) {
                auto m = load_3do(fm.file, true);
                if (m.root) {
                    fm.root = std::move(m.root);
                    fm.pose = to_pose_model(fm.root.get());
                    fm.model = build_render_model(fm.root.get(), textures_);
                    fm.ok = true;
                }
            }
        }
        return fm.ok ? &fm : nullptr;
    };
    const bool sight = b.settings.fog_of_war || b.settings.explore;

    // Features as the battle holds them; hidden ones only where the viewer sees.
    for (std::int32_t z = 0; z < b.map.height; ++z)
        for (std::int32_t x = 0; x < b.map.width; ++x) {
            const sim::MapCell& cell = b.map.cells[static_cast<std::size_t>(z * b.map.width + x)];
            if (cell.feature >= sim::kFirstSpecialFeature || cell.feature >= b.features.size()) continue;
            const sim::FeatureTypeInfo& t = b.features[cell.feature];
            const auto& rec = lb_->feature_defs.records()[cell.feature];
            if (sight && (rec.flags & feature_flags::kNoDrawUnderGray) && ((cell.flags >> 3) & 0xF) != view) {
                auto corner = [&](int cx, int cz) {
                    return std::array<std::int32_t, 3>{
                        static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(cx * 16)) << 16),
                        static_cast<std::int32_t>(static_cast<std::uint32_t>(cell.height) << 16),
                        static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(cz * 16)) << 16)};
                };
                if (!b.point_seen_by(view, corner(x, z)) && !b.point_seen_by(view, corner(x + t.footprint_x, z + t.footprint_z))) continue;
            }
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
                f.image_blended = (rec.flags & feature_flags::kAnimTrans) != 0;
                f.shadow_blended = (rec.flags & feature_flags::kShadTrans) != 0;
            } else {
                // A 3D feature without a slot: drawn at its cell, resting.
                const FeatureModel* fm = feature_model(t.model);
                if (!fm) continue;
                f.model = &fm->model;
                f.pose_model = &fm->pose;
                f.pos = {((x * 16) + t.footprint_x * 8) << 16, static_cast<std::int32_t>(cell.height) << 16, ((z * 16) + t.footprint_z * 8) << 16};
            }
            if (f.image || f.shadow || f.model) scene.features.push_back(f);
        }

    // Debris and world effects.
    for (int k = 0; k < sim::kMaxDebris; ++k) {
        if (b.effects.debris_block[static_cast<std::size_t>(k)] == -1) continue;
        const sim::DebrisPiece& d = b.effects.debris[static_cast<std::size_t>(k)];
        const auto m = model_of_pose_.find(d.model);
        if (m == model_of_pose_.end()) continue;
        render::DebrisInstance di;
        di.model = m->second;
        di.pose_model = d.model;
        di.node = d.node;
        di.pos = d.pos;
        di.turn = d.turn;
        di.team_colour = static_cast<std::uint8_t>(d.owner);
        scene.debris.push_back(di);
    }
    for (std::int32_t k = 0; k < b.effects.count; ++k) {
        const sim::WorldEffect& e = b.effects.effects[static_cast<std::size_t>(k)];
        render::EffectInstance ei;
        ei.pos = e.pos;
        if (e.art.seq) ei.art = frame_at(e.art);
        for (int g = 0; g < 3; ++g)
            if (e.glow.seq && e.glow.seq == b.effect_art.glow[static_cast<std::size_t>(g)] && e.glow.frame < glows_[static_cast<std::size_t>(g)].size())
                ei.glow = &glows_[static_cast<std::size_t>(g)][e.glow.frame];
        if (e.shard >= 0 && e.shard < sim::kMaxShards) {
            const sim::Shard& sh = b.effects.shards[static_cast<std::size_t>(e.shard)];
            const auto m = model_of_pose_.find(sh.model);
            if (m != model_of_pose_.end() && sh.node >= 0 && static_cast<std::size_t>(sh.node) < m->second->pieces.size()) {
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
    scene.light = light_table_.empty() ? nullptr : &light_table_;
    scene.game_tick = b.game_tick();
    for (const sim::NanoStream& ns : b.nano.streams)
        for (const sim::NanoParticle& np : ns.particles) scene.nano.push_back({np.pos, static_cast<std::uint8_t>(np.colour)});

    // Units the viewer sees (its own and those in its sight).
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        const sim::UnitTickState& t = b.ticks[i];
        if (t.type_index == 0 || t.type_index >= types_.size() || !types_[t.type_index] || !types_[t.type_index]->ok) continue;
        const sim::Unit& u = b.units[i];
        if (sight && t.owner != view && !b.unit_seen_by(view, u)) continue;
        const UnitDefRecord& d = defs_.units[t.type_index];
        render::UnitInstance ui;
        ui.unit = &u;
        ui.model = &types_[t.type_index]->model;
        ui.params.team_colour = static_cast<std::uint8_t>(t.owner);
        ui.params.animation_frame = frame_counter;
        ui.ground = (d.type_flags & (1u << 11)) == 0;
        ui.ground_height = std::max(0, hm_.sample(static_cast<std::int16_t>(u.pos[0] >> 16), static_cast<std::int16_t>(u.pos[2] >> 16)));
        ui.shadow = (d.type_flags & (1u << 25)) == 0;
        ui.translucent = (u.activate_flags & sim::unit_flags::kCloaked) != 0;
        scene.units.push_back(ui);
    }

    // The viewed player's fog: unexplored blocks black, unseen ones grey.
    scene.fog = nullptr;
    if (sight) {
        fog_.blocks_x = b.sight.width;
        fog_.blocks_z = b.sight.height;
        const std::size_t n = static_cast<std::size_t>(fog_.blocks_x) * static_cast<std::size_t>(fog_.blocks_z);
        fog_.unexplored.assign(n, 0);
        fog_.unseen.assign(n, 0);
        const auto& counts = b.sight.players[static_cast<std::size_t>(view)].counts;
        for (std::size_t k = 0; k < n; ++k) {
            fog_.unexplored[k] = b.settings.explore && k < b.explored.size() && !(b.explored[k] & (1u << view));
            fog_.unseen[k] = k < counts.size() && counts[k] == 0;
        }
        if (!b.settings.fog_of_war) fog_.unseen.clear();
        scene.fog = &fog_;
    }

    // Shots in the viewer's sight, by their weapon's look.
    for (std::int32_t k = 0; k < b.projectiles.count; ++k) {
        const sim::Projectile& p = b.projectiles.slots[static_cast<std::size_t>(k)];
        if (p.flags & sim::projectile_flags::kDeleted) continue;
        if (b.settings.fog_of_war && !b.point_seen_by(view, p.pos)) continue;
        const std::size_t wi = p.weapon ? p.weapon->index : 0;
        const client::ShotLook* look = wi < content_.shots.size() ? &content_.shots[wi] : nullptr;
        if (look && look->hidden) continue;
        render::ProjectileInstance pi;
        pi.pos = p.pos;
        for (int a = 0; a < 3; ++a) pi.tail[static_cast<std::size_t>(a)] = p.pos[static_cast<std::size_t>(a)] - p.vel[static_cast<std::size_t>(a)] * 2;
        if (p.weapon && (p.weapon->flags & sim::weapon_flags::kBeam)) pi.tail = p.trail;
        pi.colour = look ? look->colour : 0xE0;
        if (look && look->look == render::ProjectileLook::Sprite && look->frames && !look->frames->empty()) {
            pi.look = render::ProjectileLook::Sprite;
            pi.sprite = &(*look->frames)[(b.game_tick() - p.spawn_tick) % look->frames->size()];
        } else if (look && look->look == render::ProjectileLook::Model && look->model) {
            pi.look = render::ProjectileLook::Model;
            pi.model = look->model;
            pi.pose_model = look->pose_model;
            pi.heading = p.yaw;
            pi.pitch = static_cast<std::int16_t>(p.elevation);
            pi.ground_height = std::max(0, hm_.sample(static_cast<std::int16_t>(p.pos[0] >> 16), static_cast<std::int16_t>(p.pos[2] >> 16)));
        } else {
            pi.look = render::ProjectileLook::Line;
        }
        scene.projectiles.push_back(pi);
    }
}

} // namespace ta
