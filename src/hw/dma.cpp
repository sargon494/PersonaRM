#include "hw/dma.h"

namespace personarm::hw {

namespace {
constexpr uint32_t kAddrMask = 0x1FFFFC;
constexpr uint32_t kDicrRw = 0x00FF803F;    // bits 0-5, 15 (force IRQ), 16-22 enable, 23 master enable
constexpr uint32_t kDicrFlags = 0x7F000000; // bits 24-30, write 1 to clear
} // namespace

uint32_t Dma::Read32(uint32_t offset) {
    offset &= 0x7C;
    if (offset == 0x70) return m_dpcr;
    if (offset == 0x74) return m_dicr;
    const int ch = static_cast<int>(offset >> 4);
    if (ch >= kNumChannels) return 0;
    switch (offset & 0xC) {
    case 0x0: return m_ch[ch].madr;
    case 0x4: return m_ch[ch].bcr;
    case 0x8: return m_ch[ch].chcr;
    default: return 0;
    }
}

void Dma::WriteMasked(uint32_t offset, uint32_t v, uint32_t mask) {
    offset &= 0x7C;
    auto merge = [&](uint32_t& reg) { reg = (reg & ~mask) | (v & mask); };
    if (offset == 0x70) {
        merge(m_dpcr);
        for (int ch = 0; ch < kNumChannels; ++ch) TryStart(ch);
        return;
    }
    if (offset == 0x74) {
        // Sub-word writes only touch (and only acknowledge) the bytes actually written.
        m_dicr = (m_dicr & ~(kDicrRw & mask)) | (v & kDicrRw & mask);
        m_dicr &= ~(v & mask & kDicrFlags);
        UpdateMaster();
        return;
    }
    const int ch = static_cast<int>(offset >> 4);
    if (ch >= kNumChannels) return;
    Chan& c = m_ch[ch];
    switch (offset & 0xC) {
    case 0x0: merge(c.madr); c.madr &= 0xFFFFFF; break;
    case 0x4: merge(c.bcr); break;
    case 0x8:
        merge(c.chcr);
        // OTC: only start/trigger (bits 24, 28) and unknown bit 30 writable; step backward fixed.
        if (ch == kOtc) c.chcr = (c.chcr & 0x51000000) | 0x2;
        TryStart(ch);
        break;
    default: break;
    }
}

void Dma::TryStart(int ch) {
    Chan& c = m_ch[ch];
    if (c.running || !(c.chcr & 0x01000000)) return;
    const uint32_t sync = (c.chcr >> 9) & 3;
    if (sync == 0 && !(c.chcr & 0x10000000)) return;  // burst mode needs the trigger bit
    // unverified: transfers wait for the DPCR master enable of the channel (bit 3 of each nibble).
    if (!(m_dpcr & (8u << (ch * 4)))) return;
    c.chcr &= ~0x10000000u;
    c.running = true;
    if (sync == 1 && c.port.request) {
        c.paced = true;
        c.pending = 0;
        return;
    }
    c.pending = Transfer(ch) * kCyclesPerWord + 1;
}

uint32_t Dma::Transfer(int ch) {
    Chan& c = m_ch[ch];
    const uint32_t sync = (c.chcr >> 9) & 3;
    uint32_t addr = c.madr & kAddrMask;

    if (ch == kOtc) {
        // Reverse-linked empty ordering table: each entry points to the previous word,
        // the last (lowest) one holds the end marker 0x00FFFFFF.
        uint32_t n = c.bcr & 0xFFFF;
        if (n == 0) n = 0x10000;
        for (uint32_t i = 0; i < n; ++i, addr = (addr - 4) & kAddrMask)
            m_mem.WriteWord(addr, i + 1 == n ? 0x00FFFFFF : ((addr - 4) & kAddrMask));
        return n;  // unverified: MADR/BCR left unchanged in sync mode 0
    }

    if (sync == 2) {
        // Linked list (GPU): header = next addr (bits 0-23) | word count (bits 24-31).
        // End when bit 23 of the next address is set (0xFFFFFF marker).
        uint32_t words = 0;
        for (uint32_t nodes = 0; nodes < kLinkedListGuard; ++nodes) {
            const uint32_t header = m_mem.ReadWord(addr);
            const uint32_t count = header >> 24;
            for (uint32_t i = 1; i <= count; ++i) {
                const uint32_t w = m_mem.ReadWord((addr + 4 * i) & kAddrMask);
                if (c.port.write) c.port.write(w);
            }
            words += count + 1;
            if (header & 0x800000) break;
            addr = header & kAddrMask;
        }
        c.madr = 0x00FFFFFF;
        return words;
    }

    uint32_t n;
    if (sync == 0) {
        n = c.bcr & 0xFFFF;
        if (n == 0) n = 0x10000;
    } else {  // sync 1: block size (bits 0-15) * block count (bits 16-31)
        uint32_t bs = c.bcr & 0xFFFF, ba = c.bcr >> 16;
        if (bs == 0) bs = 0x10000;  // unverified: zero block size means 0x10000
        n = bs * ba;
    }
    addr = MoveWords(ch, addr, n);
    if (sync == 1) {  // MADR advances and the block count reaches 0
        c.madr = addr;
        c.bcr &= 0xFFFF;
    }
    return n;
}

uint32_t Dma::MoveWords(int ch, uint32_t addr, uint32_t n) {
    Chan& c = m_ch[ch];
    const bool to_device = (c.chcr & 1) != 0;
    const uint32_t step = (c.chcr & 2) ? static_cast<uint32_t>(-4) : 4u;
    for (uint32_t i = 0; i < n; ++i, addr = (addr + step) & kAddrMask) {
        if (to_device) {
            const uint32_t w = m_mem.ReadWord(addr);
            if (c.port.write) c.port.write(w);
        } else {
            m_mem.WriteWord(addr, c.port.read ? c.port.read() : 0xFFFFFFFFu);  // unverified: open bus value
        }
    }
    return addr;
}

void Dma::Complete(int ch) {
    Chan& c = m_ch[ch];
    c.running = false;
    c.chcr &= ~0x01000000u;
    // unverified: the per-channel flag is only raised when that channel's enable bit is set.
    if (m_dicr & (1u << (16 + ch))) m_dicr |= 1u << (24 + ch);
    UpdateMaster();
}

// Master flag (bit 31) = force (bit 15) OR (master enable AND any enabled flag). IRQ3 on 0->1.
void Dma::UpdateMaster() {
    const bool was = IrqLine();
    const bool now = (m_dicr & 0x8000) ||
                     ((m_dicr & 0x800000) && ((m_dicr >> 16) & (m_dicr >> 24) & 0x7F));
    m_dicr = now ? (m_dicr | 0x80000000u) : (m_dicr & 0x7FFFFFFFu);
    if (!was && now && m_irq_cb) m_irq_cb();
}

void Dma::Tick(uint32_t cycles) {
    for (int ch = 0; ch < kNumChannels; ++ch) {
        Chan& c = m_ch[ch];
        if (!c.running) continue;
        if (c.paced) {
            // One block per DRQ; MADR advances and the block count (BCR 31-16) counts down.
            uint32_t bs = c.bcr & 0xFFFF;
            if (bs == 0) bs = 0x10000;
            while ((c.bcr >> 16) && c.port.request()) {
                c.madr = MoveWords(ch, c.madr & kAddrMask, bs);
                c.bcr -= 0x10000;
            }
            if (c.bcr >> 16) continue;
            c.paced = false;
            c.pending = 1;  // completes on the next Tick
            continue;
        }
        c.pending = c.pending > cycles ? c.pending - cycles : 0;
        if (c.pending == 0) Complete(ch);
    }
}

} // namespace personarm::hw
