// renderer_sdl.hpp
#pragma once
#include "renderer.hpp"
#include <SDL2/SDL.h>

class RendererSDL : public IRenderer
{
public:
    ~RendererSDL() override;
    bool init(int w, int h, const char* title = "LanStream") override;
    bool render(RawFramePtr frame)                            override;
    bool poll_events()                                        override;
private:
    void cleanup();
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    int w_ = 0, h_ = 0;
};
