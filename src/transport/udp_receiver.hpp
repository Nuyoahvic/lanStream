// udp_receiver.hpp
#pragma once
#include "common/frame.hpp"
#include "transport/protocol.hpp"
#include <functional>
#include <thread>
#include <atomic>
#include <map>
#include <vector>

class UdpReceiver
{
public:
    using PacketCallback = std::function<void(EncodedPacketPtr)>;
    ~UdpReceiver();
    bool init(uint16_t port);
    bool start(PacketCallback cb);
    void stop();
private:
    void recv_loop(PacketCallback cb);

    struct FrameAsm
            {
        uint32_t frame_id    = 0;
        uint64_t ts          = 0;
        uint8_t  keyframe    = 0;
        uint16_t expect      = 0;
        std::map<uint16_t, std::vector<uint8_t>> chunks;
        bool complete() const;
    };

    int               sock_  = -1;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::map<uint32_t, FrameAsm> asm_;
    uint32_t last_id_ = UINT32_MAX;
};
