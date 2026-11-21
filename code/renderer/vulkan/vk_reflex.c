/*
 * NVIDIA Reflex + Boost via VK_NV_low_latency2.
 *
 * Reflex has two parts:
 *   - sleep before simulation, using a timeline semaphore, so the CPU does not
 *     run ahead of the GPU and input latency stays low;
 *   - latency markers + present IDs so the driver can measure and schedule the
 *     frame, and keep clocks boosted when Boost is requested.
 */

#include "../tr_local.h"
#include "vk_reflex.h"

extern qboolean vk_low_latency_enabled;
extern cvar_t *r_reflex;
extern cvar_t *r_reflexBoost;

static qboolean reflex_available = qfalse;
static qboolean reflex_enabled = qfalse;
static qboolean reflex_sleep_ok = qtrue;
static qboolean reflex_warned = qfalse;
static int reflex_applied_enabled = -1;
static int reflex_applied_boost = -1;

static VkDevice reflex_device = VK_NULL_HANDLE;
static VkSwapchainKHR reflex_swapchain = VK_NULL_HANDLE;
static VkSemaphore reflex_semaphore = VK_NULL_HANDLE;
static uint64_t reflex_semaphore_value = 0;
static uint64_t reflex_present_id = 0;
static qboolean reflex_present_id_used = qfalse;

static qboolean reflex_wanted(void) {
	return r_reflex && r_reflex->integer;
}

static qboolean reflex_boost_wanted(void) {
	return r_reflexBoost && r_reflexBoost->integer;
}

static void reflex_set_marker(VkLatencyMarkerNV marker) {
	VkSetLatencyMarkerInfoNV info;

	if (!reflex_available || !reflex_swapchain || !vkSetLatencyMarkerNV)
		return;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
	info.presentID = reflex_present_id;
	info.marker = marker;
	vkSetLatencyMarkerNV(reflex_device, reflex_swapchain, &info);
}

static void reflex_apply_sleep_mode(void) {
	qboolean want;
	qboolean boost;
	VkLatencySleepModeInfoNV info;
	VkResult result;

	if (!reflex_available || !reflex_swapchain || !vkSetLatencySleepModeNV) {
		reflex_enabled = qfalse;
		return;
	}

	want = reflex_wanted();
	boost = want && reflex_boost_wanted();
	if ((int)want == reflex_applied_enabled && (int)boost == reflex_applied_boost) {
		reflex_enabled = want;
		return;
	}

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
	info.lowLatencyMode = want ? VK_TRUE : VK_FALSE;
	info.lowLatencyBoost = boost ? VK_TRUE : VK_FALSE;
	info.minimumIntervalUs = 0;

	result = vkSetLatencySleepModeNV(reflex_device, reflex_swapchain, &info);
	if (result != VK_SUCCESS) {
		if (!reflex_warned) {
			ri.Printf(PRINT_WARNING, "Reflex: vkSetLatencySleepModeNV failed (0x%x)\n", result);
			reflex_warned = qtrue;
		}
		reflex_enabled = qfalse;
		return;
	}

	reflex_applied_enabled = want;
	reflex_applied_boost = boost;
	reflex_enabled = want;
	ri.Printf(PRINT_ALL, "Reflex: %s%s\n", want ? "on" : "off",
	          boost ? " + Boost" : "");
}

qboolean VK_Reflex_Init(VkDevice device, VkPhysicalDevice physicalDevice) {
	VkSemaphoreTypeCreateInfo typeInfo;
	VkSemaphoreCreateInfo createInfo;
	VkResult result;

	(void)physicalDevice;

	if (!vk_low_latency_enabled) {
		if (r_reflex && !r_reflex->integer)
			ri.Printf(PRINT_ALL, "Reflex: disabled\n");
		else
			ri.Printf(PRINT_ALL, "Reflex: VK_NV_low_latency2 unavailable\n");
		return qfalse;
	}

	reflex_device = device;
	vkSetLatencySleepModeNV = (PFN_vkSetLatencySleepModeNV)vkGetDeviceProcAddr(device, "vkSetLatencySleepModeNV");
	vkLatencySleepNV = (PFN_vkLatencySleepNV)vkGetDeviceProcAddr(device, "vkLatencySleepNV");
	vkSetLatencyMarkerNV = (PFN_vkSetLatencyMarkerNV)vkGetDeviceProcAddr(device, "vkSetLatencyMarkerNV");
	vkGetLatencyTimingsNV = (PFN_vkGetLatencyTimingsNV)vkGetDeviceProcAddr(device, "vkGetLatencyTimingsNV");
	vkQueueNotifyOutOfBandNV = (PFN_vkQueueNotifyOutOfBandNV)vkGetDeviceProcAddr(device, "vkQueueNotifyOutOfBandNV");

	if (!vkSetLatencySleepModeNV || !vkLatencySleepNV || !vkSetLatencyMarkerNV ||
	    !vkWaitSemaphores) {
		ri.Printf(PRINT_WARNING, "Reflex: low-latency entry points missing\n");
		return qfalse;
	}

	memset(&typeInfo, 0, sizeof(typeInfo));
	typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	typeInfo.initialValue = 0;

	memset(&createInfo, 0, sizeof(createInfo));
	createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	createInfo.pNext = &typeInfo;

	result = vkCreateSemaphore(device, &createInfo, NULL, &reflex_semaphore);
	if (result != VK_SUCCESS) {
		ri.Printf(PRINT_WARNING, "Reflex: timeline semaphore creation failed (0x%x)\n", result);
		return qfalse;
	}

	reflex_available = qtrue;
	reflex_warned = qfalse;
	ri.Printf(PRINT_ALL, "Reflex: VK_NV_low_latency2 ready\n");
	return qtrue;
}

void VK_Reflex_AttachSwapchain(VkSwapchainKHR swapchain) {
	reflex_swapchain = swapchain;
	reflex_applied_enabled = -1;
	reflex_applied_boost = -1;
	reflex_apply_sleep_mode();
}

void VK_Reflex_DetachSwapchain(void) {
	if (reflex_available && reflex_swapchain && vkSetLatencySleepModeNV) {
		VkLatencySleepModeInfoNV info;
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
		info.lowLatencyMode = VK_FALSE;
		info.lowLatencyBoost = VK_FALSE;
		vkSetLatencySleepModeNV(reflex_device, reflex_swapchain, &info);
	}
	reflex_swapchain = VK_NULL_HANDLE;
	reflex_enabled = qfalse;
	reflex_applied_enabled = -1;
	reflex_applied_boost = -1;
}

void VK_Reflex_Shutdown(VkDevice device) {
	VK_Reflex_DetachSwapchain();
	if (reflex_semaphore) {
		vkDestroySemaphore(device, reflex_semaphore, NULL);
		reflex_semaphore = VK_NULL_HANDLE;
	}
	reflex_available = qfalse;
	reflex_enabled = qfalse;
	reflex_sleep_ok = qtrue;
	reflex_present_id = 0;
	reflex_present_id_used = qfalse;
	reflex_semaphore_value = 0;
	reflex_device = VK_NULL_HANDLE;
}

qboolean VK_Reflex_Active(void) {
	return reflex_available && reflex_enabled && reflex_swapchain != VK_NULL_HANDLE;
}

uint64_t VK_Reflex_PresentId(void) {
	return VK_Reflex_Active() ? reflex_present_id : 0;
}

void VK_Reflex_SimulationStart(void) {
	if (!reflex_available)
		return;

	reflex_apply_sleep_mode();
	if (!VK_Reflex_Active())
		return;

	reflex_present_id++;
	reflex_present_id_used = qfalse;

	if (reflex_sleep_ok && vkLatencySleepNV && vkWaitSemaphores) {
		VkLatencySleepInfoNV sleepInfo;
		VkSemaphoreWaitInfo waitInfo;
		VkResult result;

		reflex_semaphore_value++;
		memset(&sleepInfo, 0, sizeof(sleepInfo));
		sleepInfo.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
		sleepInfo.signalSemaphore = reflex_semaphore;
		sleepInfo.value = reflex_semaphore_value;

		result = vkLatencySleepNV(reflex_device, reflex_swapchain, &sleepInfo);
		if (result == VK_SUCCESS) {
			memset(&waitInfo, 0, sizeof(waitInfo));
			waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			waitInfo.semaphoreCount = 1;
			waitInfo.pSemaphores = &reflex_semaphore;
			waitInfo.pValues = &reflex_semaphore_value;
			result = vkWaitSemaphores(reflex_device, &waitInfo, 1000000000ull);
		}

		if (result != VK_SUCCESS && !reflex_warned) {
			ri.Printf(PRINT_WARNING, "Reflex: sleep failed (0x%x); continuing without sleep\n", result);
			reflex_warned = qtrue;
			reflex_sleep_ok = qfalse;
		}
	}

	reflex_set_marker(VK_LATENCY_MARKER_SIMULATION_START_NV);
}

void VK_Reflex_SimulationEnd(void) {
	reflex_set_marker(VK_LATENCY_MARKER_SIMULATION_END_NV);
}

void VK_Reflex_RenderSubmitStart(void) {
	if (!VK_Reflex_Active())
		return;
	/* A UI-only redraw can reach the swapchain without a new simulation
	 * frame; keep present IDs unique in that case. */
	if (reflex_present_id_used)
		reflex_present_id++;
	reflex_present_id_used = qtrue;
	reflex_set_marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
}

void VK_Reflex_RenderSubmitEnd(void) {
	reflex_set_marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
}

void VK_Reflex_PresentStart(void) {
	reflex_set_marker(VK_LATENCY_MARKER_PRESENT_START_NV);
}

void VK_Reflex_PresentEnd(void) {
	reflex_set_marker(VK_LATENCY_MARKER_PRESENT_END_NV);
}
