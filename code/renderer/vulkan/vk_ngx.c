/*
 * NVIDIA DLSS (NGX) integration for the Vulkan ray-tracing path.
 *
 * The runtime feature module (libnvidia-ngx-dlss.so) is supplied by the
 * NVIDIA DLSS SDK at runtime.  This file only links the small NGX loader
 * shim which dlopen()s the installed driver library.
 */

#include "../tr_local.h"
#include "ngx/include/nvsdk_ngx.h"
#include "ngx/include/nvsdk_ngx_vk.h"
#include "ngx/include/nvsdk_ngx_helpers_vk.h"

#include <sys/stat.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

static qboolean ngx_initialized = qfalse;
static qboolean ngx_available = qfalse;
static qboolean ngx_active = qfalse;
static qboolean ngx_create_failed = qfalse;
static qboolean ngx_eval_failed = qfalse;
static NVSDK_NGX_Handle *ngx_feature = NULL;
static NVSDK_NGX_Parameter *ngx_params = NULL;
static uint32_t ngx_in_width, ngx_in_height;
static uint32_t ngx_out_width, ngx_out_height;

static qboolean ngx_succeeded(NVSDK_NGX_Result result) {
	return NVSDK_NGX_SUCCEED(result) ? qtrue : qfalse;
}

qboolean VK_NGX_Available(void) {
	return ngx_available;
}

qboolean VK_NGX_Active(void) {
	return ngx_active && ngx_available;
}

static void ngx_application_path(char *out, size_t outSize) {
	const char *home = getenv("HOME");
	if (!home || !home[0]) home = ".";
	snprintf(out, outSize, "%s/.vkq3ng-ngx", home);
	mkdir(out, 0700);
}

qboolean VK_NGX_Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device) {
	char path[1024];
	wchar_t wpath[1024];
	wchar_t *featurePaths[2];
	NVSDK_NGX_FeatureCommonInfo commonInfo;
	NVSDK_NGX_Result result;

	if (ngx_initialized) return ngx_available;
	ngx_initialized = qtrue;

	ngx_application_path(path, sizeof(path));
	if (mbstowcs(wpath, path, sizeof(wpath) / sizeof(wpath[0])) == (size_t)-1)
		wcscpy(wpath, L"./ngx");

	/* The driver does not ship the DLSS feature module; the DLSS SDK places
	 * it in /usr/share/nvidia/ngx.  Tell NGX to look there as well. */
	featurePaths[0] = L"/usr/share/nvidia/ngx";
	featurePaths[1] = wpath;
	memset(&commonInfo, 0, sizeof(commonInfo));
	commonInfo.PathListInfo.Path = (wchar_t const* const*)featurePaths;
	commonInfo.PathListInfo.Length = 2;

	result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
			"a0f57b54-1daf-4934-90ae-c4035c19df04",
			NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", wpath,
			instance, physicalDevice, device,
			NULL, NULL, &commonInfo, NVSDK_NGX_Version_API);
	if (!ngx_succeeded(result)) {
		ri.Printf(PRINT_WARNING, "DLSS: NGX init failed (0x%x)\n", result);
		return qfalse;
	}

	if (!ngx_succeeded(NVSDK_NGX_VULKAN_GetCapabilityParameters(&ngx_params)) || !ngx_params) {
		ri.Printf(PRINT_WARNING, "DLSS: could not get NGX capability parameters\n");
		NVSDK_NGX_VULKAN_Shutdown1(device);
		return qfalse;
	}

	{
		int supported = 0;
		NVSDK_NGX_Parameter_GetI(ngx_params, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
		if (!supported) {
			int initResult = 0;
			NVSDK_NGX_Parameter_GetI(ngx_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
			ri.Printf(PRINT_WARNING, "DLSS: feature unavailable (init result 0x%x); using EASU\n", initResult);
			NVSDK_NGX_VULKAN_Shutdown1(device);
			return qfalse;
		}
	}

	ngx_available = qtrue;
	ri.Printf(PRINT_ALL, "DLSS: NVIDIA NGX ready\n");
	return qtrue;
}

qboolean VK_NGX_CreateDLSS(VkCommandBuffer cmd, uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight) {
	NVSDK_NGX_DLSS_Create_Params createParams;
	NVSDK_NGX_Parameter *params = NULL;
	NVSDK_NGX_Handle *feature = NULL;
	NVSDK_NGX_Result result;

	if (!ngx_available || !cmd || ngx_create_failed) return qfalse;

	if (ngx_feature && inWidth == ngx_in_width && inHeight == ngx_in_height &&
			outWidth == ngx_out_width && outHeight == ngx_out_height) {
		return qtrue;
	}

	if (ngx_feature) {
		NVSDK_NGX_VULKAN_ReleaseFeature(ngx_feature);
		ngx_feature = NULL;
	}

	if (!ngx_succeeded(NVSDK_NGX_VULKAN_AllocateParameters(&params)) || !params) {
		ri.Printf(PRINT_WARNING, "DLSS: failed to allocate NGX parameters\n");
		return qfalse;
	}

	memset(&createParams, 0, sizeof(createParams));
	createParams.Feature.InWidth = inWidth;
	createParams.Feature.InHeight = inHeight;
	createParams.Feature.InTargetWidth = outWidth;
	createParams.Feature.InTargetHeight = outHeight;
	createParams.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
	/* Motion vectors are at input resolution and not jittered. */
	createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
	createParams.InEnableOutputSubrects = false;

	result = NGX_VULKAN_CREATE_DLSS_EXT1(vk.device, cmd, 1, 1, &feature, params, &createParams);
	if (!ngx_succeeded(result) || !feature) {
		ri.Printf(PRINT_WARNING, "DLSS: CreateFeature failed (0x%x); falling back to EASU\n", result);
		ngx_create_failed = qtrue;
		return qfalse;
	}

	ngx_feature = feature;
	ngx_params = params;
	ngx_in_width = inWidth;
	ngx_in_height = inHeight;
	ngx_out_width = outWidth;
	ngx_out_height = outHeight;
	ngx_active = qtrue;
	ri.Printf(PRINT_ALL, "DLSS: active, %ux%u -> %ux%u\n", inWidth, inHeight, outWidth, outHeight);
	return qtrue;
}

qboolean VK_NGX_EvaluateDLSS(VkCommandBuffer cmd, vkimage_t *color, VkFormat colorFormat,
		vkimage_t *depth, VkFormat depthFormat, vkimage_t *motion, VkFormat motionFormat,
		vkimage_t *output, VkFormat outputFormat, qboolean reset) {
	VkImageSubresourceRange range;
	NVSDK_NGX_Resource_VK colorResource, depthResource, motionResource, outputResource;
	NVSDK_NGX_VK_DLSS_Eval_Params evalParams;
	NVSDK_NGX_Result result;

	if (!VK_NGX_Active() || !ngx_feature || !ngx_params) return qfalse;

	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;

	colorResource = NVSDK_NGX_Create_ImageView_Resource_VK(color->view, color->handle, range, colorFormat, ngx_in_width, ngx_in_height, false);
	depthResource = NVSDK_NGX_Create_ImageView_Resource_VK(depth->view, depth->handle, range, depthFormat, ngx_in_width, ngx_in_height, false);
	motionResource = NVSDK_NGX_Create_ImageView_Resource_VK(motion->view, motion->handle, range, motionFormat, ngx_in_width, ngx_in_height, false);
	outputResource = NVSDK_NGX_Create_ImageView_Resource_VK(output->view, output->handle, range, outputFormat, ngx_out_width, ngx_out_height, true);

	memset(&evalParams, 0, sizeof(evalParams));
	evalParams.Feature.pInColor = &colorResource;
	evalParams.Feature.pInOutput = &outputResource;
	evalParams.Feature.InSharpness = 0.0f;
	evalParams.pInDepth = &depthResource;
	evalParams.pInMotionVectors = &motionResource;
	evalParams.InJitterOffsetX = 0.0f;
	evalParams.InJitterOffsetY = 0.0f;
	evalParams.InRenderSubrectDimensions.Width = ngx_in_width;
	evalParams.InRenderSubrectDimensions.Height = ngx_in_height;
	evalParams.InReset = reset ? 1 : 0;
	/* Motion vectors are stored in NDC; convert to render-pixel units. */
	evalParams.InMVScaleX = 0.5f * (float)ngx_in_width;
	evalParams.InMVScaleY = 0.5f * (float)ngx_in_height;
	evalParams.InPreExposure = 1.0f;
	evalParams.InExposureScale = 1.0f;

	result = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, ngx_feature, ngx_params, &evalParams);
	if (!ngx_succeeded(result)) {
		if (!ngx_eval_failed) {
			ri.Printf(PRINT_WARNING, "DLSS: EvaluateFeature failed (0x%x); falling back to EASU\n", result);
			ngx_eval_failed = qtrue;
		}
		ngx_active = qfalse;
		return qfalse;
	}
	return qtrue;
}

void VK_NGX_Shutdown(VkDevice device) {
	if (ngx_feature) {
		NVSDK_NGX_VULKAN_ReleaseFeature(ngx_feature);
		ngx_feature = NULL;
	}
	if (ngx_available && device) {
		NVSDK_NGX_VULKAN_Shutdown1(device);
	}
	ngx_available = qfalse;
	ngx_active = qfalse;
	ngx_create_failed = qfalse;
	ngx_eval_failed = qfalse;
	ngx_params = NULL;
	ngx_initialized = qfalse;
}
