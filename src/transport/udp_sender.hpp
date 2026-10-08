// udp_sender.hpp
#pragma once
#include "common/frame.hpp"
#include "transport/protocol.hpp"
#include <string>

class UdpSender
{
public:
    ~UdpSender();
    bool init(const std::string& ip, uint16_t port);
    void send(const EncodedPacketPtr& pkt);
    void close();
private:
    int sock_ = -1;
};
