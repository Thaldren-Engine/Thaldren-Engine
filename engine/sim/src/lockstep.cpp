// Lockstep play, command logs and state checksums (docs/RE_MEMORY.md, "Network and sync").

#include "sim/lockstep.h"

#include <cstring>

#include "sim/battle.h"

namespace sim {

namespace {

// FNV-1a over 32-bit words.
struct Sum {
    std::uint32_t h = 0x811C9DC5u;
    void add(std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            h ^= (v >> (8 * k)) & 0xFF;
            h *= 0x01000193u;
        }
    }
    void addf(float f) {
        std::uint32_t b;
        std::memcpy(&b, &f, 4);
        add(b);
    }
    void addi(std::int32_t v) { add(static_cast<std::uint32_t>(v)); }
};

void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) o.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
bool get32(const std::vector<std::uint8_t>& in, std::size_t& at, std::uint32_t& v) {
    if (at + 4 > in.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(in[at++]) << (8 * k);
    return true;
}

}  // namespace

std::uint32_t state_checksum(Battle& b) {
    Sum s;
    s.add(b.game_tick());
    s.add(b.crt_rand.state());
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        const Unit& u = b.units[i];
        const UnitTickState& t = b.ticks[i];
        if (t.type_index == 0 && !(u.flags & unit_flags::kAlive)) continue;
        s.add(static_cast<std::uint32_t>(i));
        s.add(t.type_index);
        for (std::int32_t v : u.pos) s.addi(v);
        s.add(static_cast<std::uint16_t>(u.pitch) | (static_cast<std::uint32_t>(u.heading) << 16));
        s.add(static_cast<std::uint16_t>(u.bank) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(u.hit_points)) << 16));
        s.add(u.flags & ~unit_tick_flags::kSelected);  // selection is per machine
        s.add(u.activate_flags | (u.script_flags << 8));
        s.addf(u.build_fraction_left);
        for (const MissionOrder* o = u.orders; o; o = o->next) {
            s.add(o->kind | (o->step << 8));
            for (std::int32_t v : o->pos) s.addi(v);
            s.add(o->target.unit ? o->target.unit->id : 0);
        }
        if (t.has_motion)
            for (std::int32_t v : t.velocity) s.addi(v);
    }
    for (const Projectile& p : b.projectiles.slots) {
        if (!p.weapon) continue;
        for (std::int32_t v : p.pos) s.addi(v);
        for (std::int32_t v : p.vel) s.addi(v);
    }
    for (const PlayerSlot& p : b.players) {
        s.addf(p.energy_stored);
        s.addf(p.metal_stored);
        s.add(p.state | (static_cast<std::uint32_t>(p.alive_count) << 8));
    }
    return s.h;
}

// --- CommandLog

void CommandLog::encode(std::vector<std::uint8_t>& out) const {
    put32(out, 0x474C4D43u);  // "CMLG"
    put32(out, static_cast<std::uint32_t>(commands.size()));
    for (const command::Timed& t : commands) {
        put32(out, t.tick);
        command::encode(t.command, out);
    }
    put32(out, static_cast<std::uint32_t>(checksums.size()));
    for (const auto& [tick, sum] : checksums) {
        put32(out, tick);
        put32(out, sum);
    }
}

bool CommandLog::decode(const std::vector<std::uint8_t>& in) {
    std::size_t at = 0;
    std::uint32_t magic = 0, n = 0;
    if (!get32(in, at, magic) || magic != 0x474C4D43u || !get32(in, at, n)) return false;
    commands.clear();
    checksums.clear();
    for (std::uint32_t k = 0; k < n; ++k) {
        command::Timed t;
        if (!get32(in, at, t.tick) || !command::decode(in, at, t.command)) return false;
        commands.push_back(std::move(t));
    }
    if (!get32(in, at, n)) return false;
    for (std::uint32_t k = 0; k < n; ++k) {
        std::uint32_t tick = 0, sum = 0;
        if (!get32(in, at, tick) || !get32(in, at, sum)) return false;
        checksums[tick] = sum;
    }
    return true;
}

bool Replayer::step(Battle& battle, std::uint32_t check_every) {
    const std::uint32_t due = battle.game_tick() + 1;
    while (next_ < log_.commands.size() && log_.commands[next_].tick <= due) command::apply(battle, log_.commands[next_++].command);
    battle.step();
    if (check_every && battle.game_tick() % check_every == 0) {
        auto it = log_.checksums.find(battle.game_tick());
        if (it != log_.checksums.end() && it->second != state_checksum(battle)) {
            mismatch_tick = battle.game_tick();
            return false;
        }
    }
    return true;
}

// --- Turn messages

void TurnMessage::encode(std::vector<std::uint8_t>& out) const {
    out.push_back(player);
    put32(out, tick);
    put32(out, checksum_tick);
    put32(out, checksum);
    put32(out, static_cast<std::uint32_t>(commands.size()));
    for (const command::Command& c : commands) command::encode(c, out);
}

bool TurnMessage::decode(const std::vector<std::uint8_t>& in) {
    std::size_t at = 0;
    if (in.empty()) return false;
    player = in[at++];
    std::uint32_t n = 0;
    if (!get32(in, at, tick) || !get32(in, at, checksum_tick) || !get32(in, at, checksum) || !get32(in, at, n)) return false;
    commands.clear();
    for (std::uint32_t k = 0; k < n; ++k) {
        command::Command c;
        if (!command::decode(in, at, c)) return false;
        commands.push_back(std::move(c));
    }
    return at == in.size();
}

// --- Lockstep

Lockstep::Lockstep(std::uint16_t players, std::uint8_t local, std::uint32_t delay) : players_(players), local_(local), delay_(delay) {}

void Lockstep::queue_local(const command::Command& c) {
    pending_local_.push_back(c);
    pending_local_.back().player = local_;
}

void Lockstep::send_turn(Battle& battle, const std::function<void(const std::vector<std::uint8_t>&)>& send) {
    // The first `delay` turns carry nothing (sent once, at the start).
    const std::uint32_t now = battle.game_tick();
    while (sent_upto_ < now + delay_) {
        ++sent_upto_;
        TurnMessage m;
        m.player = local_;
        m.tick = sent_upto_;
        if (sent_upto_ == now + delay_) m.commands.swap(pending_local_);
        if (sent_upto_ > delay_ + 1) {
            m.checksum_tick = sent_upto_ - delay_ - 1;
            auto it = own_checksums_.find(m.checksum_tick);
            if (it != own_checksums_.end()) m.checksum = it->second;
            else m.checksum_tick = 0;
        }
        std::vector<std::uint8_t> bytes;
        m.encode(bytes);
        receive(bytes);  // our own turn goes through the same path
        send(bytes);
    }
}

bool Lockstep::receive(const std::vector<std::uint8_t>& message) {
    TurnMessage m;
    if (!m.decode(message) || m.player >= 10) return false;
    if (m.checksum_tick) {
        auto it = own_checksums_.find(m.checksum_tick);
        if (it != own_checksums_.end() && it->second != m.checksum && !desync_tick_) desync_tick_ = m.checksum_tick;
    }
    turns_[m.tick][m.player] = std::move(m);
    return true;
}

bool Lockstep::try_step(Battle& battle) {
    const std::uint32_t due = battle.game_tick() + 1;
    auto it = turns_.find(due);
    if (it == turns_.end()) return false;
    for (int p = 0; p < 10; ++p)
        if ((players_ >> p) & 1)
            if (!it->second.count(static_cast<std::uint8_t>(p))) return false;
    // In player order.
    for (auto& [player, m] : it->second) {
        // A remote checksum that arrived before ours was computed is checked when ours is.
        for (const command::Command& c : m.commands) {
            command::apply(battle, c);
            log.commands.push_back({due, c});
        }
    }
    std::map<std::uint8_t, TurnMessage> done = std::move(it->second);
    turns_.erase(it);
    battle.step();
    const std::uint32_t sum = state_checksum(battle);
    own_checksums_[battle.game_tick()] = sum;
    log.checksums[battle.game_tick()] = sum;
    // Remote checksums for this tick that came early.
    for (auto& [tick, by_player] : turns_)
        for (auto& [player, m] : by_player)
            if (m.checksum_tick == battle.game_tick() && m.checksum != sum && !desync_tick_) desync_tick_ = m.checksum_tick;
    (void)done;
    return true;
}

}  // namespace sim
