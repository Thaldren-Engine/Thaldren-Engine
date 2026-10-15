#include "sim/orders.h"

namespace sim {
namespace {

using u32 = std::uint32_t;

MissionOrder*& list_head(Unit& unit, const MissionOrder& order) {
    return (order.flags & order_flags::kSecondary) ? unit.secondary_orders : unit.orders;
}

// Unlinks `order` from the list starting at `head` if it is there; returns whether it was.
bool unlink_from(MissionOrder*& head, MissionOrder* order) {
    MissionOrder** link = &head;
    for (MissionOrder* p = head; p; p = p->next) {
        if (p == order) {
            *link = order->next;
            return true;
        }
        link = &p->next;
    }
    return false;
}

// Removes a finished order from the list its flags name and destroys it.
// Orders other than the current primary head are marked so that their
// teardown leaves the weapon targets alone.
void finish_order(OrderContext& c, Unit& unit, MissionOrder* order) {
    MissionOrder* const current = unit.orders;
    if (!unlink_from(list_head(unit, *order), order)) return;
    if (order != current) order->flags |= order_flags::kNotCurrent;
    destroy_order(c, order);
}

void sleep_random(OrderContext& c, MissionOrder& order, u32 range) {
    const u32 r = c.rng.next_mod(range);
    order.wait |= order_wait::kTimer;
    order.wake_tick = c.game_tick + r + 30;
}

bool plays(const PlayerSlot& p) { return p.active && (p.controller == 1 || p.controller == 2); }

}  // namespace

const std::array<std::uint32_t, mission::kCount>& mission::default_flags() {
    static const std::array<std::uint32_t, kCount> flags = {
        0x0,       0x10060,  0x600,     0x200,     0x200,     0x200,    0x280,     0x600,     0x280,     0x680,
        0x4,       0x24,     0x10010C,  0xC0140,   0x200,     0x10060,  0x10060,   0x10060,   0x200,     0x224,
        0x200,     0x400,    0x20,      0x100208,  0x4,       0x100508, 0x402,     0x24,      0x0,       0x412,
        0x400,     0x400,    0x100800,  0x100200,  0x412,     0x100200, 0x200,     0x200,     0x40040,   0x0,
        0x1000204, 0x20000,  0x1020000, 0x10060,   0x10060,   0x0,      0x410,     0x600,     0x0,       0x200,
        0x200,     0x100208, 0x400,     0x600,     0x100508,  0x402,    0x412,     0x200,     0x100800,  0x100200,
        0x412,     0x100200, 0x600,     0x600,     0x20000,   0x400,    0x4,       0x204,
    };
    return flags;
}

void unlink_unit(UnitLink& link) {
    if (link.unit) {
        UnitLink** at = &link.unit->referrers;
        while (*at != &link) at = &(*at)->next;
        *at = link.next;
        link.unit = nullptr;
        link.next = nullptr;
    }
}

void release_referrers(Unit& unit) {
    // Newest link first: its owner (an order) hears event 8, then the link lets go of the
    // unit, unless the owner pointed it somewhere else meanwhile.
    while (UnitLink* l = unit.referrers) {
        Unit* was = l->unit;
        if (l->owner) l->owner->events |= 8u;
        if (l->unit == was && was) {
            unlink_unit(*l);
        } else if (unit.referrers == l) {
            break;  // a link that no longer points here but heads the list: the original would loop forever
        }
    }
}

void relink_unit(OrderContext& c, UnitLink& link, Unit* unit) {
    unlink_unit(link);
    if (unit && c.ticks[unit->id].type_index != 0) {
        link.unit = unit;
        link.next = unit->referrers;
        unit->referrers = &link;
        return;
    }
    link.unit = nullptr;
    link.next = nullptr;
}

void construct_order(OrderContext& c, MissionOrder& o, std::uint8_t kind, Unit* target, const std::int32_t* pos,
                     std::int32_t param0, std::int32_t param1, std::int32_t param2) {
    o.kind = kind;
    o.target.unit = nullptr;
    o.target.next = nullptr;
    if (target && c.ticks[target->id].type_index != 0) {
        o.target.unit = target;
        o.target.next = target->referrers;
        target->referrers = &o.target;
    }
    o.scratch0 = 0;
    o.scratch1 = 0;
    o.param0 = param0;
    o.param1 = param1;
    o.param2 = param2;
    o.created_tick = c.game_tick;
    o.target.owner = &o;
    o.step = 0;
    o.wait = 0;
    o.events = 0;
    o.wake_tick = 0xFFFFFFFFu;
    o.pos = pos ? std::array<std::int32_t, 3>{pos[0], pos[1], pos[2]} : std::array<std::int32_t, 3>{};
    o.unit = nullptr;
    o.flags = mission::default_flags()[kind];
    o.next = nullptr;
    o.attached = 0;
    if (!target) o.flags &= ~order_flags::kHasTarget;
    if (!pos) o.flags &= ~order_flags::kHasPosition;
    if (!(o.flags & order_flags::kHasTarget)) relink_unit(c, o.target, nullptr);
}

void destroy_order(OrderContext& c, MissionOrder* o) {
    Unit& unit = *o->unit;
    if (o->wait & order_wait::kNotifyDestroy) {
        std::uint32_t event = 2;
        c.systems.run_mission(unit, *o, event);
    }
    if (o->flags & order_flags::kBuilding) {
        const int fn = unit.script->program().find_function("StopBuilding");
        unit.script->call_with_args(fn, nullptr, false, 0, 0, 0, 0, 0);
        c.systems.broadcast_script_call(unit, static_cast<std::int16_t>(fn));
        o->flags &= ~order_flags::kBuilding;
    }
    if (c.ticks[unit.id].has_motion) c.systems.release_attached(unit, *o);
    if (!(o->flags & order_flags::kNotCurrent)) c.systems.release_weapon_targets(*o->unit);
    unlink_unit(o->target);
    c.systems.free_order(o);
}

void insert_order(Unit& unit, MissionOrder& order, MissionOrder* before) {
    MissionOrder** link = &list_head(unit, order);
    while (*link != before) link = &(*link)->next;
    *link = &order;
    order.unit = &unit;
    order.next = before;
    if (before) order.flags |= before->flags & order_flags::kIdle;
}

void prepend_order(Unit& unit, MissionOrder& order) { insert_order(unit, order, list_head(unit, order)); }

void remove_order(OrderContext& c, Unit& unit, MissionOrder* order) { finish_order(c, unit, order); }

void clear_orders(OrderContext& c, Unit& unit, bool all) {
    MissionOrder* const first = unit.orders;
    MissionOrder** link = &unit.orders;
    for (MissionOrder* p = *link; p; p = *link) {
        if (!all && (p->flags & order_flags::kKeep)) {
            link = &p->next;
            continue;
        }
        *link = p->next;
        if (p != first) p->flags |= order_flags::kNotCurrent;
        destroy_order(c, p);
    }
    if (!all) return;
    // A secondary order whose flags name the primary list is never found there,
    // so this would not end; the original hangs the same way.
    while (unit.secondary_orders) finish_order(c, unit, unit.secondary_orders);
}

void add_paralysis(OrderContext& c, Unit& unit, std::int32_t amount) {
    if (unit.orders && unit.orders->kind == mission::kParalyze) {
        unit.orders->param0 += amount;
        return;
    }
    MissionOrder* o = c.systems.allocate_order();
    construct_order(c, *o, mission::kParalyze, nullptr, nullptr, amount, 0, 0);
    prepend_order(unit, *o);
}

void run_primary_orders(OrderContext& c, Unit& unit) {
    for (MissionOrder* o = unit.orders; o; o = unit.orders) {
        if (c.game_tick >= o->wake_tick) {
            o->wake_tick = 0xFFFFFFFFu;
            o->events |= order_wait::kTimer;
        }
        const u32 woken = (o->events | unit.net_dirty) & o->wait;
        if (o->wait != 0 && woken == 0) return;  // still asleep
        unit.net_dirty = static_cast<std::uint16_t>(unit.net_dirty & ~woken);
        o->wait = 0;
        o->events &= ~woken;
        if (woken & order_wait::kClearWeapons)
            for (int slot = 0; slot < 3; ++slot) c.systems.clear_weapon_target(unit, slot);
        std::uint32_t event = woken;
        switch (static_cast<u32>(c.systems.run_mission(*o->unit, *o, event))) {
        case 0: o->step = 0; break;
        case 1: ++o->step; break;
        case 2:
        case 4: break;
        case 3: sleep_random(c, *o, 15); break;
        case 5:
        case 8:
            // An order whose flags name the other list is not found and runs again.
            finish_order(c, unit, o);
            break;
        case 6: {
            // To the back of the primary list (it is the head).
            MissionOrder** link = &unit.orders;
            while (*link != o) link = &(*link)->next;
            *link = o->next;
            while (*link) link = &(*link)->next;
            *link = o;
            o->next = nullptr;
            break;
        }
        case 7: {
            MissionOrder* const first = unit.orders;
            for (MissionOrder* p = first; p; p = unit.orders) {
                unit.orders = p->next;
                if (p != first) p->flags |= order_flags::kNotCurrent;
                destroy_order(c, p);
            }
            while (unit.secondary_orders) remove_order(c, unit, unit.secondary_orders);
            return;
        }
        case 9:
            o->flags |= order_flags::kLastOrder;
            if (!o->next) {
                o->step = 0;
                sleep_random(c, *o, 30);
            } else {
                finish_order(c, unit, o);
            }
            break;
        default:
            clear_orders(c, unit, true);
            return;
        }
    }
    // Nothing to do: a playing unit falls back to its type's default mission.
    const UnitTickState& t = c.ticks[unit.id];
    const std::uint8_t kind = t.tick_type ? t.tick_type->default_mission : 0;
    if (!plays(c.players[static_cast<std::size_t>(t.owner)]) || kind == 0) return;
    MissionOrder* o = c.systems.allocate_order();
    construct_order(c, *o, kind, nullptr, nullptr, 0, 0, 0);
    o->flags |= order_flags::kIdle;
    insert_order(unit, *o, (o->flags & order_flags::kSecondary) ? unit.secondary_orders : unit.orders);
}

void run_secondary_orders(OrderContext& c, Unit& unit, std::uint32_t& stale_word) {
    MissionOrder* o = unit.secondary_orders;
    while (o) {
        if (o->wait != 0 && c.game_tick < o->wake_tick) {
            o = o->next;
            continue;
        }
        o->wait = 0;
        std::uint32_t event = 0;
        const auto result = static_cast<u32>(c.systems.run_mission(*o->unit, *o, event));
        stale_word = event;
        switch (result) {
        case 0: o->step = 0; break;
        case 1: ++o->step; break;
        case 2:
        case 4: break;
        case 3:
            sleep_random(c, *o, 15);
            stale_word = 15;
            break;
        case 6:
        case 7: {
            MissionOrder* const current = unit.orders;
            if (!unlink_from(list_head(unit, *o), o)) return;
            if (o != current) o->flags |= order_flags::kNotCurrent;
            const std::uint32_t address = o->address;
            destroy_order(c, o);
            stale_word = address;
            return;
        }
        default: {  // 5, 8, 9 and unknown results
            MissionOrder* const current = unit.orders;
            if (unlink_from(list_head(unit, *o), o)) {
                if (o != current) o->flags |= order_flags::kNotCurrent;
                const std::uint32_t address = o->address;
                destroy_order(c, o);
                stale_word = address;
            }
            break;
        }
        }
        o = unit.secondary_orders;  // start over from the head
    }
}

} // namespace sim
