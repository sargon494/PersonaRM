#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include "core/memory_manager.h"
#include "input/input_manager.h"
#include "loader/disc_reader.h"
#include "rendering/renderer.h"
#include "audio/audio_system.h"
#include "cpu/cpu_context.h"

int main() {
    std::cout << "[UNIT TEST] Running Core and Input Unit Tests...\n";

    // 1. Memory Manager Tests
    {
        persona2ep::MemoryManager mgr;
        mgr.Initialize(4 * 1024 * 1024);

        assert(mgr.NumRegions() > 0);
        assert(mgr.TotalAvailable() > 0);

        void* ptr1 = mgr.Allocate(128, persona2ep::AllocationPolicy::POOLED);
        assert(ptr1 != nullptr);
        assert(mgr.IsAllocated(ptr1));

        void* ptr2 = mgr.Allocate(1024, persona2ep::AllocationPolicy::DEFAULT);
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

    std::cout << "[PASS] All unit tests completed successfully!\n";
    return 0;
}

