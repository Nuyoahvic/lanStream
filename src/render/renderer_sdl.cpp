// renderer_sdl.cpp
#include "renderer_sdl.hpp"
#include <iostream>

bool RendererSDL::init(int w, int h, const char* title)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return false;

    w_ = w; h_ = h;
    window_ = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window_)
        return false;

    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_)
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);

    texture_ = SDL_CreateTexture(renderer_,
        SDL_PIXELFORMAT_BGRA32,
        SDL_TEXTUREACCESS_STREAMING, w, h);
    return texture_ != nullptr;
}

bool RendererSDL::render(RawFramePtr f)
{
    if (!f)
        return true;

    // Rebuild the texture when the resolution changes
    if (f->width != w_ || f->height != h_)
    {
        w_ = f->width; h_ = f->height;
        SDL_DestroyTexture(texture_);
        texture_ = SDL_CreateTexture(renderer_,
            SDL_PIXELFORMAT_BGRA32,
            SDL_TEXTUREACCESS_STREAMING, w_, h_);
        SDL_SetWindowSize(window_, w_, h_);
    }
    SDL_UpdateTexture(texture_, nullptr, f->data.data(), f->linesize);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
    return true;
}

bool RendererSDL::poll_events()
{
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        if (e.type == SDL_QUIT)
            return false;

        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
            return false;

    }
    return true;
}

void RendererSDL::cleanup()
{
    if (texture_)
        SDL_DestroyTexture(texture_);

    if (renderer_)
        SDL_DestroyRenderer(renderer_);

    if (window_)
        SDL_DestroyWindow(window_);

    SDL_Quit();
}

RendererSDL::~RendererSDL()
{
    cleanup();
}