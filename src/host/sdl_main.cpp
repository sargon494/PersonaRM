// personarm_sdl: SDL3 window/input/audio host around the headless PS1 core (Engine public API only).
// Video goes through SDL's own 2D renderer: one streaming texture blitted to a 4:3 rect is all a first
// version needs, and it avoids a GL loader dependency (glad2) until shaders/upscaling arrive.

#include <cmath>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/save_manager.h"
#include "engine/engine.h"
#include "engine/file_mods.h"
#include "engine/gameplay_patches.h"
#include "host/host_logic.h"
#include "host/settings.h"
#include "loader/png_codec.h"
#include "recomp/runtime/generated_modules.h"
#include "recomp/runtime/native_cache.h"
#include "rendering/frame_interp.h"
#include "rendering/gpu_renderer.h"
#include "rendering/vram_texture_hook.h"

using namespace personarm;

static void Usage() {
    std::fprintf(stderr, "Usage: personarm_sdl <disc.cue> [options]\n"
                         "  --frames N           quit after N emulated frames\n"
                         "  --screenshot file    save the last presented frame as BMP on quit\n"
                         "  --input-script file  scripted pad presses (<frame> <button> [<hold>] per line)\n"
                         "  --load-state N       load save-state slot N (0-9) right after boot, like pressing F8\n"
                         "  --record-input file  write the pad presses you play as an --input-script (flushed every 600\n"
                         "                       frames and on quit; also records the scripted presses of --input-script)\n"
                         "  --interp HZ          present HZ pictures/s (= video.fps_target): 90 or a multiple of 60\n"
                         "                       (120) synthesize the pictures between the game's frames (display only,\n"
                         "                       one game frame late; 90 = 3 presents per 2 game frames); 30 presents\n"
                         "                       every other frame (game and audio stay at 60 Hz); 0 = 60\n"
                         "  --resolution-scale N internal resolution N x 320x240 (1-4, default 1 = native; gpu 1-9\n"
                         "                       or auto); display only, the game still sees the native VRAM\n"
                         "  --renderer R         software (default) or gpu (video.renderer): draw the scaled picture on the host GPU\n"
                         "                       (SDL_gpu, D3D12; docs/GPU_RENDERER.md); then --resolution-scale 1-9 or auto\n"
                         "  --widescreen A       off (default), 16:9, 21:9 or W:H: wider 3D view, shown at that aspect\n"
                         "                       (2D-only and FMV frames stay 4:3; docs/WIDESCREEN.md)\n"
                         "  --dump-dir D --dump-from F --dump-to G   every presented picture of frames F..G as PNG\n"
                         "  --texture-dump D     write every VRAM texture the game samples as <HASH>_<bits>bit_<WxH>.png into D\n"
                         "  --mods D             mods: D/<mod>/textures/<HASH>.png, D/<mod>/files/Fnnnn.BIN (+ manifest.json priority/serial)\n"
                         "  --text-speed S       dialogue speed: normal (game default), fast or instant; also set by a mod\n"
                         "                       manifest \"gameplay\": {\"text_speed\": ...} (this flag wins). Disc 1 (JP, US) only\n"
                         "  --encounters E       random encounters: normal (default), reduced (about half) or none\n"
                         "                       (scripted and boss battles stay). Disc 1 (JP, US) only\n"
                         "  --skip-fmv           Start ends any movie (the game's own skip; without this flag only\n"
                         "                       some movies can be skipped). Disc 1 (JP, US) only\n"
                         "  --battle-speed N     1 (default), 2 or 3 emulated frames per 1/60 s while in battle (like\n"
                         "                       fast-forward: one picture in N, battle sound dropped). Disc 1 (JP, US) only\n"
                         "  --stats             per second: game flips, distinct pictures, interpolable pairs\n"
                         "  --memcard1 F, --memcard2 F   memory card in slot 1/2: raw 128 KB .mcd, created and\n"
                         "                       formatted if missing, rewritten atomically when the game saves\n"
                         "  --recomp, --no-recomp  native (recompiled) code prepared for this disc by personarm_native\n"
                         "                       (game.recomp, default on; interpreter until it exists)   --recomp-quantum N\n"
                         "  --unthrottled        no 60 Hz pacing    --hidden  no visible window\n"
                         "  --settings F         settings file (default: per-user config dir; docs/SETTINGS.md);\n"
                         "                       command line options override it\n"
                         "  --save-settings      write the effective settings (file + command line) back to the file\n"
                         "  --window-mode M      windowed, fullscreen or borderless (Alt+Enter toggles)\n"
                         "  --window-scale N     windowed size = 320x240 * N (1..8)\n"
                         "  --volume N, --mute   master volume 0..100 / silence\n"
                         "Keys: F5 save state, F8 load state, F6/F7 previous/next slot (0-9; files\n"
                         "  <save_dir>/<serial>.state<N>), Tab or gamepad right-stick click held = fast-forward\n"
                         "  (up to video.fast_forward_max x, audio muted), Alt+Enter fullscreen, Esc quit\n");
}

int main(int argc, char** argv) {
    host::HostOptions opt;
    if (!host::ParseHostArgs(argc, argv, opt)) { Usage(); return EXIT_FAILURE; }
    const std::string &disc = opt.disc, &screenshot = opt.screenshot, &input_script = opt.input_script,
                      &dump_dir = opt.dump_dir;
    const uint64_t max_frames = opt.max_frames, dump_from = opt.dump_from, dump_to = opt.dump_to;
    const bool stats = opt.stats, unthrottled = opt.unthrottled, hidden = opt.hidden;
    std::string settings_path = opt.settings_path;
    if (settings_path.empty()) {
        if (const char* pref = SDL_GetPrefPath("PersonaRM", "PersonaRM")) settings_path = std::string(pref) + "settings.json";
    }
    std::vector<std::string> warnings;
    const host::Settings file = settings_path.empty() ? host::DefaultSettings() : host::LoadSettings(settings_path, &warnings);
    for (const std::string& w : warnings) std::fprintf(stderr, "[settings] %s\n", w.c_str());
    const host::Settings cfg = host::ApplyCli(file, opt, "");  // everything but the per-game part
    const int interp_hz = host::InterpHz(cfg.video);
    int scale = cfg.video.resolution_scale;
    bool use_gpu = cfg.video.renderer == "gpu";
    if (interp_hz && scale > 1 && !use_gpu) {  // shortcut: the software interpolator replays at 1x only (the gpu one at s x)
        std::fprintf(stderr, "[settings] video.resolution_scale %d is ignored while fps_target > 60 (software renderer)\n", scale);
        scale = 1;
    }
    if (!use_gpu && (scale < 1 || scale > 4)) {
        std::fprintf(stderr, "[settings] video.resolution_scale %d needs the gpu renderer (software: 1-4)\n", scale);
        scale = std::clamp(scale, 1, 4);
    }
    const int present_hz = host::PresentHz(cfg.video);  // 30, 60, 90, 120...
    std::array<MemoryCard, 2> cards;
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
    if (use_gpu && scale == 0) {  // auto: enough lines for the display (240 x 9 = 2160, 4K)
        const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        scale = std::clamp(dm ? (dm->h + 239) / 240 : 4, 1, 9);
    }
    const int win_scale = std::max(cfg.video.window_scale, std::min(scale, 4));
    int win_aw = 4, win_ah = 3;  // the window opens at the widescreen aspect when one is set
    host::ParseWidescreen(cfg.video.widescreen, win_aw, win_ah);
    const int win_h0 = 240 * win_scale;
    const SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0);
    if (use_gpu && (window = SDL_CreateWindow("PersonaRM", win_h0 * win_aw / win_ah, win_h0, win_flags))) {
        // The SDL GPU renderer: GpuRenderer draws on its device and the picture is presented without readback.
        renderer = SDL_CreateGPURenderer(nullptr, window);
        if (!renderer) renderer = SDL_CreateRenderer(window, nullptr);
    }
    if (!window || !renderer) {
        if (renderer) SDL_DestroyRenderer(renderer), renderer = nullptr;
        if (window) SDL_DestroyWindow(window), window = nullptr;
    }
    if (!window && !SDL_CreateWindowAndRenderer("PersonaRM", win_h0 * win_aw / win_ah, win_h0, win_flags, &window, &renderer)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    // Vsync would throttle a 120 Hz --interp to the display rate (game at half speed) and defeats --unthrottled.
    const bool vsync = cfg.video.vsync && !interp_hz && !unthrottled;
    if (!SDL_SetRenderVSync(renderer, vsync ? 1 : 0)) std::fprintf(stderr, "vsync: %s\n", SDL_GetError());
    std::string window_mode = hidden ? "windowed" : cfg.video.window_mode;
    auto apply_mode = [&](const std::string& m) {
        if (m == "fullscreen") {  // exclusive at the desktop resolution
            SDL_SetWindowFullscreenMode(window, SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window)));
            SDL_SetWindowFullscreen(window, true);
        } else if (m == "borderless") {  // desktop-sized, no mode change
            SDL_SetWindowFullscreenMode(window, nullptr);
            SDL_SetWindowFullscreen(window, true);
        } else SDL_SetWindowFullscreen(window, false);
    };
    apply_mode(window_mode);
    std::printf("[settings] file=%s window=%s x%d integer=%d aspect=%s vsync=%d interp=%d renderer=%s scale=%d volume=%d mute=%d pause_on_focus_loss=%d\n",
                settings_path.c_str(), window_mode.c_str(), cfg.video.window_scale, cfg.video.integer_scaling,
                cfg.video.aspect.c_str(), vsync, interp_hz, use_gpu ? "gpu" : "software", scale, cfg.audio.master_volume, cfg.audio.mute, cfg.pause_on_focus_loss);
    // SPU output: 44.1 kHz stereo s16. No stream = run silent.
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    auto set_gain = [&](bool silent) {
        if (audio) SDL_SetAudioStreamGain(audio, silent || cfg.audio.mute ? 0.0f : cfg.audio.master_volume / 100.0f);
    };
    set_gain(false);
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
    // Per-game options (file section "game"/<serial>, then the command line), known once the disc is.
    const host::Settings eff = host::ApplyCli(file, opt, engine.DiscSerial());
    const host::GameSettings game = host::GameFor(eff, engine.DiscSerial());
    if (opt.save_settings && !settings_path.empty()) {
        std::string err;
        if (host::SaveSettings(settings_path, eff, &err)) std::printf("[settings] saved %s\n", settings_path.c_str());
        else std::fprintf(stderr, "[settings] %s\n", err.c_str());
    }
    // Native code (on by default): the modules prepared for this disc by personarm_native (or linked
    // into a developer build). Missing or stale: the interpreter runs everything, never an error.
    if (game.recomp.value_or(true)) {
        std::string note;
        const auto mods = recomp::NativeModulesFor(disc, recomp::GeneratedModules(), note);
        if (mods.empty()) {
            std::printf("[native] off, running interpreted: %s\n", note.c_str());
        } else {
            std::printf("[native] %zu recompiled modules (%s)\n", mods.size(), note.c_str());
            engine.SetRecompModules(mods, opt.recomp_quantum);
        }
    } else {
        std::printf("[native] off (setting), running interpreted\n");
    }
    std::string card_path[2];
    for (int slot = 0; slot < 2; ++slot) {
        std::string err;
        card_path[slot] = host::MemcardFor(eff, game, engine.DiscSerial(), slot);
        if (!card_path[slot].empty()) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(card_path[slot]).parent_path(), ec);
        }
        if (!card_path[slot].empty() && !cards[slot].Open(card_path[slot], &err)) {
            std::fprintf(stderr, "memcard%d: %s\n", slot + 1, err.c_str());
            SDL_Quit();
            return EXIT_FAILURE;
        }
    }
    // Texture dump / HD replacement: off unless asked; the GPU then runs its untouched path.
    VramTextureHook texture_hook;
    if (!opt.texture_dump.empty() || !game.mods.empty()) {
        if (!opt.texture_dump.empty()) texture_hook.SetDumpDir(opt.texture_dump);
        if (!game.mods.empty())
            std::printf("[textures] %zu replacement textures indexed from %s\n", texture_hook.LoadMods(game.mods),
                        game.mods.c_str());
        engine.GetGpu().SetTextureHook(&texture_hook);
    }
    // Gameplay options (mod manifests, then the command line) and file mods: none = no patch = untouched engine.
    FileRedirect file_redirect;
    {
        GameplayOptions gameplay;
        if (!game.mods.empty()) gameplay = LoadGameplayMods(game.mods, engine.DiscSerial());
        if (!game.text_speed.empty()) ParseTextSpeed(game.text_speed, gameplay.text_speed);
        if (!game.encounter_rate.empty()) ParseEncounterRate(game.encounter_rate, gameplay.encounter_rate);
        gameplay.skip_fmv = game.skip_fmv.value_or(false);
        std::vector<GamePatch> patches = BuildGamePatches(engine.DiscSerial(), gameplay);  // logs what the disc lacks
        for (const GamePatch& g : patches) std::printf("[gameplay] %s\n", g.name.c_str());
        if (!game.mods.empty()) {
            std::vector<GamePatch> files = InstallFileMods(engine, file_redirect, game.mods);
            if (!files.empty()) std::printf("[files] %zu replacement files from %s\n", files.size(), game.mods.c_str());
            patches.insert(patches.end(), files.begin(), files.end());
        }
        engine.SetGamePatches(std::move(patches));
    }
    GpuRenderer gpu_renderer;  // --renderer gpu: the CPU keeps rasterizing at 1x only
    bool gpu_direct = true;    // its pictures are drawn by the window renderer (no readback)
    if (use_gpu) {
        std::string err;
        if (!gpu_renderer.Init(engine.GetGpu(), uint32_t(scale), &err, SDL_GetGPURendererDevice(renderer)) ||
            (interp_hz && !gpu_renderer.EnableInterp(&err))) {
            std::fprintf(stderr, "[gpu] renderer unavailable (%s): software renderer\n", err.c_str());
            gpu_renderer.Shutdown();
            use_gpu = false;
            scale = interp_hz ? 1 : std::clamp(scale, 1, 4);
        }
        if (use_gpu && !game.mods.empty()) gpu_renderer.SetTextureHook(&texture_hook);  // HD textures at s x
    }
    engine.GetGpu().SetResolutionScale(uint32_t(use_gpu ? 1 : scale));
    const bool widescreen = win_aw * 3 != win_ah * 4;
    if (widescreen) {
        engine.SetWidescreen(4 * win_ah, 3 * win_aw);
        std::printf("[video] widescreen %d:%d\n", win_aw, win_ah);
    }
    engine.SetPadScript(presses);
    for (int slot = 0; slot < 2; ++slot)
        if (!card_path[slot].empty()) engine.SetMemoryCard(slot, &cards[slot]);

    // Resolve the name tables to SDL codes once.
    struct KeyBit { SDL_Scancode code; uint16_t bit; };
    std::vector<KeyBit> keys;
    for (const auto& m : host::FlattenBindings(cfg.keyboard)) {
        if (SDL_Scancode sc = SDL_GetScancodeFromName(m.name.c_str()); sc != SDL_SCANCODE_UNKNOWN) keys.push_back({sc, m.bit});
        else std::fprintf(stderr, "[settings] input.keyboard: unknown key \"%s\"\n", m.name.c_str());
    }
    struct PadBit { SDL_GamepadButton button; SDL_GamepadAxis axis; uint16_t bit; };
    std::vector<PadBit> pad_bits;
    for (const auto& m : host::FlattenBindings(cfg.gamepad)) {
        const PadBit p{SDL_GetGamepadButtonFromString(m.name.c_str()), SDL_GetGamepadAxisFromString(m.name.c_str()), m.bit};
        if (p.button == SDL_GAMEPAD_BUTTON_INVALID && p.axis == SDL_GAMEPAD_AXIS_INVALID)
            std::fprintf(stderr, "[settings] input.gamepad: unknown button \"%s\"\n", m.name.c_str());
        else pad_bits.push_back(p);
    }
    std::vector<SDL_Gamepad*> gamepads;

    // --record-input: the effective pad mask of every emulated frame (paused frames never get here).
    host::InputRecorder recorder;
    auto write_recording = [&](uint64_t end_frame) {
        if (opt.record_input.empty()) return;
        std::time_t now = std::time(nullptr);
        char date[32] = "";
        std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::ofstream out(opt.record_input, std::ios::trunc);
        out << "# personarm_sdl --record-input: disc " << (engine.DiscSerial().empty() ? "(unrecognized)" : engine.DiscSerial())
            << ", " << date << ", memcard1 " << (card_path[0].empty() ? "none" : card_path[0]) << ", memcard2 "
            << (card_path[1].empty() ? "none" : card_path[1]) << "\n# replay: personarm --boot <cue> --input-script <this file> --max-frames "
            << end_frame << "\n" << recorder.Script(end_frame);
        if (!out) std::fprintf(stderr, "cannot write %s\n", opt.record_input.c_str());
    };
    SDL_Texture* texture = nullptr;
    int tex_w = 0, tex_h = 0;
    std::vector<uint32_t> rgba;
    std::vector<int16_t> pcm;
    uint64_t audio_samples = 0, lit_frames = 0;
    const uint64_t frame_ns = SDL_NS_PER_SECOND / 60;  // engine frame = 564480 cycles = 1/60 s
    uint64_t next = SDL_GetTicksNS();
    bool quit = false, paused = false;
    // --interp: the GPU logs its packets (display-only capture, emulation unchanged) for the interpolator.
    interp::FrameInterpolator interp;
    std::vector<uint32_t> cmd_log;
    if (interp_hz) engine.GetGpu().SetCommandLog(&cmd_log);
    uint64_t run_ns = 0, interp_ns = 0, interp_frames = 0, last_hash = 0, gpu_ns = 0, wall_t0 = SDL_GetTicksNS();
    uint32_t gpu_draws = 0;  // most draw calls in one picture this second
    uint64_t last_present_ns = 0;
    uint32_t presents = 0;
    double gap_min = 1e9, gap_max = 0, gap_sum = 0, gap_sq = 0;  // --stats: wall time between presents (ms)
    uint32_t flips = 0, changes = 0, pairs = 0, pairs_ok = 0, last_start = ~0u;

    // Save states: F5 save, F8 load, F6/F7 slot. <save_dir>/<serial>.state<N>, else the settings directory.
    namespace fs = std::filesystem;
    std::string state_dir = eff.save_dir;
    if (state_dir.empty()) {
        if (const char* pref = SDL_GetPrefPath("PersonaRM", "PersonaRM")) state_dir = pref;
    }
    int state_slot = 0;
    uint64_t title_until = 0;
    auto state_path = [&] {
        return (fs::path(state_dir) / ((engine.DiscSerial().empty() ? std::string("unknown") : engine.DiscSerial()) +
                                       ".state" + std::to_string(state_slot))).string();
    };
    auto notify = [&](const std::string& m) {  // stderr + window title for a few seconds
        std::fprintf(stderr, "[state] %s\n", m.c_str());
        SDL_SetWindowTitle(window, ("PersonaRM - " + m).c_str());
        title_until = SDL_GetTicksNS() + 3 * SDL_NS_PER_SECOND;
    };
    auto save_state = [&] {
        std::vector<uint8_t> buf;
        engine.SaveState(buf);
        const std::string path = state_path(), tmp = path + ".tmp";
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        bool ok;
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
            ok = bool(f);
        }
        if (ok) fs::rename(tmp, path, ec);  // atomic: a crash never leaves half a state
        if (!ok || ec) { fs::remove(tmp, ec); notify("cannot write " + path); return; }
        notify("saved slot " + std::to_string(state_slot) + " (" + path + ")");
    };
    auto load_state = [&] {
        const std::string path = state_path();
        std::ifstream f(path, std::ios::binary);
        if (!f) { notify("slot " + std::to_string(state_slot) + " is empty"); return; }
        const std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        if (!engine.LoadState(buf, &err)) { notify("slot " + std::to_string(state_slot) + ": " + err); return; }
        if (audio) SDL_ClearAudioStream(audio);
        cmd_log.clear();
        interp = interp::FrameInterpolator();
        next = SDL_GetTicksNS();
        notify("loaded slot " + std::to_string(state_slot) + " (frame " + std::to_string(engine.GetFrameCount()) + ")");
    };
    // Fast-forward (held): up to fast_forward_max emulated frames per 1/60 s, one picture each, audio dropped.
    const int ff_max = std::clamp(cfg.video.fast_forward_max, 2, 16);
    uint64_t ff_frames = 0;
    // battle_speed: the same, automatically, while the scene mode says battle (game.<serial>.battle_speed).
    const int battle_speed = game.battle_speed > 1 ? game.battle_speed : 1;
    const uint32_t scene_mode_addr = battle_speed > 1 ? SceneModeAddress(engine.DiscSerial()) : 0;
    if (battle_speed > 1 && !scene_mode_addr)
        std::fprintf(stderr, "[gameplay] battle_speed=%d: no battle flag known for disc %s, ignored\n", battle_speed,
                     engine.DiscSerial().empty() ? "(unrecognized)" : engine.DiscSerial().c_str());
    else if (battle_speed > 1) std::printf("[gameplay] battle_speed=%d\n", battle_speed);

    if (opt.load_slot >= 0) { state_slot = opt.load_slot; load_state(); }  // reproduce a reported moment
    while (!quit && engine.GetState() == EngineState::Running && (!max_frames || engine.GetFrameCount() < max_frames)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) quit = true;
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE && !ev.key.repeat) {
                // Ask first: Esc sits next to keys used in play, and quitting loses unsaved progress.
                if (hidden) { quit = true; continue; }
                const SDL_MessageBoxButtonData buttons[] = {
                    {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Keep playing"},
                    {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Quit"},
                };
                const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, window, "PersonaRM",
                                             "Quit the game? Unsaved progress will be lost.", 2, buttons, nullptr};
                int choice = 0;
                if (SDL_ShowMessageBox(&box, &choice) && choice == 1) quit = true;
                next = SDL_GetTicksNS();  // the dialog paused us: don't race to catch up
            }
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT) &&
                     !ev.key.repeat && !hidden) {  // Alt+Enter: leave fullscreen / enter the configured (else borderless) one
                if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) window_mode = "windowed";
                else window_mode = cfg.video.window_mode == "windowed" ? "borderless" : cfg.video.window_mode;
                apply_mode(window_mode);
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_F5) {
                save_state();
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_F8) {
                load_state();
            } else if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.scancode == SDL_SCANCODE_F6 || ev.key.scancode == SDL_SCANCODE_F7)) {
                state_slot = (state_slot + (ev.key.scancode == SDL_SCANCODE_F7 ? 1 : 9)) % 10;
                notify("slot " + std::to_string(state_slot));
            } else if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST && cfg.pause_on_focus_loss) {
                paused = true;  // never minimize or grab focus back: just stop emulating and go quiet
                set_gain(true);
                if (audio) SDL_ClearAudioStream(audio);
            } else if (ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED && paused) {
                paused = false;
                set_gain(false);
                next = SDL_GetTicksNS();
            } else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad* g = SDL_OpenGamepad(ev.gdevice.which)) gamepads.push_back(g);
            } else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                bool mapped = false;
                for (const PadBit& p : pad_bits) mapped |= p.button == SDL_GamepadButton(ev.gbutton.button);
                std::printf("[pad] %s%s\n", SDL_GetGamepadStringForButton(SDL_GamepadButton(ev.gbutton.button)),
                            mapped ? "" : " (not mapped; see input.gamepad in docs/SETTINGS.md)");
            } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (size_t i = 0; i < gamepads.size(); ++i)
                    if (SDL_GetGamepadID(gamepads[i]) == ev.gdevice.which) {
                        SDL_CloseGamepad(gamepads[i]);
                        gamepads.erase(gamepads.begin() + i);
                        break;
                    }
            }
        }

        if (paused) { SDL_Delay(16); continue; }

        uint16_t mask = 0;  // all 16 digital pad buttons
        const bool* ks = SDL_GetKeyboardState(nullptr);
        for (const KeyBit& k : keys)
            if (ks[k.code]) mask |= k.bit;
        for (SDL_Gamepad* g : gamepads)
            for (const PadBit& p : pad_bits) {
                if (p.button != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(g, p.button)) mask |= p.bit;
                if (p.axis != SDL_GAMEPAD_AXIS_INVALID && SDL_GetGamepadAxis(g, p.axis) > 16384) mask |= p.bit;
            }
        for (SDL_Gamepad* g : gamepads)  // the game reads a digital pad: the left stick drives the d-pad too
            mask |= host::StickToDpad(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
        engine.SetPadButtons(mask);
        bool ff = ks[SDL_SCANCODE_TAB];
        for (SDL_Gamepad* g : gamepads) ff |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        if (title_until && SDL_GetTicksNS() > title_until) { SDL_SetWindowTitle(window, "PersonaRM"); title_until = 0; }
        const bool in_battle = scene_mode_addr && IsBattleScene(engine.Read(scene_mode_addr, 4));
        const int speed = host::FrameSpeed(ff, ff_max, in_battle, battle_speed);
        const uint64_t slot_ns = frame_ns / uint64_t(speed);  // wall time per emulated frame
        auto pace = [&](uint64_t ns) {
            if (unthrottled) return;
            const uint64_t now = SDL_GetTicksNS();  // wait for this slot's deadline, then book the next one
            if (now < next) SDL_DelayPrecise(next - now);
            else if (now - next > 6 * frame_ns) next = now;  // too far behind: drop the debt instead of racing
            next += ns;
        };
        if (!opt.record_input.empty()) {
            recorder.Frame(engine.GetFrameCount(), mask | engine.ScriptedButtons());
            if (engine.GetFrameCount() % 600 == 599) write_recording(engine.GetFrameCount() + 1);
        }

        const uint64_t run_t0 = SDL_GetTicksNS();
        engine.RunFrame();  // with --resolution-scale this includes the scaled raster pass
        run_ns += SDL_GetTicksNS() - run_t0;

        engine.TakeAudio(pcm);
        audio_samples += pcm.size();
        if (audio && !pcm.empty() && speed == 1) {  // fast-forward / battle speed: dropped (no pitched or broken audio)
            // Keep latency bounded if the emulator ran ahead of the device clock (> 0.25 s queued).
            if (SDL_GetAudioStreamQueued(audio) > 44100 * 4 / 4) SDL_ClearAudioStream(audio);
            SDL_PutAudioStreamData(audio, pcm.data(), int(pcm.size() * sizeof(int16_t)));
        }

        Ps1Gpu& gpu = engine.GetGpu();
        const uint64_t frame = engine.GetFrameCount();
        // shortcut: a frame is "3D" if the GTE projected anything in the last 30 frames (the same rule the
        // GPU uses to squeeze 2D); 2D-only screens (title, FMV, menus without a scene) keep their 4:3 picture.
        const Widescreen2D* ws = engine.GetWidescreen2D();
        const bool wide_now = ws && ws->Wide() && !gpu.GetDisplayArea().is_24bit;
        if (interp_hz && interp.Consume(cmd_log, gpu, frame)) {
            if (use_gpu) gpu_renderer.Flip();  // snapshot the scaled picture of the new game frame
            if (interp.Active(frame)) {
                ++pairs;
                pairs_ok += interp.PairOk();
            }
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
                std::printf("[stats] f=%llu flips/s=%u pictures/s=%u interp_pairs=%u/%u %ux%u%s frame_ms=%.2f",
                            (unsigned long long)frame, flips, changes, pairs_ok, pairs, d.width, d.height,
                            d.is_24bit ? " 24bpp" : "", double(run_ns) / 60e6);
                const uint64_t now = SDL_GetTicksNS();  // whole loop per emulated frame (emulation + present)
                std::printf(" wall_ms=%.2f", double(now - wall_t0) / 60e6);
                wall_t0 = now;
                if (use_gpu)  // host GPU submission per presented picture (+ readback when dumping)
                    std::printf(" gpu_ms=%.2f max_draws=%u", double(gpu_ns) / 60e6, gpu_draws);
                if (presents) {  // presented pictures per second and the wall gap between them (even pacing: min ~ mean ~ max)
                    const double mean = gap_sum / presents;
                    std::printf(" presents/s=%u gap_ms=%.2f/%.2f/%.2f sd=%.2f", presents + 1, gap_min, mean, gap_max,
                                std::sqrt(std::max(0.0, gap_sq / presents - mean * mean)));
                }
                std::printf("\n");
                std::fflush(stdout);
                presents = 0, gap_min = 1e9, gap_max = gap_sum = gap_sq = 0;
                flips = changes = pairs_ok = pairs = 0, run_ns = 0, gpu_ns = 0, gpu_draws = 0;
            }
        }

        // Fast-forward presents one emulated frame out of ff_max (the display rate stays ~60 Hz, vsync or not).
        if (speed > 1 && ++ff_frames % uint64_t(speed) != 0 && !(max_frames && frame >= max_frames)) {
            pace(slot_ns);
            continue;
        }
        std::vector<double> phases = host::PresentPhases(present_hz, frame);  // fps_target 30: {} on skipped frames
        // 30 skips the picture (and its conversion); the GPU renderer must flush its recorded work every frame, and
        // the last frame is always shown for --screenshot.
        if (phases.empty() && (use_gpu || (max_frames && frame >= max_frames && !screenshot.empty()))) phases = {0.0};
        if (phases.empty()) { pace(slot_ns); continue; }
        const int sub = int(phases.size());
        for (int s = 0; s < sub; ++s) {
            const std::vector<uint16_t>* vram = &gpu.GetVram();
            GpuDisplayArea d = gpu.GetDisplayArea();
            bool off = (gpu.ReadGPUSTAT() >> 23) & 1;
            double t = -1;
            GpuRenderer::Source src = GpuRenderer::kLive;
            if (interp_hz && interp.Active(frame)) {
                t = interp.Fraction(double(frame) + phases[s]);
                const uint64_t t0 = SDL_GetTicksNS();
                const bool lerp = t > 0 && t < 1 && interp.PairOk();  // else Render shows one of the two frames
                src = lerp ? GpuRenderer::kReplay : t >= 1 ? GpuRenderer::kCur : GpuRenderer::kPrev;
                if (lerp && use_gpu) gpu_renderer.BeginReplay(interp.Scratch());  // the replay draws at s x too
                interp.Render(t, vram, d);
                if (lerp && use_gpu) gpu_renderer.EndReplay();
                if (lerp) interp_ns += SDL_GetTicksNS() - t0, ++interp_frames;
                off = false;
            }
            // 24bpp (FMV) is shown at 1x and stretched by the presenter.
            bool gpu_frame = use_gpu && !d.is_24bit;  // 24bpp (FMV): software at 1x, GPU work only flushed
            const bool dump = !dump_dir.empty() && frame >= dump_from && frame <= dump_to;
            // Direct: the GPU picture stays on the GPU and is drawn by the window's renderer; read back to dump only.
            bool direct = gpu_direct && gpu_frame && !off && !dump && gpu_renderer.SharedDevice();
            if (use_gpu) {  // runs even with the display off: the recorded work must be flushed
                const uint64_t gpu_t0 = SDL_GetTicksNS();
                if (!gpu_renderer.Present(d, gpu_frame && !off, gpu_frame && !off && !direct ? &rgba : nullptr, src)) {
                    std::fprintf(stderr, "[gpu] present failed (%s): software renderer at 1x\n", SDL_GetError());
                    gpu_renderer.Shutdown();
                    use_gpu = gpu_frame = direct = false;
                    scale = 1;
                }
                gpu_ns += SDL_GetTicksNS() - gpu_t0;
                gpu_draws = std::max(gpu_draws, gpu_renderer.LastDrawCalls());
            }
            SDL_Texture* shown = direct ? gpu_renderer.OutputTexture(renderer) : nullptr;
            if (direct && !shown) {  // cannot wrap the GPU texture: read back from now on
                std::fprintf(stderr, "[gpu] direct present unavailable (%s): reading pictures back\n", SDL_GetError());
                gpu_direct = direct = false;
                gpu_renderer.Present(d, true, &rgba, src);  // nothing queued: display pass + readback only
            }
            const uint32_t hs = scale > 1 && !d.is_24bit ? uint32_t(scale) : 1;
            const uint32_t w = (d.width ? d.width : 320) * hs, h = (d.height ? d.height : 240) * hs;
            if (direct) {  // non-black check on the 1x picture
                bool lit = false;
                for (uint32_t y = 0; y < h / hs && !lit; ++y)
                    for (uint32_t x = 0; x < w / hs && !lit; ++x) lit = ((*vram)[((d.y + y) & 511) * 1024 + ((d.x + x) & 1023)] & 0x7FFF) != 0;
                lit_frames += lit;
            } else {
                if (off) rgba.assign(size_t(w) * h, 0xFF000000u);  // display disabled
                else if (gpu_frame) {}  // rgba already holds the GPU picture
                else if (hs > 1) host::DisplayToRgba(gpu.GetScaledVram(), d.x, d.y, w / hs, h / hs, false, rgba, hs);
                else host::DisplayToRgba(*vram, d.x, d.y, w, h, d.is_24bit, rgba);
                for (uint32_t p : rgba)
                    if (p & 0xFFFFFFu) { ++lit_frames; break; }
            }
            if (dump) {  // t999 = live, not interpolated
                char name[64];
                std::snprintf(name, sizeof name, "/f%06llu_%d_t%03d.png", (unsigned long long)frame, s,
                              t < 0 ? 999 : int(t * 100 + 0.5));
                PngCodec::EncodeToFile(dump_dir + name, w, h, reinterpret_cast<const uint8_t*>(rgba.data()));
            }
            if (!shown) {
                if (!texture || tex_w != int(w) || tex_h != int(h)) {
                    if (texture) SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, int(w), int(h));
                    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
                    tex_w = int(w), tex_h = int(h);
                }
                SDL_UpdateTexture(texture, nullptr, rgba.data(), int(w * 4));
                shown = texture;
            }

            int win_w = 0, win_h = 0;
            SDL_GetRenderOutputSize(renderer, &win_w, &win_h);
            const host::Rect r = host::PresentRect(win_w, win_h, cfg.video.aspect == "stretch", cfg.video.integer_scaling, int(h),
                                                   wide_now ? win_aw : 4, wide_now ? win_ah : 3);
            const SDL_FRect dst{float(r.x), float(r.y), float(r.w), float(r.h)};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, shown, nullptr, &dst);
            const bool last = s == sub - 1 && !screenshot.empty() && (quit || (max_frames && frame >= max_frames));
            if (last) {
                if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                    if (!SDL_SaveBMP(shot, screenshot.c_str())) std::fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
                    SDL_DestroySurface(shot);
                }
            }
            pace(slot_ns * 60 / uint64_t(present_hz > 60 ? present_hz : 60));  // before the present: even spacing at 90
            if (stats) {
                const uint64_t tp = SDL_GetTicksNS();
                if (last_present_ns) {
                    const double g = double(tp - last_present_ns) / 1e6;
                    gap_min = std::min(gap_min, g), gap_max = std::max(gap_max, g), gap_sum += g, gap_sq += g * g;
                    ++presents;
                }
                last_present_ns = tp;
            }
            SDL_RenderPresent(renderer);
        }
    }

    write_recording(engine.GetFrameCount());
    if (interp_frames)
        std::printf("[interp] %llu synthesized pictures, %.3f ms each\n", (unsigned long long)interp_frames,
                    double(interp_ns) / 1e6 / double(interp_frames));
    std::printf("[SDL] frames=%llu non-black=%llu audio_samples=%llu\n", (unsigned long long)engine.GetFrameCount(),
                (unsigned long long)lit_frames, (unsigned long long)audio_samples);
    engine.GetGpu().SetCommandLog(nullptr);
    gpu_renderer.Shutdown();
    if (!opt.texture_dump.empty() || !game.mods.empty()) {
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
