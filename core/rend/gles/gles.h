#pragma once
#include "rend/rend.h"
#include <map>

#ifdef GLES
#if defined(TARGET_IPHONE) //apple-specific ogles2 headers
//#include <APPLE/egl.h>
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>
#else
#if !defined(TARGET_NACL32)
#include <EGL/egl.h>
#endif
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

#ifndef GL_NV_draw_path
//IMGTEC GLES emulation
#pragma comment(lib,"libEGL.lib")
#pragma comment(lib,"libGLESv2.lib")
#else /* NV gles emulation*/
#pragma comment(lib,"libGLES20.lib")
#endif

#else
#if HOST_OS == OS_DARWIN
    #include <OpenGL/gl3.h>
#else
	#include <GL3/gl3w.h>
#endif
#endif


#define glCheck() do { if (unlikely(settings.validate.OpenGlChecks)) { verify(glGetError()==GL_NO_ERROR); } } while(0)
#define eglCheck() false

#define VERTEX_POS_ARRAY 0
#define VERTEX_COL_BASE_ARRAY 1
#define VERTEX_COL_OFFS_ARRAY 2
#define VERTEX_UV_ARRAY 3
#define VERTEX_COL_BASE1_ARRAY 4
#define VERTEX_COL_OFFS1_ARRAY 5
#define VERTEX_UV1_ARRAY 6


//vertex types
extern u32 gcflip;
extern float scale_x, scale_y;


void DrawStrips(GLuint output_fbo);

struct PipelineShader
{
	GLuint program;

	GLuint scale,depth_scale;
	GLuint pp_ClipTest,cp_AlphaTestValue;
	GLuint sp_FOG_COL_RAM,sp_FOG_COL_VERT,sp_FOG_DENSITY;
	GLuint shade_scale_factor;
	GLuint pp_Number;
	GLuint pp_Stencil;
	GLuint pp_DepthFunc;
	GLuint blend_mode;
	GLuint use_alpha;
	GLuint ignore_tex_alpha;
	GLuint shading_instr;
	GLuint fog_control;

	//
	u32 cp_AlphaTest; s32 pp_ClipTestMode;
	u32 pp_Texture, pp_UseAlpha, pp_IgnoreTexA, pp_ShadInstr, pp_Offset, pp_FogCtrl;
	int pass;
	bool pp_TwoVolumes;
};


struct gl_ctx
{
#if defined(GLES) && HOST_OS != OS_DARWIN && !defined(TARGET_NACL32)
	struct
	{
		EGLNativeWindowType native_wind;
		EGLNativeDisplayType native_disp;
		EGLDisplay display;
		EGLSurface surface;
		EGLContext context;
	} setup;
#endif

	struct
	{
		GLuint program;

		GLuint scale,depth_scale;
	} modvol_shader;

	std::map<int, PipelineShader *> shaders;
	struct
	{
		GLuint program,scale,depth_scale;
	} OSD_SHADER;

	struct
	{
		GLuint geometry,modvols,idxs,idxs2;
#ifndef GLES
		GLuint vao;
#endif
	} vbo;

	PipelineShader *getShader(int programId) {
		PipelineShader *shader = shaders[programId];
		if (shader == NULL) {
			shader = new PipelineShader();
			shaders[programId] = shader;
			shader->program = -1;
		}
		return shader;
	}
};

extern gl_ctx gl;

GLuint gl_GetTexture(TSP tsp,TCW tcw);
struct text_info {
	u16* pdata;
	u32 width;
	u32 height;
	u32 textype; // 0 565, 1 1555, 2 4444
};

text_info raw_GetTexture(TSP tsp, TCW tcw);
void CollectCleanup();
void DoCleanup();
void SortPParams(int first, int count);

extern int screen_width;
extern int screen_height;

GLuint BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt);
void ReadRTTBuffer();
int GetProgramID(u32 cp_AlphaTest, u32 pp_ClipTestMode,
							u32 pp_Texture, u32 pp_UseAlpha, u32 pp_IgnoreTexA, u32 pp_ShadInstr, u32 pp_Offset,
							u32 pp_FogCtrl, bool two_volumes, int pass);
void SetCull(u32 CulliMode);

struct ShaderUniforms_t
{
	float PT_ALPHA;
	float scale_coefs[4];
	float depth_coefs[4];
	float fog_den_float;
	float ps_FOG_COL_RAM[3];
	float ps_FOG_COL_VERT[3];
	int poly_number;
	u32 stencil;
	TSP tsp0;
	TSP tsp1;
	TCW tcw0;
	TCW tcw1;
	int depth_func;

	void setUniformArray(GLuint location, int v0, int v1)
	{
		int array[] = { v0, v1 };
		glUniform1iv(location, 2, array);
	}

	void Set(PipelineShader* s)
	{
		if (s->cp_AlphaTestValue!=-1)
			glUniform1f(s->cp_AlphaTestValue,PT_ALPHA);

		if (s->scale!=-1)
			glUniform4fv( s->scale, 1, scale_coefs);

		if (s->depth_scale!=-1)
			glUniform4fv( s->depth_scale, 1, depth_coefs);

		if (s->sp_FOG_DENSITY!=-1)
			glUniform1f( s->sp_FOG_DENSITY,fog_den_float);

		if (s->sp_FOG_COL_RAM!=-1)
			glUniform3fv( s->sp_FOG_COL_RAM, 1, ps_FOG_COL_RAM);

		if (s->sp_FOG_COL_VERT!=-1)
			glUniform3fv( s->sp_FOG_COL_VERT, 1, ps_FOG_COL_VERT);

		if (s->shade_scale_factor != -1)
			glUniform1f(s->shade_scale_factor, FPU_SHAD_SCALE.scale_factor / 256.f);

		if (s->blend_mode != -1) {
			u32 blend_mode[] = { tsp0.SrcInstr, tsp0.DstInstr, tsp1.SrcInstr, tsp1.DstInstr };
			glUniform2uiv(s->blend_mode, 2, blend_mode);
		}

		if (s->use_alpha != -1)
			setUniformArray(s->use_alpha, tsp0.UseAlpha, tsp1.UseAlpha);

		if (s->ignore_tex_alpha != -1)
			setUniformArray(s->ignore_tex_alpha, tsp0.IgnoreTexA, tsp1.IgnoreTexA);

		if (s->shading_instr != -1)
			setUniformArray(s->shading_instr, tsp0.ShadInstr, tsp1.ShadInstr);

		if (s->fog_control != -1)
			setUniformArray(s->fog_control, tsp0.FogCtrl, tsp1.FogCtrl);

		if (s->pp_Number != -1)
			glUniform1i(s->pp_Number, poly_number);

		if (s->pp_Stencil != -1)
			glUniform1ui(s->pp_Stencil, stencil);

		if (s->pp_DepthFunc != -1)
			glUniform1i(s->pp_DepthFunc, depth_func);
	}

};
extern ShaderUniforms_t ShaderUniforms;

extern const char *PixelPipelineShader;
bool CompilePipelineShader(PipelineShader* s, const char *source = PixelPipelineShader);
#define TEXTURE_LOAD_ERROR 0
GLuint loadPNG(const string& subpath, int &width, int &height);

extern GLuint stencilTexId;
extern GLuint depthTexId;
extern GLuint opaqueTexId;
extern GLuint depthSaveTexId;

#define SHADER_HEADER "#version 430 \n\
\n\
layout(size1x32, binding = 4) uniform coherent restrict uimage2D abufferPointerImg; \n\
struct Pixel { \n\
	mediump vec4 color; \n\
	mediump float depth; \n\
	int seq_num; \n\
	uint blend_stencil; \n\
	uint next; \n\
}; \n\
#define EOL 0xFFFFFFFFu \n\
layout (binding = 0, std430) coherent restrict buffer PixelBuffer { \n\
	Pixel pixels[]; \n\
}; \n\
layout(binding = 0, offset = 0) uniform atomic_uint buffer_index; \n\
\n\
uint getNextPixelIndex() \n\
{ \n\
	return atomicCounterIncrement(buffer_index); \n\
} \n\
\n\
void setFragDepth(void) \n\
{ \n\
	highp float w = 100000.0 * gl_FragCoord.w; \n\
	gl_FragDepth = 1 - log2(1.0 + w) / 34; \n\
} \n\
"

void SetupModvolVBO();
