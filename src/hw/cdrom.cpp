#include "hw/cdrom.h"

#include <algorithm>
#include <cstring>

#include "loader/disc_reader.h"

namespace personarm::hw {

bool DiscSectorSource::ReadSector(uint32_t lba, uint8_t* out2352) {
    return m_reader.ReadRawSector(lba, out2352);
}

namespace {
uint8_t FromBcd(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t ToBcd(uint32_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
} // namespace

// psx-spx "CDROM Controller Status/Mode": bit1 motor on, bit5 read, bit6 seek, bit7 play.
uint8_t CdRom::Stat() const {
    uint8_t s = 0;
    if (m_motor_on) s |= 0x02;
    if (m_reading) s |= 0x20;
    return s;
}

// psx-spx "CDROM Controller I/O Ports".
uint8_t CdRom::Read8(uint32_t offset) {
    switch (offset & 3) {
    case 0: {  // HSTS: index, PRMEMPT, PRMWRDY, RSLRRDY, DRQSTS, BUSYSTS (ADPBUSY always 0)
        uint8_t s = m_index;
        if (m_params.empty()) s |= 0x08;
        if (m_params.size() < 16) s |= 0x10;
        if (!m_response.empty()) s |= 0x20;
        if (DataBytesRemaining()) s |= 0x40;
        if (m_busy) s |= 0x80;
        return s;
    }
    case 1: {  // response FIFO (any index)
        if (m_response.empty()) return 0;  // unverified: real hw wraps the 16-byte buffer
        uint8_t v = m_response.front();
        m_response.pop_front();
        return v;
    }
    case 2:  // data FIFO
        return PopData();
    default:  // HINTMSK (index 0/2) or HINTSTS (index 1/3); upper 3 bits read as 1
        return static_cast<uint8_t>(((m_index & 1) ? m_irq_flags : m_irq_enable) | 0xE0);
    }
}

// unverified: 16/32-bit reads of the data port pull 2/4 FIFO bytes; other ports zero-extend.
uint16_t CdRom::Read16(uint32_t offset) {
    if ((offset & 3) != 2) return Read8(offset);
    uint16_t lo = PopData();
    return static_cast<uint16_t>(lo | (PopData() << 8));
}

uint32_t CdRom::Read32(uint32_t offset) {
    if ((offset & 3) != 2) return Read8(offset);
    return DmaRead32();
}

uint32_t CdRom::DmaRead32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(PopData()) << (8 * i);
    return v;
}

uint8_t CdRom::PopData() {
    if (m_data_pos >= m_data.size()) return 0;  // unverified: empty FIFO repeats last byte on hw
    return m_data[m_data_pos++];
}

void CdRom::Write8(uint32_t offset, uint8_t v) {
    switch (offset & 3) {
    case 0:
        m_index = v & 3;
        return;
    case 1:
        if (m_index == 0) {
            m_busy = true;
            ExecuteCommand(v);
            m_params.clear();
        }
        // shortcut: index 1-3 (sound map / CD audio volume) ignored until SPU CD input exists.
        return;
    case 2:
        if (m_index == 0) {
            if (m_params.size() < 16) m_params.push_back(v);
        } else if (m_index == 1) {
            m_irq_enable = v & 0x1F;
        }
        return;
    default:
        if (m_index == 0) {  // HCHPCTL: bit7 BFRD loads the sector buffer into the data FIFO
            if (v & 0x80) {
                // A BFRD write while data is still pending does not rewind the FIFO (DuckStation-like,
                // unverified on hw): libcd reads the 3-word header, then the game's CdGetSector sets
                // BFRD again and expects the user data that follows.
                if (!DataBytesRemaining()) LoadDataFifo();
            } else {
                m_data.clear();
                m_data_pos = 0;
            }
        } else if (m_index == 1) {  // HCLRCTL: ack bits 0-4, bit6 clears parameter FIFO
            m_irq_flags &= static_cast<uint8_t>(~(v & 0x1F));
            if (v & 0x40) m_params.clear();
        }
        return;
    }
}

void CdRom::Queue(uint32_t delay, uint8_t irq, std::vector<uint8_t> response) {
    m_events.push_back({delay, irq, std::move(response)});
}

void CdRom::Error(uint8_t code) {
    Queue(kFirstResponseCycles, 5, {static_cast<uint8_t>(Stat() | 0x01), code});
}

// psx-spx "CDROM Controller Command Summary". INT3 = first response, INT2 = second
// (complete), INT1 = data ready, INT5 = error (0x20 wrong param count, 0x40 bad command).
void CdRom::ExecuteCommand(uint8_t cmd) {
    auto need = [&](size_t n) {
        if (m_params.size() == n) return true;
        Error(0x20);
        return false;
    };
    const uint8_t stat = Stat();
    switch (cmd) {
    case 0x01:  // Getstat
        if (need(0)) Queue(kFirstResponseCycles, 3, {stat});
        break;
    case 0x02:  // Setloc amm,ass,asect (BCD)
        if (!need(3)) break;
        m_setloc_lba = (FromBcd(m_params[0]) * 60u + FromBcd(m_params[1])) * 75u + FromBcd(m_params[2]);
        m_setloc_lba = m_setloc_lba >= 150 ? m_setloc_lba - 150 : 0;
        m_setloc_pending = true;
        Queue(kFirstResponseCycles, 3, {stat});
        break;
    case 0x06:  // ReadN
    case 0x1B:  // ReadS (no retry difference modelled)
        if (!need(0)) break;
        StartReading();
        Queue(kFirstResponseCycles, 3, {Stat()});
        break;
    case 0x09: {  // Pause
        if (!need(0)) break;
        m_reading = false;
        m_sector_waiting = false;
        m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
                                      [](const Event& e) { return e.irq == 1; }),
                       m_events.end());
        Queue(kFirstResponseCycles, 3, {stat});
        // shortcut: Pause completion delay does not scale with speed (hw ~1x:0x21181C / 2x:0x10BD93).
        Queue(kSlowSecondResponseCycles, 2, {Stat()});
        break;
    }
    case 0x0A:  // Init: mode=0, motor on, abort read
        if (!need(0)) break;
        m_mode = 0;
        m_reading = false;
        m_sector_waiting = false;
        m_motor_on = true;
        m_events.clear();
        Queue(kFirstResponseCycles, 3, {stat});
        Queue(kSlowSecondResponseCycles, 2, {Stat()});
        break;
    case 0x0B:  // Mute
    case 0x0C:  // Demute
        if (!need(0)) break;
        m_muted = cmd == 0x0B;
        Queue(kFirstResponseCycles, 3, {stat});
        break;
    case 0x0D:  // Setfilter file,channel (XA-ADPCM, active with mode bit3)
        if (!need(2)) break;
        m_filter_file = m_params[0];
        m_filter_channel = m_params[1];
        Queue(kFirstResponseCycles, 3, {stat});
        break;
    case 0x0E:  // Setmode: b7 2x, b6 XA-ADPCM, b5 0x924 sectors, b4 ignore, b3 XA filter, b2 report, b1 autopause, b0 CDDA
        if (!need(1)) break;
        m_mode = m_params[0];
        Queue(kFirstResponseCycles, 3, {stat});
        break;
    case 0x0F:  // Getparam: stat, mode, 0, file, channel
        if (need(0)) Queue(kFirstResponseCycles, 3, {stat, m_mode, 0, m_filter_file, m_filter_channel});
        break;
    case 0x10:  // GetlocL: header + subheader of the current sector
        if (need(0)) Queue(kFirstResponseCycles, 3, std::vector<uint8_t>(m_delivered + 12, m_delivered + 20));
        break;
    case 0x11: {  // GetlocP: track, index, relative mm:ss:ff, absolute mm:ss:ff
        if (!need(0)) break;
        // shortcut: single data track starting at LBA 0 (track-relative == LBA); multi-track discs need the TOC.
        uint32_t rel = m_read_lba, abs = m_read_lba + 150;
        Queue(kFirstResponseCycles, 3,
              {0x01, 0x01, ToBcd(rel / 4500), ToBcd((rel / 75) % 60), ToBcd(rel % 75),
               ToBcd(abs / 4500), ToBcd((abs / 75) % 60), ToBcd(abs % 75)});
        break;
    }
    case 0x13:  // GetTN: shortcut: reports a single track
        if (need(0)) Queue(kFirstResponseCycles, 3, {stat, 0x01, 0x01});
        break;
    case 0x15:  // SeekL
    case 0x16:  // SeekP
        if (!need(0)) break;
        m_reading = false;
        m_read_lba = m_setloc_lba;
        m_setloc_pending = false;
        Queue(kFirstResponseCycles, 3, {static_cast<uint8_t>(stat | 0x40)});
        Queue(kSecondResponseCycles, 2, {Stat()});  // shortcut: seek time independent of distance
        break;
    case 0x19:  // Test
        if (m_params.size() == 1 && m_params[0] == 0x20) {
            Queue(kFirstResponseCycles, 3, {0x94, 0x09, 0x19, 0xC0});  // unverified: PU-7 BIOS date/version
        } else {
            Error(0x10);  // unverified: only sub-function 20h implemented
        }
        break;
    case 0x1A:  // GetID
        if (!need(0)) break;
        Queue(kFirstResponseCycles, 3, {stat});
        if (m_source) {
            // Licensed Mode2 data disc, region string "SCEI" (Japan).
            Queue(kSecondResponseCycles, 2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'I'});
        } else {
            Queue(kSecondResponseCycles, 5, {0x08, 0x40, 0, 0, 0, 0, 0, 0});  // no disc
        }
        break;
    case 0x1E:  // ReadTOC
        if (!need(0)) break;
        Queue(kFirstResponseCycles, 3, {stat});
        Queue(kSlowSecondResponseCycles, 2, {Stat()});
        break;
    default:
        Error(0x40);
        break;
    }
}

void CdRom::StartReading() {
    if (m_setloc_pending) {
        m_read_lba = m_setloc_lba;
        m_setloc_pending = false;
    }
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
                                  [](const Event& e) { return e.irq == 1; }),
                   m_events.end());
    m_sector_waiting = false;
    m_reading = true;
    // shortcut: seek time folded into one extra sector period.
    m_sector_timer = static_cast<int64_t>(SectorCycles()) * 2;
}

void CdRom::ReadNextSector() {
    if (!m_source || !m_source->ReadSector(m_read_lba, m_sector)) {
        m_reading = false;
        Queue(0, 5, {static_cast<uint8_t>(Stat() | 0x01), 0x04});  // unverified: error code for read past end
        return;
    }
    ++m_read_lba;
    // XA-ADPCM (mode bit6): audio sectors (submode bit2) go to the SPU, never to the host.
    // With the filter (mode bit3) only the matching file/channel is played; others are dropped.
    const uint8_t file = m_sector[16], channel = m_sector[17], submode = m_sector[18];
    if ((m_mode & 0x40) && (submode & 0x04)) {
        bool match = !(m_mode & 0x08) || (file == m_filter_file && channel == m_filter_channel);
        if (match && m_xa_cb && !m_muted) m_xa_cb(m_sector);  // shortcut: no ADPCM decode here
        return;
    }
    Queue(0, 1, {Stat()});
    m_sector_waiting = true;
}

void CdRom::LoadDataFifo() {
    // Setmode bit5: 0x924 bytes from offset 12 (header onward), else 0x800 user data from offset 24.
    const bool whole = (m_mode & 0x20) != 0;
    const uint8_t* begin = m_delivered + (whole ? 12 : 24);
    m_data.assign(begin, begin + (whole ? 0x924 : 0x800));
    m_data_pos = 0;
}

uint64_t CdRom::CyclesToNextEvent() const {
    uint64_t e = UINT64_MAX;
    // The front response counts down even behind an unacknowledged interrupt, but only the
    // acknowledge (a register write) can let it out then.
    if (!m_events.empty() && (m_irq_flags & 7) == 0) e = std::max<uint64_t>(1, m_events.front().delay);
    // While a sector waits for its INT1, reading resumes only in the tick that delivers it.
    if (m_reading && !m_sector_waiting) e = std::min<uint64_t>(e, m_sector_timer > 0 ? uint64_t(m_sector_timer) : 1);
    return e;
}

void CdRom::Tick(uint32_t cycles) {
    if (!m_events.empty()) {
        Event& e = m_events.front();
        e.delay = e.delay > cycles ? e.delay - cycles : 0;
        // Responses queue behind an unacknowledged interrupt (psx-spx "Response/Data Queueing").
        if (e.delay == 0 && (m_irq_flags & 7) == 0) {
            m_response.assign(e.response.begin(), e.response.end());
            m_irq_flags = static_cast<uint8_t>((m_irq_flags & ~7) | e.irq);
            if (e.irq == 1) {
                std::memcpy(m_delivered, m_sector, kSectorBytes);
                m_sector_waiting = false;
            }
            if (e.irq == 3 || e.irq == 5) m_busy = false;
            m_events.pop_front();
            if (IrqPending() && m_irq_cb) m_irq_cb();
        }
    }
    if (m_reading) {
        m_sector_timer -= cycles;
        if (m_sector_timer <= 0) {
            if (m_sector_waiting) {
                // shortcut: the drive stalls until the host takes the sector instead of overrunning it.
                m_sector_timer = 0;
            } else {
                ReadNextSector();
                m_sector_timer = SectorCycles();
            }
        }
    }
}

} // namespace personarm::hw
