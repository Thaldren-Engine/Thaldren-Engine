#pragma once
// Unit script virtual machine: cooperative threads running word-coded
// programs that animate model pieces and query/drive their unit.
//
// Behaviour is specified in docs/RE_MEMORY.md ("Unit script VM") and must stay
// tick-for-tick identical to it, including its quirks (uninitialised locals,
// stack overruns into neighbouring threads, unrelated-opcode thread kills).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sim {

enum class ScriptOp : std::uint8_t {
    Invalid,  // kills the thread
    Move, Turn, Spin, StopSpin,
    Show, Hide, Cache, DontCache, Shade, DontShade,
    LegacyExplode, PlaySound, EmitSfx,
    MoveNow, TurnNow, WaitTurn, WaitMove, Sleep,
    PushConst, PushLocal, PushStatic, PushStale,
    CreateLocal, PopLocal, PopStatic, PopNone, PopStack,
    Add, Sub, Mul, Div, BitAnd, BitOr, BitXor, BitNot,
    Rand, GetValue, GetValueArgs, GetTransporting, GetCarrier,
    Lt, Le, Gt, Ge, Eq, Ne, LogAnd, LogOr, Xor2, LogNot,
    StartScript, CallScript, DropArgs, Jump, Return, JumpIfZero,
    Signal, SetSignalMask, ExplodePiece, SetValue, Attach, Drop,
    Count_
};

// A loaded program. `ops[i]` is how word i decodes when execution reaches it
// (every word is decoded, so jumps into operands behave like the original).
struct ScriptProgram {
    std::vector<std::uint32_t> code;
    std::vector<ScriptOp> ops;
    std::vector<std::uint32_t> entry;       // per function: word index
    std::vector<std::string> function_names;
    std::vector<std::string> piece_names;
    std::int32_t static_count = 0;

    // Exact (case-sensitive) name lookup; -1 if absent.
    int find_function(const std::string& name) const;
};

// Receives a thread's return value (the value on top of its stack).
class ScriptReturnSink {
public:
    virtual ~ScriptReturnSink() = default;
    virtual void on_return(std::int32_t value) = 0;
    // The word stored in the thread to mark the sink (scripts can read it back by
    // popping an empty stack). Must be nonzero.
    virtual std::uint32_t handle() const { return 1; }
};

// Everything a script can do to or ask of the world.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual void set_piece_move(std::int32_t piece, std::int32_t axis, std::int32_t value) = 0;
    virtual void set_piece_turn(std::int32_t piece, std::int32_t axis, std::int32_t value) = 0;
    virtual void set_piece_visible(std::int32_t piece, std::int32_t on) = 0;
    virtual void set_piece_cached(std::int32_t piece, std::int32_t on) = 0;
    virtual void set_piece_shaded(std::int32_t piece, std::int32_t on) = 0;
    virtual std::int32_t get_piece_move(std::int32_t piece, std::int32_t axis) = 0;
    virtual std::int32_t get_piece_turn(std::int32_t piece, std::int32_t axis) = 0;
    virtual void legacy_explode(std::int32_t piece, std::int32_t a, std::int32_t b) = 0;
    virtual void play_sound(std::int32_t sound) = 0;
    virtual void emit_sfx(std::int32_t piece, std::int32_t type) = 0;
    virtual void explode_piece(std::int32_t piece, std::int32_t type) = 0;
    virtual void attach_unit(std::int32_t unit, std::int32_t piece, std::int32_t mode) = 0;
    virtual void drop_unit(std::int32_t unit) = 0;
    virtual void set_value(std::int32_t id, std::int32_t value) = 0;
    virtual std::int32_t get_value(std::int32_t id, std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) = 0;
    virtual std::int32_t get_transporting(std::int32_t index) = 0;
    virtual std::int32_t get_carrier() = 0;
    // Bounded draw in [0, n) for n >= 2 (n < 2 yields 0 without drawing).
    virtual std::uint32_t random_mod(std::uint32_t n) = 0;
};

class ScriptInstance {
public:
    static constexpr int kThreads = 8;
    static constexpr int kThreadWords = 41;  // 9 header words + 32 stack/local words
    static constexpr int kPieceWords = 19;

    // Thread header word indices.
    enum : int { kState, kPc, kSp, kSleep, kWaitPiece, kWaitAxis, kWaitThread, kSignalMask, kSink, kStack };
    // Thread states.
    static constexpr std::uint32_t kFree = 0, kRunning = 0x01000000, kWaitingTurn = 0x02100000,
                                   kWaitingMove = 0x02200000, kSleeping = 0x02400000, kWaitingCall = 0x02800000;
    // Piece animation words.
    enum : int { kActive = 0, kMoveTarget = 1, kMoveSpeed = 4, kTurnTarget = 7, kTurnSpeed = 10, kSpinTarget = 13,
                 kSpinAccel = 16 };

    ScriptInstance(const ScriptProgram& program, ScriptHost& host, std::int32_t tick_rate);

    // Start `function` in the first free thread (signal mask 1); returns the thread or -1.
    int start(int function);
    // Start by name; if run_now, run every thread once (no time passes).
    bool call(const std::string& name, ScriptReturnSink* sink, bool run_now);
    // Start with four arguments in locals 0-3 and `arg_count` locals in use.
    bool call_with_args(int function, ScriptReturnSink* sink, bool run_now, std::int32_t arg_count,
                        std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d);
    // Start with four in/out arguments, run only that thread now, then copy locals 0-3 back.
    bool call_in_out(int function, std::int32_t* a, std::int32_t* b, std::int32_t* c, std::int32_t* d);
    // One simulation step: run every thread (sleep counters drop by `ticks`), then animate pieces.
    void tick(std::int32_t ticks);
    // The same, also updating the stack word the original's step leaves behind for its caller
    // (see docs/RE_MEMORY.md, "Stale stack word"): 7 if any thread was live, then the turn
    // target of the last rotation animated.
    void tick(std::int32_t ticks, std::uint32_t& stale_word);

    // State access (for saving, debugging and verification).
    std::uint32_t& thread_word(int thread, int word) { return mem_[static_cast<std::size_t>(thread * kThreadWords + word)]; }
    std::int32_t thread_count() const { return static_cast<std::int32_t>(mem_[kThreads * kThreadWords]); }
    const std::array<std::uint32_t, kThreads * kThreadWords + 1>& thread_memory() const { return mem_; }
    const ScriptProgram& program() const { return program_; }
    std::vector<std::int32_t>& statics() { return statics_; }
    std::vector<std::int32_t>& piece_anims() { return anims_; }  // kPieceWords per piece
    bool anim_dirty() const { return dirty_; }
    // Set when the original would have read or written outside its data (undefined behaviour there),
    // or when the optional step limit was hit.
    const std::string& fault() const { return fault_; }
    // Debugging aid: fault after this many executed instructions in total (0 = unlimited).
    void set_step_limit(std::uint64_t steps) { step_limit_ = steps; }
    // Debugging aid: counts executed instructions per op (array of ScriptOp::Count_ entries) when set.
    void set_op_counter(std::uint64_t* counts) { op_counts_ = counts; }

private:
    void run_thread(int t, std::int32_t ticks);
    void animate(std::int32_t ticks);
    void kill(int t);
    std::uint32_t pop(int t);
    void push(int t, std::uint32_t v);
    std::uint32_t& stack_word(int t, std::int32_t index);  // header-relative word (9 + sp ...)
    std::int32_t& anim(std::uint32_t piece, std::uint32_t axis, int field);
    std::int32_t& stat(std::uint32_t index);
    void set_fault(const char* what);

    const ScriptProgram& program_;
    ScriptHost& host_;
    std::int32_t tick_rate_;
    // Threads back to back, then the live thread count: overruns land where the original's would.
    std::array<std::uint32_t, kThreads * kThreadWords + 1> mem_{};
    std::array<ScriptReturnSink*, kThreads> sinks_{};
    std::vector<std::int32_t> statics_;
    std::vector<std::int32_t> anims_;
    bool dirty_ = false;
    bool turned_ = false;              // the last animate() stepped a rotation
    std::uint32_t last_turn_target_ = 0;
    std::uint32_t scratch_ = 0;
    std::string fault_;
    std::uint64_t step_limit_ = 0, steps_ = 0;
    std::uint64_t* op_counts_ = nullptr;
};

} // namespace sim
