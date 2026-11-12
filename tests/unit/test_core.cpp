// Tests rely on assert(); keep them active in Release builds (CI uses Release).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <iostream>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>
#include "core/memory_manager.h"
#include "core/save_manager.h"
#include "core/logger.h"
#include "launcher/portal.h"
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
#include "rendering/mdec_decoder.h"

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

        // Test direct RAM memcpy & memset via Vector A
        std::vector<uint8_t> ram(1024 * 1024, 0);
        ram[0x1000] = 0xAA;
        ram[0x1001] = 0xBB;
        cpu.SetGPR(4, 0x80002000); // dst
        cpu.SetGPR(5, 0x80001000); // src
        cpu.SetGPR(6, 2);          // count
        cpu.SetGPR(9, 0x2A);       // memcpy func 0x2A
        cpu.SetGPR(31, 0x80010070);
        cpu.SetPC(0x000000A0);
        handled = bios.HandleBiosHook(cpu, ram.data(), ram.size());
        assert(handled == true);
        assert(ram[0x2000] == 0xAA);
        assert(ram[0x2001] == 0xBB);

        // Test memset func 0x2B
        cpu.SetGPR(4, 0x80003000); // dst
        cpu.SetGPR(5, 0x77);       // val
        cpu.SetGPR(6, 4);          // count
        cpu.SetGPR(9, 0x2B);       // memset func 0x2B
        cpu.SetGPR(31, 0x80010080);
        cpu.SetPC(0x000000A0);
        handled = bios.HandleBiosHook(cpu, ram.data(), ram.size());
        assert(handled == true);
        assert(ram[0x3000] == 0x77);
        assert(ram[0x3003] == 0x77);

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

    // 12b. Branch delay slot: the delay slot runs once, the instruction after it is skipped
    {
        personarm::CpuContext cpu;
        std::vector<uint32_t> code(0x1000 / 4 + 8, 0);
        const uint32_t base = 0x1000 / 4;
        code[base + 0] = 0x10000002; // beq r0,r0,+2  -> target = 0x1004 + 8 = 0x100C
        code[base + 1] = 0x24010001; // delay slot: addiu r1,r0,1
        code[base + 2] = 0x24020001; // skipped:    addiu r2,r0,1
        code[base + 3] = 0x24030001; // target:     addiu r3,r0,1
        cpu.SetMemoryHooks(
            [&code](uint32_t addr, uint32_t) -> uint32_t { return code[(addr & 0xFFFF) / 4]; },
            [](uint32_t, uint32_t, uint32_t) {});
        cpu.SetPC(0x1000);
        for (int i = 0; i < 3; ++i) cpu.Step();
        assert(cpu.GetGPR(1) == 1);
        assert(cpu.GetGPR(2) == 0);
        assert(cpu.GetGPR(3) == 1);
        std::cout << "  ✓ MIPS branch delay slot test passed.\n";
    }

    // 12d. Load delay: the instruction after lw still reads the OLD r1; r1 updates afterwards
    {
        personarm::CpuContext cpu;
        std::vector<uint32_t> code(0x10000 / 4, 0);
        code[0x1000 / 4] = 0x8C012000; // lw r1,0x2000(r0)
        code[0x1004 / 4] = 0x24220000; // addiu r2,r1,0  (must see old r1)
        code[0x2000 / 4] = 9;
        cpu.SetMemoryHooks(
            [&code](uint32_t addr, uint32_t) -> uint32_t { return code[(addr & 0xFFFF) / 4]; },
            [](uint32_t, uint32_t, uint32_t) {});
        cpu.SetGPR(1, 5);
        cpu.SetPC(0x1000);
        cpu.Step();
        cpu.Step();
        cpu.Step(); // nop
        assert(cpu.GetGPR(2) == 5);
        assert(cpu.GetGPR(1) == 9);
        std::cout << "  ✓ MIPS load delay test passed.\n";
    }

    // 12e. DIV/DIVU edge cases: never trap; values per psx-spx (verify against HW-005)
    {
        std::vector<uint32_t> code(0x10000 / 4, 0);
        const uint32_t kMfLo = 0x00001812, kMfHi = 0x00002010; // mflo r3 ; mfhi r4
        auto run = [&](uint32_t div_op, uint32_t n, uint32_t d, uint32_t& lo, uint32_t& hi) {
            personarm::CpuContext cpu;
            code[0x1000 / 4] = div_op;
            code[0x1004 / 4] = kMfLo;
            code[0x1008 / 4] = kMfHi;
            cpu.SetMemoryHooks(
                [&code](uint32_t addr, uint32_t) -> uint32_t { return code[(addr & 0xFFFF) / 4]; },
                [](uint32_t, uint32_t, uint32_t) {});
            cpu.SetGPR(1, n);
            cpu.SetGPR(2, d);
            cpu.SetPC(0x1000);
            for (int i = 0; i < 4; ++i) cpu.Step();
            lo = cpu.GetGPR(3);
            hi = cpu.GetGPR(4);
        };
        const uint32_t kDiv = 0x0022001A, kDivu = 0x0022001B;
        uint32_t lo = 0, hi = 0;
        run(kDiv, 7, 0, lo, hi);            assert(lo == 0xFFFFFFFFu && hi == 7);
        run(kDiv, 0xFFFFFFF9u, 0, lo, hi);  assert(lo == 1 && hi == 0xFFFFFFF9u); // -7 / 0
        run(kDiv, 0x80000000u, 0xFFFFFFFFu, lo, hi); assert(lo == 0x80000000u && hi == 0);
        run(kDivu, 7, 0, lo, hi);           assert(lo == 0xFFFFFFFFu && hi == 7);
        run(kDiv, 7, 2, lo, hi);            assert(lo == 3 && hi == 1);
        std::cout << "  ✓ MIPS DIV/DIVU edge case test passed.\n";
    }

    // 12c. LWL (little-endian): word 0xAABBCCDD at 0x2000, lwl r1,0x2001(r0) keeps low 16 bits of r1
    {
        personarm::CpuContext cpu;
        std::vector<uint32_t> code(0x10000 / 4, 0);
        code[0x1000 / 4] = 0x88012001; // lwl r1,0x2001(r0)
        code[0x2000 / 4] = 0xAABBCCDD;
        cpu.SetMemoryHooks(
            [&code](uint32_t addr, uint32_t) -> uint32_t { return code[(addr & 0xFFFF) / 4]; },
            [](uint32_t, uint32_t, uint32_t) {});
        cpu.SetGPR(1, 0x11223344);
        cpu.SetPC(0x1000);
        cpu.Step();
        cpu.Step(); // load delay: result lands at the next step
        assert(cpu.GetGPR(1) == 0xCCDD3344);
        std::cout << "  ✓ MIPS LWL test passed.\n";
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

        // Test 16:9 widescreen mode
        gl_renderer.SetAspectRatioMode(personarm::AspectRatioMode::Widescreen16x9);
        gl_renderer.Render(1920, 1080);
        vp = gl_renderer.GetCurrentViewport();
        assert(vp.width == 1920);
        assert(vp.height == 1080);
        assert(vp.x == 0);
        assert(vp.y == 0);

        // Test 21:9 ultrawide mode
        gl_renderer.SetAspectRatioMode(personarm::AspectRatioMode::Ultrawide21x9);
        gl_renderer.Render(2560, 1080);
        vp = gl_renderer.GetCurrentViewport();
        assert(vp.width == 2520); // 1080 * 21 / 9 = 2520
        assert(vp.x == 20);

        // Test StretchToWindow
        gl_renderer.SetAspectRatioMode(personarm::AspectRatioMode::StretchToWindow);
        gl_renderer.Render(1920, 1080);
        vp = gl_renderer.GetCurrentViewport();
        assert(vp.width == 1920);
        assert(vp.height == 1080);

        gl_renderer.Shutdown();
        window.Shutdown();
        assert(window.IsOpen() == false);

        std::cout << "  ✓ Window Context & GL Renderer tests passed.\n";
    }

    // 18. MDEC Decoder & MIPS RFE Instruction Tests
    {
        // 18.1 IDCT 8x8 with zero AC coefficients
        int16_t coeff[64] = {0};
        uint8_t pixels[64] = {0};
        personarm::MdecDecoder::PerformIdct8x8(coeff, pixels);
        for (int i = 0; i < 64; ++i) {
            assert(pixels[i] == 128); // Level shift with 0 DC/AC produces mid-scale 128
        }

        // 18.2 YCbCr to RGBA color space conversion
        uint32_t black = personarm::MdecDecoder::YCbCrToRgba(0, 128, 128);
        assert((black & 0x00FFFFFF) == 0x000000); // R=0, G=0, B=0

        uint32_t white = personarm::MdecDecoder::YCbCrToRgba(255, 128, 128);
        assert((white & 0x00FFFFFF) == 0xFFFFFF); // R=255, G=255, B=255

        // 18.3 Frame decoding
        personarm::MdecDecoder mdec;
        personarm::MdecFrame frame;
        bool decoded = mdec.DecodeFrame(nullptr, 0, 320, 240, frame);
        assert(decoded == true);
        assert(frame.width == 320);
        assert(frame.height == 240);
        assert(frame.pixels_rgba.size() == 320 * 240);
        assert(mdec.GetDecodedFrameCount() == 1);

        // 18.4 MIPS RFE (Restore From Exception) instruction
        personarm::CpuContext cpu;
        // Status with IEc=0, KUc=0, IEp=1, KUp=0, IEo=1, KUo=1 -> binary ...110100
        // We set cp0_status via MTC0 or direct instruction
        // Opcode MTC0 rt=1, rd=12: (0x10 << 26) | (0x04 << 21) | (1 << 16) | (12 << 11)
        uint32_t mtc0_op = (0x10 << 26) | (0x04 << 21) | (1 << 16) | (12 << 11);
        // Opcode RFE: 0x42000010 (cop0, bit 25=1, funct=0x10)
        uint32_t rfe_op = 0x42000010;

        std::vector<uint32_t> code = { mtc0_op, rfe_op };
        cpu.SetGPR(1, 0x34); // 0b110100 -> IEp=1, KUp=0, IEo=1, KUo=1
        cpu.SetMemoryHooks(
            [&code](uint32_t addr, uint32_t) -> uint32_t {
                uint32_t idx = (addr - 0x80010000) / 4;
                if (idx < code.size()) return code[idx];
                return 0;
            },
            nullptr
        );
        cpu.SetPC(0x80010000);
        cpu.Step(); // Execute MTC0
        assert((cpu.GetRegisters().cp0_status & 0x3F) == 0x34);

        cpu.Step(); // Execute RFE
        // After RFE, bits 2..5 (0b1101) are shifted right into bits 0..3 (0b1101)
        // so IEc becomes 1, KUc becomes 0!
        assert((cpu.GetRegisters().cp0_status & 0x03) == 0x01); // IEc=1, KUc=0

        std::cout << "  ✓ MDEC Decoder & MIPS RFE tests passed.\n";
    }

    // 19. PS1 Virtual Memory Card & QuickSave Manager Tests
    {
        personarm::SaveManager save_mgr;
        assert(save_mgr.IsFormatted() == true);
        assert(save_mgr.GetCardData().size() == 131072);

        // Write a test slot
        std::vector<uint8_t> p2_save_payload(256, 0x5A);
        bool write_ok = save_mgr.WriteSlot(3, "Persona 2: EP - Seven Sisters", "BASLUS-01158GAME",
                                           p2_save_payload.data(), p2_save_payload.size());
        assert(write_ok == true);

        // List slots
        auto slots = save_mgr.ListSlots();
        assert(slots.size() == 15);
        assert(slots[3].is_in_use == true);
        assert(slots[3].product_code == "BASLUS-01158GAME");
        assert(slots[3].title == "Persona 2: EP - Seven Sisters");
        assert(slots[0].is_in_use == false);

        // Read slot back
        std::vector<uint8_t> read_payload;
        bool read_ok = save_mgr.ReadSlot(3, read_payload);
        assert(read_ok == true);
        assert(read_payload.size() >= 256);
        assert(std::memcmp(read_payload.data(), p2_save_payload.data(), 256) == 0);

        // Test QuickSave / QuickLoad
        auto& engine = personarm::Engine::Instance();
        engine.Initialize();
        engine.GetCpu().SetPC(0x80020080);
        engine.GetCpu().SetGPR(2, 999);
        engine.SetFrameCount(1234);

        std::vector<uint8_t> snapshot;
        assert(save_mgr.CaptureQuickSave(engine, snapshot) == true);
        assert(snapshot.size() > 1024);

        // Mutate engine state
        engine.GetCpu().SetPC(0x80000000);
        engine.GetCpu().SetGPR(2, 0);
        engine.SetFrameCount(0);

        // Restore
        assert(save_mgr.RestoreQuickSave(engine, snapshot.data(), snapshot.size()) == true);
        assert(engine.GetCpu().GetPC() == 0x80020080);
        assert(engine.GetCpu().GetGPR(2) == 999);
        assert(engine.GetFrameCount() == 1234);

        std::cout << "  ✓ PS1 Virtual Memory Card & QuickSave tests passed.\n";
    }

    // 20. PS1 Hardware Controller Port Bus & Turbo Speed Tests
    {
        auto& engine = personarm::Engine::Instance();
        engine.Initialize();

        // 20.1 Test Speed Multiplier
        engine.SetSpeedMultiplier(2.5f);
        assert(engine.GetSpeedMultiplier() == 2.5f);
        engine.SetSpeedMultiplier(1.0f);

        // 20.2 Test Controller Hardware Port 0x1F801040 (JOY_DATA) via MIPS instructions
        engine.GetInput().SimulateActionPress(personarm::VirtualAction::Confirm);
        engine.GetInput().SimulateActionPress(personarm::VirtualAction::MoveUp);

        // SB $1, 0($2): opcode (0x28 << 26) | (2 << 21) | (1 << 16)
        // LBU $3, 0($2): opcode (0x24 << 26) | (2 << 21) | (3 << 16)
        uint32_t sb_op = (0x28 << 26) | (2 << 21) | (1 << 16);
        uint32_t lbu_op = (0x24 << 26) | (2 << 21) | (3 << 16);
        std::vector<uint32_t> pad_code = { sb_op, lbu_op };

        std::vector<uint8_t>& ram = const_cast<std::vector<uint8_t>&>(engine.GetRam());
        std::memcpy(&ram[0x1000], pad_code.data(), pad_code.size() * sizeof(uint32_t));

        // Set $2 = 0x1F801040 (JOY_DATA)
        engine.GetCpu().SetGPR(2, 0x1F801040);

        // Step 1: Send 0x01 (Device select) -> responds 0xFF
        engine.GetCpu().SetGPR(1, 0x01);
        engine.GetCpu().SetPC(0x80001000);
        engine.GetCpu().Step(); // Executes SB 0x01
        engine.GetCpu().Step(); // Executes LBU (result lands after the load delay slot)
        engine.GetCpu().Step(); // Delay slot: delayed load is committed
        assert(engine.GetCpu().GetGPR(3) == 0xFF);

        // Step 2: Send 0x42 (Poll command 'B') -> responds 0x41 (Digital Pad ID)
        engine.GetCpu().SetGPR(1, 0x42);
        engine.GetCpu().SetPC(0x80001000);
        engine.GetCpu().Step();
        engine.GetCpu().Step();
        engine.GetCpu().Step(); // load delay slot
        assert(engine.GetCpu().GetGPR(3) == 0x41);

        // Step 3: Send 0x00 -> Header End (0x5A)
        engine.GetCpu().SetGPR(1, 0x00);
        engine.GetCpu().SetPC(0x80001000);
        engine.GetCpu().Step();
        engine.GetCpu().Step();
        engine.GetCpu().Step(); // load delay slot
        assert(engine.GetCpu().GetGPR(3) == 0x5A);

        // Step 4: Send 0x00 -> Buttons Byte 1 (MoveUp was active -> bit 4 is 0)
        engine.GetCpu().SetGPR(1, 0x00);
        engine.GetCpu().SetPC(0x80001000);
        engine.GetCpu().Step();
        engine.GetCpu().Step();
        engine.GetCpu().Step(); // load delay slot
        uint32_t btn1 = engine.GetCpu().GetGPR(3);
        assert((btn1 & (1 << 4)) == 0); // Up pressed
        assert((btn1 & (1 << 5)) != 0); // Right released

        // Step 5: Send 0x00 -> Buttons Byte 2 (Confirm was active -> bit 6 Cross is 0)
        engine.GetCpu().SetGPR(1, 0x00);
        engine.GetCpu().SetPC(0x80001000);
        engine.GetCpu().Step();
        engine.GetCpu().Step();
        engine.GetCpu().Step(); // load delay slot
        uint32_t btn2 = engine.GetCpu().GetGPR(3);
        assert((btn2 & (1 << 6)) == 0); // Cross pressed
        assert((btn2 & (1 << 5)) != 0); // Circle released

        std::cout << "  ✓ PS1 Controller Hardware Port & Turbo tests passed.\n";
    }

    // 21. Persona 2 EP disc identification (serials from community databases; see errata G1/G2)
    {
        using personarm::DiscReader;
        auto us1 = DiscReader::IdentifyPersona2("SLUS_011.58");
        assert(us1.recognized && us1.region == "US" && us1.disc_number == 1 && us1.product_id == "SLUS-01158");
        auto us2 = DiscReader::IdentifyPersona2("SLUS-01339");
        assert(us2.recognized && us2.disc_number == 2 && us2.boot_exe == "SLUS_013.39"); // disc 2 is part of the game, not a bonus disc
        auto jp = DiscReader::IdentifyPersona2("slps_027.85;1");
        assert(jp.recognized && jp.region == "JP" && jp.edition == "Deluxe Pack" && jp.boot_exe == "SLPS_027.85");
        assert(DiscReader::IdentifyPersona2("SLPS_028.26").disc_number == 2);
        assert(DiscReader::IdentifyPersona2("SLPM_869.67").recognized);
        assert(!DiscReader::IdentifyPersona2("SLPS_027.70").recognized); // old, incorrect serial
        assert(!DiscReader::IdentifyPersona2("SLPS_027.71").recognized);
        assert(!DiscReader::IdentifyPersona2("").recognized);
        assert(DiscReader::KnownPersona2BootExecutables().size() == 9);
        std::cout << "  ✓ Persona 2 disc identification tests passed.\n";
    }

    // 22. PS-X EXE header layout (PSX-SPX offsets) and memfill
    {
        static_assert(offsetof(personarm::PsxExeHeader, memfill_start) == 0x28, "memfill_start offset");
        static_assert(offsetof(personarm::PsxExeHeader, initial_sp_base) == 0x30, "sp base offset");

        std::vector<uint8_t> exe(4096, 0);
        std::memcpy(exe.data(), "PS-X EXE", 8);
        auto put32 = [&](size_t off, uint32_t v) { std::memcpy(exe.data() + off, &v, 4); };
        put32(0x10, 0x80010000); // PC
        put32(0x14, 0x80080000); // GP
        put32(0x18, 0x80010000); // destination
        put32(0x1C, 2048);       // size
        put32(0x28, 0x80010100); // memfill start
        put32(0x2C, 16);         // memfill size
        put32(0x30, 0x801FFF00); // SP base
        put32(0x34, 0x10);       // SP offset
        for (size_t i = 2048; i < 4096; ++i) exe[i] = 0xAA;

        personarm::CpuContext cpu;
        std::vector<uint8_t> ram(2 * 1024 * 1024, 0);
        personarm::PsxExeInfo info{};
        assert(personarm::ExeLoader::LoadExe(exe.data(), exe.size(), cpu, ram, info));
        assert(info.initial_sp == 0x801FFF10);
        assert(cpu.GetGPR(29) == 0x801FFF10);
        assert(ram[0x10000] == 0xAA && ram[0x10100] == 0 && ram[0x1010F] == 0 && ram[0x10110] == 0xAA);
        std::cout << "  ✓ PS-X EXE header layout & memfill tests passed.\n";
    }

    // 23. DiscReader MODE2/2352 raw sector, CUE resolution & Portal tests
    {
        const std::string test_bin = "test_synth_disc.bin";
        const std::string test_cue = "test_synth_disc.cue";
        const uint32_t SECTOR_SIZE = 2352;
        const uint32_t HEADER_OFF = 24;
        const uint32_t TOTAL_SECTORS = 25;

        std::vector<uint8_t> disc_data(TOTAL_SECTORS * SECTOR_SIZE, 0);

        // Sector 16: Primary Volume Descriptor
        size_t pvd_off = 16 * SECTOR_SIZE + HEADER_OFF;
        disc_data[pvd_off + 0] = 0x01; // PVD type
        disc_data[pvd_off + 1] = 'C';
        disc_data[pvd_off + 2] = 'D';
        disc_data[pvd_off + 3] = '0';
        disc_data[pvd_off + 4] = '0';
        disc_data[pvd_off + 5] = '1';
        std::memcpy(disc_data.data() + pvd_off + 40, "PERSONA2_EP                     ", 32);

        // Root directory record at offset 156
        size_t root_rec = pvd_off + 156;
        disc_data[root_rec + 0] = 34; // record length
        *reinterpret_cast<uint32_t*>(disc_data.data() + root_rec + 2) = 18; // root LBA
        *reinterpret_cast<uint32_t*>(disc_data.data() + root_rec + 10) = 2048; // root length

        // Sector 18: Root directory records
        size_t dir_off = 18 * SECTOR_SIZE + HEADER_OFF;
        uint8_t entry_len = 33 + 10;
        disc_data[dir_off + 0] = entry_len;
        *reinterpret_cast<uint32_t*>(disc_data.data() + dir_off + 2) = 19; // LBA
        *reinterpret_cast<uint32_t*>(disc_data.data() + dir_off + 10) = 40; // size
        disc_data[dir_off + 25] = 0x00; // file flag
        disc_data[dir_off + 32] = 10; // name len
        std::memcpy(disc_data.data() + dir_off + 33, "SYSTEM.CNF", 10);

        // Sector 19: SYSTEM.CNF content
        size_t cnf_off = 19 * SECTOR_SIZE + HEADER_OFF;
        const char* cnf_text = "BOOT = cdrom:\\SLPS_027.85;1\r\nTCB = 4\r\n";
        std::memcpy(disc_data.data() + cnf_off, cnf_text, std::strlen(cnf_text));

        // Write synthetic bin file
        {
            std::ofstream out(test_bin, std::ios::binary);
            out.write(reinterpret_cast<const char*>(disc_data.data()), disc_data.size());
        }

        // Write synthetic cue file
        {
            std::ofstream cue_out(test_cue);
            cue_out << "FILE \"" << test_bin << "\" BINARY\n";
            cue_out << "  TRACK 01 MODE2/2352\n";
            cue_out << "    INDEX 01 00:00:00\n";
        }

        // Test CUE resolution
        std::string resolved_bin = personarm::DiscReader::ResolveCueBinary(test_cue);
        assert(!resolved_bin.empty());

        // Test opening via CUE
        personarm::DiscReader reader;
        assert(reader.Open(test_cue));
        assert(reader.GetSectorSize() == 2352);
        assert(reader.GetHeaderOffset() == 24);
        assert(reader.GetVolumeLabel() == "PERSONA2_EP");
        assert(reader.FileExists("SYSTEM.CNF"));

        std::vector<uint8_t> read_cnf;
        assert(reader.ReadFile("SYSTEM.CNF", read_cnf));
        assert(read_cnf.size() == 40);
        std::string cnf_str(read_cnf.begin(), read_cnf.end());
        assert(cnf_str.find("SLPS_027.85") != std::string::npos);

        std::string boot_exe = reader.DetectBootExecutable();
        assert(boot_exe == "SLPS_027.85");

        auto p2_info = personarm::DiscReader::IdentifyPersona2(boot_exe);
        assert(p2_info.recognized);
        assert(p2_info.product_id == "SLPS-02785");
        assert(p2_info.region == "JP");
        assert(p2_info.edition == "Deluxe Pack");
        assert(p2_info.disc_number == 1);

        reader.Close();

        // Portal dual-disc management tests
        {
            personarm::Portal portal;
            assert(portal.Initialize("test_portal_config.json"));

            assert(portal.LoadDisc(1, test_cue));
            const auto& slot1 = portal.GetDiscSlot(1);
            assert(slot1.verified);
            assert(slot1.disc_number == 1);
            assert(slot1.info.recognized);
            assert(slot1.info.product_id == "SLPS-02785");

            assert(portal.GetActiveDiscNumber() == 1);
            assert(portal.SetActiveDiscNumber(2));
            assert(portal.GetActiveDiscNumber() == 2);
            assert(portal.SetActiveDiscNumber(1));
            assert(portal.GetActiveDiscNumber() == 1);

            std::remove("test_portal_config.json");
        }

        std::remove(test_bin.c_str());
        std::remove(test_cue.c_str());

        std::cout << "  ✓ DiscReader MODE2/2352, CUE parsing & Portal tests passed.\n";
    }

    std::cout << "[PASS] All unit tests completed successfully!\n";
    return 0;
}

