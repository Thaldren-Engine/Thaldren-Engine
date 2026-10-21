// The online-service buttons' links (spec: docs/RE_MEMORY.md, "Online services"; header
// online_services.h). The rules below are what online.dll (version 3, 1998-04-02) and its six
// extension DLLs (1997) do; addresses are theirs (image base 0x10000000).

#include "ta/online_services.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ta {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// strncat(dst, src, n) into an emptied buffer: at most n characters.
std::string cut(const std::string& s, std::size_t n) { return s.size() > n ? s.substr(0, n) : s; }

// The C string at the start of a registry value's bytes.
std::string c_string(const std::vector<std::uint8_t>& d) {
    std::string s;
    for (std::uint8_t b : d) {
        if (!b) break;
        s.push_back(static_cast<char>(b));
    }
    return s;
}

// A program path from a registry folder value (the extensions' 0x10001070 tail): the value's
// last character before its end (byte count - 2) not '\' or '/' (strchr("\\/", c), which also
// finds the terminating zero) -> a '\' appended, then the program's name.
bool folder_program(const OnlineSystem& sys, bool user, const char* key, const char* value, const char* program, std::string& path) {
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
    if (!sys.query || sys.query(user, key, value, 0x104, type, data) != 0) return false;
    if (data.size() < 2 || (type != 1 && type != 2)) return false;
    const std::uint8_t last = data[data.size() - 2];
    path = c_string(data);
    if (last != '\\' && last != '/' && last != 0) path += "\\";
    path += program;
    return true;
}

// The six extensions (their export tables name them so).
const char* const kKnown[] = {"TADWNGOX", "TAHEATX", "TAKALIX", "TAMPLAYX", "TATENX", "TAWIREPX"};

constexpr const char* kHeatKey = "Software\\Segasoft\\HEAT\\Main";
constexpr const char* kKaliKey = "Software\\Kali\\Kali95\\Misc";
constexpr const char* kMplayerKey = "Software\\Mpath\\Mplayer\\Main\\";
constexpr const char* kWireplayKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Wireplay.exe";

// A PE file's export table: the module name and the exported names (empty when it is not a
// 32-bit x86 DLL with exports).
bool pe_exports(const std::vector<std::uint8_t>& f, std::string& module, std::vector<std::string>& names) {
    auto u16 = [&](std::size_t o) -> std::uint32_t { return o + 2 <= f.size() ? static_cast<std::uint32_t>(f[o] | (f[o + 1] << 8)) : 0; };
    auto u32 = [&](std::size_t o) -> std::uint32_t { return o + 4 <= f.size() ? (u16(o) | (u16(o + 2) << 16)) : 0; };
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return false;
    const std::size_t pe = u32(0x3C);
    if (pe + 24 > f.size() || u32(pe) != 0x00004550) return false;
    if (u16(pe + 4) != 0x14C || !(u16(pe + 22) & 0x2000)) return false;  // i386, a DLL
    const std::size_t opt = pe + 24, nsec = u16(pe + 6), optsize = u16(pe + 20);
    if (u16(opt) != 0x10B) return false;
    const std::uint32_t exp_rva = u32(opt + 96);
    if (!exp_rva) return false;
    const std::size_t sec0 = opt + optsize;
    auto off = [&](std::uint32_t rva) -> std::size_t {
        for (std::size_t i = 0; i < nsec; ++i) {
            const std::size_t s = sec0 + i * 40;
            const std::uint32_t va = u32(s + 12), vs = std::max(u32(s + 8), u32(s + 16)), raw = u32(s + 20);
            if (rva >= va && rva < va + vs) return raw + (rva - va);
        }
        return f.size();
    };
    auto str = [&](std::uint32_t rva) {
        std::string s;
        for (std::size_t o = off(rva); o < f.size() && f[o]; ++o) s.push_back(static_cast<char>(f[o]));
        return s;
    };
    const std::size_t e = off(exp_rva);
    if (e + 40 > f.size()) return false;
    module = str(u32(e + 12));
    const std::uint32_t n = u32(e + 24), table = u32(e + 32);
    for (std::uint32_t i = 0; i < n && i < 4096; ++i) names.push_back(str(u32(off(table) + 4 * i)));
    return true;
}

}  // namespace

std::string OnlineServices::extension_name(const std::vector<std::uint8_t>& pe) {
    std::string module;
    std::vector<std::string> names;
    if (!pe_exports(pe, module, names)) return {};
    // 0x10001A50: TotalAExtVersion (every one returns 0x30000, accepted up to major 3) and
    // TotalAExtGetButtonText.
    if (std::find(names.begin(), names.end(), "TotalAExtVersion") == names.end() ||
        std::find(names.begin(), names.end(), "TotalAExtGetButtonText") == names.end())
        return {};
    std::string m = upper(module);
    if (m.size() > 4 && m.compare(m.size() - 4, 4, ".DLL") == 0) m.resize(m.size() - 4);
    return m;
}

bool OnlineServices::is_online_dll(const std::vector<std::uint8_t>& pe) {
    std::string module;
    std::vector<std::string> names;
    if (!pe_exports(pe, module, names)) return false;
    for (const char* need : {"ONLGetVersion", "ONLGetLinkInfo", "ONLProcessButtonCommand"})
        if (std::find(names.begin(), names.end(), need) == names.end()) return false;
    return true;
}

OnlineServices::OnlineServices(OnlineSystem system, std::string program_dir) : sys_(std::move(system)), dir_(std::move(program_dir)) {
    // GetModuleFileNameA cut after its last '/' or '\' (0x45B490 / 0x100019E0).
    if (!dir_.empty() && dir_.back() != '\\' && dir_.back() != '/') dir_ += "\\";
}

// LoadLibraryA(<program folder>online.dll) and ONLGetVersion() > 2 (0x45B490, 0x45B250).
bool OnlineServices::online_loads() const {
    if (!sys_.read_file) return false;
    return is_online_dll(sys_.read_file(dir_ + "online.dll"));
}

std::string OnlineServices::button_text(const std::string& m) const {
    // TotalAExtGetButtonText(buffer, 0x20, program folder): its text, at most 31 characters.
    if (m == "TADWNGOX") return "DWANGO";
    if (m == "TATENX") return "Play Free on TEN";
    if (m == "TAHEATX") {
        // 0x10001060: HKLM Software\Segasoft\HEAT\Main opens and its "Current Version" fits a
        // 10-byte buffer.
        std::uint32_t type = 0;
        std::vector<std::uint8_t> d;
        const bool on = sys_.query && sys_.query(false, kHeatKey, "Current Version", 10, type, d) == 0;
        return on ? "Play on HEAT" : "Get HEAT";
    }
    if (m == "TAKALIX") return sys_.key_opens && sys_.key_opens(true, kKaliKey) ? "Play on Kali" : "Get Kali";
    if (m == "TAMPLAYX") {
        // 0x10001070 (second argument 0): ta.mpi beside the program, or the Mplayer key opens.
        const bool on = (sys_.exists && sys_.exists(dir_ + "ta.mpi")) || (sys_.key_opens && sys_.key_opens(false, kMplayerKey));
        return on ? "Play on Mplayer" : "Get Mplayer";
    }
    if (m == "TAWIREPX") return sys_.key_opens && sys_.key_opens(false, kWireplayKey) ? "Play on Wireplay" : "Get Wireplay (UK)";
    return {};
}

// 0x10001750: the extensions beside the program.
void OnlineServices::scan() {
    ext_.clear();
    if (!sys_.find_files) return;
    std::size_t mplayer = static_cast<std::size_t>(-1);
    static const char* const kNotExtensions[] = {"online.dll", "dsetup.dll", "smackw16.dll", "smacke16.dll", "smackw32.dll"};
    for (const OnlineSystem::Found& f : sys_.find_files(dir_, "ta*.dll")) {
        if (f.directory) continue;
        bool skip = false;
        for (const char* n : kNotExtensions) skip = skip || upper(f.name) == upper(n);
        if (!skip) {
            // 0x10001A50: LoadLibraryA, TotalAExtVersion <= 3.x, a button text not empty.
            Extension e;
            e.path = dir_ + f.name;
            const std::vector<std::uint8_t> bytes = sys_.read_file ? sys_.read_file(e.path) : std::vector<std::uint8_t>{};
            e.model = extension_name(bytes);
            if (std::find(std::begin(kKnown), std::end(kKnown), e.model) != std::end(kKnown)) e.text = cut(button_text(e.model), 31);
            if (!e.text.empty()) {
                // A file named ??mplay* is the Mplayer one (the last such).
                if (f.name.size() >= 7 && upper(f.name.substr(2, 5)) == "MPLAY") mplayer = ext_.size();
                ext_.push_back(e);
            }
        }
        if (ext_.size() == 10) break;
    }
    // The Mplayer extension changes places with the first.
    if (mplayer != static_cast<std::size_t>(-1) && mplayer > 0 && mplayer < 10) std::swap(ext_[0], ext_[mplayer]);
}

std::vector<OnlineLink> OnlineServices::links(const std::function<std::string(const std::string&)>& translate) {
    std::vector<OnlineLink> out;
    if (!online_loads()) return out;
    // ONLGetLinkInfo (0x10001BB0).
    scan();
    out.push_back({0, "www.cavedog.com"});
    out.push_back({1, "Unit Download"});
    for (std::size_t k = 0; k < ext_.size(); ++k) out.push_back({static_cast<std::uint32_t>(k + 2), cut(ext_[k].text, 0x1F)});
    // 0x45B490: each name translated (a translation replaces it, 31 characters at most).
    if (translate)
        for (OnlineLink& l : out) {
            const std::string t = translate(l.name);
            if (t != l.name) l.name = cut(t, 31);
        }
    return out;
}

int OnlineServices::cd_scan(const char* sub, std::string& out) const {
    // GetLogicalDriveStringsA; each CD-ROM drive (GetDriveTypeA 5) with the service's set-up
    // program on it: 3, the program; none: 4 (the installation CD wanted).
    std::vector<std::string> roots;
    const std::uint32_t n = sys_.drives ? sys_.drives(roots) : 0;
    if (n == 0) {
        out = "No CD-ROM drives found.";
        return -1;
    }
    for (const std::string& r : roots) {
        if (!sys_.drive_type || sys_.drive_type(r) != 5) continue;
        const std::string p = r + sub;
        if (sys_.exists && sys_.exists(p)) {
            out = cut(p, 0x103);
            return 3;
        }
    }
    return 4;
}

// TotalAExtAction(buffer, 0x104, program folder) of the extension in `slot`: -1 failed (the
// buffer: why), 1 a web address to open, 3 a program to start, 4 the installation CD wanted.
int OnlineServices::action(std::size_t slot, std::string& out) const {
    out.clear();
    const std::string& m = ext_[slot].model;
    if (m == "TADWNGOX") {
        out = "http://www.dwango.com/html/totala.html";
        return 1;
    }
    if (m == "TATENX") {
        out = "http://www.ten.net/totala";
        return 1;
    }
    if (m == "TAHEATX") {
        std::uint32_t type = 0;
        std::vector<std::uint8_t> d;
        if (sys_.query && sys_.query(false, kHeatKey, "Current Version", 10, type, d) == 0) {
            out = "http://www.heat.net/pub/totala";
            return 1;
        }
        return cd_scan("Internet\\HEAT\\katalyst.exe", out);
    }
    if (m == "TAKALIX") {
        std::string path;
        if (folder_program(sys_, true, kKaliKey, "Location", "Kali95.exe", path)) {
            out = cut(path, 0x103);
            return 3;
        }
        out = "http://www.kali.net/games/annihilation";
        return 1;
    }
    if (m == "TAMPLAYX") {
        // 0x10001070 (second argument 1): ta.mpi, or the key with its "Root Directory" (the
        // program path it makes there is not used).
        std::string path;
        const bool on = (sys_.exists && sys_.exists(dir_ + "ta.mpi")) || folder_program(sys_, false, kMplayerKey, "Root Directory", "Programs\\mplaunch.exe", path);
        if (on) {
            out = "http://www.mplayer.com/cgi-bin/mplayer-set-up.sh?dist=16Q";
            return 1;
        }
        return cd_scan("Internet\\Mplayer\\mplaynow.exe", out);
    }
    if (m == "TAWIREPX") {
        std::string path;
        if (folder_program(sys_, false, kWireplayKey, "Path", "Wireplay.exe", path)) {
            out = cut(path, 0x103);
            return 3;
        }
        return cd_scan("Internet\\Wireplay\\wire9527.exe", out);
    }
    return -1;
}

std::string OnlineServices::failure_text(const std::string& name) {
    // 0x505A08 with name, "\n\n", "\n\n" (sprintf into the 320-byte buffer).
    return "The " + name + " service failed or took\n\ntoo long. Please see the readme\n\nfile for more information.";
}

int OnlineServices::press(std::uint32_t id, std::string& message, const OnlineOpener& open, const std::function<std::string(const std::string&)>& translate) {
    auto tr = [&](const std::string& s) { return translate ? translate(s) : s; };
    auto fmt = [](const std::string& f, const std::string& a) {
        // _snprintf(buffer, 0x140, f, a) as the message: 319 characters at most.
        std::string s;
        const std::size_t p = f.find("%s");
        s = p == std::string::npos ? f : f.substr(0, p) + a + f.substr(p + 2);
        return cut(s, 0x13F);
    };
    message.clear();
    // 0x45B250: no call for id -1, without online.dll or below version 3: 2.
    if (id == 0xFFFFFFFFu || !online_loads()) return 2;
    auto opened = [&](const std::string& target, bool program) { return open && open(target, program); };
    int r = 2;
    // ONLProcessButtonCommand (0x100011E0).
    if (id == 0 || id == 1) {
        const char* url = id == 0 ? "http://www.cavedog.com" : "http://www.totalannihilation.com/ta/unit0.htm";
        if (opened(url, false)) return 0;
        message = fmt("Unable to browse to URL: %s", url);  // not translated
        return 2;
    }
    // getCommandForLink (0x10001C80): the extension loaded again, TotalAExtAction.
    std::string buf;
    const std::size_t slot = static_cast<std::size_t>(id - 2);
    int a;
    if (slot >= ext_.size()) {
        buf = "Internal error";
        a = -1;
    } else if (!sys_.read_file || extension_name(sys_.read_file(ext_[slot].path)) != ext_[slot].model) {
        buf = "Unable to load extension DLL.";
        a = -1;
    } else {
        a = action(slot, buf);
    }
    switch (a) {
    case 0:
        r = 0;
        break;
    case 1:
        if (!buf.empty() && opened(buf, false)) return 0;
        message = fmt(tr("Unable to browse to URL: %s"), buf);
        r = 2;
        break;
    case 3:
        if (!buf.empty() && opened(buf, true)) return 0;
        message = fmt(tr("Unable to execute command: %s"), buf);
        r = 2;
        break;
    case 4:
        message = cut(ext_[slot].text, 0x13F);
        r = 1;
        break;
    case -1:
        if (!buf.empty()) message = cut(tr(buf), 0x13F);
        r = 2;
        break;
    default:
        r = 2;
        break;
    }
    if (r == 1) {
        // 0x45B250: _snprintf(tmp, 0x78, the translated text, "\n\n", the button text); an
        // overflow keeps 119 characters; back into the 320-byte buffer.
        std::string t = tr("Please insert the Installation CD (Disc 1) and select%s\"%s\" again.");
        std::string s;
        std::size_t p = t.find("%s");
        if (p != std::string::npos) {
            s = t.substr(0, p) + "\n\n";
            t = t.substr(p + 2);
            p = t.find("%s");
            s += p == std::string::npos ? t : t.substr(0, p) + message + t.substr(p + 2);
        } else {
            s = t;
        }
        message = cut(s, 0x77);
    }
    return r;
}

OnlineSystem real_online_system() {
    OnlineSystem s;
    s.read_file = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
#ifdef _WIN32
    s.find_files = [](const std::string& dir, const std::string& pattern) {
        std::vector<OnlineSystem::Found> out;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do out.push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
        while (FindNextFileA(h, &fd));
        FindClose(h);
        return out;
    };
    s.exists = [](const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
    // The original is a 32-bit program: its registry is the 32-bit view.
    s.key_opens = [](bool user, const std::string& key) {
        HKEY k = nullptr;
        if (RegOpenKeyExA(user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) return false;
        RegCloseKey(k);
        return true;
    };
    s.query = [](bool user, const std::string& key, const std::string& value, std::uint32_t size, std::uint32_t& type, std::vector<std::uint8_t>& data) -> long {
        HKEY k = nullptr;
        const LSTATUS o = RegOpenKeyExA(user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &k);
        if (o != ERROR_SUCCESS) return o;
        data.assign(size, 0);
        DWORD t = 0, cb = size;
        const LSTATUS q = RegQueryValueExA(k, value.c_str(), nullptr, &t, data.data(), &cb);
        RegCloseKey(k);
        type = t;
        data.resize(q == ERROR_SUCCESS ? std::min<DWORD>(cb, size) : 0);
        return q;
    };
    s.drives = [](std::vector<std::string>& roots) -> std::uint32_t {
        char buf[0xF4] = {};
        const DWORD n = GetLogicalDriveStringsA(0xF3, buf);
        if (n == 0 || n > 0xF3) return n;
        for (const char* p = buf; *p; p += std::strlen(p) + 1) roots.emplace_back(p);
        return n;
    };
    s.drive_type = [](const std::string& root) { return static_cast<unsigned>(GetDriveTypeA(root.c_str())); };
#else
    s.find_files = [](const std::string&, const std::string&) { return std::vector<OnlineSystem::Found>{}; };
    s.exists = [](const std::string&) { return false; };
    s.key_opens = [](bool, const std::string&) { return false; };
    s.query = [](bool, const std::string&, const std::string&, std::uint32_t, std::uint32_t&, std::vector<std::uint8_t>&) -> long { return 2; };
    s.drives = [](std::vector<std::string>&) -> std::uint32_t { return 0; };
    s.drive_type = [](const std::string&) { return 0u; };
#endif
    return s;
}

}  // namespace ta
