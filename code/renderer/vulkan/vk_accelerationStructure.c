#include "../tr_local.h"

static VkDeviceSize VK_AlignAS(VkDeviceSize value, VkDeviceSize alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static VkDeviceSize VK_ASStorageAlign(void)
{
	return 256;
}

static VkDeviceSize VK_ASScratchAlign(void)
{
	VkDeviceSize alignment = vk.accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
	if (alignment == 0)
		alignment = 256;
	return alignment;
}

static void VK_FillTrianglesGeometry(const vkRTGeometry *src, VkAccelerationStructureGeometryKHR *dst, uint32_t *primitiveCount)
{
	Com_Memset(dst, 0, sizeof(*dst));
	dst->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	dst->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	dst->flags = src->flags;

	VkAccelerationStructureGeometryTrianglesDataKHR *triangles = &dst->geometry.triangles;
	triangles->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles->vertexFormat = src->geometry.triangles.vertexFormat;
	triangles->vertexData.deviceAddress = VK_GetBufferDeviceAddressHandle(src->geometry.triangles.vertexData) + src->geometry.triangles.vertexOffset;
	triangles->vertexStride = src->geometry.triangles.vertexStride;
	triangles->maxVertex = src->geometry.triangles.vertexCount > 0 ? src->geometry.triangles.vertexCount - 1 : 0;
	triangles->indexType = src->geometry.triangles.indexType;
	triangles->indexData.deviceAddress = VK_GetBufferDeviceAddressHandle(src->geometry.triangles.indexData) + src->geometry.triangles.indexOffset;

	*primitiveCount = src->geometry.triangles.indexCount / 3;
}

static VkDeviceAddress VK_AllocScratch(VkDeviceSize size)
{
	if (size == 0)
		return 0;

	const VkDeviceSize alignment = VK_ASScratchAlign();
	const VkDeviceSize offset = VK_AlignAS(vk_d.scratchBufferOffset, alignment);
	if (offset + size > vk_d.scratchBuffer.allocSize)
		ri.Error(ERR_FATAL, "Vulkan: Scratch Buffer to small!");

	vk_d.scratchBufferOffset = offset + size;
	return VK_GetBufferDeviceAddress(&vk_d.scratchBuffer) + offset;
}

static VkDeviceSize VK_AllocASStorage(vkbuffer_t *storage, VkDeviceSize size, VkDeviceSize *offset)
{
	const VkDeviceSize start = VK_AlignAS(offset ? *offset : 0, VK_ASStorageAlign());
	const VkDeviceSize end = start + VK_AlignAS(size, VK_ASStorageAlign());
	if (end > storage->allocSize)
		ri.Error(ERR_FATAL, "Vulkan: Acceleration Structure Buffer to small!");
	if (offset)
		*offset = end;
	return start;
}

static void VK_CreateAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size,
										   vkbuffer_t *storage, VkDeviceSize offset,
										   VkAccelerationStructureKHR *accelerationStructure, uint64_t *address)
{
	VkAccelerationStructureCreateInfoKHR createInfo = { 0 };
	createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	createInfo.type = type;
	createInfo.size = size;
	createInfo.buffer = storage->buffer;
	createInfo.offset = offset;

	VK_CHECK(vkCreateAccelerationStructureKHR(vk.device, &createInfo, NULL, accelerationStructure), "failed to create Acceleration Structure KHR");

	if (address) {
		VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { 0 };
		addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		addressInfo.accelerationStructure = *accelerationStructure;
		*address = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &addressInfo);
	}
}

static void VK_ASMemoryBarrier(VkCommandBuffer commandBuffer)
{
	VkMemoryBarrier barrier = { 0 };
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(commandBuffer,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		0, 1, &barrier, 0, NULL, 0, NULL);
}

void VK_CreateBottomAS(VkCommandBuffer commandBuffer,
						vkbottomAS_t* bas, vkbuffer_t *bottomASBuffer,
						VkDeviceSize* offset, VkBuildAccelerationStructureFlagsKHR flag)
{
	VkAccelerationStructureGeometryKHR geometry = { 0 };
	uint32_t primitiveCount = 0;
	VK_FillTrianglesGeometry(&bas->geometries, &geometry, &primitiveCount);

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { 0 };
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = flag;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizes = { 0 };
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizes);

	const VkDeviceSize storageOffset = VK_AllocASStorage(bottomASBuffer, sizes.accelerationStructureSize, offset);
	bas->offset = storageOffset;
	VK_CreateAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
		sizes.accelerationStructureSize, bottomASBuffer, storageOffset,
		&bas->accelerationStructure, &bas->handle);

	buildInfo.dstAccelerationStructure = bas->accelerationStructure;
	buildInfo.scratchData.deviceAddress = VK_AllocScratch(sizes.buildScratchSize);

	VkAccelerationStructureBuildRangeInfoKHR range = { 0 };
	range.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, &pRange);
	VK_ASMemoryBarrier(commandBuffer);
}

void VK_UpdateBottomAS(VkCommandBuffer commandBuffer,
						vkbottomAS_t* oldBas, vkbottomAS_t* newBas,
						vkbuffer_t* bottomASBuffer, VkDeviceSize* offset, VkBuildAccelerationStructureFlagsKHR flag)
{
	VkAccelerationStructureGeometryKHR geometry = { 0 };
	uint32_t primitiveCount = 0;
	VK_FillTrianglesGeometry(&newBas->geometries, &geometry, &primitiveCount);

	const qboolean updateInPlace = (oldBas == newBas);
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { 0 };
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = flag;
	buildInfo.mode = updateInPlace ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizes = { 0 };
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizes);

	if (updateInPlace) {
		buildInfo.srcAccelerationStructure = oldBas->accelerationStructure;
		buildInfo.dstAccelerationStructure = newBas->accelerationStructure;
		buildInfo.scratchData.deviceAddress = VK_AllocScratch(sizes.updateScratchSize ? sizes.updateScratchSize : sizes.buildScratchSize);
	}
	else {
		const VkDeviceSize storageOffset = VK_AllocASStorage(bottomASBuffer, sizes.accelerationStructureSize, offset);
		newBas->offset = storageOffset;
		VK_CreateAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			sizes.accelerationStructureSize, bottomASBuffer, storageOffset,
			&newBas->accelerationStructure, &newBas->handle);
		buildInfo.dstAccelerationStructure = newBas->accelerationStructure;
		buildInfo.scratchData.deviceAddress = VK_AllocScratch(sizes.buildScratchSize);
	}

	VkAccelerationStructureBuildRangeInfoKHR range = { 0 };
	range.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, &pRange);
	VK_ASMemoryBarrier(commandBuffer);
}

// destroy and create new AS
void VK_RecreateBottomAS(VkCommandBuffer commandBuffer, vkbottomAS_t* bas, vkbuffer_t* bottomASBuffer, VkBuildAccelerationStructureFlagsKHR flag) {
	if (bas->accelerationStructure != VK_NULL_HANDLE)
		vkDestroyAccelerationStructureKHR(vk.device, bas->accelerationStructure, NULL);
	bas->accelerationStructure = VK_NULL_HANDLE;
	bas->handle = 0;
	VK_CreateBottomAS(commandBuffer, bas, bottomASBuffer, NULL, flag);
}

void VK_MakeTopAS(VkCommandBuffer commandBuffer,
					vktopAS_t* topAS, vkbuffer_t* topASBuffer,
					vkbottomAS_t* basList, uint32_t basCount, vkbuffer_t* instanceBuffer,
					VkBuildAccelerationStructureFlagsKHR flag)
{
	(void)basList;

	VkAccelerationStructureGeometryKHR geometry = { 0 };
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.arrayOfPointers = VK_FALSE;
	geometry.geometry.instances.data.deviceAddress = VK_GetBufferDeviceAddress(instanceBuffer);

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { 0 };
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags = flag;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	uint32_t primitiveCount = basCount;
	VkAccelerationStructureBuildSizesInfoKHR sizes = { 0 };
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizes);

	const VkDeviceSize storageOffset = VK_AllocASStorage(topASBuffer, sizes.accelerationStructureSize, NULL);
	topAS->offset = storageOffset;
	VK_CreateAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
		sizes.accelerationStructureSize, topASBuffer, storageOffset,
		&topAS->accelerationStructure, &topAS->handle);

	buildInfo.dstAccelerationStructure = topAS->accelerationStructure;
	buildInfo.scratchData.deviceAddress = VK_AllocScratch(sizes.buildScratchSize);

	VkAccelerationStructureBuildRangeInfoKHR range = { 0 };
	range.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, &pRange);
	VK_ASMemoryBarrier(commandBuffer);
}

void VK_DestroyTopAccelerationStructure(vktopAS_t* as) {
	if (as->accelerationStructure != VK_NULL_HANDLE) {
		vkDestroyAccelerationStructureKHR(vk.device, as->accelerationStructure, NULL);
		as->accelerationStructure = VK_NULL_HANDLE;
	}
	memset(as, 0, sizeof(vktopAS_t));
}

void VK_DestroyBottomAccelerationStructure(vkbottomAS_t* as) {
	if (as->accelerationStructure != VK_NULL_HANDLE) {
		vkDestroyAccelerationStructureKHR(vk.device, as->accelerationStructure, NULL);
		as->accelerationStructure = VK_NULL_HANDLE;
	}
	memset(as, 0, sizeof(vkbottomAS_t));
}

void VK_DestroyAllAccelerationStructures() {
	for (int i = 0; i < vk.swapchain.imageCount; i++) {
		VK_DestroyTopAccelerationStructure(&vk_d.topAS[i]);
	}
	for (int i = 0; i < vk_d.bottomASCount; ++i) {
		VK_DestroyBottomAccelerationStructure(&vk_d.bottomASList[i]);
	}
	vk_d.bottomASCount = 0;
}
