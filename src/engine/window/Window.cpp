#include "Window.hpp"

bool Window::init(){
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)){
        return false;
    }
    m_window = SDL_CreateWindow("Zenith",  width,  height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!m_window){
        return false;
    }
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        return false;
    }
    return true;
}

SDL_Window* Window::handle() const{
    return m_window;
}


void Window::shutdown(){
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}