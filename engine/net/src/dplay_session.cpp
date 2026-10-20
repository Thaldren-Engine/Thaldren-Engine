#include "net/dplay_session.h"

#include <chrono>
#include <cstring>
#include <random>

namespace net::dplay {
namespace {

std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint32_t random32() {
    static std::random_device rd;
    static std::mt19937 g(rd());
    return static_cast<std::uint32_t>(g());
}

Guid random_guid() {
    Guid g;
    for (std::size_t i = 0; i < 16; i += 4) {
        const std::uint32_t r = random32();
        std::memcpy(g.b.data() + i, &r, 4);
    }
    g.b[7] = static_cast<std::uint8_t>((g.b[7] & 0x0F) | 0x40);  // a version 4 (random) id
    g.b[8] = static_cast<std::uint8_t>((g.b[8] & 0x3F) | 0x80);
    return g;
}

}  // namespace

Guid game_application() { return Guid::parse("99797420-f5f5-11cf-9827-00a0241496c8"); }

Session::Session(std::unique_ptr<Provider> provider, std::function<void(const std::string&)> log)
    : provider_(std::move(provider)), log_(std::move(log)) {}

Session::~Session() { close(); }

void Session::log_msg(const char* dir, const Bytes& who, const Bytes& payload) {
    if (!log_) return;
    std::string line = std::string(dir) + " " + provider_->describe(who) + " ";
    if (is_system(payload.data(), payload.size())) {
        const std::uint16_t c = static_cast<std::uint16_t>(payload[4] | (payload[5] << 8));
        line += std::string("system ") + command_name(c) + " (" + std::to_string(c) + ")";
    } else if (payload.size() >= 8) {
        std::uint32_t f = 0, t = 0;
        std::memcpy(&f, payload.data(), 4);
        std::memcpy(&t, payload.data() + 4, 4);
        char b[64];
        std::snprintf(b, sizeof b, "game from %08x to %08x", f, t);
        line += b;
    }
    line += " " + std::to_string(payload.size()) + " bytes " + hex(payload.data(), payload.size());
    log_(line);
}

bool Session::host(const HostSettings& s, std::string& error) {
    if (!provider_->listen(error)) {
        state_ = State::Failed;
        error_ = error;
        return false;
    }
    desc_ = SessionDesc{};
    desc_.instance = random_guid();
    desc_.application = s.application;
    desc_.max_players = s.max_players;
    desc_.reserved1 = random32();  // the key every player id is XORed with
    for (int i = 0; i < 4; ++i) desc_.user[static_cast<std::size_t>(i)] = s.user[i];
    desc_.name = s.session_name;
    desc_.password = s.password;
    if (!s.password.empty()) desc_.flags |= kPasswordRequired;
    password_ = s.password;
    next_index_ = 0;
    // The host's system player: index 0 (as the original's host numbers it).
    Player sys;
    sys.flags = kSystemPlayer | kNameServer | kInSession | kSenderLocal;
    sys.id = next_id();
    sys.system_id = sys.id;
    sys.sp_data = provider_->local_sp_data();
    system_id_ = sys.id;
    players_[sys.id] = Entry{sys, true};
    state_ = State::Hosting;
    return true;
}

std::uint32_t Session::next_id() { return make_player_id(desc_.reserved1, next_index_++); }

void Session::find(const Guid& application, const std::string& password) {
    if (state_ == State::Idle || state_ == State::Finding || state_ == State::Failed) state_ = State::Finding;
    find_application_ = application;
    const Bytes p = enum_sessions(application, kEnumAvailable | 0x80, password);
    log_msg("enum ->", {}, p);
    provider_->enumerate(p);
}

bool Session::join(const FoundSession& s, const std::string& password) {
    desc_ = s.desc;
    host_address_ = s.address;
    password_ = password;
    state_ = State::Joining;
    pending_.push_back(Pending{kSystemPlayer | kSenderLocal, {}, {}});
    send_system(host_address_, request_player_id(kSystemPlayer | kSenderLocal));
    return true;
}

void Session::close() {
    if (state_ == State::Hosting || state_ == State::Joined) {
        // Each local game player, then the machine itself, leaves.
        std::vector<std::uint32_t> mine;
        for (const auto& [id, e] : players_)
            if (e.local && !e.p.system()) mine.push_back(id);
        for (std::uint32_t id : mine) to_every_machine(delete_player(0, id));
        if (system_id_) to_every_machine(delete_player(0, system_id_));
        for (int i = 0; i < 20; ++i) provider_->pump();
    }
    state_ = State::Closed;
}

std::vector<Player> Session::players() const {
    std::vector<Player> out;
    for (const auto& [id, e] : players_) out.push_back(e.p);
    return out;
}

const Player* Session::player(std::uint32_t id) const {
    auto it = players_.find(id);
    return it == players_.end() ? nullptr : &it->second.p;
}

const Session::Entry* Session::machine_of(std::uint32_t player) const {
    auto it = players_.find(player);
    if (it == players_.end()) return nullptr;
    auto m = players_.find(it->second.p.system_id);
    return m == players_.end() ? nullptr : &m->second;
}

void Session::send_system(const Bytes& to, const Bytes& payload) {
    ++counters_.system_sent;
    log_msg("->", to, payload);
    provider_->send(to, payload, true);
}

void Session::to_every_machine(const Bytes& payload) {
    for (const auto& [id, e] : players_)
        if (e.p.system() && id != system_id_) send_system(e.p.sp_data, payload);
}

void Session::count_players() {
    std::uint32_t n = 0;
    for (const auto& [id, e] : players_)
        if (!e.p.system()) ++n;
    desc_.current_players = n;
}

void Session::add_player(const Player& in, bool local, const Bytes& reply_to) {
    Player p = in;
    if (!local) {
        p.flags = (p.flags & ~kSenderLocal) | kInSession;
        p.sp_data = provider_->complete_sp_data(p.sp_data, reply_to);
    }
    // A player of a machine not yet known: the machine joins the table with the player's
    // provider data (a machine that joined after this one introduces itself this way).
    if (!p.system() && p.system_id && !players_.count(p.system_id)) {
        Player m;
        m.flags = kSystemPlayer | kInSession;
        m.id = p.system_id;
        m.system_id = p.system_id;
        m.sp_data = p.sp_data;
        players_[m.id] = Entry{m, false};
    }
    const bool fresh = !players_.count(p.id);
    players_[p.id] = Entry{p, local};
    count_players();
    if (fresh && !p.system()) {
        SessionEvent e;
        e.kind = SessionEvent::Kind::PlayerCreated;
        e.id = static_cast<std::int32_t>(p.id);
        e.local = local;
        e.short_name = p.short_name;
        e.long_name = p.long_name;
        e.data = p.player_data;
        emit(std::move(e));
    }
}

void Session::remove_player(std::uint32_t id, bool tell_others) {
    auto it = players_.find(id);
    if (it == players_.end()) return;
    const Player p = it->second.p;
    if (p.system()) {
        // The machine leaves: its players with it.
        std::vector<std::uint32_t> theirs;
        for (const auto& [pid, e] : players_)
            if (!e.p.system() && e.p.system_id == id) theirs.push_back(pid);
        for (std::uint32_t pid : theirs) remove_player(pid, tell_others);
    }
    players_.erase(id);
    count_players();
    if (tell_others && state_ == State::Hosting) to_every_machine(delete_player(0, id));
    if (!p.system()) {
        SessionEvent e;
        e.kind = SessionEvent::Kind::PlayerDestroyed;
        e.id = static_cast<std::int32_t>(id);
        e.short_name = p.short_name;
        e.long_name = p.long_name;
        emit(std::move(e));
    }
}

std::int32_t Session::create_player(const std::string& name, const std::vector<std::uint8_t>& data) {
    if (state_ == State::Hosting) {
        Player p;
        p.flags = kSenderLocal | kInSession;
        p.id = next_id();
        p.system_id = system_id_;
        p.short_name = name;
        p.long_name = name;
        p.sp_data = provider_->local_sp_data();
        p.player_data = data;
        add_player(p, true, {});
        Player wire = p;
        wire.flags = kSenderLocal;
        to_every_machine(net::dplay::create_player(0, wire));
        return static_cast<std::int32_t>(p.id);
    }
    if (state_ != State::Joined) return -1;
    pending_.push_back(Pending{kSenderLocal, name, data});
    send_system(host_address_, request_player_id(kSenderLocal));
    return 0;
}

void Session::destroy_player(std::int32_t id) {
    auto it = players_.find(static_cast<std::uint32_t>(id));
    if (it == players_.end() || !it->second.local || it->second.p.system()) return;
    to_every_machine(delete_player(0, static_cast<std::uint32_t>(id)));
    remove_player(static_cast<std::uint32_t>(id), false);
}

std::string Session::player_name(std::int32_t id) const {
    const Player* p = player(static_cast<std::uint32_t>(id));
    if (!p) return {};
    return !p->short_name.empty() ? p->short_name : p->long_name;
}

void Session::set_session_info(const std::string& name, const std::uint32_t user[4], bool closed) {
    if (state_ != State::Hosting) return;
    SessionDesc d = desc_;
    d.name = name;
    for (int i = 0; i < 4; ++i) d.user[static_cast<std::size_t>(i)] = user[i];
    d.flags = closed ? (d.flags | kJoinDisabled) : (d.flags & ~kJoinDisabled);
    if (d.name == desc_.name && d.user == desc_.user && d.flags == desc_.flags) return;
    desc_ = d;
    send_desc_change();
}

void Session::send_desc_change() { to_every_machine(session_desc_changed(0, desc_)); }

void Session::send(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) {
    if (!connected()) return;
    const Bytes m = game_message(static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to), bytes.data(), bytes.size());
    ++counters_.sent;
    if (to == 0) {
        for (const auto& [id, e] : players_)
            if (e.p.system() && id != system_id_) {
                log_msg("->", e.p.sp_data, m);
                provider_->send(e.p.sp_data, m, true);
            }
        return;
    }
    const Entry* mach = machine_of(static_cast<std::uint32_t>(to));
    if (!mach || mach->local) return;
    log_msg("->", mach->p.sp_data, m);
    provider_->send(mach->p.sp_data, m, true);
}

void Session::send_unguaranteed(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) {
    if (!connected()) return;
    const Bytes m = game_message(static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to), bytes.data(), bytes.size());
    ++counters_.sent;
    for (const auto& [id, e] : players_) {
        if (!e.p.system() || id == system_id_) continue;
        if (to != 0) {
            const Entry* mach = machine_of(static_cast<std::uint32_t>(to));
            if (!mach || mach->p.id != id) continue;
        }
        log_msg("~>", e.p.sp_data, m);
        provider_->send(e.p.sp_data, m, false);
    }
}

bool Session::receive(net::Message& out) {
    if (inbox_.empty()) pump();
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

bool Session::next_event(SessionEvent& e) {
    if (events_.empty()) return false;
    e = std::move(events_.front());
    events_.pop_front();
    return true;
}

void Session::pump() {
    provider_->pump();
    Inbound in;
    while (provider_->receive(in)) handle(in);
    Bytes who;
    while (provider_->lost(who)) {
        // The machine at that address is gone: its players leave.
        std::vector<std::uint32_t> gone;
        for (const auto& [id, e] : players_)
            if (e.p.system() && id != system_id_ && provider_->same_machine(e.p.sp_data, who)) gone.push_back(id);
        for (std::uint32_t id : gone) {
            if (log_) log_("lost machine " + provider_->describe(who));
            const bool host_gone = (players_[id].p.flags & kNameServer) != 0;
            remove_player(id, true);
            if (host_gone && state_ == State::Joined) {
                SessionEvent e;
                e.kind = SessionEvent::Kind::Lost;
                emit(std::move(e));
            }
        }
        if (state_ == State::Joining && provider_->same_machine(host_address_, who)) {
            state_ = State::Failed;
            error_ = "the host closed the connection";
        }
    }
    if (!provider_->error().empty() && error_.empty()) error_ = provider_->error();
}

void Session::packet2(const Inbound& in) {
    // A piece of a reliable message: put in place when it is the next one expected, always
    // acknowledged (the sender sends it again until it is); the whole message is then handled
    // as if it had come by itself.
    const Bytes& p = in.payload;
    auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, p.data() + at, 4);
        return v;
    };
    if (p.size() < 0x30) return;
    std::array<std::uint8_t, 16> id{};
    std::memcpy(id.data(), p.data() + 8, 16);
    const std::uint32_t index = u32(0x18), size = u32(0x1C), offset = u32(0x20), count = u32(0x24), total = u32(0x28);
    if (p.size() - 0x30 < size || total > 0x100000 || offset > total || total - offset < size) return;
    Reassembly& r = reassembly_[id];
    if (r.message.empty() && r.next == 0) {
        r.message.assign(total, 0);
        r.count = count;
    }
    if (r.message.size() != total) return;
    if (index == r.next) {
        std::memcpy(r.message.data() + offset, p.data() + 0x30, size);
        ++r.next;
    }
    Bytes ack(p.begin(), p.begin() + 0x1C);
    ack[4] = static_cast<std::uint8_t>(cmd::kPacket2Ack);
    ack[5] = 0;
    send_system(in.reply_to, ack);
    if (r.next == r.count && !r.delivered) {
        r.delivered = true;
        Inbound whole = in;
        whole.payload = r.message;
        handle(whole);
    }
}

void Session::handle(const Inbound& in) {
    const Bytes& p = in.payload;
    if (is_system(p.data(), p.size()) && p.size() >= 8 && (p[4] | (p[5] << 8)) == cmd::kPacket2Data) {
        ++counters_.system_received;
        log_msg("<-", in.reply_to, p);
        packet2(in);
        return;
    }
    if (is_system(p.data(), p.size()) && p.size() >= 8 && (p[4] | (p[5] << 8)) == cmd::kPacket2Ack) return;
    if (is_system(p.data(), p.size())) {
        ++counters_.system_received;
        log_msg("<-", in.reply_to, p);
        if (auto m = decode(p.data(), p.size())) handle_system(*m, in);
        else if (log_) log_("   (malformed)");
        return;
    }
    if (p.size() < 8) return;
    std::uint32_t from = 0, to = 0;
    std::memcpy(&from, p.data(), 4);
    std::memcpy(&to, p.data() + 4, 4);
    log_msg(in.datagram ? "<~" : "<-", in.reply_to, p);
    if (!connected()) return;
    // Game messages: from a remote machine's player, to everybody or to a local player.
    auto f = players_.find(from);
    if (f == players_.end() || f->second.local) return;
    if (to != 0) {
        auto t = players_.find(to);
        if (t == players_.end() || !t->second.local) return;
    }
    ++counters_.received;
    net::Message m;
    m.from = static_cast<std::int32_t>(from);
    m.to = static_cast<std::int32_t>(to);
    m.bytes.assign(p.begin() + 8, p.end());
    inbox_.push_back(std::move(m));
}

void Session::handle_system(const Message& m, const Inbound& in) {
    switch (m.command) {
    case cmd::kEnumSessions: {
        if (state_ != State::Hosting) break;
        if (!m.application.null() && m.application != desc_.application) break;
        const bool full = desc_.current_players >= desc_.max_players;
        if ((m.flags & kEnumAvailable) && ((desc_.flags & (kJoinDisabled | kNewPlayersDisabled)) || full)) break;
        send_system(in.reply_to, enum_sessions_reply(desc_));
        break;
    }
    case cmd::kEnumSessionsReply: {
        if (state_ != State::Finding) break;
        if (!find_application_.null() && m.desc.application != find_application_) break;
        bool known = false;
        for (FoundSession& f : found_)
            if (f.desc.instance == m.desc.instance) {
                f.desc = m.desc;
                f.address = in.reply_to;
                f.seen_ms = now_ms();
                known = true;
            }
        if (!known) found_.push_back(FoundSession{m.desc, in.reply_to, now_ms()});
        break;
    }
    case cmd::kRequestPlayerId: {
        if (state_ != State::Hosting) break;
        const std::uint32_t id = next_id();
        send_system(in.reply_to, request_player_reply(id, kOk));
        break;
    }
    case cmd::kRequestPlayerReply: {
        if (pending_.empty()) break;
        const Pending want = pending_.front();
        pending_.pop_front();
        if (m.result != kOk) {
            if (want.flags & kSystemPlayer) {
                state_ = State::Failed;
                result_ = m.result;
                error_ = "the host gave no player id";
            }
            break;
        }
        Player p;
        p.id = m.id;
        p.sp_data = provider_->local_sp_data();
        if (want.flags & kSystemPlayer) {
            // This machine's system player: ask to be added to the session.
            p.flags = kSystemPlayer | kSenderLocal;
            p.system_id = m.id;
            system_id_ = m.id;
            players_[p.id] = Entry{p, true};
            send_system(host_address_, add_forward_request(0, p, password_, desc_.reserved1));
        } else {
            p.flags = kSenderLocal;
            p.system_id = system_id_;
            p.short_name = want.name;
            p.long_name = want.name;
            p.player_data = want.data;
            add_player(p, true, {});
            to_every_machine(net::dplay::create_player(0, p));
        }
        break;
    }
    case cmd::kAddForwardRequest: {
        if (state_ != State::Hosting) break;
        if (password_ != m.password) {
            send_system(in.reply_to, add_forward_reply(kErrInvalidPassword));
            break;
        }
        if (desc_.flags & (kJoinDisabled | kNewPlayersDisabled)) {
            send_system(in.reply_to, add_forward_reply(kErrAccessDenied));
            break;
        }
        Player sys = m.player;
        sys.flags = kSystemPlayer | kInSession;
        sys.system_id = sys.id;
        sys.sp_data = provider_->complete_sp_data(sys.sp_data, in.reply_to);
        // The machines already in the session hear of the newcomer.
        Player wire = sys;
        to_every_machine(net::dplay::create_player(0, wire));
        players_[sys.id] = Entry{sys, false};
        // The newcomer gets the table: itself first, then the others (as the original's host
        // lists them).
        std::vector<Player> table{sys};
        for (const auto& [id, e] : players_)
            if (id != sys.id) table.push_back(e.p);
        send_system(sys.sp_data, super_enum_players_reply(desc_, table));
        break;
    }
    case cmd::kAddForwardReply:
        if (state_ == State::Joining && m.result != kOk) {
            state_ = State::Failed;
            result_ = m.result;
            error_ = m.result == kErrInvalidPassword ? "wrong password" : "the host refused to add this machine";
        }
        break;
    case cmd::kSuperEnumPlayersReply: {
        if (state_ != State::Joining) break;
        desc_ = m.desc;
        for (const Player& p : m.players) {
            if (p.id == system_id_) continue;
            add_player(p, false, host_address_);
        }
        state_ = State::Joined;
        break;
    }
    case cmd::kCreatePlayer:
    case cmd::kAddForward: {
        if (!connected()) break;
        if (m.player.id == 0 || m.player.id == system_id_ || players_.count(m.player.id)) break;
        Player p = m.player;
        if (p.system()) p.system_id = p.id;
        add_player(p, false, in.reply_to);
        break;
    }
    case cmd::kDeletePlayer:
        if (!connected()) break;
        if (m.id == system_id_) {
            // This machine was removed from the session.
            SessionEvent e;
            e.kind = SessionEvent::Kind::Lost;
            emit(std::move(e));
            break;
        }
        remove_player(m.id, false);
        break;
    case cmd::kPlayerNameChanged: {
        auto it = players_.find(m.id);
        if (it == players_.end()) break;
        it->second.p.short_name = m.short_name;
        it->second.p.long_name = m.long_name;
        SessionEvent e;
        e.kind = SessionEvent::Kind::PlayerName;
        e.id = static_cast<std::int32_t>(m.id);
        e.short_name = m.short_name;
        e.long_name = m.long_name;
        emit(std::move(e));
        break;
    }
    case cmd::kPlayerDataChanged: {
        auto it = players_.find(m.id);
        if (it == players_.end()) break;
        it->second.p.player_data = m.data;
        SessionEvent e;
        e.kind = SessionEvent::Kind::PlayerData;
        e.id = static_cast<std::int32_t>(m.id);
        e.data = m.data;
        emit(std::move(e));
        break;
    }
    case cmd::kSessionDescChanged: {
        if (state_ != State::Joined) break;
        desc_ = m.desc;
        SessionEvent e;
        e.kind = SessionEvent::Kind::SessionDesc;
        emit(std::move(e));
        break;
    }
    case cmd::kPing:
        send_system(in.reply_to, ping_reply(system_id_, m.tick));
        break;
    default:
        break;
    }
}

}  // namespace net::dplay
