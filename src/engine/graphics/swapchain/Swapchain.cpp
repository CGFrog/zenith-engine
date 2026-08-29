#include "Swapchain.hpp"

bool Swapchain::initSwapchain(const Window& window, const Surface& surface, const VulkanContext& context){
    m_scWidth = window.width;
    m_scHeight = window.height;
    
    VkSurfaceCapabilitiesKHR surfaceCaps = surface.capabilities(context.pDevice());
	VkSwapchainCreateInfoKHR swapchainCreateInfo {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surface.handle(),
		.minImageCount = surfaceCaps.minImageCount,
		.imageFormat = swapchainFormat,
		.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
		.imageExtent{.width = m_scWidth, .height = m_scHeight },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR
	};

	if (vkCreateSwapchainKHR(context.device(), &swapchainCreateInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
		return false;
	}

	// grab the swapchain images
	uint32_t imageCount = 0;
	vkGetSwapchainImagesKHR(context.device(), m_swapchain, &imageCount, nullptr);
	m_swapchainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(context.device(), m_swapchain, &imageCount, m_swapchainImages.data());
	m_swapchainImageViews.resize(imageCount);

	// create the swapchain image views
	for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
		VkImageViewCreateInfo imgViewInfo {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_swapchainImages[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = swapchainFormat,
			.subresourceRange {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1
			}
		};

		if (vkCreateImageView(context.device(), &imgViewInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS){
			return false;
		}
	}


	// create depth image
	VkImageCreateInfo depthCreateInfo {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = depthFormat,
		.extent{.width = m_scWidth, .height = m_scHeight, .depth = 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	VmaAllocationCreateInfo allocInfo {
		.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO
	};
	if (vmaCreateImage(context.vmaAllocator(), &depthCreateInfo, &allocInfo, &m_depthImage, &m_depthImageAllocation, nullptr) != VK_SUCCESS) {
		return false;
	}

	VkImageViewCreateInfo depthImgViewInfo {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = m_depthImage,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = depthFormat,
		.subresourceRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1}
	};
	return vkCreateImageView(context.device(), &depthImgViewInfo, nullptr, &m_depthImageView) == VK_SUCCESS;
}


const std::vector<VkImage>& Swapchain::swapchainImages() const{
    return m_swapchainImages;
}

const std::vector<VkImageView>& Swapchain::swapchainImageViews() const{
    return m_swapchainImageViews;
}

void Swapchain::destroySwapchain(const VulkanContext& context){
    for (VkImageView swapchainImgView : m_swapchainImageViews) {
		vkDestroyImageView(context.device(), swapchainImgView, nullptr);
	}
	m_swapchainImageViews.clear();

    if (m_swapchain) {
		vkDestroySwapchainKHR(context.device(), m_swapchain, nullptr);
		m_swapchain = nullptr;
	}

	if (m_depthImageView) {
		vkDestroyImageView(context.device(), m_depthImageView, nullptr);
		vmaDestroyImage(context.vmaAllocator(), m_depthImage, m_depthImageAllocation);
		m_depthImageView = nullptr;
	}
}