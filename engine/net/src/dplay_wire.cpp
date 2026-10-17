#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "net/dplay_wire.h"

#include <cstdio>
#include <cstring>
#include <random>

namespace net::dplay {
namespace {

void put32(Bytes& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put16(Bytes& o, std::uint16_t v) {
    o.push_back(static_cast<std::uint8_t>(v));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void set32(Bytes& o, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

// A system message: signature, command, version.
Bytes start(std::uint16_t command) {
    Bytes o;
    put32(o, kSignature);
    put16(o, command);
    put16(o, kVersion);
    return o;
}

void put_desc(Bytes& o, const SessionDesc& d) {
    put32(o, 80);
    put32(o, d.flags);
    o.insert(o.end(), d.instance.b.begin(), d.instance.b.end());
    o.insert(o.end(), d.application.b.begin(), d.application.b.end());
    put32(o, d.max_players);
    put32(o, d.current_players);
    put32(o, 0);  // the name and password pointers of the sender's memory: zero on the wire
    put32(o, 0);
    put32(o, d.reserved1);
    put32(o, d.reserved2);
    for (std::uint32_t u : d.user) put32(o, u);
}

SessionDesc get_desc(const std::uint8_t* p) {
    SessionDesc d;
    d.flags = get32(p + 4);
    std::memcpy(d.instance.b.data(), p + 8, 16);
    std::memcpy(d.application.b.data(), p + 24, 16);
    d.max_players = get32(p + 40);
    d.current_players = get32(p + 44);
    d.reserved1 = get32(p + 56);
    d.reserved2 = get32(p + 60);
    for (int i = 0; i < 4; ++i) d.user[static_cast<std::size_t>(i)] = get32(p + 64 + 4 * i);
    return d;
}

// Size field of a super-packed length: 0 none, 1 byte, 2 word, 3 double word.
int size_code(std::size_t n) { return n == 0 ? 0 : n < 0x100 ? 1 : n < 0x10000 ? 2 : 3; }
void put_var(Bytes& o, int code, std::size_t n) {
    if (code == 1) o.push_back(static_cast<std::uint8_t>(n));
    else if (code == 2) put16(o, static_cast<std::uint16_t>(n));
    else if (code == 3) put32(o, static_cast<std::uint32_t>(n));
}
bool get_var(const std::uint8_t* p, std::size_t n, std::size_t& o, int code, std::size_t& v) {
    const std::size_t w = code == 1 ? 1 : code == 2 ? 2 : code == 3 ? 4 : 0;
    if (o + w > n) return false;
    v = code == 1 ? p[o] : code == 2 ? get16(p + o) : code == 3 ? get32(p + o) : 0;
    o += w;
    return true;
}

bool decode_packed(const std::uint8_t* p, std::size_t n, Player& out) {
    if (n < 48) return false;
    const std::uint32_t size = get32(p), fixed = get32(p + 36);
    if (size > n || fixed < 48 || fixed > size) return false;
    out.flags = get32(p + 4);
    out.id = get32(p + 8);
    const std::uint32_t short_len = get32(p + 12), long_len = get32(p + 16), sp_len = get32(p + 20), data_len = get32(p + 24);
    const std::uint32_t members = get32(p + 28);
    out.system_id = get32(p + 32);
    out.version = get32(p + 40);
    out.parent = get32(p + 44);
    std::size_t o = fixed;
    if (static_cast<std::uint64_t>(o) + short_len + long_len + sp_len + data_len + 4ull * members > size) return false;
    if (short_len) out.short_name = get_wide(p + o, short_len);
    o += short_len;
    if (long_len) out.long_name = get_wide(p + o, long_len);
    o += long_len;
    out.sp_data.assign(p + o, p + o + sp_len);
    o += sp_len;
    out.player_data.assign(p + o, p + o + data_len);
    return true;
}

bool decode_super(const std::uint8_t* p, std::size_t n, std::size_t& o, Player& out) {
    if (o + 20 > n) return false;
    const std::uint32_t fixed = get32(p + o);
    out.flags = get32(p + o + 4);
    out.id = get32(p + o + 8);
    const std::uint32_t mask = get32(p + o + 12);
    const std::uint32_t vs = get32(p + o + 16);
    if (out.flags & kSystemPlayer) {
        out.version = vs;
        out.system_id = out.id;
    } else {
        out.system_id = vs;
    }
    o += fixed + 4;
    if (o > n) return false;
    std::size_t used = 0;
    if (mask & 1) {
        out.short_name = get_wide(p + o, n - o, &used);
        o += used;
    }
    if (mask & 2) {
        out.long_name = get_wide(p + o, n - o, &used);
        o += used;
    }
    std::size_t len = 0;
    if (!get_var(p, n, o, static_cast<int>((mask >> 4) & 3), len) || o + len > n) return false;
    out.player_data.assign(p + o, p + o + len);
    o += len;
    if (!get_var(p, n, o, static_cast<int>((mask >> 2) & 3), len) || o + len > n) return false;
    out.sp_data.assign(p + o, p + o + len);
    o += len;
    if (!get_var(p, n, o, static_cast<int>((mask >> 6) & 3), len) || o + 4 * len > n) return false;
    o += 4 * len;  // group members (no groups in the game's sessions)
    if (mask & 0x100) {
        if (o + 4 > n) return false;
        out.parent = get32(p + o);
        o += 4;
    }
    if (!get_var(p, n, o, static_cast<int>((mask >> 9) & 3), len) || o + 4 * len > n) return false;
    o += 4 * len;
    return true;
}

}  // namespace

bool Guid::null() const {
    for (std::uint8_t c : b)
        if (c) return false;
    return true;
}

std::string Guid::str() const {
    char s[40];
    std::snprintf(s, sizeof s, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", get32(b.data()), get16(b.data() + 4), get16(b.data() + 6),
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

Guid Guid::parse(const std::string& s) {
    Guid g;
    unsigned a = 0, b1 = 0, c = 0, d[8] = {};
    if (std::sscanf(s.c_str(), "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b1, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) == 11) {
        for (int i = 0; i < 4; ++i) g.b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(a >> (8 * i));
        g.b[4] = static_cast<std::uint8_t>(b1);
        g.b[5] = static_cast<std::uint8_t>(b1 >> 8);
        g.b[6] = static_cast<std::uint8_t>(c);
        g.b[7] = static_cast<std::uint8_t>(c >> 8);
        for (int i = 0; i < 8; ++i) g.b[static_cast<std::size_t>(8 + i)] = static_cast<std::uint8_t>(d[i]);
    }
    return g;
}

void put_sockaddr(Bytes& out, const SockAddr& a) {
    put16(out, 2);
    out.push_back(static_cast<std::uint8_t>(a.port >> 8));
    out.push_back(static_cast<std::uint8_t>(a.port));
    put32(out, a.ip);
    for (int i = 0; i < 8; ++i) out.push_back(0);
}

SockAddr get_sockaddr(const std::uint8_t* p) {
    SockAddr a;
    a.port = static_cast<std::uint16_t>((p[2] << 8) | p[3]);
    a.ip = get32(p + 4);
    return a;
}

std::string ip_text(std::uint32_t ip) {
    char s[24];
    std::snprintf(s, sizeof s, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
    return s;
}

void put_wide(Bytes& out, const std::string& s) {
    for (char c : s) put16(out, static_cast<std::uint8_t>(c));
    put16(out, 0);
}

std::string get_wide(const std::uint8_t* p, std::size_t avail, std::size_t* used) {
    std::string s;
    std::size_t i = 0;
    for (; i + 1 < avail; i += 2) {
        const std::uint16_t c = get16(p + i);
        if (!c) {
            i += 2;
            break;
        }
        s.push_back(static_cast<char>(c < 0x100 ? c : '?'));
    }
    if (used) *used = i;
    return s;
}

Bytes enum_sessions(const Guid& application, std::uint32_t flags, const std::string& password) {
    Bytes o = start(cmd::kEnumSessions);
    o.insert(o.end(), application.b.begin(), application.b.end());
    put32(o, password.empty() ? 0 : 32);  // password offset (after the fixed fields)
    put32(o, flags);
    if (!password.empty()) put_wide(o, password);
    return o;
}

Bytes enum_sessions_reply(const SessionDesc& d) {
    Bytes o = start(cmd::kEnumSessionsReply);
    put_desc(o, d);
    put32(o, 92);  // the name follows: 8 + 80 + 4
    put_wide(o, d.name);
    return o;
}

Bytes request_player_id(std::uint32_t flags) {
    Bytes o = start(cmd::kRequestPlayerId);
    put32(o, flags);
    return o;
}

Bytes request_player_reply(std::uint32_t id, std::uint32_t result) {
    Bytes o = start(cmd::kRequestPlayerReply);
    put32(o, id);
    for (int i = 0; i < 6; ++i) put32(o, 0);  // security description (none)
    put32(o, 0);                              // security provider names (none)
    put32(o, 0);
    put32(o, result);
    return o;
}

Bytes packed_player(const Player& p) {
    Bytes names;
    if (!p.short_name.empty()) put_wide(names, p.short_name);
    const std::size_t short_len = names.size();
    if (!p.long_name.empty()) put_wide(names, p.long_name);
    const std::size_t long_len = names.size() - short_len;
    Bytes o;
    const std::size_t size = 48 + names.size() + p.sp_data.size() + p.player_data.size();
    put32(o, static_cast<std::uint32_t>(size));
    put32(o, p.flags);
    put32(o, p.id);
    put32(o, static_cast<std::uint32_t>(short_len));
    put32(o, static_cast<std::uint32_t>(long_len));
    put32(o, static_cast<std::uint32_t>(p.sp_data.size()));
    put32(o, static_cast<std::uint32_t>(p.player_data.size()));
    put32(o, 0);  // group members
    put32(o, p.system_id);
    put32(o, 48);  // fixed part
    put32(o, p.version);
    put32(o, p.parent);
    o.insert(o.end(), names.begin(), names.end());
    o.insert(o.end(), p.sp_data.begin(), p.sp_data.end());
    o.insert(o.end(), p.player_data.begin(), p.player_data.end());
    return o;
}

Bytes super_packed_player(const Player& p) {
    std::uint32_t mask = 0;
    if (!p.short_name.empty()) mask |= 1;
    if (!p.long_name.empty()) mask |= 2;
    const int sp = size_code(p.sp_data.size()), pd = size_code(p.player_data.size());
    mask |= static_cast<std::uint32_t>(sp) << 2;
    mask |= static_cast<std::uint32_t>(pd) << 4;
    if (p.parent) mask |= 0x100;
    Bytes o;
    put32(o, 16);
    put32(o, p.flags);
    put32(o, p.id);
    put32(o, mask);
    put32(o, p.system() ? p.version : p.system_id);
    if (mask & 1) put_wide(o, p.short_name);
    if (mask & 2) put_wide(o, p.long_name);
    put_var(o, pd, p.player_data.size());
    o.insert(o.end(), p.player_data.begin(), p.player_data.end());
    put_var(o, sp, p.sp_data.size());
    o.insert(o.end(), p.sp_data.begin(), p.sp_data.end());
    if (mask & 0x100) put32(o, p.parent);
    return o;
}

Bytes add_forward_request(std::uint32_t to, const Player& p, const std::string& password, std::uint32_t key) {
    Bytes o = start(cmd::kAddForwardRequest);
    put32(o, to);
    put32(o, p.id);
    put32(o, 0);   // group
    put32(o, 28);  // the player follows
    put32(o, 0);   // password offset, set below
    const Bytes pk = packed_player(p);
    o.insert(o.end(), pk.begin(), pk.end());
    set32(o, 24, static_cast<std::uint32_t>(o.size()));
    put_wide(o, password);
    put32(o, key);
    return o;
}

Bytes add_forward_reply(std::uint32_t result) {
    Bytes o = start(cmd::kAddForwardReply);
    put32(o, result);
    return o;
}

Bytes super_enum_players_reply(const SessionDesc& d, const std::vector<Player>& players) {
    Bytes o = start(cmd::kSuperEnumPlayersReply);
    put32(o, static_cast<std::uint32_t>(players.size()));
    put32(o, 0);  // groups
    put32(o, 0);  // packed offset, set below
    put32(o, 0);  // shortcuts
    put32(o, 36);  // the description follows the fixed fields
    put32(o, 0);   // name offset
    put32(o, 0);   // password offset
    put_desc(o, d);
    set32(o, 28, static_cast<std::uint32_t>(o.size()));
    put_wide(o, d.name);
    if (!d.password.empty()) {
        set32(o, 32, static_cast<std::uint32_t>(o.size()));
        put_wide(o, d.password);
    }
    set32(o, 16, static_cast<std::uint32_t>(o.size()));
    for (const Player& p : players) {
        const Bytes sp = super_packed_player(p);
        o.insert(o.end(), sp.begin(), sp.end());
    }
    return o;
}

Bytes create_player(std::uint32_t to, const Player& p) {
    Bytes o = start(cmd::kCreatePlayer);
    put32(o, to);
    put32(o, p.id);
    put32(o, 0);   // group
    put32(o, 28);  // the player follows
    put32(o, 0);   // no password
    const Bytes pk = packed_player(p);
    o.insert(o.end(), pk.begin(), pk.end());
    put16(o, 0);  // two reserved fields, zero in every observed message
    put32(o, 0);
    return o;
}

Bytes delete_player(std::uint32_t to, std::uint32_t id) {
    Bytes o = start(cmd::kDeletePlayer);
    put32(o, to);
    put32(o, id);
    put32(o, 0);  // group
    put32(o, 0);  // no player follows
    put32(o, 0);
    return o;
}

Bytes player_name_changed(std::uint32_t to, std::uint32_t id, const std::string& short_name, const std::string& long_name) {
    Bytes o = start(cmd::kPlayerNameChanged);
    put32(o, to);
    put32(o, id);
    put32(o, 0);
    put32(o, 0);
    if (!short_name.empty()) {
        set32(o, 16, static_cast<std::uint32_t>(o.size()));
        put_wide(o, short_name);
    }
    if (!long_name.empty()) {
        set32(o, 20, static_cast<std::uint32_t>(o.size()));
        put_wide(o, long_name);
    }
    return o;
}

Bytes player_data_changed(std::uint32_t to, std::uint32_t id, const Bytes& data) {
    Bytes o = start(cmd::kPlayerDataChanged);
    put32(o, to);
    put32(o, id);
    put32(o, static_cast<std::uint32_t>(data.size()));
    put32(o, 24);
    o.insert(o.end(), data.begin(), data.end());
    return o;
}

Bytes session_desc_changed(std::uint32_t to, const SessionDesc& d) {
    Bytes o = start(cmd::kSessionDescChanged);
    put32(o, to);
    put32(o, 0);  // name offset
    put32(o, 0);  // password offset
    put_desc(o, d);
    set32(o, 12, static_cast<std::uint32_t>(o.size()));
    put_wide(o, d.name);
    // The password follows even when empty (as the original's machines send it).
    set32(o, 16, static_cast<std::uint32_t>(o.size()));
    put_wide(o, d.password);
    return o;
}

Bytes ping(std::uint32_t from, std::uint32_t tick) {
    Bytes o = start(cmd::kPing);
    put32(o, from);
    put32(o, tick);
    return o;
}

Bytes ping_reply(std::uint32_t from, std::uint32_t tick) {
    Bytes o = start(cmd::kPingReply);
    put32(o, from);
    put32(o, tick);
    return o;
}

Bytes game_message(std::uint32_t from, std::uint32_t to, const std::uint8_t* data, std::size_t size) {
    Bytes o;
    put32(o, from);
    put32(o, to);
    o.insert(o.end(), data, data + size);
    return o;
}

bool is_system(const std::uint8_t* p, std::size_t n) { return n >= 8 && get32(p) == kSignature; }

std::optional<Message> decode(const std::uint8_t* p, std::size_t n) {
    if (!is_system(p, n)) return std::nullopt;
    Message m;
    m.command = get16(p + 4);
    m.version = get16(p + 6);
    auto need = [&](std::size_t k) { return n >= k; };
    switch (m.command) {
    case cmd::kEnumSessions: {
        if (!need(32)) return std::nullopt;
        std::memcpy(m.application.b.data(), p + 8, 16);
        const std::uint32_t pw = get32(p + 24);
        m.flags = get32(p + 28);
        if (pw && pw < n) m.password = get_wide(p + pw, n - pw);
        break;
    }
    case cmd::kEnumSessionsReply: {
        if (!need(92)) return std::nullopt;
        m.desc = get_desc(p + 8);
        const std::uint32_t name = get32(p + 88);
        if (name && name < n) m.desc.name = get_wide(p + name, n - name);
        break;
    }
    case cmd::kRequestPlayerId:
        if (!need(12)) return std::nullopt;
        m.flags = get32(p + 8);
        break;
    case cmd::kRequestPlayerReply:
        if (!need(48)) return std::nullopt;
        m.id = get32(p + 8);
        m.result = get32(p + 44);
        break;
    case cmd::kAddForwardRequest:
    case cmd::kCreatePlayer:
    case cmd::kDeletePlayer:
    case cmd::kAddForward: {
        if (!need(28)) return std::nullopt;
        m.to = get32(p + 8);
        m.id = get32(p + 12);
        const std::uint32_t create = get32(p + 20), pw = get32(p + 24);
        if (create && create < n && !decode_packed(p + create, n - create, m.player)) return std::nullopt;
        if (m.command == cmd::kAddForwardRequest) {
            if (pw && pw < n) {
                std::size_t used = 0;
                m.password = get_wide(p + pw, n - pw, &used);
                if (pw + used + 4 <= n) m.key = get32(p + pw + used);
            }
        }
        break;
    }
    case cmd::kAddForwardReply:
        if (!need(12)) return std::nullopt;
        m.result = get32(p + 8);
        break;
    case cmd::kSuperEnumPlayersReply: {
        if (!need(36)) return std::nullopt;
        const std::uint32_t count = get32(p + 8), groups = get32(p + 12), packed = get32(p + 16), desc = get32(p + 24), name = get32(p + 28),
                            pw = get32(p + 32);
        if (desc + 80 > n) return std::nullopt;
        m.desc = get_desc(p + desc);
        if (name && name < n) m.desc.name = get_wide(p + name, n - name);
        if (pw && pw < n) m.desc.password = get_wide(p + pw, n - pw);
        std::size_t o = packed;
        for (std::uint32_t i = 0; i < count + groups; ++i) {
            Player pl;
            if (!decode_super(p, n, o, pl)) return std::nullopt;
            if (i < count) m.players.push_back(std::move(pl));
        }
        break;
    }
    case cmd::kPlayerNameChanged: {
        if (!need(24)) return std::nullopt;
        m.to = get32(p + 8);
        m.id = get32(p + 12);
        const std::uint32_t s = get32(p + 16), l = get32(p + 20);
        if (s && s < n) m.short_name = get_wide(p + s, n - s);
        if (l && l < n) m.long_name = get_wide(p + l, n - l);
        break;
    }
    case cmd::kPlayerDataChanged: {
        if (!need(24)) return std::nullopt;
        m.to = get32(p + 8);
        m.id = get32(p + 12);
        const std::uint32_t size = get32(p + 16), off = get32(p + 20);
        if (off + static_cast<std::uint64_t>(size) > n) return std::nullopt;
        m.data.assign(p + off, p + off + size);
        break;
    }
    case cmd::kSessionDescChanged: {
        if (!need(100)) return std::nullopt;
        m.to = get32(p + 8);
        const std::uint32_t name = get32(p + 12), pw = get32(p + 16);
        m.desc = get_desc(p + 20);
        if (name && name < n) m.desc.name = get_wide(p + name, n - name);
        if (pw && pw < n) m.desc.password = get_wide(p + pw, n - pw);
        break;
    }
    case cmd::kPing:
    case cmd::kPingReply:
        if (!need(16)) return std::nullopt;
        m.id = get32(p + 8);
        m.tick = get32(p + 12);
        break;
    default:
        break;
    }
    return m;
}

const char* command_name(std::uint16_t c) {
    switch (c) {
    case cmd::kEnumSessionsReply: return "EnumSessionsReply";
    case cmd::kEnumSessions: return "EnumSessions";
    case cmd::kRequestPlayerId: return "RequestPlayerId";
    case cmd::kRequestPlayerReply: return "RequestPlayerReply";
    case cmd::kCreatePlayer: return "CreatePlayer";
    case cmd::kDeletePlayer: return "DeletePlayer";
    case cmd::kPlayerDataChanged: return "PlayerDataChanged";
    case cmd::kPlayerNameChanged: return "PlayerNameChanged";
    case cmd::kAddForwardRequest: return "AddForwardRequest";
    case cmd::kPing: return "Ping";
    case cmd::kPingReply: return "PingReply";
    case cmd::kSessionDescChanged: return "SessionDescChanged";
    case cmd::kAddForwardReply: return "AddForwardReply";
    case cmd::kSuperEnumPlayersReply: return "SuperEnumPlayersReply";
    case cmd::kAddForward: return "AddForward";
    case cmd::kAddForwardAck: return "AddForwardAck";
    default: return "?";
    }
}

Bytes tcpip_sp_data(const SockAddr& stream, const SockAddr& datagram) {
    Bytes o;
    put_sockaddr(o, stream);
    put_sockaddr(o, datagram);
    return o;
}

bool tcpip_from_sp_data(const Bytes& sp, SockAddr& stream, SockAddr& datagram) {
    if (sp.size() < 32) return false;
    stream = get_sockaddr(sp.data());
    datagram = get_sockaddr(sp.data() + 16);
    return true;
}

Bytes tcpip_frame(const SockAddr& return_address, const Bytes& payload) {
    Bytes o;
    put32(o, static_cast<std::uint32_t>(kTcpipHeader + payload.size()) | (kToken << 20));
    put_sockaddr(o, return_address);
    o.insert(o.end(), payload.begin(), payload.end());
    return o;
}

std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

}  // namespace net::dplay
