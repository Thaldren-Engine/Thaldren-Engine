#include "client/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace client {

namespace {
// The output rate. The original asks for an 11025 Hz, 16-bit stereo primary buffer, which only
// the old platform mixers honoured. Today's systems ignore it: every sound buffer is converted
// to the output device's own rate, interpolating between neighbouring samples, and mixed
// there. This mixer does the same: it runs at the device's rate (48000 Hz when the device
// doesn't say) and reads each sound with linear interpolation.
constexpr int kFallbackRate = 48000;

// The frame at a 32.32 position, linearly interpolated with the next one (the last frame
// holds; a looping sound wraps to its start).
inline void read_frame(const Sample& s, std::size_t frames, std::uint64_t pos, bool loop, float& a, float& b) {
    const std::size_t at = static_cast<std::size_t>(pos >> 32);
    std::size_t next = at + 1;
    if (next >= frames) next = loop ? 0 : at;
    const float t = static_cast<float>(pos & 0xFFFFFFFFu) * (1.0f / 4294967296.0f);
    const std::size_t ch = static_cast<std::size_t>(s.channels);
    const float a0 = s.pcm[at * ch], a1 = s.pcm[next * ch];
    a = a0 + (a1 - a0) * t;
    if (ch > 1) {
        const float b0 = s.pcm[at * ch + 1], b1 = s.pcm[next * ch + 1];
        b = b0 + (b1 - b0) * t;
    } else {
        b = a;
    }
}
}

struct AudioOut::Impl {
    SDL_AudioStream* stream = nullptr;
    int rate = kFallbackRate;  // the output device's own rate
    mutable std::mutex lock;
    struct Voice {
        int id = 0;
        const Sample* s = nullptr;
        std::uint64_t pos = 0;   // 32.32 frame position in the sample
        std::uint64_t step = 0;  // per output frame
        float left = 1.0f, right = 1.0f;
        bool loop = false;
    };
    std::vector<Voice> voices;
    float fx_volume = 1.0f;
    int music_volume = 256;
    std::shared_ptr<const Sample> music;
    std::uint64_t music_pos = 0, music_step = 0;  // 32.32 frames
    bool music_on = false, music_repeat = false;
    std::vector<float> mix;
    std::vector<std::int16_t> out;

    static void SDLCALL feed(void* user, SDL_AudioStream* stream, int additional, int) {
        Impl& m = *static_cast<Impl*>(user);
        const int frames = additional / 4;
        if (frames <= 0) return;
        m.mix.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
        {
            std::lock_guard<std::mutex> g(m.lock);
            for (Voice& v : m.voices) {
                if (!v.s) continue;
                const std::size_t n = v.s->pcm.size() / static_cast<std::size_t>(std::max(1, v.s->channels));
                const float lv = v.left * m.fx_volume, rv = v.right * m.fx_volume;
                for (int f = 0; f < frames; ++f) {
                    std::size_t at = static_cast<std::size_t>(v.pos >> 32);
                    if (at >= n) {
                        if (!v.loop || n == 0) {
                            v.s = nullptr;
                            break;
                        }
                        v.pos %= static_cast<std::uint64_t>(n) << 32;
                        at = static_cast<std::size_t>(v.pos >> 32);
                    }
                    float a = 0, b = 0;
                    read_frame(*v.s, n, v.pos, v.loop, a, b);
                    m.mix[static_cast<std::size_t>(f) * 2] += a * lv;
                    m.mix[static_cast<std::size_t>(f) * 2 + 1] += b * rv;
                    v.pos += v.step;
                }
            }
            if (m.music && m.music_on) {
                const Sample& s = *m.music;
                const std::size_t n = s.pcm.size() / static_cast<std::size_t>(std::max(1, s.channels));
                const float mv = m.music_volume / 256.0f * m.fx_volume;
                for (int f = 0; f < frames; ++f) {
                    std::size_t at = static_cast<std::size_t>(m.music_pos >> 32);
                    if (at >= n) {
                        if (!m.music_repeat || n == 0) {
                            m.music_on = false;  // run out (the position stays at the end)
                            m.music_pos = static_cast<std::uint64_t>(n) << 32;
                            break;
                        }
                        m.music_pos %= static_cast<std::uint64_t>(n) << 32;
                        at = static_cast<std::size_t>(m.music_pos >> 32);
                    }
                    float a = 0, b = 0;
                    read_frame(s, n, m.music_pos, m.music_repeat, a, b);
                    m.mix[static_cast<std::size_t>(f) * 2] += a * mv;
                    m.mix[static_cast<std::size_t>(f) * 2 + 1] += b * mv;
                    m.music_pos += m.music_step;
                }
            }
        }
        // The platform mixer clips the sum.
        m.out.resize(m.mix.size());
        for (std::size_t i = 0; i < m.mix.size(); ++i)
            m.out[i] = static_cast<std::int16_t>(std::clamp(static_cast<int>(m.mix[i]), -32768, 32767));
        SDL_PutAudioStreamData(stream, m.out.data(), frames * 4);
    }
};

bool AudioOut::open() {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    auto m = std::make_unique<Impl>();
    SDL_AudioSpec device{};
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, nullptr) && device.freq > 0) m->rate = device.freq;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, m->rate};
    m->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::feed, m.get());
    if (!m->stream) return false;
    SDL_ResumeAudioStreamDevice(m->stream);
    std::printf("sound output: %d Hz\n", m->rate);
    impl_ = std::move(m);
    return true;
}

AudioOut::AudioOut() = default;

AudioOut::~AudioOut() {
    if (impl_ && impl_->stream) SDL_DestroyAudioStream(impl_->stream);
}

void AudioOut::run(const SoundCommand& c) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    Impl::Voice* v = nullptr;
    for (auto& x : impl_->voices)
        if (x.id == c.voice) v = &x;
    if (c.kind == SoundCommand::Kind::Stop) {
        if (v) v->s = nullptr;
        return;
    }
    if (!c.sample || c.sample->pcm.empty()) return;
    if (!v) {
        for (auto& x : impl_->voices)
            if (!x.s) {
                v = &x;
                break;
            }
        if (!v) {
            impl_->voices.emplace_back();
            v = &impl_->voices.back();
        }
    }
    v->id = c.voice;
    v->s = c.sample;
    v->pos = 0;
    v->step = (static_cast<std::uint64_t>(std::max(1, c.sample->rate)) << 32) / static_cast<std::uint64_t>(impl_->rate);
    v->left = c.gain_left;
    v->right = c.gain_right;
    v->loop = c.loop;
}

void AudioOut::forget(const std::vector<Sample>& samples) {
    if (!impl_ || samples.empty()) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    for (auto& v : impl_->voices)
        if (v.s >= samples.data() && v.s < samples.data() + samples.size()) v.s = nullptr;
}

void AudioOut::set_fx_volume(float v) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->fx_volume = std::clamp(v, 0.0f, 1.0f);
}

void AudioOut::set_music_volume(int v) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->music_volume = std::clamp(v, 0, 256);
}

void AudioOut::music_start(std::shared_ptr<const Sample> track, std::uint64_t frame, bool repeat) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->music = std::move(track);
    impl_->music_repeat = repeat;
    impl_->music_on = impl_->music != nullptr && !impl_->music->pcm.empty();
    if (!impl_->music_on) return;
    const std::uint64_t n = impl_->music->pcm.size() / static_cast<std::size_t>(std::max(1, impl_->music->channels));
    if (frame > n) frame = n;  // (past the end: at the end)
    impl_->music_pos = frame << 32;
    impl_->music_step = (static_cast<std::uint64_t>(std::max(1, impl_->music->rate)) << 32) / static_cast<std::uint64_t>(impl_->rate);
}

void AudioOut::music_halt() {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->music_on = false;
}

void AudioOut::music_set_repeat(bool repeat) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->music_repeat = repeat;
}

bool AudioOut::music_playing() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->music != nullptr && impl_->music_on;
}

std::uint64_t AudioOut::music_frame() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->music ? impl_->music_pos >> 32 : 0;
}

} // namespace client
