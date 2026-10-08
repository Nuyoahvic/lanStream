#include "decoder_ffmpeg.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <string>

static std::string av_err(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

// Callback required by FFmpeg to select the VAAPI pixel format when decoding on GPU
static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (*p == AV_PIX_FMT_VAAPI)
            return AV_PIX_FMT_VAAPI;
    }
    std::cerr << "[decoder] Warning: VAAPI pixel format not found in supported list, falling back" << std::endl;
    return AV_PIX_FMT_NONE;
}

bool DecoderFFmpeg::init()
{
    // Prioritize hardware decoding based on platform, falling back to standard software h264
    std::vector<std::string> names = {
#ifdef PLATFORM_WINDOWS
            "h264_cuvid", "h264_qsv",
#elif defined(PLATFORM_MACOS)
            "h264_vda", "h264_videotoolbox",
#elif defined(PLATFORM_LINUX)
            "h264_vaapi",
#endif
            "h264", ""
    };

    for (auto &name: names)
    {
        const AVCodec* codec = avcodec_find_decoder_by_name(name.c_str());
        if (!codec)
            continue;

        if (open_codec(codec))
        {
            std::cout << "[decoder] using " << name.c_str();
            if (use_hw_)
                std::cout << " (GPU)" << std::endl;
            else
                std::cout << " (CPU)" << std::endl;
            return true;
        }
        std::cerr << "[decoder] " << name.c_str() << " not usable, trying next" << std::endl;
        cleanup();
    }
    std::cerr << "[decoder] no usable decoder found" << std::endl;
    return false;
}

bool DecoderFFmpeg::setup_vaapi()
{
    const char* dev = std::getenv("LANSTREAM_VAAPI_DEVICE");
    if (!dev || !*dev)
        dev = "/dev/dri/renderD128";

    int ret = av_hwdevice_ctx_create(&hw_dev_, AV_HWDEVICE_TYPE_VAAPI,
                                     dev, nullptr, 0);
    if (ret < 0)
    {
        std::cerr << "[decoder] cannot open VAAPI device " << dev << ": "
                  << av_err(ret) << std::endl;
        return false;
    }

    ctx_->hw_device_ctx = av_buffer_ref(hw_dev_);
    ctx_->get_format    = get_vaapi_format;

    return ctx_->hw_device_ctx != nullptr;
}

bool DecoderFFmpeg::open_codec(const AVCodec* codec)
{
    std::string codec_name = codec->name;
    if (codec_name.contains("vaapi"))
        use_hw_ = true;
    else
        use_hw_ = false;

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_)
        return false;

    ctx_->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->thread_count = 1; // Lowest single-thread latency

    if (use_hw_)
    {
        if (!setup_vaapi())
            return false;
    }

    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0)
    {
        std::cerr << "[decoder] avcodec_open2 failed: " << av_err(ret) << std::endl;
        return false;
    }

    avf_ = av_frame_alloc();
    if (use_hw_)
        hw_frame_ = av_frame_alloc();

    pkt_ = av_packet_alloc();
    return pkt_ != nullptr;
}

void DecoderFFmpeg::decode(EncodedPacketPtr ep, FrameCallback cb)
{
    if (!ep)
        return;

    pkt_->data = const_cast<uint8_t*>(ep->data.data());
    pkt_->size = static_cast<int>(ep->data.size());

    if (avcodec_send_packet(ctx_, pkt_) < 0)
        return;

    while (true)
    {
        int ret = avcodec_receive_frame(ctx_, avf_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            break;

        AVFrame* frame_to_scale = avf_;

        // If decoding via VAAPI, the frame lives on the GPU. Transfer it to CPU NV12 surfaces.
        if (use_hw_ && avf_->format == AV_PIX_FMT_VAAPI)
        {
            av_frame_unref(hw_frame_);
            hw_frame_->format = AV_PIX_FMT_NV12;
            hw_frame_->width  = avf_->width;
            hw_frame_->height = avf_->height;

            if (av_hwframe_transfer_data(hw_frame_, avf_, 0) < 0)
            {
                av_frame_unref(avf_);
                continue;
            }
            hw_frame_->pts = avf_->pts;
            frame_to_scale = hw_frame_;
        }

        int w = frame_to_scale->width, h = frame_to_scale->height;
        if (!sws_ || w != sw_ || h != sh_)
        {
            if (sws_)
                sws_freeContext(sws_);

            sw_ = w; sh_ = h;
            sws_ = sws_getContext(w, h, static_cast<AVPixelFormat>(frame_to_scale->format),
                                  w, h, AV_PIX_FMT_BGRA,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }

        auto f = std::make_shared<RawFrame>();
        f->timestamp_us = ep->timestamp_us;
        f->width    = w;
        f->height   = h;
        f->linesize = w * 4;
        f->format   = PixelFormat::BGRA;
        f->data.resize(w * 4 * h);

        uint8_t* dst[1]  = { f->data.data() };
        int dst_ls[1]    = { w * 4 };

        sws_scale(sws_, frame_to_scale->data, frame_to_scale->linesize, 0, h, dst, dst_ls);

        av_frame_unref(avf_);
        cb(std::move(f));
    }
}

DecoderFFmpeg::~DecoderFFmpeg()
{
    cleanup();
}

void DecoderFFmpeg::cleanup()
{
    if (sws_)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (ctx_)
        avcodec_free_context(&ctx_);
    if (avf_)
        av_frame_free(&avf_);
    if (hw_frame_)
        av_frame_free(&hw_frame_);
    if (pkt_)
        av_packet_free(&pkt_);
    if (hw_dev_)
        av_buffer_unref(&hw_dev_);
}