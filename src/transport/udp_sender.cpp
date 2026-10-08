// udp_sender.cpp
#include "udp_sender.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

bool UdpSender::init(const std::string& ip, uint16_t port)
{
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2,2), &wd);
#endif
    sock_ = (int)::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0)
        return false;

    int sndbuf = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               (char*)&sndbuf, sizeof(sndbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    return ::connect(sock_, (sockaddr*)&addr, sizeof(addr)) == 0;
}

void UdpSender::send(const EncodedPacketPtr& pkt)
{
    if (!pkt || sock_ < 0)
        return;
    const size_t total  = pkt->data.size();
    const size_t chunks = (total + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    static uint8_t buf[UDP_MTU + sizeof(PacketHeader)];

    for (size_t ci = 0; ci < chunks; ++ci)
    {
        size_t offset = ci * MAX_PAYLOAD;
        size_t csz    = std::min<size_t>(MAX_PAYLOAD, total - offset);

        PacketHeader hdr{};
        hdr.magic        = PACKET_MAGIC;
        hdr.frame_id     = pkt->frame_id;
        hdr.timestamp_us = pkt->timestamp_us;
        hdr.chunk_index  = (uint16_t)ci;
        hdr.chunk_count  = (uint16_t)chunks;
        hdr.payload_size = (uint32_t)csz;
        hdr.is_keyframe  = pkt->is_keyframe ? 1 : 0;

        memcpy(buf,              &hdr,                         sizeof(hdr));
        memcpy(buf + sizeof(hdr), pkt->data.data() + offset,  csz);
        ::send(sock_, (char*)buf, sizeof(hdr) + csz, 0);
    }
}

void UdpSender::close()
{
    if (sock_ >= 0)
    {
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = -1;
    }
}

UdpSender::~UdpSender()
{
    close();
}