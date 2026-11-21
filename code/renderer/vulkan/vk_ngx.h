#ifndef VK_NGX_H
#define VK_NGX_H

#include "../tr_local.h"

qboolean VK_NGX_Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
qboolean VK_NGX_Available(void);
qboolean VK_NGX_Active(void);
qboolean VK_NGX_CreateDLSS(VkCommandBuffer cmd, uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight);
qboolean VK_NGX_EvaluateDLSS(VkCommandBuffer cmd, vkimage_t *color, VkFormat colorFormat,
		vkimage_t *depth, VkFormat depthFormat, vkimage_t *motion, VkFormat motionFormat,
		vkimage_t *output, VkFormat outputFormat, qboolean reset);
void VK_NGX_Shutdown(VkDevice device);

#endif
