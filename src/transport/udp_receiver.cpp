// udp_receiver.cpp
#include "udp_receiver.hpp"
#include <cstring>
#include <iostream>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

bool UdpReceiver::init(uint16_t port)
{
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2,2), &wd);
#endif
    sock_ = (int)::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0)
        return false;

    // Set a receive timeout to prevent recv from blocking after stop()
#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
#else
    struct timeval tv{0, 100000};
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sock_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "[udp_receiver] bind failed" << std::endl;
        return false;
    }
    return true;
}

UdpReceiver::~UdpReceiver()
{
    stop();
}

bool UdpReceiver::start(PacketCallback cb)
{
    running_ = true;
    thread_ = std::thread([this, cb]
            {
        recv_loop(cb);
            });
    return true;
}

void UdpReceiver::stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
#ifdef _WIN32
    if (sock_ >= 0) { closesocket(sock_); sock_ = -1; }
#else
    if (sock_ >= 0)
    {
        ::close(sock_);
        sock_ = -1;
    }
#endif
}

bool UdpReceiver::FrameAsm::complete() const
{
    return expect > 0 && chunks.size() == expect;
}

void UdpReceiver::recv_loop(PacketCallback cb)
{
    static uint8_t buf[65536];
    while (running_)
    {
        int n = (int)::recv(sock_, (char*)buf, sizeof(buf), 0);
        if (n < (int)sizeof(PacketHeader))
            continue;

        PacketHeader hdr{};
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != PACKET_MAGIC)
            continue;
        // Discard expired frames to ensure low latency
        if (last_id_ != UINT32_MAX && hdr.frame_id <= last_id_)
            continue;

        auto& a = asm_[hdr.frame_id];
        a.frame_id = hdr.frame_id;
        a.ts       = hdr.timestamp_us;
        a.keyframe = hdr.is_keyframe;
        a.expect   = hdr.chunk_count;
        a.chunks[hdr.chunk_index].assign(buf + sizeof(hdr),
                                         buf + sizeof(hdr) + hdr.payload_size);

        if (!a.complete())
            continue;

        auto ep = std::make_shared<EncodedPacket>();
        ep->frame_id     = a.frame_id;
        ep->timestamp_us = a.ts;
        ep->is_keyframe  = a.keyframe != 0;
        for (auto& [idx, data] : a.chunks)
            ep->data.insert(ep->data.end(), data.begin(), data.end());

        last_id_ = hdr.frame_id;
        for (auto it = asm_.begin(); it != asm_.end();)
        {
            if (it->first <= last_id_)
                it = asm_.erase(it);
            else
                ++it;
        }
        cb(std::move(ep));
    }
}
