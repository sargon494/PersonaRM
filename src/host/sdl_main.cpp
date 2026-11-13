// personarm_sdl: SDL3 window/input/audio host around the headless PS1 core (Engine public API only).
// Video goes through SDL's own 2D renderer: one streaming texture blitted to a 4:3 rect is all a first
// version needs, and it avoids a GL loader dependency (glad2) until shaders/upscaling arrive.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "host/host_logic.h"

using namespace personarm;

static void Usage() {
    std::fprintf(stderr, "Usage: personarm_sdl <disc.cue> [--frames N] [--screenshot out.bmp]\n"
                         "  --frames N         quit after N emulated frames\n"
                         "  --screenshot file  save the last presented frame as BMP on quit\n");
}

int main(int argc, char** argv) {
    std::string disc, screenshot;
    uint64_t max_frames = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (argv[i][0] != '-' && disc.empty()) disc = argv[i];
        else { Usage(); return EXIT_FAILURE; }
    }
    if (disc.empty()) { Usage(); return EXIT_FAILURE; }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("PersonaRM", 960, 720, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    // XA audio: 44.1 kHz stereo s16. No stream = run silent.
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else std::fprintf(stderr, "no audio: %s\n", SDL_GetError());

    Engine& engine = Engine::Instance();
    EngineBootConfig config;
    config.enable_bios_hle = true;
    engine.Initialize(config);
    if (!engine.BootFromDisc(disc)) {
        std::fprintf(stderr, "cannot boot %s\n", disc.c_str());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    // Resolve the name tables to SDL codes once.
    struct KeyBit { SDL_Scancode code; uint16_t bit; };
    std::vector<KeyBit> keys;
    for (const auto& m : host::KeyboardMap())
        if (SDL_Scancode sc = SDL_GetScancodeFromName(m.name); sc != SDL_SCANCODE_UNKNOWN) keys.push_back({sc, m.bit});
    struct PadBit { SDL_GamepadButton button; SDL_GamepadAxis axis; uint16_t bit; };
    std::vector<PadBit> pad_bits;
    for (const auto& m : host::GamepadMap())
        pad_bits.push_back({SDL_GetGamepadButtonFromString(m.name), SDL_GetGamepadAxisFromString(m.name), m.bit});
    std::vector<SDL_Gamepad*> gamepads;

    SDL_Texture* texture = nullptr;
    int tex_w = 0, tex_h = 0;
    std::vector<uint32_t> rgba;
    uint64_t xa_samples = 0, lit_frames = 0;
    const uint64_t frame_ns = SDL_NS_PER_SECOND / 60;  // engine frame = 564480 cycles = 1/60 s
    uint64_t next = SDL_GetTicksNS();
    bool quit = false;

    while (!quit && engine.GetState() == EngineState::Running && (!max_frames || engine.GetFrameCount() < max_frames)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) quit = true;
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
            else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad* g = SDL_OpenGamepad(ev.gdevice.which)) gamepads.push_back(g);
            } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (size_t i = 0; i < gamepads.size(); ++i)
                    if (SDL_GetGamepadID(gamepads[i]) == ev.gdevice.which) {
                        SDL_CloseGamepad(gamepads[i]);
                        gamepads.erase(gamepads.begin() + i);
                        break;
                    }
            }
        }

        // Pad: the live mask is fed as a one-frame pad script (the only public path that carries all
        // 16 buttons; InputManager's VirtualActions have no Start/Select/L2/R2).
        uint16_t mask = 0;
        const bool* ks = SDL_GetKeyboardState(nullptr);
        for (const KeyBit& k : keys)
            if (ks[k.code]) mask |= k.bit;
        for (SDL_Gamepad* g : gamepads)
            for (const PadBit& p : pad_bits) {
                if (p.button != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(g, p.button)) mask |= p.bit;
                if (p.axis != SDL_GAMEPAD_AXIS_INVALID && SDL_GetGamepadAxis(g, p.axis) > 16384) mask |= p.bit;
            }
        engine.SetPadScript({Engine::PadPress{mask, engine.GetFrameCount(), 1}});

        engine.RunFrame();

        const std::vector<int16_t> xa = engine.TakeXaAudio();
        xa_samples += xa.size();
        if (audio && !xa.empty()) {
            // Keep latency bounded if the emulator ran ahead of the device clock (> 0.25 s queued).
            if (SDL_GetAudioStreamQueued(audio) > 44100 * 4 / 4) SDL_ClearAudioStream(audio);
            SDL_PutAudioStreamData(audio, xa.data(), int(xa.size() * sizeof(int16_t)));
        }

        Ps1Gpu& gpu = engine.GetGpu();
        const GpuDisplayArea& d = gpu.GetDisplayArea();
        const uint32_t w = d.width ? d.width : 320, h = d.height ? d.height : 240;
        if ((gpu.ReadGPUSTAT() >> 23) & 1) rgba.assign(size_t(w) * h, 0xFF000000u);  // display disabled
        else host::DisplayToRgba(gpu.GetVram(), d.x, d.y, w, h, d.is_24bit, rgba);
        for (uint32_t p : rgba)
            if (p & 0xFFFFFFu) { ++lit_frames; break; }
        if (!texture || tex_w != int(w) || tex_h != int(h)) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, int(w), int(h));
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            tex_w = int(w), tex_h = int(h);
        }
        SDL_UpdateTexture(texture, nullptr, rgba.data(), int(w * 4));

        int win_w = 0, win_h = 0;
        SDL_GetRenderOutputSize(renderer, &win_w, &win_h);
        const host::Rect r = host::Letterbox4x3(win_w, win_h);
        const SDL_FRect dst{float(r.x), float(r.y), float(r.w), float(r.h)};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, &dst);
        const bool last = !screenshot.empty() && (quit || (max_frames && engine.GetFrameCount() >= max_frames));
        if (last) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                if (!SDL_SaveBMP(shot, screenshot.c_str())) std::fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
                SDL_DestroySurface(shot);
            }
        }
        SDL_RenderPresent(renderer);

        next += frame_ns;
        const uint64_t now = SDL_GetTicksNS();
        if (now < next) SDL_DelayPrecise(next - now);
        else if (now - next > 6 * frame_ns) next = now;  // too far behind: drop the debt instead of racing
    }

    std::printf("[SDL] frames=%llu non-black=%llu xa_samples=%llu\n", (unsigned long long)engine.GetFrameCount(),
                (unsigned long long)lit_frames, (unsigned long long)xa_samples);
    engine.Shutdown();
    for (SDL_Gamepad* g : gamepads) SDL_CloseGamepad(g);
    if (texture) SDL_DestroyTexture(texture);
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
}
