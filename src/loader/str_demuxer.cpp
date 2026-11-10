#include "loader/str_demuxer.h"
#include "core/logger.h"
#include <cstring>
#include <algorithm>

namespace personarm {

void StrDemuxer::Reset() {
    m_current_frame_id = 0xFFFFFFFF;
    m_chunks_received = 0;
    m_total_chunks = 0;
    m_width = 320;
    m_height = 240;
    m_frame_buffer.clear();
    m_sectors_processed = 0;
    m_frames_completed = 0;
}

bool StrDemuxer::ProcessSector(const uint8_t* sector_data, size_t sector_size, StrDemuxedFrame& out_frame) {
    m_sectors_processed++;

    if (!sector_data || sector_size < sizeof(StrSectorHeader)) {
        return false;
    }

    // Offset into sector where STR header starts
    // Mode 2 Form 2 can have 16 or 24 byte sub-headers depending on raw disc sector (2352 vs 2336)
    size_t header_offset = 0;
    if (sector_size == 2352) {
        header_offset = 24; // 12-byte sync + 4-byte header + 8-byte subheader
    } else if (sector_size >= 2336) {
        header_offset = 0;
    }

    if (header_offset + sizeof(StrSectorHeader) > sector_size) {
        return false;
    }

    const auto* header = reinterpret_cast<const StrSectorHeader*>(sector_data + header_offset);

    // Validate STR Magic 0x0160
    if (header->magic != STR_MAGIC) {
        return false;
    }

    uint32_t frame_num = header->frame_number;
    uint16_t chunk_idx = header->chunk_index;
    uint16_t total_chunks = header->total_chunks;

    // Check if new frame started
    if (frame_num != m_current_frame_id) {
        m_current_frame_id = frame_num;
        m_chunks_received = 0;
        m_total_chunks = total_chunks;
        m_width = header->width > 0 ? header->width : 320;
        m_height = header->height > 0 ? header->height : 240;
        m_frame_buffer.clear();
    }

    // Append chunk payload to assembled frame
    const uint8_t* payload = sector_data + header_offset + sizeof(StrSectorHeader);
    size_t available_payload = sector_size - (header_offset + sizeof(StrSectorHeader));
    size_t chunk_bytes = std::min<size_t>(header->payload_size, available_payload);

    m_frame_buffer.insert(m_frame_buffer.end(), payload, payload + chunk_bytes);
    m_chunks_received++;

    // Check if all chunks received for current frame
    if (m_chunks_received >= m_total_chunks && m_total_chunks > 0) {
        out_frame.frame_index = m_current_frame_id;
        out_frame.width = m_width;
        out_frame.height = m_height;
        out_frame.mdec_bitstream = std::move(m_frame_buffer);
        out_frame.complete = true;

        m_frames_completed++;
        m_frame_buffer.clear();
        m_chunks_received = 0;

        PRM_LOG_DEBUG("STR", "Assembled complete STR frame #" + std::to_string(out_frame.frame_index) +
                             " (" + std::to_string(out_frame.mdec_bitstream.size()) + " bytes, " +
                             std::to_string(m_total_chunks) + " chunks)");
        return true;
    }

    return false;
}

} // namespace personarm

