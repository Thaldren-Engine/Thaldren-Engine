// The original's runtime library as the shipped game shows it (docs/RE_MEMORY.md, "Runtime
// library and crash log"). See ta/runtime_library.h.

#include "ta/runtime_library.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include "sim/x87.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#endif

namespace ta::runtime {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool space(char c) { return static_cast<unsigned char>(c) < 0x80 && std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string format(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

}  // namespace

// ---- switches -----------------------------------------------------------------------------

std::size_t find_switch(std::string_view line, std::string_view sw) {
    if (sw.empty() || line.empty()) return std::string_view::npos;
    for (std::size_t p = 0; p < line.size(); ++p) {
        bool same = p + sw.size() <= line.size();
        for (std::size_t k = 0; same && k < sw.size(); ++k) same = lower(line[p + k]) == lower(sw[k]);
        if (same) {
            const std::size_t e = p + sw.size();
            if (e == line.size() || space(line[e]) || line[e] == '=') return e;
        }
    }
    return std::string_view::npos;
}

bool switch_value(std::string_view line, bool fallback, const char* on1, const char* off1, const char* on2, const char* off2) {
    bool v = fallback;
    auto has = [&](const char* s) { return s && find_switch(line, s) != std::string_view::npos; };
    if (has(on1) || has(on2)) v = true;
    if (has(off1) || has(off2)) v = false;
    return v;
}

const std::array<const char*, 19>& library_switches() {
    // (0x50C908..0x50C950, in the table's order)
    static const std::array<const char*, 19> k = {"-memfussy",       "-memnofussy",          "-memfrontalign",       "-gonzo",
                                                  "-memset",         "-memnoset",            "-fpufussy",            "-fpunofussy",
                                                  "-dprinton",       "-dprintoff",           "-dprintfile",          "-memorystatus",
                                                  "-performancestatus", "-disableimagehlp",  "-enableimagehlp",      "-disableimagehlplines",
                                                  "-enableimagehlplines", "-debughelper",    "-saveresources"};
    return k;
}

bool is_library_switch(std::string_view word) {
    for (const char* sw : library_switches()) {
        const std::size_t n = std::strlen(sw);
        if (word.size() < n) continue;
        bool same = true;
        for (std::size_t k = 0; k < n && same; ++k) same = lower(word[k]) == sw[k];
        if (same) return true;
    }
    return false;
}

Switches read_switches(std::string_view line) {
    Switches s;
    s.fpufussy = switch_value(line, false, "-fpufussy", "-fpunofussy");                     // 0x4DF160
    s.memfussy = switch_value(line, false, "-memfussy", "-memnofussy", "-memfrontalign");    // 0x4D80D0
    s.backalign = switch_value(line, true, nullptr, "-memfrontalign");                       // 0x4DB760
    s.gonzo = switch_value(line, true, nullptr, "-gonzo");                                   // 0x4D8140
    s.setmemory = switch_value(line, false, "-memset", "-memnoset");                         // 0x4D8200
    s.imagehlp = switch_value(line, true, "-enableimagehlp", "-disableimagehlp");           // 0x4DE180
    s.debughelper = find_switch(line, "-debughelper") != std::string_view::npos;             // 0x4DA1D0
    // 0x4D8260: the value after "-memset" (blanks and '=' skipped): "0x" / "0X" read as hex,
    // else decimal; the default stays when nothing is read.
    const std::size_t at = find_switch(line, "-memset");
    if (at != std::string_view::npos) {
        std::string rest(line.substr(at));
        std::size_t k = 0;
        while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t' || rest[k] == '=')) ++k;
        unsigned int v = 0;
        int n = 0;
        if (k + 1 < rest.size() && rest[k] == '0' && (rest[k + 1] == 'x' || rest[k + 1] == 'X')) n = std::sscanf(rest.c_str() + k, "%x", &v);
        else n = std::sscanf(rest.c_str() + k, "%d", reinterpret_cast<int*>(&v));
        if (n == 1) s.setvalue = v;
    }
    return s;
}

namespace {
Switches g_switches;
}
const Switches& active_switches() { return g_switches; }

// ---- the crash log ------------------------------------------------------------------------

std::string exception_name(std::uint32_t code) {
    static const struct {
        std::uint32_t code;
        const char* name;
    } kNames[] = {{0x40010005u, "Control-C"},
                  {0x40010008u, "Control-Break"},
                  {0x80000002u, "Datatype Misalignment"},
                  {0x80000003u, "Breakpoint"},
                  {0xC0000005u, "Access Violation"},
                  {0xC0000006u, "In Page Error"},
                  {0xC0000017u, "No Memory"},
                  {0xC000001Du, "Illegal Instruction"},
                  {0xC0000025u, "Noncontinuable Exception"},
                  {0xC0000026u, "Invalid Disposition"},
                  {0xC000008Cu, "Array Bounds Exceeded"},
                  {0xC000008Du, "Float Denormal Operand"},
                  {0xC000008Eu, "Float Divide by Zero"},
                  {0xC000008Fu, "Float Inexact Result"},
                  {0xC0000090u, "Float Invalid Operation"},
                  {0xC0000091u, "Float Overflow"},
                  {0xC0000092u, "Float Stack Check"},
                  {0xC0000093u, "Float Underflow"},
                  {0xC0000094u, "Integer Divide by Zero"},
                  {0xC0000095u, "Integer Overflow"},
                  {0xC0000096u, "Privileged Instruction"},
                  {0xC00000FDu, "Stack Overflow"},
                  {0xC0000142u, "DLL Initialization Failed"},
                  {0xE06D7363u, "Microsoft C++ Exception"}};
    for (const auto& e : kNames)
        if (e.code == code) return e.name;
    return "Unknown exception type";
}

std::string machine_text(const MachineInfo& m) {
    // 0x4DED60, in this order.
    std::string s = "Time: " + m.time;
    s += m.program + ", run by " + m.user + " on " + m.machine + "\n";
    if (m.exe_dated)
        s += format("Executable is %d bytes long and dated %d/%d/%d %02d:%02d:%02d\n", static_cast<int>(m.exe_size), (m.dos_date >> 5) & 0xF,
                    m.dos_date & 0x1F, (m.dos_date >> 9) + 1980, m.dos_time >> 11, (m.dos_time >> 5) & 0x3F, (m.dos_time & 0x1F) * 2);
    s += format("UTC link time: %08lx - ", static_cast<unsigned long>(m.link_stamp)) + m.link_time;
    // (the library's own version and date, constants of the 1998 library)
    s += "Library version 996. Library date Jun 30 1998 16:08:53\n";
    s += m.processors < 2 ? std::string("1 processor\n") : format("%d processors\n", static_cast<int>(m.processors));
    s += format("%d MBytes physical memory\n", static_cast<int>(static_cast<std::uint32_t>(m.total_phys + 0xDBBA0u) >> 20));
    s += format("Stack goes from %08llX to %08llX\n", static_cast<unsigned long long>(m.stack_from), static_cast<unsigned long long>(m.stack_to));
    return s;
}

std::string stack_text(const CrashRecord& r) {
    // 0x4D9CA0: a budget of 0xA44C bytes; an entry is written only while more than 30 are left.
    std::string s;
    std::int64_t left = 0xA44C;
    auto add = [&](const std::string& t) {
        s += t;
        left -= static_cast<std::int64_t>(t.size());
    };
    const int n = static_cast<int>(r.call_stack.size());
    if (n > 0) {
        add("Call stack:\n");
        for (int i = 0; i < n; ++i) {
            if (left < 0x1F) break;
            add(format("%08llX", static_cast<unsigned long long>(r.call_stack[static_cast<std::size_t>(i)])) +
                (i == n - 1 || i % 8 == 7 ? "\n" : " "));
        }
    }
    const int w = static_cast<int>(r.stack_words.size());
    if (w > 0 && left > 0x1E) {
        add("Stack dump:\n");
        for (int i = 0; i < w; ++i) {
            if (left < 0x1F) break;
            if (i % 8 == 0) add(format("%08llX: ", static_cast<unsigned long long>(r.stack_start + static_cast<std::uint64_t>(i) * 4)));
            add(format("%08lX", static_cast<unsigned long>(r.stack_words[static_cast<std::size_t>(i)])) +
                (i == w - 1 || i % 8 == 7 ? "\n" : " "));
        }
    }
    return s;
}

std::string crash_log_head(const std::string& program_path, const CrashRecord& r) {
    // The program's file name (after the last '\'), and that without what follows its last '.'.
    std::string file = program_path;
    const std::size_t slash = file.find_last_of('\\');
    if (slash != std::string::npos) file = file.substr(slash + 1);
    std::string base = file;
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    return base + " caused an " + exception_name(r.code) + " in\n" +
           format("module %s at %04x:%08llx.\n", file.c_str(), static_cast<unsigned>(r.cs), static_cast<unsigned long long>(r.eip));
}

std::string crash_log_body(const std::string& title, const CrashRecord& r, const MachineInfo& m) {
    constexpr std::size_t kSize = 0x7358;
    std::string s = "Exception handler called in " + title + ". ";
    {
        // (0x4DED60 copies at most the room left, less one)
        std::string mt = machine_text(m);
        const std::size_t room = kSize - s.size();
        if (mt.size() > room - 1) mt.resize(room - 1);
        s += mt;
    }
    auto x8 = [](std::uint64_t v) { return format("%08llX", static_cast<unsigned long long>(v)); };
    s += "Instruction pointer is " + x8(r.eip) + "\n";
    s += format("ExceptionCode = %08lX", static_cast<unsigned long>(r.code));
    s += " - " + exception_name(r.code) + "\n";
    if (r.code == 0xC0000005u && r.params.size() >= 2) {
        // (0x4D8680, "was the address read-only", answers no in the shipped library)
        s += format("Access violation: Illegal %s, data address 0x%08llX\n", r.params[0] ? "write" : "read",
                    static_cast<unsigned long long>(r.params[1]));
    }
    s += format("ExceptionFlags = %08lX\t", static_cast<unsigned long>(r.flags));
    s += "ExceptionAddress = " + x8(r.address) + "\n";
    if (!r.params.empty()) {
        s += "Parameters = ";
        for (std::size_t i = 0; i < r.params.size(); ++i) s += x8(r.params[i]) + (i + 1 == r.params.size() ? "\n" : "\t");
    }
    s += "\n";
    s += "Registers:\n";
    s += "EAX=" + x8(r.eax) + format(" CS=%04lX", static_cast<unsigned long>(r.cs)) + " EIP=" + x8(r.eip) +
         format(" EFLGS=%08lX\n", static_cast<unsigned long>(r.eflags));
    s += "EBX=" + x8(r.ebx) + format(" SS=%04lX", static_cast<unsigned long>(r.ss)) + " ESP=" + x8(r.esp) + " EBP=" + x8(r.ebp) + "\n";
    s += "ECX=" + x8(r.ecx) + format(" DS=%04lX", static_cast<unsigned long>(r.ds)) + " ESI=" + x8(r.esi) +
         format(" FS=%08lX\n", static_cast<unsigned long>(r.fs));
    s += "EDX=" + x8(r.edx) + format(" ES=%04lX", static_cast<unsigned long>(r.es)) + " EDI=" + x8(r.edi) +
         format(" GS=%08lX\n", static_cast<unsigned long>(r.gs));
    s += "\n";
    s += "Bytes at CS:EIP:\n";
    for (int i = 0; i < 16; ++i) s += format("%02x%c", r.code_bytes[static_cast<std::size_t>(i)], i == 15 ? '\n' : ' ');
    s += "\n";
    const std::int64_t room = static_cast<std::int64_t>(kSize) - static_cast<std::int64_t>(s.size()) - 1000;
    if (room > 0) {
        // (lstrcpynA: at most room - 1 characters of the walk's text)
        std::string st = stack_text(r);
        if (static_cast<std::int64_t>(st.size()) > room - 1) st.resize(static_cast<std::size_t>(room - 1));
        s += st;
        s += "\n";
        s += "Dr0 = " + x8(r.dr[0]) + "\t";
        s += "Dr1 = " + x8(r.dr[1]) + "\t";
        s += "Dr2 = " + x8(r.dr[2]) + "\n";
        s += "Dr3 = " + x8(r.dr[3]) + "\t";
        s += "Dr6 = " + x8(r.dr[4]) + "\t";
        s += "Dr7 = " + x8(r.dr[5]) + "\n";
        s += "\n";
        s += format("ContextFlags = %08lX\n", static_cast<unsigned long>(r.context_flags));
        s += format("Control Word = %08lX\t\t", static_cast<unsigned long>(r.fpu_control));
        s += format("StatusWord = %08lX\n", static_cast<unsigned long>(r.fpu_status));
        s += format("TagWord = %08lX\t\t", static_cast<unsigned long>(r.fpu_tag));
        s += format("ErrorOffset = %08lX\n", static_cast<unsigned long>(r.fpu_error_offset));
        s += format("ErrorSelector = %08lX\t", static_cast<unsigned long>(r.fpu_error_selector));
        s += format("DataOffset = %08lX\n", static_cast<unsigned long>(r.fpu_data_offset));
        s += format("DataSelector = %08lX\t\t", static_cast<unsigned long>(r.fpu_data_selector));
        s += format("Cr0NpxState = %08lX\n", static_cast<unsigned long>(r.cr0_npx_state));
        s += "\n\n\n\n\n";
    }
    if (s.size() > kSize - 1) s.resize(kSize - 1);
    // 0x4DA3F0: with room for them, every LF (and a lone CR) becomes CR LF.
    std::size_t lf = 0;
    for (char c : s) lf += c == '\n';
    if (static_cast<std::int64_t>(kSize) - static_cast<std::int64_t>(s.size()) - 1 - static_cast<std::int64_t>(lf) > 0) {
        std::string o;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r') {
                o += "\r\n";
                if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
            } else if (s[i] == '\n') {
                o += "\r\n";
            } else {
                o += s[i];
            }
        }
        s = o;
    }
    return s;
}

// ---- the program's side -----------------------------------------------------------------------

#ifdef _WIN32
namespace {

std::string g_title;
bool g_logged = false;  // (0x5289C0: one crash log per run)

std::string program_path() {
    char buf[1000];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return n ? std::string(buf, n) : std::string();
}

std::string error_log_path() {
    // (0x4D8E60 / 0x49E700: the program's folder with its '\', else "C:\")
    std::string p = program_path();
    const std::size_t slash = p.find_last_of('\\');
    if (p.empty() || slash == std::string::npos) p = "C:\\";
    else p.resize(slash + 1);
    return p + kErrorLog;
}

HANDLE open_log() {
    const HANDLE h = CreateFileA(error_log_path().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) SetFilePointer(h, 0, nullptr, FILE_END);
    return h;
}

void write_all(HANDLE h, const std::string& s) {
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
}

MachineInfo machine_info() {
    MachineInfo m;
    const std::time_t now = std::time(nullptr);
    if (const std::tm* lt = std::localtime(&now)) m.time = std::asctime(lt);
    char user[300];
    DWORD un = sizeof user;
    if (GetUserNameA(user, &un)) m.user = user;
    if (const char* c = std::getenv("computername")) m.machine = c;
    m.program = program_path();
    if (m.program.empty()) m.program = "Unknown";
    const HANDLE f = CreateFileA(m.program.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        m.exe_size = GetFileSize(f, nullptr);
        FILETIME ft, lft;
        WORD d = 0, t = 0;
        if (GetFileTime(f, nullptr, nullptr, &ft) && FileTimeToLocalFileTime(&ft, &lft) && FileTimeToDosDateTime(&lft, &d, &t)) {
            m.exe_dated = true;
            m.dos_date = d;
            m.dos_time = t;
        }
        CloseHandle(f);
    }
    if (const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA(nullptr))) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        m.link_stamp = nt->FileHeader.TimeDateStamp;
        const std::time_t stamp = static_cast<std::time_t>(m.link_stamp);
        if (const std::tm* gt = std::gmtime(&stamp)) m.link_time = std::asctime(gt);
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    m.processors = si.dwNumberOfProcessors;
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) m.total_phys = ms.ullTotalPhys;
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    m.stack_from = reinterpret_cast<std::uint64_t>(tib->StackBase);
    m.stack_to = reinterpret_cast<std::uint64_t>(tib->StackLimit);
    return m;
}

CrashRecord record_of(const EXCEPTION_POINTERS* ep) {
    CrashRecord r;
    const EXCEPTION_RECORD& e = *ep->ExceptionRecord;
    const CONTEXT& c = *ep->ContextRecord;
    r.code = e.ExceptionCode;
    // A trap the SSE unit raised under -fpufussy names the x87's exception the original gets.
    if (r.code == 0xC00002B4u || r.code == 0xC00002B5u) {
#if defined(_M_X64)
        const DWORD mx = c.MxCsr;
#else
        DWORD mx = 0;
        std::memcpy(&mx, c.ExtendedRegisters + 24, 4);  // (FXSAVE layout: MXCSR at +24)
#endif
        if (mx & 1) r.code = 0xC0000090u;        // invalid operation
        else if (mx & 4) r.code = 0xC000008Eu;   // divide by zero
    }
    r.flags = e.ExceptionFlags;
    r.address = reinterpret_cast<std::uint64_t>(e.ExceptionAddress);
    for (DWORD i = 0; i < e.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) r.params.push_back(e.ExceptionInformation[i]);
#if defined(_M_X64)
    r.eax = c.Rax, r.ebx = c.Rbx, r.ecx = c.Rcx, r.edx = c.Rdx, r.esi = c.Rsi, r.edi = c.Rdi, r.ebp = c.Rbp, r.esp = c.Rsp, r.eip = c.Rip;
    r.fpu_control = c.FltSave.ControlWord, r.fpu_status = c.FltSave.StatusWord, r.fpu_tag = c.FltSave.TagWord;
    r.fpu_error_offset = c.FltSave.ErrorOffset, r.fpu_error_selector = c.FltSave.ErrorSelector;
    r.fpu_data_offset = c.FltSave.DataOffset, r.fpu_data_selector = c.FltSave.DataSelector;
#elif defined(_M_IX86)
    r.eax = c.Eax, r.ebx = c.Ebx, r.ecx = c.Ecx, r.edx = c.Edx, r.esi = c.Esi, r.edi = c.Edi, r.ebp = c.Ebp, r.esp = c.Esp, r.eip = c.Eip;
    r.fpu_control = c.FloatSave.ControlWord, r.fpu_status = c.FloatSave.StatusWord, r.fpu_tag = c.FloatSave.TagWord;
    r.fpu_error_offset = c.FloatSave.ErrorOffset, r.fpu_error_selector = c.FloatSave.ErrorSelector;
    r.fpu_data_offset = c.FloatSave.DataOffset, r.fpu_data_selector = c.FloatSave.DataSelector;
    r.cr0_npx_state = c.FloatSave.Spare0;  // (the Cr0NpxState word)
#endif
    r.eflags = c.EFlags, r.cs = c.SegCs, r.ss = c.SegSs, r.ds = c.SegDs, r.es = c.SegEs, r.fs = c.SegFs, r.gs = c.SegGs;
    r.context_flags = c.ContextFlags;
    r.dr = {c.Dr0, c.Dr1, c.Dr2, c.Dr3, c.Dr6, c.Dr7};
    // The bytes at the instruction pointer (unreadable: left 0).
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(r.eip), r.code_bytes.data(), r.code_bytes.size(), &got);
    // The call stack (0x4DE600: the instruction pointer, then up to 30 in all) and the stack's
    // words from the stack pointer (at most 0x800, while readable).
    r.call_stack.push_back(r.eip);
#if defined(_M_X64)
    {
        CONTEXT u = c;
        for (int k = 1; k < 30; ++k) {
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &image, nullptr);
            if (!fn) {
                if (!u.Rsp) break;
                u.Rip = *reinterpret_cast<const DWORD64*>(u.Rsp);
                u.Rsp += 8;
            } else {
                void* data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, u.Rip, fn, &u, &data, &frame, nullptr);
            }
            if (!u.Rip) break;
            r.call_stack.push_back(u.Rip);
        }
    }
#endif
    r.stack_start = r.esp;
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto top = reinterpret_cast<std::uint64_t>(tib->StackBase);
    for (std::uint64_t a = r.esp; a + 4 <= top && r.stack_words.size() < 0x800; a += 4)
        r.stack_words.push_back(*reinterpret_cast<const std::uint32_t*>(a));
    return r;
}

// 0x4D8E60: the log appended to ErrorLog.txt; 0 (the search goes on).
LONG write_crash_log(EXCEPTION_POINTERS* ep, const char* title) {
    if (g_logged) return EXCEPTION_CONTINUE_SEARCH;
    g_logged = true;
    const CrashRecord r = record_of(ep);
    const HANDLE h = open_log();
    write_all(h, crash_log_head(program_path(), r));
    write_all(h, crash_log_body(title, r, machine_info()));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI global_filter(EXCEPTION_POINTERS* ep) { return write_crash_log(ep, "Global Exception Handler"); }

void new_handler() { out_of_memory(); }

int zero() {
    static volatile int z = 0;
    return z;
}

// 0x49E680: a division by zero inside a guarded block whose filter writes the crash log.
void caught_division(const char* title) {
    __try {
        volatile int v = 1 / zero();
        (void)v;
    } __except (write_crash_log(GetExceptionInformation(), title), EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

// Tests (environment RUNTIME_TEST_QUIET): the boxes are printed instead of shown and a crash ends
// the process without the system's crash dialog.
static bool quiet() {
    static const bool q = std::getenv("RUNTIME_TEST_QUIET") != nullptr;
    return q;
}

static void box(const char* text, const char* caption, UINT flags) {
    if (quiet()) {
        std::printf("box [%s] %08X: %s\n", caption, static_cast<unsigned>(flags), text);
        std::fflush(stdout);
        return;
    }
    MessageBoxA(nullptr, text, caption, flags);
}

void start(const std::string& command_line, const std::string& title) {
    g_title = title;
    if (quiet()) SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    const Switches s = read_switches(command_line);
    g_switches = s;
    if (s.debughelper) {
        // 0x4DA150 -> 0x4DA120: the library is not there (as shipped): the message, then the
        // program ends (0x4DA0C0: raise(SIGABRT), whose default ends it with exit code 3).
        OutputDebugStringA(kNoDebugHelper);
        box(kNoDebugHelper, "Cavedog", MB_OK);
        std::_Exit(3);
    }
    SetUnhandledExceptionFilter(global_filter);
    if (s.fpufussy) {
        // 0x4DF160: _controlfp(0, 0x18): the invalid-operation and divide-by-zero traps unmasked.
        // Here: the floating-point unit's traps (SSE, where a 64-bit program computes) and the
        // simulation's out-of-range conversions, which it makes in software (the x87's FISTP
        // signals the invalid operation there).
        unsigned int cur = 0;
        _controlfp_s(&cur, 0, _EM_INVALID | _EM_ZERODIVIDE);
        sim::x87::invalid_traps() = true;
    }
    std::set_new_handler(new_handler);
}

void out_of_memory() {
    // 0x49E700: the line, the crash log of a division by zero made on purpose and caught
    // (0x49E680), the box (MB_ICONHAND | MB_SYSTEMMODAL | MB_TOPMOST), then raise(SIGABRT): exit
    // code 3.
    std::set_new_handler(nullptr);
    {
        const HANDLE h = open_log();
        write_all(h, kOutOfMemory);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    caught_division("Out of memory handler");
    box(kOutOfMemory, g_title.c_str(), MB_ICONHAND | MB_SYSTEMMODAL | MB_TOPMOST);
    std::_Exit(3);
}

void divide_by_zero() {
    volatile int v = 1 / zero();
    (void)v;
    std::_Exit(3);  // (not reached: the division traps)
}

void debug_break() {
    DebugBreak();
    std::_Exit(3);  // (not reached without a debugger)
}

#else

void start(const std::string& command_line, const std::string&) { g_switches = read_switches(command_line); }
void out_of_memory() {
    std::fputs(kOutOfMemory, stderr);
    std::_Exit(3);
}
void divide_by_zero() { std::abort(); }
void debug_break() { std::abort(); }

#endif

}  // namespace ta::runtime
