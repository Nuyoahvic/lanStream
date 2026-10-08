#pragma once
#include "encoder.hpp"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

class EncoderFFmpeg : public IEncoder
{
public:
    ~EncoderFFmpeg() override;
    bool init(int width, int height, int fps, int bitrate_kbps) override;
    void encode(RawFramePtr frame, PacketCallback cb) override;
    void flush(PacketCallback cb)                     override;
private:
    bool open_codec(const AVCodec* codec, int fps, int bitrate_kbps);
    bool setup_vaapi();
    void cleanup();
    void do_encode(AVFrame* frame, PacketCallback& cb);

    AVCodecContext* ctx_      = nullptr;
    AVFrame*        avf_      = nullptr;   // CPU frame (NV12 for VAAPI, YUV420P otherwise)
    AVFrame*        hw_frame_ = nullptr;   // GPU surface, only used by VAAPI
    AVBufferRef*    hw_dev_   = nullptr;   // VAAPI device, only used by VAAPI
    AVPacket*       pkt_      = nullptr;
    SwsContext*     sws_      = nullptr;
    uint32_t        fid_      = 0;         // packets produced
    int64_t         pts_      = 0;         // frames submitted
    bool            use_hw_   = false;
    int             width_    = 0;
    int             height_   = 0;
};
