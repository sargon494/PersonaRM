#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <optional>
#include <stdexcept>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────
// Forward Declarations & Common Aliases
// ───────────────────────────────────────────────────────────────────────

class MemoryRegion;
class MemoryAllocator;
class MemoryManager;

using SizeType = std::size_t;
using PointerType = void*;

// ───────────────────────────────────────────────────────────────────────
// Memory Allocation Policy
// ───────────────────────────────────────────────────────────────────────

enum class AllocationPolicy {
    DEFAULT,      // Default allocator (buddy system)
    POOLED,       // Pooled allocation for small objects
    FIXED,        // Fixed-size allocation
    DYNAMIC,      // Dynamic allocation with arena
};

// ───────────────────────────────────────────────────────────────────────
// Memory Region
// ───────────────────────────────────────────────────────────────────────

class MemoryRegion {
public:
    friend class MemoryManager;

    MemoryRegion() = default;
    explicit MemoryRegion(SizeType size);
    ~MemoryRegion();

    MemoryRegion(const MemoryRegion&) = delete;
    MemoryRegion& operator=(const MemoryRegion&) = delete;
    MemoryRegion(MemoryRegion&& other) noexcept;
    MemoryRegion& operator=(MemoryRegion&& other) noexcept;

    void Initialize(SizeType size);
    void Reset();
    bool IsInitialized() const { return m_initialized; }
    SizeType Size() const { return m_size; }
    SizeType Available() const { return m_size > m_used ? m_size - m_used : 0; }
    PointerType Base() const { return m_base; }
    PointerType Top() const { return m_top; }
    SizeType Used() const { return m_used; }

private:
    PointerType m_base = nullptr;
    PointerType m_top = nullptr;
    SizeType m_size = 0;
    SizeType m_used = 0;
    bool m_initialized = false;
};

// ───────────────────────────────────────────────────────────────────────
// Memory Allocator (Abstract Base)
// ───────────────────────────────────────────────────────────────────────

class MemoryAllocator {
public:
    virtual ~MemoryAllocator() = default;

    virtual PointerType Allocate(SizeType size) = 0;
    virtual void Deallocate(PointerType ptr, SizeType size = 0) = 0;
    virtual void Reset() = 0;

    virtual SizeType Allocated(SizeType size = 0) const = 0;
    virtual SizeType Used() const = 0;
    virtual SizeType Available() const = 0;
    virtual bool IsAllocated(PointerType ptr) const = 0;

    static constexpr SizeType MinAllocation = 8;
    static constexpr SizeType MaxAllocation = 4 * 1024 * 1024;  // 4MB

protected:
    MemoryAllocator() = default;
    MemoryAllocator(SizeType min_size, SizeType max_size);

    SizeType m_min_size = MinAllocation;
    SizeType m_max_size = MaxAllocation;
};

// ───────────────────────────────────────────────────────────────────────
// Buddy System Allocator
// ───────────────────────────────────────────────────────────────────────

class BuddyAllocator : public MemoryAllocator {
public:
    friend class MemoryManager;

    BuddyAllocator(SizeType max_size = 64 * 1024 * 1024);
    ~BuddyAllocator() override;

    PointerType Allocate(SizeType size) override;
    void Deallocate(PointerType ptr, SizeType size = 0) override;
    void Reset() override;

    SizeType Allocated(SizeType size = 0) const override { (void)size; return m_allocated; }
    SizeType Used() const override { return m_used; }
    SizeType Available() const override { return m_max_size > m_used ? m_max_size - m_used : 0; }
    bool IsAllocated(PointerType ptr) const override;

private:
    void BuddySplit(SizeType order);

    void* m_base = nullptr;
    SizeType m_max_size = 0;
    SizeType m_allocated = 0;
    SizeType m_used = 0;
    std::vector<bool> m_free_list;
    std::vector<SizeType> m_block_sizes;
};

// ───────────────────────────────────────────────────────────────────────
// Pool Allocator
// ───────────────────────────────────────────────────────────────────────

class PoolAllocator : public MemoryAllocator {
public:
    friend class MemoryManager;

    PoolAllocator(SizeType pool_size, SizeType object_size = 256);
    ~PoolAllocator() override = default;

    PointerType Allocate(SizeType size = 0) override;
    void Deallocate(PointerType ptr, SizeType size = 0) override;
    void Reset() override;

    SizeType Allocated(SizeType size = 0) const override { (void)size; return m_allocated; }
    SizeType Used() const override { return m_used; }
    SizeType Available() const override { return m_pool_size > m_used ? m_pool_size - m_used : 0; }
    bool IsAllocated(PointerType ptr) const override;

    void Reserve(SizeType count);

private:
    SizeType m_pool_size = 0;
    SizeType m_object_size = 0;
    SizeType m_allocated = 0;
    SizeType m_used = 0;
    std::vector<uint8_t> m_pool_memory;
    std::vector<bool> m_in_use;
};

// ───────────────────────────────────────────────────────────────────────
// Arena Allocator
// ───────────────────────────────────────────────────────────────────────

class ArenaAllocator {
public:
    friend class MemoryManager;

    ArenaAllocator() = default;
    ~ArenaAllocator();

    PointerType Allocate(SizeType size);
    void Reset();
    void FreeRange(PointerType begin, PointerType end);

    SizeType Allocated(SizeType size = 0) const { (void)size; return m_used; }
    SizeType Used() const { return m_used; }
    SizeType Available() const { return m_capacity > m_used ? m_capacity - m_used : 0; }
    SizeType Capacity() const { return m_capacity; }

private:
    PointerType m_base = nullptr;
    PointerType m_top = nullptr;
    SizeType m_capacity = 0;
    SizeType m_used = 0;
};

// ───────────────────────────────────────────────────────────────────────
// Memory Manager
// ───────────────────────────────────────────────────────────────────────

class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // Core operations
    void Initialize(SizeType total_size = 0);
    void Shutdown();
    void Reset();

    // Allocation
    PointerType Allocate(SizeType size, AllocationPolicy policy = AllocationPolicy::DEFAULT);
    void Deallocate(PointerType ptr);
    bool IsAllocated(PointerType ptr) const;

    // Region management
    MemoryRegion* CreateRegion(SizeType size);
    void DestroyRegion(MemoryRegion* region);
    MemoryRegion* GetRegionByPointer(PointerType ptr);

    // Pool management
    PoolAllocator* CreatePool(SizeType pool_size, SizeType object_size = 256);
    void DestroyPool(PoolAllocator* pool);

    // Arena management
    ArenaAllocator* CreateArena(SizeType arena_size);
    void DestroyArena(ArenaAllocator* arena);

    // Statistics
    SizeType TotalAllocated() const;
    SizeType TotalUsed() const;
    SizeType TotalAvailable() const;
    SizeType NumRegions() const { return m_regions.size(); }
    SizeType NumPools() const { return m_pools.size(); }
    SizeType NumArenas() const { return m_arenas.size(); }

    // Fragmentation
    float FragmentationRatio() const;

private:
    struct Region {
        MemoryRegion region;
        std::string name;
    };

    struct Pool {
        std::unique_ptr<PoolAllocator> allocator;
        std::string name;
    };

    struct Arena {
        ArenaAllocator allocator;
        std::string name;
    };

    SizeType m_total_size = 0;
    std::vector<Region> m_regions;
    std::vector<Pool> m_pools;
    std::vector<Arena> m_arenas;
};

}  // namespace personarm

// Backwards-compatibility alias for legacy code
namespace persona2ep = personarm;
