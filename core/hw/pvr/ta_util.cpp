/*
	Copyright 2022 flyinghead

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
#include "ta_ctx.h"
#include "pvr_mem.h"
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

/// iOS ARM64 NEON optimizations for PVR processing
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
#include <arm_neon.h>
#include <arm_acle.h>

/// iOS-specific SIMD optimizations for maximum FMV performance
struct IOSPVRSIMDOptimizations {
	/// Performance metrics
	uint64_t simd_operations = 0;
	uint64_t vertex_transforms = 0;
	uint64_t triangle_sorts = 0;
	uint64_t matrix_ops = 0;
	
	/// Log performance improvements
	void logSIMDMetrics() {
		if ((simd_operations % 10000) == 0 && simd_operations > 0) {
			INFO_LOG(PVR, "🚀 iOS SIMD PVR: Operations=%llu, Vertices=%llu, Triangles=%llu, Matrices=%llu",
			         simd_operations, vertex_transforms, triangle_sorts, matrix_ops);
		}
	}
};

static IOSPVRSIMDOptimizations g_ios_simd_opts;

/// iOS ARM64 NEON-optimized vertex infinity check with SIMD
/// Processes 4 vertices simultaneously for maximum throughput
static bool is_vertex_inf_neon_batch(const Vertex* vtx, int count) {
	g_ios_simd_opts.vertex_transforms += count;
	g_ios_simd_opts.simd_operations++;
	
	/// Process vertices in batches of 4 for optimal NEON performance
	int batches = count / 4;
	for (int batch = 0; batch < batches; batch++) {
		const Vertex* v = &vtx[batch * 4];
		
		/// Load 4 vertices worth of X coordinates
		float32x4_t x_vals = {v[0].x, v[1].x, v[2].x, v[3].x};
		float32x4_t y_vals = {v[0].y, v[1].y, v[2].y, v[3].y};
		float32x4_t z_vals = {v[0].z, v[1].z, v[2].z, v[3].z};
		
		/// Check for NaN using NEON
		uint32x4_t x_nan = vmvnq_u32(vceqq_f32(x_vals, x_vals));
		uint32x4_t y_nan = vmvnq_u32(vceqq_f32(y_vals, y_vals));
		uint32x4_t z_nan = vmvnq_u32(vceqq_f32(z_vals, z_vals));
		
		/// Check for huge values using NEON
		float32x4_t max_xy = vdupq_n_f32(1e25f);
		float32x4_t max_z = vdupq_n_f32(3.4e37f);
		
		uint32x4_t x_huge = vcgtq_f32(vabsq_f32(x_vals), max_xy);
		uint32x4_t y_huge = vcgtq_f32(vabsq_f32(y_vals), max_xy);
		uint32x4_t z_huge = vcgtq_f32(z_vals, max_z);
		
		/// Combine all conditions
		uint32x4_t invalid = vorrq_u32(vorrq_u32(x_nan, y_nan), 
		                              vorrq_u32(vorrq_u32(z_nan, x_huge),
		                                        vorrq_u32(y_huge, z_huge)));
		
		/// Check if any vertex is invalid
		if (vmaxvq_u32(invalid) != 0) {
			return true;
		}
	}
	
	/// Handle remaining vertices
	for (int i = batches * 4; i < count; i++) {
		if (std::isnan(vtx[i].x) || fabsf(vtx[i].x) > 1e25f ||
		    std::isnan(vtx[i].y) || fabsf(vtx[i].y) > 1e25f ||
		    std::isnan(vtx[i].z) || vtx[i].z > 3.4e37f) {
			return true;
		}
	}
	
	g_ios_simd_opts.logSIMDMetrics();
	return false;
}

/// iOS ARM64 NEON-optimized Z-depth calculation with SIMD
/// Processes multiple vertices simultaneously for triangle sorting
static void calculate_min_z_neon_batch(const Vertex* vertices, const u32* indices, int triangle_count, float* z_values) {
	g_ios_simd_opts.triangle_sorts += triangle_count;
	g_ios_simd_opts.simd_operations++;
	
	/// Process triangles in batches of 4 for optimal NEON performance
	int batches = triangle_count / 4;
	for (int batch = 0; batch < batches; batch++) {
		int base_idx = batch * 4;
		
		/// Load Z values for 4 triangles (12 vertices)
		float32x4_t z0 = {vertices[indices[base_idx * 3 + 0]].z,
		                  vertices[indices[(base_idx + 1) * 3 + 0]].z,
		                  vertices[indices[(base_idx + 2) * 3 + 0]].z,
		                  vertices[indices[(base_idx + 3) * 3 + 0]].z};
		
		float32x4_t z1 = {vertices[indices[base_idx * 3 + 1]].z,
		                  vertices[indices[(base_idx + 1) * 3 + 1]].z,
		                  vertices[indices[(base_idx + 2) * 3 + 1]].z,
		                  vertices[indices[(base_idx + 3) * 3 + 1]].z};
		
		float32x4_t z2 = {vertices[indices[base_idx * 3 + 2]].z,
		                  vertices[indices[(base_idx + 1) * 3 + 2]].z,
		                  vertices[indices[(base_idx + 2) * 3 + 2]].z,
		                  vertices[indices[(base_idx + 3) * 3 + 2]].z};
		
		/// Calculate minimum Z for each triangle using NEON
		float32x4_t min_z = vminq_f32(vminq_f32(z0, z1), z2);
		
		/// Store results
		vst1q_f32(&z_values[base_idx], min_z);
	}
	
	/// Handle remaining triangles
	for (int i = batches * 4; i < triangle_count; i++) {
		int base_idx = i * 3;
		float z0 = vertices[indices[base_idx + 0]].z;
		float z1 = vertices[indices[base_idx + 1]].z;
		float z2 = vertices[indices[base_idx + 2]].z;
		z_values[i] = std::min(std::min(z0, z1), z2);
	}
	
	g_ios_simd_opts.logSIMDMetrics();
}

/// iOS ARM64 NEON-optimized matrix operations for Naomi2
/// Processes matrix-vector multiplications with SIMD for maximum performance
static float calculate_projected_z_neon(const Vertex* v, const float* mat) {
	g_ios_simd_opts.matrix_ops++;
	g_ios_simd_opts.simd_operations++;
	
	/// Load vertex position
	float32x4_t vertex = {v->x, v->y, v->z, 1.0f};
	
	/// Load matrix row 2 (Z transformation)
	float32x4_t mat_row2 = vld1q_f32(&mat[8]); // mat[2], mat[6], mat[10], mat[14]
	
	/// Perform dot product using NEON
	float32x4_t products = vmulq_f32(vertex, mat_row2);
	
	/// Sum all components
	float32x2_t sum_pairs = vadd_f32(vget_low_f32(products), vget_high_f32(products));
	float sum = vget_lane_f32(vpadd_f32(sum_pairs, sum_pairs), 0);
	
	g_ios_simd_opts.logSIMDMetrics();
	return -1.0f / sum;
}

/// iOS ARM64 NEON-optimized polygon bounding box calculation
/// Processes vertex bounds with SIMD for sorting optimization
static void calculate_poly_bounds_neon(const Vertex* vertices, u32 count, glm::vec3& min_bounds, glm::vec3& max_bounds) {
	g_ios_simd_opts.vertex_transforms += count;
	g_ios_simd_opts.simd_operations++;
	
	if (count == 0) return;
	
	/// Initialize bounds with first vertex
	float32x4_t min_vals = {vertices[0].x, vertices[0].y, vertices[0].z, 0};
	float32x4_t max_vals = min_vals;
	
	/// Process vertices in batches of 4
	u32 batches = count / 4;
	for (u32 batch = 0; batch < batches; batch++) {
		u32 base_idx = batch * 4;
		
		/// Load 4 vertices
		float32x4_t x_vals = {vertices[base_idx + 0].x, vertices[base_idx + 1].x,
		                      vertices[base_idx + 2].x, vertices[base_idx + 3].x};
		float32x4_t y_vals = {vertices[base_idx + 0].y, vertices[base_idx + 1].y,
		                      vertices[base_idx + 2].y, vertices[base_idx + 3].y};
		float32x4_t z_vals = {vertices[base_idx + 0].z, vertices[base_idx + 1].z,
		                      vertices[base_idx + 2].z, vertices[base_idx + 3].z};
		
		/// Update bounds using compatible NEON intrinsics
		float32x2_t x_min_pair = vpmin_f32(vget_low_f32(x_vals), vget_high_f32(x_vals));
		float32x2_t y_min_pair = vpmin_f32(vget_low_f32(y_vals), vget_high_f32(y_vals));
		float32x2_t z_min_pair = vpmin_f32(vget_low_f32(z_vals), vget_high_f32(z_vals));
		
		float x_min = vget_lane_f32(vpmin_f32(x_min_pair, x_min_pair), 0);
		float y_min = vget_lane_f32(vpmin_f32(y_min_pair, y_min_pair), 0);
		float z_min = vget_lane_f32(vpmin_f32(z_min_pair, z_min_pair), 0);
		
		float32x2_t x_max_pair = vpmax_f32(vget_low_f32(x_vals), vget_high_f32(x_vals));
		float32x2_t y_max_pair = vpmax_f32(vget_low_f32(y_vals), vget_high_f32(y_vals));
		float32x2_t z_max_pair = vpmax_f32(vget_low_f32(z_vals), vget_high_f32(z_vals));
		
		float x_max = vget_lane_f32(vpmax_f32(x_max_pair, x_max_pair), 0);
		float y_max = vget_lane_f32(vpmax_f32(y_max_pair, y_max_pair), 0);
		float z_max = vget_lane_f32(vpmax_f32(z_max_pair, z_max_pair), 0);
		
		min_vals = vsetq_lane_f32(std::min(vgetq_lane_f32(min_vals, 0), x_min), min_vals, 0);
		min_vals = vsetq_lane_f32(std::min(vgetq_lane_f32(min_vals, 1), y_min), min_vals, 1);
		min_vals = vsetq_lane_f32(std::min(vgetq_lane_f32(min_vals, 2), z_min), min_vals, 2);
		
		max_vals = vsetq_lane_f32(std::max(vgetq_lane_f32(max_vals, 0), x_max), max_vals, 0);
		max_vals = vsetq_lane_f32(std::max(vgetq_lane_f32(max_vals, 1), y_max), max_vals, 1);
		max_vals = vsetq_lane_f32(std::max(vgetq_lane_f32(max_vals, 2), z_max), max_vals, 2);
	}
	
	/// Handle remaining vertices
	for (u32 i = batches * 4; i < count; i++) {
		min_vals = vsetq_lane_f32(std::min(vgetq_lane_f32(min_vals, 0), vertices[i].x), min_vals, 0);
		min_vals = vsetq_lane_f32(std::min(vgetq_lane_f32(min_vals, 1), vertices[i].y), min_vals, 1);
		min_vals = vsetq_lane_f32(std::min(vgetq_lane_f32(min_vals, 2), vertices[i].z), min_vals, 2);
		
		max_vals = vsetq_lane_f32(std::max(vgetq_lane_f32(max_vals, 0), vertices[i].x), max_vals, 0);
		max_vals = vsetq_lane_f32(std::max(vgetq_lane_f32(max_vals, 1), vertices[i].y), max_vals, 1);
		max_vals = vsetq_lane_f32(std::max(vgetq_lane_f32(max_vals, 2), vertices[i].z), max_vals, 2);
	}
	
	/// Extract results
	min_bounds.x = vgetq_lane_f32(min_vals, 0);
	min_bounds.y = vgetq_lane_f32(min_vals, 1);
	min_bounds.z = vgetq_lane_f32(min_vals, 2);
	
	max_bounds.x = vgetq_lane_f32(max_vals, 0);
	max_bounds.y = vgetq_lane_f32(max_vals, 1);
	max_bounds.z = vgetq_lane_f32(max_vals, 2);
	
	g_ios_simd_opts.logSIMDMetrics();
}

#endif

//
// Check if a vertex has NaN or huge x,y,z values
//
static bool is_vertex_inf(const Vertex& vtx)
{
	// manic panic ghosts needs 1.0e25f for x and y
	return std::isnan(vtx.x) || fabsf(vtx.x) > 1e25f
			|| std::isnan(vtx.y) || fabsf(vtx.y) > 1e25f
			|| std::isnan(vtx.z) || vtx.z > 3.4e37f;
}

struct IndexTrig
{
	IndexTrig() = default;
	IndexTrig(u32 pid, u32 v0, u32 v1, u32 v2) : pid(pid), z(0) {
		vid[0] = v0;
		vid[1] = v1;
		vid[2] = v2;
	}

	u32 vid[3];
	u32 pid;
	f32 z;
};

static float minZ(const Vertex *v, const u32 *mod)
{
	return std::min(std::min(v[mod[0]].z, v[mod[1]].z), v[mod[2]].z);
}

static bool operator<(const IndexTrig& left, const IndexTrig& right)
{
	return left.z < right.z;
}

static float getProjectedZ(const Vertex *v, const float *mat)
{
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
	/// Use iOS ARM64 NEON optimization for matrix operations
	return calculate_projected_z_neon(v, mat);
#else
	// -1 / z
	return -1 / (mat[2] * v->x + mat[1 * 4 + 2] * v->y + mat[2 * 4 + 2] * v->z + mat[3 * 4 + 2]);
#endif
}

void sortTriangles(rend_context& ctx, RenderPass& pass, const RenderPass& previousPass)
{
	int first = previousPass.tr_count;
	int count = pass.tr_count - first;
	if (count == 0)
		return;

	const PolyParam * const pp_base = &ctx.global_param_tr[first];
	const PolyParam * const pp_end = pp_base + count;

	//make lists of all triangles, with their pid and vid
	static std::vector<IndexTrig> triangleList;

	int vtx_count = ctx.verts.size() - pp_base->first;
	triangleList.reserve(vtx_count);
	triangleList.clear();

	for (const PolyParam *pp = pp_base; pp != pp_end; pp++)
	{
		if (pp->count < 3)
			continue;

		const Vertex *v0 = &ctx.verts[pp->first];
		const Vertex *v1 = &ctx.verts[pp->first + 1];
		float z0 = 0, z1 = 0;

		if (pp->isNaomi2())
		{
			z0 = getProjectedZ(v0, ctx.matrices[pp->mvMatrix].mat);
			z1 = getProjectedZ(v1, ctx.matrices[pp->mvMatrix].mat);
		}
		else
		{
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
			/// Use iOS ARM64 NEON batch processing for vertex validation
			if (pp->count >= 4 && is_vertex_inf_neon_batch(&ctx.verts[pp->first], pp->count)) {
				// Skip this polygon if any vertices are invalid
				continue;
			}
#endif
			if (is_vertex_inf(*v0))
				v0 = nullptr;
			if (is_vertex_inf(*v1))
				v1 = nullptr;
		}
		for (u32 i = 2; i < pp->count; i++)
		{
			const Vertex *v2 = &ctx.verts[pp->first + i];
			if (!pp->isNaomi2() && is_vertex_inf(*v2))
				v2 = nullptr;
			if (v0 != nullptr && v1 != nullptr && v2 != nullptr)
			{
				triangleList.emplace_back((u32)(pp - pp_base),
						(u32)(v0 - &ctx.verts[0]), (u32)(v1 - &ctx.verts[0]), (u32)(v2 - &ctx.verts[0]));
				if (pp->isNaomi2())
				{
					float z2 = getProjectedZ(v2, ctx.matrices[pp->mvMatrix].mat);
					triangleList.back().z = std::min(z0, std::min(z1, z2));
					z0 = z1;
					z1 = z2;
				}
				else
				{
					triangleList.back().z = minZ(&ctx.verts[0], triangleList.back().vid);
				}
			}
			if (i & 1)
				v1 = v2;
			else
				v0 = v2;
		}
	}

	//sort them
	std::stable_sort(triangleList.begin(), triangleList.end());

	//Merge pids/draw cmds if two different pids are actually equal
	for (size_t k = 1; k < triangleList.size(); k++)
		if (triangleList[k].pid != triangleList[k - 1].pid)
		{
			const PolyParam& curPoly = pp_base[triangleList[k].pid];
			const PolyParam& prevPoly = pp_base[triangleList[k - 1].pid];
			if (curPoly.equivalentIgnoreCullingDirection(prevPoly)
					&& (curPoly.isp.CullMode < 2 || curPoly.isp.CullMode == prevPoly.isp.CullMode))
				triangleList[k].pid = triangleList[k - 1].pid;
		}

	//re-assemble them into drawing commands

	int idx = -1;
	int idxSize = ctx.idx.size();

	for (size_t i = 0; i < triangleList.size(); i++)
	{
		int pid = triangleList[i].pid;
		u32* midx = triangleList[i].vid;

		ctx.idx.emplace_back(midx[0]);
		ctx.idx.emplace_back(midx[1]);
		ctx.idx.emplace_back(midx[2]);

		if (idx != pid)
		{
			SortedTriangle cur = { (u32)(&pp_base[pid] - &ctx.global_param_tr[0]), (u32)(idxSize + i * 3), 0 };

			if (idx != -1)
			{
				SortedTriangle& last = ctx.sortedTriangles.back();
				last.count = cur.first - last.first;
			}

			ctx.sortedTriangles.push_back(cur);
			idx = pid;
		}
	}

	if (!triangleList.empty())
	{
		SortedTriangle& last = ctx.sortedTriangles.back();
		last.count = idxSize + triangleList.size() * 3 - last.first;
	}
	else
	{
		// Add a dummy one to signal we're using sorted triangles
		ctx.sortedTriangles.push_back({ (u32)(&pp_base[0] - &ctx.global_param_tr[0]), 0, 0});
	}
	pass.sorted_tr_count = ctx.sortedTriangles.size();

#if PRINT_SORT_STATS
	printf("Reassembled into %d from %d\n", (int)ctx.sortedTriangles.size(), pp_end - pp_base);
#endif
}

static bool operator<(const PolyParam& left, const PolyParam& right)
{
	return left.zvZ < right.zvZ;
}

void sortPolyParams(std::vector<PolyParam>& polys, int first, int end, rend_context& ctx)
{
	if (end - first <= 1)
		return;

	PolyParam * const pp_end = polys.data() + end;

	for (PolyParam *pp = &polys[first]; pp != pp_end; pp++)
	{
		if (pp->count < 3)
		{
			pp->zvZ = 0;
		}
		else
		{
			Vertex *vtx = &ctx.verts[pp->first];
			Vertex *vtx_end = vtx + pp->count;

			if (pp->isNaomi2())
			{
				glm::mat4 mvMat = glm::make_mat4(ctx.matrices[pp->mvMatrix].mat);
				glm::vec3 min{ 1e38f, 1e38f, 1e38f };
				glm::vec3 max{ -1e38f, -1e38f, -1e38f };
				
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
				/// Use iOS ARM64 NEON optimization for bounding box calculation
				calculate_poly_bounds_neon(vtx, pp->count, min, max);
#else
				while (vtx != vtx_end)
				{
					glm::vec3 pos{ vtx->x, vtx->y, vtx->z };
					min = glm::min(min, pos);
					max = glm::max(max, pos);
					vtx++;
				}
#endif
				glm::vec4 center((min + max) / 2.f, 1);
				glm::vec4 extents(max - glm::vec3(center), 0);
				// transform
				center = mvMat * center;
				glm::vec3 extentX = mvMat * glm::vec4(extents.x, 0, 0, 0);
				glm::vec3 extentY = mvMat * glm::vec4(0, extents.y, 0, 0);
				glm::vec3 extentZ = mvMat * glm::vec4(0, 0, extents.z, 0);
				// new AA extents
				glm::vec3 newExtent = glm::abs(extentX) + glm::abs(extentY) + glm::abs(extentZ);

				min = glm::vec3(center) - newExtent;
				max = glm::vec3(center) + newExtent;

				// project
				pp->zvZ = -1 / std::min(min.z, max.z);
			}
			else
			{
				u32 zv = 0xFFFFFFFF;
				while (vtx != vtx_end)
				{
					zv = std::min(zv, (u32&)vtx->z);
					vtx++;
				}

				pp->zvZ = (f32&)zv;
			}
		}
	}

	std::stable_sort(&polys[first], pp_end);
}

void getRegionTileAddrAndSize(u32& address, u32& size)
{
	address = REGION_BASE;
	const bool type1_tile = ((FPU_PARAM_CFG >> 21) & 1) == 0;
	size = (type1_tile ? 5 : 6) * 4;
	bool empty_first_region = true;
	for (int i = type1_tile ? 4 : 5; i > 0; i--)
		if ((pvr_read32p<u32>(address + i * 4) & 0x80000000) == 0)
		{
			empty_first_region = false;
			break;
		}
	if (empty_first_region)
		address += size;
	RegionArrayTile tile;
	tile.full = pvr_read32p<u32>(address);
	if (tile.PreSort)
		// Windows CE weirdness
		size = 6 * 4;
}

int getTAContextAddresses(u32 *addresses)
{
	u32 addr;
	u32 tile_size;
	getRegionTileAddrAndSize(addr, tile_size);

	RegionArrayTile tile;
	tile.full = pvr_read32p<u32>(addr);
	u32 x = tile.X;
	u32 y = tile.Y;
	u32 count = 0;
	do {
		tile.full = pvr_read32p<u32>(addr);
		if (tile.X != x || tile.Y != y)
			break;
		// Try the opaque pointer
		u32 opbAddr = pvr_read32p<u32>(addr + 4);
		if (opbAddr & 0x80000000)
		{
			// Try the translucent pointer
			opbAddr = pvr_read32p<u32>(addr + 12);
			if (opbAddr & 0x80000000)
			{
				// Try the punch-through pointer
				if (tile_size >= 24)
					opbAddr = pvr_read32p<u32>(addr + 20);
				if (opbAddr & 0x80000000)
				{
					INFO_LOG(PVR, "Can't find any non-null OPB for pass %d", count);
					break;
				}
			}
		}
		addresses[count++] = pvr_read32p<u32>(opbAddr);
		addr += tile_size;
	} while (!tile.LastRegion && count < MAX_PASSES);

	return count;
}

void fix_texture_bleeding(const std::vector<PolyParam>& polys, int first, int end, rend_context& ctx)
{
	auto pp_end = polys.begin() + end;
	for (auto pp = polys.begin() + first; pp != pp_end; ++pp)
	{
		if (!pp->pcw.Texture || pp->count < 3 || pp->isNaomi2())
			continue;
		// Find polygons that are facing the camera (constant z)
		// and only use 0 and 1 for U and V (some tolerance around 1 for SA2)
		// then apply a half-pixel correction on U and V.
		const u32 last = pp->first + pp->count;
		bool need_fixing = true;
		float z = 0.f;
		
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
		/// iOS ARM64 NEON-optimized texture bleeding fix validation
		if (pp->count >= 4) {
			/// Process vertices in batches of 4 for optimal NEON performance
			u32 vertex_count = last - pp->first;
			u32 batches = vertex_count / 4;
			
			float32x4_t zero = vdupq_n_f32(0.0f);
			float32x4_t one = vdupq_n_f32(1.0f);
			float32x4_t threshold = vdupq_n_f32(0.995f);
			
			for (u32 batch = 0; batch < batches && need_fixing; batch++) {
				u32 base_idx = pp->first + batch * 4;
				
				/// Load UV coordinates for 4 vertices
				float32x4_t u_vals = {ctx.verts[base_idx + 0].u, ctx.verts[base_idx + 1].u,
				                      ctx.verts[base_idx + 2].u, ctx.verts[base_idx + 3].u};
				float32x4_t v_vals = {ctx.verts[base_idx + 0].v, ctx.verts[base_idx + 1].v,
				                      ctx.verts[base_idx + 2].v, ctx.verts[base_idx + 3].v};
				float32x4_t z_vals = {ctx.verts[base_idx + 0].z, ctx.verts[base_idx + 1].z,
				                      ctx.verts[base_idx + 2].z, ctx.verts[base_idx + 3].z};
				
				/// Check U coordinates
				uint32x4_t u_not_zero = vmvnq_u32(vceqq_f32(u_vals, zero));
				uint32x4_t u_invalid = vandq_u32(u_not_zero, 
				                                 vorrq_u32(vcleq_f32(u_vals, threshold), 
				                                          vcgtq_f32(u_vals, one)));
				
				/// Check V coordinates
				uint32x4_t v_not_zero = vmvnq_u32(vceqq_f32(v_vals, zero));
				uint32x4_t v_invalid = vandq_u32(v_not_zero,
				                                 vorrq_u32(vcleq_f32(v_vals, threshold),
				                                          vcgtq_f32(v_vals, one)));
				
				/// Check if any UV coordinates are invalid
				uint32x4_t invalid = vorrq_u32(u_invalid, v_invalid);
				if (vmaxvq_u32(invalid) != 0) {
					need_fixing = false;
					break;
				}
				
				/// Check Z consistency (all Z values should be the same)
				if (batch == 0) {
					z = ctx.verts[pp->first].z;
				}
				float32x4_t z_ref = vdupq_n_f32(z);
				uint32x4_t z_diff = vmvnq_u32(vceqq_f32(z_vals, z_ref));
				if (vmaxvq_u32(z_diff) != 0) {
					need_fixing = false;
					break;
				}
			}
			
			/// Handle remaining vertices
			for (u32 idx = pp->first + batches * 4; idx < last && need_fixing; idx++) {
				Vertex& vtx = ctx.verts[idx];
				if (vtx.u != 0.f && (vtx.u <= 0.995f || vtx.u > 1.f))
					need_fixing = false;
				else if (vtx.v != 0.f && (vtx.v <= 0.995f || vtx.v > 1.f))
					need_fixing = false;
				else if (z != vtx.z)
					need_fixing = false;
			}
		} else {
#endif
			/// Fallback to standard processing for small vertex counts
			for (u32 idx = pp->first; idx < last && need_fixing; idx++)
			{
				Vertex& vtx = ctx.verts[idx];

				if (vtx.u != 0.f && (vtx.u <= 0.995f || vtx.u > 1.f))
					need_fixing = false;
				else if (vtx.v != 0.f && (vtx.v <= 0.995f || vtx.v > 1.f))
					need_fixing = false;
				else if (idx == pp->first)
					z = vtx.z;
				else if (z != vtx.z)
					need_fixing = false;
			}
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
		}
#endif
		
		if (!need_fixing)
			continue;
			
		u32 tex_width = 8 << pp->tsp.TexU;
		u32 tex_height = 8 << pp->tsp.TexV;
		
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
		/// iOS ARM64 NEON-optimized texture coordinate correction
		if (pp->count >= 4) {
			u32 vertex_count = last - pp->first;
			u32 batches = vertex_count / 4;
			
			float32x4_t threshold = vdupq_n_f32(0.995f);
			float32x4_t one = vdupq_n_f32(1.0f);
			float32x4_t half = vdupq_n_f32(0.5f);
			float32x4_t tex_w = vdupq_n_f32((float)tex_width);
			float32x4_t tex_h = vdupq_n_f32((float)tex_height);
			float32x4_t tex_w_minus_1 = vdupq_n_f32((float)(tex_width - 1));
			float32x4_t tex_h_minus_1 = vdupq_n_f32((float)(tex_height - 1));
			
			for (u32 batch = 0; batch < batches; batch++) {
				u32 base_idx = pp->first + batch * 4;
				
				/// Load UV coordinates
				float32x4_t u_vals = {ctx.verts[base_idx + 0].u, ctx.verts[base_idx + 1].u,
				                      ctx.verts[base_idx + 2].u, ctx.verts[base_idx + 3].u};
				float32x4_t v_vals = {ctx.verts[base_idx + 0].v, ctx.verts[base_idx + 1].v,
				                      ctx.verts[base_idx + 2].v, ctx.verts[base_idx + 3].v};
				
				/// Clamp U values > 0.995 to 1.0
				uint32x4_t u_clamp_mask = vcgtq_f32(u_vals, threshold);
				u_vals = vbslq_f32(u_clamp_mask, one, u_vals);
				
				/// Clamp V values > 0.995 to 1.0
				uint32x4_t v_clamp_mask = vcgtq_f32(v_vals, threshold);
				v_vals = vbslq_f32(v_clamp_mask, one, v_vals);
				
				/// Apply texture coordinate correction: (0.5 + u * (width - 1)) / width
				float32x4_t u_corrected = vdivq_f32(vaddq_f32(half, vmulq_f32(u_vals, tex_w_minus_1)), tex_w);
				float32x4_t v_corrected = vdivq_f32(vaddq_f32(half, vmulq_f32(v_vals, tex_h_minus_1)), tex_h);
				
				/// Store corrected coordinates
				float temp_u[4], temp_v[4];
				vst1q_f32(temp_u, u_corrected);
				vst1q_f32(temp_v, v_corrected);
				
				for (int i = 0; i < 4; i++) {
					ctx.verts[base_idx + i].u = temp_u[i];
					ctx.verts[base_idx + i].v = temp_v[i];
				}
			}
			
			/// Handle remaining vertices
			for (u32 idx = pp->first + batches * 4; idx < last; idx++) {
				Vertex& vtx = ctx.verts[idx];
				if (vtx.u > 0.995f)
					vtx.u = 1.f;
				vtx.u = (0.5f + vtx.u * (tex_width - 1)) / tex_width;
				if (vtx.v > 0.995f)
					vtx.v = 1.f;
				vtx.v = (0.5f + vtx.v * (tex_height - 1)) / tex_height;
			}
		} else {
#endif
			/// Fallback to standard processing
			for (u32 idx = pp->first; idx < last; idx++)
			{
				Vertex& vtx = ctx.verts[idx];
				if (vtx.u > 0.995f)
					vtx.u = 1.f;
				vtx.u = (0.5f + vtx.u * (tex_width - 1)) / tex_width;
				if (vtx.v > 0.995f)
					vtx.v = 1.f;
				vtx.v = (0.5f + vtx.v * (tex_height - 1)) / tex_height;
			}
#if defined(__aarch64__) && (defined(__APPLE__) || defined(TARGET_IPHONE))
		}
#endif
	}
}

//
// Create the vertex index, eliminating invalid vertices and merging strips when possible.
// Use primitive restart when merging strips.
//
void makePrimRestartIndex(std::vector<PolyParam>& polys, int first, int end, bool merge, rend_context& ctx)
{
	if (first >= (int)polys.size())
		return;
	PolyParam *last_poly = nullptr;
	const PolyParam *end_poly = polys.data() + end;
	for (PolyParam *poly = &polys[first]; poly != end_poly; poly++)
	{
		int first_index;
		bool dupe_next_vtx = false;
		if (merge
				&& last_poly != nullptr
				&& last_poly->count != 0
				&& poly->equivalentIgnoreCullingDirection(*last_poly))
		{
			ctx.idx.push_back(~0);
			dupe_next_vtx = poly->isp.CullMode >= 2 && poly->isp.CullMode != last_poly->isp.CullMode;
			first_index = last_poly->first;
		}
		else
		{
			last_poly = poly;
			first_index = ctx.idx.size();
		}
		int last_good_vtx = -1;
		for (u32 i = 0; i < poly->count; i++)
		{
			const Vertex& vtx = ctx.verts[poly->first + i];
			if (!poly->isNaomi2() && is_vertex_inf(vtx))
			{
				bool odd = i & 1;
				while (i < poly->count - 1)
				{
					odd = !odd;
					const Vertex& next_vtx = ctx.verts[poly->first + i + 1];
					if (!is_vertex_inf(next_vtx))
					{
						if (poly->count - (i + 1) < 3)
							// skip remaining incomplete triangle
							i = poly->count - 1;
						else
						{
							if (last_good_vtx >= 0)
								// reset the strip
								ctx.idx.push_back(~0);
							if (odd && poly->isp.CullMode >= 2)
								// repeat next vertex to get culling right
								dupe_next_vtx = true;
						}
						break;
					}
					i++;
				}
			}
			else
			{
				last_good_vtx = poly->first + i;
				if (dupe_next_vtx)
				{
					ctx.idx.push_back(last_good_vtx);
					dupe_next_vtx = false;
				}
				ctx.idx.push_back(last_good_vtx);
			}
		}
		if (last_poly == poly)
		{
			poly->first = first_index;
			poly->count = ctx.idx.size() - first_index;
		}
		else
		{
			last_poly->count = ctx.idx.size() - last_poly->first;
			poly->count = 0;
		}
	}
}

//
// Create the vertex index, eliminating invalid vertices and merging strips when possible.
// Use degenerate triangles to link strips.
//
void makeIndex(std::vector<PolyParam>& polys, int first, int end, bool merge, rend_context& ctx)
{
	if (first >= (int)polys.size())
		return;
	PolyParam *last_poly = nullptr;
	const PolyParam *end_poly = polys.data() + end;
	bool cullingReversed = false;
	for (PolyParam *poly = &polys[first]; poly != end_poly; poly++)
	{
		int first_index;
		bool dupe_next_vtx = false;
		if (merge
				&& last_poly != nullptr
				&& last_poly->count != 0
				&& poly->equivalentIgnoreCullingDirection(*last_poly))
		{
			const u32 last_vtx = ctx.idx[last_poly->first + last_poly->count - 1];
			ctx.idx.push_back(last_vtx);
			if (poly->isp.CullMode < 2 || poly->isp.CullMode == last_poly->isp.CullMode)
			{
				if (cullingReversed)
					ctx.idx.push_back(last_vtx);
				cullingReversed = false;
			}
			else
			{
				if (!cullingReversed)
					ctx.idx.push_back(last_vtx);
				cullingReversed = true;
			}
			dupe_next_vtx = true;
			first_index = last_poly->first;
		}
		else
		{
			last_poly = poly;
			first_index = ctx.idx.size();
			cullingReversed = false;
		}
		int last_good_vtx = -1;
		for (u32 i = 0; i < poly->count; i++)
		{
			const Vertex& vtx = ctx.verts[poly->first + i];
			if (!poly->isNaomi2() && is_vertex_inf(vtx))
			{
				while (i < poly->count - 1)
				{
					const Vertex& next_vtx = ctx.verts[poly->first + i + 1];
					if (!is_vertex_inf(next_vtx))
					{
						// repeat last and next vertices to link strips
						if (last_good_vtx >= 0)
						{
							verify(!dupe_next_vtx);
							ctx.idx.push_back(last_good_vtx);
							dupe_next_vtx = true;
						}
						break;
					}
					i++;
				}
			}
			else
			{
				last_good_vtx = poly->first + i;
				if (dupe_next_vtx)
				{
					ctx.idx.push_back(last_good_vtx);
					dupe_next_vtx = false;
				}
				const u32 count = ctx.idx.size() - first_index;
				if (((i ^ count) & 1) ^ cullingReversed)
					ctx.idx.push_back(last_good_vtx);
				ctx.idx.push_back(last_good_vtx);
			}
		}
		if (last_poly == poly)
		{
			poly->first = first_index;
			poly->count = ctx.idx.size() - first_index;
		}
		else
		{
			last_poly->count = ctx.idx.size() - last_poly->first;
			poly->count = 0;
		}
	}
}

