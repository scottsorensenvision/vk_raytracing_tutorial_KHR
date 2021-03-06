/* Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "raytrace.hpp"
#include "nvh/fileoperations.hpp"
#include "nvvk/descriptorsets_vk.hpp"

#include "nvvk/shaders_vk.hpp"
#include "obj_loader.h"

extern std::vector<std::string> defaultSearchPaths;


void Raytracer::setup(const vk::Device&         device,
                      const vk::PhysicalDevice& physicalDevice,
                      nvvk::Allocator*            allocator,
                      uint32_t                  queueFamily)
{
  m_device             = device;
  m_physicalDevice     = physicalDevice;
  m_alloc              = allocator;
  m_graphicsQueueIndex = queueFamily;

  // Requesting ray tracing properties
  auto properties = m_physicalDevice.getProperties2<vk::PhysicalDeviceProperties2,
                                                    vk::PhysicalDeviceRayTracingPropertiesKHR>();
  m_rtProperties  = properties.get<vk::PhysicalDeviceRayTracingPropertiesKHR>();
  m_rtBuilder.setup(m_device, allocator, m_graphicsQueueIndex);

  m_debug.setup(device);
}


void Raytracer::destroy()
{
  m_rtBuilder.destroy();
  m_device.destroy(m_rtDescPool);
  m_device.destroy(m_rtDescSetLayout);
  m_device.destroy(m_rtPipeline);
  m_device.destroy(m_rtPipelineLayout);
  m_alloc->destroy(m_rtSBTBuffer);
}

//--------------------------------------------------------------------------------------------------
// Converting a OBJ primitive to the ray tracing geometry used for the BLAS
//
nvvk::RaytracingBuilderKHR::Blas Raytracer::objectToVkGeometryKHR(const ObjModel& model)
{
  // Setting up the creation info of acceleration structure
  vk::AccelerationStructureCreateGeometryTypeInfoKHR asCreate;
  asCreate.setGeometryType(vk::GeometryTypeKHR::eTriangles);
  asCreate.setIndexType(vk::IndexType::eUint32);
  asCreate.setVertexFormat(vk::Format::eR32G32B32Sfloat);
  asCreate.setMaxPrimitiveCount(model.nbIndices / 3);  // Nb triangles
  asCreate.setMaxVertexCount(model.nbVertices);
  asCreate.setAllowsTransforms(VK_FALSE);  // No adding transformation matrices

  // Building part
  vk::DeviceAddress vertexAddress = m_device.getBufferAddress({model.vertexBuffer.buffer});
  vk::DeviceAddress indexAddress  = m_device.getBufferAddress({model.indexBuffer.buffer});

  vk::AccelerationStructureGeometryTrianglesDataKHR triangles;
  triangles.setVertexFormat(asCreate.vertexFormat);
  triangles.setVertexData(vertexAddress);
  triangles.setVertexStride(sizeof(VertexObj));
  triangles.setIndexType(asCreate.indexType);
  triangles.setIndexData(indexAddress);
  triangles.setTransformData({});

  // Setting up the build info of the acceleration
  vk::AccelerationStructureGeometryKHR asGeom;
  asGeom.setGeometryType(asCreate.geometryType);
  asGeom.setFlags(vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation);  // For AnyHit
  asGeom.geometry.setTriangles(triangles);


  vk::AccelerationStructureBuildOffsetInfoKHR offset;
  offset.setFirstVertex(0);
  offset.setPrimitiveCount(asCreate.maxPrimitiveCount);
  offset.setPrimitiveOffset(0);
  offset.setTransformOffset(0);

  nvvk::RaytracingBuilderKHR::Blas blas;
  blas.asGeometry.emplace_back(asGeom);
  blas.asCreateGeometryInfo.emplace_back(asCreate);
  blas.asBuildOffsetInfo.emplace_back(offset);
  return blas;
}


//--------------------------------------------------------------------------------------------------
// Returning the ray tracing geometry used for the BLAS, containing all spheres
//
nvvk::RaytracingBuilderKHR::Blas Raytracer::implicitToVkGeometryKHR(const ImplInst& implicitObj)
{

  // Setting up the creation info of acceleration structure
  vk::AccelerationStructureCreateGeometryTypeInfoKHR asCreate;
  asCreate.setGeometryType(vk::GeometryTypeKHR::eAabbs);
  asCreate.setIndexType(vk::IndexType::eNoneKHR);
  asCreate.setVertexFormat(vk::Format::eUndefined);
  asCreate.setMaxPrimitiveCount(static_cast<uint32_t>(implicitObj.objImpl.size()));  // Nb triangles
  asCreate.setMaxVertexCount(0);
  asCreate.setAllowsTransforms(VK_FALSE);  // No adding transformation matrices

  vk::DeviceAddress dataAddress = m_device.getBufferAddress({implicitObj.implBuf.buffer});

  vk::AccelerationStructureGeometryAabbsDataKHR aabbs;
  aabbs.setData(dataAddress);
  aabbs.setStride(sizeof(ObjImplicit));

  // Setting up the build info of the acceleration
  vk::AccelerationStructureGeometryKHR asGeom;
  asGeom.setGeometryType(asCreate.geometryType);
  asGeom.setFlags(vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation);  // For AnyHit
  asGeom.geometry.setAabbs(aabbs);


  vk::AccelerationStructureBuildOffsetInfoKHR offset;
  offset.setFirstVertex(0);
  offset.setPrimitiveCount(asCreate.maxPrimitiveCount);
  offset.setPrimitiveOffset(0);
  offset.setTransformOffset(0);

  nvvk::RaytracingBuilderKHR::Blas blas;
  blas.asGeometry.emplace_back(asGeom);
  blas.asCreateGeometryInfo.emplace_back(asCreate);
  blas.asBuildOffsetInfo.emplace_back(offset);
  return blas;
}


void Raytracer::createBottomLevelAS(std::vector<ObjModel>& models, ImplInst& implicitObj)
{
  // BLAS - Storing each primitive in a geometry
  std::vector<nvvk::RaytracingBuilderKHR::Blas> allBlas;
  allBlas.reserve(models.size());
  for(const auto& obj : models)
  {
    auto blas = objectToVkGeometryKHR(obj);

    // We could add more geometry in each BLAS, but we add only one for now
    allBlas.emplace_back(blas);
  }

  // Adding implicit
  if(!implicitObj.objImpl.empty())
  {
    auto blas = implicitToVkGeometryKHR(implicitObj);
    allBlas.emplace_back(blas);
    implicitObj.blasId = static_cast<int>(allBlas.size() - 1);  // remember blas ID for tlas
  }


  m_rtBuilder.buildBlas(allBlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
                                     | vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction);
}

void Raytracer::createTopLevelAS(std::vector<ObjInstance>& instances, ImplInst& implicitObj)
{
  std::vector<nvvk::RaytracingBuilderKHR::Instance> tlas;
  tlas.reserve(instances.size());
  for(int i = 0; i < static_cast<int>(instances.size()); i++)
  {
    nvvk::RaytracingBuilderKHR::Instance rayInst;
    rayInst.transform  = instances[i].transform;  // Position of the instance
    rayInst.instanceId = i;                       // gl_InstanceID
    rayInst.blasId     = instances[i].objIndex;
    rayInst.hitGroupId = 0;  // We will use the same hit group for all objects
    rayInst.flags      = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    tlas.emplace_back(rayInst);
  }

  // Add the blas containing all implicit
  if(!implicitObj.objImpl.empty())
  {
    nvvk::RaytracingBuilderKHR::Instance rayInst;
    rayInst.transform  = implicitObj.transform;                      // Position of the instance
    rayInst.instanceId = static_cast<uint32_t>(implicitObj.blasId);  // Same for material index
    rayInst.blasId     = static_cast<uint32_t>(implicitObj.blasId);
    rayInst.hitGroupId = 1;  // We will use the same hit group for all objects (the second one)
    rayInst.flags      = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    tlas.emplace_back(rayInst);
  }

  m_rtBuilder.buildTlas(tlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace);
}

//--------------------------------------------------------------------------------------------------
// This descriptor set holds the Acceleration structure and the output image
//
void Raytracer::createRtDescriptorSet(const vk::ImageView& outputImage)
{
  using vkDT   = vk::DescriptorType;
  using vkSS   = vk::ShaderStageFlagBits;
  using vkDSLB = vk::DescriptorSetLayoutBinding;

  m_rtDescSetLayoutBind.addBinding(vkDSLB(0, vkDT::eAccelerationStructureKHR, 1,
                                          vkSS::eRaygenKHR | vkSS::eClosestHitKHR));  // TLAS
  m_rtDescSetLayoutBind.addBinding(
      vkDSLB(1, vkDT::eStorageImage, 1, vkSS::eRaygenKHR));  // Output image

  m_rtDescPool      = m_rtDescSetLayoutBind.createPool(m_device);
  m_rtDescSetLayout = m_rtDescSetLayoutBind.createLayout(m_device);
  m_rtDescSet       = m_device.allocateDescriptorSets({m_rtDescPool, 1, &m_rtDescSetLayout})[0];

  vk::AccelerationStructureKHR                   tlas = m_rtBuilder.getAccelerationStructure();
  vk::WriteDescriptorSetAccelerationStructureKHR descASInfo;
  descASInfo.setAccelerationStructureCount(1);
  descASInfo.setPAccelerationStructures(&tlas);
  vk::DescriptorImageInfo imageInfo{{}, outputImage, vk::ImageLayout::eGeneral};

  std::vector<vk::WriteDescriptorSet> writes;
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 0, &descASInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 1, &imageInfo));
  m_device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Writes the output image to the descriptor set
// - Required when changing resolution
//
void Raytracer::updateRtDescriptorSet(const vk::ImageView& outputImage)
{
  using vkDT = vk::DescriptorType;

  // (1) Output buffer
  vk::DescriptorImageInfo imageInfo{{}, outputImage, vk::ImageLayout::eGeneral};
  vk::WriteDescriptorSet  wds{m_rtDescSet, 1, 0, 1, vkDT::eStorageImage, &imageInfo};
  m_device.updateDescriptorSets(wds, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Pipeline for the ray tracer: all shaders, raygen, chit, miss
//
void Raytracer::createRtPipeline(vk::DescriptorSetLayout& sceneDescLayout)
{
  std::vector<std::string> paths = defaultSearchPaths;

  vk::ShaderModule raygenSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("shaders/raytrace.rgen.spv", true, paths));
  vk::ShaderModule missSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("shaders/raytrace.rmiss.spv", true, paths));

  // The second miss shader is invoked when a shadow ray misses the geometry. It
  // simply indicates that no occlusion has been found
  vk::ShaderModule shadowmissSM =
      nvvk::createShaderModule(m_device,
                               nvh::loadFile("shaders/raytraceShadow.rmiss.spv", true, paths));


  std::vector<vk::PipelineShaderStageCreateInfo> stages;

  // Raygen
  vk::RayTracingShaderGroupCreateInfoKHR rg{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  stages.push_back({{}, vk::ShaderStageFlagBits::eRaygenKHR, raygenSM, "main"});
  rg.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(rg);

  // Miss
  vk::RayTracingShaderGroupCreateInfoKHR mg{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, missSM, "main"});
  mg.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(mg);
  // Shadow Miss
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, shadowmissSM, "main"});
  mg.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(mg);

  // Hit Group0 - Closest Hit + AnyHit
  vk::ShaderModule chitSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("shaders/raytrace.rchit.spv", true, paths));
  vk::ShaderModule ahitSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("shaders/raytrace.rahit.spv", true, paths));

  vk::RayTracingShaderGroupCreateInfoKHR hg{vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  stages.push_back({{}, vk::ShaderStageFlagBits::eClosestHitKHR, chitSM, "main"});
  hg.setClosestHitShader(static_cast<uint32_t>(stages.size() - 1));
  stages.push_back({{}, vk::ShaderStageFlagBits::eAnyHitKHR, ahitSM, "main"});
  hg.setAnyHitShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(hg);


  // Hit Group1 - Closest Hit + Intersection (procedural)
  vk::ShaderModule chit2SM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("shaders/raytrace2.rchit.spv", true, paths));
  vk::ShaderModule ahit2SM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("shaders/raytrace2.rahit.spv", true, paths));
  vk::ShaderModule rintSM =
      nvvk::createShaderModule(m_device,  //
                               nvh::loadFile("shaders/raytrace.rint.spv", true, paths));
  {
    vk::RayTracingShaderGroupCreateInfoKHR hg{vk::RayTracingShaderGroupTypeKHR::eProceduralHitGroup,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    stages.push_back({{}, vk::ShaderStageFlagBits::eClosestHitKHR, chit2SM, "main"});
    hg.setClosestHitShader(static_cast<uint32_t>(stages.size() - 1));
    stages.push_back({{}, vk::ShaderStageFlagBits::eAnyHitKHR, ahit2SM, "main"});
    hg.setAnyHitShader(static_cast<uint32_t>(stages.size() - 1));
    stages.push_back({{}, vk::ShaderStageFlagBits::eIntersectionKHR, rintSM, "main"});
    hg.setIntersectionShader(static_cast<uint32_t>(stages.size() - 1));
    m_rtShaderGroups.push_back(hg);
  }

  // Callable shaders
  vk::RayTracingShaderGroupCreateInfoKHR callGroup{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                                   VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                                   VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};

  vk::ShaderModule call0 =
      nvvk::createShaderModule(m_device,
                               nvh::loadFile("shaders/light_point.rcall.spv", true, paths));
  vk::ShaderModule call1 =
      nvvk::createShaderModule(m_device,
                               nvh::loadFile("shaders/light_spot.rcall.spv", true, paths));
  vk::ShaderModule call2 =
      nvvk::createShaderModule(m_device, nvh::loadFile("shaders/light_inf.rcall.spv", true, paths));

  stages.push_back({{}, vk::ShaderStageFlagBits::eCallableKHR, call0, "main"});
  callGroup.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(callGroup);
  stages.push_back({{}, vk::ShaderStageFlagBits::eCallableKHR, call1, "main"});
  callGroup.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(callGroup);
  stages.push_back({{}, vk::ShaderStageFlagBits::eCallableKHR, call2, "main"});
  callGroup.setGeneralShader(static_cast<uint32_t>(stages.size() - 1));
  m_rtShaderGroups.push_back(callGroup);


  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;

  // Push constant: we want to be able to update constants used by the shaders
  vk::PushConstantRange pushConstant{vk::ShaderStageFlagBits::eRaygenKHR
                                         | vk::ShaderStageFlagBits::eClosestHitKHR
                                         | vk::ShaderStageFlagBits::eMissKHR
                                         | vk::ShaderStageFlagBits::eCallableKHR,
                                     0, sizeof(RtPushConstants)};
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstant);

  // Descriptor sets: one specific to ray tracing, and one shared with the rasterization pipeline
  std::vector<vk::DescriptorSetLayout> rtDescSetLayouts = {m_rtDescSetLayout, sceneDescLayout};
  pipelineLayoutCreateInfo.setSetLayoutCount(static_cast<uint32_t>(rtDescSetLayouts.size()));
  pipelineLayoutCreateInfo.setPSetLayouts(rtDescSetLayouts.data());

  m_rtPipelineLayout = m_device.createPipelineLayout(pipelineLayoutCreateInfo);

  // Assemble the shader stages and recursion depth info into the ray tracing pipeline
  vk::RayTracingPipelineCreateInfoKHR rayPipelineInfo;
  rayPipelineInfo.setStageCount(static_cast<uint32_t>(stages.size()));  // Stages are shaders
  rayPipelineInfo.setPStages(stages.data());

  rayPipelineInfo.setGroupCount(static_cast<uint32_t>(
      m_rtShaderGroups.size()));  // 1-raygen, n-miss, n-(hit[+anyhit+intersect])
  rayPipelineInfo.setPGroups(m_rtShaderGroups.data());

  rayPipelineInfo.setMaxRecursionDepth(2);  // Ray depth
  rayPipelineInfo.setLayout(m_rtPipelineLayout);
  m_rtPipeline = m_device.createRayTracingPipelineKHR({}, rayPipelineInfo).value;

  m_device.destroy(raygenSM);
  m_device.destroy(missSM);
  m_device.destroy(shadowmissSM);
  m_device.destroy(chitSM);
  m_device.destroy(ahitSM);
  m_device.destroy(chit2SM);
  m_device.destroy(ahit2SM);
  m_device.destroy(rintSM);
  m_device.destroy(call0);
  m_device.destroy(call1);
  m_device.destroy(call2);
}

//--------------------------------------------------------------------------------------------------
// The Shader Binding Table (SBT)
// - getting all shader handles and writing them in a SBT buffer
// - Besides exception, this could be always done like this
//   See how the SBT buffer is used in run()
//
void Raytracer::createRtShaderBindingTable()
{
  auto groupCount =
      static_cast<uint32_t>(m_rtShaderGroups.size());               // 3 shaders: raygen, miss, chit
  uint32_t groupHandleSize = m_rtProperties.shaderGroupHandleSize;  // Size of a program identifier

  // Fetch all the shader handles used in the pipeline, so that they can be written in the SBT
  uint32_t sbtSize = groupCount * groupHandleSize;

  std::vector<uint8_t> shaderHandleStorage(sbtSize);
  m_device.getRayTracingShaderGroupHandlesKHR(m_rtPipeline, 0, groupCount, sbtSize,
                                              shaderHandleStorage.data());
  // Write the handles in the SBT
  nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
  vk::CommandBuffer cmdBuf = genCmdBuf.createCommandBuffer();

  m_rtSBTBuffer =
      m_alloc->createBuffer(cmdBuf, shaderHandleStorage, vk::BufferUsageFlagBits::eRayTracingKHR);
  m_debug.setObjectName(m_rtSBTBuffer.buffer, "SBT");


  genCmdBuf.submitAndWait(cmdBuf);

  m_alloc->finalizeAndReleaseStaging();
}

//--------------------------------------------------------------------------------------------------
// Ray Tracing the scene
//
void Raytracer::raytrace(const vk::CommandBuffer& cmdBuf,
                         const nvmath::vec4f&     clearColor,
                         vk::DescriptorSet&       sceneDescSet,
                         vk::Extent2D&            size,
                         ObjPushConstants&        sceneConstants)
{
  m_debug.beginLabel(cmdBuf, "Ray trace");
  // Initializing push constant values
  m_rtPushConstants.clearColor           = clearColor;
  m_rtPushConstants.lightPosition        = sceneConstants.lightPosition;
  m_rtPushConstants.lightIntensity       = sceneConstants.lightIntensity;
  m_rtPushConstants.lightDirection       = sceneConstants.lightDirection;
  m_rtPushConstants.lightSpotCutoff      = sceneConstants.lightSpotCutoff;
  m_rtPushConstants.lightSpotOuterCutoff = sceneConstants.lightSpotOuterCutoff;
  m_rtPushConstants.lightType            = sceneConstants.lightType;
  m_rtPushConstants.frame                = sceneConstants.frame;

  cmdBuf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipeline);
  cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipelineLayout, 0,
                            {m_rtDescSet, sceneDescSet}, {});
  cmdBuf.pushConstants<RtPushConstants>(m_rtPipelineLayout,
                                        vk::ShaderStageFlagBits::eRaygenKHR
                                            | vk::ShaderStageFlagBits::eClosestHitKHR
                                            | vk::ShaderStageFlagBits::eMissKHR
                                            | vk::ShaderStageFlagBits::eCallableKHR,
                                        0, m_rtPushConstants);

  vk::DeviceSize progSize = m_rtProperties.shaderGroupHandleSize;  // Size of a program identifier
  vk::DeviceSize rayGenOffset        = 0u * progSize;  // Start at the beginning of m_sbtBuffer
  vk::DeviceSize missOffset          = 1u * progSize;  // Jump over raygen
  vk::DeviceSize hitGroupOffset      = 3u * progSize;  // Jump over the previous shaders
  vk::DeviceSize callableGroupOffset = 5u * progSize;  // Jump over the previous shaders
  vk::DeviceSize sbtSize             = (vk::DeviceSize)m_rtShaderGroups.size() * progSize;

  const vk::StridedBufferRegionKHR raygenShaderBindingTable = {m_rtSBTBuffer.buffer, rayGenOffset,
                                                               progSize, sbtSize};
  const vk::StridedBufferRegionKHR missShaderBindingTable   = {m_rtSBTBuffer.buffer, missOffset,
                                                             progSize, sbtSize};
  const vk::StridedBufferRegionKHR hitShaderBindingTable    = {m_rtSBTBuffer.buffer, hitGroupOffset,
                                                            progSize, sbtSize};
  const vk::StridedBufferRegionKHR callableShaderBindingTable = {
      m_rtSBTBuffer.buffer, callableGroupOffset, progSize, sbtSize};

  cmdBuf.traceRaysKHR(&raygenShaderBindingTable, &missShaderBindingTable, &hitShaderBindingTable,
                      &callableShaderBindingTable,  //
                      size.width, size.height, 1);  //


  m_debug.endLabel(cmdBuf);
}
