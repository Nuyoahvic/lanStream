#pragma once
#include <cstdint>

#pragma pack(push, 1)

struct PacketHeader
{
    uint32_t magic;          // 0x4C534C41 "LSLA"
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint16_t chunk_index;    // Current shard index
    uint16_t chunk_count;    // Total number of fragments for this frame
    uint32_t payload_size;   // Number of bytes in this fragment's payload
    uint8_t  is_keyframe;
    uint8_t  pad[3];
};
#pragma pack(pop)

static constexpr uint32_t PACKET_MAGIC = 0x4C534C41u;
static constexpr int      UDP_MTU      = 1400;
static constexpr int      MAX_PAYLOAD  = UDP_MTU - (int)sizeof(PacketHeader);
