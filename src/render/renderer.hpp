#pragma once
#include "common/frame.hpp"

class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual bool init(int w, int h, const char* title = "LanStream") = 0;
    virtual bool render(RawFramePtr frame)                           = 0;
    virtual bool poll_events()                                       = 0;
};
