// The game's own movie library (smackw32.dll, shipped with the game) against client::SmackerFile:
// frame pictures, palettes and the decompressed audio of each frame, frame by frame.
//
// The library is loaded into this process (no TotalA.exe). Its window, dialog and sound entry
// points are pointed at stand-ins first (no message box, no sound device, no window), so it only
// decodes. Each frame is put into a 640 x 480 buffer kept from frame to frame, the way the
// game's player does it (0x47C3A0: SmackToBuffer at x 0, y (480 - height) / 2, destination
// height = the movie's height, then SmackDoFrame); the engine's frame is shown on its own
// buffer with SmackerFile::show at the same place. Audio: SmackGetTrackData for track 1 of the
// current frame against the engine's unpacked frame audio.
//
// Usage: movie_harness <smackw32.dll> <movie.zrb> [more movies] [--frames N] [--dump PREFIX]
//   --frames N   compare at most N frames of each movie (default: all)
//   --dump P     writes the first differing frame of each movie as P<movie>_orig.raw / _ours.raw
//   --wait MS    the library's frame clock instead: when SmackWait lets each of 60 frames through,
//                the player stalled MS ms before frame 30 (sound off)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "client/smacker.h"

namespace {

using SmackOpenFn = void*(__stdcall*)(const char*, std::uint32_t, std::uint32_t);
using SmackCloseFn = void(__stdcall*)(void*);
using SmackDoFrameFn = std::uint32_t(__stdcall*)(void*);
using SmackNextFrameFn = void(__stdcall*)(void*);
using SmackToBufferFn = void(__stdcall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, void*, std::uint32_t);
using SmackGetTrackDataFn = std::uint32_t(__stdcall*)(void*, void*, std::uint32_t);

int __stdcall fake_message_box(HWND, LPCSTR text, LPCSTR caption, UINT) {
    std::fprintf(stderr, "(library message: %s: %s)\n", caption ? caption : "", text ? text : "");
    return IDOK;
}
MMRESULT __stdcall fake_wave_out_open(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD) { return MMSYSERR_NODRIVER; }
HMODULE __stdcall fake_load_library(LPCSTR name) {
    std::fprintf(stderr, "(library asked for %s: refused)\n", name ? name : "");
    return nullptr;
}
HWND __stdcall fake_create_window(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) { return nullptr; }

// Points the library's imports of `name` (from any DLL) at `fn`.
int patch_import(HMODULE mod, const char* name, void* fn) {
    auto* base = reinterpret_cast<std::uint8_t*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) != 0) continue;
            DWORD old = 0;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            iat->u1.Function = reinterpret_cast<std::uintptr_t>(fn);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            ++patched;
        }
    }
    return patched;
}

std::uint32_t rd(const void* smk, std::uint32_t off) {
    std::uint32_t v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(smk) + off, 4);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc < 3) {
        std::fprintf(stderr, "usage: movie_harness <smackw32.dll> <movie.zrb>... [--frames N] [--dump PREFIX]\n");
        return 2;
    }
    std::vector<std::string> movies;
    int max_frames = -1, stall = 0;
    bool wait_test = false;
    std::string dump;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) max_frames = std::atoi(argv[++i]);
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == "--wait" && i + 1 < argc) wait_test = true, stall = std::atoi(argv[++i]);
        else movies.push_back(a);
    }
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    patch_import(lib, "MessageBoxA", reinterpret_cast<void*>(&fake_message_box));
    patch_import(lib, "waveOutOpen", reinterpret_cast<void*>(&fake_wave_out_open));
    patch_import(lib, "LoadLibraryA", reinterpret_cast<void*>(&fake_load_library));
    patch_import(lib, "CreateWindowExA", reinterpret_cast<void*>(&fake_create_window));
    auto open = reinterpret_cast<SmackOpenFn>(GetProcAddress(lib, "_SmackOpen@12"));
    auto close = reinterpret_cast<SmackCloseFn>(GetProcAddress(lib, "_SmackClose@4"));
    auto do_frame = reinterpret_cast<SmackDoFrameFn>(GetProcAddress(lib, "_SmackDoFrame@4"));
    auto next = reinterpret_cast<SmackNextFrameFn>(GetProcAddress(lib, "_SmackNextFrame@4"));
    auto to_buffer = reinterpret_cast<SmackToBufferFn>(GetProcAddress(lib, "_SmackToBuffer@28"));
    auto track_data = reinterpret_cast<SmackGetTrackDataFn>(GetProcAddress(lib, "_SmackGetTrackData@12"));
    if (!open || !close || !do_frame || !next || !to_buffer || !track_data) {
        std::fprintf(stderr, "missing library entry points\n");
        return 1;
    }
    if (wait_test) {
        // The library's frame clock (SmackWait, sound off as here): when each frame becomes due,
        // with the player stalled for `stall` ms before frame 30 (a window without the focus:
        // 0x47C3A0 neither shows nor advances a frame then).
        auto wait = reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(GetProcAddress(lib, "_SmackWait@4"));
        void* smk = wait ? open(movies.front().c_str(), 0xFE100, 0xFFFFFFFFu) : nullptr;
        if (!smk) {
            std::fprintf(stderr, "cannot open the movie / no SmackWait\n");
            return 1;
        }
        std::vector<std::uint8_t> buf(640 * 480, 0);
        const std::uint32_t lh = rd(smk, 8);
        timeBeginPeriod(1);
        LARGE_INTEGER q0, qf, q;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        double last = 0;
        for (int f = 0; f < 120; ++f) {
            if (f == 30) Sleep(static_cast<DWORD>(stall));
            int polls = 0;
            while (wait(smk)) {
                ++polls;
                Sleep(0);
            }
            QueryPerformanceCounter(&q);
            const double t = static_cast<double>(q.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart);
            to_buffer(smk, 0, (480 - lh) >> 1, 640, lh, buf.data(), 0);
            do_frame(smk);
            std::printf("frame %d due at %.3f ms (+%.3f, %d polls)\n", f, t, t - last, polls);
            last = t;
            next(smk);
        }
        timeEndPeriod(1);
        close(smk);
        return 0;
    }
    int failures = 0;
    for (const std::string& path : movies) {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        client::SmackerFile ours;
        std::string err;
        if (!ours.open(bytes, err)) {
            std::printf("%s: engine: %s\n", path.c_str(), err.c_str());
            ++failures;
            continue;
        }
        // Track 1 (the game opens all seven, 0xFE100: tracks and the extra buffer).
        void* smk = open(path.c_str(), 0xFE100, 0xFFFFFFFFu);
        if (!smk) {
            std::printf("%s: the library cannot open it\n", path.c_str());
            ++failures;
            continue;
        }
        const std::uint32_t lw = rd(smk, 4), lh = rd(smk, 8), lframes = rd(smk, 0xC);
        std::printf("%s: library %ux%u, %u frames; engine %dx%d stored, %d shown, %d frames, %.3f ms, audio %d Hz %d ch %d bit\n", path.c_str(), lw, lh,
                    lframes, ours.width(), ours.height(), ours.shown_height(), ours.frames(), ours.frame_ms(), ours.audio_rate(0), ours.audio_channels(0),
                    ours.audio_bits(0));
        const int frames = max_frames >= 0 ? std::min<int>(max_frames, static_cast<int>(lframes)) : static_cast<int>(lframes);
        std::vector<std::uint8_t> lib_buf(640 * 480, 0), our_buf(640 * 480, 0), lib_audio(1 << 20);
        const int top = (480 - static_cast<int>(lh)) >> 1;
        long pixel_diffs = 0, palette_diffs = 0, audio_bytes = 0, audio_diffs = 0, frames_diff = 0;
        bool dumped = false;
        for (int f = 0; f < frames; ++f) {
            std::fill(lib_audio.begin(), lib_audio.end(), static_cast<std::uint8_t>(0xCD));
            const std::uint32_t got = track_data(smk, lib_audio.data(), 0x2000);
            to_buffer(smk, 0, static_cast<std::uint32_t>(top), 640, lh, lib_buf.data(), 0);
            do_frame(smk);
            if (!ours.next_frame()) {
                std::printf("  frame %d: the engine could not decode it\n", f);
                ++failures;
                break;
            }
            ours.show(our_buf.data(), 640, 480, top);
            long d = 0;
            for (std::size_t k = 0; k < lib_buf.size(); ++k) d += lib_buf[k] != our_buf[k];
            long pd = 0;
            for (std::size_t k = 0; k < 768; ++k) pd += reinterpret_cast<const std::uint8_t*>(smk)[0x6C + k] != ours.palette()[k];
            const std::vector<std::uint8_t>& a = ours.frame_audio(0);
            long ad = 0;
            for (std::size_t k = 0; k < a.size(); ++k) ad += lib_audio[k] != a[k];
            if (f < 3 || d || pd || ad)
                if (f < 3 || frames_diff < 10)
                    std::printf("  frame %d: %ld pixels, %ld palette bytes, audio %zu bytes (library said %u) %ld differ\n", f, d, pd, a.size(), got, ad);
            if (d || pd || ad) ++frames_diff;
            pixel_diffs += d;
            palette_diffs += pd;
            audio_bytes += static_cast<long>(a.size());
            audio_diffs += ad;
            if ((d || pd) && !dump.empty() && !dumped) {
                dumped = true;
                std::string base = path.substr(path.find_last_of("\\/") + 1);
                std::ofstream(dump + base + "_orig.raw", std::ios::binary).write(reinterpret_cast<const char*>(lib_buf.data()), static_cast<std::streamsize>(lib_buf.size()));
                std::ofstream(dump + base + "_ours.raw", std::ios::binary).write(reinterpret_cast<const char*>(our_buf.data()), static_cast<std::streamsize>(our_buf.size()));
            }
            if (f + 1 < frames) next(smk);
        }
        close(smk);
        std::printf("%s: %d frames: %ld frames differ; %ld pixels, %ld palette bytes; audio %ld bytes, %ld differ\n", path.c_str(), frames, frames_diff,
                    pixel_diffs, palette_diffs, audio_bytes, audio_diffs);
        if (frames_diff) ++failures;
    }
    return failures ? 1 : 0;
}
