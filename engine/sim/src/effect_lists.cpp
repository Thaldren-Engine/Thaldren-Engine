// The effect particle lists (docs/RE_MEMORY.md, "Battle step", stage 9).

#include "sim/effect_lists.h"

#include <cmath>

#include "sim/heightmap.h"
#include "sim/x87.h"

namespace sim {

namespace {

using i32 = std::int32_t;
using u32 = std::uint32_t;
using i64 = std::int64_t;

// r * range / 0x8000 (64-bit, truncating).
i32 scaled(u32 r, i32 range) { return static_cast<i32>(static_cast<i64>(r) * range / 0x8000); }

// The length of a vector of whole 16.16 numbers as the original takes it: the squares and
// their sum at double precision (the FPU runs at 53 bits), the root truncated.
i32 length(const std::array<i32, 3>& d) {
    const double x = static_cast<double>(d[0]), y = static_cast<double>(d[1]), z = static_cast<double>(d[2]);
    return x87::ftol32(std::sqrt(x * x + y * y + z * z));
}

// Adds `d` to the whole-unit half of a 16.16 coordinate (a 16-bit add).
i32 add_whole(i32 v, i32 d) {
    const u32 hi = static_cast<std::uint16_t>(static_cast<i32>(static_cast<std::int16_t>(static_cast<u32>(v) >> 16)) + d);
    return static_cast<i32>((static_cast<u32>(v) & 0xFFFFu) | (hi << 16));
}

bool expired_signed(i32 expire, u32 tick) { return expire < static_cast<i32>(tick); }

bool should_emit(const FxEmitter& e, u32 tick) {
    if (e.kind == FxKind::Steam) return tick >= e.next_emit;
    return static_cast<i32>(e.next_emit) <= static_cast<i32>(e.deadline) && e.next_emit <= tick;
}

void emit(FxContext& c, FxEmitter& e) {
    EffectLists& fx = c.fx;
    FxParticle p;
    switch (e.kind) {
    case FxKind::Dots:
        p.frame_count = fx.flame_frames - 1;
        p.expire = static_cast<i32>(c.tick + static_cast<u32>(e.flight));
        p.frame = scaled(c.rand.next(), p.frame_count);
        e.particles.push_back(p);
        e.next_emit = c.tick + 10;
        return;
    case FxKind::Flame:
        p.period = e.period;
        p.expire = static_cast<i32>(e.deadline);
        p.frame_count = fx.flame_frames - 1;
        e.particles.push_back(p);
        e.next_emit = c.tick + 1;
        return;
    case FxKind::Wake:
        p.period = e.period;
        p.expire = static_cast<i32>(c.tick + static_cast<u32>(e.period) * 6u);
        p.pos = e.origin;
        for (int k = 0; k < 3; ++k) p.pos[static_cast<std::size_t>(k)] = add_whole(p.pos[static_cast<std::size_t>(k)], scaled(c.rand.next(), 7) - 3);
        p.vel = e.vel;
        e.particles.push_back(p);
        e.next_emit = c.tick + 1;
        return;
    case FxKind::Smoke:
    case FxKind::Steam:
        p.hold_period = p.hold_left = e.hold;
        p.frame_max = scaled(c.rand.next(), e.frames - 2) + 2;
        e.particles.push_back(p);
        e.next_emit = c.tick + static_cast<u32>(e.period);
        return;
    }
}

// One step of an emitter's particles, then its emission when due.
void step_emitter(FxContext& c, FxEmitter& e) {
    auto& ps = e.particles;
    for (std::size_t i = 0; i < ps.size();) {
        FxParticle& p = ps[i];
        bool gone = false;
        switch (e.kind) {
        case FxKind::Dots:
            if (p.frame_count == 0) {
                c.fx.fault = "flame dot frame count of zero (division by zero)";
                return;
            }
            p.frame = (p.frame + 1) % p.frame_count;
            gone = expired_signed(p.expire, c.tick);
            break;
        case FxKind::Flame:
            if (p.period == 0 || p.frame_count == 0) {
                c.fx.fault = "flame period or frame count of zero (division by zero)";
                return;
            }
            p.counter = (p.counter + 1) % p.period;
            if (p.counter == 0) p.frame = (p.frame + 1) % p.frame_count;
            gone = expired_signed(p.expire, c.tick);
            break;
        case FxKind::Wake: {
            for (int k = 0; k < 3; ++k)
                p.pos[static_cast<std::size_t>(k)] = static_cast<i32>(static_cast<u32>(p.pos[static_cast<std::size_t>(k)]) + static_cast<u32>(p.vel[static_cast<std::size_t>(k)]));
            if (p.period == 0) {
                c.fx.fault = "wake period of zero (division by zero)";
                return;
            }
            p.counter = (p.counter + 1) % p.period;
            // Kept while not expired and over water (the ground below the sea level).
            bool keep = false;
            if (static_cast<i32>(c.tick) <= p.expire) {
                const i32 h = c.heights ? c.heights->sample(static_cast<std::int16_t>(static_cast<u32>(p.pos[0]) >> 16),
                                                             static_cast<std::int16_t>(static_cast<u32>(p.pos[2]) >> 16))
                                        : -1;
                keep = h < static_cast<i32>(c.sea_level);
            }
            gone = !keep;
            break;
        }
        case FxKind::Smoke:
        case FxKind::Steam:
            if (--p.hold_left == 0) {
                ++p.frame;
                const i32 h = p.hold_period / 2;
                p.hold_left = scaled(c.rand.next(), h) + h;
            }
            gone = !(p.frame < p.frame_max);
            break;
        }
        if (gone) ps.erase(ps.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    if (should_emit(e, c.tick)) emit(c, e);
}

bool done(const FxEmitter& e, u32 tick) {
    switch (e.kind) {
    case FxKind::Smoke: return e.particles.empty() && e.deadline < tick;
    case FxKind::Steam: return false;
    default: return e.particles.empty();
    }
}

// Takes an object from the pool, sets it up (which emits once) and appends it to its list,
// dropping the list's oldest entry first when it already holds more than 400.
bool add(FxContext& c, FxEmitter e) {
    EffectLists& fx = c.fx;
    if (fx.blocked || fx.live() + c.nano_streams >= EffectLists::kPool) return false;
    emit(c, e);
    auto& list = fx.lists[e.list % EffectLists::kLists];
    if (list.size() > EffectLists::kCap) list.erase(list.begin());
    list.push_back(std::move(e));
    return true;
}

FxEmitter make(FxKind kind, int list, u32 tick, i32 duration) {
    FxEmitter e;
    e.kind = kind;
    e.list = static_cast<std::uint8_t>(list);
    e.deadline = tick + static_cast<u32>(duration);
    return e;
}

bool smoke(FxContext& c, const std::array<i32, 3>& pos, i32 clamp, i32 period, i32 hold, i32 duration, bool second, int list) {
    FxEmitter e = make(FxKind::Smoke, list, c.tick, duration);
    e.origin = pos;
    e.period = period;
    i32 frames = (second ? c.fx.smoke2_frames : c.fx.smoke1_frames) - 1;
    if (clamp != 0 && clamp < frames) frames = clamp;
    e.frames = frames;
    e.hold = hold != 0 ? hold : 7;
    return add(c, std::move(e));
}

}  // namespace

int EffectLists::live() const {
    int n = 0;
    for (const auto& l : lists) n += static_cast<int>(l.size());
    return n;
}

void EffectLists::reset() {
    for (auto& l : lists) l.clear();
    fault.clear();
}

bool fx_smoke(FxContext& c, const std::array<i32, 3>& pos, int list, bool second_smoke) {
    return smoke(c, pos, 0, 1, 0, 0, second_smoke, list);
}

bool fx_smoke_emitter(FxContext& c, const std::array<i32, 3>& pos, i32 period, i32 duration, int list) {
    return smoke(c, pos, 0, period, 0, duration, false, list);
}

bool fx_muzzle_smoke(FxContext& c, const std::array<i32, 3>& pos, int list) { return smoke(c, pos, 3, 1, 0x1E, 0, false, list); }

bool fx_steam(FxContext& c, const std::array<i32, 3>& pos, int list) {
    FxEmitter e = make(FxKind::Steam, list, c.tick, 0x96);
    e.origin = pos;
    e.period = 5;
    e.frames = c.fx.smoke1_frames - 1;
    e.hold = 7;
    return add(c, std::move(e));
}

bool fx_flame(FxContext& c, const std::array<i32, 3>& from, const std::array<i32, 3>& to, i32 param, i32 duration, int list) {
    (void)to;  // the flames' path is a picture
    if (duration == 0) {  // the set-up divides by the duration
        c.fx.fault = "flame of zero duration (division by zero)";
        return false;
    }
    FxEmitter e = make(FxKind::Flame, list, c.tick, duration);
    e.origin = from;
    e.period = param;
    return add(c, std::move(e));
}

bool fx_flame_jittered(FxContext& c, const std::array<i32, 3>& pos, int list) {
    std::array<i32, 3> p = pos;
    for (int k = 0; k < 3; ++k) p[static_cast<std::size_t>(k)] = add_whole(p[static_cast<std::size_t>(k)], scaled(c.rand.next(), 3) - 1);
    const i32 duration = scaled(c.rand.next(), 3) + 1;
    return fx_flame(c, p, p, 1, duration, list);
}

bool fx_wake(FxContext& c, const std::array<i32, 3>& from, const std::array<i32, 3>& to, i32 size, bool rising, int list) {
    (void)rising;  // colours only
    FxEmitter e = make(FxKind::Wake, list, c.tick, 1);
    e.origin = from;
    e.period = size;
    const std::array<i32, 3> d = {static_cast<i32>(static_cast<u32>(to[0]) - static_cast<u32>(from[0])),
                                  static_cast<i32>(static_cast<u32>(to[1]) - static_cast<u32>(from[1])),
                                  static_cast<i32>(static_cast<u32>(to[2]) - static_cast<u32>(from[2]))};
    // Speed 2 units a step: reciprocal of twice the length (16.16), times the difference.
    const i32 len = length(d);
    const i32 twice = static_cast<i32>((static_cast<i64>(len) * 0x20000) >> 16);
    if (twice == 0) {
        c.fx.fault = "wake of zero length (division by zero)";
        return false;
    }
    const i32 r = static_cast<i32>((static_cast<i64>(1) << 32) / twice);
    for (int k = 0; k < 3; ++k) e.vel[static_cast<std::size_t>(k)] = static_cast<i32>((static_cast<i64>(d[static_cast<std::size_t>(k)]) * r) >> 16);
    return add(c, std::move(e));
}

bool fx_dots(FxContext& c, const std::array<i32, 3>& from, const std::array<i32, 3>& to, i32 duration, int list) {
    FxEmitter e = make(FxKind::Dots, list, c.tick, duration);
    e.origin = from;
    const std::array<i32, 3> d = {static_cast<i32>(static_cast<u32>(to[0]) - static_cast<u32>(from[0])),
                                  static_cast<i32>(static_cast<u32>(to[1]) - static_cast<u32>(from[1])),
                                  static_cast<i32>(static_cast<u32>(to[2]) - static_cast<u32>(from[2]))};
    const i64 q = (static_cast<i64>(length(d)) << 16) / 0x50000;
    e.flight = static_cast<std::int16_t>(static_cast<u32>(q) >> 16);
    if (e.flight == 0) {
        c.fx.fault = "flame dots over less than a step (division by zero)";
        return false;
    }
    return add(c, std::move(e));
}

void step_effect_lists(FxContext& c, int first, int last) {
    for (int l = first; l < last && l < EffectLists::kLists; ++l) {
        auto& list = c.fx.lists[static_cast<std::size_t>(l)];
        for (std::size_t i = 0; i < list.size();) {
            if (done(list[i], c.tick)) {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            step_emitter(c, list[i]);
            ++i;
        }
    }
}

}  // namespace sim
