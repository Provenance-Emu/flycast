/*
    Created on: Nov 6, 2019

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
#include "compiler.h"
#include "vulkan_context.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

// Add these includes for directory operations
#ifdef _WIN32
#include <direct.h>  // For _mkdir on Windows
#else
#include <sys/stat.h>  // For mkdir on Unix-like systems
#endif

int ShaderCompiler::initCount;

void ShaderCompiler::Init()
{
	if (initCount++ == 0) {
		bool rc = glslang::InitializeProcess();
		verify(rc);
	}
}
void ShaderCompiler::Term()
{
	if (--initCount == 0)
		glslang::FinalizeProcess();
	initCount = std::max(initCount, 0);
}

static EShLanguage translateShaderStage(vk::ShaderStageFlagBits stage)
{
	switch (stage)
	{
	case vk::ShaderStageFlagBits::eVertex:                  return EShLangVertex;
	case vk::ShaderStageFlagBits::eTessellationControl:     return EShLangTessControl;
	case vk::ShaderStageFlagBits::eTessellationEvaluation:  return EShLangTessEvaluation;
	case vk::ShaderStageFlagBits::eGeometry:                return EShLangGeometry;
	case vk::ShaderStageFlagBits::eFragment:                return EShLangFragment;
	case vk::ShaderStageFlagBits::eCompute:                 return EShLangCompute;
	case vk::ShaderStageFlagBits::eRaygenNV:                return EShLangRayGenNV;
	case vk::ShaderStageFlagBits::eAnyHitNV:                return EShLangAnyHitNV;
	case vk::ShaderStageFlagBits::eClosestHitNV:            return EShLangClosestHitNV;
	case vk::ShaderStageFlagBits::eMissNV:                  return EShLangMissNV;
	case vk::ShaderStageFlagBits::eIntersectionNV:          return EShLangIntersectNV;
	case vk::ShaderStageFlagBits::eCallableNV:              return EShLangCallableNV;
	case vk::ShaderStageFlagBits::eTaskNV:                  return EShLangTaskNV;
	case vk::ShaderStageFlagBits::eMeshNV:                  return EShLangMeshNV;

	default:
		die("Unknown shader stage");
		return EShLangVertex;
	}
}

static bool GLSLtoSPV(const vk::ShaderStageFlagBits shaderType, std::string const& glslShader, std::vector<unsigned int> &spvShader)
{
	EShLanguage stage = translateShaderStage(shaderType);

	const char *shaderStrings[1];
	shaderStrings[0] = glslShader.data();

	glslang::TShader shader(stage);
	shader.setStrings(shaderStrings, 1);

	// Enable SPIR-V and Vulkan rules when parsing GLSL
	EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

	if (!shader.parse(GetDefaultResources(), 100, false, messages))
	{
		ERROR_LOG(RENDERER, "%s", shader.getInfoLog());
		ERROR_LOG(RENDERER, "%s", shader.getInfoDebugLog());
        return false;  // something didn't work
	}

	glslang::TProgram program;
	program.addShader(&shader);

	//
	// Program-level processing...
	//

	if (!program.link(messages))
	{
		ERROR_LOG(RENDERER, "%s", shader.getInfoLog());
		ERROR_LOG(RENDERER, "%s", shader.getInfoDebugLog());
		return false;
	}

	glslang::GlslangToSpv(*program.getIntermediate(stage), spvShader);
	return true;
}

vk::UniqueShaderModule ShaderCompiler::Compile(vk::ShaderStageFlagBits shaderStage, std::string const& shaderText)
{
	std::vector<unsigned int> shaderSPV;
	bool ok = GLSLtoSPV(shaderStage, shaderText, shaderSPV);
	verify(ok);

	return VulkanContext::Instance()->GetDevice().createShaderModuleUnique
			(vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(), shaderSPV));
}

// Add this helper function
std::string getShaderCachePath(const std::string& filename)
{
    #ifdef __ANDROID__
    return "/sdcard/flycast/shader_cache/" + filename;
    #else
    return "shader_cache/" + filename;
    #endif
}

void ShaderCompiler::CacheShader(const std::string& key, const std::vector<uint32_t>& spirv)
{
    std::string cachePath = getShaderCachePath(key + ".spv");

    // Create directory if it doesn't exist
    std::string cacheDir = getShaderCachePath("");
    #ifdef __ANDROID__
    mkdir(cacheDir.c_str(), 0755);
    #else
    // On other platforms, try to create the directory
    #ifdef _WIN32
    _mkdir(cacheDir.c_str());
    #else
    mkdir(cacheDir.c_str(), 0755);
    #endif
    #endif

    FILE *f = fopen(cachePath.c_str(), "wb");
    if (f != nullptr)
    {
        fwrite(spirv.data(), sizeof(uint32_t), spirv.size(), f);
        fclose(f);
    }
}

bool ShaderCompiler::LoadCachedShader(const std::string& key, std::vector<uint32_t>& spirv)
{
    std::string cachePath = getShaderCachePath(key + ".spv");
    FILE *f = fopen(cachePath.c_str(), "rb");
    if (f == nullptr)
        return false;

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size % sizeof(uint32_t) != 0)
    {
        fclose(f);
        return false;
    }

    spirv.resize(size / sizeof(uint32_t));
    if (fread(spirv.data(), sizeof(uint32_t), spirv.size(), f) != spirv.size())
    {
        fclose(f);
        return false;
    }

    fclose(f);
    return true;
}
