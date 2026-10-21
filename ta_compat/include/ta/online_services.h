// The online-service buttons of the multiplayer connection screen (spec: docs/RE_MEMORY.md,
// "Online services"). The original asks the online.dll beside the program for its links
// (0x45B490: ONLGetVersion > 2, ONLGetLinkInfo) and passes a pressed button's link to it
// (0x45B250: ONLProcessButtonCommand). online.dll gives two fixed links and one per service
// extension DLL (ta*.dll beside the program); each extension's button text and press depend on
// whether its service is installed (a registry key, a file) and on a CD in a CD-ROM drive.
//
// The rewrite does not load these 32-bit DLLs: what they return is reproduced here as data and
// rules, the extensions being recognised by their export table (internal name and the
// extension entry points). The probes read the real registry and files (read only) through
// OnlineSystem; opening a web page or starting a program goes through the caller's hook.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ta {

// What the service DLLs look at, as the original's 32-bit process sees it.
struct OnlineSystem {
    struct Found {
        std::string name;       // the file's name (WIN32_FIND_DATA cFileName)
        bool directory = false; // attribute 0x10
    };
    // FindFirstFileA / FindNextFileA on dir + pattern, in the order the system gives.
    std::function<std::vector<Found>(const std::string& dir, const std::string& pattern)> find_files;
    // GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES.
    std::function<bool(const std::string& path)> exists;
    // The file's bytes (the DLLs' export tables; empty: unreadable).
    std::function<std::vector<std::uint8_t>(const std::string& path)> read_file;
    // RegOpenKeyExA(HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER, key, 0, KEY_READ) in the 32-bit
    // view succeeds.
    std::function<bool(bool current_user, const std::string& key)> key_opens;
    // RegQueryValueExA of `value` in that key with a buffer of `size` bytes: the result code
    // (0 success; the key not opening: its error), the type and the bytes returned.
    std::function<long(bool current_user, const std::string& key, const std::string& value, std::uint32_t size, std::uint32_t& type,
                       std::vector<std::uint8_t>& data)>
        query;
    // GetLogicalDriveStringsA(0xF3, buf): its result (0: failed) and the roots ("D:\").
    std::function<std::uint32_t(std::vector<std::string>& roots)> drives;
    // GetDriveTypeA(root) (5: CD-ROM).
    std::function<unsigned(const std::string& root)> drive_type;
};

// This machine (Windows: the system calls above, read only; elsewhere nothing is found).
OnlineSystem real_online_system();

// A link as ONLGetLinkInfo gives it (entries of 0x24 bytes: the id, a 32-byte name).
struct OnlineLink {
    std::uint32_t id = 0;
    std::string name;  // at most 31 characters
};

// What a press asks the platform to do: open a web address (ShellExecuteA "open", empty
// parameters and folder, shown normal) or start a program (CreateProcessA with the path as the
// command line). The hook returns whether it started (ShellExecuteA's result above 32,
// CreateProcessA's success).
using OnlineOpener = std::function<bool(const std::string& target, bool program)>;

class OnlineServices {
public:
    // program_dir: the folder of the original program (GetModuleFileNameA's folder, ending in
    // '\'), where online.dll and the extensions are looked for.
    OnlineServices(OnlineSystem system, std::string program_dir);

    // 0x45B490: online.dll loads and its version is above 2 -> ONLGetLinkInfo's entries (the
    // extensions looked for anew, 0x10001750), each name then translated by the caller's table
    // (`translate`, cut to 31 characters); otherwise none.
    std::vector<OnlineLink> links(const std::function<std::string(const std::string&)>& translate = {});

    // 0x45B250 (from the screen's handler 0x4441A0): ONLProcessButtonCommand(id, buffer, 320,
    // the translation 0x45B480). 0: started (the program then ends); 1: the extension wants the
    // installation CD ("Please insert the Installation CD (Disc 1) and select\n\n"TEXT" again."
    // in `message`); 2: failed, `message` the reason (empty: none given, also when online.dll
    // is missing or too old).
    int press(std::uint32_t id, std::string& message, const OnlineOpener& open, const std::function<std::string(const std::string&)>& translate = {});

    // The message the screen shows for a failed press without a reason (0x44439C), before the
    // message box's translation; `name` is the button's link name.
    static std::string failure_text(const std::string& name);

    // The extensions as the last links() found them (their file paths, internal names).
    struct Extension {
        std::string path;   // program_dir + file name
        std::string model;  // the recognised extension: "TADWNGOX" ... (upper case)
        std::string text;   // the button text
    };
    const std::vector<Extension>& extensions() const { return ext_; }

    // The extension a DLL file is (its export table's name, upper case, without ".DLL") when it
    // is a 32-bit DLL exporting TotalAExtVersion and TotalAExtGetButtonText; else empty.
    static std::string extension_name(const std::vector<std::uint8_t>& pe);
    // Whether a file is online.dll as shipped (a 32-bit DLL exporting ONLGetVersion,
    // ONLGetLinkInfo and ONLProcessButtonCommand; its ONLGetVersion returns 3).
    static bool is_online_dll(const std::vector<std::uint8_t>& pe);

private:
    bool online_loads() const;
    void scan();
    int action(std::size_t slot, std::string& out) const;  // TotalAExtAction
    std::string button_text(const std::string& model) const;  // TotalAExtGetButtonText
    int cd_scan(const char* sub, std::string& out) const;     // the extensions' CD search

    OnlineSystem sys_;
    std::string dir_;
    std::vector<Extension> ext_;
};

}  // namespace ta
