#pragma once
#include "common/frame.hpp"
#include <functional>

class IEncoder
{
public:
    using PacketCallback = std::function<void(EncodedPacketPtr)>;
    virtual ~IEncoder() = default;
    virtual bool init(int width, int height,
                      int fps = 60, int bitrate_kbps = 8000) = 0;
    virtual void encode(RawFramePtr frame, PacketCallback cb) = 0;
    virtual void flush(PacketCallback cb)                     = 0;
};
