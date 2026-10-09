#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <iostream>

static_assert(SDL_VERSION == SDL_VERSIONNUM(3, 4, 18));
static_assert(SDL_MIXER_VERSION == SDL_VERSIONNUM(3, 2, 4));

int main() {
    const int sdl = SDL_GetVersion();
    const int mixer = MIX_Version();
    std::cout << "SDL=" << sdl << " SDL_mixer=" << mixer << '\n';
    return sdl == SDL_VERSION && mixer == SDL_MIXER_VERSION ? 0 : 1;
}
