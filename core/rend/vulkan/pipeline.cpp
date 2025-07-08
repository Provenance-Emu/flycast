/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "pipeline.h"
#include "hw/pvr/Renderer_if.h"
#include "oslib/directory.h"
#include "stdclass.h"
#include "rend/osd.h"

// Use a different function to get the writable path
std::string getWritableCachePath(const std::string& filename)
{
	// Try different approaches
	#ifdef __ANDROID__
	return "/sdcard/flycast/" + filename;
	#else
	// Use a simple approach - store in the current directory
	return filename;
	#endif
}

void PipelineManager::CreateModVolPipeline(ModVolMode mode, int cullMode, bool naomi2)
{
	// Vertex input state
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo;

	if (mode == ModVolMode::Final)
	{
		pipelineVertexInputStateCreateInfo = GetMainVertexInputStateCreateInfo(false, naomi2);
		pipelineInputAssemblyStateCreateInfo = vk::PipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
				vk::PrimitiveTopology::eTriangleStrip);
	}
	else
	{
		static const vk::VertexInputBindingDescription vertexBindingDescription(0, sizeof(float) * 3);
		static const vk::VertexInputAttributeDescription vertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, 0);	// pos
		pipelineVertexInputStateCreateInfo = vk::PipelineVertexInputStateCreateInfo(
				vk::PipelineVertexInputStateCreateFlags(),
				vertexBindingDescription,
				vertexInputAttributeDescription);
		pipelineInputAssemblyStateCreateInfo = vk::PipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
				vk::PrimitiveTopology::eTriangleList);
	}

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  cullMode == 3 ? vk::CullModeFlagBits::eBack
			  : cullMode == 2 ? vk::CullModeFlagBits::eFront
			  : vk::CullModeFlagBits::eNone,        // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;

	// Depth and stencil
	vk::StencilOpState stencilOpState;
	switch (mode)
	{
	case ModVolMode::Xor:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eInvert, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 2, 2);
		break;
	case ModVolMode::Or:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 2, 2, 2);
		break;
	case ModVolMode::Inclusion:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eZero, vk::StencilOp::eReplace, vk::StencilOp::eZero, vk::CompareOp::eLessOrEqual, 3, 3, 1);
		break;
	case ModVolMode::Exclusion:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eZero, vk::StencilOp::eKeep, vk::StencilOp::eZero, vk::CompareOp::eEqual, 3, 3, 1);
		break;
	case ModVolMode::Final:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eZero, vk::StencilOp::eZero, vk::StencilOp::eZero, vk::CompareOp::eEqual, 0x81, 3, 0x81);
		break;
	}
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo
	(
	  vk::PipelineDepthStencilStateCreateFlags(), // flags
	  mode == ModVolMode::Xor || mode == ModVolMode::Or, // depthTestEnable
	  false,                                      // depthWriteEnable
	  vk::CompareOp::eGreater,                    // depthCompareOp
	  false,                                      // depthBoundTestEnable
	  true,                                       // stencilTestEnable
	  stencilOpState,                             // front
	  stencilOpState                              // back
	);

	// Color flags and blending
	vk::ColorComponentFlags colorComponentFlags(
			mode != ModVolMode::Final ? (vk::ColorComponentFlagBits)0
					: vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(
		mode == ModVolMode::Final,          // blendEnable
		vk::BlendFactor::eSrcAlpha,         // srcColorBlendFactor
		vk::BlendFactor::eOneMinusSrcAlpha, // dstColorBlendFactor
		vk::BlendOp::eAdd,                  // colorBlendOp
		vk::BlendFactor::eSrcAlpha,         // srcAlphaBlendFactor
		vk::BlendFactor::eOneMinusSrcAlpha, // dstAlphaBlendFactor
		vk::BlendOp::eAdd,                  // alphaBlendOp
		colorComponentFlags                 // colorWriteMask
	);

	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
		vk::PipelineColorBlendStateCreateFlags(),   // flags
		false,                                      // logicOpEnable
		vk::LogicOp::eNoOp,                         // logicOp
		pipelineColorBlendAttachmentState,         // attachments
		{ { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	std::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), dynamicStates);

	ModVolShaderParams shaderParams { naomi2, !settings.platform.isNaomi2() && config::NativeDepthInterpolation };
	vk::ShaderModule vertex_module = shaderManager->GetModVolVertexShader(shaderParams);
	vk::ShaderModule fragment_module = shaderManager->GetModVolShader(!settings.platform.isNaomi2() && config::NativeDepthInterpolation);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main"),
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main"),
	};
	vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo
	(
	  vk::PipelineCreateFlags(),                  // flags
	  stages,                                     // stages
	  &pipelineVertexInputStateCreateInfo,        // pVertexInputState
	  &pipelineInputAssemblyStateCreateInfo,      // pInputAssemblyState
	  nullptr,                                    // pTessellationState
	  &pipelineViewportStateCreateInfo,           // pViewportState
	  &pipelineRasterizationStateCreateInfo,      // pRasterizationState
	  &pipelineMultisampleStateCreateInfo,        // pMultisampleState
	  &pipelineDepthStencilStateCreateInfo,       // pDepthStencilState
	  &pipelineColorBlendStateCreateInfo,         // pColorBlendState
	  &pipelineDynamicStateCreateInfo,            // pDynamicState
	  *pipelineLayout,                            // layout
	  renderPass                                  // renderPass
	);

	modVolPipelines[hash(mode, cullMode, naomi2)] =
			GetContext()->GetDevice().createGraphicsPipelineUnique(GetContext()->GetPipelineCache(),
					graphicsPipelineCreateInfo).value;
}

void PipelineManager::CreateDepthPassPipeline(int cullMode, bool naomi2)
{
	// Vertex input state
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetMainVertexInputStateCreateInfo(false, false);
	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo
	(
			vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleList
	);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  cullMode == 3 ? vk::CullModeFlagBits::eBack
			  : cullMode == 2 ? vk::CullModeFlagBits::eFront
			  : vk::CullModeFlagBits::eNone,        // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);

	// Dreamcast uses the last vertex as the provoking vertex, but Vulkan uses the first.
	// Utilize VK_EXT_provoking_vertex when available to set the provoking vertex to be the
	// last vertex
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provokingVertexInfo{};
	if (GetContext()->hasProvokingVertex())
	{
		provokingVertexInfo.provokingVertexMode = vk::ProvokingVertexModeEXT::eLastVertex;
		pipelineRasterizationStateCreateInfo.pNext = &provokingVertexInfo;
	}

	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;

	// Depth and stencil
	vk::StencilOpState stencilOpState;
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo
	(
	  vk::PipelineDepthStencilStateCreateFlags(), // flags
	  true,                                       // depthTestEnable
	  true,                                       // depthWriteEnable
	  vk::CompareOp::eGreaterOrEqual,             // depthCompareOp
	  false,                                      // depthBoundTestEnable
	  false,                                      // stencilTestEnable
	  stencilOpState,                             // front
	  stencilOpState                              // back
	);

	// Color flags and blending
	vk::ColorComponentFlags colorComponentFlags((vk::ColorComponentFlagBits)0);
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(
		false,                              // blendEnable
		vk::BlendFactor::eZero,             // srcColorBlendFactor
		vk::BlendFactor::eZero,             // dstColorBlendFactor
		vk::BlendOp::eAdd,                  // colorBlendOp
		vk::BlendFactor::eZero,             // srcAlphaBlendFactor
		vk::BlendFactor::eZero,             // dstAlphaBlendFactor
		vk::BlendOp::eAdd,                  // alphaBlendOp
		colorComponentFlags                 // colorWriteMask
	);

	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
		vk::PipelineColorBlendStateCreateFlags(),   // flags
		false,                                      // logicOpEnable
		vk::LogicOp::eNoOp,                         // logicOp
		pipelineColorBlendAttachmentState,         // attachments
		{ { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	std::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), dynamicStates);

	ModVolShaderParams shaderParams { naomi2, !settings.platform.isNaomi2() && config::NativeDepthInterpolation };
	vk::ShaderModule vertex_module = shaderManager->GetModVolVertexShader(shaderParams);
	vk::ShaderModule fragment_module = shaderManager->GetModVolShader(!settings.platform.isNaomi2() && config::NativeDepthInterpolation);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main"),
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main"),
	};
	vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo
	(
	  vk::PipelineCreateFlags(),                  // flags
	  stages,                                     // stages
	  &pipelineVertexInputStateCreateInfo,        // pVertexInputState
	  &pipelineInputAssemblyStateCreateInfo,      // pInputAssemblyState
	  nullptr,                                    // pTessellationState
	  &pipelineViewportStateCreateInfo,           // pViewportState
	  &pipelineRasterizationStateCreateInfo,      // pRasterizationState
	  &pipelineMultisampleStateCreateInfo,        // pMultisampleState
	  &pipelineDepthStencilStateCreateInfo,       // pDepthStencilState
	  &pipelineColorBlendStateCreateInfo,         // pColorBlendState
	  &pipelineDynamicStateCreateInfo,            // pDynamicState
	  *pipelineLayout,                            // layout
	  renderPass                                  // renderPass
	);

	depthPassPipelines[hash(cullMode, naomi2)] =
			GetContext()->GetDevice().createGraphicsPipelineUnique(GetContext()->GetPipelineCache(),
					graphicsPipelineCreateInfo).value;
}

void PipelineManager::CreatePipeline(u32 listType, bool sortTriangles, const PolyParam& pp, int gpuPalette, bool dithering)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetMainVertexInputStateCreateInfo(true, pp.isNaomi2());

	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo;
	if (sortTriangles && !config::PerStripSorting) {
		pipelineInputAssemblyStateCreateInfo.topology = vk::PrimitiveTopology::eTriangleList;
	}
	else
	{
		pipelineInputAssemblyStateCreateInfo.topology = vk::PrimitiveTopology::eTriangleStrip;
		pipelineInputAssemblyStateCreateInfo.primitiveRestartEnable = true;
	}

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  pp.isp.CullMode == 3 ? vk::CullModeFlagBits::eBack
			  : pp.isp.CullMode == 2 ? vk::CullModeFlagBits::eFront
			  : vk::CullModeFlagBits::eNone,        // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);

	// Dreamcast uses the last vertex as the provoking vertex, but Vulkan uses the first.
	// Utilize VK_EXT_provoking_vertex when available to set the provoking vertex to be the
	// last vertex
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provokingVertexInfo{};
	if (GetContext()->hasProvokingVertex())
	{
		provokingVertexInfo.provokingVertexMode = vk::ProvokingVertexModeEXT::eLastVertex;
		pipelineRasterizationStateCreateInfo.pNext = &provokingVertexInfo;
	}

	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;

	// Depth and stencil
	vk::CompareOp depthOp;
	if (listType == ListType_Punch_Through || sortTriangles)
		depthOp = vk::CompareOp::eGreaterOrEqual;
	else
		depthOp = depthOps[pp.isp.DepthMode];
	bool depthWriteEnable;
	if (sortTriangles /* && !config::PerStripSorting */)
		// FIXME temporary work-around for intel driver bug
		depthWriteEnable = GetContext()->GetVendorID() == VulkanContext::VENDOR_INTEL;
	else
	{
		// Z Write Disable seems to be ignored for punch-through.
		// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
		if (listType == ListType_Punch_Through)
			depthWriteEnable = true;
		else
			depthWriteEnable = !pp.isp.ZWriteDis;
	}

	bool shadowed = listType == ListType_Opaque || listType == ListType_Punch_Through;
	vk::StencilOpState stencilOpState;
	if (shadowed)
	{
		if (pp.pcw.Shadow != 0)
			stencilOpState = vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 0x80, 0x80);
		else
			stencilOpState = vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 0x80, 0);
	}
	else
		stencilOpState = vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::CompareOp::eNever);
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo
	(
	  vk::PipelineDepthStencilStateCreateFlags(), // flags
	  true,                                       // depthTestEnable
	  depthWriteEnable,                           // depthWriteEnable
	  depthOp,                                    // depthCompareOp
	  false,                                      // depthBoundTestEnable
	  shadowed,                                   // stencilTestEnable
	  stencilOpState,                             // front
	  stencilOpState                              // back
	);

	// Color flags and blending
	vk::ColorComponentFlags colorComponentFlags(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	// Apparently punch-through polys support blending, or at least some combinations
	// Opaque polygons support blending in list continuations (wild guess)
	u32 src = pp.tsp.SrcInstr;
	u32 dst = pp.tsp.DstInstr;
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState
	{
	  true,                          // blendEnable
	  getBlendFactor(src, true),     // srcColorBlendFactor
	  getBlendFactor(dst, false),    // dstColorBlendFactor
	  vk::BlendOp::eAdd,             // colorBlendOp
	  getBlendFactor(src, true),     // srcAlphaBlendFactor
	  getBlendFactor(dst, false),    // dstAlphaBlendFactor
	  vk::BlendOp::eAdd,             // alphaBlendOp
	  colorComponentFlags            // colorWriteMask
	};

	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
	  vk::PipelineColorBlendStateCreateFlags(),   // flags
	  false,                                      // logicOpEnable
	  vk::LogicOp::eNoOp,                         // logicOp
	  pipelineColorBlendAttachmentState,         // attachments
	  { { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	vk::DynamicState dynamicStates[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), 2, dynamicStates);

	bool divPosZ = !settings.platform.isNaomi2() && config::NativeDepthInterpolation;
	vk::ShaderModule vertex_module = shaderManager->GetVertexShader(VertexShaderParams { pp.pcw.Gouraud == 1, pp.isNaomi2(), divPosZ });
	FragmentShaderParams params = {};
	params.alphaTest = listType == ListType_Punch_Through;
	params.bumpmap = pp.tcw.PixelFmt == PixelBumpMap;
	params.clamping = pp.tsp.ColorClamp;
	params.insideClipTest = (pp.tileclip >> 28) == 3;
	params.fog = config::Fog ? pp.tsp.FogCtrl : 2;
	params.gouraud = pp.pcw.Gouraud;
	params.ignoreTexAlpha = pp.tsp.IgnoreTexA || pp.tcw.PixelFmt == Pixel565;
	params.offset = pp.pcw.Offset;
	params.shaderInstr = pp.tsp.ShadInstr;
	params.texture = pp.pcw.Texture;
	params.trilinear = pp.pcw.Texture && pp.tsp.FilterMode > 1 && listType != ListType_Punch_Through && pp.tcw.MipMapped == 1;
	params.useAlpha = pp.tsp.UseAlpha;
	params.palette = gpuPalette;
	params.divPosZ = divPosZ;
	params.dithering = dithering;
	vk::ShaderModule fragment_module = shaderManager->GetFragmentShader(params);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main"),
			vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main"),
	};
	vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo
	(
	  vk::PipelineCreateFlags(),                  // flags
	  stages,                                     // stages
	  &pipelineVertexInputStateCreateInfo,        // pVertexInputState
	  &pipelineInputAssemblyStateCreateInfo,      // pInputAssemblyState
	  nullptr,                                    // pTessellationState
	  &pipelineViewportStateCreateInfo,           // pViewportState
	  &pipelineRasterizationStateCreateInfo,      // pRasterizationState
	  &pipelineMultisampleStateCreateInfo,        // pMultisampleState
	  &pipelineDepthStencilStateCreateInfo,       // pDepthStencilState
	  &pipelineColorBlendStateCreateInfo,         // pColorBlendState
	  &pipelineDynamicStateCreateInfo,            // pDynamicState
	  *pipelineLayout,                            // layout
	  renderPass                                  // renderPass
	);

	pipelines[hash(listType, sortTriangles, &pp, gpuPalette, dithering)] = GetContext()->GetDevice().createGraphicsPipelineUnique(GetContext()->GetPipelineCache(),
			graphicsPipelineCreateInfo).value;
}

void PipelineManager::SavePipelineCache()
{
	VulkanContext *context = GetContext();
	vk::Device device = context->GetDevice();

	try {
		std::vector<uint8_t> cacheData = device.getPipelineCacheData(context->GetPipelineCache());
		if (!cacheData.empty())
		{
			std::string cachePath = getWritableCachePath("vulkan_pipeline_cache.bin");
			FILE *f = fopen(cachePath.c_str(), "wb");
			if (f != nullptr)
			{
				fwrite(cacheData.data(), 1, cacheData.size(), f);
				fclose(f);
				INFO_LOG(RENDERER, "Saved Vulkan pipeline cache: %zu bytes", cacheData.size());
			}
		}
	}
	catch (const vk::SystemError& e) {
		WARN_LOG(RENDERER, "Failed to save pipeline cache: %s", e.what());
	}
}

void PipelineManager::LoadPipelineCache()
{
	std::string cachePath = getWritableCachePath("vulkan_pipeline_cache.bin");
	FILE *f = fopen(cachePath.c_str(), "rb");
	if (f == nullptr)
		return;

	fseek(f, 0, SEEK_END);
	size_t cacheSize = ftell(f);
	fseek(f, 0, SEEK_SET);

	std::vector<uint8_t> cacheData(cacheSize);
	if (fread(cacheData.data(), 1, cacheSize, f) == cacheSize)
	{
		try {
			// Create a new pipeline cache and use it
			vk::PipelineCacheCreateInfo createInfo(vk::PipelineCacheCreateFlags(), cacheSize, cacheData.data());
			vk::Device device = GetContext()->GetDevice();
			vk::PipelineCache newCache = device.createPipelineCache(createInfo);

			// Log success
			INFO_LOG(RENDERER, "Loaded Vulkan pipeline cache: %zu bytes", cacheSize);

			// TODO: Replace the existing pipeline cache with this one
			// For now, we'll just destroy it since we can't replace it
			device.destroyPipelineCache(newCache);
		}
		catch (const vk::SystemError& e) {
			WARN_LOG(RENDERER, "Failed to load pipeline cache: %s", e.what());
		}
	}
	fclose(f);
}

void OSDPipeline::CreatePipeline()
{
	// Vertex input state for OSD quad rendering
	static const vk::VertexInputBindingDescription vertexBindingDescription(0, sizeof(OSDVertex));
	static const vk::VertexInputAttributeDescription vertexInputAttributeDescriptions[] = {
		vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32Sfloat, offsetof(OSDVertex, x)),	// position (vec2 -> vec4)
		vk::VertexInputAttributeDescription(1, 0, vk::Format::eR8G8B8A8Unorm, offsetof(OSDVertex, r)),	// color (vec4)
		vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(OSDVertex, u)),	// texture coords (vec2)
	};
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo(
		vk::PipelineVertexInputStateCreateFlags(),
		vertexBindingDescription,
		vertexInputAttributeDescriptions
	);

	// Input assembly state - triangle strip for quads
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(
		vk::PipelineInputAssemblyStateCreateFlags(),
		vk::PrimitiveTopology::eTriangleStrip
	);

	// Viewport and scissor states (dynamic)
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(
		vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr
	);

	// Rasterization state
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo(
		vk::PipelineRasterizationStateCreateFlags(),
		false,                                        // depthClampEnable
		false,                                        // rasterizerDiscardEnable
		vk::PolygonMode::eFill,                       // polygonMode
		vk::CullModeFlagBits::eNone,                  // cullMode
		vk::FrontFace::eCounterClockwise,             // frontFace
		false,                                        // depthBiasEnable
		0.0f,                                         // depthBiasConstantFactor
		0.0f,                                         // depthBiasClamp
		0.0f,                                         // depthBiasSlopeFactor
		1.0f                                          // lineWidth
	);

	// Multisample state
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;

	// Depth and stencil state - no depth testing for OSD
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo(
		vk::PipelineDepthStencilStateCreateFlags(),
		false,                                        // depthTestEnable
		false,                                        // depthWriteEnable
		vk::CompareOp::eAlways,                       // depthCompareOp
		false,                                        // depthBoundsTestEnable
		false,                                        // stencilTestEnable
		{},                                           // front
		{}                                            // back
	);

	// Color blending - alpha blending for OSD
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(
		true,                                         // blendEnable
		vk::BlendFactor::eSrcAlpha,                   // srcColorBlendFactor
		vk::BlendFactor::eOneMinusSrcAlpha,           // dstColorBlendFactor
		vk::BlendOp::eAdd,                            // colorBlendOp
		vk::BlendFactor::eSrcAlpha,                   // srcAlphaBlendFactor
		vk::BlendFactor::eOneMinusSrcAlpha,           // dstAlphaBlendFactor
		vk::BlendOp::eAdd,                            // alphaBlendOp
		vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | 
		vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA  // colorWriteMask
	);

	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo(
		vk::PipelineColorBlendStateCreateFlags(),
		false,                                        // logicOpEnable
		vk::LogicOp::eNoOp,                           // logicOp
		pipelineColorBlendAttachmentState,           // attachments
		{ { 1.0f, 1.0f, 1.0f, 1.0f } }                // blendConstants
	);

	// Dynamic state
	std::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(
		vk::PipelineDynamicStateCreateFlags(), dynamicStates
	);

	// Shaders
	vk::ShaderModule vertex_module = shaderManager->GetOSDVertexShader();
	vk::ShaderModule fragment_module = shaderManager->GetOSDFragmentShader();

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
		vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main"),
		vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main"),
	};

	// Create the graphics pipeline
	vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo(
		vk::PipelineCreateFlags(),                    // flags
		stages,                                       // stages
		&pipelineVertexInputStateCreateInfo,          // pVertexInputState
		&pipelineInputAssemblyStateCreateInfo,        // pInputAssemblyState
		nullptr,                                      // pTessellationState
		&pipelineViewportStateCreateInfo,             // pViewportState
		&pipelineRasterizationStateCreateInfo,        // pRasterizationState
		&pipelineMultisampleStateCreateInfo,          // pMultisampleState
		&pipelineDepthStencilStateCreateInfo,         // pDepthStencilState
		&pipelineColorBlendStateCreateInfo,           // pColorBlendState
		&pipelineDynamicStateCreateInfo,              // pDynamicState
		*pipelineLayout,                              // layout
		renderPass                                    // renderPass
	);

	pipeline = GetContext()->GetDevice().createGraphicsPipelineUnique(
		GetContext()->GetPipelineCache(), graphicsPipelineCreateInfo).value;
}
