#include <iostream>
#include <cassert>
#include "core/memory_manager.h"
#include "input/input_manager.h"

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

    std::cout << "[PASS] All unit tests completed successfully!\n";
    return 0;
}
