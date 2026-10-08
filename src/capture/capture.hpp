#pragma once
#include "common/frame.hpp"
#include <functional>

class ICapture
{
public:
    using FrameCallback = std::function<void(RawFramePtr)>;
    virtual ~ICapture() = default;
    virtual bool init(int display_index = 0) = 0;
    virtual bool start(FrameCallback cb)     = 0;
    virtual void stop()                      = 0;
    virtual int  width()  const              = 0;
    virtual int  height() const              = 0;
};
