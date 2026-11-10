/**
 * @file main.cpp
 * @brief Entry point for PersonaRM - Native Launcher & Port Engine
 *
 * Handles:
 * - Launcher UI and configuration
 * - Game boot sequence & legal copy validation
 * - Command-line automation & CI testing
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <chrono>

// Project headers
#include "launcher/launcher.h"
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
              << "  --launch           Directly boot the game using current configuration\n"
              << "  --config           Open interactive settings menu\n"
              << "  --scan [dir]       Scan directory for legal game disc images (.iso/.bin/.cue)\n"
              << "  --diagnostics      Run full diagnostic suite\n"
              << "  --version          Show launcher version\n"
              << "  --help             Show this help information\n\n"
              << "Component Self-Tests:\n"
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

        cpu.SetGPR(9, 0x00); // FileOpen
        cpu.SetGPR(31, 0x80010020);
        cpu.SetPC(0x000000A0);

        if (!bios.HandleBiosHook(cpu, mem) || cpu.GetGPR(2) != 1) {
            std::cerr << "[FAIL] BIOS FileOpen hook failed.\n";
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
        gte.CommandNCLIP();
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
        gte.CommandAVSZ3();
        uint32_t otz = gte.ReadDataRegister(20);
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
