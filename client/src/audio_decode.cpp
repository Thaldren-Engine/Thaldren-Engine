// Compressed audio files through the platform's decoder: Media Foundation on Windows
// (MP3 and the other formats it supports); other platforms have none here yet.

#include "client/audio.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstring>
#include <mutex>
#include <string>

namespace client {

bool decode_audio_file(const std::string& path, Sample& out) {
    static std::once_flag started;
    static bool ok = false;
    std::call_once(started, [] { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
    if (!ok) return false;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring wpath(path.begin(), path.end());
    IMFSourceReader* reader = nullptr;
    bool done = false;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader))) {
        IMFMediaType* want = nullptr;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(stream, TRUE);
        if (SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, want))) {
            IMFMediaType* got = nullptr;
            if (SUCCEEDED(reader->GetCurrentMediaType(stream, &got))) {
                UINT32 rate = 0, channels = 0;
                got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
                got->Release();
                out.rate = static_cast<int>(rate);
                out.channels = static_cast<int>(channels);
                out.pcm.clear();
                for (;;) {
                    DWORD flags = 0;
                    IMFSample* sample = nullptr;
                    if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample))) break;
                    if (sample) {
                        IMFMediaBuffer* buf = nullptr;
                        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                            BYTE* data = nullptr;
                            DWORD len = 0;
                            if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                                const std::size_t at = out.pcm.size();
                                out.pcm.resize(at + len / 2);
                                std::memcpy(out.pcm.data() + at, data, (len / 2) * 2);
                                buf->Unlock();
                            }
                            buf->Release();
                        }
                        sample->Release();
                    }
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                }
                done = rate > 0 && channels > 0 && !out.pcm.empty();
            }
        }
        want->Release();
        reader->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return done;
}

}  // namespace client

#else

namespace client {
bool decode_audio_file(const std::string&, Sample&) { return false; }
}  // namespace client

#endif
