#pragma once
// Resources: each player's stored metal and energy, the resource slots units
// (and players) book production and use on, the periodic settlement that turns
// those bookings into stored resources and decides who stalls, transfers
// between players and the wind that drives wind generators
// (see docs/RE_MEMORY.md, "Economy").
//
// How resources move:
// - Spending right away (weapon shots, cloaking, transfers): the player's stored
//   amount must cover the cost; it is taken at once and booked as use on the slot.
//   Weapon shots use use_resources (sim/weapons.h); the others are below.
// - Spending over time (building, repairing, stockpiling): a request is booked as
//   use on the unit's slot and as "paid" use unless the slot is stalled. Nothing is
//   taken at once. The request returns false while the slot is stalled, and the
//   caller then makes no progress this step.
// - Every 30 steps the player's settlement adds all production to the stored amount,
//   pays the stalled backlog first and then the paid use, both as far as the stored
//   amount reaches. Whatever could not be paid becomes the slot's new stall, so a
//   unit that was short stays stalled until a later settlement pays its backlog.
//
// Unit slots live in UnitTickState (metal_make, energy_use, metal_use and the rest);
// each player has one more slot of its own (PlayerEconomy::own) that transfers use.

#include <array>
#include <cstdint>
#include <vector>

#include "sim/projectile.h"
#include "sim/rng.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

// Unit type data the economy needs.
struct EconomyType {
    float energy_make = 0.0f;       // made while the unit is complete
    float energy_use = 0.0f;        // used while active (or moving, for mobile units); negative = made
    float metal_make = 0.0f;        // made while the unit is complete
    float extracts_metal = 0.0f;    // metal extractor: per cell of metal under its footprint (sim/unit_create.h)
    std::uint8_t makes_metal = 0;   // metal maker: metal per settlement while its energy use is paid
    float wind_generator = 0.0f;    // energy per settlement at full wind
    float tidal_generator = 0.0f;   // energy per settlement at a tidal strength of 1
    float cloak_cost = 0.0f, cloak_cost_moving = 0.0f;  // energy per settlement while cloaked
    float energy_storage = 0.0f, metal_storage = 0.0f;  // added to the player's capacity while complete
};

// A resource slot that belongs to no unit (field meanings as in UnitTickState).
struct ResourceSlot {
    float energy_make = 0.0f, energy_use = 0.0f, energy_use_paid = 0.0f, energy_stall = 0.0f;
    float energy_make_prev = 0.0f, energy_use_prev = 0.0f;
    float metal_make = 0.0f, metal_use = 0.0f, metal_use_paid = 0.0f, metal_stall = 0.0f;
    float metal_make_prev = 0.0f, metal_use_prev = 0.0f;
    int player = 0;  // player record whose stored resources it spends
};

// A player's resources besides the stored amounts (PlayerSlot::energy_stored / metal_stored).
struct PlayerEconomy {
    float energy_storage = 0.0f, metal_storage = 0.0f;  // capacity found by the last settlement
    float energy_income = 0.0f, energy_usage = 0.0f;    // totals of the last settlement
    float metal_income = 0.0f, metal_usage = 0.0f;
    double energy_produced = 0.0, energy_consumed = 0.0, energy_wasted = 0.0;  // battle totals
    double metal_produced = 0.0, metal_consumed = 0.0, metal_wasted = 0.0;
    // Extra capacity while PlayerSlot::flags bit 0 is set (the bit the commander's death clears).
    float energy_storage_bonus = 0.0f, metal_storage_bonus = 0.0f;
    // Automatic sharing to allies (network games): amounts above these are shared.
    float share_metal_above = 0.0f, share_energy_above = 0.0f;
    std::uint8_t share_flags = 0;      // bit 1 metal, bit 2 energy, bit 5 the explored map
    std::uint8_t side_controller = 0;  // the player's side record byte the sharing checks (1 = may receive)
    std::uint32_t next_update_tick = 0;  // step of the player's next 30-step turn (players stage)
    // Two more steps the record's set-up stamps (player record +0xF4 / +0xF8; the statistics'
    // timers of the front end); saved games keep them.
    std::uint32_t win_lose_tick = 0, display_tick = 0;
    ResourceSlot own;                    // the player's own slot (transfers)
};

// The wind (one per battle), changed by the wind stage.
struct WindState {
    std::uint32_t next_change_tick = 0;
    std::int32_t min_speed = 0, max_speed = 0;  // from the map
    std::int32_t denominator = 5000;            // speed of full wind generator output (set when the map loads)
    std::int32_t speed = 0;
    std::uint16_t heading = 0;
    Vec3i velocity{};       // per-step drift (x and z; y is never written)
    float fraction = 0.0f;  // speed / denominator, at most 1
    bool changed = false;   // set by the step that changed it
};

// Victory state the settlement depends on (owned by the victory checks).
struct EndGameState {
    std::uint16_t flags = 0;       // bit 2: the game is ending
    std::int16_t countdown = -1;   // steps of 30 left; negative while not counting
};

struct EconomyEnvironment {
    std::uint32_t game_tick = 0;
    std::int32_t ai_difficulty = 0;  // 0 easy, 1 medium: AI income is scaled by 0.5 / 0.7
    float wind_fraction = 0.0f;
    float tidal_strength = 0.0f;     // from the map
};

class EconomySystems {
public:
    virtual ~EconomySystems() = default;
    // The settlement's cloak decision for a unit (sets or clears its cloaked activate
    // flag, every settlement, for every live unit of a non-watched player).
    virtual void set_cloaked(Unit& unit, bool on) = 0;
};

struct Economy {
    std::vector<Unit>& units;
    std::vector<UnitTickState>& ticks;
    std::vector<PlayerSlot>& players;
    std::vector<PlayerEconomy>& accounts;      // by player record
    const std::vector<EconomyType>& types;     // by unit type index
    EconomyEnvironment& env;
    EconomySystems& systems;
};

// ---- Booking on a unit's slot (the API construction, repair and stockpiling use) ----

// Requests energy and metal for this step's work: books both as use and, unless the
// slot is stalled on either, as paid use. Returns false (nothing paid, no progress)
// while stalled. (0x4011C0)
bool add_use_if_not_stalled(UnitTickState& slot, float energy, float metal);
bool add_use_if_not_stalled(ResourceSlot& slot, float energy, float metal);
// Energy only; stalls only on energy. (0x401180)
bool add_energy_use_if_not_stalled(UnitTickState& slot, float energy);

// Spends from the slot's player's stored amount at once if it covers the cost, booking
// it as use. (0x401220, 0x401260; both at once: use_resources in sim/weapons.h)
bool try_consume_energy(PlayerSlot& payer, ResourceSlot& slot, float amount);
bool try_consume_metal(PlayerSlot& payer, ResourceSlot& slot, float amount);

// Adds `amount` to a slot's production the way income is credited everywhere: an AI
// player's income is scaled by 0.5 on easy and 0.7 on medium. (Inline in the original.)
void credit_make(float& make, const PlayerSlot& slot_player, std::int32_t ai_difficulty, double amount);

// ---- Players ----

// A player's economy at battle setup (0x464700): everything zero, the next 30-step turn
// at `game_tick`, its own slot spending `players[player].state`'s record, flags bit 0 cleared.
void init_player_economy(Economy& e, int player);
// Starting storage bonus (0x496E90): bonuses of at least 200 and flags bit 0 set.
void set_storage_bonus(PlayerSlot& p, PlayerEconomy& a, std::int32_t metal, std::int32_t energy);

// The settlement (0x401360), every 30 steps per player from the players stage: books each
// live unit's production and upkeep, decides cloaking, then pays stalls and use from
// production plus stored resources, caps by storage and resets the slots.
void settle_player(Economy& e, int player);

// Whether the players stage settles this player in its 30-step turn: playing (controller
// 1/2), with live units or none ever made, and the game not ending.
bool settlement_allowed(const PlayerSlot& p, const EndGameState& end);

// Transfers (0x464B30 energy, 0x464C60 metal). `sending`: the local machine's own
// transfer (clamped to what `from` has, spent from its own slot, then announced);
// otherwise a received one (credited only). Returns the amount credited (0: nothing).
float transfer_energy(Economy& e, int from, int to, float amount, bool sending);
float transfer_metal(Economy& e, int from, int to, float amount, bool sending);

// Automatic sharing in network games (0x457D30, called with the local player every step
// of a network game): every 60 steps a third of the metal and half of the energy above
// the player's thresholds go to an allied remote player with less (up to its free
// storage). `shared(kind, to, amount)` is told of each transfer (kind 2 metal, 1 energy),
// `share_map(to)` of each explored-map share every 450 steps.
template <class Shared, class ShareMap>
void auto_share(Economy& e, int player, const std::vector<std::array<std::uint8_t, 16>>& alliances, Shared shared,
                ShareMap share_map);

// ---- Wind (0x490C40, a stage of its own after the features) ----

// Changes the wind when its time has come (two battle RNG draws or one, one rand()).
void step_wind(WindState& w, std::uint32_t game_tick, BattleRng& rng, Lcg15& crt_rand);

// ---- Map metal (while the map loads) ----

// Indestructible features with metal stamp it into the cells of their footprint
// (0x422040). `metal_by_type` is indexed like `features` (the feature types' metal value).
void stamp_feature_metal(MapGrid& map, const std::vector<FeatureTypeInfo>& features,
                         const std::vector<float>& metal_by_type);

// ---- implementation of the template ----

namespace economy_detail {
bool may_receive_share(const Economy& e, int player, int other, const std::vector<std::array<std::uint8_t, 16>>& alliances);
float share_amount(float have, float above, float factor, float storage, float stored);
}  // namespace economy_detail

template <class Shared, class ShareMap>
void auto_share(Economy& e, int player, const std::vector<std::array<std::uint8_t, 16>>& alliances, Shared shared,
                ShareMap share_map) {
    PlayerSlot& p = e.players[static_cast<std::size_t>(player)];
    PlayerEconomy& a = e.accounts[static_cast<std::size_t>(player)];
    if (e.env.game_tick % 60 == 0) {
        int to = player;
        if ((a.share_flags & 2) && p.metal_stored > a.share_metal_above)
            for (int i = 0; i < 10; ++i)
                if (economy_detail::may_receive_share(e, player, i, alliances) &&
                    !(e.players[static_cast<std::size_t>(i)].metal_stored >= p.metal_stored))
                    to = i;
        if (to != player && p.metal_stored > a.share_metal_above) {
            const PlayerSlot& r = e.players[static_cast<std::size_t>(to)];
            const PlayerEconomy& ra = e.accounts[static_cast<std::size_t>(to)];
            const float amount = economy_detail::share_amount(p.metal_stored, a.share_metal_above, 0.33333334f,
                                                              ra.metal_storage, r.metal_stored);
            // The original passes the state indices (equal to the slots in a running game).
            const float sent = transfer_metal(e, p.state, r.state, amount, true);
            if (sent != 0.0f) shared(2, static_cast<int>(r.state), sent);
        }
        to = player;
        if ((a.share_flags & 4) && p.energy_stored > a.share_energy_above)
            for (int i = 0; i < 10; ++i)
                if (economy_detail::may_receive_share(e, player, i, alliances) &&
                    !(e.players[static_cast<std::size_t>(i)].energy_stored >= p.energy_stored))
                    to = i;
        if (to != player && p.energy_stored > a.share_energy_above) {
            const PlayerSlot& r = e.players[static_cast<std::size_t>(to)];
            const PlayerEconomy& ra = e.accounts[static_cast<std::size_t>(to)];
            const float amount = economy_detail::share_amount(p.energy_stored, a.share_energy_above, 0.5f,
                                                              ra.energy_storage, r.energy_stored);
            const float sent = transfer_energy(e, p.state, r.state, amount, true);
            if (sent != 0.0f) shared(1, static_cast<int>(r.state), sent);
        }
    }
    if (e.env.game_tick % 450 == 0 && (a.share_flags & 0x20))
        for (int i = 0; i < 10; ++i)
            if (economy_detail::may_receive_share(e, player, i, alliances) && p.state != 10 &&
                e.players[static_cast<std::size_t>(i)].state != 10)
                share_map(i);
}

} // namespace sim
