#include "encoder_ffmpeg.hpp"
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

bool EncoderFFmpeg::init(int width, int height, int fps, int bitrate_kbps)
{
    width_ = width; height_ = height;

    // Prioritize hardware encoding based on the platform, with a final fallback to libx264.
    std::vector<std::string> names = {
#ifdef PLATFORM_WINDOWS
            "h264_nvenc", "h264_qsv", "h264_amf",
#elif defined(PLATFORM_MACOS)
            "h264_videotoolbox",
#elif defined(PLATFORM_LINUX)
            "h264_vaapi", "h264_nvenc",
#endif
            "libx264", ""
    };

    // avcodec_find_encoder_by_name() only says the encoder was *compiled into*
    // FFmpeg, not that it works on this machine. So try each candidate for real
    // (open it) and fall through to the next one if that fails.
    for (auto &name: names)
    {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec)
            continue;

        if (open_codec(codec, fps, bitrate_kbps))
        {
            std::cout << "[encoder] using " << name.c_str();
            if (use_hw_)
                std::cout << " (GPU)" << std::endl;
            else
                std::cout << " (CPU)" << std::endl;
            return true;
        }
        std::cerr << "[encoder] " << name.c_str() << " not usable, trying next" << std::endl;
        cleanup();
    }
    std::cerr << "[encoder] no usable encoder" << std::endl;
    return false;
}

// VAAPI encoders do not take normal CPU frames (yuv420p). They need
//   1) a VAAPI device,
//   2) a pool of GPU surfaces (hw_frames_ctx) and pix_fmt = AV_PIX_FMT_VAAPI,
//   3) every frame uploaded from CPU memory into one of those surfaces.
bool EncoderFFmpeg::setup_vaapi()
{
    const char* dev = std::getenv("LANSTREAM_VAAPI_DEVICE");

    if (!dev || !*dev)
        dev = "/dev/dri/renderD128";

    int ret = av_hwdevice_ctx_create(&hw_dev_, AV_HWDEVICE_TYPE_VAAPI,
                                     dev, nullptr, 0);
    if (ret < 0)
    {
        std::cerr << "[encoder] cannot open VAAPI device " << dev << ": "
                  << av_err(ret) << std::endl;
        return false;
    }

    AVBufferRef* frames_ref = av_hwframe_ctx_alloc(hw_dev_);

    if (!frames_ref)
        return false;

    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    fc->format            = AV_PIX_FMT_VAAPI;
    fc->sw_format         = AV_PIX_FMT_NV12;
    fc->width             = width_;
    fc->height            = height_;
    fc->initial_pool_size = 20;

    ret = av_hwframe_ctx_init(frames_ref);
    if (ret < 0)
    {
        std::cerr << "[encoder] VAAPI frame pool init failed: "
                  << av_err(ret) << std::endl;
        av_buffer_unref(&frames_ref);
        return false;
    }
    ctx_->hw_frames_ctx = av_buffer_ref(frames_ref);
    av_buffer_unref(&frames_ref);
    return ctx_->hw_frames_ctx != nullptr;
}

bool EncoderFFmpeg::open_codec(const AVCodec* codec, int fps, int bitrate_kbps)
{
    std::string codec_name = codec->name;
    if (codec_name.contains("vaapi"))
        use_hw_ = true;
    else
        use_hw_ = false;

    ctx_ = avcodec_alloc_context3(codec);

    if (!ctx_)
        return false;

    ctx_->width        = width_;
    ctx_->height       = height_;
    ctx_->time_base    = {1, fps};
    ctx_->framerate    = {fps, 1};
    ctx_->bit_rate     = bitrate_kbps * 1000LL;
    ctx_->gop_size     = 1;            // All I-frames, lowest latency
    ctx_->max_b_frames = 0;

    if (use_hw_)
        ctx_->pix_fmt = AV_PIX_FMT_VAAPI;
    else
        ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

    if (use_hw_)
    {
        if (!setup_vaapi())
            return false;

        // 1. Force CQP mode because HD 520 (iHD driver) only supports CQP
        av_opt_set(ctx_->priv_data, "rc_mode", "CQP", 0);
        av_opt_set_int(ctx_->priv_data, "qp", 24, 0); // 18-28 range (lower = higher quality)
        ctx_->bit_rate = 0;                           // Must be 0 for CQP mode

        // h264_vaapi has no preset/tune options, and its profile is called
        // "constrained_baseline" ("baseline" is not accepted).
        av_opt_set    (ctx_->priv_data, "profile",     "constrained_baseline", 0);
        av_opt_set_int(ctx_->priv_data, "async_depth", 1, 0);   // lowest latency
    }
    else
    {
        if (codec_name.contains("libx264"))
        {   // x264-only options
            av_opt_set(ctx_->priv_data, "preset", "ultrafast",   0);
            av_opt_set(ctx_->priv_data, "tune",   "zerolatency", 0);
        }
        av_opt_set(ctx_->priv_data, "profile", "baseline", 0);
    }

    int ret = avcodec_open2(ctx_, codec, nullptr);

    if (ret < 0)
    {
        std::cerr << "[encoder] avcodec_open2 failed: " << av_err(ret) << std::endl;
        return false;
    }

    // CPU-side frame that swscale writes into.
    AVPixelFormat sw_fmt;

    if (use_hw_)
        sw_fmt = AV_PIX_FMT_NV12;
    else
        sw_fmt = AV_PIX_FMT_YUV420P;

    avf_ = av_frame_alloc();
    avf_->format = sw_fmt;
    avf_->width  = width_;
    avf_->height = height_;
    av_frame_get_buffer(avf_, 32);

    if (use_hw_)
        hw_frame_ = av_frame_alloc();

    pkt_ = av_packet_alloc();

    sws_ = sws_getContext(width_, height_, AV_PIX_FMT_BGRA,
                          width_, height_, sw_fmt,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    return sws_ != nullptr;
}

void EncoderFFmpeg::do_encode(AVFrame* frame, PacketCallback& cb) {
    int ret = avcodec_send_frame(ctx_, frame);

    if (ret < 0)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            std::cerr << "[encoder] send_frame failed: " << av_err(ret) << std::endl;
        }
        return;
    }

    while (true)
    {
        ret = avcodec_receive_packet(ctx_, pkt_);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;

        if (ret < 0)
            break;

        auto ep = std::make_shared<EncodedPacket>();
        ep->frame_id     = fid_++;
        ep->timestamp_us = now_us();

        if (pkt_->flags & AV_PKT_FLAG_KEY)
            ep->is_keyframe = true;
        else
            ep->is_keyframe = false;

        ep->data.assign(pkt_->data, pkt_->data + pkt_->size);
        av_packet_unref(pkt_);
        cb(std::move(ep));
    }
}

void EncoderFFmpeg::encode(RawFramePtr raw, PacketCallback cb)
{
    if (!raw || raw->data.empty())
        return;

    const uint8_t* src[1]  = { raw->data.data() };
    int  src_ls[1]          = { raw->linesize };
    av_frame_make_writable(avf_);
    sws_scale(sws_, src, src_ls, 0, height_,
              avf_->data, avf_->linesize);

    if (use_hw_)
    {
        // grab a free GPU surface and upload the converted frame into it
        av_frame_unref(hw_frame_);

        if (av_hwframe_get_buffer(ctx_->hw_frames_ctx, hw_frame_, 0) < 0)
            return;

        if (av_hwframe_transfer_data(hw_frame_, avf_, 0) < 0)
            return;
        hw_frame_->pts = pts_++;
        do_encode(hw_frame_, cb);
    }
    else
    {
        avf_->pts = pts_++;
        do_encode(avf_, cb);
    }
}

void EncoderFFmpeg::flush(PacketCallback cb)
{
    do_encode(nullptr, cb);
}

EncoderFFmpeg::~EncoderFFmpeg()
{
    cleanup();
}

void EncoderFFmpeg::cleanup()
{
    if (sws_)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }

    if (pkt_)
        av_packet_free(&pkt_);
    if (hw_frame_)
        av_frame_free(&hw_frame_);
    if (avf_)
        av_frame_free(&avf_);
    if (ctx_)
        avcodec_free_context(&ctx_);
    if (hw_dev_)
        av_buffer_unref(&hw_dev_);
}