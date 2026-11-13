#pragma once
// PS1 DMA controller (0x1F801080..0x1F8010FF): 7 channels (MADR/BCR/CHCR at +10h*N),
// DPCR at +70h, DICR at +74h. Source: psx-spx "DMA Channels" (HW-022 in
// docs/research/PS1_HARDWARE_REQUIREMENTS.md). The game uses channels 0,1,2,3,4,6.
//
// WIRING (done in Engine::WireDevices, src/engine/engine.cpp):
//   1. Implement hw::DmaMemory over main RAM (addresses arrive masked to 0x1FFFFC).
//   2. hw::Dma dma(mem); for addr in [Dma::kBase, kBase+0x7F] route Read/Write 8/16/32
//      with offset = addr - Dma::kBase.
//   3. Ports: dma.SetPort(Dma::kGpu,   {gpuread_fn, gp0_write_fn});
//             dma.SetPort(Dma::kCdrom, {[&]{ return cd.DmaRead32(); }, nullptr});
//             dma.SetPort(Dma::kSpu,   {spu_fifo_read_fn, spu_fifo_write_fn});
//             dma.SetPort(Dma::kMdecIn/kMdecOut, {..., ..., drq_fn});  // DRQ-paced
//   4. dma.SetIrqCallback([&]{ i_stat |= 1u << 3; });  // IRQ3 = DMA in I_STAT
//   5. Call dma.Tick(cycles) every CPU step/batch (completion delays live there).
#include <cstdint>
#include <functional>

namespace personarm::hw {

/// Word access to main RAM for DMA. Addresses are physical and word-aligned.
class DmaMemory {
public:
    virtual ~DmaMemory() = default;
    virtual uint32_t ReadWord(uint32_t addr) = 0;
    virtual void WriteWord(uint32_t addr, uint32_t value) = 0;
};

/// Device side of a channel: `read` = device->RAM, `write` = RAM->device. Either may be empty.
/// `request` (optional, sync mode 1 only) is the device DRQ: when set, blocks move one at a time
/// from Tick() while it returns true, instead of all at once at start (MDEC DMA0/DMA1).
struct DmaPort {
    std::function<uint32_t()> read;
    std::function<void(uint32_t)> write;
    std::function<bool()> request;
};

class Dma {
public:
    static constexpr uint32_t kBase = 0x1F801080;
    enum Channel { kMdecIn = 0, kMdecOut, kGpu, kCdrom, kSpu, kPio, kOtc, kNumChannels };
    // shortcut: transfers run instantly at start; completion (busy clear, DICR flag, IRQ3)
    // fires after words*kCyclesPerWord cycles (ports with a `request` DRQ move block by block instead).
    // Real DMA steals CPU cycles.
    static constexpr uint32_t kCyclesPerWord = 1;
    static constexpr uint32_t kLinkedListGuard = 0x100000;  // max nodes before aborting a looped list

    explicit Dma(DmaMemory& mem) : m_mem(mem) {}

    void SetPort(int channel, DmaPort port) { m_ch[channel].port = std::move(port); }
    void SetIrqCallback(std::function<void()> cb) { m_irq_cb = std::move(cb); }

    uint32_t Read32(uint32_t offset);
    uint16_t Read16(uint32_t offset) { return static_cast<uint16_t>(Read32(offset & ~3u) >> ((offset & 2) * 8)); }
    uint8_t Read8(uint32_t offset) { return static_cast<uint8_t>(Read32(offset & ~3u) >> ((offset & 3) * 8)); }
    void Write32(uint32_t offset, uint32_t v) { WriteMasked(offset & ~3u, v, 0xFFFFFFFFu); }
    void Write16(uint32_t offset, uint16_t v) {
        const uint32_t sh = (offset & 2) * 8;
        WriteMasked(offset & ~3u, static_cast<uint32_t>(v) << sh, 0xFFFFu << sh);
    }
    void Write8(uint32_t offset, uint8_t v) {
        const uint32_t sh = (offset & 3) * 8;
        WriteMasked(offset & ~3u, static_cast<uint32_t>(v) << sh, 0xFFu << sh);
    }

    void Tick(uint32_t cycles);
    /// CPU cycles until Tick() may act on its own (complete a channel); 1 while a DRQ-paced transfer
    /// runs (it may move on any tick); UINT64_MAX when idle. A host may batch ticks up to this point.
    uint64_t CyclesToNextEvent() const;

    /// DICR bit 31 (the IRQ3 line).
    bool IrqLine() const { return (m_dicr >> 31) != 0; }
    bool Busy(int channel) const { return (m_ch[channel].chcr >> 24) & 1; }

private:
    struct Chan {
        uint32_t madr = 0, bcr = 0, chcr = 0;
        uint32_t pending = 0;  // cycles until completion; 0 = idle
        bool running = false;
        bool paced = false;  // DRQ-paced sync-1 transfer in progress (blocks left in BCR bits 16-31)
        DmaPort port;
    };

    void WriteMasked(uint32_t offset, uint32_t value, uint32_t mask);
    void TryStart(int ch);
    uint32_t Transfer(int ch);  // returns words moved
    uint32_t MoveWords(int ch, uint32_t addr, uint32_t n);  // returns the address after the last word
    void Complete(int ch);
    void UpdateMaster();

    DmaMemory& m_mem;
    std::function<void()> m_irq_cb;
    Chan m_ch[kNumChannels];
    uint32_t m_dpcr = 0x07654321;
    uint32_t m_dicr = 0;
};

} // namespace personarm::hw
