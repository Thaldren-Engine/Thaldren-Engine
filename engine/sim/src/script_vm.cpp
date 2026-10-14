#include "sim/script_vm.h"

namespace sim {
namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

i32 s(u32 v) { return static_cast<i32>(v); }
u32 u(i32 v) { return static_cast<u32>(v); }

// C division with truncation; the caller guarantees d != 0 and no INT_MIN / -1.
i32 cdiv(i32 n, i32 d) { return n / d; }

// Magnitude of v reduced modulo 0x10000, sign kept (as the angle code does it).
i32 wrap_keep_sign(u32 v) {
    const u32 sign = u(s(v) >> 31);
    return s((((v ^ sign) - sign) & 0xFFFFu ^ sign) - sign);
}

} // namespace

int ScriptProgram::find_function(const std::string& name) const {
    for (std::size_t i = 0; i < function_names.size(); ++i)
        if (function_names[i] == name) return static_cast<int>(i);
    return -1;
}

ScriptInstance::ScriptInstance(const ScriptProgram& program, ScriptHost& host, std::int32_t tick_rate)
    : program_(program), host_(host), tick_rate_(tick_rate),
      statics_(static_cast<std::size_t>(program.static_count), 0),
      anims_(program.piece_names.size() * kPieceWords, 0) {}

void ScriptInstance::set_fault(const char* what) {
    if (fault_.empty()) fault_ = what;
}

std::uint32_t& ScriptInstance::stack_word(int t, std::int32_t index) {
    const std::int64_t at = static_cast<std::int64_t>(t) * kThreadWords + index;
    if (at < 0 || at >= static_cast<std::int64_t>(mem_.size())) {
        set_fault("stack access outside the thread block");
        return scratch_;
    }
    return mem_[static_cast<std::size_t>(at)];
}

std::int32_t& ScriptInstance::anim(std::uint32_t piece, std::uint32_t axis, int field) {
    const std::uint64_t at = static_cast<std::uint64_t>(piece) * kPieceWords + axis + static_cast<std::uint32_t>(field);
    if (at >= anims_.size()) {
        set_fault("piece/axis outside the animation table");
        return reinterpret_cast<std::int32_t&>(scratch_);
    }
    return anims_[static_cast<std::size_t>(at)];
}

std::int32_t& ScriptInstance::stat(std::uint32_t index) {
    if (index >= statics_.size()) {
        set_fault("static variable outside the table");
        return reinterpret_cast<std::int32_t&>(scratch_);
    }
    return statics_[index];
}

std::uint32_t ScriptInstance::pop(int t) {
    u32& sp = thread_word(t, kSp);
    const u32 v = stack_word(t, kStack + s(sp));
    sp -= 1;
    return v;
}

void ScriptInstance::push(int t, std::uint32_t v) {
    u32& sp = thread_word(t, kSp);
    const u32 at = sp;
    sp = at + 1;
    stack_word(t, kStack + 1 + s(at)) = v;
}

int ScriptInstance::start(int function) {
    if (function < 0 || static_cast<std::size_t>(function) >= program_.entry.size()) return -1;
    for (int t = 0; t < kThreads; ++t) {
        if (thread_word(t, kState) != kFree) continue;
        thread_word(t, kState) = kRunning;
        thread_word(t, kPc) = program_.entry[static_cast<std::size_t>(function)];
        thread_word(t, kSp) = 0xFFFFFFFFu;
        thread_word(t, kSink) = 0;
        sinks_[static_cast<std::size_t>(t)] = nullptr;
        thread_word(t, kSignalMask) = 1;
        // Locals are not cleared: they keep whatever the slot held before.
        mem_[kThreads * kThreadWords] += 1;
        return t;
    }
    return -1;
}

bool ScriptInstance::call(const std::string& name, ScriptReturnSink* sink, bool run_now) {
    const int t = start(program_.find_function(name));
    if (t < 0) return false;
    thread_word(t, kSink) = sink ? sink->handle() : 0u;
    sinks_[static_cast<std::size_t>(t)] = sink;
    if (run_now) {
        if (thread_count() != 0)
            for (int k = 0; k < kThreads; ++k) run_thread(k, 0);
        animate(0);
    }
    return true;
}

bool ScriptInstance::call_with_args(int function, ScriptReturnSink* sink, bool run_now, std::int32_t arg_count,
                                    std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) {
    const int t = start(function);
    if (t < 0) {
        if (sink) sink->on_return(0);
        return false;
    }
    thread_word(t, kSink) = sink ? sink->handle() : 0u;
    sinks_[static_cast<std::size_t>(t)] = sink;
    push(t, u(a));
    push(t, u(b));
    push(t, u(c));
    push(t, u(d));
    thread_word(t, kSp) = u(arg_count - 1);
    if (run_now) {
        if (thread_count() != 0)
            for (int k = 0; k < kThreads; ++k) run_thread(k, 0);
        animate(0);
    }
    return true;
}

bool ScriptInstance::call_in_out(int function, std::int32_t* a, std::int32_t* b, std::int32_t* c, std::int32_t* d) {
    const int t = start(function);
    if (t < 0) return false;
    thread_word(t, kSink) = 0;
    sinks_[static_cast<std::size_t>(t)] = nullptr;
    push(t, a ? u(*a) : 0u);
    push(t, b ? u(*b) : 0u);
    push(t, c ? u(*c) : 0u);
    push(t, d ? u(*d) : 0u);
    thread_word(t, kSp) = 3;
    run_thread(t, 0);
    // The locals are read back even if the thread has finished or was reused.
    if (a) *a = s(thread_word(t, kStack + 0));
    if (b) *b = s(thread_word(t, kStack + 1));
    if (c) *c = s(thread_word(t, kStack + 2));
    if (d) *d = s(thread_word(t, kStack + 3));
    return true;
}

void ScriptInstance::tick(std::int32_t ticks, std::uint32_t& stale_word) {
    const bool live = thread_count() != 0;
    tick(ticks);
    if (live) stale_word = 7;
    if (turned_) stale_word = last_turn_target_;
}

void ScriptInstance::tick(std::int32_t ticks) {
    if (thread_count() != 0)
        for (int t = 0; t < kThreads; ++t) run_thread(t, ticks);
    animate(ticks);
}

void ScriptInstance::kill(int t) {
    thread_word(t, kState) = kFree;
    mem_[kThreads * kThreadWords] -= 1;
}

void ScriptInstance::run_thread(int t, std::int32_t ticks) {
    u32& state = thread_word(t, kState);
    if ((state & 0xFF000000u) == 0) return;
    if ((state & 0xFF000000u) == 0x02000000u) {
        const u32 kind = state & 0x00F00000u;
        const u32 piece = thread_word(t, kWaitPiece), axis = thread_word(t, kWaitAxis);
        if (kind == 0x00100000u) {
            if (anim(piece, axis, kTurnSpeed) == 0) state = kRunning;
        } else if (kind == 0x00200000u) {
            if (anim(piece, axis, kMoveSpeed) == 0) state = kRunning;
        } else if (kind == 0x00400000u) {
            u32& sleep = thread_word(t, kSleep);
            sleep -= u(ticks);
            if (s(sleep) <= 0) state = kRunning;
        }
    }
    if ((state & 0xFF000000u) != kRunning) return;

    const auto& code = program_.code;
    auto word = [&](u32 at) -> u32 {
        if (at >= code.size()) {
            set_fault("code read outside the program");
            return 0;
        }
        return code[at];
    };
    u32 stale_push = 0;  // the original pushes an uninitialised local for unknown push kinds
    bool pushed = false;
    bool running = true;
    while (running) {
        if (step_limit_ != 0 && ++steps_ > step_limit_) {
            set_fault("step limit reached (endless loop?)");
            return;
        }
        u32& pc = thread_word(t, kPc);
        if (pc >= program_.ops.size()) {
            set_fault("pc outside the program");
            kill(t);
            return;
        }
        // Most instructions advance from the pc as it is *after* their stack traffic (a stack
        // underrun can overwrite it); a few advance from the pc they started at.
        const u32 at = pc;
        if (op_counts_) ++op_counts_[static_cast<std::size_t>(program_.ops[at])];
        switch (program_.ops[at]) {
        case ScriptOp::Move: {
            const u32 piece = word(at + 1), axis = word(at + 2);
            anim(piece, axis, kMoveTarget) = s(pop(t));
            anim(piece, axis, kMoveSpeed) = cdiv(s(pop(t)), tick_rate_);
            const i32 cur = host_.get_piece_move(s(piece), s(axis));
            if (anim(piece, axis, kMoveTarget) < cur) anim(piece, axis, kMoveSpeed) = s(0u - u(anim(piece, axis, kMoveSpeed)));
            anim(piece, 0, kActive) = 1;
            dirty_ = true;
            pc = pc + 3;
            break;
        }
        case ScriptOp::Turn: {
            const u32 piece = word(at + 1), axis = word(at + 2);
            anim(piece, axis, kTurnTarget) = s(pop(t) & 0xFFFFu);
            anim(piece, axis, kSpinAccel) = 0;
            anim(piece, axis, kTurnSpeed) = cdiv(s(pop(t)), tick_rate_);
            const i32 cur = host_.get_piece_turn(s(piece), s(axis));
            const u32 diff = u(anim(piece, axis, kTurnTarget)) - u(cur);
            if (diff == 0) {
                anim(piece, axis, kTurnSpeed) = 0;
            } else {
                const i32 sd = s(diff);
                const i32 mag = s((diff ^ u(sd >> 31)) - u(sd >> 31));
                if ((0x8000 < mag) != (sd < 0)) anim(piece, axis, kTurnSpeed) = s(0u - u(anim(piece, axis, kTurnSpeed)));
            }
            anim(piece, 0, kActive) = 1;
            dirty_ = true;
            pc = pc + 3;
            break;
        }
        case ScriptOp::Spin: {
            const u32 piece = word(at + 1), axis = word(at + 2);
            anim(piece, axis, kTurnTarget) = -1;
            anim(piece, axis, kSpinTarget) = cdiv(s(pop(t)), tick_rate_);
            anim(piece, axis, kSpinAccel) = cdiv(s(pop(t)), tick_rate_);
            if (anim(piece, axis, kSpinAccel) == 0) anim(piece, axis, kTurnSpeed) = anim(piece, axis, kSpinTarget);
            anim(piece, 0, kActive) = 1;
            dirty_ = true;
            pc = pc + 3;
            break;
        }
        case ScriptOp::StopSpin: {
            // Neither marks the piece active nor the table dirty.
            const u32 piece = word(at + 1), axis = word(at + 2);
            anim(piece, axis, kSpinTarget) = 0;
            anim(piece, axis, kSpinAccel) = s(0u - u(cdiv(s(pop(t)), tick_rate_)));
            if (anim(piece, axis, kSpinAccel) == 0) anim(piece, axis, kTurnSpeed) = 0;
            pc = pc + 3;
            break;
        }
        case ScriptOp::Show: host_.set_piece_visible(s(word(at + 1)), 1); pc = pc + 2; break;
        case ScriptOp::Hide: host_.set_piece_visible(s(word(at + 1)), 0); pc = pc + 2; break;
        case ScriptOp::Cache: host_.set_piece_cached(s(word(at + 1)), 1); pc = pc + 2; break;
        case ScriptOp::DontCache: host_.set_piece_cached(s(word(at + 1)), 0); pc = pc + 2; break;
        case ScriptOp::Shade: host_.set_piece_shaded(s(word(at + 1)), 1); pc = pc + 2; break;
        case ScriptOp::DontShade: host_.set_piece_shaded(s(word(at + 1)), 0); pc = pc + 2; break;
        case ScriptOp::LegacyExplode: {
            const u32 b = pop(t);
            const u32 a = pop(t);
            host_.legacy_explode(s(word(at + 1)), s(a), s(b));
            pc = pc + 2;
            break;
        }
        case ScriptOp::PlaySound: host_.play_sound(s(word(at + 1))); pc = pc + 2; break;
        case ScriptOp::EmitSfx: {
            const u32 type = pop(t);
            host_.emit_sfx(s(word(at + 1)), s(type));
            pc = pc + 2;
            break;
        }
        case ScriptOp::MoveNow: {
            const u32 piece = word(at + 1), axis = word(at + 2);
            anim(piece, axis, kMoveTarget) = s(pop(t));
            anim(piece, axis, kMoveSpeed) = 0;
            host_.set_piece_move(s(piece), s(axis), anim(piece, axis, kMoveTarget));
            pc = pc + 3;
            break;
        }
        case ScriptOp::TurnNow: {
            const u32 piece = word(at + 1), axis = word(at + 2);
            anim(piece, axis, kTurnTarget) = s(pop(t) & 0xFFFFu);
            anim(piece, axis, kSpinAccel) = 0;
            anim(piece, axis, kTurnSpeed) = 0;
            host_.set_piece_turn(s(piece), s(axis), anim(piece, axis, kTurnTarget));
            pc = pc + 3;
            break;
        }
        case ScriptOp::WaitTurn:
        case ScriptOp::WaitMove:
            state = program_.ops[at] == ScriptOp::WaitTurn ? kWaitingTurn : kWaitingMove;
            thread_word(t, kWaitAxis) = word(at + 2);
            thread_word(t, kWaitPiece) = word(at + 1);
            pc = at + 3;
            running = false;
            break;
        case ScriptOp::Sleep: {
            state = kSleeping;
            const u32 ms = pop(t);
            thread_word(t, kSleep) = u(cdiv(s(u(tick_rate_) * ms), 1000));
            pc = at + 1;
            running = false;
            break;
        }
        case ScriptOp::PushConst: pushed = true; stale_push = word(at + 1); push(t, stale_push); pc = pc + 2; break;
        case ScriptOp::PushLocal: pushed = true; stale_push = stack_word(t, kStack + s(word(at + 1))); push(t, stale_push); pc = pc + 2; break;
        case ScriptOp::PushStatic: pushed = true; stale_push = u(stat(word(at + 1))); push(t, stale_push); pc = pc + 2; break;
        case ScriptOp::PushStale:
            if (!pushed) set_fault("push of an uninitialised value");
            push(t, stale_push);
            pc = pc + 2;
            break;
        case ScriptOp::CreateLocal: thread_word(t, kSp) += 1; pc = at + 1; break;
        case ScriptOp::PopLocal: {
            const u32 v = pop(t);
            stack_word(t, kStack + s(word(at + 1))) = v;
            pc = pc + 2;
            break;
        }
        case ScriptOp::PopStatic: {
            const u32 v = pop(t);
            stat(word(at + 1)) = s(v);
            pc = pc + 2;
            break;
        }
        case ScriptOp::PopNone: pc = pc + 2; break;
        case ScriptOp::PopStack: thread_word(t, kSp) -= 1; pc = at + 1; break;
        case ScriptOp::Add: case ScriptOp::Sub: case ScriptOp::Mul: case ScriptOp::Div:
        case ScriptOp::BitAnd: case ScriptOp::BitOr: case ScriptOp::BitXor:
        case ScriptOp::Lt: case ScriptOp::Le: case ScriptOp::Gt: case ScriptOp::Ge: case ScriptOp::Eq: case ScriptOp::Ne:
        case ScriptOp::LogAnd: case ScriptOp::LogOr: case ScriptOp::Xor2: {
            const u32 b = pop(t);
            const u32 a = pop(t);
            u32 r = 0;
            switch (program_.ops[at]) {
            case ScriptOp::Add: r = a + b; break;
            case ScriptOp::Sub: r = a - b; break;
            case ScriptOp::Mul: r = a * b; break;
            case ScriptOp::Div:
                if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) {
                    set_fault("division fault (the original crashes)");
                    r = 0;
                } else {
                    r = u(s(a) / s(b));
                }
                break;
            case ScriptOp::BitAnd: r = a & b; break;
            case ScriptOp::BitOr: r = a | b; break;
            case ScriptOp::BitXor: r = a ^ b; break;
            case ScriptOp::Lt: r = s(a) < s(b); break;
            case ScriptOp::Le: r = s(a) <= s(b); break;
            case ScriptOp::Gt: r = s(a) > s(b); break;
            case ScriptOp::Ge: r = s(a) >= s(b); break;
            case ScriptOp::Eq: r = a == b; break;
            case ScriptOp::Ne: r = a != b; break;
            case ScriptOp::LogAnd: r = (a != 0 && b != 0) ? 1u : 0u; break;
            case ScriptOp::LogOr: r = (a != 0 || b != 0) ? 1u : 0u; break;
            case ScriptOp::Xor2: r = a ^ b; break;  // bitwise, not logical
            default: break;
            }
            push(t, r);
            pc = pc + 1;
            break;
        }
        case ScriptOp::BitNot: {
            u32& top = stack_word(t, kStack + s(thread_word(t, kSp)));
            top = ~top;
            pc = pc + 1;
            break;
        }
        case ScriptOp::LogNot: {
            u32& top = stack_word(t, kStack + s(thread_word(t, kSp)));
            top = top == 0 ? 1u : 0u;
            pc = pc + 1;
            break;
        }
        case ScriptOp::Rand: {
            const u32 hi = pop(t);
            const u32 lo = pop(t);
            push(t, host_.random_mod(hi - lo + 1) + lo);
            pc = pc + 1;
            break;
        }
        case ScriptOp::GetValue: {
            const u32 id = pop(t);
            push(t, u(host_.get_value(s(id), 0, 0, 0, 0)));
            pc = pc + 1;
            break;
        }
        case ScriptOp::GetValueArgs: {
            const u32 d = pop(t), c = pop(t), b = pop(t), a = pop(t), id = pop(t);
            push(t, u(host_.get_value(s(id), s(a), s(b), s(c), s(d))));
            pc = pc + 1;
            break;
        }
        case ScriptOp::GetTransporting: {
            const u32 x = pop(t);
            push(t, u(host_.get_transporting(s(x))));
            pc = pc + 1;
            break;
        }
        case ScriptOp::GetCarrier: push(t, u(host_.get_carrier())); pc = pc + 1; break;
        case ScriptOp::StartScript:
        case ScriptOp::CallScript: {
            const i32 argc = s(word(at + 2));
            const int callee = start(s(word(at + 1)));
            if (callee >= 0) {
                // Arguments go to the callee's locals argc-1 .. 0 (top of stack last).
                for (i32 k = argc; k > 0; --k) stack_word(callee, kStack + k - 1) = pop(t);
                thread_word(callee, kSignalMask) = thread_word(t, kSignalMask);
            }
            if (program_.ops[at] == ScriptOp::CallScript) {
                thread_word(t, kWaitThread) = u(callee);
                state = kWaitingCall;
                running = false;
            }
            thread_word(t, kPc) += 3;
            break;
        }
        case ScriptOp::DropArgs: {
            const i32 argc = s(word(at + 2));
            if (argc > 4) set_fault("more than 4 dropped arguments (the original overruns its stack frame)");
            for (i32 k = argc; k > 0; --k) pop(t);
            pc = at + 3;
            break;
        }
        case ScriptOp::Jump: pc = word(at + 1); break;
        case ScriptOp::JumpIfZero: {
            const u32 v = pop(t);
            pc = v == 0 ? word(at + 1) : at + 2;
            break;
        }
        case ScriptOp::Return: {
            if (thread_word(t, kSink) != 0) {
                const u32 v = pop(t);
                ScriptReturnSink* sink = sinks_[static_cast<std::size_t>(t)];
                if (sink && thread_word(t, kSink) == sink->handle()) sink->on_return(s(v));
                else set_fault("return sink word corrupted (the original calls through it)");
            }
            kill(t);
            for (int k = 0; k < kThreads; ++k)
                if ((thread_word(k, kState) & 0xFFF00000u) == kWaitingCall && thread_word(k, kWaitThread) == u(t))
                    thread_word(k, kState) = kRunning;
            running = false;
            break;
        }
        case ScriptOp::Signal: {
            const u32 mask = pop(t);
            for (int k = 0; k < kThreads; ++k) {
                if (thread_word(k, kState) == 0 || (mask & thread_word(k, kSignalMask)) == 0) continue;
                kill(k);
                for (int w = 0; w < kThreads; ++w)
                    if ((thread_word(w, kState) & 0xFFF00000u) == kWaitingCall && thread_word(w, kWaitThread) == u(k))
                        thread_word(w, kState) = kRunning;
                if (k == t) running = false;
            }
            thread_word(t, kPc) += 1;
            break;
        }
        case ScriptOp::SetSignalMask: thread_word(t, kSignalMask) = pop(t); pc = at + 1; break;
        case ScriptOp::ExplodePiece: {
            const u32 type = pop(t);
            host_.explode_piece(s(word(at + 1)), s(type));
            pc = pc + 2;
            break;
        }
        case ScriptOp::SetValue: {
            const u32 v = pop(t);
            const u32 id = pop(t);
            host_.set_value(s(id), s(v));
            pc = pc + 1;
            break;
        }
        case ScriptOp::Attach: {
            const u32 c = pop(t), b = pop(t), a = pop(t);
            host_.attach_unit(s(a), s(b), s(c));
            pc = pc + 1;
            break;
        }
        case ScriptOp::Drop: host_.drop_unit(s(pop(t))); pc = pc + 1; break;
        case ScriptOp::Invalid:
        default:
            // Waiting callers are not woken.
            kill(t);
            running = false;
            break;
        }
    }
}

void ScriptInstance::animate(std::int32_t ticks) {
    turned_ = false;
    if (ticks == 0 || !dirty_) return;
    dirty_ = false;
    const std::size_t pieces = program_.piece_names.size();
    for (std::size_t p = 0; p < pieces; ++p) {
        std::int32_t* a = &anims_[p * kPieceWords];
        if (a[kActive] == 0) continue;
        a[kActive] = 0;
        const i32 piece = static_cast<i32>(p);
        for (i32 axis = 0; axis < 3; ++axis) {
            // Linear move.
            if (a[kMoveSpeed + axis] != 0) {
                const i32 speed = a[kMoveSpeed + axis];
                const i32 next = s(u(host_.get_piece_move(piece, axis)) + u(ticks) * u(speed));
                const i32 target = a[kMoveTarget + axis];
                i32 value;
                if (speed > 0 ? next < target : next > target) {
                    a[kActive] = 1;
                    value = next;
                } else {
                    value = target;
                    a[kMoveSpeed + axis] = 0;
                }
                host_.set_piece_move(piece, axis, value);
            }
            // Spin acceleration towards the target speed.
            if (a[kSpinAccel + axis] != 0) {
                a[kTurnSpeed + axis] = s(u(a[kTurnSpeed + axis]) + u(a[kSpinAccel + axis]));
                const bool reached = a[kSpinAccel + axis] > 0 ? !(a[kTurnSpeed + axis] < a[kSpinTarget + axis])
                                                              : !(a[kTurnSpeed + axis] > a[kSpinTarget + axis]);
                if (reached) {
                    a[kTurnSpeed + axis] = a[kSpinTarget + axis];
                    a[kSpinAccel + axis] = 0;
                }
            }
            // Rotation (towards a target, or endless spin when the target is -1).
            if (a[kTurnSpeed + axis] != 0) {
                const i32 cur = host_.get_piece_turn(piece, axis);
                const u32 step = u(ticks) * u(a[kTurnSpeed + axis]);
                u32 value = u(cur) + step;
                const i32 target = a[kTurnTarget + axis];
                turned_ = true;
                last_turn_target_ = u(target);
                if (target == -1) {
                    a[kActive] = 1;
                } else if (a[kTurnSpeed + axis] > 0) {
                    if (wrap_keep_sign(u(target) - u(cur) + 0x10000u) > s(step)) {
                        a[kActive] = 1;
                    } else {
                        value = u(target);
                        a[kTurnSpeed + axis] = 0;
                    }
                } else {
                    if (wrap_keep_sign(u(cur) - u(target) + 0x10000u) > s(0u - step)) {
                        a[kActive] = 1;
                    } else {
                        value = u(target);
                        a[kTurnSpeed + axis] = 0;
                    }
                }
                host_.set_piece_turn(piece, axis, s(value & 0xFFFFu));
            }
        }
        if (a[kActive] != 0) dirty_ = true;
    }
}

} // namespace sim
