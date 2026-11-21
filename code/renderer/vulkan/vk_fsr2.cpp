/*
 * AMD FidelityFX Super Resolution 2 (FSR2 2.2.1) for the ray-tracing path.
 *
 * The FSR2 SDK is compiled into the renderer as a small static C++ library.
 * Its Vulkan backend is driven directly with the same color/depth/motion
 * buffers already used by the ASVGF denoiser, so no vendor-specific runtime
 * component is required.  The public entry points are C so vk_fsr2.c can call
 * them from the C renderer.
 */

#include "../fsr2/ffx_fsr2.h"
#include "../fsr2/vk/ffx_fsr2_vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FfxFsr2Context       fsr2Context;
static FfxFsr2Interface     fsr2Interface;
static void                *fsr2Scratch = NULL;
static size_t               fsr2ScratchSize = 0;
static VkDevice             fsr2Device = VK_NULL_HANDLE;
static VkPhysicalDevice     fsr2PhysicalDevice = VK_NULL_HANDLE;
static int                  fsr2Initialized = 0;
static int                  fsr2InterfaceReady = 0;
static int                  fsr2ContextCreated = 0;
static int                  fsr2Active = 0;
static int                  fsr2Failed = 0;
static uint32_t             fsr2RenderWidth = 0;
static uint32_t             fsr2RenderHeight = 0;
static uint32_t             fsr2DisplayWidth = 0;
static uint32_t             fsr2DisplayHeight = 0;

static void fsr2Message(FfxFsr2MsgType type, const wchar_t *message) {
    const char *kind = (type == FFX_FSR2_MESSAGE_TYPE_ERROR) ? "error" : "warning";
    char text[512];
    size_t i = 0;
    if (!message) return;
    for (; message[i] && i + 1 < sizeof(text); ++i)
        text[i] = (char)((message[i] < 128) ? message[i] : '?');
    text[i] = 0;
    fprintf(stderr, "FSR2: %s: %s\n", kind, text);
}

extern "C" int VK_FSR2_Init(VkInstance instance, VkPhysicalDevice physicalDevice,
                            VkDevice device, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                            PFN_vkGetDeviceProcAddr getDeviceProcAddr) {
    if (fsr2Initialized)
        return fsr2InterfaceReady;

    fsr2Initialized = 1;
    ffxFsr2SetInstanceProcAddr(instance, getInstanceProcAddr);
    if (!ffxFsr2InstanceProcReady()) {
        fprintf(stderr, "FSR2: could not resolve Vulkan instance functions\n");
        return 0;
    }
    fsr2PhysicalDevice = physicalDevice;
    fsr2Device = device;

    fsr2ScratchSize = ffxFsr2GetScratchMemorySizeVK(physicalDevice);
    fsr2Scratch = calloc(1, fsr2ScratchSize);
    if (!fsr2Scratch) {
        fprintf(stderr, "FSR2: out of memory for backend scratch buffer\n");
        return 0;
    }

    FfxErrorCode err = ffxFsr2GetInterfaceVK(&fsr2Interface, fsr2Scratch,
                                             fsr2ScratchSize, physicalDevice,
                                             getDeviceProcAddr);
    if (err != FFX_OK) {
        fprintf(stderr, "FSR2: ffxFsr2GetInterfaceVK failed (%d)\n", (int)err);
        free(fsr2Scratch);
        fsr2Scratch = NULL;
        return 0;
    }

    fsr2InterfaceReady = 1;
    return 1;
}

extern "C" int VK_FSR2_Available(void) {
    return fsr2InterfaceReady && !fsr2Failed;
}

extern "C" int VK_FSR2_Active(void) {
    return fsr2Active && !fsr2Failed;
}

extern "C" int VK_FSR2_Create(uint32_t renderWidth, uint32_t renderHeight,
                              uint32_t displayWidth, uint32_t displayHeight) {
    if (!fsr2InterfaceReady || fsr2Failed)
        return 0;
    if (renderWidth < 64 || renderHeight < 64 || displayWidth < 64 || displayHeight < 64)
        return 0;

    if (fsr2ContextCreated &&
        renderWidth == fsr2RenderWidth && renderHeight == fsr2RenderHeight &&
        displayWidth == fsr2DisplayWidth && displayHeight == fsr2DisplayHeight) {
        fsr2Active = 1;
        return 1;
    }

    if (fsr2ContextCreated) {
        ffxFsr2ContextDestroy(&fsr2Context);
        fsr2ContextCreated = 0;
        fsr2Active = 0;
    }

    FfxFsr2ContextDescription desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    /* The RT path writes motion vectors at render resolution. */
    desc.maxRenderSize.width = renderWidth;
    desc.maxRenderSize.height = renderHeight;
    desc.displaySize.width = displayWidth;
    desc.displaySize.height = displayHeight;
    desc.callbacks = fsr2Interface;
    desc.device = ffxGetDeviceVK(fsr2Device);
    desc.fpMessage = fsr2Message;

    FfxErrorCode err = ffxFsr2ContextCreate(&fsr2Context, &desc);
    if (err != FFX_OK) {
        fprintf(stderr, "FSR2: context creation failed (%d); using EASU\n", (int)err);
        fsr2Failed = 1;
        fsr2Active = 0;
        return 0;
    }

    fsr2RenderWidth = renderWidth;
    fsr2RenderHeight = renderHeight;
    fsr2DisplayWidth = displayWidth;
    fsr2DisplayHeight = displayHeight;
    fsr2ContextCreated = 1;
    fsr2Active = 1;
    fprintf(stderr, "FSR2: active, %ux%u -> %ux%u\n",
            renderWidth, renderHeight, displayWidth, displayHeight);
    return 1;
}

extern "C" int VK_FSR2_Evaluate(VkCommandBuffer cmd,
                                VkImage colorImage, VkImageView colorView, VkFormat colorFormat,
                                VkImage depthImage, VkImageView depthView, VkFormat depthFormat,
                                VkImage motionImage, VkImageView motionView, VkFormat motionFormat,
                                VkImage outputImage, VkImageView outputView, VkFormat outputFormat,
                                uint32_t renderWidth, uint32_t renderHeight,
                                uint32_t displayWidth, uint32_t displayHeight,
                                int reset, float jitterX, float jitterY,
                                float frameTimeMs, float cameraNear, float cameraFar,
                                float cameraFovY) {
    if (!fsr2Active || !fsr2ContextCreated || fsr2Failed)
        return 0;

    FfxFsr2DispatchDescription params;
    memset(&params, 0, sizeof(params));
    params.commandList = ffxGetCommandListVK(cmd);
    /* Images are kept in VK_IMAGE_LAYOUT_GENERAL by the RT path.  Tell FSR2
     * that they start as UAVs so its first barrier uses the matching layout. */
    params.color = ffxGetTextureResourceVK(&fsr2Context, colorImage, colorView,
                                           renderWidth, renderHeight, colorFormat,
                                           L"FSR2_InputColor",
                                           FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    params.depth = ffxGetTextureResourceVK(&fsr2Context, depthImage, depthView,
                                           renderWidth, renderHeight, depthFormat,
                                           L"FSR2_InputDepth",
                                           FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    params.motionVectors = ffxGetTextureResourceVK(&fsr2Context, motionImage, motionView,
                                                   renderWidth, renderHeight, motionFormat,
                                                   L"FSR2_InputMotionVectors",
                                                   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    params.output = ffxGetTextureResourceVK(&fsr2Context, outputImage, outputView,
                                            displayWidth, displayHeight, outputFormat,
                                            L"FSR2_OutputUpscaledColor",
                                            FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    params.jitterOffset.x = jitterX;
    params.jitterOffset.y = jitterY;
    /* The RT shaders store NDC-space motion.  FSR2 wants UV-space motion, so
     * scale NDC by half a pixel dimension. */
    params.motionVectorScale.x = (float)renderWidth * 0.5f;
    params.motionVectorScale.y = (float)renderHeight * 0.5f;
    params.renderSize.width = renderWidth;
    params.renderSize.height = renderHeight;
    params.enableSharpening = false;
    params.sharpness = 0.0f;
    params.frameTimeDelta = (frameTimeMs > 0.0f) ? frameTimeMs : 16.666f;
    params.preExposure = 1.0f;
    params.reset = reset ? true : false;
    params.cameraNear = cameraNear;
    params.cameraFar = cameraFar;
    params.cameraFovAngleVertical = cameraFovY;
    params.viewSpaceToMetersFactor = 1.0f;
    params.enableAutoReactive = false;

    FfxErrorCode err = ffxFsr2ContextDispatch(&fsr2Context, &params);
    if (err != FFX_OK) {
        fprintf(stderr, "FSR2: dispatch failed (%d); using EASU\n", (int)err);
        fsr2Failed = 1;
        fsr2Active = 0;
        return 0;
    }
    return 1;
}

extern "C" void VK_FSR2_Shutdown(VkDevice device) {
    (void)device;
    if (fsr2ContextCreated) {
        ffxFsr2ContextDestroy(&fsr2Context);
        fsr2ContextCreated = 0;
    }
    if (fsr2Scratch) {
        free(fsr2Scratch);
        fsr2Scratch = NULL;
    }
    fsr2ScratchSize = 0;
    fsr2InterfaceReady = 0;
    fsr2Active = 0;
    fsr2Failed = 0;
    fsr2Initialized = 0;
}
