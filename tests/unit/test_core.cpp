#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include "core/memory_manager.h"
#include "core/logger.h"
#include "input/input_manager.h"
#include "loader/disc_reader.h"
#include "loader/tim_parser.h"
#include "rendering/renderer.h"
#include "audio/audio_system.h"
#include "audio/vag_decoder.h"
#include "cpu/cpu_context.h"

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

    std::cout << "[PASS] All unit tests completed successfully!\n";
    return 0;
}

