#pragma once
// Weapon fire routines: turn an aimed weapon slot into projectiles
// (see docs/RE_MEMORY.md, "Weapons: fire routines").

#include <cstdint>

#include "sim/projectile.h"
#include "sim/weapon_slot.h"
#include "sim/weapons.h"

namespace sim {

// Runs the fire routine of the unit's weapon slot at `target` (or none) and
// `target_pos`; nonzero when a shot went off. Turret weapons re-check their aim,
// add their accuracy spread and fire a direct or ballistic shot; vertical
// launchers fire a missile (interceptors only at a shot to intercept);
// line-of-sight weapons fire straight from the query piece once the unit faces
// the target; dropped weapons release a bomb with the unit's motion.
std::int32_t fire_weapon(ProjectileWorld& w, Unit& unit, int slot, Unit* target, const WorldPos& target_pos);

} // namespace sim
