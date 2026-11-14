#include "core/save_manager.h"
#include "engine/engine.h"
#include "core/logger.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <chrono>
#include <algorithm>

namespace personarm {

namespace {

// Helper: Compute XOR checksum of a 128-byte frame/sector
uint8_t ComputeXorChecksum(const uint8_t* data, size_t len) {
    uint8_t chk = 0;
    for (size_t i = 0; i < len; ++i) {
        chk ^= data[i];
    }
    return chk;
}

#pragma pack(push, 1)
struct MemCardDirectoryEntry {
    uint32_t alloc_state;       // 0xA0 = Free, 0x51 = Allocated first block
    uint32_t file_size;         // Size in bytes (multiple of 8192)
    uint16_t next_block;        // 0xFFFF = End of file
    char filename[21];          // e.g. "BASLUS-01158GAME"
    uint8_t padding[96];
    uint8_t checksum;           // XOR of bytes 0..126
};

struct QuickSaveHeader {
    char magic[8];              // "PRMSTATE"
    uint32_t version;           // 1
    uint64_t frame_count;
    uint32_t ram_size;
    uint32_t scratchpad_size;
    uint32_t vram_size;
    CpuRegisters cpu_regs;
};
#pragma pack(pop)

} // anonymous namespace

// ── MemoryCard ──────────────────────────────────────────────────────────────

void MemoryCard::Format() {
    m_data.assign(kSize, 0);
    auto frame = [&](uint32_t n) { return &m_data[size_t(n) * kFrameSize]; };
    auto seal = [&](uint32_t n) { frame(n)[127] = ComputeXorChecksum(frame(n), 127); };
    // psx-spx "Memory Card Data Format": frame 0 header, 1-15 free directory entries, 16-35 broken
    // sector list (none), 36-62 FFh-filled, 63 write-test frame = copy of frame 0.
    frame(0)[0] = 'M'; frame(0)[1] = 'C'; seal(0);
    for (uint32_t n = 1; n < 16; ++n) {
        frame(n)[0] = 0xA0;
        frame(n)[8] = frame(n)[9] = 0xFF;  // no next block
        seal(n);
    }
    for (uint32_t n = 16; n < 36; ++n) {
        std::memset(frame(n), 0xFF, 4);
        frame(n)[8] = frame(n)[9] = 0xFF;
        seal(n);
    }
    for (uint32_t n = 36; n < 63; ++n) std::memset(frame(n), 0xFF, kFrameSize);
    std::memcpy(frame(63), frame(0), kFrameSize);
    m_dirty = true;
}

bool MemoryCard::Open(const std::string& path, std::string* err) {
    m_path = path;
    m_flag = 0x08;
    m_step = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (std::ifstream(path + ".tmp", std::ios::binary)) {
            // A crash between writing and renaming a new card: do not format over what may be a save.
            if (err) *err = path + " is missing but " + path + ".tmp exists (interrupted write?): rename it to use it";
            return false;
        }
        Format();
        if (!Flush()) { if (err) *err = "cannot create " + path; return false; }
        PRM_LOG_INFO("MEMCARD", "created and formatted " + path);
        return true;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() != kSize) {
        if (err) *err = path + ": " + std::to_string(data.size()) + " bytes, a raw card image has 131072";
        return false;
    }
    m_data = std::move(data);
    m_dirty = false;
    PRM_LOG_INFO("MEMCARD", "loaded " + path);
    return true;
}

bool MemoryCard::Flush() {
    if (!m_dirty || m_path.empty()) return true;
    const std::string tmp = m_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(m_data.data()), std::streamsize(m_data.size()));
        out.flush();
        if (!out) { PRM_LOG_ERROR("MEMCARD", "cannot write " + tmp); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, m_path, ec);  // replaces the old image in one step
    if (ec) { PRM_LOG_ERROR("MEMCARD", "cannot replace " + m_path + ": " + ec.message()); return false; }
    m_dirty = false;
    return true;
}

void MemoryCard::WriteFrame(uint32_t n, const uint8_t* src) {
    uint8_t* dst = &m_data[size_t(n % kFrames) * kFrameSize];
    if (std::memcmp(dst, src, kFrameSize)) { std::memcpy(dst, src, kFrameSize); m_dirty = true; }
    m_flag &= ~0x08;
}

// psx-spx "Memory Card Read/Write Commands". Steps count bytes after the 81h address byte.
// Read  'R': FLAG 5A 5D (MSB) (LSB) 5C 5D MSB LSB data*128 CHK 47
// Write 'W': FLAG 5A 5D (MSB) (LSB) data*128 CHK 5C 5D 47/4E/FF
// ID    'S': FLAG 5A 5D 5C 5D 04 00 00 80
// The last byte of each command gets no /ACK. A bad sector reads back address FFFFh and aborts.
// shortcut: no 31000-cycle extra delay after Read byte 7 (Sony cards); /ACK timing is the port's.
bool MemoryCard::Exchange(uint8_t tx, uint8_t& rx) {
    rx = 0xFF;
    if (m_step >= 0xFFFF) return false;  // aborted or not addressed: silent until the next select
    const uint32_t s = m_step++;
    const uint8_t prev = m_prev;
    m_prev = tx;
    if (s == 0) { if (tx != 0x81) m_step = 0xFFFF; return tx == 0x81; }  // not for us: stay silent
    if (s == 1) {
        m_cmd = tx;
        rx = m_flag;
        if (tx != 'R' && tx != 'W' && tx != 'S') { m_step = 0xFFFF; return false; }
        return true;
    }
    if (s == 2) { rx = 0x5A; return true; }
    if (s == 3) { rx = 0x5D; return true; }
    if (m_cmd == 'S') {
        static const uint8_t kId[] = {0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};
        if (s - 4 >= sizeof(kId)) return false;
        rx = kId[s - 4];
        return s - 4 < sizeof(kId) - 1;
    }
    if (s == 4) { rx = 0x00; m_sector = uint16_t(tx << 8); return true; }
    if (s == 5) { rx = prev; m_sector |= tx; m_chk = uint8_t((m_sector >> 8) ^ (m_sector & 0xFF)); return true; }
    const bool valid = m_sector < kFrames;
    if (m_cmd == 'R') {
        if (s == 6) { rx = 0x5C; return true; }
        if (s == 7) { rx = 0x5D; return true; }
        if (s == 8) { rx = valid ? uint8_t(m_sector >> 8) : 0xFF; return true; }
        if (s == 9) { rx = valid ? uint8_t(m_sector) : 0xFF; return valid; }  // bad sector: abort here
        if (!valid) return false;
        if (s < 10 + kFrameSize) { rx = Frame(m_sector)[s - 10]; m_chk ^= rx; return true; }
        if (s == 10 + kFrameSize) { rx = m_chk; return true; }
        if (s == 11 + kFrameSize) rx = 0x47;
        return false;
    }
    // Write
    if (s < 6 + kFrameSize) { rx = prev; m_buf[s - 6] = tx; m_chk ^= tx; return true; }
    if (s == 6 + kFrameSize) { rx = prev; m_chk ^= tx; return true; }  // m_chk == 0 when the checksum matched
    if (s == 7 + kFrameSize) { rx = 0x5C; return true; }
    if (s == 8 + kFrameSize) { rx = 0x5D; return true; }
    if (s == 9 + kFrameSize) {
        if (!valid) rx = 0xFF;
        else if (m_chk) rx = 0x4E;
        else { WriteFrame(m_sector, m_buf.data()); rx = 0x47; }
    }
    return false;
}

SaveManager::SaveManager() {
    FormatMemoryCard();
}

void SaveManager::FormatMemoryCard() {
    m_card_data.assign(PS1_MEMCARD_TOTAL_SIZE, 0);

    // Block 0, Sector 0: Header frame
    m_card_data[0] = 'M';
    m_card_data[1] = 'C';
    m_card_data[127] = ComputeXorChecksum(m_card_data.data(), 127);

    // Block 0, Sectors 1..15: 15 unallocated directory frames
    for (uint32_t slot = 0; slot < PS1_MEMCARD_USER_SLOTS; ++slot) {
        size_t entry_offset = (slot + 1) * 128;
        MemCardDirectoryEntry entry{};
        entry.alloc_state = 0xA0; // Free / available
        entry.file_size = 0;
        entry.next_block = 0xFFFF;
        entry.checksum = ComputeXorChecksum(reinterpret_cast<const uint8_t*>(&entry), 127);

        std::memcpy(&m_card_data[entry_offset], &entry, sizeof(entry));
    }

    m_is_formatted = true;
    PRM_LOG_INFO("SAVE", "Formated new 128 KB PlayStation 1 virtual Memory Card.");
}

bool SaveManager::LoadMemoryCard(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        PRM_LOG_ERROR("SAVE", "Failed to open memory card file: " + path);
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (size != PS1_MEMCARD_TOTAL_SIZE) {
        PRM_LOG_ERROR("SAVE", "Invalid memory card file size (" + std::to_string(size) + " bytes; expected 131072)");
        return false;
    }

    m_card_data.resize(PS1_MEMCARD_TOTAL_SIZE);
    file.read(reinterpret_cast<char*>(m_card_data.data()), PS1_MEMCARD_TOTAL_SIZE);

    if (m_card_data[0] != 'M' || m_card_data[1] != 'C') {
        PRM_LOG_WARN("SAVE", "Memory card header missing 'MC' magic; formatting card.");
        FormatMemoryCard();
        return false;
    }

    m_is_formatted = true;
    PRM_LOG_INFO("SAVE", "Loaded 128 KB Memory Card image from " + path);
    return true;
}

bool SaveManager::SaveMemoryCard(const std::string& path) const {
    if (!m_is_formatted || m_card_data.size() != PS1_MEMCARD_TOTAL_SIZE) {
        return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        PRM_LOG_ERROR("SAVE", "Failed to write memory card to: " + path);
        return false;
    }

    file.write(reinterpret_cast<const char*>(m_card_data.data()), PS1_MEMCARD_TOTAL_SIZE);
    return true;
}

bool SaveManager::ReadSlot(uint32_t slot, std::vector<uint8_t>& out_payload) const {
    if (slot >= PS1_MEMCARD_USER_SLOTS || !m_is_formatted) {
        return false;
    }

    // Inspect directory entry in Block 0
    size_t entry_offset = (slot + 1) * 128;
    const auto* entry = reinterpret_cast<const MemCardDirectoryEntry*>(&m_card_data[entry_offset]);
    if (entry->alloc_state != 0x51) {
        return false; // Slot empty
    }

    // Read Block (slot + 1)
    size_t block_offset = (slot + 1) * PS1_MEMCARD_BLOCK_SIZE;
    const auto* header = reinterpret_cast<const Ps1SaveHeader*>(&m_card_data[block_offset]);
    if (header->magic[0] != 'S' || header->magic[1] != 'C') {
        return false;
    }

    size_t header_size = sizeof(Ps1SaveHeader);
    size_t payload_size = PS1_MEMCARD_BLOCK_SIZE - header_size;
    out_payload.resize(payload_size);
    std::memcpy(out_payload.data(), &m_card_data[block_offset + header_size], payload_size);
    return true;
}

bool SaveManager::WriteSlot(uint32_t slot, const std::string& title,
                            const std::string& product_code,
                            const uint8_t* data, size_t size) {
    if (slot >= PS1_MEMCARD_USER_SLOTS || !m_is_formatted) {
        return false;
    }

    // Update Directory entry in Block 0
    size_t entry_offset = (slot + 1) * 128;
    MemCardDirectoryEntry entry{};
    entry.alloc_state = 0x51; // In use
    entry.file_size = static_cast<uint32_t>(PS1_MEMCARD_BLOCK_SIZE);
    entry.next_block = 0xFFFF;

    std::strncpy(entry.filename, product_code.c_str(), sizeof(entry.filename) - 1);
    entry.checksum = ComputeXorChecksum(reinterpret_cast<const uint8_t*>(&entry), 127);
    std::memcpy(&m_card_data[entry_offset], &entry, sizeof(entry));

    // Write Block (slot + 1)
    size_t block_offset = (slot + 1) * PS1_MEMCARD_BLOCK_SIZE;
    Ps1SaveHeader header{};
    header.magic[0] = 'S';
    header.magic[1] = 'C';
    header.icon_flags = 0x11; // 1 icon static
    header.block_count = 1;
    std::strncpy(header.title_sjis, title.c_str(), sizeof(header.title_sjis) - 1);

    // Palette: basic gray scale palette
    for (int i = 0; i < 16; ++i) {
        uint16_t gray = static_cast<uint16_t>((i * 2) & 0x1F);
        header.icon_clut[i] = gray | (gray << 5) | (gray << 10);
    }

    std::memcpy(&m_card_data[block_offset], &header, sizeof(header));

    size_t header_size = sizeof(header);
    size_t max_payload = PS1_MEMCARD_BLOCK_SIZE - header_size;
    size_t write_len = std::min(size, max_payload);

    if (data && write_len > 0) {
        std::memcpy(&m_card_data[block_offset + header_size], data, write_len);
    }

    PRM_LOG_INFO("SAVE", "Saved slot " + std::to_string(slot) + " (" + title + ", ID: " + product_code + ")");
    return true;
}

std::vector<SaveSlotInfo> SaveManager::ListSlots() const {
    std::vector<SaveSlotInfo> slots;
    slots.reserve(PS1_MEMCARD_USER_SLOTS);

    for (uint32_t i = 0; i < PS1_MEMCARD_USER_SLOTS; ++i) {
        SaveSlotInfo info{};
        info.slot_index = i;

        size_t entry_offset = (i + 1) * 128;
        const auto* entry = reinterpret_cast<const MemCardDirectoryEntry*>(&m_card_data[entry_offset]);

        if (entry->alloc_state == 0x51) {
            info.is_in_use = true;
            info.product_code = entry->filename;
            info.data_size = entry->file_size;

            size_t block_offset = (i + 1) * PS1_MEMCARD_BLOCK_SIZE;
            const auto* header = reinterpret_cast<const Ps1SaveHeader*>(&m_card_data[block_offset]);
            if (header->magic[0] == 'S' && header->magic[1] == 'C') {
                info.title = header->title_sjis;
            }
        }
        slots.push_back(info);
    }

    return slots;
}

bool SaveManager::CaptureQuickSave(const Engine& engine, std::vector<uint8_t>& out_snapshot) {
    const auto& ram = engine.GetRam();
    const auto& scratchpad = engine.GetScratchpad();
    const auto& vram = engine.GetGpu().GetVram();

    QuickSaveHeader hdr{};
    std::memcpy(hdr.magic, "PRMSTATE", 8);
    hdr.version = 1;
    hdr.frame_count = engine.GetFrameCount();
    hdr.ram_size = static_cast<uint32_t>(ram.size());
    hdr.scratchpad_size = static_cast<uint32_t>(scratchpad.size());
    hdr.vram_size = static_cast<uint32_t>(vram.size() * sizeof(uint16_t));
    hdr.cpu_regs = engine.GetCpu().GetRegisters();

    size_t total_size = sizeof(hdr) + hdr.ram_size + hdr.scratchpad_size + hdr.vram_size;
    out_snapshot.resize(total_size);

    size_t offset = 0;
    std::memcpy(&out_snapshot[offset], &hdr, sizeof(hdr));
    offset += sizeof(hdr);

    std::memcpy(&out_snapshot[offset], ram.data(), hdr.ram_size);
    offset += hdr.ram_size;

    std::memcpy(&out_snapshot[offset], scratchpad.data(), hdr.scratchpad_size);
    offset += hdr.scratchpad_size;

    std::memcpy(&out_snapshot[offset], vram.data(), hdr.vram_size);

    PRM_LOG_INFO("SAVE", "QuickSave snapshot captured at frame " + std::to_string(hdr.frame_count));
    return true;
}

bool SaveManager::RestoreQuickSave(Engine& engine, const uint8_t* snapshot_data, size_t size) {
    if (!snapshot_data || size < sizeof(QuickSaveHeader)) {
        PRM_LOG_ERROR("SAVE", "Corrupted or undersized QuickSave buffer.");
        return false;
    }

    QuickSaveHeader hdr{};
    std::memcpy(&hdr, snapshot_data, sizeof(hdr));

    if (std::memcmp(hdr.magic, "PRMSTATE", 8) != 0 || hdr.version != 1) {
        PRM_LOG_ERROR("SAVE", "Invalid QuickSave header magic or version.");
        return false;
    }

    size_t expected_size = sizeof(hdr) + hdr.ram_size + hdr.scratchpad_size + hdr.vram_size;
    if (size < expected_size) {
        PRM_LOG_ERROR("SAVE", "Incomplete QuickSave snapshot data.");
        return false;
    }

    size_t offset = sizeof(hdr);

    std::vector<uint8_t> ram(hdr.ram_size);
    std::memcpy(ram.data(), &snapshot_data[offset], hdr.ram_size);
    engine.SetRam(ram);
    offset += hdr.ram_size;

    std::vector<uint8_t> scratchpad(hdr.scratchpad_size);
    std::memcpy(scratchpad.data(), &snapshot_data[offset], hdr.scratchpad_size);
    engine.SetScratchpad(scratchpad);
    offset += hdr.scratchpad_size;

    std::vector<uint16_t> vram(hdr.vram_size / sizeof(uint16_t));
    std::memcpy(vram.data(), &snapshot_data[offset], hdr.vram_size);
    engine.GetGpu().SetVram(vram);

    engine.GetCpu().SetRegisters(hdr.cpu_regs);
    engine.SetFrameCount(hdr.frame_count);

    PRM_LOG_INFO("SAVE", "QuickSave restored successfully. Current frame: " + std::to_string(hdr.frame_count));
    return true;
}

} // namespace personarm
