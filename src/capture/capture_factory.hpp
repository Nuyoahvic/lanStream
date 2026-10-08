#pragma once
#include "capture.hpp"
#include <memory>

#ifdef PLATFORM_WINDOWS
#  include "capture_dxgi.hpp"
#elif defined(PLATFORM_MACOS)
#  include "capture_cg.hpp"
#elif defined(PLATFORM_LINUX)
#  include "capture_x11.hpp"
#endif

inline std::unique_ptr<ICapture> create_capture()
{
#ifdef PLATFORM_WINDOWS
    return std::make_unique<CaptureDXGI>();
#elif defined(PLATFORM_MACOS)
    return std::make_unique<CaptureCG>();
#else
    return std::make_unique<CaptureX11>();
#endif
}
