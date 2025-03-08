#pragma once

#include "types.h"
#include <glm/glm.hpp>

// Define the Vertex structure if it's not already defined elsewhere
struct Vertex
{
    float x, y, z;       // Position
    u32 col;             // Color
    u32 spc;             // Specular
    float u, v;          // Texture coordinates
    float nx, ny, nz;    // Normal (for Naomi 2)
};

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
// Function declarations for NEON-optimized vertex operations
void TransformVerticesNeon(const Vertex* src, Vertex* dst, int count, const glm::mat4& mvp);
void ProcessVertexColorsNeon(Vertex* vertices, int count, const float colorScale[4]);
#endif
