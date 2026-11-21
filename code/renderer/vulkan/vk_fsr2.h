#ifndef VK_FSR2_H
#define VK_FSR2_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FSR2 backend lifecycle.  The bridge in vk_fsr2.c supplies the renderer
 * context; this interface only knows about Vulkan objects so the FSR2 C++
 * SDK can be compiled without pulling in the Quake headers. */
int  VK_FSR2_Init(VkInstance instance, VkPhysicalDevice physicalDevice,
                  VkDevice device, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                  PFN_vkGetDeviceProcAddr getDeviceProcAddr);
int  VK_FSR2_Available(void);
int  VK_FSR2_Active(void);
int  VK_FSR2_Create(uint32_t renderWidth, uint32_t renderHeight,
                    uint32_t displayWidth, uint32_t displayHeight);
int  VK_FSR2_Evaluate(VkCommandBuffer cmd,
                      VkImage colorImage, VkImageView colorView, VkFormat colorFormat,
                      VkImage depthImage, VkImageView depthView, VkFormat depthFormat,
                      VkImage motionImage, VkImageView motionView, VkFormat motionFormat,
                      VkImage outputImage, VkImageView outputView, VkFormat outputFormat,
                      uint32_t renderWidth, uint32_t renderHeight,
                      uint32_t displayWidth, uint32_t displayHeight,
                      int reset, float jitterX, float jitterY,
                      float frameTimeMs, float cameraNear, float cameraFar,
                      float cameraFovY);
void VK_FSR2_Shutdown(VkDevice device);

#ifdef __cplusplus
}
#endif

#endif
