#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>

#include "capture/capture_factory.hpp"
#include "encoder/encoder_ffmpeg.hpp"
#include "transport/udp_sender.hpp"

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run = false; }

int main(int argc, char **argv)
{
    // Usage: sender <remote_IP> [port=5000] [fps=60] [bitrate_kbps=8000]
    std::string ip{};
    uint16_t port{};
    int fps{}, bitrate{};

    EncoderFFmpeg encoder;
    UdpSender udp;

    if (argc > 1)
        ip = argv[1];
    else
        ip = "0.0.0.0";

    if (argc > 2)
        port = (uint16_t)std::stoi(argv[2]);
    else
           port = 5000;

    if (argc > 3)
        fps = std::stoi(argv[3]);
    else
        fps = 60;

    if (argc > 4)
        bitrate = std::stoi(argv[4]);
    else
        bitrate = 8000;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    auto capture = create_capture();
    if (!capture->init(0))
    {
        std::cerr << "capture init failed\n";
        return 1;
    }

    int w = capture->width(), h = capture->height();
    std::cout << "Sender: " << w << "x" << h
              << " -> " << ip << ":" << port
              << " fps=" << fps << " bitrate=" << bitrate
              << "kbps" << std::endl;


    if (!encoder.init(w, h, fps, bitrate))
    {
        std::cerr << "encoder init failed" << std::endl;
        return 1;
    }

    if (!udp.init(ip, port))
    {
        std::cerr << "udp sender init failed" << std::endl;
        return 1;
    }

    capture->start([&](RawFramePtr frame)
    {
        if (!g_run)
            return;
        encoder.encode(std::move(frame), [&](EncodedPacketPtr pkt)
        {
            // Print latency (capture to encoding completion) every 60 frames
            if (pkt->frame_id % 60 == 0)
            {
                double ms = (now_us() - pkt->timestamp_us) / 1000.0;
                std::string str{};
                if (pkt->is_keyframe)
                    str = " [I]";
                std::cout << "frame=" << pkt->frame_id
                          << " size=" << pkt->data.size() << "B"
                          << " enc_ms=" << ms
                          << str << std::endl;
            }
            udp.send(pkt);
        });
    });

    while (g_run)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    capture->stop();
    encoder.flush([&](EncodedPacketPtr p)
    {
        udp.send(p);
    });

    std::cout << "sender stopped." << std::endl;

    return 0;
}
