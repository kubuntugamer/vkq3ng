#include "../tr_local.h"
#include "../../shader/glsl/constants.h"

static uint32_t VK_AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static void VK_CreatePipelineCache(vkrtpipeline_t *pipeline);
static void VK_CreatePipelineLayout(vkrtpipeline_t*pipeline);
// RTX
static void VK_CreateShaderBindingTable(vkrtpipeline_t* pipeline);
static void VK_CreateRayTracingPipeline(vkrtpipeline_t* pipeline);

/*
 * Use these functions to define pipeline before calling VK_FinishPipeline
 */
void VK_SetRayTracingDescriptorSet(vkrtpipeline_t *pipeline, vkdescriptor_t *descriptor){
    pipeline->descriptor = descriptor;
}
void VK_Set2RayTracingDescriptorSets(vkrtpipeline_t* pipeline, vkdescriptor_t* descriptor, vkdescriptor_t* descriptor2) {
	pipeline->descriptor = descriptor;
	pipeline->descriptor2 = descriptor2;
}

void VK_SetRayTracingShader(vkrtpipeline_t *pipeline, vkshader_t *shader){
    pipeline->shader = shader;
}

void VK_AddRayTracingPushConstant(vkrtpipeline_t* pipeline,
	VkShaderStageFlags      stageFlags,
	uint32_t                offset,
	uint32_t                size) {
	pipeline->pushConstantRange.size++;
	pipeline->pushConstantRange.p = realloc(pipeline->pushConstantRange.p, pipeline->pushConstantRange.size * sizeof(VkPushConstantRange));
	pipeline->pushConstantRange.p[pipeline->pushConstantRange.size - 1].stageFlags = stageFlags;
	pipeline->pushConstantRange.p[pipeline->pushConstantRange.size - 1].offset = offset;
	pipeline->pushConstantRange.p[pipeline->pushConstantRange.size - 1].size = size;
}
/*
 *
 */

void VK_FinishRayTracingPipeline(vkrtpipeline_t *pipeline) {
	VK_CreatePipelineLayout(pipeline);
	VK_CreateRayTracingPipeline(pipeline);
	VK_CreateShaderBindingTable(pipeline);
}

void VK_DestroyRayTracingPipeline(vkrtpipeline_t *pipeline) {
	VK_DestroyBuffer(&pipeline->shaderBindingTableBuffer);

    if (pipeline->layout) {
        vkDestroyPipelineLayout(vk.device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
    }
    
	if (pipeline->handle) {
		vkDestroyPipeline(vk.device, pipeline->handle, NULL);
		pipeline->handle = VK_NULL_HANDLE;
	}

	if (pipeline->cache) {
		vkDestroyPipelineCache(vk.device, pipeline->cache, NULL);
		pipeline->cache = VK_NULL_HANDLE;
	}

	memset(pipeline, 0, sizeof(vkrtpipeline_t));
}

static void VK_CreatePipelineCache(vkrtpipeline_t*pipeline)
{
    VkPipelineCacheCreateInfo pipelineCacheInfo = { 0 };
    pipelineCacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    VK_CHECK(vkCreatePipelineCache(vk.device, &pipelineCacheInfo, NULL, &pipeline->cache), "failed to create Pipeline Cache!");
}

static void VK_CreatePipelineLayout(vkrtpipeline_t*pipeline)
{
    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { 0 };
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    
	VkDescriptorSetLayout dLayout[2];
	dLayout[0] = pipeline->descriptor->layout;
	if (pipeline->descriptor2 != NULL) {
		dLayout[1] = pipeline->descriptor2->layout;
		pipelineLayoutInfo.setLayoutCount = 2;
	}
	else if (pipeline->descriptor->layout != VK_NULL_HANDLE) {
		pipelineLayoutInfo.setLayoutCount = 1;
	}
	else {
		pipelineLayoutInfo.setLayoutCount = 0;
	}
    pipelineLayoutInfo.pSetLayouts = &dLayout[0];
    pipelineLayoutInfo.pushConstantRangeCount = pipeline->pushConstantRange.size;
    pipelineLayoutInfo.pPushConstantRanges = &pipeline->pushConstantRange.p[0];

    VK_CHECK(vkCreatePipelineLayout(vk.device, &pipelineLayoutInfo, NULL, &pipeline->layout), "failed to create Pipeline Layout!");
}

static void VK_CreateRTXPipeline(vkrtpipeline_t* pipeline)
{
}

static void VK_CreateRayTracingPipeline(vkrtpipeline_t* pipeline)
{
	VkRayTracingPipelineCreateInfoKHR rayPipelineInfo = { 0 };
	rayPipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
	rayPipelineInfo.stageCount = (uint32_t)pipeline->shader->size;
	rayPipelineInfo.pStages = &pipeline->shader->shaderStageCreateInfos[0];
	rayPipelineInfo.groupCount = (uint32_t)pipeline->shader->shaderGroupSize;
	rayPipelineInfo.pGroups = &pipeline->shader->shaderGroupCreateInfos[0];
	rayPipelineInfo.maxPipelineRayRecursionDepth = 5;
	rayPipelineInfo.layout = pipeline->layout;
	VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rayPipelineInfo, NULL, &pipeline->handle), " failed to create Ray Tracing Pipeline");
}

static void VK_CreateShaderBindingTable(vkrtpipeline_t *pipeline) {
	const uint32_t handleSize = vk.rayTracingProperties.shaderGroupHandleSize;
	const uint32_t groupCount = (uint32_t)(pipeline->shader->shaderGroupSize ? pipeline->shader->shaderGroupSize : pipeline->shader->size);
	const uint32_t align = vk.rayTracingProperties.shaderGroupBaseAlignment > vk.rayTracingProperties.shaderGroupHandleAlignment ? vk.rayTracingProperties.shaderGroupBaseAlignment : vk.rayTracingProperties.shaderGroupHandleAlignment;
	const uint32_t stride = VK_AlignUp(handleSize, align);
	const uint32_t handleStorageSize = handleSize * groupCount;
	const uint32_t sbtSize = stride * groupCount;

	pipeline->sbtGroupCount = groupCount;
	pipeline->sbtRecordStride = stride;

	VK_CreateShaderBindingTableBuffer(&pipeline->shaderBindingTableBuffer, sbtSize);

	uint8_t* shaderHandleStorage = calloc(handleStorageSize, sizeof(uint8_t));
	uint8_t* sbt = calloc(sbtSize, sizeof(uint8_t));
	VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, pipeline->handle, 0, groupCount, handleStorageSize, shaderHandleStorage), "failed to get shader handles");
	for (uint32_t i = 0; i < groupCount; i++) {
		Com_Memcpy(sbt + i * stride, shaderHandleStorage + i * handleSize, handleSize);
	}
	VK_UploadBufferData(&pipeline->shaderBindingTableBuffer, (void*)sbt);
	VK_UnmapBuffer(&pipeline->shaderBindingTableBuffer);
	free(shaderHandleStorage);
	free(sbt);
}

void VK_BindRayTracingDescriptorSet(vkrtpipeline_t *pipeline, vkdescriptor_t *descriptor) {
	VkCommandBuffer commandBuffer = vk.swapchain.commandBuffers[vk.swapchain.currentImage];
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline->layout, 0, 1,
		&descriptor->set, 0, NULL);
}

void VK_Bind2RayTracingDescriptorSets(vkrtpipeline_t* pipeline, vkdescriptor_t* descriptor1, vkdescriptor_t* descriptor2) {
	VkCommandBuffer commandBuffer = vk.swapchain.commandBuffers[vk.swapchain.currentImage];

	VkDescriptorSet sets[2] = { descriptor1->set, descriptor2->set };
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline->layout, 0, 2,
		&sets[0], 0, NULL);
}

void VK_BindRayTracingPipeline(vkrtpipeline_t *pipeline) {
	VkCommandBuffer commandBuffer = vk.swapchain.commandBuffers[vk.swapchain.currentImage];
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline->handle);
}

void VK_SetRayTracingPushConstant(vkrtpipeline_t *pipeline, VkShaderStageFlags stage, uint32_t offset, uint32_t size, void* data)
{
    VkCommandBuffer commandBuffer = vk.swapchain.commandBuffers[vk.swapchain.currentImage];
    vkCmdPushConstants(commandBuffer, pipeline->layout, stage, offset, size, data);
}

void VK_TraceRays(vkrtpipeline_t* pipeline) {
	VkCommandBuffer commandBuffer = vk.swapchain.commandBuffers[vk.swapchain.currentImage];
	const VkDeviceAddress sbtAddress = VK_GetBufferDeviceAddress(&pipeline->shaderBindingTableBuffer);
	const uint64_t stride = pipeline->sbtRecordStride;
	const uint64_t regionSize = stride * pipeline->sbtGroupCount;

	VkStridedDeviceAddressRegionKHR raygen = { 0 };
	raygen.deviceAddress = sbtAddress;
	raygen.stride = stride;
	raygen.size = stride;

	VkStridedDeviceAddressRegionKHR miss = { 0 };
	miss.deviceAddress = sbtAddress;
	miss.stride = stride;
	miss.size = regionSize;

	VkStridedDeviceAddressRegionKHR hit = { 0 };
	hit.deviceAddress = sbtAddress;
	hit.stride = stride;
	hit.size = regionSize;

	VkStridedDeviceAddressRegionKHR callable = { 0 };

	vkCmdTraceRaysKHR(commandBuffer, &raygen, &miss, &hit, &callable,
		vk.renderExtent.width, vk.renderExtent.height, 1);
}
