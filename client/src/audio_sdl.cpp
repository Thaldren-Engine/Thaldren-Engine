#include "client/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace client {

namespace {
constexpr int kRate = 22050;
}

struct AudioOut::Impl {
    SDL_AudioStream* stream = nullptr;
    const std::vector<Sample>* samples = nullptr;
    std::mutex lock;
    struct Voice {
        const Sample* s = nullptr;
        std::uint64_t pos = 0;   // 32.32 frame position in the sample
        std::uint64_t step = 0;  // per output frame
        int left = 256, right = 256;
        std::uint64_t started = 0;
    };
    std::vector<Voice> voices;
    std::uint64_t serial = 0;
    int fx_volume = 256, music_volume = 256;
    std::shared_ptr<const Sample> music;
    std::uint64_t music_pos = 0, music_step = 0;
    std::vector<std::int32_t> mix;
    std::vector<std::int16_t> out;

    static void SDLCALL feed(void* user, SDL_AudioStream* stream, int additional, int) {
        Impl& m = *static_cast<Impl*>(user);
        const int frames = additional / 4;
        if (frames <= 0) return;
        m.mix.assign(static_cast<std::size_t>(frames) * 2, 0);
        {
            std::lock_guard<std::mutex> g(m.lock);
            for (Voice& v : m.voices) {
                if (!v.s) continue;
                const std::size_t n = v.s->pcm.size() / static_cast<std::size_t>(std::max(1, v.s->channels));
                const int lv = v.left * m.fx_volume / 256, rv = v.right * m.fx_volume / 256;
                for (int f = 0; f < frames; ++f) {
                    const std::size_t at = static_cast<std::size_t>(v.pos >> 32);
                    if (at >= n) {
                        v.s = nullptr;
                        break;
                    }
                    const std::size_t i = at * static_cast<std::size_t>(v.s->channels);
                    const int a = v.s->pcm[i];
                    const int b = v.s->channels > 1 ? v.s->pcm[i + 1] : a;
                    m.mix[static_cast<std::size_t>(f) * 2] += a * lv / 256;
                    m.mix[static_cast<std::size_t>(f) * 2 + 1] += b * rv / 256;
                    v.pos += v.step;
                }
            }
            if (m.music) {
                const Sample& s = *m.music;
                const std::size_t n = s.pcm.size() / static_cast<std::size_t>(std::max(1, s.channels));
                for (int f = 0; f < frames; ++f) {
                    const std::size_t at = static_cast<std::size_t>(m.music_pos >> 32);
                    if (at >= n) {
                        m.music.reset();
                        break;
                    }
                    const std::size_t i = at * static_cast<std::size_t>(s.channels);
                    const int a = s.pcm[i];
                    const int b = s.channels > 1 ? s.pcm[i + 1] : a;
                    m.mix[static_cast<std::size_t>(f) * 2] += a * m.music_volume / 256;
                    m.mix[static_cast<std::size_t>(f) * 2 + 1] += b * m.music_volume / 256;
                    m.music_pos += m.music_step;
                }
            }
        }
        m.out.resize(m.mix.size());
        for (std::size_t i = 0; i < m.mix.size(); ++i) m.out[i] = static_cast<std::int16_t>(std::clamp(m.mix[i], -32768, 32767));
        SDL_PutAudioStreamData(stream, m.out.data(), frames * 4);
    }
};

bool AudioOut::open(const std::vector<Sample>* samples, int max_voices) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    auto m = std::make_unique<Impl>();
    m->samples = samples;
    m->voices.resize(static_cast<std::size_t>(std::max(1, max_voices)));
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

void AudioOut::set_samples(const std::vector<Sample>* samples) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    // Voices of the old table stop (their samples may go away).
    for (auto& v : impl_->voices) v.s = nullptr;
    impl_->samples = samples;
}

void AudioOut::play(int index, int pan) {
    if (!impl_ || !impl_->samples || index < 0 || static_cast<std::size_t>(index) >= impl_->samples->size()) return;
    play_sample(&(*impl_->samples)[static_cast<std::size_t>(index)], pan, 256);
}

void AudioOut::play_sample(const Sample* s, int pan, int volume) {
    if (!impl_ || !s || s->pcm.empty()) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    Impl::Voice* use = nullptr;
    for (auto& v : impl_->voices)
        if (!v.s) {
            use = &v;
            break;
        }
    if (!use) use = &*std::min_element(impl_->voices.begin(), impl_->voices.end(),
                                       [](const Impl::Voice& a, const Impl::Voice& b) { return a.started < b.started; });
    use->s = s;
    use->pos = 0;
    use->step = (static_cast<std::uint64_t>(s->rate) << 32) / kRate;
    pan = std::clamp(pan, -100, 100);
    volume = std::clamp(volume, 0, 256);
    use->left = (pan > 0 ? 256 * (100 - pan) / 100 : 256) * volume / 256;
    use->right = (pan < 0 ? 256 * (100 + pan) / 100 : 256) * volume / 256;
    use->started = ++impl_->serial;
}

void AudioOut::set_fx_volume(int v) {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->lock);
    impl_->fx_volume = std::clamp(v, 0, 256);
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

bool decode_wav(const std::vector<std::uint8_t>& file, Sample& out) {
    SDL_IOStream* io = SDL_IOFromConstMem(file.data(), file.size());
    if (!io) return false;
    SDL_AudioSpec spec{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) return false;
    // Convert whatever it holds to signed 16-bit at its own rate and channel count.
    SDL_AudioSpec to{SDL_AUDIO_S16, spec.channels, spec.freq};
    Uint8* conv = nullptr;
    int conv_len = 0;
    const bool ok = SDL_ConvertAudioSamples(&spec, buf, static_cast<int>(len), &to, &conv, &conv_len);
    SDL_free(buf);
    if (!ok) return false;
    out.rate = spec.freq;
    out.channels = spec.channels;
    out.pcm.resize(static_cast<std::size_t>(conv_len) / 2);
    std::memcpy(out.pcm.data(), conv, out.pcm.size() * 2);
    SDL_free(conv);
    return true;
}

} // namespace client
