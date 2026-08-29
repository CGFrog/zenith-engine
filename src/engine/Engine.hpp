#pragma once

#include "Window.hpp"
#include "Renderer.hpp"

class Engine {

public:

    bool init();    
    void run();
    void shutdown();

private:
    Window m_window;
    Renderer m_renderer;
};
