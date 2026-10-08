#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <chrono>

enum class PixelFormat
{
    BGRA,
    NV12,
    YUV420P
};

static inline uint64_t now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()).count();
}

struct RawFrame
{
    uint64_t   timestamp_us = 0;
    int        width        = 0;
    int        height       = 0;
    int        linesize     = 0;   // bytes per row
    PixelFormat format      = PixelFormat::BGRA;
    std::vector<uint8_t> data;
};

struct EncodedPacket
{
    uint32_t frame_id     = 0;
    uint64_t timestamp_us = 0;
    bool     is_keyframe  = false;
    std::vector<uint8_t> data;
};

using RawFramePtr     = std::shared_ptr<RawFrame>;
using EncodedPacketPtr = std::shared_ptr<EncodedPacket>;
