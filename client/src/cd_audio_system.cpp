// The system's side of CD audio (see client/cd_audio_system.h).

#include "client/cd_audio_system.h"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace client {

bool file_opens_read_write(const std::string& path) {
    // (std::fstream opened with in | out: the C library's "r+" mode)
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) return false;
    std::fclose(f);
    return true;
}

#ifdef _WIN32

namespace {

class SystemCdDevice : public CdCommandDevice {
public:
    explicit SystemCdDevice(std::function<void*()> window) : window_(std::move(window)) {}
    std::uint32_t send(const std::string& command, std::string* reply, int reply_size, bool notify) override {
        char buf[512] = {};
        const int n = reply && reply_size > 0 ? (reply_size < static_cast<int>(sizeof buf) ? reply_size : static_cast<int>(sizeof buf)) : 0;
        HWND w = notify && window_ ? static_cast<HWND>(window_()) : nullptr;
        const MCIERROR r = mciSendStringA(command.c_str(), n ? buf : nullptr, static_cast<UINT>(n), w);
        if (reply && n) *reply = buf;  // (the system writes its answer, empty on failure)
        return static_cast<std::uint32_t>(r);
    }

private:
    std::function<void*()> window_;
};

class SystemAux : public CdAuxVolume {
public:
    int count() override { return static_cast<int>(auxGetNumDevs()); }
    bool caps(int id, int& technology) override {
        AUXCAPSA c{};
        if (auxGetDevCapsA(static_cast<UINT_PTR>(id), &c, sizeof c) != MMSYSERR_NOERROR) return false;
        technology = c.wTechnology;
        return true;
    }
    bool get(int id, std::uint32_t& volume) override {
        DWORD v = 0;
        if (auxGetVolume(static_cast<UINT>(id), &v) != MMSYSERR_NOERROR) return false;
        volume = v;
        return true;
    }
    bool set(int id, std::uint32_t volume) override { return auxSetVolume(static_cast<UINT>(id), volume) == MMSYSERR_NOERROR; }
};

class SystemDrives : public CdDrives {
public:
    unsigned drive_type(char letter) override {
        char root[4] = {letter, ':', '\\', 0};
        return GetDriveTypeA(root);
    }
    bool volume_serial(char letter, std::uint32_t& serial) override {
        char root[4] = {letter, ':', '\\', 0};
        char name[64], fs[64];
        DWORD s = 0, max_len = 0, flags = 0;
        // (no error dialog for an empty drive)
        const UINT old = SetErrorMode(SEM_FAILCRITICALERRORS);
        const BOOL ok = GetVolumeInformationA(root, name, sizeof name, &s, &max_len, &flags, fs, sizeof fs);
        SetErrorMode(old);
        if (!ok) return false;
        serial = s;
        return true;
    }
};

BOOL CALLBACK find_player(HWND w, LPARAM lp) {
    char cls[200] = {};
    GetClassNameA(w, cls, 0xC7);
    if (std::string(cls) == "SJE_CdPlayerClass") *reinterpret_cast<HWND*>(lp) = w;
    return TRUE;
}

WNDPROC g_old_proc = nullptr;
std::function<void(std::uint32_t, std::uintptr_t)> g_tap;

LRESULT CALLBACK tap_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_tap) g_tap(static_cast<std::uint32_t>(msg), static_cast<std::uintptr_t>(wp));
    return CallWindowProcA(g_old_proc, w, msg, wp, lp);
}

}  // namespace

std::unique_ptr<CdCommandDevice> system_cd_device(std::function<void*()> window) { return std::make_unique<SystemCdDevice>(std::move(window)); }
std::unique_ptr<CdAuxVolume> system_aux_volume() { return std::make_unique<SystemAux>(); }
std::unique_ptr<CdDrives> system_cd_drives() { return std::make_unique<SystemDrives>(); }

std::uintptr_t find_cd_player_window() {
    HWND found = nullptr;
    EnumWindows(find_player, reinterpret_cast<LPARAM>(&found));
    return reinterpret_cast<std::uintptr_t>(found);
}

void close_cd_player_window(std::uintptr_t window) {
    HWND w = reinterpret_cast<HWND>(window);
    SendMessageA(w, WM_CLOSE, 0, 0);
    SendMessageA(w, WM_QUIT, 0, 0);
    Sleep(500);
}

void tap_window_messages(void* window, std::function<void(std::uint32_t, std::uintptr_t)> fn) {
    HWND w = static_cast<HWND>(window);
    if (!w) return;
    g_tap = std::move(fn);
    if (!g_old_proc) g_old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&tap_proc)));
}

#else

std::unique_ptr<CdCommandDevice> system_cd_device(std::function<void*()>) { return nullptr; }
std::unique_ptr<CdAuxVolume> system_aux_volume() { return std::make_unique<CdAuxVolume>(); }
std::unique_ptr<CdDrives> system_cd_drives() { return std::make_unique<CdDrives>(); }
std::uintptr_t find_cd_player_window() { return 0; }
void close_cd_player_window(std::uintptr_t) {}
void tap_window_messages(void*, std::function<void(std::uint32_t, std::uintptr_t)>) {}

#endif

}  // namespace client
