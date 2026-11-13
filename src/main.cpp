/**
 * @file main.cpp
 * @brief Entry point for PersonaRM - Native Launcher & Port Engine
 *
 * Handles:
 * - Launcher UI and configuration
 * - Game boot sequence & legal copy validation
 * - Command-line automation & CI testing
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <chrono>

// Project headers
#include "launcher/launcher.h"
#include "launcher/portal.h"
#include "core/memory_manager.h"
#include "core/save_manager.h"
#include "input/input_manager.h"
#include "loader/disc_reader.h"
#include "loader/tim_parser.h"
#include "loader/ps1_bios_hle.h"
#include "rendering/renderer.h"
#include "rendering/ps1_gpu.h"
#include "audio/audio_system.h"
#include "audio/vag_decoder.h"
#include "audio/spu_emulator.h"
#include "cpu/gte.h"
#include "loader/exe_loader.h"
#include "loader/png_codec.h"
#include "loader/str_demuxer.h"
#include "engine/engine.h"
#include "rendering/window_context.h"
#include "rendering/gl_renderer.h"
#include "rendering/mdec_decoder.h"

static constexpr const char* LAUNCHER_NAME    = "PersonaRM - Native Launcher & Hub";
static constexpr const char* LAUNCHER_VERSION = "0.2.0";
static constexpr const char* GAME_NAME        = "Persona 2: Eternal Punishment";

static void PrintVersion() {
    std::cout << LAUNCHER_NAME << "\n"
              << "Version: " << LAUNCHER_VERSION << "\n"
              << "Target Game: " << GAME_NAME << "\n"
              << "License: GPL v3.0 or later\n"
              << "Build: " << __DATE__ << " " << __TIME__ << "\n";
}

static void PrintUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Launcher Options:\n"
              << "  --portal           Launch interactive PersonaRM Portal & Game Hub\n"
              << "  --launch           Directly boot the game using current configuration\n"
              << "  --disc1 <path>     Link Disc 1 image (Main Game)\n"
              << "  --disc2 <path>     Link Disc 2 image (Special Preview / Swap Disc)\n"
              << "  --swap-disc <1|2>  Switch active disc target\n"
              << "  --verify-disc <f>  Inspect and display technical details of any disc image\n"
              << "  --trace-out <file> Write a PRMTRACE v1 execution trace (docs/TRACE_FORMAT.md)\n"
              << "  --boot <disc.cue>  Headless boot harness: run the disc's BOOT EXE with the HLE BIOS and\n"
              << "                     log milestones (main, scene loop, load_file_by_id/cd_read_start, overlay\n"
              << "                     entries, scene changes, GP1 display mode, first polygon / image, SPU\n"
              << "                     key-on, unimplemented BIOS calls) for --max-frames N (default 600).\n"
              << "                     --stop-at-first-load   stop when the first load_file_by_id returns\n"
              << "                     --dump-ram <file>      2 MB RAM image at stop\n"
              << "                     --dump-ram-at-loads <dir>  RAM image at each load_file_by_id return\n"
              << "                     --dump-vram <png>      1024x512 VRAM at stop\n"
              << "                     --dump-frames <dir> --every N   display area PNG every N frames\n"
              << "                     --dump-audio <wav>     CD XA audio, 16-bit stereo 44.1 kHz\n"
              << "                     --press <btn>@<frame>[+<n>]     hold a pad button n frames (default 5)\n"
              << "                     --watch-pc <hex>       log the first 20 hits of a PC (repeatable)\n"
              << "  --config           Open interactive settings menu\n"
              << "  --scan [dir]       Scan directory for legal game disc images (.iso/.bin/.cue)\n"
              << "  --diagnostics      Run full diagnostic suite\n"
              << "  --version          Show launcher version\n"
              << "  --help             Show this help information\n\n"
              << "Component Self-Tests:\n"
              << "  --test-portal      Verify PersonaRM Portal & BYOC disc management\n"
              << "  --test-memory      Verify memory manager allocations and policies\n"
              << "  --test-input       Verify controller & KBM action bindings (SDL2 not integrated)\n"
              << "  --test-renderer    Verify renderer viewport logic (no GL context yet)\n"
              << "  --test-audio       Verify audio mixer logic (no physical output yet)\n"
              << "  --test-load        Verify binary/disc loader\n"
              << "  --test-tim         Verify TIM texture parser\n"
              << "  --test-vag         Verify VAG SPU audio decoder\n"
              << "  --test-gpu         Verify PS1 GPU VRAM & GP0/GP1 commands\n"
              << "  --test-bios        Verify PS1 BIOS HLE system call dispatcher\n"
              << "  --test-gte         Verify PS1 Geometry Transformation Engine math\n"
              << "  --test-exe         Verify PS-X EXE header and payload loader\n"
              << "  --test-spu         Verify PS1 SPU 24-voice audio mixer\n"
              << "  --test-str         Verify PS1 STR/MDEC CD-ROM XA video demuxer\n"
              << "  --test-engine      Verify PersonaRM unified core engine & frame loop\n"
              << "  --test-window      Verify Window Context & GL presentation pipeline\n"
              << "  --test-mdec        Verify PS1 MDEC video macroblock IDCT decoder\n"
              << "  --test-save        Verify modern save state system\n"
              << "  --test-performance Run engine frametime benchmarks\n";
}

static int RunMemorySelfTest() {
    std::cout << "[TEST] Memory Manager self-test...\n";
    try {
        personarm::MemoryManager mgr;
        mgr.Initialize(2 * 1024 * 1024); // 2MB

        void* block1 = mgr.Allocate(128, personarm::AllocationPolicy::POOLED);
        void* block2 = mgr.Allocate(1024, personarm::AllocationPolicy::DEFAULT);

        if (!block1 || !block2) {
            std::cerr << "[FAIL] Failed to allocate memory blocks.\n";
            return 1;
        }

        std::cout << "  Regions: " << mgr.NumRegions() << "\n";
        std::cout << "  Total Allocated: " << mgr.TotalAllocated() << " bytes\n";
        std::cout << "  Total Available: " << mgr.TotalAvailable() << " bytes\n";

        mgr.Deallocate(block1);
        mgr.Shutdown();
        std::cout << "[PASS] Memory Manager self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during memory test: " << e.what() << "\n";
        return 1;
    }
}

static int RunInputSelfTest() {
    std::cout << "[TEST] Input Manager self-test...\n";
    try {
        auto& input = personarm::InputManager::Instance();
        if (!input.Initialize()) {
            std::cerr << "[FAIL] Failed to initialize InputManager.\n";
            return 1;
        }

        input.SimulateActionPress(personarm::VirtualAction::Confirm);
        input.Update(0.016f);

        if (!input.IsActionPressed(personarm::VirtualAction::Confirm)) {
            std::cerr << "[FAIL] Action Confirm was not registered.\n";
            return 1;
        }

        std::cout << "  - Confirm Glyph (KBM): " << input.GetActionGlyphDescription(personarm::VirtualAction::Confirm) << "\n";
        std::cout << "  - Attack Glyph (KBM):  " << input.GetActionGlyphDescription(personarm::VirtualAction::BattleAttack) << "\n";
        std::cout << "  - Turbo Glyph (KBM):   " << input.GetActionGlyphDescription(personarm::VirtualAction::Turbo) << "\n";

        input.SimulateActionRelease(personarm::VirtualAction::Confirm);
        input.Update(0.016f);

        if (input.IsActionPressed(personarm::VirtualAction::Confirm)) {
            std::cerr << "[FAIL] Action Confirm was not released.\n";
            return 1;
        }

        std::cout << "[PASS] Input Manager self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during input test: " << e.what() << "\n";
        return 1;
    }
}

static int RunLoaderSelfTest() {
    std::cout << "[TEST] Disc & Executable Loader self-test...\n";
    try {
        std::vector<uint8_t> psx_header(2048, 0);
        std::memcpy(psx_header.data(), "PS-X EXE", 8);
        *reinterpret_cast<uint32_t*>(psx_header.data() + 0x10) = 0x80010000;
        *reinterpret_cast<uint32_t*>(psx_header.data() + 0x18) = 0x80010000;
        *reinterpret_cast<uint32_t*>(psx_header.data() + 0x1C) = 512 * 1024;

        auto info = personarm::DiscReader::ParseExecutable(psx_header.data(), psx_header.size());
        if (!info.valid || info.format != "PS-X EXE" || info.entry_point != 0x80010000) {
            std::cerr << "[FAIL] Failed to parse PS-X EXE header.\n";
            return 1;
        }

        std::cout << "  - Format:      " << info.format << "\n";
        std::cout << "  - Entry Point: 0x" << std::hex << info.entry_point << std::dec << "\n";
        std::cout << "  - Segments:    " << info.segments.size() << "\n";

        std::cout << "[PASS] Disc & Loader self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during loader test: " << e.what() << "\n";
        return 1;
    }
}

static int RunRendererSelfTest() {
    std::cout << "[TEST] Renderer & Viewport Scaling self-test...\n";
    try {
        auto& renderer = personarm::Renderer::Instance();
        personarm::RendererConfig config;
        config.window_width = 1920;
        config.window_height = 1080;
        config.aspect_ratio = personarm::AspectRatioMode::Widescreen16x9;

        if (!renderer.Initialize(config)) {
            std::cerr << "[FAIL] Failed to initialize Renderer.\n";
            return 1;
        }

        auto vp = renderer.GetCurrentViewport();
        std::cout << "  - Viewport (16:9 1080p): " << vp.width << "x" << vp.height << " at (" << vp.x << "," << vp.y << ")\n";

        // Test 4:3 letterboxing on 1080p window
        auto vp_43 = renderer.CalculateLetterboxViewport(1920, 1080, personarm::AspectRatioMode::Original4x3);
        std::cout << "  - Viewport (4:3 in 1080p): " << vp_43.width << "x" << vp_43.height << " (Pillarboxed at x=" << vp_43.x << ")\n";

        if (vp_43.width != 1440 || vp_43.x != 240) {
            std::cerr << "[FAIL] Incorrect letterbox calculation.\n";
            return 1;
        }

        std::cout << "[PASS] Renderer self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during renderer test: " << e.what() << "\n";
        return 1;
    }
}

static int RunAudioSelfTest() {
    std::cout << "[TEST] Audio System self-test...\n";
    try {
        auto& audio = personarm::AudioSystem::Instance();
        personarm::AudioConfig config;
        config.master_volume = 1.0f;
        config.bgm_volume = 0.8f;
        config.sfx_volume = 0.9f;

        if (!audio.Initialize(config)) {
            std::cerr << "[FAIL] Failed to initialize AudioSystem.\n";
            return 1;
        }

        audio.SetChannelVolume(personarm::AudioChannel::BGM, 0.5f);
        float effective = audio.GetEffectiveVolume(personarm::AudioChannel::BGM);
        std::cout << "  - BGM Effective Volume (Master 1.0 * BGM 0.5): " << effective << "\n";

        if (effective < 0.49f || effective > 0.51f) {
            std::cerr << "[FAIL] Incorrect effective volume calculation.\n";
            return 1;
        }

        std::cout << "[PASS] Audio System self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during audio test: " << e.what() << "\n";
        return 1;
    }
}

static int RunTimSelfTest() {
    std::cout << "[TEST] TIM Texture Parser self-test...\n";
    try {
        std::vector<uint8_t> tim_buf(72, 0);
        tim_buf[0] = 0x10; // Magic
        tim_buf[4] = 0x08; // 4-bit with CLUT

        uint32_t clut_size = 44;
        std::memcpy(&tim_buf[8], &clut_size, 4);
        uint16_t clut_w = 16, clut_h = 1;
        std::memcpy(&tim_buf[16], &clut_w, 2);
        std::memcpy(&tim_buf[18], &clut_h, 2);

        uint32_t img_size = 20;
        std::memcpy(&tim_buf[52], &img_size, 4);
        uint16_t img_w = 2, img_h = 2;
        std::memcpy(&tim_buf[60], &img_w, 2);
        std::memcpy(&tim_buf[62], &img_h, 2);

        auto img = personarm::TimParser::ParseFromMemory(tim_buf.data(), tim_buf.size());
        if (!img || img->width != 8 || img->height != 2) {
            std::cerr << "[FAIL] TIM parser failed to decode synthetic texture.\n";
            return 1;
        }

        std::cout << "  - Decoded 4-bit TIM: " << img->width << "x" << img->height << " (" << img->pixels_rgba.size() << " RGBA pixels)\n";
        std::cout << "[PASS] TIM Texture Parser self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during TIM test: " << e.what() << "\n";
        return 1;
    }
}

static int RunVagSelfTest() {
    std::cout << "[TEST] VAG Audio Decoder self-test...\n";
    try {
        std::vector<uint8_t> vag_buf(64, 0);
        vag_buf[0] = 0x56; vag_buf[1] = 0x41; vag_buf[2] = 0x47; vag_buf[3] = 0x70; // "VAGp"
        vag_buf[7] = 0x20;  // Version
        vag_buf[15] = 16;   // Data size = 1 block
        vag_buf[18] = 0xAC; vag_buf[19] = 0x44; // 44100 Hz BE
        vag_buf[48] = 0x08; // Shift=8, Filter=0
        vag_buf[49] = 0x01; // Loop End

        auto audio = personarm::VagDecoder::DecodeFromMemory(vag_buf.data(), vag_buf.size());
        if (!audio || audio->pcm_samples.size() != 28) {
            std::cerr << "[FAIL] VAG decoder failed to decode synthetic audio block.\n";
            return 1;
        }

        std::cout << "  - Decoded VAG Audio: " << audio->pcm_samples.size() << " samples at " << audio->sample_rate << " Hz\n";
        std::cout << "[PASS] VAG Audio Decoder self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during VAG test: " << e.what() << "\n";
        return 1;
    }
}

static int RunGpuSelfTest() {
    std::cout << "[TEST] PS1 GPU & VRAM self-test...\n";
    try {
        personarm::Ps1Gpu gpu;
        gpu.FillVramRect(0, 0, 32, 32, 0x03E0); // Green (15-bit)
        if (gpu.ReadVram(0, 0) != 0x03E0) {
            std::cerr << "[FAIL] VRAM fill failed.\n";
            return 1;
        }

        std::vector<uint32_t> frame;
        uint32_t w = 0, h = 0;
        gpu.OutputFrameToRGBA(frame, w, h);
        std::cout << "  - GPU Frame output: " << w << "x" << h << " (" << frame.size() << " pixels)\n";
        std::cout << "[PASS] PS1 GPU & VRAM self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during GPU test: " << e.what() << "\n";
        return 1;
    }
}

static int RunBiosSelfTest() {
    std::cout << "[TEST] PS1 BIOS HLE Dispatcher self-test...\n";
    try {
        auto& bios = personarm::Ps1BiosHle::Instance();
        bios.Initialize();

        personarm::CpuContext cpu;
        personarm::MemoryManager mem;
        mem.Initialize(1024 * 1024);

        cpu.SetGPR(9, 0x00); // A(00h) open: no disc linked, so the file cannot exist -> -1
        cpu.SetGPR(31, 0x80010020);
        cpu.SetPC(0x000000A0);

        if (!bios.HandleBiosHook(cpu, mem) || cpu.GetGPR(2) != 0xFFFFFFFFu || cpu.GetPC() != 0x80010020) {
            std::cerr << "[FAIL] BIOS open hook failed.\n";
            return 1;
        }

        std::cout << "  - BIOS Intercepted calls: " << bios.GetInterceptedCallCount() << "\n";
        std::cout << "[PASS] PS1 BIOS HLE Dispatcher self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during BIOS test: " << e.what() << "\n";
        return 1;
    }
}

static int RunGteSelfTest() {
    std::cout << "[TEST] PS1 GTE Geometry Transformation Engine self-test...\n";
    try {
        personarm::Gte gte;
        // Test Normal Clipping (NCLIP)
        gte.WriteDataRegister(12, 0);                 // SXY0 = (0, 0)
        gte.WriteDataRegister(13, (0 << 16) | 20);    // SXY1 = (20, 0)
        gte.WriteDataRegister(14, (20 << 16) | 0);    // SXY2 = (0, 20)
        gte.ExecuteCommand(0x4A000006); // NCLIP
        int32_t mac0 = static_cast<int32_t>(gte.ReadDataRegister(24));
        if (mac0 == 0) {
            std::cerr << "[FAIL] GTE NCLIP produced zero normal.\n";
            return 1;
        }

        // Test AVSZ3 (Average Z of 3 vertices: (150 + 150 + 150) / 3 ≈ 150)
        gte.WriteControlRegister(29, 0x0555); // 1/3 fixed point (1365/4096)
        gte.WriteDataRegister(17, 150);
        gte.WriteDataRegister(18, 150);
        gte.WriteDataRegister(19, 150);
        gte.ExecuteCommand(0x4A00002D); // AVSZ3
        // OTZ is data r7 (psx-spx "GTE Registers"); r20 is RGB0.
        uint32_t otz = gte.ReadDataRegister(7);
        if (otz < 145 || otz > 155) {
            std::cerr << "[FAIL] GTE AVSZ3 computed incorrect average Z.\n";
            return 1;
        }

        std::cout << "  - GTE NCLIP cross product: " << mac0 << "\n";
        std::cout << "  - GTE AVSZ3 average OTZ:   " << otz << "\n";
        std::cout << "[PASS] PS1 GTE self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during GTE test: " << e.what() << "\n";
        return 1;
    }
}

static int RunExeSelfTest() {
    std::cout << "[TEST] PS-X EXE Loader self-test...\n";
    try {
        std::vector<uint8_t> dummy_exe(4096, 0);
        personarm::PsxExeHeader header{};
        std::memcpy(header.magic, "PS-X EXE", 8);
        header.initial_pc = 0x80010000;
        header.destination_ram = 0x80010000;
        header.file_size = 2048;
        header.initial_sp_base = 0x801FFF00;
        header.initial_gp = 0x80080000;
        std::memcpy(dummy_exe.data(), &header, sizeof(header));

        personarm::CpuContext cpu;
        std::vector<uint8_t> ram(2 * 1024 * 1024, 0);
        personarm::PsxExeInfo info{};

        if (!personarm::ExeLoader::LoadExe(dummy_exe.data(), dummy_exe.size(), cpu, ram, info)) {
            std::cerr << "[FAIL] ExeLoader failed to parse valid PS-X EXE.\n";
            return 1;
        }

        std::string cnf = "BOOT = cdrom:\\SLUS_011.58;1\r\n";
        std::string boot_bin = personarm::ExeLoader::ExtractBootExecutable(cnf);
        if (boot_bin != "SLUS_011.58") {
            std::cerr << "[FAIL] Failed to extract boot executable from SYSTEM.CNF.\n";
            return 1;
        }

        std::cout << "  - Extracted Boot EXE: " << boot_bin << "\n";
        std::cout << "  - Configured Entry Point: 0x" << std::hex << info.entry_point << std::dec << "\n";
        std::cout << "[PASS] PS-X EXE Loader self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during EXE test: " << e.what() << "\n";
        return 1;
    }
}

static int RunSpuSelfTest() {
    std::cout << "[TEST] PS1 SPU 24-Voice Audio Emulator self-test...\n";
    try {
        personarm::SpuEmulator spu;
        spu.SetMasterVolume(0x3FFF, 0x3FFF);
        spu.SetVoiceVolume(0, 0x3FFF, 0x3FFF);
        spu.SetVoicePitch(0, 4096);
        spu.SetVoiceStartAddress(0, 0x1000);

        // Upload a valid 16-byte dummy VAG block with Loop End flag (0x01)
        uint8_t dummy_block[16] = { 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        spu.UploadToRam(0x1000, dummy_block, sizeof(dummy_block));

        spu.KeyOn(1 << 0);
        if (spu.GetActiveVoiceMask() == 0) {
            std::cerr << "[FAIL] SPU KeyOn failed to activate voice 0.\n";
            return 1;
        }

        std::vector<int16_t> pcm;
        spu.GenerateSamples(pcm, 128);
        if (pcm.size() != 256) {
            std::cerr << "[FAIL] SPU generated incorrect number of PCM samples.\n";
            return 1;
        }

        std::cout << "  - Generated " << pcm.size() / 2 << " stereo frames at 44.1 kHz\n";
        std::cout << "[PASS] PS1 SPU Audio self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during SPU test: " << e.what() << "\n";
        return 1;
    }
}

static int RunStrSelfTest() {
    std::cout << "[TEST] PS1 STR Video Demuxer self-test...\n";
    try {
        personarm::StrDemuxer demuxer;

        // Create a synthetic 2336-byte STR sector
        std::vector<uint8_t> sector(2336, 0);
        auto* hdr = reinterpret_cast<personarm::StrSectorHeader*>(sector.data());
        hdr->magic = personarm::STR_MAGIC;
        hdr->chunk_type = 0x01;
        hdr->chunk_index = 0;
        hdr->total_chunks = 1;
        hdr->frame_number = 1;
        hdr->payload_size = 512;
        hdr->width = 320;
        hdr->height = 240;

        personarm::StrDemuxedFrame frame;
        bool completed = demuxer.ProcessSector(sector.data(), sector.size(), frame);
        if (!completed || !frame.complete || frame.frame_index != 1) {
            std::cerr << "[FAIL] StrDemuxer failed to assemble single-chunk frame.\n";
            return 1;
        }

        std::cout << "  - Demuxed frame #" << frame.frame_index << ": " << frame.width << "x" << frame.height << "\n";
        std::cout << "[PASS] PS1 STR Video Demuxer self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during STR test: " << e.what() << "\n";
        return 1;
    }
}

static int RunEngineSelfTest() {
    std::cout << "[TEST] PersonaRM Core Engine self-test...\n";
    try {
        auto& engine = personarm::Engine::Instance();
        personarm::EngineBootConfig config;
        config.enable_bios_hle = true;

        if (!engine.Initialize(config)) {
            std::cerr << "[FAIL] Failed to initialize Core Engine.\n";
            return 1;
        }

        // Test booting synthetic executable
        std::vector<uint8_t> exe(4096, 0);
        auto* hdr = reinterpret_cast<personarm::PsxExeHeader*>(exe.data());
        std::memcpy(hdr->magic, "PS-X EXE", 8);
        hdr->initial_pc = 0x80010000;
        hdr->destination_ram = 0x80010000;
        hdr->file_size = 2048;
        hdr->initial_sp_base = 0x801FFF00;

        if (!engine.BootExecutable(exe.data(), exe.size())) {
            std::cerr << "[FAIL] Engine failed to boot executable.\n";
            return 1;
        }

        engine.RunFrame();
        if (engine.GetFrameCount() != 1) {
            std::cerr << "[FAIL] Engine did not advance frame counter.\n";
            return 1;
        }

        std::vector<uint32_t> frame;
        uint32_t w = 0, h = 0;
        engine.GetCurrentFrame(frame, w, h);
        std::cout << "  - Engine Frame Output: " << w << "x" << h << " (" << frame.size() << " pixels)\n";

        engine.Shutdown();
        std::cout << "[PASS] PersonaRM Core Engine self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during Engine test: " << e.what() << "\n";
        return 1;
    }
}

static int RunWindowSelfTest() {
    std::cout << "[TEST] Window Context & GL Presentation pipeline self-test...\n";
    try {
        personarm::WindowContext window;
        personarm::WindowConfig win_cfg;
        win_cfg.headless = true;
        win_cfg.width = 1280;
        win_cfg.height = 720;

        if (!window.Initialize(win_cfg)) {
            std::cerr << "[FAIL] Failed to initialize WindowContext.\n";
            return 1;
        }

        personarm::GlRenderer gl_renderer;
        if (!gl_renderer.Initialize(window)) {
            std::cerr << "[FAIL] Failed to initialize GlRenderer.\n";
            return 1;
        }

        std::vector<uint32_t> frame(320 * 240, 0xFF336699);
        gl_renderer.UploadFrame(frame.data(), 320, 240);
        gl_renderer.Render(1280, 720);

        auto vp = gl_renderer.GetCurrentViewport();
        std::cout << "  - Viewport Letterbox: " << vp.width << "x" << vp.height << " offset (" << vp.x << "," << vp.y << ")\n";

        gl_renderer.Shutdown();
        window.Shutdown();
        std::cout << "[PASS] Window Context & GL Presentation self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during Window test: " << e.what() << "\n";
        return 1;
    }
}

static int RunMdecSelfTest() {
    std::cout << "[TEST] PS1 MDEC Video Macroblock IDCT Decoder self-test...\n";
    try {
        personarm::MdecDecoder mdec;

        // Verify IDCT 8x8 baseline (zero AC gives mid-scale 128)
        int16_t coeff[64] = {0};
        uint8_t pixels[64] = {0};
        personarm::MdecDecoder::PerformIdct8x8(coeff, pixels);
        if (pixels[0] != 128) {
            std::cerr << "[FAIL] MDEC IDCT baseline check failed: expected 128, got " << (int)pixels[0] << "\n";
            return 1;
        }

        // Verify Color space conversion
        uint32_t black = personarm::MdecDecoder::YCbCrToRgba(0, 128, 128);
        if ((black & 0x00FFFFFF) != 0x000000) {
            std::cerr << "[FAIL] MDEC YCbCr conversion failed for black pixel.\n";
            return 1;
        }

        // Verify full frame decoding
        personarm::MdecFrame frame;
        if (!mdec.DecodeFrame(nullptr, 0, 320, 240, frame)) {
            std::cerr << "[FAIL] MDEC frame decoding failed.\n";
            return 1;
        }

        if (frame.width != 320 || frame.height != 240 || frame.pixels_rgba.size() != 320 * 240) {
            std::cerr << "[FAIL] MDEC frame dimensions mismatch.\n";
            return 1;
        }

        std::cout << "  - MDEC Frame Decoded: " << frame.width << "x" << frame.height << " (" << frame.pixels_rgba.size() << " pixels)\n";
        std::cout << "[PASS] PS1 MDEC Video Decoder self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during MDEC test: " << e.what() << "\n";
        return 1;
    }
}

static int RunSaveSelfTest() {
    std::cout << "[TEST] Modern & Virtual PS1 Save System self-test...\n";
    try {
        personarm::SaveManager save_mgr;
        if (!save_mgr.IsFormatted()) {
            std::cerr << "[FAIL] SaveManager was not formatted.\n";
            return 1;
        }

        // Test writing slot 0
        std::vector<uint8_t> sample_save = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
        if (!save_mgr.WriteSlot(0, "Persona 2: EP - Velvet Room", "BASLUS-01158GAME", sample_save.data(), sample_save.size())) {
            std::cerr << "[FAIL] Failed to write slot 0.\n";
            return 1;
        }

        // Verify slot listing
        auto slots = save_mgr.ListSlots();
        if (slots.size() != 15 || !slots[0].is_in_use || slots[0].product_code != "BASLUS-01158GAME") {
            std::cerr << "[FAIL] Slot listing check failed.\n";
            return 1;
        }

        // Verify reading back slot 0
        std::vector<uint8_t> read_data;
        if (!save_mgr.ReadSlot(0, read_data) || read_data.size() < sample_save.size()) {
            std::cerr << "[FAIL] Failed to read slot 0.\n";
            return 1;
        }
        if (std::memcmp(read_data.data(), sample_save.data(), sample_save.size()) != 0) {
            std::cerr << "[FAIL] Read payload content mismatch.\n";
            return 1;
        }

        // Test QuickSave / QuickLoad with Engine
        auto& engine = personarm::Engine::Instance();
        engine.Initialize();
        engine.GetCpu().SetPC(0x80010040);
        engine.SetFrameCount(42);

        std::vector<uint8_t> snapshot;
        if (!save_mgr.CaptureQuickSave(engine, snapshot)) {
            std::cerr << "[FAIL] CaptureQuickSave failed.\n";
            return 1;
        }

        // Alter state
        engine.GetCpu().SetPC(0x00000000);
        engine.SetFrameCount(0);

        // Restore state
        if (!save_mgr.RestoreQuickSave(engine, snapshot.data(), snapshot.size())) {
            std::cerr << "[FAIL] RestoreQuickSave failed.\n";
            return 1;
        }

        if (engine.GetCpu().GetPC() != 0x80010040 || engine.GetFrameCount() != 42) {
            std::cerr << "[FAIL] QuickSave state restoration mismatch.\n";
            return 1;
        }

        std::cout << "  - PS1 128KB Virtual Memory Card: OK\n";
        std::cout << "  - QuickSave Snapshot: " << snapshot.size() << " bytes\n";
        std::cout << "[PASS] Save System self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during Save test: " << e.what() << "\n";
        return 1;
    }
}

static int RunPerformanceSelfTest() {
    std::cout << "[TEST] PersonaRM Core Engine Frametime Performance Benchmark...\n";
    try {
        auto& engine = personarm::Engine::Instance();
        engine.Initialize();

        auto start_time = std::chrono::high_resolution_clock::now();

        constexpr uint32_t BENCHMARK_CYCLES = 1000000;
        uint32_t step_count = 0;
        while (step_count < BENCHMARK_CYCLES) {
            engine.GetCpu().Step();
            step_count++;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        double elapsed_sec = elapsed_us / 1000000.0;
        double mhz = (step_count / (elapsed_sec > 0 ? elapsed_sec : 0.001)) / 1000000.0;

        std::cout << "  - Executed: " << step_count << " CPU steps in " << elapsed_us << " us\n";
        std::cout << "  - Emulation Speed: " << mhz << " MHz (PS1 Target: 33.87 MHz)\n";
        std::cout << "[PASS] Performance Benchmark self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during Performance test: " << e.what() << "\n";
        return 1;
    }
}

static int RunPortalSelfTest() {
    std::cout << "[TEST] PersonaRM Portal & BYOC Launcher self-test...\n";
    try {
        personarm::Portal portal;
        if (!portal.Initialize()) {
            std::cerr << "[FAIL] Portal failed to initialize.\n";
            return 1;
        }

        // Verify default active disc slot
        if (portal.GetActiveDiscNumber() != 1 && portal.GetActiveDiscNumber() != 2) {
            std::cerr << "[FAIL] Invalid default active disc number.\n";
            return 1;
        }

        portal.SetActiveDiscNumber(2);
        if (portal.GetActiveDiscNumber() != 2) {
            std::cerr << "[FAIL] Failed to switch active disc to 2.\n";
            return 1;
        }

        portal.SetActiveDiscNumber(1);
        if (portal.GetActiveDiscNumber() != 1) {
            std::cerr << "[FAIL] Failed to switch active disc back to 1.\n";
            return 1;
        }

        // Verify synthetic disc inspection
        auto slot = personarm::Portal::InspectDisc("nonexistent.cue");
        if (slot.verified) {
            std::cerr << "[FAIL] Nonexistent disc reported as verified.\n";
            return 1;
        }

        // Verify auto-scan runs cleanly without exceptions
        auto scanned = portal.AutoScanDiscs({ "." });
        (void)scanned;

        std::cout << "[PASS] PersonaRM Portal self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Exception during Portal test: " << e.what() << "\n";
        return 1;
    }
}

// ── Boot harness (gate G1 attempt, RECOMPILATION_STRATEGY) ───────────────────
// Milestone PCs from docs/research/OVERLAY_MAP.md / FUNCTION_CATALOG.md (disc 1 confirmed;
// disc 2 only the loader entry points are known, 0 = not instrumented).
struct BootMilestones { uint32_t main, scene_loop, load_file, cd_read_start, scene_mode, scene_req; };

struct BootOptions {
    uint64_t max_frames = 600;
    std::string dump_ram, dump_vram, dump_frames, dump_audio;
    std::string dump_ram_at_loads; // dir: RAM image at each load_file_by_id return (G1 reference comparison)
    uint64_t every = 60;
    bool stop_at_first_load = false;
    std::vector<personarm::Engine::PadPress> presses;
    std::vector<uint32_t> watch_pcs; // --watch-pc: log the first hits of these PCs (debugging aid)
};

// Overlay slots (OVERLAY_MAP OV-010, discs 1 and 2): code loaded above the resident EXE text.
static bool InOverlaySlot(uint32_t pc) {
    return (pc >= 0x80093000 && pc < 0x80160000) || (pc >= 0x80189000 && pc < 0x801CE800);
}

static bool WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint32_t>& rgba) {
    const bool ok = personarm::PngCodec::EncodeToFile(path, w, h, reinterpret_cast<const uint8_t*>(rgba.data()));
    if (!ok) std::cerr << "[BOOT] cannot write " << path << "\n";
    return ok;
}

// Display area as RGBA bytes. Unlike Ps1Gpu::OutputFrameToRGBA this honours 24bpp mode (FMV frames):
// each row holds width*3 bytes packed into 16-bit VRAM words (psx-spx "GPU Display Control").
static void DisplayToRgba(const personarm::Ps1Gpu& gpu, std::vector<uint32_t>& out, uint32_t& w, uint32_t& h) {
    const auto& d = gpu.GetDisplayArea();
    if (!d.is_24bit) {
        gpu.OutputFrameToRGBA(out, w, h);
        return;
    }
    w = d.width;
    h = d.height;
    out.assign(size_t(w) * h, 0xFF000000u);
    if ((gpu.ReadGPUSTAT() >> 23) & 1) return; // display disabled
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t vy = (d.y + y) & (personarm::PS1_VRAM_HEIGHT - 1);
        auto byte_at = [&](uint32_t i) {
            const uint16_t px = gpu.ReadVram((d.x + i / 2) & (personarm::PS1_VRAM_WIDTH - 1), vy);
            return uint32_t(i & 1 ? px >> 8 : px & 0xFF);
        };
        for (uint32_t x = 0; x < w; ++x)
            out[size_t(y) * w + x] = 0xFF000000u | byte_at(3 * x) | byte_at(3 * x + 1) << 8 | byte_at(3 * x + 2) << 16;
    }
}

// 16-bit stereo 44.1 kHz PCM WAV (RIFF, little-endian host assumed).
static bool WriteWav(const std::string& path, const std::vector<int16_t>& pcm) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
    if (!f) std::cerr << "[BOOT] cannot write " << path << "\n";
    return static_cast<bool>(f);
}

static int RunBootHarness(const std::string& disc_path, const BootOptions& opt) {
    using personarm::Engine;
    auto& engine = Engine::Instance();
    personarm::EngineBootConfig config;
    config.enable_bios_hle = true;
    engine.Initialize(config);
    if (!engine.BootFromDisc(disc_path)) {
        std::cerr << "[BOOT] cannot boot " << disc_path << "\n";
        return EXIT_FAILURE;
    }
    engine.SetPadScript(opt.presses);
    personarm::DiscReader probe;
    probe.Open(disc_path);
    const auto disc = personarm::DiscReader::IdentifyPersona2(probe.DetectBootExecutable());
    const BootMilestones ms = disc.disc_number == 2
        ? BootMilestones{0, 0, 0x800254E0, 0x8002541C, 0, 0x8007A80C}
        : BootMilestones{0x80024760, 0x80023590, 0x800272D0, 0x8002724C, 0x8007E298, 0x8007E2AC};
    auto hex = [](uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return std::string(b); };
    auto stamp = [&]() {
        return "[BOOT f=" + std::to_string(engine.GetFrameCount()) + " cyc=" +
               std::to_string(engine.GetCycles()) + "] ";
    };
    std::cout << stamp() << "disc " << (disc.recognized ? disc.product_id : std::string("unknown"))
              << " entry pc=" << hex(engine.GetCpu().GetPC()) << " sp=" << hex(engine.GetCpu().GetGPR(29)) << "\n";

    struct PendingLoad { uint32_t ra, sp, id; };
    std::vector<PendingLoad> pending;
    std::set<uint32_t> entries_seen; // overlay entry targets since the last load
    bool seen_main = false, seen_loop = false, done = false;
    uint32_t loads = 0, reads = 0, prev_pc = 0;
    std::vector<uint32_t> watch_hits(opt.watch_pcs.size());
    engine.SetStepHook([&](personarm::CpuContext& cpu) {
        const uint32_t pc = cpu.GetPC(), last = prev_pc;
        prev_pc = pc;
        if (!pending.empty() && pc == pending.back().ra && cpu.GetGPR(29) >= pending.back().sp) {
            std::cout << stamp() << "load_file_by_id(" << pending.back().id << ") returned v0=" << hex(cpu.GetGPR(2)) << "\n";
            if (!opt.dump_ram_at_loads.empty()) { // keep it out of the repo
                std::ofstream f(opt.dump_ram_at_loads + "/load_" + std::to_string(loads) + "_F" +
                                std::to_string(pending.back().id) + ".bin", std::ios::binary);
                f.write(reinterpret_cast<const char*>(engine.GetRam().data()), static_cast<std::streamsize>(engine.GetRam().size()));
            }
            pending.pop_back();
            if (opt.stop_at_first_load) { done = true; return false; }
        }
        for (size_t w = 0; w < opt.watch_pcs.size(); ++w)
            if (pc == opt.watch_pcs[w] && watch_hits[w]++ < 20)
                std::cout << stamp() << "watch " << hex(pc) << " ra=" << hex(cpu.GetGPR(31)) << " a0=" << hex(cpu.GetGPR(4))
                          << " a1=" << hex(cpu.GetGPR(5)) << " v0=" << hex(cpu.GetGPR(2)) << "\n";
        if (pc == ms.main && !seen_main) { seen_main = true; std::cout << stamp() << "reached main " << hex(pc) << "\n"; }
        if (pc == ms.scene_loop && !seen_loop) { seen_loop = true; std::cout << stamp() << "reached game_scene_loop " << hex(pc) << "\n"; }
        if (pc == ms.load_file) {
            const uint32_t a0 = cpu.GetGPR(4), a1 = cpu.GetGPR(5), ra = cpu.GetGPR(31);
            std::cout << stamp() << "load_file_by_id(id=" << a0 << " / F" << a0 << ", dest=" << hex(a1)
                      << ") ra=" << hex(ra) << "\n";
            if (auto* t = engine.GetTraceSink()) t->OnLoadFile(a0, a1, ra);
            pending.push_back({ra, cpu.GetGPR(29), a0});
            entries_seen.clear();
            ++loads;
        }
        if (pc == ms.cd_read_start) {
            const uint32_t lba = cpu.GetGPR(4), size = cpu.GetGPR(5), dest = cpu.GetGPR(6);
            std::cout << stamp() << "cd_read_start(lba=" << lba << ", size=" << size << ", dest=" << hex(dest)
                      << ") ra=" << hex(cpu.GetGPR(31)) << "\n";
            if (auto* t = engine.GetTraceSink()) t->OnCdRead(lba, (size + 2047) / 2048, dest);
            entries_seen.clear();
            ++reads;
        }
        // Call from resident code into an overlay slot: the previous instruction was the delay slot
        // of the jal/jalr, so ra = delay slot + 4.
        if (InOverlaySlot(pc) && !InOverlaySlot(last) && cpu.GetGPR(31) == last + 4 && entries_seen.insert(pc).second)
            std::cout << stamp() << "overlay entry " << hex(pc) << " from " << hex(last - 4) << "\n";
        return true;
    });
    bool seen_kon = false;
    engine.SetSpuKeyOnHook([&](uint32_t mask) {
        if (!seen_kon) { seen_kon = true; std::cout << stamp() << "first SPU key-on mask=" << hex(mask) << "\n"; }
    });

    bool seen_poly = false, seen_image = false;
    uint64_t last_calls = 0;
    uint32_t scene_mode = 0xDEADBEEF, scene_req = 0xDEADBEEF;
    personarm::GpuDisplayArea disp{};
    uint32_t disp_off = 2;
    std::set<uint32_t> disp_starts;
    std::vector<uint32_t> frame;
    uint32_t fw = 0, fh = 0;
    std::vector<int16_t> audio;
    bool seen_xa = false;
    while (engine.GetFrameCount() < opt.max_frames && engine.GetState() == personarm::EngineState::Running) {
        engine.RunFrame();
        {
            const std::vector<int16_t> xa = engine.TakeXaAudio();
            if (!xa.empty() && !seen_xa) seen_xa = true, std::cout << stamp() << "first XA audio sector\n";
            if (!opt.dump_audio.empty()) audio.insert(audio.end(), xa.begin(), xa.end());
        }
        auto& gpu = engine.GetGpu();
        if (!seen_poly && gpu.GetPolygonCount()) {
            seen_poly = true;
            std::cout << stamp() << "first GP0 polygon drawn\n";
        }
        if (ms.scene_mode) {
            const uint32_t m = engine.Read(ms.scene_mode, 4);
            if (m != scene_mode) { std::cout << stamp() << "scene mode " << hex(scene_mode) << " -> " << hex(m) << "\n"; scene_mode = m; }
        }
        const uint32_t rq = engine.Read(ms.scene_req, 4);
        if (rq != scene_req) {
            std::cout << stamp() << "scene request id " << hex(scene_req) << " -> " << hex(rq)
                      << " entry=" << hex(engine.Read(ms.scene_req + 4, 4)) << "\n";
            scene_req = rq;
        }
        const auto& d = gpu.GetDisplayArea();
        const uint32_t off = (gpu.ReadGPUSTAT() >> 23) & 1;
        // Double buffering flips the start every frame: log mode changes and start positions not seen in this mode.
        const bool mode_changed = off != disp_off || d.width != disp.width || d.height != disp.height ||
                                  d.is_24bit != disp.is_24bit || d.is_interlaced != disp.is_interlaced || d.is_pal != disp.is_pal;
        if (mode_changed) disp_starts.clear();
        if (disp_starts.insert(d.x | uint32_t(d.y) << 16).second) {
            std::cout << stamp() << "display " << (off ? "off" : "on") << " start=(" << d.x << "," << d.y << ") "
                      << d.width << "x" << d.height << (d.is_24bit ? " 24bpp" : " 15bpp")
                      << (d.is_interlaced ? " interlaced" : "") << (d.is_pal ? " PAL" : " NTSC") << "\n";
        }
        disp = d;
        disp_off = off;
        const bool want_dump = !opt.dump_frames.empty() && opt.every && engine.GetFrameCount() % opt.every == 0;
        if (!seen_image || want_dump) {
            DisplayToRgba(gpu, frame, fw, fh);
            if (!seen_image && !off && std::any_of(frame.begin(), frame.end(), [](uint32_t p) { return (p & 0xFFFFFFu) != 0; })) {
                seen_image = true;
                std::cout << stamp() << "first non-black display area (" << fw << "x" << fh << ")\n";
            }
            if (want_dump) {
                char name[32];
                std::snprintf(name, sizeof name, "/frame_%06llu.png", static_cast<unsigned long long>(engine.GetFrameCount()));
                WritePng(opt.dump_frames + name, fw, fh, frame);
            }
        }
        const auto& r = engine.GetCpu().GetRegisters();
        const uint64_t calls = engine.GetBios().GetInterceptedCallCount();
        if (engine.GetFrameCount() % 60 == 0 || engine.GetState() != personarm::EngineState::Running) {
            std::cout << stamp() << "pc=" << hex(r.pc) << " ra=" << hex(r.gpr[31]) << " sr=" << hex(r.cp0_status)
                      << " istat=" << hex(engine.Read(0x1F801070, 4)) << " imask=" << hex(engine.Read(0x1F801074, 4))
                      << " bios_calls=" << calls << " (+" << (calls - last_calls) << ") polys=" << gpu.GetPolygonCount() << "\n";
            last_calls = calls;
        }
    }
    engine.SetStepHook(nullptr);
    engine.SetSpuKeyOnHook(nullptr);
    if (!opt.dump_ram.empty()) { // debugging aid: raw 2 MB RAM image at stop (keep it out of the repo)
        std::ofstream f(opt.dump_ram, std::ios::binary);
        f.write(reinterpret_cast<const char*>(engine.GetRam().data()), static_cast<std::streamsize>(engine.GetRam().size()));
    }
    if (!opt.dump_audio.empty()) WriteWav(opt.dump_audio, audio);  // keep it out of the repo
    if (!opt.dump_vram.empty()) { // whole 1024x512 VRAM as 15-bit colour (keep it out of the repo)
        const auto& vram = engine.GetGpu().GetVram();
        std::vector<uint32_t> rgba(vram.size());
        for (size_t i = 0; i < vram.size(); ++i) {
            const uint32_t p = vram[i];
            rgba[i] = 0xFF000000u | ((p >> 10 & 0x1Fu) << 19) | ((p >> 5 & 0x1Fu) << 11) | ((p & 0x1Fu) << 3);
        }
        WritePng(opt.dump_vram, personarm::PS1_VRAM_WIDTH, personarm::PS1_VRAM_HEIGHT, rgba);
    }
    std::string unimpl;
    for (uint32_t k : engine.GetBios().GetUnimplementedCalls()) {
        char b[16];
        std::snprintf(b, sizeof b, " %c%02X", (k >> 8) == 0xA0 ? 'A' : (k >> 8) == 0xB0 ? 'B' : 'C', k & 0xFFu);
        unimpl += b;
    }
    std::cout << stamp() << (done ? "STOP: first load_file_by_id completed" : "STOP: frame limit or engine stopped")
              << "; main=" << seen_main << " scene_loop=" << seen_loop << " load_file calls=" << loads
              << " cd_read_start calls=" << reads << " polygon=" << seen_poly << " image=" << seen_image
              << " key-on=" << seen_kon << " bios_calls=" << engine.GetBios().GetInterceptedCallCount()
              << " unimplemented:" << (unimpl.empty() ? " none" : unimpl) << "\n";
    engine.Shutdown();
    return done || !opt.stop_at_first_load ? EXIT_SUCCESS : 2;
}

int main(int argc, char* argv[]) {
    personarm::Launcher launcher;
    if (!launcher.Initialize()) {
        std::cerr << "Failed to initialize PersonaRM Launcher.\n";
        return EXIT_FAILURE;
    }

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--version") == 0) {
                PrintVersion();
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[i], "--help") == 0) {
                PrintUsage(argv[0]);
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[i], "--trace-out") == 0) {
                if (i + 1 >= argc) {
                    std::cerr << "--trace-out needs a file path\n";
                    return EXIT_FAILURE;
                }
                personarm::Engine::Instance().SetTraceOutput(argv[++i]); // modifier: keep parsing
                continue;
            }
            if (std::strcmp(argv[i], "--boot") == 0) {
                if (i + 1 >= argc) {
                    std::cerr << "--boot needs a disc image (.cue/.bin/.iso)\n";
                    return EXIT_FAILURE;
                }
                std::string disc = argv[++i];
                BootOptions opt;
                for (int k = 1; k < argc; ++k) {
                    if (std::strcmp(argv[k], "--stop-at-first-load") == 0) opt.stop_at_first_load = true;
                    if (k + 1 >= argc) continue;
                    if (std::strcmp(argv[k], "--max-frames") == 0) opt.max_frames = std::strtoull(argv[k + 1], nullptr, 10);
                    if (std::strcmp(argv[k], "--every") == 0) opt.every = std::strtoull(argv[k + 1], nullptr, 10);
                    if (std::strcmp(argv[k], "--dump-ram") == 0) opt.dump_ram = argv[k + 1];
                    if (std::strcmp(argv[k], "--dump-ram-at-loads") == 0) opt.dump_ram_at_loads = argv[k + 1];
                    if (std::strcmp(argv[k], "--dump-vram") == 0) opt.dump_vram = argv[k + 1];
                    if (std::strcmp(argv[k], "--dump-frames") == 0) opt.dump_frames = argv[k + 1];
                    if (std::strcmp(argv[k], "--dump-audio") == 0) opt.dump_audio = argv[k + 1];
                    if (std::strcmp(argv[k], "--watch-pc") == 0) opt.watch_pcs.push_back(std::strtoul(argv[k + 1], nullptr, 16));
                    if (std::strcmp(argv[k], "--trace-out") == 0)
                        personarm::Engine::Instance().SetTraceOutput(argv[k + 1]);
                    if (std::strcmp(argv[k], "--press") == 0) {
                        personarm::Engine::PadPress p{};
                        if (!personarm::Engine::ParsePadPress(argv[k + 1], p)) {
                            std::cerr << "--press needs <button>@<frame>[+<frames>] (button: up down left right "
                                         "cross circle square triangle l1 r1 l2 r2 start select)\n";
                            return EXIT_FAILURE;
                        }
                        opt.presses.push_back(p);
                    }
                }
                return RunBootHarness(disc, opt);
            }
            if (std::strcmp(argv[i], "--stop-at-first-load") == 0) continue; // read by --boot
            if (std::strcmp(argv[i], "--max-frames") == 0 || std::strcmp(argv[i], "--dump-ram") == 0 ||
                std::strcmp(argv[i], "--dump-vram") == 0 || std::strcmp(argv[i], "--dump-frames") == 0 || std::strcmp(argv[i], "--dump-audio") == 0 ||
                std::strcmp(argv[i], "--every") == 0 || std::strcmp(argv[i], "--press") == 0 ||
                std::strcmp(argv[i], "--watch-pc") == 0) { ++i; continue; } // read by --boot
            if (std::strcmp(argv[i], "--launch") == 0) {
                return launcher.LaunchGame();
            }
            if (std::strcmp(argv[i], "--config") == 0) {
                launcher.RunInteractiveMenu();
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[i], "--scan") == 0) {
                std::string dir = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : ".";
                std::cout << "Scanning for game images in '" << dir << "'...\n";
                auto res = launcher.ScanForGameBinaries(dir);
                std::cout << "Found " << res.size() << " candidate(s):\n";
                for (const auto& f : res) std::cout << "  - " << f << "\n";
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[i], "--diagnostics") == 0) {
                return launcher.RunDiagnostics();
            }
            if (std::strcmp(argv[i], "--test-memory") == 0) {
                return RunMemorySelfTest();
            }
            if (std::strcmp(argv[i], "--test-input") == 0) {
                return RunInputSelfTest();
            }
            if (std::strcmp(argv[i], "--test-load") == 0) {
                return RunLoaderSelfTest();
            }
            if (std::strcmp(argv[i], "--test-tim") == 0) {
                return RunTimSelfTest();
            }
            if (std::strcmp(argv[i], "--test-vag") == 0) {
                return RunVagSelfTest();
            }
            if (std::strcmp(argv[i], "--test-gpu") == 0) {
                return RunGpuSelfTest();
            }
            if (std::strcmp(argv[i], "--test-bios") == 0) {
                return RunBiosSelfTest();
            }
            if (std::strcmp(argv[i], "--test-gte") == 0) {
                return RunGteSelfTest();
            }
            if (std::strcmp(argv[i], "--test-exe") == 0) {
                return RunExeSelfTest();
            }
            if (std::strcmp(argv[i], "--test-spu") == 0) {
                return RunSpuSelfTest();
            }
            if (std::strcmp(argv[i], "--test-str") == 0) {
                return RunStrSelfTest();
            }
            if (std::strcmp(argv[i], "--test-engine") == 0) {
                return RunEngineSelfTest();
            }
            if (std::strcmp(argv[i], "--test-window") == 0) {
                return RunWindowSelfTest();
            }
            if (std::strcmp(argv[i], "--test-mdec") == 0) {
                return RunMdecSelfTest();
            }
            if (std::strcmp(argv[i], "--test-renderer") == 0) {
                return RunRendererSelfTest();
            }
            if (std::strcmp(argv[i], "--test-audio") == 0) {
                return RunAudioSelfTest();
            }
            if (std::strcmp(argv[i], "--test-save") == 0) {
                return RunSaveSelfTest();
            }
            if (std::strcmp(argv[i], "--portal") == 0) {
                personarm::Portal portal;
                portal.Initialize();
                portal.RunInteractiveMenu();
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[i], "--disc1") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    personarm::Portal portal;
                    portal.Initialize();
                    portal.LoadDisc(1, argv[++i]);
                    std::cout << "[Portal] Linked Disc 1: " << argv[i] << "\n";
                    return EXIT_SUCCESS;
                }
            }
            if (std::strcmp(argv[i], "--disc2") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    personarm::Portal portal;
                    portal.Initialize();
                    portal.LoadDisc(2, argv[++i]);
                    std::cout << "[Portal] Linked Disc 2: " << argv[i] << "\n";
                    return EXIT_SUCCESS;
                }
            }
            if (std::strcmp(argv[i], "--swap-disc") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    int disc_num = std::atoi(argv[++i]);
                    personarm::Portal portal;
                    portal.Initialize();
                    portal.SetActiveDiscNumber(disc_num);
                    std::cout << "[Portal] Set active disc to: Disc " << disc_num << "\n";
                    return EXIT_SUCCESS;
                }
            }
            if (std::strcmp(argv[i], "--verify-disc") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    std::string path = argv[++i];
                    auto slot = personarm::Portal::InspectDisc(path);
                    std::cout << "Disc Inspection: " << path << "\n";
                    std::cout << "  - Verified: " << (slot.verified ? "YES" : "NO") << "\n";
                    std::cout << "  - Boot Executable: " << (slot.boot_exe.empty() ? "N/A" : slot.boot_exe) << "\n";
                    std::cout << "  - Volume Label: " << (slot.volume_label.empty() ? "N/A" : slot.volume_label) << "\n";
                    std::cout << "  - Files Count: " << slot.file_count << "\n";
                    if (slot.info.recognized) {
                        std::cout << "  - Product ID: " << slot.info.product_id << "\n";
                        std::cout << "  - Region: " << slot.info.region << "\n";
                        std::cout << "  - Edition: " << slot.info.edition << "\n";
                        std::cout << "  - Disc Number: " << slot.info.disc_number << " of " << slot.info.disc_count << "\n";
                    }
                    return slot.verified ? EXIT_SUCCESS : EXIT_FAILURE;
                }
            }
            if (std::strcmp(argv[i], "--test-portal") == 0) {
                return RunPortalSelfTest();
            }
            if (std::strcmp(argv[i], "--test-performance") == 0) {
                return RunPerformanceSelfTest();
            }

            std::cerr << "Unknown option: " << argv[i] << "\n";
            PrintUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Default interactive mode: show launcher hub
    launcher.RunInteractiveMenu();
    return launcher.LaunchGame();
}
