#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>

#include "common/blocking_queue.hpp"
#include "transport/udp_receiver.hpp"
#include "decoder/decoder_ffmpeg.hpp"
#include "render/renderer_sdl.hpp"

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run = false; }

int main(int argc, char **argv)
{
    // Usage: receiver [port=5000]
    uint16_t port{};
    UdpReceiver udp;
    DecoderFFmpeg decoder;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (argc > 1)
        port = (uint16_t)std::stoi(argv[1]);
    else
        port = 5000;

    std::cout << "Receiver: port=" << port << std::endl;

    if (!udp.init(port))
    {
        std::cerr << "udp init failed" << std::endl;
        return 1;
    }

    if (!decoder.init())
    {
        std::cerr << "decoder init failed" << std::endl;
        return 1;
    }

    // Frame queue from the decoding thread to the main rendering thread
    // Capacity reduced to 2 to minimize latency caused by queuing at the end of the queue
    BlockingQueue<RawFramePtr> fq(2);

    udp.start([&](EncodedPacketPtr pkt)
    {
        decoder.decode(std::move(pkt), [&](RawFramePtr f)
        {
            // Push the decoded raw frame into the queue
            fq.push(f);
        });
    });

    // Rendering must take place on the main thread (required by SDL2)
    RendererSDL renderer;
    bool inited = false;
    uint32_t cnt = 0;

    while (g_run)
    {
        if (!renderer.poll_events())
            break;

        // Attempt to capture a frame within 10ms
        auto opt = fq.pop(1);
        if (!opt)
            // No new frame; continue processing events.
            continue;

        // Get a frame first
        RawFramePtr latest = *opt;

        // To reduce latency: If there are additional frames in the queue, discard the older ones and retain only the latest frame.
        // Since BlockingQueue only provides a blocking pop(timeout) method,
        // a fully non-blocking approach isn't possible; however, we can attempt the operation multiple times using a very short timeout.
        while (true)
        {
            auto opt_more = fq.pop(0);  // If your BlockingQueue does not allow 0, you can change it to 1–2 ms.
            if (!opt_more)
                break;
            latest = *opt_more;
        }

        auto& f = latest;

        if (!inited)
        {
            renderer.init(f->width, f->height, "LanStream Receiver");
            inited = true;
        }

        if (cnt++ % 60 == 0)
        {
            double e2e = (now_us() - f->timestamp_us) / 1000.0;
            std::cout << "frame=" << cnt
                      << " e2e_ms=" << e2e << std::endl;
        }
        renderer.render(f);
    }

    udp.stop();
    fq.close();
    std::cout << "receiver stopped." << std::endl;
    return 0;
}
