#pragma once
// The system's mouse trails while the program runs (docs/RE_MEMORY.md, "Mouse trails"): at
// start-up, before the window is made, the user's setting is saved
// (SystemParametersInfo(SPI_GETMOUSETRAILS = 0x5E, 0, &saved, SPIF_UPDATEINIFILE)) and the
// trails are switched off (SPI_SETMOUSETRAILS = 0x5D, 0, null, SPIF_UPDATEINIFILE); when the
// message loop ends (window closed, Exit to Windows) the saved value is put back
// (SPI_SETMOUSETRAILS, saved, null, SPIF_UPDATEINIFILE). Other ends (a fatal error's exit(1),
// an online service started with exit(0), a failed start-up after the switch) leave the trails
// off, and since the profile was written they stay off.
//
// The system call goes through a function given by the program (Windows: SystemParametersInfoA),
// so tests record the calls instead of changing the machine's setting.

#include <cstdint>
#include <functional>

namespace client {

// (action, uiParam, pvParam, fWinIni) -> the call's result (nonzero: done).
using SystemParametersCall = std::function<int(std::uint32_t action, std::uint32_t param, void* data, std::uint32_t flags)>;

class MouseTrails {
public:
    static constexpr std::uint32_t kGetMouseTrails = 0x5E;  // SPI_GETMOUSETRAILS
    static constexpr std::uint32_t kSetMouseTrails = 0x5D;  // SPI_SETMOUSETRAILS
    static constexpr std::uint32_t kUpdateIniFile = 1;      // SPIF_UPDATEINIFILE (no SPIF_SENDCHANGE)

    explicit MouseTrails(SystemParametersCall call) : call_(std::move(call)) {}

    // 0x4B5980: the setting saved, then the trails off. The saved word keeps what it held when
    // the query fails (the original's display record is a zeroed global: 0).
    void switch_off() {
        if (!call_) return;
        call_(kGetMouseTrails, 0, &saved_, kUpdateIniFile);
        call_(kSetMouseTrails, 0, nullptr, kUpdateIniFile);
    }
    // 0x4B6110 (WinMain after its message loop): the saved setting back.
    void restore() {
        if (!call_) return;
        call_(kSetMouseTrails, saved_, nullptr, kUpdateIniFile);
    }
    std::uint32_t saved() const { return saved_; }

private:
    SystemParametersCall call_;
    std::uint32_t saved_ = 0;
};

}  // namespace client
