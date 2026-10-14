#include "ta/cob.h"

#include <cstring>

namespace ta {
namespace {

using Op = sim::ScriptOp;

bool rd32(const std::vector<std::uint8_t>& f, std::size_t at, std::uint32_t& v) {
    if (at + 4 > f.size() || at + 4 < at) return false;
    std::memcpy(&v, &f[at], 4);
    return true;
}

bool cstr_at(const std::vector<std::uint8_t>& f, std::uint32_t at, std::string& s) {
    s.clear();
    for (std::size_t i = at; i < f.size(); ++i) {
        if (f[i] == 0) return true;
        s += static_cast<char>(f[i]);
    }
    return false;
}

} // namespace

Op cob_decode(std::uint32_t w) {
    // The interpreter compares the word masked with 0x100FF000.
    switch (w & 0x100FF000u) {
    case 0x10001000u: return Op::Move;
    case 0x10002000u: return Op::Turn;
    case 0x10003000u: return Op::Spin;
    case 0x10004000u: return Op::StopSpin;
    case 0x10005000u: return Op::Show;
    case 0x10006000u: return Op::Hide;
    case 0x10007000u: return Op::Cache;
    case 0x10008000u: return Op::DontCache;
    case 0x10009000u: return Op::LegacyExplode;
    case 0x1000A000u: return Op::PlaySound;
    case 0x1000B000u: return Op::MoveNow;
    case 0x1000C000u: return Op::TurnNow;
    case 0x1000D000u: return Op::Shade;
    case 0x1000E000u: return Op::DontShade;
    case 0x1000F000u: return Op::EmitSfx;
    case 0x10011000u: return Op::WaitTurn;
    case 0x10012000u: return Op::WaitMove;
    case 0x10013000u: return Op::Sleep;
    case 0x10021000u:
        switch (w & 7u) {  // operand kind
        case 1: return Op::PushConst;
        case 2: return Op::PushLocal;
        case 4: return Op::PushStatic;
        default: return Op::PushStale;
        }
    case 0x10022000u: return Op::CreateLocal;
    case 0x10023000u:
        switch (w & 7u) {
        case 2: return Op::PopLocal;
        case 4: return Op::PopStatic;
        default: return Op::PopNone;
        }
    case 0x10024000u: return Op::PopStack;
    case 0x10031000u: return Op::Add;
    case 0x10032000u: return Op::Sub;
    case 0x10033000u: return Op::Mul;
    case 0x10034000u: return Op::Div;
    case 0x10035000u: return Op::BitAnd;
    case 0x10036000u: return Op::BitOr;
    case 0x10037000u: return Op::BitXor;
    case 0x10038000u: return Op::BitNot;
    case 0x10041000u: return Op::Rand;
    case 0x10042000u: return Op::GetValue;
    case 0x10043000u: return Op::GetValueArgs;
    case 0x10044000u: return Op::GetTransporting;
    case 0x10045000u: return Op::GetCarrier;
    case 0x10051000u: return Op::Lt;
    case 0x10052000u: return Op::Le;
    case 0x10053000u: return Op::Gt;
    case 0x10054000u: return Op::Ge;
    case 0x10055000u: return Op::Eq;
    case 0x10056000u: return Op::Ne;
    case 0x10057000u: return Op::LogAnd;
    case 0x10058000u: return Op::LogOr;
    case 0x10059000u: return Op::Xor2;
    case 0x1005A000u: return Op::LogNot;
    case 0x10061000u: return Op::StartScript;
    case 0x10062000u: return Op::CallScript;
    case 0x10063000u: return Op::DropArgs;
    case 0x10064000u: return Op::Jump;
    case 0x10065000u: return Op::Return;
    case 0x10066000u: return Op::JumpIfZero;
    case 0x10067000u: return Op::Signal;
    case 0x10068000u: return Op::SetSignalMask;
    case 0x10071000u: return Op::ExplodePiece;
    case 0x10082000u: return Op::SetValue;
    case 0x10083000u: return Op::Attach;
    case 0x10084000u: return Op::Drop;
    default: return Op::Invalid;
    }
}

// Header (u32 each): version, function count, piece count, code length (words),
// static count, sound count, then file offsets of: entry table (u32 word index
// per function), function name offsets, piece name offsets, code, sound table
// (8 bytes per sound, name offset in the second word).
bool load_cob(const std::vector<std::uint8_t>& f, CobFile& out, std::string& error) {
    out = CobFile{};
    std::uint32_t h[11];
    for (int i = 0; i < 11; ++i) {
        if (!rd32(f, static_cast<std::size_t>(i) * 4, h[i])) {
            error = "truncated header";
            return false;
        }
    }
    out.version = h[0];
    const std::uint32_t functions = h[1], pieces = h[2], code_words = h[3];
    auto& p = out.program;
    p.static_count = static_cast<std::int32_t>(h[4]);
    for (std::uint32_t i = 0; i < functions; ++i) {
        std::uint32_t entry = 0, name_at = 0;
        std::string name;
        if (!rd32(f, h[6] + i * 4ull, entry) || !rd32(f, h[7] + i * 4ull, name_at) || !cstr_at(f, name_at, name)) {
            error = "bad function table";
            return false;
        }
        p.entry.push_back(entry);
        p.function_names.push_back(name);
    }
    for (std::uint32_t i = 0; i < pieces; ++i) {
        std::uint32_t name_at = 0;
        std::string name;
        if (!rd32(f, h[8] + i * 4ull, name_at) || !cstr_at(f, name_at, name)) {
            error = "bad piece table";
            return false;
        }
        p.piece_names.push_back(name);
    }
    for (std::uint32_t i = 0; i < h[5]; ++i) {
        std::uint32_t name_at = 0;
        std::string name;
        if (!rd32(f, h[10] + i * 8ull + 4, name_at) || !cstr_at(f, name_at, name)) {
            error = "bad sound table";
            return false;
        }
        out.sound_names.push_back(name);
    }
    // The game never bounds the program counter; we keep every word from the
    // code start to the end of the file so stray jumps read the same bytes.
    if (h[9] > f.size()) {
        error = "code offset past end of file";
        return false;
    }
    const std::size_t words = (f.size() - h[9]) / 4;
    if (words < code_words) {
        error = "code shorter than its declared length";
        return false;
    }
    p.code.resize(words);
    std::memcpy(p.code.data(), &f[h[9]], words * 4);
    p.ops.resize(words);
    for (std::size_t i = 0; i < words; ++i) p.ops[i] = cob_decode(p.code[i]);
    return true;
}

} // namespace ta
