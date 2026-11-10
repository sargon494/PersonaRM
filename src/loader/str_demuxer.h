#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// PlayStation 1 STR / MDEC Video Sector Formats (CD-ROM XA Mode 2 Form 2)
// ───────────────────────────────────────────────────────────────────────────

constexpr size_t PS1_XA_SECTOR_SIZE = 2336; // Data payload of Mode 2 Form 2 sector
constexpr uint16_t STR_MAGIC        = 0x0160;

#pragma pack(push, 1)
struct StrSectorHeader {
    uint8_t  file_number;
    uint8_t  channel_number;
    uint8_t  submode;
    uint8_t  coding_info;

    // STR Chunk metadata
    uint16_t magic;         // 0x0160
    uint16_t chunk_type;    // 0x01 = Demuxed video chunk
    uint16_t chunk_index;   // Current chunk index (0..total_chunks - 1)
    uint16_t total_chunks;  // Number of chunks that make up this frame
    uint32_t frame_number;  // Sequential video frame number
    uint32_t payload_size;  // Size of MDEC compressed bitstream in chunk
    uint16_t width;         // Video frame width (usually 320)
    uint16_t height;        // Video frame height (usually 240)
};
#pragma pack(pop)

struct StrDemuxedFrame {
    uint32_t frame_index = 0;
    uint16_t width = 320;
    uint16_t height = 240;
    std::vector<uint8_t> mdec_bitstream;
    bool complete = false;
};

class StrDemuxer {
public:
    StrDemuxer() = default;
    ~StrDemuxer() = default;

    void Reset();

    /// Feed a raw CD-ROM sector (2336 or 2352 bytes) into the demuxer.
    /// If a complete frame is assembled, returns true and populates out_frame.
    bool ProcessSector(const uint8_t* sector_data, size_t sector_size, StrDemuxedFrame& out_frame);

    uint32_t GetProcessedSectors() const { return m_sectors_processed; }
    uint32_t GetCompletedFrames() const { return m_frames_completed; }

private:
    uint32_t m_current_frame_id = 0xFFFFFFFF;
    uint16_t m_chunks_received = 0;
    uint16_t m_total_chunks = 0;
    uint16_t m_width = 320;
    uint16_t m_height = 240;

    std::vector<uint8_t> m_frame_buffer;
    uint32_t m_sectors_processed = 0;
    uint32_t m_frames_completed = 0;
};

} // namespace personarm

