#ifdef PLATFORM_LINUX
#include "capture_x11.hpp"
#include <sys/shm.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

bool CaptureX11::init(int)
{
    dpy_  = XOpenDisplay(nullptr);

    if (!dpy_)
    {
        std::cerr << "XOpenDisplay failed" << std::endl;
        return false;
    }

    int scr = DefaultScreen(dpy_);
    root_   = RootWindow(dpy_, scr);
    width_  = DisplayWidth(dpy_,  scr);
    height_ = DisplayHeight(dpy_, scr);

    shm_img_ = XShmCreateImage(dpy_, DefaultVisual(dpy_, scr),
                                DefaultDepth(dpy_, scr), ZPixmap,
                                nullptr, &shm_info_, width_, height_);
    shm_info_.shmid   = shmget(IPC_PRIVATE,
                                shm_img_->bytes_per_line * shm_img_->height,
                                IPC_CREAT | 0777);
    shm_info_.shmaddr = shm_img_->data =
        reinterpret_cast<char*>(shmat(shm_info_.shmid, nullptr, 0));
    shm_info_.readOnly = False;
    XShmAttach(dpy_, &shm_info_);
    return true;
}

int CaptureX11::width() const
{
    return width_;
}

int CaptureX11::height() const
{
    return height_;
}

bool CaptureX11::start(FrameCallback cb)
{
    running_ = true;
    thread_ = std::thread([this, cb]
            {
        capture_loop(cb);
            });
    return true;
}

CaptureX11::~CaptureX11()
{
    stop();
}

void CaptureX11::stop()
{
    running_ = false;

    if (thread_.joinable())
        thread_.join();

    if (dpy_)
    {
        XShmDetach(dpy_, &shm_info_);
        XDestroyImage(shm_img_);
        shmdt(shm_info_.shmaddr);
        shmctl(shm_info_.shmid, IPC_RMID, nullptr);
        XCloseDisplay(dpy_);
        dpy_ = nullptr;
    }
}

void CaptureX11::capture_loop(FrameCallback cb)
{
    constexpr int kFps = 60;
    const auto interval = std::chrono::microseconds(1000000 / kFps);
    while (running_)
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        XShmGetImage(dpy_, root_, shm_img_, 0, 0, AllPlanes);

        auto f = std::make_shared<RawFrame>();
        f->timestamp_us = now_us();
        f->width    = width_;
        f->height   = height_;
        f->linesize = shm_img_->bytes_per_line;
        f->format   = PixelFormat::BGRA;
        f->data.resize(shm_img_->bytes_per_line * height_);
        memcpy(f->data.data(), shm_img_->data, f->data.size());
        cb(std::move(f));

        auto sleep = interval - (std::chrono::high_resolution_clock::now() - t0);
        if (sleep > std::chrono::microseconds(0))
            std::this_thread::sleep_for(sleep);
    }
}

#endif
