#include "app/application.hpp"

#include <SDL3/SDL_main.h>

#include <exception>
#include <iostream>

int main(int, char**) {
    try {
        return moorhuhn::app::run();
    } catch (const std::exception& error) {
        std::cerr << "moorhuhn: " << error.what() << '\n';
        return 1;
    }
}
