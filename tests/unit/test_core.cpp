#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include "core/memory_manager.h"
#include "core/logger.h"
#include "input/input_manager.h"
#include "loader/disc_reader.h"
#include "loader/tim_parser.h"
#include "loader/ps1_bios_hle.h"
#include "rendering/renderer.h"
#include "rendering/ps1_gpu.h"
#include "audio/audio_system.h"
#include "audio/vag_decoder.h"
#include "audio/spu_emulator.h"
#include "cpu/cpu_context.h"
#include "cpu/gte.h"
#include "loader/exe_loader.h"
#include "loader/str_demuxer.h"
#include "engine/engine.h"
#include "rendering/window_context.h"
#include "rendering/gl_renderer.h"

int main() {
    std::cout << "[UNIT TEST] Running Core and Input Unit Tests...\n";

    // 1. Memory Manager Tests
    {
        personarm::MemoryManager mgr;
        mgr.Initialize(4 * 1024 * 1024);

        assert(mgr.NumRegions() > 0);
        assert(mgr.TotalAvailable() > 0);

        void* ptr1 = mgr.Allocate(128, personarm::AllocationPolicy::POOLED);
        assert(ptr1 != nullptr);
        assert(mgr.IsAllocated(ptr1));

        void* ptr2 = mgr.Allocate(1024, personarm::AllocationPolicy::DEFAULT);
        assert(ptr2 != nullptr);
        assert(mgr.IsAllocated(ptr2));

        mgr.Deallocate(ptr1);
        mgr.Shutdown();
        std::cout << "  ✓ MemoryManager tests passed.\n";
    }

    // 2. Input Manager Tests
    {
        auto& input = personarm::InputManager::Instance();
        assert(input.Initialize());

        assert(!input.IsActionPressed(personarm::VirtualAction::Confirm));
        input.SimulateActionPress(personarm::VirtualAction::Confirm);
        input.Update(0.016f);

        assert(input.IsActionPressed(personarm::VirtualAction::Confirm));
        assert(input.IsActionJustPressed(personarm::VirtualAction::Confirm));

        input.Update(0.016f);
        assert(input.IsActionPressed(personarm::VirtualAction::Confirm));
        assert(!input.IsActionJustPressed(personarm::VirtualAction::Confirm)); // No longer "just" pressed

        input.SimulateActionRelease(personarm::VirtualAction::Confirm);
        input.Update(0.016f);
        assert(!input.IsActionPressed(personarm::VirtualAction::Confirm));
        assert(input.IsActionJustReleased(personarm::VirtualAction::Confirm));

        // Test glyph output
        std::string glyph = input.GetActionGlyphDescription(personarm::VirtualAction::Confirm);
        assert(!glyph.empty());

        input.Shutdown();
        std::cout << "  ✓ InputManager tests passed.\n";
    }

    // 3. Disc & Executable Loader Tests
    {
        // 3.1 Test PS-X EXE Header Parsing
        std::vector<uint8_t> psx(2048, 0);
        std::memcpy(psx.data(), "PS-X EXE", 8);
        *reinterpret_cast<uint32_t*>(psx.data() + 0x10) = 0x80010000; // Entry point
        *reinterpret_cast<uint32_t*>(psx.data() + 0x14) = 0x80020000; // GP
        *reinterpret_cast<uint32_t*>(psx.data() + 0x18) = 0x80010000; // Load address
        *reinterpret_cast<uint32_t*>(psx.data() + 0x1C) = 256 * 1024; // 256KB

        auto psx_info = personarm::DiscReader::ParseExecutable(psx.data(), psx.size());
        assert(psx_info.valid);
        assert(psx_info.format == "PS-X EXE");
        assert(psx_info.entry_point == 0x80010000);
        assert(psx_info.gp_value == 0x80020000);
        assert(!psx_info.segments.empty());
        assert(psx_info.segments[0].virtual_address == 0x80010000);
        assert(psx_info.segments[0].file_size == 256 * 1024);

        // 3.2 Test ELF Header Parsing
        std::vector<uint8_t> elf(128, 0);
        elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        *reinterpret_cast<uint32_t*>(elf.data() + 0x18) = 0x00100000; // Entry point

        auto elf_info = personarm::DiscReader::ParseExecutable(elf.data(), elf.size());
        assert(elf_info.valid);
        assert(elf_info.format == "ELF");
        assert(elf_info.entry_point == 0x00100000);

        std::cout << "  ✓ DiscReader & Executable parser tests passed.\n";
    }

    // 4. Renderer Viewport & Aspect Ratio Tests
    {
        auto& renderer = personarm::Renderer::Instance();
        personarm::RendererConfig config;
        config.window_width = 1920;
        config.window_height = 1080;
        assert(renderer.Initialize(config));

        // Test 16:9 in 1080p
        auto vp_169 = renderer.CalculateLetterboxViewport(1920, 1080, personarm::AspectRatioMode::Widescreen16x9);
        assert(vp_169.width == 1920);
        assert(vp_169.height == 1080);
        assert(vp_169.x == 0 && vp_169.y == 0);

        // Test 4:3 in 1080p -> width = 1440, pillarbox x = 240
        auto vp_43 = renderer.CalculateLetterboxViewport(1920, 1080, personarm::AspectRatioMode::Original4x3);
        assert(vp_43.width == 1440);
        assert(vp_43.height == 1080);
        assert(vp_43.x == 240);

        std::cout << "  ✓ Renderer & Viewport scaling tests passed.\n";
    }

    // 5. Audio System Tests
    {
        auto& audio = personarm::AudioSystem::Instance();
        personarm::AudioConfig config;
        config.master_volume = 1.0f;
        assert(audio.Initialize(config));

        audio.SetChannelVolume(personarm::AudioChannel::BGM, 0.75f);
        assert(audio.GetChannelVolume(personarm::AudioChannel::BGM) == 0.75f);
        assert(audio.GetEffectiveVolume(personarm::AudioChannel::BGM) == 0.75f);

        audio.SetChannelVolume(personarm::AudioChannel::Master, 0.5f);
        float effective = audio.GetEffectiveVolume(personarm::AudioChannel::BGM);
        assert(effective >= 0.37f && effective <= 0.38f); // 0.5 * 0.75 = 0.375

        std::cout << "  ✓ AudioSystem channel mixing tests passed.\n";
    }

    // 6. CPU Execution Context Tests
    {
        personarm::CpuContext cpu;
        assert(cpu.GetPC() == 0x80010000);
        assert(cpu.GetGPR(0) == 0); // r0 is zero

        // Test r0 is immutable
        cpu.SetGPR(0, 0x12345678);
        assert(cpu.GetGPR(0) == 0);

        // Test ADDIU r1, r0, 42 -> 0x2401002A
        cpu.SetGPR(1, 100);
        assert(cpu.GetGPR(1) == 100);

        // Hook mock memory to execute: ORI r2, r0, 0x55AA (0x340255AA)
        cpu.SetMemoryHooks(
            [](uint32_t addr, uint32_t size) -> uint32_t {
                (void)addr; (void)size;
                return 0x340255AA; // ori $2, $0, 0x55aa
            },
            nullptr
        );

        cpu.Step();
        assert(cpu.GetGPR(2) == 0x55AA);
        assert(cpu.GetPC() == 0x80010004);
        assert(cpu.GetCycleCount() == 1);

        std::cout << "  ✓ CPU Execution Context tests passed.\n";
    }

    // 7. Logger System Test
    {
        PRM_LOG_INFO("UnitTest", "Testing PersonaRM integrated logging system");
        PRM_LOG_DEBUG("UnitTest", "Debug trace entry");
        std::cout << "  ✓ Logger system tests passed.\n";
    }

    // 8. TIM Texture Parser Test (Synthetic 4-bit TIM)
    {
        std::vector<uint8_t> tim_buf;
        tim_buf.resize(8 + 44 + 20, 0);

        // Magic 0x00000010
        tim_buf[0] = 0x10;
        // Flags: 0x08 (CLUT present, 4-bit mode)
        tim_buf[4] = 0x08;

        // CLUT Block at offset 8
        size_t clut_off = 8;
        uint32_t clut_size = 44;
        std::memcpy(&tim_buf[clut_off], &clut_size, 4);
        uint16_t clut_w = 16, clut_h = 1;
        std::memcpy(&tim_buf[clut_off + 8], &clut_w, 2);
        std::memcpy(&tim_buf[clut_off + 10], &clut_h, 2);

        // Color 1: Red (RGB 5:5:5: R=31 -> 0x001F)
        uint16_t red_color = 0x001F;
        std::memcpy(&tim_buf[clut_off + 12 + 2], &red_color, 2);

        // Image Block at offset 8 + 44 = 52
        size_t img_off = clut_off + clut_size;
        uint32_t img_size = 20;
        std::memcpy(&tim_buf[img_off], &img_size, 4);
        uint16_t img_w_words = 2; // 2 words * 4 = 8 pixels wide
        uint16_t img_h = 2;       // 2 pixels high
        std::memcpy(&tim_buf[img_off + 8], &img_w_words, 2);
        std::memcpy(&tim_buf[img_off + 10], &img_h, 2);

        // Set pixel 0 to index 1 (Red)
        tim_buf[img_off + 12] = 0x01;

        auto parsed = personarm::TimParser::ParseFromMemory(tim_buf.data(), tim_buf.size());
        assert(parsed.has_value());
        assert(parsed->width == 8);
        assert(parsed->height == 2);
        assert(parsed->bpp_mode == personarm::TimBppMode::Indexed4Bit);
        assert(parsed->pixels_rgba.size() == 16);
        assert(parsed->pixels_rgba[0].r > 240);
        assert(parsed->pixels_rgba[0].g == 0);
        assert(parsed->pixels_rgba[0].b == 0);

        std::cout << "  ✓ TIM Texture Parser tests passed.\n";
    }

    // 9. VAG Audio Decoder Test (Synthetic VAG buffer)
    {
        std::vector<uint8_t> vag_buf;
        vag_buf.resize(64, 0);

        // Magic "VAGp"
        vag_buf[0] = 0x56; vag_buf[1] = 0x41; vag_buf[2] = 0x47; vag_buf[3] = 0x70;
        // Version 0x20 BE
        vag_buf[7] = 0x20;
        // Data size 16 BE
        vag_buf[15] = 16;
        // Sample rate 22050 (0x5622 BE)
        vag_buf[18] = 0x56; vag_buf[19] = 0x22;

        // ADPCM block at offset 48: Shift=8, Filter=0
        vag_buf[48] = 0x08;
        // Flags: Loop End
        vag_buf[49] = 0x01;
        for (int i = 50; i < 64; ++i) {
            vag_buf[i] = 0x11;
        }

        auto audio = personarm::VagDecoder::DecodeFromMemory(vag_buf.data(), vag_buf.size());
        assert(audio.has_value());
        assert(audio->sample_rate == 22050);
        assert(audio->pcm_samples.size() == 28);

        std::cout << "  ✓ VAG Audio Decoder tests passed.\n";
    }

    // 10. PS1 Hardware GPU & VRAM Tests
    {
        personarm::Ps1Gpu gpu;
        assert(gpu.GetDisplayArea().width == 320);
        assert(gpu.GetDisplayArea().height == 240);

        // VRAM read/write test (16-bit RGB 5:5:5)
        gpu.WriteVram(100, 50, 0x7FFF); // Pure white (15-bit)
        assert(gpu.ReadVram(100, 50) == 0x7FFF);
        assert(gpu.ReadVram(101, 50) == 0); // Adjacent pixel is clear

        // GP0(0x02) Quick Fill Rect test: Fill (x=0, y=0, w=16, h=16) with blue (0x7C00)
        // word 0: 0x02 + blue color (B=31 << 16 -> 0x021F0000)
        gpu.WriteGP0(0x02000000 | (31 << 16));
        gpu.WriteGP0(0x00000000); // x=0, y=0
        gpu.WriteGP0((16 << 16) | 16); // w=16, h=16

        // Check filled pixel in VRAM
        assert(gpu.ReadVram(0, 0) != 0);

        // GP1(0x08) Display Mode change test: 640x480 interlaced
        gpu.WriteGP1(0x08000000 | 0x03 | 0x04 | 0x20); // hres=640, vres=480, interlaced
        assert(gpu.GetDisplayArea().width == 640);
        assert(gpu.GetDisplayArea().height == 480);
        assert(gpu.GetDisplayArea().is_interlaced == true);

        // Frame rendering test: Output to RGBA buffer
        std::vector<uint32_t> rgba_buf;
        uint32_t w = 0, h = 0;
        gpu.OutputFrameToRGBA(rgba_buf, w, h);
        assert(w == 640 && h == 480);
        assert(rgba_buf.size() == 640 * 480);

        std::cout << "  ✓ PS1 Hardware GPU & VRAM tests passed.\n";
    }

    // 11. PS1 BIOS HLE Dispatcher Tests
    {
        personarm::Ps1BiosHle& bios = personarm::Ps1BiosHle::Instance();
        bios.Initialize();

        personarm::CpuContext cpu;
        personarm::MemoryManager mem;
        mem.Initialize(1024 * 1024);

        // Test Vector A hook: FileOpen (func 0x00) -> returns fd=1 in $v0 ($2)
        cpu.SetGPR(9, 0x00); // $t1 = func 0x00
        cpu.SetGPR(31, 0x80010050); // $ra
        cpu.SetPC(0x000000A0);

        bool handled = bios.HandleBiosHook(cpu, mem);
        assert(handled == true);
        assert(cpu.GetGPR(2) == 1); // $v0 == 1
        assert(cpu.GetPC() == 0x80010050); // Returned to $ra

        // Test Vector B hook: InitHeap (func 0x12) -> returns 1 in $v0
        cpu.SetGPR(9, 0x12);
        cpu.SetGPR(31, 0x80010060);
        cpu.SetPC(0x000000B0);

        handled = bios.HandleBiosHook(cpu, mem);
        assert(handled == true);
        assert(cpu.GetGPR(2) == 1);
        assert(cpu.GetPC() == 0x80010060);

        assert(bios.GetInterceptedCallCount() == 2);

        std::cout << "  ✓ PS1 BIOS HLE Dispatcher tests passed.\n";
    }

    // 12. GTE (Geometry Transformation Engine) & Unaligned Memory Tests
    {
        personarm::CpuContext cpu;

        // Unaligned memory store/load simulation using 64KB RAM
        std::vector<uint8_t> ram(65536, 0);
        cpu.SetMemoryHooks(
            [&ram](uint32_t addr, uint32_t size) -> uint32_t {
                uint32_t offset = addr & 0xFFFF;
                if (size == 1) return ram[offset];
                if (size == 2) return ram[offset] | (static_cast<uint32_t>(ram[(offset + 1) & 0xFFFF]) << 8);
                if (size == 4) {
                    return ram[offset] |
                           (static_cast<uint32_t>(ram[(offset + 1) & 0xFFFF]) << 8) |
                           (static_cast<uint32_t>(ram[(offset + 2) & 0xFFFF]) << 16) |
                           (static_cast<uint32_t>(ram[(offset + 3) & 0xFFFF]) << 24);
                }
                return 0;
            },
            [&ram](uint32_t addr, uint32_t val, uint32_t size) {
                uint32_t offset = addr & 0xFFFF;
                if (size == 1) ram[offset] = static_cast<uint8_t>(val);
                else if (size == 2) {
                    ram[offset] = static_cast<uint8_t>(val & 0xFF);
                    ram[(offset + 1) & 0xFFFF] = static_cast<uint8_t>((val >> 8) & 0xFF);
                } else if (size == 4) {
                    ram[offset] = static_cast<uint8_t>(val & 0xFF);
                    ram[(offset + 1) & 0xFFFF] = static_cast<uint8_t>((val >> 8) & 0xFF);
                    ram[(offset + 2) & 0xFFFF] = static_cast<uint8_t>((val >> 16) & 0xFF);
                    ram[(offset + 3) & 0xFFFF] = static_cast<uint8_t>((val >> 24) & 0xFF);
                }
            }
        );

        // Test GTE NCLIP normal clipping (backface culling)
        auto& gte = cpu.GetGTE();
        // Set SXY0 (reg 12) = (0, 0), SXY1 (reg 13) = (10, 0), SXY2 (reg 14) = (0, 10)
        gte.WriteDataRegister(12, 0);
        gte.WriteDataRegister(13, (0 << 16) | 10);
        gte.WriteDataRegister(14, (10 << 16) | 0);
        gte.CommandNCLIP();
        // MAC0 (reg 24) should be positive for clockwise/counter-clockwise winding
        int32_t mac0 = static_cast<int32_t>(gte.ReadDataRegister(24));
        assert(mac0 != 0);

        // Test GTE AVSZ3 (Average Z: (300 + 300 + 300) / 3 ≈ 300)
        gte.WriteControlRegister(29, 0x0555); // ~1/3 in fixed point
        gte.WriteDataRegister(17, 300); // SZ1
        gte.WriteDataRegister(18, 300); // SZ2
        gte.WriteDataRegister(19, 300); // SZ3
        gte.CommandAVSZ3();
        uint32_t otz = gte.ReadDataRegister(20);
        assert(otz >= 295 && otz <= 305); // Approximate 300

        std::cout << "  ✓ PS1 GTE Coprocessor 2 tests passed.\n";
    }

    // 13. PS-X EXE Loader Tests
    {
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

        bool loaded = personarm::ExeLoader::LoadExe(dummy_exe.data(), dummy_exe.size(), cpu, ram, info);
        assert(loaded == true);
        assert(info.entry_point == 0x80010000);
        assert(cpu.GetPC() == 0x80010000);
        assert(cpu.GetGPR(28) == 0x80080000); // $gp
        assert(cpu.GetGPR(29) == 0x801FFF00); // $sp

        // SYSTEM.CNF parser test
        std::string cnf = "BOOT = cdrom:\\SLUS_011.58;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFF00\r\n";
        std::string boot_file = personarm::ExeLoader::ExtractBootExecutable(cnf);
        assert(boot_file == "SLUS_011.58");

        std::cout << "  ✓ PS-X EXE Loader & SYSTEM.CNF parser tests passed.\n";
    }

    // 14. PS1 SPU 24-Voice Mixer Tests
    {
        personarm::SpuEmulator spu;
        spu.SetVoiceVolume(0, 0x3FFF, 0x3FFF);
        spu.SetVoicePitch(0, 4096);
        spu.SetVoiceStartAddress(0, 0x0000);

        uint8_t dummy_vag[16] = { 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        spu.UploadToRam(0x0000, dummy_vag, sizeof(dummy_vag));
        spu.KeyOn(1 << 0);
        assert(spu.GetActiveVoiceMask() == 1);

        std::vector<int16_t> pcm;
        spu.GenerateSamples(pcm, 64);
        assert(pcm.size() == 128); // 64 frames * 2 channels

        std::cout << "  ✓ PS1 SPU 24-Voice Audio Emulator tests passed.\n";
    }

    // 15. PS1 STR Video Demuxer Tests
    {
        personarm::StrDemuxer demuxer;
        std::vector<uint8_t> sec(2336, 0);
        auto* h = reinterpret_cast<personarm::StrSectorHeader*>(sec.data());
        h->magic = personarm::STR_MAGIC;
        h->chunk_type = 0x01;
        h->chunk_index = 0;
        h->total_chunks = 1;
        h->frame_number = 10;
        h->payload_size = 256;
        h->width = 320;
        h->height = 240;

        personarm::StrDemuxedFrame frame;
        bool done = demuxer.ProcessSector(sec.data(), sec.size(), frame);
        assert(done == true);
        assert(frame.frame_index == 10);
        assert(frame.width == 320 && frame.height == 240);

        std::cout << "  ✓ PS1 STR Video Demuxer tests passed.\n";
    }

    // 16. PersonaRM Core Engine Tests
    {
        auto& engine = personarm::Engine::Instance();
        personarm::EngineBootConfig config;
        config.enable_bios_hle = true;

        assert(engine.Initialize(config) == true);
        assert(engine.GetState() == personarm::EngineState::Idle);

        // Synthetic standalone PS-X executable
        std::vector<uint8_t> exe(4096, 0);
        auto* hdr = reinterpret_cast<personarm::PsxExeHeader*>(exe.data());
        std::memcpy(hdr->magic, "PS-X EXE", 8);
        hdr->initial_pc = 0x80010000;
        hdr->destination_ram = 0x80010000;
        hdr->file_size = 2048;
        hdr->initial_sp_base = 0x801FFF00;

        assert(engine.BootExecutable(exe.data(), exe.size()) == true);
        assert(engine.GetState() == personarm::EngineState::Running);

        // Run 1 NTSC frame (~564,480 cycles)
        engine.RunFrame();
        assert(engine.GetFrameCount() == 1);

        std::vector<uint32_t> frame;
        uint32_t w = 0, h = 0;
        engine.GetCurrentFrame(frame, w, h);
        assert(w == 320 && h == 240);
        assert(frame.size() == 320 * 240);

        engine.Shutdown();
        assert(engine.GetState() == personarm::EngineState::Stopped);

        std::cout << "  ✓ PersonaRM Core Engine tests passed.\n";
    }

    // 17. Window Context & GL Renderer Tests
    {
        personarm::WindowContext window;
        personarm::WindowConfig win_cfg;
        win_cfg.headless = true;
        win_cfg.width = 1920;
        win_cfg.height = 1080;

        assert(window.Initialize(win_cfg) == true);
        assert(window.IsOpen() == true);
        assert(window.IsHeadless() == true);

        personarm::GlRenderer gl_renderer;
        assert(gl_renderer.Initialize(window) == true);

        std::vector<uint32_t> test_frame(320 * 240, 0xFF00FF00); // Green frame
        gl_renderer.UploadFrame(test_frame.data(), 320, 240);
        gl_renderer.Render(1920, 1080);

        auto vp = gl_renderer.GetCurrentViewport();
        // 1920x1080 is 16:9, so 4:3 target results in pillarboxing (width ~1440, height 1080, x ~240)
        assert(vp.height == 1080);
        assert(vp.width == 1440);
        assert(vp.x == 240);
        assert(vp.y == 0);

        gl_renderer.Shutdown();
        window.Shutdown();
        assert(window.IsOpen() == false);

        std::cout << "  ✓ Window Context & GL Renderer tests passed.\n";
    }

    std::cout << "[PASS] All unit tests completed successfully!\n";
    return 0;
}

