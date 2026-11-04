/*
===========================================================================

Quake4NX - native deko3D renderer, internal interface.

The device keeps clip space y up and depth -1..1, as on OpenGL, so the engine's
matrices and winding work unchanged. The screen's first row is its top, so the
window coordinates of viewports, scissors and fragment positions are converted
from OpenGL's bottom up ones.
	dk_device.cpp	device, memory, swapchain, command buffers, frame pacing
	dk_image.cpp	idImage: storage, uploads, screen copies
	dk_state.cpp	GL_State and everything the draws share
	dk_programs.cpp	shader modules, by the name materials give them
	dk_draw.cpp		the view: depth, stencil shadows, interactions, passes, fog
	dk_backend.cpp	the frame's commands, render targets, what the front end links against
	dk_GLStubs.cpp	the OpenGL calls left in the front end

===========================================================================
*/

#ifndef __DK_LOCAL_H__
#define __DK_LOCAL_H__

#include <deko3d.h>

static const int DK_SCREEN_WIDTH		= 1280;
static const int DK_SCREEN_HEIGHT		= 720;
static const int DK_SWAP_IMAGES			= 3;
static const int DK_FRAMES_IN_FLIGHT	= 2;
static const int DK_MAX_TEXTURE_UNITS	= 8;
static const int DK_MAX_IMAGE_DESCRIPTORS = 8192;
static const int DK_NUM_SAMPLERS		= 18;		// texture filters * texture repeats
static const int DK_FS_ENV_SCREEN_SIZE	= 7;		// fragment env register with the screen's size

/*
================================================================================
Memory

The system allows few memory blocks, so storage comes from large ones.
================================================================================
*/
class idDkHeap {
public:
				idDkHeap();
	void		Init( DkDevice device, uint32_t flags, uint32_t blockSize, uint32_t maxBytes );
	void		Shutdown();
	bool		Alloc( uint32_t size, uint32_t align, dkAlloc_t &alloc );
	void		Free( const dkAlloc_t &alloc );
	void *		CpuAddr( const dkAlloc_t &alloc ) const;
	DkGpuAddr	GpuAddr( const dkAlloc_t &alloc ) const;
	uint32_t	BytesUsed() const { return bytesUsed; }
	uint32_t	BytesReserved() const { return numBlocks * blockSize; }

private:
	static const int MAX_BLOCKS = 32;
	typedef struct {
		uint32_t	offset;
		uint32_t	size;
	} freeRange_t;

	bool		AddBlock();

	DkDevice			device;
	uint32_t			flags;
	uint32_t			blockSize;
	uint32_t			maxBlocks;
	uint32_t			bytesUsed;
	int					numBlocks;
	DkMemBlock			blocks[MAX_BLOCKS];
	idList<freeRange_t>	freeRanges[MAX_BLOCKS];
};

/*
================================================================================
Device
================================================================================
*/
typedef struct {
	DkDevice		device;
	DkQueue			queue;
	DkCmdBuf		cmd;				// the frame's commands
	DkCmdBuf		uploadCmd;			// transfers, submitted ahead of the frame
	DkSwapchain		swapchain;
	DkImage			colorImages[DK_SWAP_IMAGES];
	DkImage			depthImage;
	DkImageView		colorViews[DK_SWAP_IMAGES];
	DkImageView		depthView;
	int				imageSlot;			// acquired swapchain image, -1 between frames
	int				frameIndex;			// 0 .. DK_FRAMES_IN_FLIGHT-1
	unsigned int	frameCount;

	DkMemBlock		imageDescriptorBlock;
	DkMemBlock		samplerDescriptorBlock;
	DkMemBlock		uniformBlock;

	idDkHeap		imageHeap;
	idDkHeap		vertexHeap;

	unsigned int	drawsThisFrame;
} dkGlobals_t;

extern dkGlobals_t	dkr;

bool		Dk_Init( void );
void		Dk_Shutdown( void );
void		Dk_BeginFrame( void );				// acquires the screen on first use in a frame
void		Dk_Present( void );
void		Dk_WaitIdle( void );
bool		Dk_ReadPixels( int x, int y, int width, int height, void *pixels, int components );

// Bytes for a transfer. Dk_SubmitUploads runs them ahead of the frame's commands.
void *		Dk_UploadSpace( uint32_t size, uint32_t align, DkGpuAddr &gpuAddr );
void		Dk_SubmitUploads( void );

// Frees wait for the frames that may still be reading them.
void		Dk_DeferFreeImage( dkAlloc_t alloc, int descriptorIndex );
void		Dk_DeferFreeVertex( dkAlloc_t alloc );

int			Dk_AllocImageDescriptor( void );
void		Dk_WriteImageDescriptor( int index, const DkImageView &view );
void		Dk_DescriptorsChanged( void );
int			Dk_SamplerIndex( textureFilter_t filter, textureRepeat_t repeat );

/*
================================================================================
State (dk_state.cpp)
================================================================================
*/
typedef enum {
	VFMT_NONE,
	VFMT_DRAWVERT,			// idDrawVert in buffer 0
	VFMT_DRAWVERT_COLOR,	// idDrawVert, with colors from buffer 1
	VFMT_DRAWVERT_TEX3,		// idDrawVert, with 3D texture coordinates from buffer 1
	VFMT_SHADOW,			// homogeneous positions in buffer 0
	VFMT_COUNT
} dkVertexFormat_t;

// ARB program registers. The rows are uploaded row-major, so programs read them with DP4.
static const int DK_VS_ENV_COUNT	= 24;
static const int DK_LOCAL_COUNT		= 16;
static const int DK_FS_ENV_COUNT	= 8;

// fixed function stages keep their inputs in the program local registers
typedef enum {
	FFV_COLOR = 0,			// constant color
	FFV_COLOR_MODE,			// rgb modulate, rgb add, alpha modulate, alpha add of the vertex color
	FFV_TEX_S,				// texture matrix
	FFV_TEX_T,
	FFV_TEX_Q,
	FFV_PLANE0,				// texgen planes
	FFV_PLANE1,
	FFV_PLANE2,
	FFV_PLANE3
} dkFixedVertexParm_t;

void		Dk_InitState( void );
void		Dk_ResetState( void );					// after a new command list
void		Dk_BindRenderTarget( void );
void		Dk_BindScreenTarget( void );
void		Dk_SetTargetMetrics( int height, bool flipped );
bool		Dk_BindImageTarget( idImage *color, idImage *depth, int cubeFace );
bool		Dk_TargetIsScreen( void );
void		Dk_ClearRenderTarget( bool clearColor, const float *rgba, bool clearDepth, float depth );
void		Dk_CopyRenderTarget( idRenderTexture *source, idRenderTexture *dest );
void		Dk_SetProgram( const DkShader *vertex, const DkShader *fragment );
void		Dk_SetVertexFormat( dkVertexFormat_t format );
void		Dk_BindVertexBuffer( int slot, DkGpuAddr addr, uint32_t size );
void		Dk_SetTexture( int unit, DkResHandle handle );
void		Dk_SetVsEnv( int index, const float *value );
void		Dk_SetVsLocal( int index, const float *value );
void		Dk_SetFsEnv( int index, const float *value );
void		Dk_SetFsLocal( int index, const float *value );
void		Dk_SetMvp( const float *modelView, const float *projection );
void		Dk_SetTextureMatrix( const float *matrix );
void		Dk_SetViewport( int x, int y, int w, int h, float minDepth, float maxDepth );
void		Dk_SetScissor( int x, int y, int w, int h );
void		Dk_SetDepthTest( bool enable );
void		Dk_SetStencilTest( bool enable );
void		Dk_SetStencilFunc( DkCompareOp func, int ref, int mask );
void		Dk_SetStencilOps( DkStencilOp frontZFail, DkStencilOp frontZPass, DkStencilOp backZFail, DkStencilOp backZPass );
void		Dk_SetPolygonOffset( bool enable, float factor, float units );
void		Dk_SetDepthBounds( bool enable, float zmin, float zmax );
void		Dk_Clear( bool color, const float *rgba, bool depth, bool stencil, int stencilValue );
void		Dk_FlushForDraw( void );
void		Dk_DrawIndexed( DkGpuAddr indexAddr, int numIndexes );
void		Dk_DrawStrip( int numVerts );
void		Dk_ColorTargetBarrier( void );			// before the screen is read

/*
================================================================================
Programs (dk_programs.cpp)
================================================================================
*/
typedef enum {
	DKP_DEPTH,
	DKP_DEPTH_CLIP,
	DKP_DEPTH_ALPHA,
	DKP_STAGE,
	DKP_STAGE_NORMAL_CUBE,
	DKP_STAGE_TEX3_CUBE,
	DKP_STAGE_SCREEN,
	DKP_COLOR,
	DKP_COLOR_CLIP,
	DKP_SHADOW,
	DKP_INTERACTION,
	DKP_ENVIRONMENT,
	DKP_BUMPY_ENVIRONMENT,
	DKP_FOG,
	DKP_BLEND_LIGHT,
	DKP_HEATHAZE,
	DKP_HEATHAZE_MASK,
	DKP_HEATHAZE_MASK_VERTEX,
	DKP_MONOCHROME,
	DKP_COUNT
} dkFixedProgram_t;

void				Dk_InitPrograms( void );
void				Dk_ShutdownPrograms( void );
void				Dk_UseFixedProgram( dkFixedProgram_t program );
const DkShader *	Dk_FixedVertexShader( dkFixedProgram_t program );
const char *		Dk_MaterialProgramName( int programIndex );

/*
================================================================================
Views (dk_draw.cpp)
================================================================================
*/
void	RB_SetDefaultGLState( void );
void	RB_DrawViewCommand( const void *data );

#endif /* !__DK_LOCAL_H__ */
