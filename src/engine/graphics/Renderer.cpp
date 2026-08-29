#include "Renderer.hpp"


bool Renderer::init(const Window& window){
    if (volkInitialize() != VK_SUCCESS) {
        return false;
    }

    if (!m_context.initInstance()){
        return false;
    }

    if (!m_surface.init(m_context.instance(), window.handle())){
        return false;
    }

    
}






