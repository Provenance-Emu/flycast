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
#pragma once
#include "vulkan.h"

class ShaderCompiler
{
public:
	static void Init();
	static void Term();
	static vk::UniqueShaderModule Compile(vk::ShaderStageFlagBits shaderStage, std::string const& shaderText);

	// Add these declarations
	static void CacheShader(const std::string& key, const std::vector<uint32_t>& spirv);
	static bool LoadCachedShader(const std::string& key, std::vector<uint32_t>& spirv);

private:
	static int initCount;
};
