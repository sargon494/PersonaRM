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

// CD-ROM XA subheader "submode" byte (PSX-SPX: bit 7 = EOF ... bit 0 = EOR).
// Verify against https://psx-spx.consoledev.net/ before relying on it.
constexpr uint8_t XA_SUBMODE_EOR      = 1u << 0; // End of Record
constexpr uint8_t XA_SUBMODE_VIDEO    = 1u << 1;
constexpr uint8_t XA_SUBMODE_AUDIO    = 1u << 2;
constexpr uint8_t XA_SUBMODE_DATA     = 1u << 3;
constexpr uint8_t XA_SUBMODE_TRIGGER  = 1u << 4;
constexpr uint8_t XA_SUBMODE_FORM2    = 1u << 5; // 0 = Form 1, 1 = Form 2
constexpr uint8_t XA_SUBMODE_REALTIME = 1u << 6;
constexpr uint8_t XA_SUBMODE_EOF      = 1u << 7; // End of File

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

// Real STR v2/v3 video sector header: the first 20h bytes of the user data (raw sector + 18h).
// Source: psx-spx "CDROM File Video Streaming STR (Sony)"; matches AF-013 on disc 1 (F0866).
// StrSectorHeader/StrDemuxer below use an older synthetic layout kept for main.cpp's self-test.
#pragma pack(push, 1)
struct StrVideoHeader {
    uint16_t status;         // 0160h
    uint16_t type;           // 8001h = MDEC
    uint16_t sector_index;   // chunk within the frame
    uint16_t sector_count;   // chunks per frame
    uint32_t frame_number;   // 1 = first
    uint32_t frame_size;     // BS bytes in the frame
    uint16_t width;
    uint16_t height;
    uint8_t bs_header_copy[12];
};
#pragma pack(pop)
static_assert(sizeof(StrVideoHeader) == 0x20, "STR header is 20h bytes");

/// Assembles BS frames from raw 2352-byte STR video sectors (7E0h payload bytes each).
class StrFrameAssembler {
public:
    static constexpr size_t kPayload = 0x7E0;
    /// Returns true when `raw2352` completes a frame: `bs` = frame_size bytes, `hdr` = its header.
    /// Non-video sectors (no 0160h/8001h header) are ignored.
    bool Push(const uint8_t* raw2352, std::vector<uint8_t>& bs, StrVideoHeader& hdr);

private:
    std::vector<uint8_t> m_buf;
    uint32_t m_frame = 0xFFFFFFFF;
    uint32_t m_have = 0;  // bitmask of received chunks (frames have < 32 chunks)
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

