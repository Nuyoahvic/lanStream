#pragma once
#include "decoder.hpp"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

class DecoderFFmpeg: public IDecoder
{
public:
    bool init() override;
    void decode(EncodedPacketPtr ep, FrameCallback cb) override;
    ~DecoderFFmpeg() override;

private:
    bool open_codec(const AVCodec* codec);
    bool setup_vaapi();
    void cleanup();

    AVCodecContext* ctx_ = nullptr;
    AVFrame* avf_ = nullptr;
    AVFrame* hw_frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVBufferRef* hw_dev_ = nullptr;
    SwsContext* sws_ = nullptr;

    bool use_hw_ = false;
    int sw_ = 0;
    int sh_ = 0;
};