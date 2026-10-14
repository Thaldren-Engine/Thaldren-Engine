#include "sim/economy.h"

#include "sim/features.h"
#include "sim/trig.h"
#include "sim/x87.h"

// Arithmetic follows the original's x87 at 53-bit precision: every operation rounds to
// double, and a value is rounded to float exactly where the original stores one.
// Comparisons are written so that unordered (NaN) operands take the same branch as there.

namespace sim {
namespace {

using i32 = std::int32_t;
using u32 = std::uint32_t;

inline float f32(double v) { return static_cast<float>(v); }

constexpr u32 kAliveFlag = 0x10000000u, kStructureFlag = 0x20000000u, kMovingBits = 0xCu;
constexpr u32 kWantsCloak = 0x800u, kCloakBlocked = 0x1000u;

// Books a unit's own energy use: negative use is production; otherwise it is use, paid
// unless the slot is stalled. Returns whether it was booked as paid.
bool book_energy_use(UnitTickState& t, const PlayerSlot& slot_player, i32 ai_difficulty, float use) {
    if (!(use >= 0.0f)) {  // negative (or unordered): production
        credit_make(t.energy_make, slot_player, ai_difficulty, -static_cast<double>(use));
        return false;
    }
    t.energy_use = f32(static_cast<double>(use) + t.energy_use);
    if (t.energy_stall > 0.0f) return false;
    t.energy_use_paid = f32(static_cast<double>(use) + t.energy_use_paid);
    return true;
}

// The two-stage payment of one resource from `pool`: the stalled backlog first, then the
// paid use, each in full or by the fraction the pool covers.
void pay_from_pool(float& pool, float stall, float paid, float& stall_fraction, float& paid_fraction) {
    double taken;
    if (stall > pool) {
        stall_fraction = f32(static_cast<double>(pool) / stall);
        taken = pool;
    } else {
        stall_fraction = 1.0f;
        taken = stall;
    }
    const double rest = static_cast<double>(pool) - taken;  // kept at double precision
    if (paid > rest) {
        paid_fraction = f32(rest / paid);
        pool = f32(rest - rest);
    } else {
        paid_fraction = 1.0f;
        pool = f32(rest - static_cast<double>(paid));
    }
}

// End of a settlement for one slot: the figures move to "previous", production and use
// restart from zero, and the unpaid parts of the backlog and of the paid use become the
// new backlog.
template <class Slot>
void close_slot(Slot& s, float e_stall_fraction, float e_paid_fraction, float m_stall_fraction, float m_paid_fraction) {
    const double e_paid_left = static_cast<double>(s.energy_use_paid) - static_cast<double>(e_paid_fraction) * s.energy_use_paid;
    const double e_stall_left = static_cast<double>(s.energy_stall) - static_cast<double>(e_stall_fraction) * s.energy_stall;
    s.energy_use_prev = s.energy_use;
    s.energy_make_prev = s.energy_make;
    s.energy_use = 0.0f;
    s.energy_make = 0.0f;
    s.energy_use_paid = 0.0f;
    s.energy_stall = f32(e_paid_left + e_stall_left);
    const double m_paid_left = static_cast<double>(s.metal_use_paid) - static_cast<double>(m_paid_fraction) * s.metal_use_paid;
    const double m_stall_left = static_cast<double>(s.metal_stall) - static_cast<double>(m_stall_fraction) * s.metal_stall;
    s.metal_use_prev = s.metal_use;
    s.metal_make_prev = s.metal_make;
    s.metal_use = 0.0f;
    s.metal_make = 0.0f;
    s.metal_use_paid = 0.0f;
    s.metal_stall = f32(m_paid_left + m_stall_left);
}

const EconomyType& type_of(const Economy& e, const UnitTickState& t) {
    static const EconomyType kNone{};
    return t.type_index < e.types.size() ? e.types[t.type_index] : kNone;
}

PlayerSlot& player_at(Economy& e, int index) { return e.players[static_cast<std::size_t>(index)]; }

} // namespace

bool add_use_if_not_stalled(UnitTickState& slot, float energy, float metal) {
    slot.energy_use = f32(static_cast<double>(energy) + slot.energy_use);
    slot.metal_use = f32(static_cast<double>(metal) + slot.metal_use);
    if (slot.energy_stall > 0.0f || slot.metal_stall > 0.0f) return false;
    slot.energy_use_paid = f32(static_cast<double>(energy) + slot.energy_use_paid);
    slot.metal_use_paid = f32(static_cast<double>(metal) + slot.metal_use_paid);
    return true;
}

bool add_use_if_not_stalled(ResourceSlot& slot, float energy, float metal) {
    slot.energy_use = f32(static_cast<double>(energy) + slot.energy_use);
    slot.metal_use = f32(static_cast<double>(metal) + slot.metal_use);
    if (slot.energy_stall > 0.0f || slot.metal_stall > 0.0f) return false;
    slot.energy_use_paid = f32(static_cast<double>(energy) + slot.energy_use_paid);
    slot.metal_use_paid = f32(static_cast<double>(metal) + slot.metal_use_paid);
    return true;
}

bool add_energy_use_if_not_stalled(UnitTickState& slot, float energy) {
    slot.energy_use = f32(static_cast<double>(energy) + slot.energy_use);
    if (slot.energy_stall > 0.0f) return false;
    slot.energy_use_paid = f32(static_cast<double>(energy) + slot.energy_use_paid);
    return true;
}

bool try_consume_energy(PlayerSlot& payer, ResourceSlot& slot, float amount) {
    if (!(payer.energy_stored >= amount)) return false;
    payer.energy_stored = f32(static_cast<double>(payer.energy_stored) - amount);
    slot.energy_use = f32(static_cast<double>(amount) + slot.energy_use);
    return true;
}

bool try_consume_metal(PlayerSlot& payer, ResourceSlot& slot, float amount) {
    if (!(payer.metal_stored >= amount)) return false;
    payer.metal_stored = f32(static_cast<double>(payer.metal_stored) - amount);
    slot.metal_use = f32(static_cast<double>(amount) + slot.metal_use);
    return true;
}

void credit_make(float& make, const PlayerSlot& slot_player, i32 ai_difficulty, double amount) {
    if (slot_player.active && slot_player.controller == 2) {
        if (ai_difficulty == 0) {
            make = f32(static_cast<double>(make) - amount * -0.5);
            return;
        }
        if (ai_difficulty == 1) {
            make = f32(static_cast<double>(make) - amount * -0.7);
            return;
        }
    }
    make = f32(static_cast<double>(make) + amount);
}

void init_player_economy(Economy& e, int player) {
    PlayerSlot& p = player_at(e, player);
    PlayerEconomy& a = e.accounts[static_cast<std::size_t>(player)];
    a.next_update_tick = e.env.game_tick;
    a.energy_produced = a.metal_produced = a.energy_consumed = a.metal_consumed = a.energy_wasted = a.metal_wasted = 0.0;
    p.energy_stored = p.metal_stored = 0.0f;
    a.energy_income = a.energy_usage = a.metal_income = a.metal_usage = 0.0f;
    a.energy_storage = a.metal_storage = 0.0f;
    a.share_metal_above = a.share_energy_above = 0.0f;
    a.own = ResourceSlot{};
    a.own.player = p.state;
    p.flags = static_cast<std::uint16_t>(p.flags & ~1u);
    // The storage bonuses are left as they were.
}

void set_storage_bonus(PlayerSlot& p, PlayerEconomy& a, i32 metal, i32 energy) {
    p.flags = static_cast<std::uint16_t>(p.flags | 1u);
    a.energy_storage_bonus = static_cast<float>(energy < 200 ? 200 : energy);
    a.metal_storage_bonus = static_cast<float>(metal < 200 ? 200 : metal);
}

bool settlement_allowed(const PlayerSlot& p, const EndGameState& end) {
    if (!p.active || p.controller < 1 || p.controller > 3 || p.state == 10) return false;
    if (p.alive_count == 0 && p.units_created != 0) return false;
    if (p.controller != 1 && p.controller != 2) return false;
    return !(end.flags & 4) && end.countdown < 0;
}

void settle_player(Economy& e, int player) {
    PlayerSlot& p = player_at(e, player);
    PlayerEconomy& a = e.accounts[static_cast<std::size_t>(player)];
    const i32 diff = e.env.ai_difficulty;
    float e_use = 0.0f, m_use = 0.0f, e_stall = 0.0f, m_stall = 0.0f;
    float e_paid = 0.0f, m_paid = 0.0f, e_make = 0.0f, m_make = 0.0f;
    a.metal_storage = 0.0f;
    a.energy_storage = 0.0f;
    const bool watched = p.active && p.controller == 3;
    const int last = p.last_unit < static_cast<int>(e.units.size()) ? p.last_unit : static_cast<int>(e.units.size()) - 1;
    for (int id = p.first_unit; id <= last; ++id) {
        Unit& u = e.units[static_cast<std::size_t>(id)];
        if (!(u.flags & kAliveFlag)) continue;
        UnitTickState& t = e.ticks[static_cast<std::size_t>(id)];
        const EconomyType& ty = type_of(e, t);
        PlayerSlot& sp = player_at(e, t.owner);  // the player the unit's slot books for
        if (u.flags & kStructureFlag) {
            if (u.activate_flags & unit_flags::kActivated) {
                const bool paid = book_energy_use(t, sp, diff, ty.energy_use);
                // One special production at most, in this order.
                if (ty.extracts_metal > 0.0f) {
                    if (paid) credit_make(t.metal_make, sp, diff, u.metal_extraction);
                } else if (ty.makes_metal != 0) {
                    if (paid) credit_make(t.metal_make, sp, diff, static_cast<double>(ty.makes_metal));
                } else if (ty.wind_generator > 0.0f) {
                    credit_make(t.energy_make, sp, diff, static_cast<double>(e.env.wind_fraction) * ty.wind_generator);
                } else if (ty.tidal_generator > 0.0f) {
                    credit_make(t.energy_make, sp, diff, static_cast<double>(e.env.tidal_strength) * ty.tidal_generator);
                }
            }
        } else if ((u.activate_flags & unit_flags::kActivated) || (u.flags & kMovingBits)) {
            book_energy_use(t, sp, diff, ty.energy_use);
        }
        // Complete units (a build fraction left of exactly zero) produce and store.
        if (!(u.build_fraction_left < 0.0f) && !(u.build_fraction_left > 0.0f)) {
            credit_make(t.energy_make, sp, diff, ty.energy_make);
            credit_make(t.metal_make, sp, diff, ty.metal_make);
            a.metal_storage = f32(static_cast<double>(ty.metal_storage) + a.metal_storage);
            a.energy_storage = f32(static_cast<double>(ty.energy_storage) + a.energy_storage);
        }
        if (!watched) {
            // Cloak upkeep: paid at once from the stored energy, whole units only.
            bool cloaked = false;
            if ((u.flags & kWantsCloak) && !(u.flags & kCloakBlocked) && !(u.cloak_holdoff_tick > e.env.game_tick)) {
                const float rate = (u.flags & kMovingBits) ? ty.cloak_cost_moving : ty.cloak_cost;
                const double cost = x87::ftol32(rate);
                if (!(cost > sp.energy_stored)) {
                    sp.energy_stored = f32(static_cast<double>(sp.energy_stored) - cost);
                    t.energy_use = f32(cost + t.energy_use);
                    cloaked = true;
                }
            }
            e.systems.set_cloaked(u, cloaked);
        }
        e_make = f32(static_cast<double>(t.energy_make) + e_make);
        e_use = f32(static_cast<double>(t.energy_use) + e_use);
        e_paid = f32(static_cast<double>(t.energy_use_paid) + e_paid);
        e_stall = f32(static_cast<double>(t.energy_stall) + e_stall);
        m_make = f32(static_cast<double>(t.metal_make) + m_make);
        m_use = f32(static_cast<double>(t.metal_use) + m_use);
        m_paid = f32(static_cast<double>(t.metal_use_paid) + m_paid);
        m_stall = f32(static_cast<double>(t.metal_stall) + m_stall);
    }
    // The player's own slot; its energy production stays at double precision from here.
    ResourceSlot& own = a.own;
    const double energy_made = static_cast<double>(own.energy_make) + e_make;
    e_use = f32(static_cast<double>(own.energy_use) + e_use);
    e_paid = f32(static_cast<double>(own.energy_use_paid) + e_paid);
    e_stall = f32(static_cast<double>(own.energy_stall) + e_stall);
    m_make = f32(static_cast<double>(own.metal_make) + m_make);
    m_use = f32(static_cast<double>(own.metal_use) + m_use);
    m_paid = f32(static_cast<double>(own.metal_use_paid) + m_paid);
    m_stall = f32(static_cast<double>(own.metal_stall) + m_stall);
    if (p.flags & 1) {
        a.energy_storage = f32(static_cast<double>(a.energy_storage_bonus) + a.energy_storage);
        a.metal_storage = f32(static_cast<double>(a.metal_storage_bonus) + a.metal_storage);
    }
    a.energy_income = f32(energy_made);
    a.energy_usage = e_use;
    a.energy_produced = energy_made + a.energy_produced;
    a.energy_consumed = static_cast<double>(e_use) + a.energy_consumed;
    a.metal_income = m_make;
    a.metal_usage = m_use;
    a.metal_produced = static_cast<double>(m_make) + a.metal_produced;
    a.metal_consumed = static_cast<double>(m_use) + a.metal_consumed;
    float e_pool = f32(energy_made + p.energy_stored);
    float m_pool = f32(static_cast<double>(m_make) + p.metal_stored);
    float e_stall_fraction, e_paid_fraction, m_stall_fraction, m_paid_fraction;
    pay_from_pool(e_pool, e_stall, e_paid, e_stall_fraction, e_paid_fraction);
    pay_from_pool(m_pool, m_stall, m_paid, m_stall_fraction, m_paid_fraction);
    p.energy_stored = e_pool;
    if (e_pool > a.energy_storage) {
        p.energy_stored = a.energy_storage;
        a.energy_wasted = (static_cast<double>(e_pool) - a.energy_storage) + a.energy_wasted;
    }
    p.metal_stored = m_pool;
    if (m_pool > a.metal_storage) {
        p.metal_stored = a.metal_storage;
        a.metal_wasted = (static_cast<double>(m_pool) - a.metal_storage) + a.metal_wasted;
    }
    for (int id = p.first_unit; id <= last; ++id) {
        if (!(e.units[static_cast<std::size_t>(id)].flags & kAliveFlag)) continue;
        close_slot(e.ticks[static_cast<std::size_t>(id)], e_stall_fraction, e_paid_fraction, m_stall_fraction, m_paid_fraction);
    }
    close_slot(own, e_stall_fraction, e_paid_fraction, m_stall_fraction, m_paid_fraction);
}

float transfer_energy(Economy& e, int from, int to, float amount, bool sending) {
    if (from == 10 || to == 10) return 0.0f;
    if (from < 0 || to < 0 || static_cast<std::size_t>(from) >= e.players.size() || static_cast<std::size_t>(to) >= e.players.size())
        return 0.0f;
    if (sending && amount > player_at(e, from).energy_stored) amount = player_at(e, from).energy_stored;
    if (!(amount < 0.0f) && !(amount > 0.0f)) return 0.0f;  // zero (or unordered): nothing happens
    if (sending) {
        ResourceSlot& s = e.accounts[static_cast<std::size_t>(from)].own;
        try_consume_energy(player_at(e, s.player), s, amount);
    }
    ResourceSlot& r = e.accounts[static_cast<std::size_t>(to)].own;
    credit_make(r.energy_make, player_at(e, r.player), e.env.ai_difficulty, amount);
    return amount;
}

float transfer_metal(Economy& e, int from, int to, float amount, bool sending) {
    if (from == 10 || to == 10) return 0.0f;
    if (from < 0 || to < 0 || static_cast<std::size_t>(from) >= e.players.size() || static_cast<std::size_t>(to) >= e.players.size())
        return 0.0f;
    if (sending && amount > player_at(e, from).metal_stored) amount = player_at(e, from).metal_stored;
    if (!(amount < 0.0f) && !(amount > 0.0f)) return 0.0f;
    if (sending) {
        ResourceSlot& s = e.accounts[static_cast<std::size_t>(from)].own;
        try_consume_metal(player_at(e, s.player), s, amount);
    }
    ResourceSlot& r = e.accounts[static_cast<std::size_t>(to)].own;
    credit_make(r.metal_make, player_at(e, r.player), e.env.ai_difficulty, amount);
    return amount;
}

namespace economy_detail {

bool may_receive_share(const Economy& e, int player, int other, const std::vector<std::array<std::uint8_t, 16>>& alliances) {
    const PlayerSlot& o = e.players[static_cast<std::size_t>(other)];
    if (!o.active || o.controller < 1 || o.controller > 3 || o.state == 10) return false;
    if (o.alive_count == 0 && o.units_created != 0) return false;
    if (o.controller != 3) return false;  // remote players only
    if (e.accounts[static_cast<std::size_t>(other)].side_controller != 1) return false;
    return static_cast<std::size_t>(player) < alliances.size() && alliances[static_cast<std::size_t>(player)][static_cast<std::size_t>(other)] != 0;
}

float share_amount(float have, float above, float factor, float storage, float stored) {
    const double share = (static_cast<double>(have) - above) * factor;
    const double space = static_cast<double>(storage) - stored;
    return !(share >= space) ? f32(share) : f32(space);  // unordered keeps the share
}

} // namespace economy_detail

void step_wind(WindState& w, u32 game_tick, BattleRng& rng, Lcg15& crt_rand) {
    if (!(w.next_change_tick < game_tick)) {
        w.changed = false;
        return;
    }
    const std::int64_t r = static_cast<std::int64_t>(static_cast<i32>(crt_rand.next())) * 10;
    const i32 steps = static_cast<i32>(r / 0x8000);
    w.next_change_tick += static_cast<u32>(steps + 5) * 30u;
    w.speed = static_cast<i32>(static_cast<u32>(w.min_speed) + rng.next_mod(static_cast<u32>(w.max_speed) - static_cast<u32>(w.min_speed)));
    if (w.speed != 0) w.heading = static_cast<std::uint16_t>(rng.next_mod(0x10000));
    w.velocity[0] = static_cast<i32>((0u - static_cast<u32>(trig::mul_sin(w.heading, w.speed))) << 1);
    w.velocity[2] = static_cast<i32>((0u - static_cast<u32>(trig::mul_cos(w.heading, w.speed))) << 1);
    w.fraction = f32(static_cast<double>(w.speed) / static_cast<double>(w.denominator));
    if (w.fraction > 1.0f) w.fraction = 1.0f;
    w.changed = true;
}

void stamp_feature_metal(MapGrid& map, const std::vector<FeatureTypeInfo>& features, const std::vector<float>& metal_by_type) {
    const i32 cells = map.width * map.height;
    for (i32 c = 0; c < cells; ++c) {
        const std::uint16_t f = map.cells[static_cast<std::size_t>(c)].feature;
        if (f >= kFirstSpecialFeature || f >= features.size() || f >= metal_by_type.size()) continue;
        const float metal = metal_by_type[f];
        if (!(metal < 0.0f) && !(metal > 0.0f)) continue;
        const FeatureTypeInfo& t = features[f];
        if (!(t.flags & feature_type_flags::kIndestructible)) continue;
        const i32 x0 = c % map.width, z0 = c / map.width;
        const auto value = static_cast<std::uint8_t>(x87::ftol32(metal));
        for (i32 z = z0; z < z0 + t.footprint_z; ++z)
            for (i32 x = x0; x < x0 + t.footprint_x; ++x)
                if (MapCell* mc = map.cell(x, z)) mc->metal = value;
    }
}

} // namespace sim
