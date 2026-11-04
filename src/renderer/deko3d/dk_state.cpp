/*
===========================================================================

Quake4NX - native deko3D renderer: state tracking.

State changes only mark what they touch. Dk_FlushForDraw writes the commands
for what really changed, right before the draw that needs it, so a run of
surfaces with the same state costs little more than its draw commands.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../tr_local.h"

#undef BIT
#include <switch.h>

#include "dk_local.h"

frameData_t *	frameData;
backEndState_t	backEnd;

enum {
	DIRTY_RASTERIZER	= 1 << 0,
	DIRTY_COLOR			= 1 << 1,
	DIRTY_COLOR_WRITE	= 1 << 2,
	DIRTY_BLEND			= 1 << 3,
	DIRTY_DEPTH_STENCIL	= 1 << 4,
	DIRTY_PROGRAM		= 1 << 5,
	DIRTY_FORMAT		= 1 << 6,
	DIRTY_ALL			= ( 1 << 7 ) - 1
};

// UBO layout, in bytes from the start of the uniform block
static const uint32_t UBO_ALIGN			= DK_UNIFORM_BUF_ALIGNMENT;
static const uint32_t UBO_MATRICES_SIZE	= 16 * 4 * sizeof( float );			// mvp, modelview, projection, texture rows
static const uint32_t UBO_VS_ENV_SIZE	= DK_VS_ENV_COUNT * 4 * sizeof( float );
static const uint32_t UBO_LOCAL_SIZE	= DK_LOCAL_COUNT * 4 * sizeof( float );
static const uint32_t UBO_FS_ENV_SIZE	= DK_FS_ENV_COUNT * 4 * sizeof( float );

typedef struct {
	DkGpuAddr	addr;
	uint32_t	size;
	float *		shadow;
	int			dirtyFirst;		// in vec4 registers
	int			dirtyLast;
} dkUniformBlock_t;

enum {
	UB_MATRICES,
	UB_VS_ENV,
	UB_VS_LOCAL,
	UB_FS_ENV,
	UB_FS_LOCAL,
	UB_COUNT
};

static dkUniformBlock_t	s_blocks[UB_COUNT];
static float			s_matrices[16 * 4];
static float			s_vsEnv[DK_VS_ENV_COUNT * 4];
static float			s_vsLocal[DK_LOCAL_COUNT * 4];
static float			s_fsEnv[DK_FS_ENV_COUNT * 4];
static float			s_fsLocal[DK_LOCAL_COUNT * 4];

static DkRasterizerState	s_rasterizer;
static DkColorState			s_color;
static DkColorWriteState	s_colorWrite;
static DkBlendState			s_blend;
static DkDepthStencilState	s_depthStencil;
static DkMultisampleState	s_multisample;
static const DkShader *		s_vertexShader;
static const DkShader *		s_fragmentShader;
static dkVertexFormat_t		s_format;
static DkResHandle			s_textures[DK_MAX_TEXTURE_UNITS];
static uint32_t				s_textureDirty;
static uint32_t				s_dirty;
static bool					s_depthTest;
static bool					s_stencilTest;
static bool					s_depthWrite;
static DkCompareOp			s_depthFunc;
static int					s_targetHeight = DK_SCREEN_HEIGHT;
static bool					s_targetFlipped = false;

/*
================
Vertex formats
================
*/
static const DkVtxAttribState s_drawVertAttribs[] = {
	{ 0, 0, DRAWVERT_XYZ_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },			// 0 position
	{ 0, 0, DRAWVERT_ST_OFFSET, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },				// 1 texcoord
	{ 0, 0, DRAWVERT_NORMAL_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },			// 2 normal
	{ 0, 0, DRAWVERT_TANGENT0_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },		// 3 tangent
	{ 0, 0, DRAWVERT_TANGENT1_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },		// 4 bitangent
	{ 0, 0, DRAWVERT_COLOR_OFFSET, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0 },			// 5 color
};

static const DkVtxAttribState s_drawVertColorAttribs[] = {
	{ 0, 0, DRAWVERT_XYZ_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_ST_OFFSET, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_NORMAL_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_TANGENT0_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_TANGENT1_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 1, 0, 0, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0 },
};

static const DkVtxAttribState s_drawVertTex3Attribs[] = {
	{ 0, 0, DRAWVERT_XYZ_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 1, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_NORMAL_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_TANGENT0_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_TANGENT1_OFFSET, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
	{ 0, 0, DRAWVERT_COLOR_OFFSET, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0 },
};

static const DkVtxAttribState s_shadowAttribs[] = {
	{ 0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 },
};

static const DkVtxBufferState s_drawVertBuffers[] = { { sizeof( idDrawVert ), 0 } };
static const DkVtxBufferState s_colorBuffers[] = { { sizeof( idDrawVert ), 0 }, { 4, 0 } };
static const DkVtxBufferState s_tex3Buffers[] = { { sizeof( idDrawVert ), 0 }, { 3 * sizeof( float ), 0 } };
static const DkVtxBufferState s_shadowBuffers[] = { { sizeof( shadowCache_t ), 0 } };

static void Dk_FlushFormat( void ) {
	switch ( s_format ) {
		case VFMT_DRAWVERT:
			dkCmdBufBindVtxAttribState( dkr.cmd, s_drawVertAttribs, 6 );
			dkCmdBufBindVtxBufferState( dkr.cmd, s_drawVertBuffers, 1 );
			break;
		case VFMT_DRAWVERT_COLOR:
			dkCmdBufBindVtxAttribState( dkr.cmd, s_drawVertColorAttribs, 6 );
			dkCmdBufBindVtxBufferState( dkr.cmd, s_colorBuffers, 2 );
			break;
		case VFMT_DRAWVERT_TEX3:
			dkCmdBufBindVtxAttribState( dkr.cmd, s_drawVertTex3Attribs, 6 );
			dkCmdBufBindVtxBufferState( dkr.cmd, s_tex3Buffers, 2 );
			break;
		case VFMT_SHADOW:
			dkCmdBufBindVtxAttribState( dkr.cmd, s_shadowAttribs, 1 );
			dkCmdBufBindVtxBufferState( dkr.cmd, s_shadowBuffers, 1 );
			break;
		default:
			break;
	}
}

/*
================
Dk_InitState
================
*/
void Dk_InitState( void ) {
	const DkGpuAddr base = dkMemBlockGetGpuAddr( dkr.uniformBlock );
	uint32_t offset = 0;
	const struct { float *shadow; uint32_t size; } layout[UB_COUNT] = {
		{ s_matrices, UBO_MATRICES_SIZE },
		{ s_vsEnv, UBO_VS_ENV_SIZE },
		{ s_vsLocal, UBO_LOCAL_SIZE },
		{ s_fsEnv, UBO_FS_ENV_SIZE },
		{ s_fsLocal, UBO_LOCAL_SIZE },
	};
	for ( int i = 0; i < UB_COUNT; i++ ) {
		s_blocks[i].addr = base + offset;
		s_blocks[i].size = ( layout[i].size + UBO_ALIGN - 1 ) & ~( UBO_ALIGN - 1 );
		s_blocks[i].shadow = layout[i].shadow;
		offset += s_blocks[i].size;
	}
	memset( s_matrices, 0, sizeof( s_matrices ) );
	memset( s_vsEnv, 0, sizeof( s_vsEnv ) );
	memset( s_vsLocal, 0, sizeof( s_vsLocal ) );
	memset( s_fsEnv, 0, sizeof( s_fsEnv ) );
	memset( s_fsLocal, 0, sizeof( s_fsLocal ) );

	dkRasterizerStateDefaults( &s_rasterizer );
	dkColorStateDefaults( &s_color );
	dkColorWriteStateDefaults( &s_colorWrite );
	dkBlendStateDefaults( &s_blend );
	dkDepthStencilStateDefaults( &s_depthStencil );
	dkMultisampleStateDefaults( &s_multisample );

	s_rasterizer.cullMode = DkFace_None;
	s_depthStencil.depthTestEnable = true;
	s_depthStencil.depthWriteEnable = true;
	s_depthStencil.stencilTestEnable = false;
	s_depthStencil.stencilFrontFailOp = s_depthStencil.stencilBackFailOp = DkStencilOp_Keep;
	s_depthStencil.stencilFrontPassOp = s_depthStencil.stencilBackPassOp = DkStencilOp_Keep;
	s_depthStencil.stencilFrontDepthFailOp = s_depthStencil.stencilBackDepthFailOp = DkStencilOp_Keep;

	s_depthTest = true;
	s_stencilTest = false;
	s_depthWrite = true;
	s_depthFunc = DkCompareOp_Less;
}

/*
================
Dk_ResetState

A new frame's command list starts with everything bound again.
================
*/
void Dk_ResetState( void ) {
	s_vertexShader = NULL;
	s_fragmentShader = NULL;
	s_format = VFMT_NONE;
	s_dirty = DIRTY_ALL & ~DIRTY_PROGRAM & ~DIRTY_FORMAT;
	s_textureDirty = 0;
	memset( s_textures, 0xff, sizeof( s_textures ) );

	dkCmdBufBindImageDescriptorSet( dkr.cmd, dkMemBlockGetGpuAddr( dkr.imageDescriptorBlock ), DK_MAX_IMAGE_DESCRIPTORS );
	dkCmdBufBindSamplerDescriptorSet( dkr.cmd, dkMemBlockGetGpuAddr( dkr.samplerDescriptorBlock ), DK_NUM_SAMPLERS );
	dkCmdBufBarrier( dkr.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader );
	dkCmdBufBindMultisampleState( dkr.cmd, &s_multisample );

	for ( int i = 0; i < UB_COUNT; i++ ) {
		s_blocks[i].dirtyFirst = 0;
		s_blocks[i].dirtyLast = s_blocks[i].size / ( 4 * sizeof( float ) ) - 1;
	}

	dkCmdBufBindUniformBuffer( dkr.cmd, DkStage_Vertex, 0, s_blocks[UB_MATRICES].addr, s_blocks[UB_MATRICES].size );
	dkCmdBufBindUniformBuffer( dkr.cmd, DkStage_Vertex, 1, s_blocks[UB_VS_ENV].addr, s_blocks[UB_VS_ENV].size );
	dkCmdBufBindUniformBuffer( dkr.cmd, DkStage_Vertex, 2, s_blocks[UB_VS_LOCAL].addr, s_blocks[UB_VS_LOCAL].size );
	dkCmdBufBindUniformBuffer( dkr.cmd, DkStage_Fragment, 0, s_blocks[UB_FS_ENV].addr, s_blocks[UB_FS_ENV].size );
	dkCmdBufBindUniformBuffer( dkr.cmd, DkStage_Fragment, 1, s_blocks[UB_FS_LOCAL].addr, s_blocks[UB_FS_LOCAL].size );
}

void Dk_BindRenderTarget( void ) {
	const DkImageView *color = &dkr.colorViews[dkr.imageSlot];
	dkCmdBufBindRenderTargets( dkr.cmd, &color, 1, &dkr.depthView );
}

void Dk_BindScreenTarget( void ) {
	if ( dkr.imageSlot < 0 ) {
		return;
	}
	Dk_BindRenderTarget();
	Dk_SetTargetMetrics( DK_SCREEN_HEIGHT, false );
}

bool Dk_TargetIsScreen( void ) {
	return !s_targetFlipped;
}

void Dk_ClearRenderTarget( bool clearColor, const float *rgba, bool clearDepth, float depth ) {
	Dk_FlushForDraw();
	if ( clearColor ) {
		dkCmdBufClearColorFloat( dkr.cmd, 0, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3] );
	}
	if ( clearDepth ) {
		dkCmdBufClearDepthStencil( dkr.cmd, true, depth, 0xff, 128 );
	}
}

void Dk_ColorTargetBarrier( void ) {
	dkCmdBufBarrier( dkr.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image );
}

/*
================
Registers
================
*/
static ID_INLINE void Dk_MarkDirty( dkUniformBlock_t &block, int first, int last ) {
	if ( block.dirtyFirst > block.dirtyLast ) {
		block.dirtyFirst = first;
		block.dirtyLast = last;
	} else {
		if ( first < block.dirtyFirst ) {
			block.dirtyFirst = first;
		}
		if ( last > block.dirtyLast ) {
			block.dirtyLast = last;
		}
	}
}

static ID_INLINE void Dk_SetRegister( dkUniformBlock_t &block, int index, const float *value ) {
	float *dst = block.shadow + index * 4;
	if ( dst[0] == value[0] && dst[1] == value[1] && dst[2] == value[2] && dst[3] == value[3] ) {
		return;
	}
	dst[0] = value[0];
	dst[1] = value[1];
	dst[2] = value[2];
	dst[3] = value[3];
	Dk_MarkDirty( block, index, index );
}

void Dk_SetVsEnv( int index, const float *value ) {
	Dk_SetRegister( s_blocks[UB_VS_ENV], index, value );
}

void Dk_SetVsLocal( int index, const float *value ) {
	Dk_SetRegister( s_blocks[UB_VS_LOCAL], index, value );
}

void Dk_SetFsEnv( int index, const float *value ) {
	Dk_SetRegister( s_blocks[UB_FS_ENV], index, value );
}

void Dk_SetFsLocal( int index, const float *value ) {
	Dk_SetRegister( s_blocks[UB_FS_LOCAL], index, value );
}

// column-major GL matrix to rows, for programs that DP4 against them
static ID_INLINE void Dk_MatrixToRows( const float *m, float *rows ) {
	for ( int r = 0; r < 4; r++ ) {
		rows[r * 4 + 0] = m[0 + r];
		rows[r * 4 + 1] = m[4 + r];
		rows[r * 4 + 2] = m[8 + r];
		rows[r * 4 + 3] = m[12 + r];
	}
}

void Dk_SetMvp( const float *modelView, const float *projection ) {
	float mvp[16];
	myGlMultMatrix( modelView, projection, mvp );
	Dk_MatrixToRows( mvp, s_matrices );
	Dk_MatrixToRows( modelView, s_matrices + 16 );
	Dk_MatrixToRows( projection, s_matrices + 32 );
	Dk_MarkDirty( s_blocks[UB_MATRICES], 0, 11 );
}

void Dk_SetTextureMatrix( const float *matrix ) {
	Dk_MatrixToRows( matrix, s_matrices + 48 );
	Dk_MarkDirty( s_blocks[UB_MATRICES], 12, 15 );
}

/*
================
Bindings
================
*/
void Dk_SetProgram( const DkShader *vertex, const DkShader *fragment ) {
	if ( vertex != s_vertexShader || fragment != s_fragmentShader ) {
		s_vertexShader = vertex;
		s_fragmentShader = fragment;
		s_dirty |= DIRTY_PROGRAM;
	}
}

void Dk_SetVertexFormat( dkVertexFormat_t format ) {
	if ( format != s_format ) {
		s_format = format;
		s_dirty |= DIRTY_FORMAT;
	}
}

void Dk_BindVertexBuffer( int slot, DkGpuAddr addr, uint32_t size ) {
	dkCmdBufBindVtxBuffer( dkr.cmd, slot, addr, size );
}

void Dk_SetTexture( int unit, DkResHandle handle ) {
	if ( s_textures[unit] != handle ) {
		s_textures[unit] = handle;
		s_textureDirty |= 1u << unit;
	}
}

// The engine measures y from the bottom. The screen's first row is its top, so y turns
// around there. A render target keeps OpenGL's rows, first row at the bottom, for
// the materials that read it: there the viewport's height is negative.
void Dk_SetViewport( int x, int y, int w, int h, float minDepth, float maxDepth ) {
	DkViewport viewport;
	if ( s_targetFlipped ) {
		viewport = { (float)x, (float)( y + h ), (float)w, (float)-h, minDepth, maxDepth };
	} else {
		viewport = { (float)x, (float)( s_targetHeight - y - h ), (float)w, (float)h, minDepth, maxDepth };
	}
	dkCmdBufSetViewports( dkr.cmd, 0, &viewport, 1 );
}

void Dk_SetScissor( int x, int y, int w, int h ) {
	const int top = s_targetFlipped ? y : s_targetHeight - y - h;
	DkScissor scissor = { (uint32_t)Max( x, 0 ), (uint32_t)Max( top, 0 ), (uint32_t)Max( w, 0 ), (uint32_t)Max( h, 0 ) };
	dkCmdBufSetScissors( dkr.cmd, 0, &scissor, 1 );
}

void Dk_SetTargetMetrics( int height, bool flipped ) {
	s_targetHeight = height;
	s_targetFlipped = flipped;
	// a flipped target turns triangles around: what the mirrored path does for culling
	const DkFrontFace face = flipped ? DkFrontFace_CW : DkFrontFace_CCW;
	if ( s_rasterizer.frontFace != face ) {
		s_rasterizer.frontFace = face;
		s_dirty |= DIRTY_RASTERIZER;
	}
}

void Dk_SetDepthTest( bool enable ) {
	if ( enable != s_depthTest ) {
		s_depthTest = enable;
		s_depthStencil.depthTestEnable = enable;
		s_dirty |= DIRTY_DEPTH_STENCIL;
	}
}

void Dk_SetStencilTest( bool enable ) {
	if ( enable != s_stencilTest ) {
		s_stencilTest = enable;
		s_depthStencil.stencilTestEnable = enable;
		s_dirty |= DIRTY_DEPTH_STENCIL;
	}
}

void Dk_SetStencilFunc( DkCompareOp func, int ref, int mask ) {
	if ( s_depthStencil.stencilFrontCompareOp != func || s_depthStencil.stencilBackCompareOp != func ) {
		s_depthStencil.stencilFrontCompareOp = func;
		s_depthStencil.stencilBackCompareOp = func;
		s_dirty |= DIRTY_DEPTH_STENCIL;
	}
	dkCmdBufSetStencil( dkr.cmd, DkFace_FrontAndBack, 0xff, (uint8_t)ref, (uint8_t)mask );
}

void Dk_SetStencilOps( DkStencilOp frontZFail, DkStencilOp frontZPass, DkStencilOp backZFail, DkStencilOp backZPass ) {
	if ( s_depthStencil.stencilFrontDepthFailOp != frontZFail || s_depthStencil.stencilFrontPassOp != frontZPass ||
		s_depthStencil.stencilBackDepthFailOp != backZFail || s_depthStencil.stencilBackPassOp != backZPass ) {
		s_depthStencil.stencilFrontDepthFailOp = frontZFail;
		s_depthStencil.stencilFrontPassOp = frontZPass;
		s_depthStencil.stencilBackDepthFailOp = backZFail;
		s_depthStencil.stencilBackPassOp = backZPass;
		s_dirty |= DIRTY_DEPTH_STENCIL;
	}
}

void Dk_SetPolygonOffset( bool enable, float factor, float units ) {
	const uint32_t mask = enable ? ( DkPolygonFlag_Fill | DkPolygonFlag_Line | DkPolygonFlag_Point ) : 0;
	if ( s_rasterizer.depthBiasEnableMask != mask ) {
		s_rasterizer.depthBiasEnableMask = mask;
		s_dirty |= DIRTY_RASTERIZER;
	}
	if ( enable ) {
		dkCmdBufSetDepthBias( dkr.cmd, units, 0.0f, factor );
	}
}

void Dk_SetDepthBounds( bool enable, float zmin, float zmax ) {
	dkCmdBufSetDepthBounds( dkr.cmd, enable, zmin, zmax );
}

void Dk_Clear( bool color, const float *rgba, bool depth, bool stencil, int stencilValue ) {
	if ( color ) {
		dkCmdBufClearColorFloat( dkr.cmd, 0, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3] );
	}
	if ( depth || stencil ) {
		dkCmdBufClearDepthStencil( dkr.cmd, depth, 1.0f, stencil ? 0xff : 0, (uint8_t)stencilValue );
	}
}

/*
================
Dk_FlushForDraw
================
*/
void Dk_FlushForDraw( void ) {
	const uint32_t dirty = s_dirty;
	if ( dirty ) {
		s_dirty = 0;
		if ( dirty & DIRTY_PROGRAM ) {
			const DkShader *shaders[2] = { s_vertexShader, s_fragmentShader };
			dkCmdBufBindShaders( dkr.cmd, DkStageFlag_GraphicsMask, shaders, s_fragmentShader ? 2 : 1 );
		}
		if ( dirty & DIRTY_FORMAT ) {
			Dk_FlushFormat();
		}
		if ( dirty & DIRTY_RASTERIZER ) {
			dkCmdBufBindRasterizerState( dkr.cmd, &s_rasterizer );
		}
		if ( dirty & DIRTY_COLOR ) {
			dkCmdBufBindColorState( dkr.cmd, &s_color );
		}
		if ( dirty & DIRTY_COLOR_WRITE ) {
			dkCmdBufBindColorWriteState( dkr.cmd, &s_colorWrite );
		}
		if ( dirty & DIRTY_BLEND ) {
			dkCmdBufBindBlendState( dkr.cmd, 0, &s_blend );
		}
		if ( dirty & DIRTY_DEPTH_STENCIL ) {
			dkCmdBufBindDepthStencilState( dkr.cmd, &s_depthStencil );
		}
	}

	for ( int i = 0; i < UB_COUNT; i++ ) {
		dkUniformBlock_t &block = s_blocks[i];
		if ( block.dirtyFirst <= block.dirtyLast ) {
			const uint32_t offset = block.dirtyFirst * 4 * sizeof( float );
			const uint32_t size = ( block.dirtyLast - block.dirtyFirst + 1 ) * 4 * sizeof( float );
			dkCmdBufPushConstants( dkr.cmd, block.addr, block.size, offset, size, block.shadow + block.dirtyFirst * 4 );
			block.dirtyFirst = 1;
			block.dirtyLast = 0;
		}
	}

	if ( s_textureDirty ) {
		uint32_t mask = s_textureDirty;
		s_textureDirty = 0;
		for ( int unit = 0; mask; unit++, mask >>= 1 ) {
			if ( mask & 1 ) {
				dkCmdBufBindTexture( dkr.cmd, DkStage_Fragment, unit, s_textures[unit] );
			}
		}
	}
}

void Dk_DrawIndexed( DkGpuAddr indexAddr, int numIndexes ) {
	Dk_FlushForDraw();
	dkCmdBufBindIdxBuffer( dkr.cmd, DkIdxFormat_Uint32, indexAddr );
	dkCmdBufDrawIndexed( dkr.cmd, DkPrimitive_Triangles, numIndexes, 1, 0, 0, 0 );
	dkr.drawsThisFrame++;
}

void Dk_DrawStrip( int numVerts ) {
	Dk_FlushForDraw();
	dkCmdBufDraw( dkr.cmd, DkPrimitive_TriangleStrip, numVerts, 1, 0, 0 );
	dkr.drawsThisFrame++;
}

/*
================
GL_State

This routine is responsible for setting the most commonly changed state
================
*/
static DkBlendFactor Dk_SrcFactor( int bits ) {
	switch ( bits & GLS_SRCBLEND_BITS ) {
		case GLS_SRCBLEND_ZERO:					return DkBlendFactor_Zero;
		case GLS_SRCBLEND_ONE:					return DkBlendFactor_One;
		case GLS_SRCBLEND_DST_COLOR:			return DkBlendFactor_DstColor;
		case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	return DkBlendFactor_InvDstColor;
		case GLS_SRCBLEND_SRC_ALPHA:			return DkBlendFactor_SrcAlpha;
		case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	return DkBlendFactor_InvSrcAlpha;
		case GLS_SRCBLEND_DST_ALPHA:			return DkBlendFactor_DstAlpha;
		case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	return DkBlendFactor_InvDstAlpha;
		case GLS_SRCBLEND_ALPHA_SATURATE:		return DkBlendFactor_SrcAlphaSaturate;
		case GLS_SRCBLEND_SRC_COLOR:			return DkBlendFactor_SrcColor;
		case GLS_SRCBLEND_ONE_MINUS_SRC_COLOR:	return DkBlendFactor_InvSrcColor;
	}
	return DkBlendFactor_One;
}

static DkBlendFactor Dk_DstFactor( int bits ) {
	switch ( bits & GLS_DSTBLEND_BITS ) {
		case GLS_DSTBLEND_ZERO:					return DkBlendFactor_Zero;
		case GLS_DSTBLEND_ONE:					return DkBlendFactor_One;
		case GLS_DSTBLEND_SRC_COLOR:			return DkBlendFactor_SrcColor;
		case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	return DkBlendFactor_InvSrcColor;
		case GLS_DSTBLEND_SRC_ALPHA:			return DkBlendFactor_SrcAlpha;
		case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	return DkBlendFactor_InvSrcAlpha;
		case GLS_DSTBLEND_DST_ALPHA:			return DkBlendFactor_DstAlpha;
		case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	return DkBlendFactor_InvDstAlpha;
	}
	return DkBlendFactor_Zero;
}

void GL_State( int stateBits ) {
	int diff;

	if ( !r_useStateCaching.GetBool() || backEnd.glState.forceGlState ) {
		diff = -1;
		backEnd.glState.forceGlState = false;
	} else {
		diff = stateBits ^ backEnd.glState.glStateBits;
		if ( !diff ) {
			return;
		}
	}

	if ( diff & ( GLS_DEPTHFUNC_EQUAL | GLS_DEPTHFUNC_LESS | GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK ) ) {
		DkCompareOp func = DkCompareOp_Lequal;
		if ( stateBits & GLS_DEPTHFUNC_EQUAL ) {
			func = DkCompareOp_Equal;
		} else if ( stateBits & GLS_DEPTHFUNC_ALWAYS ) {
			func = DkCompareOp_Always;
		}
		s_depthStencil.depthCompareOp = func;
		s_depthStencil.depthWriteEnable = !( stateBits & GLS_DEPTHMASK );
		s_dirty |= DIRTY_DEPTH_STENCIL;
	}

	if ( diff & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) {
		const DkBlendFactor src = Dk_SrcFactor( stateBits );
		const DkBlendFactor dst = Dk_DstFactor( stateBits );
		const bool opaque = src == DkBlendFactor_One && dst == DkBlendFactor_Zero;
		dkBlendStateSetFactors( &s_blend, src, dst, src, dst );
		dkColorStateSetBlendEnable( &s_color, 0, !opaque );
		s_dirty |= DIRTY_BLEND | DIRTY_COLOR;
	}

	if ( diff & ( GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK ) ) {
		uint32_t mask = 0;
		if ( !( stateBits & GLS_REDMASK ) ) {
			mask |= DkColorMask_R;
		}
		if ( !( stateBits & GLS_GREENMASK ) ) {
			mask |= DkColorMask_G;
		}
		if ( !( stateBits & GLS_BLUEMASK ) ) {
			mask |= DkColorMask_B;
		}
		if ( !( stateBits & GLS_ALPHAMASK ) ) {
			mask |= DkColorMask_A;
		}
		dkColorWriteStateSetMask( &s_colorWrite, 0, mask );
		s_dirty |= DIRTY_COLOR_WRITE;
	}

	if ( diff & GLS_POLYMODE_LINE ) {
		const DkPolygonMode mode = ( stateBits & GLS_POLYMODE_LINE ) ? DkPolygonMode_Line : DkPolygonMode_Fill;
		s_rasterizer.polygonModeFront = mode;
		s_rasterizer.polygonModeBack = mode;
		s_dirty |= DIRTY_RASTERIZER;
	}

	if ( diff & GLS_ATEST_BITS ) {
		switch ( stateBits & GLS_ATEST_BITS ) {
			case GLS_ATEST_EQ_255:
				s_color.alphaCompareOp = DkCompareOp_Equal;
				dkCmdBufSetAlphaRef( dkr.cmd, 1.0f );
				break;
			case GLS_ATEST_LT_128:
				s_color.alphaCompareOp = DkCompareOp_Less;
				dkCmdBufSetAlphaRef( dkr.cmd, 0.5f );
				break;
			case GLS_ATEST_GE_128:
				s_color.alphaCompareOp = DkCompareOp_Gequal;
				dkCmdBufSetAlphaRef( dkr.cmd, 0.5f );
				break;
			default:
				s_color.alphaCompareOp = DkCompareOp_Always;
				break;
		}
		s_dirty |= DIRTY_COLOR;
	}

	backEnd.glState.glStateBits = stateBits;
}

void GL_ClearStateDelta( void ) {
	backEnd.glState.forceGlState = true;
}

/*
================
GL_Cull

The id convention is that CT_FRONT_SIDED draws what OpenGL calls back faces,
and mirrored views swap the two.
================
*/
void GL_Cull( int cullType ) {
	if ( backEnd.glState.faceCulling == cullType ) {
		return;
	}

	const bool mirror = backEnd.viewDef && backEnd.viewDef->isMirror;
	DkFace mode = DkFace_None;
	if ( cullType == CT_BACK_SIDED ) {
		mode = mirror ? DkFace_Front : DkFace_Back;
	} else if ( cullType == CT_FRONT_SIDED ) {
		mode = mirror ? DkFace_Back : DkFace_Front;
	}
	if ( s_rasterizer.cullMode != mode ) {
		s_rasterizer.cullMode = mode;
		s_dirty |= DIRTY_RASTERIZER;
	}

	backEnd.glState.faceCulling = cullType;
}

void GL_SelectTexture( int unit ) {
	backEnd.glState.currenttmu = unit;
}

void GL_SelectTextureNoClient( int unit ) {
	backEnd.glState.currenttmu = unit;
}

void GL_TexEnv( int env ) {
}
