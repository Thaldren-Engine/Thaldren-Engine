#include "client/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace client {

namespace {
// The output rate. The original asks for an 11025 Hz, 16-bit stereo primary buffer, which only
// the old platform mixers honoured; today's mix at the device rate, as this one does (22050 Hz,
// every sample resampled to it, nearest frame).
constexpr int kRate = 22050;
}

struct AudioOut::Impl {
    SDL_AudioStream* stream = nullptr;
    std::mutex lock;
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
    std::uint64_t music_pos = 0, music_step = 0;
    bool music_paused = false;
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
                    const std::size_t i = at * static_cast<std::size_t>(v.s->channels);
                    const float a = v.s->pcm[i];
                    const float b = v.s->channels > 1 ? v.s->pcm[i + 1] : a;
                    m.mix[static_cast<std::size_t>(f) * 2] += a * lv;
                    m.mix[static_cast<std::size_t>(f) * 2 + 1] += b * rv;
                    v.pos += v.step;
                }
            }
            if (m.music && !m.music_paused) {
                const Sample& s = *m.music;
                const std::size_t n = s.pcm.size() / static_cast<std::size_t>(std::max(1, s.channels));
                const float mv = m.music_volume / 256.0f;
                for (int f = 0; f < frames; ++f) {
                    const std::size_t at = static_cast<std::size_t>(m.music_pos >> 32);
                    if (at >= n) {
                        m.music.reset();
                        break;
                    }
                    const std::size_t i = at * static_cast<std::size_t>(s.channels);
                    const float a = s.pcm[i];
                    const float b = s.channels > 1 ? s.pcm[i + 1] : a;
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
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kRate};
    m->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::feed, m.get());
    if (!m->stream) return false;
    SDL_ResumeAudioStreamDevice(m->stream);
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
    v->step = (static_cast<std::uint64_t>(std::max(1, c.sample->rate)) << 32) / kRate;
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

void AudioOut::play_music(std::shared_ptr<const Sample> track) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->music = std::move(track);
    impl_->music_pos = 0;
    impl_->music_step = impl_->music ? (static_cast<std::uint64_t>(impl_->music->rate) << 32) / kRate : 0;
}

void AudioOut::stop_music() { play_music(nullptr); }

void AudioOut::pause_music(bool paused) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->music_paused = paused;
}

bool AudioOut::music_playing() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->music != nullptr;
}

double AudioOut::music_position() const {
    if (!impl_) return 0.0;
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->music ? static_cast<double>(impl_->music_pos >> 32) / impl_->music->rate : 0.0;
}

} // namespace client
