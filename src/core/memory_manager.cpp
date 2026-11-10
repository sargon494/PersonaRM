#include "memory_manager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <sstream>

namespace persona2ep {

// ───────────────────────────────────────────────────────────────────────
// MemoryRegion Implementation
// ───────────────────────────────────────────────────────────────────────

MemoryRegion::MemoryRegion(SizeType size) {
    Initialize(size);
}

MemoryRegion::~MemoryRegion() {
    Reset();
}

MemoryRegion::MemoryRegion(MemoryRegion&& other) noexcept
    : m_base(other.m_base),
      m_top(other.m_top),
      m_size(other.m_size),
      m_used(other.m_used),
      m_initialized(other.m_initialized) {
    other.m_base = nullptr;
    other.m_top = nullptr;
    other.m_size = 0;
    other.m_used = 0;
    other.m_initialized = false;
}

MemoryRegion& MemoryRegion::operator=(MemoryRegion&& other) noexcept {
    if (this != &other) {
        Reset();
        m_base = other.m_base;
        m_top = other.m_top;
        m_size = other.m_size;
        m_used = other.m_used;
        m_initialized = other.m_initialized;

        other.m_base = nullptr;
        other.m_top = nullptr;
        other.m_size = 0;
        other.m_used = 0;
        other.m_initialized = false;
    }
    return *this;
}

void MemoryRegion::Initialize(SizeType size) {
    if (size > SizeType(1024) * SizeType(1024) * SizeType(1024)) {
        throw std::runtime_error("MemoryRegion: size too large");
    }

    Reset();

    m_base = std::malloc(size);
    if (!m_base) {
        throw std::bad_alloc();
    }

    m_top = static_cast<uint8_t*>(m_base) + size;
    m_size = size;
    m_used = 0;
    m_initialized = true;
}

void MemoryRegion::Reset() {
    if (m_base) {
        std::free(m_base);
        m_base = nullptr;
        m_top = nullptr;
        m_size = 0;
        m_used = 0;
        m_initialized = false;
    }
}

// ───────────────────────────────────────────────────────────────────────
// MemoryAllocator Implementation
// ───────────────────────────────────────────────────────────────────────

MemoryAllocator::MemoryAllocator(SizeType min_size, SizeType max_size)
    : m_min_size(min_size), m_max_size(max_size) {}

// ───────────────────────────────────────────────────────────────────────
// BuddyAllocator Implementation
// ───────────────────────────────────────────────────────────────────────

BuddyAllocator::BuddyAllocator(SizeType max_size)
    : m_max_size(max_size) {
    m_base = std::malloc(m_max_size);
    if (!m_base) {
        throw std::bad_alloc();
    }

    size_t levels = static_cast<size_t>(std::log2(static_cast<double>(max_size))) + 1;
    m_block_sizes.resize(levels);
    m_free_list.resize(levels, false);

    m_block_sizes[0] = max_size;
    m_free_list[0] = true;

    for (size_t i = 1; i < levels; ++i) {
        m_block_sizes[i] = m_block_sizes[i - 1] / 2;
        m_free_list[i] = false;
    }

    m_allocated = 0;
    m_used = 0;
}

BuddyAllocator::~BuddyAllocator() {
    Reset();
}

PointerType BuddyAllocator::Allocate(SizeType size) {
    if (size == 0) {
        size = m_min_size;
    }

    if (size > m_max_size || !m_base) {
        throw std::bad_alloc();
    }

    if (m_used + size > m_max_size) {
        throw std::bad_alloc();
    }

    PointerType ptr = static_cast<uint8_t*>(m_base) + m_used;
    m_used += size;
    m_allocated += size;
    return ptr;
}

void BuddyAllocator::Deallocate(PointerType ptr, SizeType size) {
    (void)ptr;
    if (size > m_used) {
        m_used = 0;
    } else {
        m_used -= size;
    }
}

void BuddyAllocator::Reset() {
    if (m_base) {
        std::free(m_base);
        m_base = nullptr;
    }
    m_allocated = 0;
    m_used = 0;
    std::fill(m_free_list.begin(), m_free_list.end(), false);
}

bool BuddyAllocator::IsAllocated(PointerType ptr) const {
    if (!m_base || !ptr) {
        return false;
    }
    return ptr >= m_base && ptr < static_cast<const uint8_t*>(m_base) + m_used;
}

void BuddyAllocator::BuddySplit(SizeType order) {
    (void)order;
}

// ───────────────────────────────────────────────────────────────────────
// PoolAllocator Implementation
// ───────────────────────────────────────────────────────────────────────

PoolAllocator::PoolAllocator(SizeType pool_size, SizeType object_size)
    : m_pool_size(pool_size), m_object_size(object_size) {
    if (m_object_size == 0) {
        m_object_size = 256;
    }
    size_t count = m_pool_size / m_object_size;
    m_pool_memory.resize(m_pool_size);
    m_in_use.resize(count, false);
}

PointerType PoolAllocator::Allocate(SizeType size) {
    if (size == 0) {
        size = m_object_size;
    }
    if (size > m_object_size) {
        throw std::runtime_error("PoolAllocator: size exceeds object size");
    }

    for (size_t i = 0; i < m_in_use.size(); ++i) {
        if (!m_in_use[i]) {
            m_in_use[i] = true;
            m_allocated += m_object_size;
            m_used += m_object_size;
            return m_pool_memory.data() + (i * m_object_size);
        }
    }

    throw std::bad_alloc();
}

void PoolAllocator::Deallocate(PointerType ptr, SizeType size) {
    (void)size;
    if (!ptr || m_pool_memory.empty()) {
        return;
    }

    uint8_t* byte_ptr = static_cast<uint8_t*>(ptr);
    uint8_t* start_ptr = m_pool_memory.data();
    uint8_t* end_ptr = start_ptr + m_pool_memory.size();

    if (byte_ptr < start_ptr || byte_ptr >= end_ptr) {
        throw std::runtime_error("PoolAllocator: invalid pointer");
    }

    size_t offset = static_cast<size_t>(byte_ptr - start_ptr);
    size_t index = offset / m_object_size;

    if (index < m_in_use.size() && m_in_use[index]) {
        m_in_use[index] = false;
        m_allocated -= m_object_size;
        m_used -= m_object_size;
    }
}

void PoolAllocator::Reset() {
    std::fill(m_in_use.begin(), m_in_use.end(), false);
    m_allocated = 0;
    m_used = 0;
}

bool PoolAllocator::IsAllocated(PointerType ptr) const {
    if (!ptr || m_pool_memory.empty()) {
        return false;
    }
    const uint8_t* byte_ptr = static_cast<const uint8_t*>(ptr);
    const uint8_t* start_ptr = m_pool_memory.data();
    const uint8_t* end_ptr = start_ptr + m_pool_memory.size();

    if (byte_ptr < start_ptr || byte_ptr >= end_ptr) {
        return false;
    }
    size_t index = static_cast<size_t>(byte_ptr - start_ptr) / m_object_size;
    return index < m_in_use.size() ? m_in_use[index] : false;
}

void PoolAllocator::Reserve(SizeType count) {
    if (count * m_object_size > m_pool_size) {
        m_pool_size = count * m_object_size;
        m_pool_memory.resize(m_pool_size);
        m_in_use.resize(count, false);
    }
}

// ───────────────────────────────────────────────────────────────────────
// ArenaAllocator Implementation
// ───────────────────────────────────────────────────────────────────────

ArenaAllocator::~ArenaAllocator() {
    Reset();
}

PointerType ArenaAllocator::Allocate(SizeType size) {
    if (size == 0) {
        size = 1;
    }

    if (m_capacity - m_used < size) {
        throw std::bad_alloc();
    }

    PointerType ptr = m_top;
    m_top = static_cast<uint8_t*>(m_top) + size;
    m_used += size;

    return ptr;
}

void ArenaAllocator::Reset() {
    if (m_base) {
        std::free(m_base);
        m_base = nullptr;
    }
    m_top = nullptr;
    m_capacity = 0;
    m_used = 0;
}

void ArenaAllocator::FreeRange(PointerType begin, PointerType end) {
    if (begin < m_base || end > m_top) {
        return;
    }
    SizeType size = static_cast<uint8_t*>(end) - static_cast<uint8_t*>(begin);
    if (size <= m_used) {
        m_used -= size;
        m_top = static_cast<uint8_t*>(m_top) - size;
    }
}

// ───────────────────────────────────────────────────────────────────────
// MemoryManager Implementation
// ───────────────────────────────────────────────────────────────────────

MemoryManager::MemoryManager() = default;

MemoryManager::~MemoryManager() {
    Shutdown();
}

void MemoryManager::Initialize(SizeType total_size) {
    Shutdown();
    m_total_size = total_size;

    Region default_region;
    default_region.region.Initialize(total_size > 0 ? total_size : 64 * 1024 * 1024);
    default_region.name = "default";
    m_regions.push_back(std::move(default_region));
}

void MemoryManager::Shutdown() {
    for (auto& region : m_regions) {
        region.region.Reset();
    }
    m_regions.clear();
    m_pools.clear();
    for (auto& arena : m_arenas) {
        arena.allocator.Reset();
    }
    m_arenas.clear();
}

void MemoryManager::Reset() {
    Shutdown();
}

PointerType MemoryManager::Allocate(SizeType size, AllocationPolicy policy) {
    switch (policy) {
        case AllocationPolicy::DEFAULT:
            if (!m_regions.empty()) {
                if (m_regions[0].region.Available() >= size) {
                    PointerType ptr = static_cast<uint8_t*>(m_regions[0].region.m_base) + m_regions[0].region.m_used;
                    m_regions[0].region.m_used += size;
                    return ptr;
                }
            }
            throw std::bad_alloc();

        case AllocationPolicy::POOLED:
            if (m_pools.empty()) {
                CreatePool(64 * 1024, 256);
            }
            return m_pools[0].allocator->Allocate(size);

        case AllocationPolicy::FIXED:
            return Allocate(size, AllocationPolicy::DEFAULT);

        case AllocationPolicy::DYNAMIC:
            if (m_arenas.empty()) {
                CreateArena(1024 * 1024);
            }
            return m_arenas[0].allocator.Allocate(size);

        default:
            throw std::runtime_error("MemoryManager: unknown policy");
    }
}

void MemoryManager::Deallocate(PointerType ptr) {
    if (!ptr) return;

    for (const auto& pool : m_pools) {
        if (pool.allocator->IsAllocated(ptr)) {
            pool.allocator->Deallocate(ptr, 0);
            return;
        }
    }
}

bool MemoryManager::IsAllocated(PointerType ptr) const {
    if (!ptr) return false;

    for (const auto& r : m_regions) {
        if (ptr >= r.region.m_base && ptr < static_cast<const uint8_t*>(r.region.m_base) + r.region.m_used) {
            return true;
        }
    }

    for (const auto& pool : m_pools) {
        if (pool.allocator->IsAllocated(ptr)) {
            return true;
        }
    }

    for (const auto& arena : m_arenas) {
        if (ptr >= arena.allocator.m_base && ptr < arena.allocator.m_top) {
            return true;
        }
    }

    return false;
}

MemoryRegion* MemoryManager::CreateRegion(SizeType size) {
    Region region;
    region.region.Initialize(size);
    region.name = "region_" + std::to_string(m_regions.size());
    m_regions.push_back(std::move(region));
    return &m_regions.back().region;
}

void MemoryManager::DestroyRegion(MemoryRegion* region) {
    if (!region) return;
    auto it = std::find_if(m_regions.begin(), m_regions.end(),
        [region](const Region& r) { return &r.region == region; });
    if (it != m_regions.end()) {
        it->region.Reset();
        m_regions.erase(it);
    }
}

MemoryRegion* MemoryManager::GetRegionByPointer(PointerType ptr) {
    for (auto& r : m_regions) {
        if (ptr >= r.region.m_base && ptr < r.region.m_top) {
            return &r.region;
        }
    }
    return nullptr;
}

PoolAllocator* MemoryManager::CreatePool(SizeType pool_size, SizeType object_size) {
    Pool pool;
    pool.allocator = std::make_unique<PoolAllocator>(pool_size, object_size);
    pool.name = "pool_" + std::to_string(m_pools.size());
    PoolAllocator* raw = pool.allocator.get();
    m_pools.push_back(std::move(pool));
    return raw;
}

void MemoryManager::DestroyPool(PoolAllocator* pool) {
    if (!pool) return;
    auto it = std::find_if(m_pools.begin(), m_pools.end(),
        [pool](const Pool& p) { return p.allocator.get() == pool; });
    if (it != m_pools.end()) {
        m_pools.erase(it);
    }
}

ArenaAllocator* MemoryManager::CreateArena(SizeType arena_size) {
    Arena arena;
    arena.allocator.m_base = std::malloc(arena_size);
    if (!arena.allocator.m_base) {
        throw std::bad_alloc();
    }
    arena.allocator.m_top = arena.allocator.m_base;
    arena.allocator.m_capacity = arena_size;
    arena.allocator.m_used = 0;
    arena.name = "arena_" + std::to_string(m_arenas.size());
    m_arenas.push_back(std::move(arena));
    return &m_arenas.back().allocator;
}

void MemoryManager::DestroyArena(ArenaAllocator* arena) {
    if (!arena) return;
    auto it = std::find_if(m_arenas.begin(), m_arenas.end(),
        [arena](const Arena& a) { return &a.allocator == arena; });
    if (it != m_arenas.end()) {
        it->allocator.Reset();
        m_arenas.erase(it);
    }
}

SizeType MemoryManager::TotalAllocated() const {
    SizeType total = 0;
    for (const auto& r : m_regions) total += r.region.m_used;
    for (const auto& p : m_pools) total += p.allocator->Used();
    for (const auto& a : m_arenas) total += a.allocator.Used();
    return total;
}

SizeType MemoryManager::TotalUsed() const {
    return TotalAllocated();
}

SizeType MemoryManager::TotalAvailable() const {
    SizeType total = 0;
    for (const auto& r : m_regions) total += r.region.Available();
    for (const auto& p : m_pools) total += p.allocator->Available();
    for (const auto& a : m_arenas) total += a.allocator.Available();
    return total;
}

float MemoryManager::FragmentationRatio() const {
    SizeType total_alloc = TotalAllocated();
    if (total_alloc == 0) return 0.0f;
    SizeType total_avail = TotalAvailable();
    if (total_avail == 0) return 0.0f;
    return (static_cast<float>(total_avail) / static_cast<float>(total_alloc + total_avail)) * 100.0f;
}

}  // namespace persona2ep
