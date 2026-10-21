#pragma once
// The original's runtime library as a player of the shipped game can meet it (docs/RE_MEMORY.md,
// "Runtime library and crash log"): its command-line switches (0x4D9F60 / 0x4D9FE0, the
// game's skip list 0x4DA0E0), its start-up from WinMain (0x4DA1D0(8): the crash handler,
// -fpufussy, -debughelper), the crash log it appends to ErrorLog.txt beside the program
// (0x4D8E60) and the game's out-of-memory handler (0x49E700).
//
// The texts are built here from plain values (testable without a crash); the Windows part
// installs the handlers and fills the values from a real exception.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ta::runtime {

// ---- switches -----------------------------------------------------------------------------

// 0x4D9F60: where a switch's text ends in the whole command line (the program's path counts
// too), compared case-insensitively, followed by the line's end, a white-space character or
// '='; std::string_view::npos when absent (an empty switch is never found).
std::size_t find_switch(std::string_view command_line, std::string_view sw);
// 0x4D9FE0: the default, 1 when one of the "on" switches is present, 0 when one of the "off"
// switches is (they win).
bool switch_value(std::string_view command_line, bool fallback, const char* on1, const char* off1, const char* on2 = nullptr,
                  const char* off2 = nullptr);
// 0x4DA0E0: the game's own parser skips a word beginning (case-insensitively) with one of the
// library's nineteen switches.
const std::array<const char*, 19>& library_switches();
bool is_library_switch(std::string_view word);

// What the library reads (each once, when first asked).
struct Switches {
    bool fpufussy = false;       // -fpufussy / -fpunofussy (default off): the FPU's invalid and divide traps on
    bool debughelper = false;    // -debughelper: DebugHelper.dll's DebugFunc1, then the program ends
    bool memfussy = false;       // -memfussy / -memnofussy / -memfrontalign: the guarded heap
    bool backalign = true;       // off with -memfrontalign
    bool gonzo = true;           // off with -gonzo
    bool setmemory = false;      // -memset / -memnoset: new blocks filled
    std::uint32_t setvalue = 0xDEADBEEFu;  // -memset=<n> (0x... hex, else decimal)
    bool imagehlp = true;        // -enableimagehlp / -disableimagehlp: the crash log's stack walk
};
Switches read_switches(std::string_view command_line);

// The -debughelper message (0x4DA150 when the DLL is absent, as shipped), box title "Cavedog".
constexpr const char* kNoDebugHelper = "Couldn't find library 'debughelper.dll'\n";

// ---- the crash log (0x4D8E60) --------------------------------------------------------------

// 0x4D98C0: the exception's name ("Unknown exception type" when not in the table).
std::string exception_name(std::uint32_t code);

struct CrashRecord {
    std::uint32_t code = 0, flags = 0;
    std::uint64_t address = 0;
    std::vector<std::uint64_t> params;
    // Registers (the original's 32-bit names; a 64-bit program gives its full registers).
    std::uint64_t eax = 0, ebx = 0, ecx = 0, edx = 0, esi = 0, edi = 0, ebp = 0, esp = 0, eip = 0;
    std::uint32_t eflags = 0, cs = 0, ss = 0, ds = 0, es = 0, fs = 0, gs = 0, context_flags = 0;
    std::array<std::uint64_t, 6> dr{};  // Dr0, Dr1, Dr2, Dr3, Dr6, Dr7
    std::uint32_t fpu_control = 0, fpu_status = 0, fpu_tag = 0, fpu_error_offset = 0, fpu_error_selector = 0, fpu_data_offset = 0,
                  fpu_data_selector = 0, cr0_npx_state = 0;
    std::array<std::uint8_t, 16> code_bytes{};  // at CS:EIP
    std::vector<std::uint64_t> call_stack;      // the instruction pointer first, at most 30
    std::uint64_t stack_start = 0;              // the stack pointer the dump starts at
    std::vector<std::uint32_t> stack_words;     // at most 0x800, as long as readable
};

struct MachineInfo {
    std::string time;               // asctime() of the local time ("Wed Oct 21 12:00:00 2026\n")
    std::string program;            // the program's path
    std::string user = "unknown user";
    std::string machine = "unknown machine";  // %computername%
    bool exe_dated = false;         // the program file's size and last write time could be read
    std::uint32_t exe_size = 0;
    std::uint16_t dos_date = 0, dos_time = 0;
    std::uint32_t link_stamp = 0;   // the program's PE time stamp
    std::string link_time;          // asctime() of it as UTC
    std::uint32_t processors = 1;
    std::uint64_t total_phys = 0;   // bytes
    std::uint64_t stack_from = 0, stack_to = 0;
};

// 0x4DED60: the machine's part of the log.
std::string machine_text(const MachineInfo& m);
// 0x4D9CA0: "Call stack:" and "Stack dump:" (the walk's text, at most 0xA44C bytes).
std::string stack_text(const CrashRecord& r);
// The log's first two lines (written as they are, before the rest): "<program> caused an
// <exception> in" and "module <program file> at <cs>:<eip>.".
std::string crash_log_head(const std::string& program_path, const CrashRecord& r);
// The rest (0x7358 bytes at most; line ends made CR LF by 0x4DA3F0): "Exception handler called
// in <title>. ", the machine, the exception, the registers, the code bytes, the stack, the debug
// and FPU registers, five empty lines.
std::string crash_log_body(const std::string& title, const CrashRecord& r, const MachineInfo& m);

// The out-of-memory handler's own line and its box (0x49E700).
constexpr const char* kOutOfMemory = "Out of memory!\r\nYour hard disk may be full\r\n";
constexpr const char* kErrorLog = "ErrorLog.txt";

// ---- the program's side ---------------------------------------------------------------------

// The library's start-up as the game's WinMain runs it (0x4DA1D0(8) and 0x4D8E50(0x49E700)):
// -debughelper ends the program after its message; the crash handler is installed (unhandled
// exceptions append the crash log to ErrorLog.txt beside the program, then go on to the system);
// -fpufussy unmasks the invalid-operation and divide-by-zero traps; allocation failures run the
// out-of-memory handler. `title` names the program in the out-of-memory box.
void start(const std::string& command_line, const std::string& title);
// The out-of-memory handler (0x49E700): its line to ErrorLog.txt, the crash log of a division
// by zero made on purpose ("Out of memory handler"), the system-modal box, exit code 3.
[[noreturn]] void out_of_memory();
// The crash log of a real exception (the unhandled-exception path; the debug console command's
// division by zero and DebugBreak end there).
[[noreturn]] void divide_by_zero();
[[noreturn]] void debug_break();

}  // namespace ta::runtime
