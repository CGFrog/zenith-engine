#include "Engine.hpp"

bool Engine::init(){
    if (!m_window.init()){
        return false;
    }

    if (!m_renderer.init()){
        return false;
    }
}

void Engine::shutdown(){
    m_renderer.shutdown();
    m_window.shutdown();
}