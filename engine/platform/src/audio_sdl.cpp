#include "giant/platform/audio_output.h"

#include "giant/core/log.h"

#include <SDL3/SDL.h>

#include <vector>

namespace giant::platform {

struct AudioOutput::Impl {
    SDL_AudioStream* stream = nullptr;
    AudioRender render;
    std::vector<float> buffer; // grown on the first pulls, then reused
    AudioOutput* owner = nullptr;

    static void SDLCALL pull(void* user, SDL_AudioStream* stream, int additional, int) {
        Impl& self = *static_cast<Impl*>(user);
        if (additional <= 0) return;
        const u32 frames = static_cast<u32>(additional) / (2 * sizeof(float));
        if (frames == 0) return;
        if (self.buffer.size() < static_cast<usize>(frames) * 2) self.buffer.resize(static_cast<usize>(frames) * 2);
        self.render(self.buffer.data(), frames);
        SDL_PutAudioStreamData(stream, self.buffer.data(), static_cast<int>(frames * 2 * sizeof(float)));
        self.owner->m_frames.fetch_add(frames, std::memory_order_relaxed);
    }
};

AudioOutput::AudioOutput(ConstructKey, std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

std::unique_ptr<AudioOutput> AudioOutput::open(u32 rate, AudioRender render) {
    if (!render) return nullptr;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log_warning("platform", std::string("no audio: ") + SDL_GetError());
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->render = std::move(render);
    impl->buffer.resize(4096 * 2);
    Impl* raw = impl.get();
    auto out = std::make_unique<AudioOutput>(ConstructKey{}, std::move(impl));
    raw->owner = out.get();
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(rate)};
    raw->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::pull, raw);
    if (raw->stream == nullptr) {
        log_warning("platform", std::string("no audio device: ") + SDL_GetError());
        out->m_impl.reset();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    const char* driver = SDL_GetCurrentAudioDriver();
    out->m_device = driver != nullptr ? driver : "audio";
    SDL_ResumeAudioStreamDevice(raw->stream);
    return out;
}

AudioOutput::~AudioOutput() {
    if (m_impl == nullptr) return;
    SDL_DestroyAudioStream(m_impl->stream); // waits for a pull in progress
    m_impl.reset();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

} // namespace giant::platform
