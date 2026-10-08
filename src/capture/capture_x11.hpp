#pragma once
#ifdef PLATFORM_LINUX
#include "capture.hpp"
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <thread>
#include <atomic>

class CaptureX11 : public ICapture
{
public:
    ~CaptureX11() override;
    bool init(int display_index = 0) override;
    bool start(FrameCallback cb)     override;
    void stop()                      override;
    int  width()  const override;
    int  height() const override;
private:
    void capture_loop(FrameCallback cb);
    Display*          dpy_      = nullptr;
    Window            root_     = 0;
    XShmSegmentInfo   shm_info_{};
    XImage*           shm_img_  = nullptr;
    int width_ = 0, height_ = 0;
    std::thread       thread_;
    std::atomic<bool> running_{false};
};
#endif
