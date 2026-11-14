#pragma once
// PS1 CD-ROM controller (0x1F801800..0x1F801803, index-banked registers).
// Source: psx-spx "CDROM Controller I/O Ports", "CDROM Controller Command Summary",
// "CDROM - Response/Data Queueing" and "CDROM Controller Status/Mode" (HW-07x in
// docs/research/PS1_HARDWARE_REQUIREMENTS.md). Anything recalled without re-reading
// the source is marked `unverified:` in cdrom.cpp.
//
// WIRING (done in Engine::WireDevices, src/engine/engine.cpp):
//   1. Own a `hw::DiscSectorSource src(disc_reader)` and `hw::CdRom cd(&src)`.
//   2. Bus: for addr in [CdRom::kBase, kBase+3] route Read8/Write8 (and 16/32) with
//      offset = addr - CdRom::kBase.
//   3. Call cd.Tick(cycles) with the CPU cycles elapsed each step/batch.
//   4. cd.SetIrqCallback([&]{ i_stat |= 1u << 2; });  // IRQ2 = CDROM in I_STAT
//   5. DMA3: dma.SetPort(Dma::kCdrom, {[&]{ return cd.DmaRead32(); }, nullptr}).
//   6. Optional: cd.SetXaCallback(...) to feed XA-ADPCM sectors to the SPU.
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace personarm {
class DiscReader;
}

namespace personarm::hw {

/// Raw 2352-byte sector provider (sync + header + subheader + data).
class SectorSource {
public:
    virtual ~SectorSource() = default;
    virtual bool ReadSector(uint32_t lba, uint8_t* out2352) = 0;
};

/// SectorSource backed by the existing DiscReader (.bin/.cue/.iso parsing lives there).
class DiscSectorSource : public SectorSource {
public:
    explicit DiscSectorSource(DiscReader& reader) : m_reader(reader) {}
    bool ReadSector(uint32_t lba, uint8_t* out2352) override;

private:
    DiscReader& m_reader;
};

class CdRom {
public:
    static constexpr uint32_t kBase = 0x1F801800;
    static constexpr uint32_t kSectorBytes = 2352;
    // Cycle timings at 33.8688 MHz. 75 sectors/s at 1x, 150 at 2x (psx-spx).
    static constexpr uint32_t kCyclesPerSector1x = 33868800 / 75;
    static constexpr uint32_t kCyclesPerSector2x = 33868800 / 150;
    // shortcut: fixed first-response delay (real hw varies per command/state), tune with a reference trace.
    static constexpr uint32_t kFirstResponseCycles = 0xC4E1; // unverified: common emulator average
    // shortcut: fixed second-response delays for GetID/Init/Pause/SeekL/ReadTOC; real ones depend on seek distance/speed.
    static constexpr uint32_t kSecondResponseCycles = 0x4A00;
    static constexpr uint32_t kSlowSecondResponseCycles = 0x1000000 / 8; // Init/ReadTOC/Pause, roughly

    explicit CdRom(SectorSource* source = nullptr) : m_source(source) {}

    void SetSectorSource(SectorSource* source) { m_source = source; }
    void SetIrqCallback(std::function<void()> cb) { m_irq_cb = std::move(cb); }
    void SetXaCallback(std::function<void(const uint8_t* raw2352)> cb) { m_xa_cb = std::move(cb); }

    uint8_t Read8(uint32_t offset);
    uint16_t Read16(uint32_t offset);
    uint32_t Read32(uint32_t offset);
    void Write8(uint32_t offset, uint8_t value);
    void Write16(uint32_t offset, uint16_t value) { Write8(offset, static_cast<uint8_t>(value)); }
    void Write32(uint32_t offset, uint32_t value) { Write8(offset, static_cast<uint8_t>(value)); }

    void Tick(uint32_t cycles);
    /// CPU cycles until Tick() may act on its own (deliver a response, read a sector); UINT64_MAX when
    /// only a register access can start anything. A host may batch ticks up to (at least) this point.
    uint64_t CyclesToNextEvent() const;

    /// DMA3 (CDROM -> RAM): pops 4 bytes from the data FIFO, little-endian.
    uint32_t DmaRead32();

    /// True while (IRQ flags & IRQ enable) != 0, i.e. the line into I_STAT bit 2.
    bool IrqPending() const { return (m_irq_flags & m_irq_enable & 0x1F) != 0; }
    uint8_t Stat() const;
    uint8_t Mode() const { return m_mode; }
    uint32_t DataBytesRemaining() const { return static_cast<uint32_t>(m_data.size() - m_data_pos); }

private:
    struct Event {
        uint32_t delay;
        uint8_t irq;  // 1..5
        std::vector<uint8_t> response;
    };

    void ExecuteCommand(uint8_t cmd);
    void Queue(uint32_t delay, uint8_t irq, std::vector<uint8_t> response);
    void Error(uint8_t code);
    void StartReading();
    void ReadNextSector();
    void LoadDataFifo();
    uint8_t PopData();
    uint32_t SectorCycles() const { return (m_mode & 0x80) ? kCyclesPerSector2x : kCyclesPerSector1x; }

    SectorSource* m_source = nullptr;
    std::function<void()> m_irq_cb;
    std::function<void(const uint8_t*)> m_xa_cb;

    uint8_t m_index = 0;
    uint8_t m_irq_enable = 0;
    uint8_t m_irq_flags = 0;
    std::deque<uint8_t> m_params;
    std::deque<uint8_t> m_response;
    std::vector<uint8_t> m_data;
    size_t m_data_pos = 0;
    std::deque<Event> m_events;  // delivered in order, one at a time (next waits for ack)
    bool m_busy = false;

    uint8_t m_mode = 0;
    uint8_t m_filter_file = 0;
    uint8_t m_filter_channel = 0;
    bool m_muted = false;
    bool m_motor_on = true;
    bool m_reading = false;
    bool m_setloc_pending = false;
    uint32_t m_setloc_lba = 0;
    uint32_t m_read_lba = 0;
    int64_t m_sector_timer = 0;
    bool m_sector_waiting = false;  // INT1 queued but not yet delivered
    uint8_t m_sector[kSectorBytes] = {};     // last sector read from disc
    uint8_t m_delivered[kSectorBytes] = {};  // sector of the last delivered INT1
};

} // namespace personarm::hw
