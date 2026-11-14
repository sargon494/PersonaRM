// personarm_sdl: SDL3 window/input/audio host around the headless PS1 core (Engine public API only).
// Video goes through SDL's own 2D renderer: one streaming texture blitted to a 4:3 rect is all a first
// version needs, and it avoids a GL loader dependency (glad2) until shaders/upscaling arrive.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "core/save_manager.h"
#include "engine/engine.h"
#include "host/host_logic.h"
#include "loader/png_codec.h"
#include "recomp/runtime/generated_modules.h"
#include "rendering/frame_interp.h"
#include "rendering/vram_texture_hook.h"

using namespace personarm;

static void Usage() {
    std::fprintf(stderr, "Usage: personarm_sdl <disc.cue> [options]\n"
                         "  --frames N           quit after N emulated frames\n"
                         "  --screenshot file    save the last presented frame as BMP on quit\n"
                         "  --input-script file  scripted pad presses (<frame> <button> [<hold>] per line)\n"
                         "  --interp HZ          present HZ pictures/s (multiple of 60, e.g. 120), synthesizing the\n"
                         "                       ones between the game's frames (display only, one game frame late)\n"
                         "  --dump-dir D --dump-from F --dump-to G   every presented picture of frames F..G as PNG\n"
                         "  --texture-dump D     write every VRAM texture the game samples as <HASH>_<bits>bit_<WxH>.png into D\n"
                         "  --mods D             HD texture mods: D/<mod>/textures/<HASH>.png (+ manifest.json priority/serial)\n"
                         "  --stats              per second: game flips, distinct pictures, interpolable pairs\n"
                         "  --memcard1 F, --memcard2 F   memory card in slot 1/2: raw 128 KB .mcd, created and\n"
                         "                       formatted if missing, rewritten atomically when the game saves\n"
                         "  --recomp             run the recompiled code linked into this build (local\n"
                         "                       -DPRM_RECOMP_GENERATED_DIR builds)   --recomp-quantum N (default 1)\n"
                         "  --unthrottled        no 60 Hz pacing    --hidden  no visible window\n");
}

int main(int argc, char** argv) {
    host::HostOptions opt;
    if (!host::ParseHostArgs(argc, argv, opt)) { Usage(); return EXIT_FAILURE; }
    const std::string &disc = opt.disc, &screenshot = opt.screenshot, &input_script = opt.input_script,
                      &dump_dir = opt.dump_dir;
    const uint64_t max_frames = opt.max_frames, dump_from = opt.dump_from, dump_to = opt.dump_to;
    const int interp_hz = opt.interp_hz;
    const bool stats = opt.stats, unthrottled = opt.unthrottled, hidden = opt.hidden;
    if (opt.recomp && recomp::GeneratedModules().empty()) {
        std::fprintf(stderr, "--recomp: this build has no recompiled code (configure with "
                             "-DPRM_RECOMP_GENERATED_DIR=<dir outside the repo>)\n");
        return EXIT_FAILURE;
    }
    std::array<MemoryCard, 2> cards;
    for (int slot = 0; slot < 2; ++slot) {
        std::string err;
        if (!opt.memcard[slot].empty() && !cards[slot].Open(opt.memcard[slot], &err)) {
            std::fprintf(stderr, "--memcard%d: %s\n", slot + 1, err.c_str());
            return EXIT_FAILURE;
        }
    }
    std::vector<Engine::PadPress> presses;
    if (!input_script.empty()) {
        std::ifstream in(input_script);
        size_t bad = 0;
        if (!in || !Engine::ParsePadScript(in, presses, bad)) {
            std::fprintf(stderr, "cannot read %s (line %zu)\n", input_script.c_str(), bad);
            return EXIT_FAILURE;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("PersonaRM", 960, 720, SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0),
                                     &window, &renderer)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    // SPU output: 44.1 kHz stereo s16. No stream = run silent.
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else std::fprintf(stderr, "no audio: %s\n", SDL_GetError());

    Engine& engine = Engine::Instance();
    EngineBootConfig config;
    config.enable_bios_hle = true;
    engine.Initialize(config);
    if (opt.recomp) engine.SetRecompModules(recomp::GeneratedModules(), opt.recomp_quantum);
    if (!engine.BootFromDisc(disc)) {
        std::fprintf(stderr, "cannot boot %s\n", disc.c_str());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    // Texture dump / HD replacement: off unless asked; the GPU then runs its untouched path.
    VramTextureHook texture_hook;
    if (!opt.texture_dump.empty() || !opt.mods.empty()) {
        if (!opt.texture_dump.empty()) texture_hook.SetDumpDir(opt.texture_dump);
        if (!opt.mods.empty())
            std::printf("[textures] %zu replacement textures indexed from %s\n", texture_hook.LoadMods(opt.mods),
                        opt.mods.c_str());
        engine.GetGpu().SetTextureHook(&texture_hook);
    }
    engine.SetPadScript(presses);
    for (int slot = 0; slot < 2; ++slot)
        if (!opt.memcard[slot].empty()) engine.SetMemoryCard(slot, &cards[slot]);

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
    std::vector<int16_t> pcm;
    uint64_t audio_samples = 0, lit_frames = 0;
    const uint64_t frame_ns = SDL_NS_PER_SECOND / 60;  // engine frame = 564480 cycles = 1/60 s
    uint64_t next = SDL_GetTicksNS();
    bool quit = false;
    // --interp: the GPU logs its packets (display-only capture, emulation unchanged) for the interpolator.
    interp::FrameInterpolator interp;
    std::vector<uint32_t> cmd_log;
    if (interp_hz) engine.GetGpu().SetCommandLog(&cmd_log);
    const int sub = interp_hz ? interp_hz / 60 : 1;  // presents per emulated frame
    uint64_t interp_ns = 0, interp_frames = 0, last_hash = 0;
    uint32_t flips = 0, changes = 0, pairs = 0, pairs_ok = 0, last_start = ~0u;

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

        uint16_t mask = 0;  // all 16 digital pad buttons
        const bool* ks = SDL_GetKeyboardState(nullptr);
        for (const KeyBit& k : keys)
            if (ks[k.code]) mask |= k.bit;
        for (SDL_Gamepad* g : gamepads)
            for (const PadBit& p : pad_bits) {
                if (p.button != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(g, p.button)) mask |= p.bit;
                if (p.axis != SDL_GAMEPAD_AXIS_INVALID && SDL_GetGamepadAxis(g, p.axis) > 16384) mask |= p.bit;
            }
        engine.SetPadButtons(mask);

        engine.RunFrame();

        engine.TakeAudio(pcm);
        audio_samples += pcm.size();
        if (audio && !pcm.empty()) {
            // Keep latency bounded if the emulator ran ahead of the device clock (> 0.25 s queued).
            if (SDL_GetAudioStreamQueued(audio) > 44100 * 4 / 4) SDL_ClearAudioStream(audio);
            SDL_PutAudioStreamData(audio, pcm.data(), int(pcm.size() * sizeof(int16_t)));
        }

        Ps1Gpu& gpu = engine.GetGpu();
        const uint64_t frame = engine.GetFrameCount();
        if (interp_hz && interp.Consume(cmd_log, gpu, frame) && interp.Active(frame)) {
            ++pairs;
            pairs_ok += interp.PairOk();
        }
        if (stats) {  // flips = display start changes; pictures = emulated frames whose display area changed
            const GpuDisplayArea& d = gpu.GetDisplayArea();
            host::DisplayToRgba(gpu.GetVram(), d.x, d.y, d.width ? d.width : 320, d.height ? d.height : 240,
                                d.is_24bit, rgba);
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t p : rgba) hash = (hash ^ p) * 1099511628211ull;
            changes += hash != last_hash;
            last_hash = hash;
            const uint32_t start = d.x | uint32_t(d.y) << 16;
            flips += start != last_start;
            last_start = start;
            if (frame % 60 == 0) {
                std::printf("[stats] f=%llu flips/s=%u pictures/s=%u interp_pairs=%u/%u %ux%u%s\n",
                            (unsigned long long)frame, flips, changes, pairs_ok, pairs, d.width, d.height,
                            d.is_24bit ? " 24bpp" : "");
                std::fflush(stdout);
                flips = changes = pairs_ok = pairs = 0;
            }
        }

        for (int s = 0; s < sub; ++s) {
            const std::vector<uint16_t>* vram = &gpu.GetVram();
            GpuDisplayArea d = gpu.GetDisplayArea();
            bool off = (gpu.ReadGPUSTAT() >> 23) & 1;
            double t = -1;
            if (interp_hz && interp.Active(frame)) {
                t = interp.Fraction(double(frame) + double(s) / sub);
                const uint64_t t0 = SDL_GetTicksNS();
                interp.Render(t, vram, d);
                if (t > 0 && t < 1 && interp.PairOk()) interp_ns += SDL_GetTicksNS() - t0, ++interp_frames;
                off = false;
            }
            const uint32_t w = d.width ? d.width : 320, h = d.height ? d.height : 240;
            if (off) rgba.assign(size_t(w) * h, 0xFF000000u);  // display disabled
            else host::DisplayToRgba(*vram, d.x, d.y, w, h, d.is_24bit, rgba);
            for (uint32_t p : rgba)
                if (p & 0xFFFFFFu) { ++lit_frames; break; }
            if (!dump_dir.empty() && frame >= dump_from && frame <= dump_to) {  // t999 = live, not interpolated
                char name[64];
                std::snprintf(name, sizeof name, "/f%06llu_%d_t%03d.png", (unsigned long long)frame, s,
                              t < 0 ? 999 : int(t * 100 + 0.5));
                PngCodec::EncodeToFile(dump_dir + name, w, h, reinterpret_cast<const uint8_t*>(rgba.data()));
            }
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
            const bool last = s == sub - 1 && !screenshot.empty() && (quit || (max_frames && frame >= max_frames));
            if (last) {
                if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                    if (!SDL_SaveBMP(shot, screenshot.c_str())) std::fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
                    SDL_DestroySurface(shot);
                }
            }
            SDL_RenderPresent(renderer);

            if (unthrottled) continue;
            next += frame_ns / sub;
            const uint64_t now = SDL_GetTicksNS();
            if (now < next) SDL_DelayPrecise(next - now);
            else if (now - next > 6 * frame_ns) next = now;  // too far behind: drop the debt instead of racing
        }
    }

    if (interp_frames)
        std::printf("[interp] %llu synthesized pictures, %.3f ms each\n", (unsigned long long)interp_frames,
                    double(interp_ns) / 1e6 / double(interp_frames));
    std::printf("[SDL] frames=%llu non-black=%llu audio_samples=%llu\n", (unsigned long long)engine.GetFrameCount(),
                (unsigned long long)lit_frames, (unsigned long long)audio_samples);
    engine.GetGpu().SetCommandLog(nullptr);
    if (!opt.texture_dump.empty() || !opt.mods.empty()) {
        std::printf("[textures] uploads=%zu dumped=%zu replaced=%zu\n", texture_hook.UploadCount(),
                    texture_hook.DumpedCount(), texture_hook.ReplacedUploadCount());
        engine.GetGpu().SetTextureHook(nullptr);
    }
    if (!engine.FlushMemoryCards()) std::fprintf(stderr, "a memory card could not be written\n");
    engine.SetMemoryCard(0, nullptr);
    engine.SetMemoryCard(1, nullptr);
    engine.Shutdown();
    for (SDL_Gamepad* g : gamepads) SDL_CloseGamepad(g);
    if (texture) SDL_DestroyTexture(texture);
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
}
