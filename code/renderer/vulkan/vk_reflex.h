#ifndef VK_REFLEX_H
#define VK_REFLEX_H

#include "../tr_local.h"

extern qboolean vk_low_latency_enabled;

qboolean VK_Reflex_Init(VkDevice device, VkPhysicalDevice physicalDevice);
void VK_Reflex_Shutdown(VkDevice device);
void VK_Reflex_AttachSwapchain(VkSwapchainKHR swapchain);
void VK_Reflex_DetachSwapchain(void);

qboolean VK_Reflex_Active(void);
uint64_t VK_Reflex_PresentId(void);

/* Called once per game frame: sleep before simulation, then mark it. */
void VK_Reflex_SimulationStart(void);
void VK_Reflex_SimulationEnd(void);

/* Called around the Vulkan submit/present in VK_EndFrame. */
void VK_Reflex_RenderSubmitStart(void);
void VK_Reflex_RenderSubmitEnd(void);
void VK_Reflex_PresentStart(void);
void VK_Reflex_PresentEnd(void);

#endif
