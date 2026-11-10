#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <memory>

namespace personarm {

class Engine;

// Standard PS1 Memory Card Geometry
constexpr size_t PS1_MEMCARD_BLOCK_SIZE = 8192;   // 8 KB per block
constexpr size_t PS1_MEMCARD_NUM_BLOCKS = 16;     // 1 Directory block + 15 Save blocks
constexpr size_t PS1_MEMCARD_TOTAL_SIZE = PS1_MEMCARD_BLOCK_SIZE * PS1_MEMCARD_NUM_BLOCKS; // 128 KB (131072 bytes)
constexpr size_t PS1_MEMCARD_USER_SLOTS = 15;     // 15 user save slots

#pragma pack(push, 1)
struct Ps1SaveHeader {
    char magic[2];              // "SC" (0x53, 0x43)
    uint8_t icon_flags;         // Icon display mode (0x11 = 1 frame static)
    uint8_t block_count;        // Number of 8 KB blocks (usually 1)
    char title_sjis[64];        // Game title in Shift-JIS or ASCII
    uint8_t reserved[28];
    uint16_t icon_clut[16];     // 16-color 15-bit RGB palette
};
#pragma pack(pop)

struct SaveSlotInfo {
    uint32_t slot_index = 0;    // 0..14
    bool is_in_use = false;
    std::string title;
    std::string product_code;   // e.g. "BASLUS-01158"
    uint32_t data_size = 0;
};

// ───────────────────────────────────────────────────────────────────────────
// SaveManager — Virtual PS1 Memory Card & QuickSave / QuickLoad System
// ───────────────────────────────────────────────────────────────────────────

class SaveManager {
public:
    SaveManager();
    ~SaveManager() = default;

    /// Formats a pristine 128 KB PlayStation 1 Memory Card image
    void FormatMemoryCard();

    /// Loads raw 128 KB memory card file (.mcr / .srm / .mcd)
    bool LoadMemoryCard(const std::string& path);

    /// Saves raw 128 KB memory card image to disk
    bool SaveMemoryCard(const std::string& path) const;

    /// Reads save block payload from memory card (slot: 0..14)
    bool ReadSlot(uint32_t slot, std::vector<uint8_t>& out_payload) const;

    /// Writes save block payload with standard PS1 header (slot: 0..14)
    bool WriteSlot(uint32_t slot, const std::string& title,
                   const std::string& product_code,
                   const uint8_t* data, size_t size);

    /// List all 15 slots and their statuses
    std::vector<SaveSlotInfo> ListSlots() const;

    /// QuickSave: Serializes engine state into snapshot buffer
    bool CaptureQuickSave(const Engine& engine, std::vector<uint8_t>& out_snapshot);

    /// QuickLoad: Restores engine state from snapshot buffer
    bool RestoreQuickSave(Engine& engine, const uint8_t* snapshot_data, size_t size);

    const std::vector<uint8_t>& GetCardData() const { return m_card_data; }
    bool IsFormatted() const { return m_is_formatted; }

private:
    std::vector<uint8_t> m_card_data;
    bool m_is_formatted = false;
};

} // namespace personarm
