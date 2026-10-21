#pragma once
// The system's side of CD audio (Windows; elsewhere every device is absent): the command-string
// interface (mciSendStringA), the auxiliary devices, the drives, the CD player's window, and a
// tap on the program window's messages (WM_DEVICECHANGE, MM_MCINOTIFY) for CdMusic.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "client/cd_audio.h"

namespace client {

// mciSendStringA; `window` gives the window that receives "notify" messages.
std::unique_ptr<CdCommandDevice> system_cd_device(std::function<void*()> window);
std::unique_ptr<CdAuxVolume> system_aux_volume();
std::unique_ptr<CdDrives> system_cd_drives();
// The Windows CD player's window (class "SJE_CdPlayerClass", the last one EnumWindows lists), 0
// when there is none; closing it: WM_CLOSE, WM_QUIT, then 500 ms of sleep.
std::uintptr_t find_cd_player_window();
void close_cd_player_window(std::uintptr_t window);
// Calls `fn(message, wparam)` for every message the window procedure of `window` gets (before
// the window's own procedure). One window at a time.
void tap_window_messages(void* window, std::function<void(std::uint32_t, std::uintptr_t)> fn);
// The fstream (in | out) open check of a file: true when it opens for reading and writing.
bool file_opens_read_write(const std::string& path);

}  // namespace client
