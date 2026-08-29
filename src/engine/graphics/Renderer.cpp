#include "Renderer.hpp"


bool Renderer::init(){
    if (volkInitialize() != VK_SUCCESS) {
        return false;
    }

    if (!m_context.initInstance()){
        return false;
    }
}






