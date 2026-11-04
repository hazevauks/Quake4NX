/*
===========================================================================

Quake4NX host check: deko3D's interface, behaving as the GPU does where it matters here.

src/renderer/deko3d runs on a PC against this, unchanged. What makes it worth having is
what a GPU does that a plain drawing function would not:

 - Commands are recorded, and only run when their list is submitted.
 - State stays set across command lists and frames, until something changes it.
 - Images live in their memory blocks, at the offsets they were given, so two
   allocations that overlap show it. Descriptors are read from memory when a draw
   needs them, and a block with no CPU access gives the CPU no address.
 - What the debug library would refuse is reported, with some of what a GPU would
   only get wrong silently: an image used with no descriptor, or a draw with nothing bound.

Only the frames a scenario asks a picture of are rasterized. The shaders are
written out again in C++ at the end of this file, by the rules of those in shaders/,
so a change to one has to be made in both. What this cannot show is that the console's
GPU agrees: only a console can.

===========================================================================
*/

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <deko3d.h>

#include "host_scenario.h"

typedef uint8_t byte;

static const int MAX_REPORTED = 40;
static int s_errors = 0;

int FakeDeko3D_Errors( void ) {
	return s_errors;
}

static void Refuse( const char *fmt, ... ) {
	s_errors++;
	if ( s_errors > MAX_REPORTED ) {
		return;
	}
	char text[512];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( text, sizeof( text ), fmt, ap );
	va_end( ap );
	printf( "FAKE DEKO3D ERROR: %s\n", text );
	fflush( stdout );
}

/*
================================================================================
Objects
================================================================================
*/
typedef std::function<void()> Op;

namespace dk { namespace detail {
struct Device { uint32_t flags; };
struct MemBlock { byte *mem; uint32_t size; uint32_t flags; };
struct Queue { int unused; };
struct Swapchain { int numImages; int next; const DkImage *images[8]; };
struct CmdBuf {
	std::vector<Op> ops;
	std::vector<std::vector<Op> *> lists;
};
} }

struct FakeLayout {
	uint32_t	magic;
	DkImageType	type;
	DkImageFormat format;
	uint32_t	flags;
	uint32_t	w, h, layers;
	uint32_t	mips;
	uint64_t	size;
};

struct FakeImage {
	uint32_t	magic;
	FakeLayout	layout;
	byte *		data;
	DkGpuAddr	addr;
};

struct FakeDescriptor {
	uint32_t			magic;
	const FakeImage *	image;
	uint8_t				swizzle[4];
	uint8_t				mipOffset;
};

struct FakeSamplerDesc {
	uint32_t	magic;
	uint8_t		minLinear, magLinear, mipMode;
	uint8_t		wrap[3];
	float		border[4];
};

struct FakeShader {
	uint32_t	magic;
	char		name[64];
};

static const uint32_t MAGIC = 0x46414b45;

static_assert( sizeof( FakeLayout ) <= sizeof( DkImageLayout ), "layout" );
static_assert( sizeof( FakeImage ) <= sizeof( DkImage ), "image" );
static_assert( sizeof( FakeDescriptor ) <= sizeof( DkImageDescriptor ), "descriptor" );
static_assert( sizeof( FakeSamplerDesc ) <= sizeof( DkSamplerDescriptor ), "sampler" );
static_assert( sizeof( FakeShader ) <= sizeof( DkShader ), "shader" );

/*
================================================================================
Formats
================================================================================
*/
static int BlockBytes( DkImageFormat format ) {
	switch ( format ) {
		case DkImageFormat_RGBA_BC1:	return 8;
		case DkImageFormat_RGBA_BC2:
		case DkImageFormat_RGBA_BC3:
		case DkImageFormat_RGBA_BC7_Unorm:	return 16;
		default:						return 0;
	}
}

static int PixelBytes( DkImageFormat format ) {
	switch ( format ) {
		case DkImageFormat_R8_Unorm:		return 1;
		case DkImageFormat_RG8_Unorm:
		case DkImageFormat_R16_Unorm:
		case DkImageFormat_RGB565_Unorm:	return 2;
		case DkImageFormat_RGBA8_Unorm:
		case DkImageFormat_RGBA8_Unorm_sRGB:
		case DkImageFormat_RG16_Unorm:
		case DkImageFormat_Z24X8:
		case DkImageFormat_Z24S8:			return 4;
		case DkImageFormat_RGBA16_Float:	return 8;
		default:
			Refuse( "image format %d is not handled", (int)format );
			return 4;
	}
}

static uint64_t MipBytes( DkImageFormat format, uint32_t w, uint32_t h ) {
	const int block = BlockBytes( format );
	if ( block ) {
		return (uint64_t)( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * block;
	}
	return (uint64_t)w * h * PixelBytes( format );
}

static uint32_t MipDim( uint32_t size, uint32_t mip ) {
	const uint32_t v = size >> mip;
	return v ? v : 1;
}

static uint64_t MipOffset( const FakeLayout &l, uint32_t mip, uint32_t layer ) {
	uint64_t layerBytes = 0;
	for ( uint32_t m = 0; m < l.mips; m++ ) {
		layerBytes += MipBytes( l.format, MipDim( l.w, m ), MipDim( l.h, m ) );
	}
	uint64_t offset = layerBytes * layer;
	for ( uint32_t m = 0; m < mip; m++ ) {
		offset += MipBytes( l.format, MipDim( l.w, m ), MipDim( l.h, m ) );
	}
	return offset;
}

/*
Commands and descriptors outlive the DkImage they were made from. On a GPU they hold what the
image was, not where its object lived, and the renderer rightly deletes the object at once.
*/
static const FakeImage *StableImage( const FakeImage *image ) {
	typedef std::tuple<const byte *, int, int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> imageKey_t;
	static std::map<imageKey_t, const FakeImage *> images;
	const FakeLayout &l = image->layout;
	const FakeImage *&stable = images[imageKey_t( image->data, (int)l.type, (int)l.format, l.flags, l.w, l.h, l.layers, l.mips )];
	if ( !stable ) {
		stable = new FakeImage( *image );
	}
	return stable;
}

static const FakeImage *ImageOf( const DkImageView *view ) {
	const FakeImage *image = (const FakeImage *)view->pImage;
	if ( !image || image->magic != MAGIC ) {
		Refuse( "a view of an image that was not initialized" );
		return NULL;
	}
	return StableImage( image );
}

/*
================================================================================
Device, memory
================================================================================
*/
DkDevice dkDeviceCreate( DkDeviceMaker const *maker ) {
	DkDevice device = new dk::detail::Device;
	device->flags = maker->flags;
	return device;
}

void dkDeviceDestroy( DkDevice obj ) {
	delete obj;
}

DkMemBlock dkMemBlockCreate( DkMemBlockMaker const *maker ) {
	if ( maker->size == 0 || ( maker->size & ( DK_MEMBLOCK_ALIGNMENT - 1 ) ) ) {
		Refuse( "a memory block of %u bytes is not aligned", maker->size );
	}
	DkMemBlock block = new dk::detail::MemBlock;
	block->mem = (byte *)aligned_alloc( DK_MEMBLOCK_ALIGNMENT, maker->size );
	if ( !block->mem ) {
		Refuse( "no memory for a block of %u bytes", maker->size );
		delete block;
		return NULL;
	}
	memset( block->mem, 0xCD, maker->size );
	block->size = maker->size;
	block->flags = maker->flags;
	return block;
}

void dkMemBlockDestroy( DkMemBlock obj ) {
	if ( obj ) {
		free( obj->mem );
		delete obj;
	}
}

void *dkMemBlockGetCpuAddr( DkMemBlock obj ) {
	return ( obj->flags & ( DkMemBlockFlags_CpuUncached | DkMemBlockFlags_CpuCached ) ) ? obj->mem : NULL;
}

DkGpuAddr dkMemBlockGetGpuAddr( DkMemBlock obj ) {
	return (DkGpuAddr)(uintptr_t)obj->mem;
}

uint32_t dkMemBlockGetSize( DkMemBlock obj ) {
	return obj->size;
}

DkResult dkFenceWait( DkFence *obj, int64_t timeout_ns ) {
	return DkResult_Success;	// a list runs when it is submitted
}

/*
================================================================================
Shaders

A .dksh here is a file with the shader's name in it (tools/hostcheck/build.py).
================================================================================
*/
void dkShaderInitialize( DkShader *obj, DkShaderMaker const *maker ) {
	FakeShader *shader = (FakeShader *)obj;
	memset( shader, 0, sizeof( *shader ) );
	const char *name = (const char *)maker->codeMem->mem + maker->codeOffset;
	snprintf( shader->name, sizeof( shader->name ), "%s", name );
	shader->magic = MAGIC;
}

bool dkShaderIsValid( DkShader const *obj ) {
	return ( (const FakeShader *)obj )->magic == MAGIC;
}

/*
================================================================================
Images
================================================================================
*/
void dkImageLayoutInitialize( DkImageLayout *obj, DkImageLayoutMaker const *maker ) {
	FakeLayout *l = (FakeLayout *)obj;
	memset( l, 0, sizeof( *l ) );
	l->magic = MAGIC;
	l->type = maker->type;
	l->format = maker->format;
	l->flags = maker->flags;
	l->w = maker->dimensions[0];
	l->h = maker->dimensions[1];
	l->layers = maker->dimensions[2] ? maker->dimensions[2] : 1;
	l->mips = maker->mipLevels ? maker->mipLevels : 1;
	if ( l->w == 0 || l->h == 0 ) {
		Refuse( "an image with no size" );
	}
	uint32_t maxMips = 1;
	for ( uint32_t size = l->w > l->h ? l->w : l->h; size > 1; size >>= 1 ) {
		maxMips++;
	}
	if ( l->mips > maxMips ) {
		Refuse( "%u levels on a %ux%u image", l->mips, l->w, l->h );
	}
	if ( BlockBytes( l->format ) && ( l->flags & DkImageFlags_UsageRender ) ) {
		Refuse( "a block compressed image cannot be a render target" );
	}
	l->size = MipOffset( *l, 0, l->layers );
}

uint64_t dkImageLayoutGetSize( DkImageLayout const *obj ) {
	return ( (const FakeLayout *)obj )->size;
}

uint32_t dkImageLayoutGetAlignment( DkImageLayout const *obj ) {
	return 512;
}

void dkImageInitialize( DkImage *obj, DkImageLayout const *layout, DkMemBlock memBlock, uint32_t offset ) {
	FakeImage *image = (FakeImage *)obj;
	const FakeLayout *l = (const FakeLayout *)layout;
	if ( l->magic != MAGIC ) {
		Refuse( "an image from a layout that was not initialized" );
	}
	if ( offset + l->size > memBlock->size ) {
		Refuse( "an image of %llu bytes at %u in a block of %u", (unsigned long long)l->size, offset, memBlock->size );
	}
	if ( !( memBlock->flags & DkMemBlockFlags_Image ) ) {
		Refuse( "an image in a memory block without the image flag" );
	}
	image->magic = MAGIC;
	image->layout = *l;
	image->data = memBlock->mem + offset;
	image->addr = dkMemBlockGetGpuAddr( memBlock ) + offset;
}

DkGpuAddr dkImageGetGpuAddr( DkImage const *obj ) {
	return ( (const FakeImage *)obj )->addr;
}

void dkImageDescriptorInitialize( DkImageDescriptor *obj, DkImageView const *view, bool usesLoadOrStore, bool decayMS ) {
	FakeDescriptor *d = (FakeDescriptor *)obj;
	d->magic = MAGIC;
	d->image = ImageOf( view );
	for ( int i = 0; i < 4; i++ ) {
		d->swizzle[i] = (uint8_t)view->swizzle[i];
	}
	d->mipOffset = view->mipLevelOffset;
}

void dkSamplerDescriptorInitialize( DkSamplerDescriptor *obj, DkSampler const *sampler ) {
	FakeSamplerDesc *d = (FakeSamplerDesc *)obj;
	d->magic = MAGIC;
	d->minLinear = sampler->minFilter == DkFilter_Linear;
	d->magLinear = sampler->magFilter == DkFilter_Linear;
	d->mipMode = (uint8_t)sampler->mipFilter;
	for ( int i = 0; i < 3; i++ ) {
		d->wrap[i] = (uint8_t)sampler->wrapMode[i];
	}
	for ( int i = 0; i < 4; i++ ) {
		d->border[i] = sampler->borderColor[i].value_f;
	}
}

/*
================================================================================
Command buffers
================================================================================
*/
DkCmdBuf dkCmdBufCreate( DkCmdBufMaker const *maker ) {
	return new dk::detail::CmdBuf;
}

void dkCmdBufDestroy( DkCmdBuf obj ) {
	for ( size_t i = 0; i < obj->lists.size(); i++ ) {
		delete obj->lists[i];
	}
	delete obj;
}

void dkCmdBufAddMemory( DkCmdBuf obj, DkMemBlock mem, uint32_t offset, uint32_t size ) {
	if ( offset + size > mem->size ) {
		Refuse( "command memory past the end of its block" );
	}
	if ( !( mem->flags & ( DkMemBlockFlags_CpuUncached | DkMemBlockFlags_CpuCached ) ) ) {
		Refuse( "command memory the CPU cannot write" );
	}
}

DkCmdList dkCmdBufFinishList( DkCmdBuf obj ) {
	std::vector<Op> *list = new std::vector<Op>();
	list->swap( obj->ops );
	obj->lists.push_back( list );
	return (DkCmdList)(uintptr_t)list;
}

void dkCmdBufClear( DkCmdBuf obj ) {
	obj->ops.clear();
	for ( size_t i = 0; i < obj->lists.size(); i++ ) {
		delete obj->lists[i];
	}
	obj->lists.clear();
}

void dkCmdBufSignalFence( DkCmdBuf obj, DkFence *fence, bool flush ) {
}

void dkCmdBufBarrier( DkCmdBuf obj, DkBarrier mode, uint32_t invalidateFlags ) {
}

/*
================================================================================
GPU state
================================================================================
*/
struct GpuState {
	DkGpuAddr			imageDescriptors = 0;
	uint32_t			numImageDescriptors = 0;
	DkGpuAddr			samplerDescriptors = 0;
	uint32_t			numSamplerDescriptors = 0;

	const FakeShader *	vertexShader = NULL;
	const FakeShader *	fragmentShader = NULL;

	const FakeImage *	colorTarget = NULL;
	const FakeImage *	depthTarget = NULL;

	DkRasterizerState	rasterizer;
	DkColorState		color;
	DkColorWriteState	colorWrite;
	DkBlendState		blend;
	DkDepthStencilState	depthStencil;

	DkVtxAttribState	attribs[16];
	uint32_t			numAttribs = 0;
	DkVtxBufferState	buffers[16];
	uint32_t			numBuffers = 0;
	DkBufExtents		vtx[16];
	DkGpuAddr			idxAddr = 0;
	DkIdxFormat			idxFormat = DkIdxFormat_Uint32;

	DkBufExtents		ubo[2][4];
	DkResHandle			textures[16];

	DkViewport			viewport;
	DkScissor			scissor;
	float				depthBiasConstant = 0, depthBiasSlope = 0;
	float				alphaRef = 0;
	uint8_t				stencilMask = 0xff, stencilRef = 0, stencilFuncMask = 0xff;
	bool				depthBounds = false;
	float				depthBoundsNear = 0, depthBoundsFar = 1;
};

static GpuState gs;
static bool s_rasterize = false;
static bool s_stateInitialized = false;

// Images drawn into stay as they are for later frames, and the UI only redraws its layers when they change,
// so everything but the screen is always drawn. Only what lands on the screen waits for a picture to be asked for.
static inline bool RasterizingTo( const FakeImage *target ) {
	return s_rasterize || ( target && !( target->layout.flags & DkImageFlags_UsagePresent ) );
}

// what a frame asked for, for a scenario's stats
static int s_statDraws = 0;
static int s_statIndexes = 0;

static void InitGpuState( void ) {
	if ( s_stateInitialized ) {
		return;
	}
	s_stateInitialized = true;
	dkRasterizerStateDefaults( &gs.rasterizer );
	dkColorStateDefaults( &gs.color );
	dkColorWriteStateDefaults( &gs.colorWrite );
	dkBlendStateDefaults( &gs.blend );
	dkDepthStencilStateDefaults( &gs.depthStencil );
	memset( gs.ubo, 0, sizeof( gs.ubo ) );
	memset( gs.vtx, 0, sizeof( gs.vtx ) );
	memset( gs.textures, 0xff, sizeof( gs.textures ) );
	memset( &gs.viewport, 0, sizeof( gs.viewport ) );
	memset( &gs.scissor, 0, sizeof( gs.scissor ) );
}

#define REC( cmd, body ) do { InitGpuState(); ( cmd )->ops.push_back( [=]() { body; } ); } while ( 0 )

void dkCmdBufBindImageDescriptorSet( DkCmdBuf obj, DkGpuAddr setAddr, uint32_t numDescriptors ) {
	REC( obj, gs.imageDescriptors = setAddr; gs.numImageDescriptors = numDescriptors );
}

void dkCmdBufBindSamplerDescriptorSet( DkCmdBuf obj, DkGpuAddr setAddr, uint32_t numDescriptors ) {
	REC( obj, gs.samplerDescriptors = setAddr; gs.numSamplerDescriptors = numDescriptors );
}

void dkCmdBufBindShaders( DkCmdBuf obj, uint32_t stageMask, DkShader const *const shaders[], uint32_t numShaders ) {
	const FakeShader *a = numShaders > 0 ? (const FakeShader *)shaders[0] : NULL;
	const FakeShader *b = numShaders > 1 ? (const FakeShader *)shaders[1] : NULL;
	REC( obj, gs.vertexShader = a; gs.fragmentShader = b );
}

void dkCmdBufBindRenderTargets( DkCmdBuf obj, DkImageView const *const colorTargets[], uint32_t numColorTargets, DkImageView const *depthTarget ) {
	const FakeImage *color = numColorTargets ? ImageOf( colorTargets[0] ) : NULL;
	const FakeImage *depth = depthTarget ? ImageOf( depthTarget ) : NULL;
	if ( color && !( color->layout.flags & DkImageFlags_UsageRender ) ) {
		Refuse( "a color target without the render usage" );
	}
	if ( color && depth && ( color->layout.w != depth->layout.w || color->layout.h != depth->layout.h ) ) {
		Refuse( "color and depth targets of different sizes" );
	}
	REC( obj, gs.colorTarget = color; gs.depthTarget = depth );
}

void dkCmdBufBindRasterizerState( DkCmdBuf obj, DkRasterizerState const *state ) {
	const DkRasterizerState v = *state;
	REC( obj, gs.rasterizer = v );
}

void dkCmdBufBindMultisampleState( DkCmdBuf obj, DkMultisampleState const *state ) {
}

void dkCmdBufBindColorState( DkCmdBuf obj, DkColorState const *state ) {
	const DkColorState v = *state;
	REC( obj, gs.color = v );
}

void dkCmdBufBindColorWriteState( DkCmdBuf obj, DkColorWriteState const *state ) {
	const DkColorWriteState v = *state;
	REC( obj, gs.colorWrite = v );
}

void dkCmdBufBindBlendStates( DkCmdBuf obj, uint32_t firstId, DkBlendState const states[], uint32_t numStates ) {
	const DkBlendState v = states[0];
	REC( obj, gs.blend = v );
}

void dkCmdBufBindDepthStencilState( DkCmdBuf obj, DkDepthStencilState const *state ) {
	const DkDepthStencilState v = *state;
	REC( obj, gs.depthStencil = v );
}

void dkCmdBufBindVtxAttribState( DkCmdBuf obj, DkVtxAttribState const attribs[], uint32_t numAttribs ) {
	std::vector<DkVtxAttribState> v( attribs, attribs + numAttribs );
	REC( obj, memcpy( gs.attribs, v.data(), v.size() * sizeof( DkVtxAttribState ) ); gs.numAttribs = (uint32_t)v.size() );
}

void dkCmdBufBindVtxBufferState( DkCmdBuf obj, DkVtxBufferState const buffers[], uint32_t numBuffers ) {
	std::vector<DkVtxBufferState> v( buffers, buffers + numBuffers );
	REC( obj, memcpy( gs.buffers, v.data(), v.size() * sizeof( DkVtxBufferState ) ); gs.numBuffers = (uint32_t)v.size() );
}

void dkCmdBufBindVtxBuffers( DkCmdBuf obj, uint32_t firstId, DkBufExtents const buffers[], uint32_t numBuffers ) {
	std::vector<DkBufExtents> v( buffers, buffers + numBuffers );
	REC( obj, for ( size_t i = 0; i < v.size(); i++ ) { gs.vtx[firstId + i] = v[i]; } );
}

void dkCmdBufBindIdxBuffer( DkCmdBuf obj, DkIdxFormat format, DkGpuAddr address ) {
	if ( address == 0 ) {
		Refuse( "an index buffer at address 0" );
	}
	REC( obj, gs.idxFormat = format; gs.idxAddr = address );
}

void dkCmdBufBindUniformBuffers( DkCmdBuf obj, DkStage stage, uint32_t firstId, DkBufExtents const buffers[], uint32_t numBuffers ) {
	std::vector<DkBufExtents> v( buffers, buffers + numBuffers );
	const int s = stage == DkStage_Fragment ? 1 : 0;
	REC( obj, for ( size_t i = 0; i < v.size(); i++ ) { gs.ubo[s][firstId + i] = v[i]; } );
}

void dkCmdBufBindTextures( DkCmdBuf obj, DkStage stage, uint32_t firstId, DkResHandle const handles[], uint32_t numHandles ) {
	if ( stage != DkStage_Fragment ) {
		Refuse( "a texture on a stage other than the fragment one" );
	}
	std::vector<DkResHandle> v( handles, handles + numHandles );
	REC( obj, for ( size_t i = 0; i < v.size(); i++ ) { gs.textures[firstId + i] = v[i]; } );
}

void dkCmdBufSetViewports( DkCmdBuf obj, uint32_t firstId, DkViewport const viewports[], uint32_t numViewports ) {
	const DkViewport v = viewports[0];
	REC( obj, gs.viewport = v );
}

void dkCmdBufSetScissors( DkCmdBuf obj, uint32_t firstId, DkScissor const scissors[], uint32_t numScissors ) {
	const DkScissor v = scissors[0];
	REC( obj, gs.scissor = v );
}

void dkCmdBufSetDepthBias( DkCmdBuf obj, float constantFactor, float clamp, float slopeFactor ) {
	REC( obj, gs.depthBiasConstant = constantFactor; gs.depthBiasSlope = slopeFactor );
}

void dkCmdBufSetDepthBounds( DkCmdBuf obj, bool enable, float nearValue, float farValue ) {
	REC( obj, gs.depthBounds = enable; gs.depthBoundsNear = nearValue; gs.depthBoundsFar = farValue );
}

void dkCmdBufSetAlphaRef( DkCmdBuf obj, float ref ) {
	REC( obj, gs.alphaRef = ref );
}

void dkCmdBufSetStencil( DkCmdBuf obj, DkFace face, uint8_t mask, uint8_t funcRef, uint8_t funcMask ) {
	REC( obj, gs.stencilMask = mask; gs.stencilRef = funcRef; gs.stencilFuncMask = funcMask );
}

void dkCmdBufPushConstants( DkCmdBuf obj, DkGpuAddr uboAddr, uint32_t uboSize, uint32_t offset, uint32_t size, const void *data ) {
	if ( offset + size > uboSize ) {
		Refuse( "pushing %u bytes at %u into a uniform buffer of %u", size, offset, uboSize );
	}
	std::vector<byte> v( (const byte *)data, (const byte *)data + size );
	REC( obj, memcpy( (byte *)(uintptr_t)uboAddr + offset, v.data(), v.size() ) );
}

void dkCmdBufDiscardDepthStencil( DkCmdBuf obj ) {
}

/*
================================================================================
Copies
================================================================================
*/
void dkCmdBufCopyBufferToImage( DkCmdBuf obj, DkCopyBuf const *src, DkImageView const *dstView, DkImageRect const *dstRect, uint32_t flags ) {
	const FakeImage *image = ImageOf( dstView );
	if ( !image ) {
		return;
	}
	const DkCopyBuf buffer = *src;
	const DkImageRect rect = *dstRect;
	const uint32_t mip = dstView->mipLevelOffset;
	const uint32_t layer = dstView->layerOffset;
	const uint32_t w = MipDim( image->layout.w, mip );
	const uint32_t h = MipDim( image->layout.h, mip );
	if ( rect.x + rect.width > ( w + 3 ) && BlockBytes( image->layout.format ) ) {
		Refuse( "a block upload past the edge of its level" );
	}
	if ( !BlockBytes( image->layout.format ) && ( rect.x + rect.width > w || rect.y + rect.height > h ) ) {
		Refuse( "an upload of %ux%u at %u,%u to a level of %ux%u", rect.width, rect.height, rect.x, rect.y, w, h );
	}
	if ( mip >= image->layout.mips ) {
		Refuse( "an upload to level %u of an image of %u", mip, image->layout.mips );
	}
	REC( obj,
		const int block = BlockBytes( image->layout.format );
		const uint32_t rowBytes = block ? ( ( rect.width + 3 ) / 4 ) * block : rect.width * PixelBytes( image->layout.format );
		const uint32_t rows = block ? ( rect.height + 3 ) / 4 : rect.height;
		const uint32_t stride = block ? ( ( w + 3 ) / 4 ) * block : w * PixelBytes( image->layout.format );
		const uint32_t px = block ? rect.x / 4 : rect.x;
		const uint32_t py = block ? rect.y / 4 : rect.y;
		const uint32_t pixelBytes = block ? block : PixelBytes( image->layout.format );
		byte *dst = image->data + MipOffset( image->layout, mip, layer ) + py * stride + px * pixelBytes;
		const byte *from = (const byte *)(uintptr_t)buffer.addr;
		for ( uint32_t row = 0; row < rows; row++ ) {
			memcpy( dst + row * stride, from + row * ( buffer.rowLength ? buffer.rowLength : rowBytes ), rowBytes );
		}
	);
}

void dkCmdBufBlitImage( DkCmdBuf obj, DkImageView const *srcView, DkImageRect const *srcRect, DkImageView const *dstView, DkImageRect const *dstRect, uint32_t flags, uint32_t factor ) {
	const FakeImage *from = ImageOf( srcView );
	const FakeImage *to = ImageOf( dstView );
	if ( !from || !to ) {
		return;
	}
	if ( !( from->layout.flags & DkImageFlags_Usage2DEngine ) || !( to->layout.flags & DkImageFlags_Usage2DEngine ) ) {
		Refuse( "a blit of an image without the 2D engine usage" );
	}
	const DkImageRect s = *srcRect;
	const DkImageRect d = *dstRect;
	if ( s.x + s.width > from->layout.w || s.y + s.height > from->layout.h ) {
		Refuse( "a blit from outside its source: %u,%u %ux%u of %ux%u", s.x, s.y, s.width, s.height, from->layout.w, from->layout.h );
	}
	REC( obj,
		if ( RasterizingTo( from ) && PixelBytes( from->layout.format ) == 4 && PixelBytes( to->layout.format ) == 4 ) {
			const uint32_t stride = from->layout.w * 4;
			for ( uint32_t y = 0; y < d.height; y++ ) {
				const uint32_t sy = s.y + y * s.height / d.height;
				const uint32_t ry = ( flags & DkBlitFlag_FlipY ) ? ( s.y + s.height - 1 - ( sy - s.y ) ) : sy;
				byte *dst = to->data + ( d.y + y ) * to->layout.w * 4 + d.x * 4;
				for ( uint32_t x = 0; x < d.width; x++ ) {
					const uint32_t sx = s.x + x * s.width / d.width;
					memcpy( dst + x * 4, from->data + ry * stride + sx * 4, 4 );
				}
			}
		}
	);
}

void dkCmdBufCopyImageToBuffer( DkCmdBuf obj, DkImageView const *srcView, DkImageRect const *srcRect, DkCopyBuf const *dst, uint32_t flags ) {
	const FakeImage *from = ImageOf( srcView );
	if ( !from ) {
		return;
	}
	const DkImageRect s = *srcRect;
	const DkCopyBuf buffer = *dst;
	REC( obj,
		for ( uint32_t y = 0; y < s.height; y++ ) {
			memcpy( (byte *)(uintptr_t)buffer.addr + y * buffer.rowLength, from->data + ( s.y + y ) * from->layout.w * 4 + s.x * 4, s.width * 4 );
		}
	);
}

/*
================================================================================
Queue, swapchain
================================================================================
*/
DkQueue dkQueueCreate( DkQueueMaker const *maker ) {
	return new dk::detail::Queue;
}

void dkQueueDestroy( DkQueue obj ) {
	delete obj;
}

bool dkQueueIsInErrorState( DkQueue obj ) {
	return false;
}

void dkQueueSignalFence( DkQueue obj, DkFence *fence, bool flush ) {
}

void dkQueueSubmitCommands( DkQueue obj, DkCmdList cmds ) {
	std::vector<Op> *list = (std::vector<Op> *)(uintptr_t)cmds;
	for ( size_t i = 0; i < list->size(); i++ ) {
		( *list )[i]();
	}
}

void dkQueueFlush( DkQueue obj ) {
}

void dkQueueWaitIdle( DkQueue obj ) {
}

DkSwapchain dkSwapchainCreate( DkSwapchainMaker const *maker ) {
	DkSwapchain swapchain = new dk::detail::Swapchain;
	swapchain->numImages = maker->numImages;
	swapchain->next = 0;
	for ( uint32_t i = 0; i < maker->numImages; i++ ) {
		swapchain->images[i] = maker->pImages[i];
		const FakeImage *image = (const FakeImage *)maker->pImages[i];
		if ( !( image->layout.flags & DkImageFlags_UsagePresent ) ) {
			Refuse( "a swapchain image without the present usage" );
		}
	}
	return swapchain;
}

void dkSwapchainDestroy( DkSwapchain obj ) {
	delete obj;
}

int dkQueueAcquireImage( DkQueue obj, DkSwapchain swapchain ) {
	const int slot = swapchain->next;
	swapchain->next = ( swapchain->next + 1 ) % swapchain->numImages;
	s_rasterize = Host_ShotPending();
	s_statDraws = 0;
	s_statIndexes = 0;
	return slot;
}

static void WritePng( const char *path, const byte *rgba, uint32_t w, uint32_t h );

void dkQueuePresentImage( DkQueue obj, DkSwapchain swapchain, int imageSlot ) {
	const char *shot = s_rasterize ? Host_TakeShotName() : NULL;
	if ( shot ) {
		const FakeImage *image = (const FakeImage *)swapchain->images[imageSlot];
		std::string path = std::string( Host_OutputDir() ) + "/" + shot + ".png";
		WritePng( path.c_str(), image->data, image->layout.w, image->layout.h );
		printf( "picture %s: %i draws, %i indexes\n", path.c_str(), s_statDraws, s_statIndexes );
		fflush( stdout );
	}
	Host_FrameStats( s_statDraws, s_statIndexes );
}

/*
================================================================================
PNG
================================================================================
*/
static uint32_t Crc32( const byte *data, size_t size, uint32_t crc = 0 ) {
	static uint32_t table[256];
	if ( !table[1] ) {
		for ( uint32_t i = 0; i < 256; i++ ) {
			uint32_t c = i;
			for ( int k = 0; k < 8; k++ ) {
				c = ( c & 1 ) ? 0xedb88320u ^ ( c >> 1 ) : c >> 1;
			}
			table[i] = c;
		}
	}
	crc = ~crc;
	for ( size_t i = 0; i < size; i++ ) {
		crc = table[( crc ^ data[i] ) & 0xff] ^ ( crc >> 8 );
	}
	return ~crc;
}

static void PngChunk( FILE *f, const char *type, const std::vector<byte> &data ) {
	const byte length[4] = { (byte)( data.size() >> 24 ), (byte)( data.size() >> 16 ), (byte)( data.size() >> 8 ), (byte)data.size() };
	fwrite( length, 1, 4, f );
	std::vector<byte> all( type, type + 4 );
	all.insert( all.end(), data.begin(), data.end() );
	fwrite( all.data(), 1, all.size(), f );
	const uint32_t crc = Crc32( all.data(), all.size() );
	const byte tail[4] = { (byte)( crc >> 24 ), (byte)( crc >> 16 ), (byte)( crc >> 8 ), (byte)crc };
	fwrite( tail, 1, 4, f );
}

static void WritePng( const char *path, const byte *rgba, uint32_t w, uint32_t h ) {
	FILE *f = fopen( path, "wb" );
	if ( !f ) {
		Refuse( "could not write the picture %s", path );
		return;
	}
	static const byte signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite( signature, 1, 8, f );
	std::vector<byte> ihdr = { (byte)( w >> 24 ), (byte)( w >> 16 ), (byte)( w >> 8 ), (byte)w, (byte)( h >> 24 ), (byte)( h >> 16 ), (byte)( h >> 8 ), (byte)h, 8, 2, 0, 0, 0 };
	PngChunk( f, "IHDR", ihdr );

	std::vector<byte> raw;
	raw.reserve( ( w * 3 + 1 ) * h );
	for ( uint32_t y = 0; y < h; y++ ) {
		raw.push_back( 0 );
		for ( uint32_t x = 0; x < w; x++ ) {
			const byte *p = rgba + ( y * w + x ) * 4;
			raw.push_back( p[0] );
			raw.push_back( p[1] );
			raw.push_back( p[2] );
		}
	}
	// stored deflate blocks: larger files, nothing to link
	std::vector<byte> z = { 0x78, 0x01 };
	uint32_t a = 1, b = 0;
	for ( size_t i = 0; i < raw.size(); i++ ) {
		a = ( a + raw[i] ) % 65521;
		b = ( b + a ) % 65521;
	}
	for ( size_t offset = 0; offset < raw.size(); ) {
		const size_t count = raw.size() - offset < 65535 ? raw.size() - offset : 65535;
		z.push_back( offset + count == raw.size() ? 1 : 0 );
		z.push_back( (byte)count );
		z.push_back( (byte)( count >> 8 ) );
		z.push_back( (byte)~count );
		z.push_back( (byte)( ( ~count ) >> 8 ) );
		z.insert( z.end(), raw.begin() + offset, raw.begin() + offset + count );
		offset += count;
	}
	const uint32_t adler = ( b << 16 ) | a;
	z.push_back( (byte)( adler >> 24 ) );
	z.push_back( (byte)( adler >> 16 ) );
	z.push_back( (byte)( adler >> 8 ) );
	z.push_back( (byte)adler );
	PngChunk( f, "IDAT", z );
	PngChunk( f, "IEND", std::vector<byte>() );
	fclose( f );
}

/*
================================================================================
Sampling
================================================================================
*/
static inline float Saturate( float v ) {
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

static void DecodeBc1Colors( const byte *block, byte colors[4][4], bool bc1 ) {
	const uint16_t c0 = block[0] | ( block[1] << 8 );
	const uint16_t c1 = block[2] | ( block[3] << 8 );
	const int r0 = ( ( c0 >> 11 ) & 31 ) * 255 / 31, g0 = ( ( c0 >> 5 ) & 63 ) * 255 / 63, b0 = ( c0 & 31 ) * 255 / 31;
	const int r1 = ( ( c1 >> 11 ) & 31 ) * 255 / 31, g1 = ( ( c1 >> 5 ) & 63 ) * 255 / 63, b1 = ( c1 & 31 ) * 255 / 31;
	colors[0][0] = r0; colors[0][1] = g0; colors[0][2] = b0; colors[0][3] = 255;
	colors[1][0] = r1; colors[1][1] = g1; colors[1][2] = b1; colors[1][3] = 255;
	if ( !bc1 || c0 > c1 ) {
		colors[2][0] = ( 2 * r0 + r1 ) / 3; colors[2][1] = ( 2 * g0 + g1 ) / 3; colors[2][2] = ( 2 * b0 + b1 ) / 3; colors[2][3] = 255;
		colors[3][0] = ( r0 + 2 * r1 ) / 3; colors[3][1] = ( g0 + 2 * g1 ) / 3; colors[3][2] = ( b0 + 2 * b1 ) / 3; colors[3][3] = 255;
	} else {
		colors[2][0] = ( r0 + r1 ) / 2; colors[2][1] = ( g0 + g1 ) / 2; colors[2][2] = ( b0 + b1 ) / 2; colors[2][3] = 255;
		colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
	}
}

static void ReadMipTexel( const FakeImage *image, uint32_t mip, uint32_t layer, uint32_t x, uint32_t y, float out[4] ) {
	const FakeLayout &l = image->layout;
	const uint32_t w = MipDim( l.w, mip );
	const byte *base = image->data + MipOffset( l, mip, layer );
	const int block = BlockBytes( l.format );
	if ( block ) {
		const byte *b = base + ( y / 4 ) * ( ( w + 3 ) / 4 ) * block + ( x / 4 ) * block;
		const uint32_t px = x & 3, py = y & 3;
		byte colors[4][4];
		if ( block == 8 ) {
			DecodeBc1Colors( b, colors, true );
			const uint32_t idx = ( b[4 + py] >> ( px * 2 ) ) & 3;
			out[0] = colors[idx][0] / 255.0f;
			out[1] = colors[idx][1] / 255.0f;
			out[2] = colors[idx][2] / 255.0f;
			out[3] = colors[idx][3] / 255.0f;
			return;
		}
		DecodeBc1Colors( b + 8, colors, false );
		const uint32_t idx = ( b[12 + py] >> ( px * 2 ) ) & 3;
		out[0] = colors[idx][0] / 255.0f;
		out[1] = colors[idx][1] / 255.0f;
		out[2] = colors[idx][2] / 255.0f;
		if ( l.format == DkImageFormat_RGBA_BC3 ) {
			const int a0 = b[0], a1 = b[1];
			const uint64_t bits = (uint64_t)b[2] | ( (uint64_t)b[3] << 8 ) | ( (uint64_t)b[4] << 16 ) | ( (uint64_t)b[5] << 24 ) | ( (uint64_t)b[6] << 32 ) | ( (uint64_t)b[7] << 40 );
			const int code = (int)( ( bits >> ( 3 * ( py * 4 + px ) ) ) & 7 );
			int alpha;
			if ( code == 0 ) {
				alpha = a0;
			} else if ( code == 1 ) {
				alpha = a1;
			} else if ( a0 > a1 ) {
				alpha = ( ( 8 - code ) * a0 + ( code - 1 ) * a1 ) / 7;
			} else if ( code == 6 ) {
				alpha = 0;
			} else if ( code == 7 ) {
				alpha = 255;
			} else {
				alpha = ( ( 6 - code ) * a0 + ( code - 1 ) * a1 ) / 5;
			}
			out[3] = alpha / 255.0f;
		} else {
			out[3] = ( ( b[py * 2 + ( px >> 1 )] >> ( ( px & 1 ) * 4 ) ) & 15 ) / 15.0f;
		}
		return;
	}
	const byte *p = base + ( y * w + x ) * PixelBytes( l.format );
	switch ( l.format ) {
		case DkImageFormat_RGBA8_Unorm:
		case DkImageFormat_RGBA8_Unorm_sRGB:
			out[0] = p[0] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[2] / 255.0f; out[3] = p[3] / 255.0f;
			break;
		case DkImageFormat_R8_Unorm:
			out[0] = p[0] / 255.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
			break;
		case DkImageFormat_RG8_Unorm:
			out[0] = p[0] / 255.0f; out[1] = p[1] / 255.0f; out[2] = 0.0f; out[3] = 1.0f;
			break;
		case DkImageFormat_RGB565_Unorm: {
			const uint16_t v = p[0] | ( p[1] << 8 );
			out[0] = ( ( v >> 11 ) & 31 ) / 31.0f; out[1] = ( ( v >> 5 ) & 63 ) / 63.0f; out[2] = ( v & 31 ) / 31.0f; out[3] = 1.0f;
			break;
		}
		default:
			out[0] = out[1] = out[2] = out[3] = 0.0f;
			break;
	}
}

// A level from how far a unit's coordinates moved since the pixel to the left: enough for a picture to check by eye.
static const int MAX_UNITS = 16;
static int		s_fragX = 0, s_fragY = 0;
static int		s_unitX[MAX_UNITS], s_unitY[MAX_UNITS];
static float	s_unitU[MAX_UNITS], s_unitV[MAX_UNITS], s_unitLod[MAX_UNITS];

static int UnitLevel( int unit, float u, float v, int width, int height ) {
	if ( s_unitY[unit] == s_fragY && s_unitX[unit] == s_fragX - 1 ) {
		const float du = fabsf( u - s_unitU[unit] ) * width;
		const float dv = fabsf( v - s_unitV[unit] ) * height;
		const float texels = du > dv ? du : dv;
		s_unitLod[unit] = texels > 1.0f ? log2f( texels ) : 0.0f;
	}
	s_unitX[unit] = s_fragX;
	s_unitY[unit] = s_fragY;
	s_unitU[unit] = u;
	s_unitV[unit] = v;
	return (int)s_unitLod[unit];
}

static inline int WrapCoord( int v, int size, int mode, bool &border ) {
	switch ( mode ) {
		case DkWrapMode_Repeat:
			v %= size;
			return v < 0 ? v + size : v;
		case DkWrapMode_MirroredRepeat: {
			v %= 2 * size;
			if ( v < 0 ) {
				v += 2 * size;
			}
			return v < size ? v : 2 * size - 1 - v;
		}
		case DkWrapMode_ClampToBorder:
			if ( v < 0 || v >= size ) {
				border = true;
			}
			return v < 0 ? 0 : ( v >= size ? size - 1 : v );
		default:
			return v < 0 ? 0 : ( v >= size ? size - 1 : v );
	}
}

static void SwizzleTexel( const FakeDescriptor *d, const float in[4], float out[4] ) {
	for ( int i = 0; i < 4; i++ ) {
		switch ( d->swizzle[i] ) {
			case DkImageSwizzle_Zero:	out[i] = 0.0f; break;
			case DkImageSwizzle_One:	out[i] = 1.0f; break;
			case DkImageSwizzle_Red:	out[i] = in[0]; break;
			case DkImageSwizzle_Green:	out[i] = in[1]; break;
			case DkImageSwizzle_Blue:	out[i] = in[2]; break;
			default:					out[i] = in[3]; break;
		}
	}
}

static bool ResolveHandle( int unit, const FakeDescriptor *&image, const FakeSamplerDesc *&sampler ) {
	const DkResHandle handle = gs.textures[unit];
	if ( handle == 0xffffffffu ) {
		Refuse( "shader %s samples unit %i with no texture bound", gs.fragmentShader ? gs.fragmentShader->name : "?", unit );
		return false;
	}
	const uint32_t imageId = handle & ( ( 1u << 20 ) - 1 );
	const uint32_t samplerId = handle >> 20;
	if ( imageId >= gs.numImageDescriptors || samplerId >= gs.numSamplerDescriptors ) {
		Refuse( "a texture handle out of range: image %u, sampler %u", imageId, samplerId );
		return false;
	}
	image = (const FakeDescriptor *)( (const byte *)(uintptr_t)gs.imageDescriptors + imageId * sizeof( DkImageDescriptor ) );
	sampler = (const FakeSamplerDesc *)( (const byte *)(uintptr_t)gs.samplerDescriptors + samplerId * sizeof( DkSamplerDescriptor ) );
	if ( image->magic != MAGIC || sampler->magic != MAGIC ) {
		Refuse( "unit %i of %s uses a descriptor that was never written", unit, gs.fragmentShader ? gs.fragmentShader->name : "?" );
		return false;
	}
	return true;
}

static void SampleLevel( const FakeDescriptor *d, const FakeSamplerDesc *smp, uint32_t mip, uint32_t layer, float u, float v, float out[4] ) {
	const FakeLayout &l = d->image->layout;
	const int w = (int)MipDim( l.w, mip );
	const int h = (int)MipDim( l.h, mip );
	const bool linear = smp->magLinear;
	if ( !linear ) {
		bool border = false;
		const int x = WrapCoord( (int)floorf( u * w ), w, smp->wrap[0], border );
		const int y = WrapCoord( (int)floorf( v * h ), h, smp->wrap[1], border );
		if ( border ) {
			memcpy( out, smp->border, sizeof( float ) * 4 );
			return;
		}
		ReadMipTexel( d->image, mip, layer, x, y, out );
		return;
	}
	const float fx = u * w - 0.5f;
	const float fy = v * h - 0.5f;
	const int x0 = (int)floorf( fx );
	const int y0 = (int)floorf( fy );
	const float ax = fx - x0;
	const float ay = fy - y0;
	float t[4][4];
	for ( int i = 0; i < 4; i++ ) {
		bool border = false;
		const int x = WrapCoord( x0 + ( i & 1 ), w, smp->wrap[0], border );
		const int y = WrapCoord( y0 + ( i >> 1 ), h, smp->wrap[1], border );
		if ( border ) {
			memcpy( t[i], smp->border, sizeof( float ) * 4 );
		} else {
			ReadMipTexel( d->image, mip, layer, x, y, t[i] );
		}
	}
	for ( int c = 0; c < 4; c++ ) {
		out[c] = ( t[0][c] * ( 1 - ax ) + t[1][c] * ax ) * ( 1 - ay ) + ( t[2][c] * ( 1 - ax ) + t[3][c] * ax ) * ay;
	}
}

static void Texture2D( int unit, float u, float v, float out[4] ) {
	const FakeDescriptor *d;
	const FakeSamplerDesc *smp;
	if ( !ResolveHandle( unit, d, smp ) ) {
		out[0] = 1.0f; out[1] = 0.0f; out[2] = 1.0f; out[3] = 1.0f;
		return;
	}
	if ( d->image->layout.type == DkImageType_Cubemap ) {
		Refuse( "a cube map sampled as 2D by %s, unit %i", gs.fragmentShader ? gs.fragmentShader->name : "?", unit );
	}
	uint32_t mip = d->mipOffset;
	if ( smp->mipMode != DkMipFilter_None && d->image->layout.mips > 1 ) {
		mip += (uint32_t)UnitLevel( unit, u, v, (int)d->image->layout.w, (int)d->image->layout.h );
		if ( mip >= d->image->layout.mips ) {
			mip = d->image->layout.mips - 1;
		}
	}
	float t[4];
	SampleLevel( d, smp, mip, 0, u, v, t );
	SwizzleTexel( d, t, out );
}

static void TextureCube( int unit, const float dir[3], float out[4] ) {
	const FakeDescriptor *d;
	const FakeSamplerDesc *smp;
	if ( !ResolveHandle( unit, d, smp ) ) {
		out[0] = 1.0f; out[1] = 0.0f; out[2] = 1.0f; out[3] = 1.0f;
		return;
	}
	if ( d->image->layout.type != DkImageType_Cubemap ) {
		Refuse( "a 2D image sampled as a cube map by %s, unit %i", gs.fragmentShader ? gs.fragmentShader->name : "?", unit );
		out[0] = 1.0f; out[1] = 0.0f; out[2] = 1.0f; out[3] = 1.0f;
		return;
	}
	const float ax = fabsf( dir[0] ), ay = fabsf( dir[1] ), az = fabsf( dir[2] );
	uint32_t face;
	float s, t, m;
	if ( ax >= ay && ax >= az ) {
		face = dir[0] > 0 ? 0 : 1; m = ax; s = dir[0] > 0 ? -dir[2] : dir[2]; t = -dir[1];
	} else if ( ay >= az ) {
		face = dir[1] > 0 ? 2 : 3; m = ay; s = dir[0]; t = dir[1] > 0 ? dir[2] : -dir[2];
	} else {
		face = dir[2] > 0 ? 4 : 5; m = az; s = dir[2] > 0 ? dir[0] : -dir[0]; t = -dir[1];
	}
	if ( m <= 0.0f ) {
		m = 1.0f;
	}
	float texel[4];
	SampleLevel( d, smp, 0, face, s / m * 0.5f + 0.5f, t / m * 0.5f + 0.5f, texel );
	SwizzleTexel( d, texel, out );
}

/*
================================================================================
Shaders, by the rules of those in shaders/
================================================================================
*/
struct Vec4 {
	float x, y, z, w;
};

static inline float Dot4( const Vec4 &a, const float *b ) {
	return a.x * b[0] + a.y * b[1] + a.z * b[2] + a.w * b[3];
}

static inline float Dot3( const float *a, const float *b ) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void Normalize3( float *v ) {
	const float length = sqrtf( Dot3( v, v ) );
	if ( length > 0.0f ) {
		v[0] /= length; v[1] /= length; v[2] /= length;
	}
}

struct VertexIn {
	float position[4];		// attribute 0, w is 1 unless the format has one
	float texCoord[3];		// attribute 1
	float normal[3];		// attribute 2
	float tangent[3];		// attribute 3
	float bitangent[3];		// attribute 4
	float color[4];			// attribute 5
};

static const int MAX_VARYING = 7;
struct VertexOut {
	float clip[4];
	float v[MAX_VARYING][4];
	float clipDistance;
};

static const float *VsUbo( int binding ) {
	return (const float *)(uintptr_t)gs.ubo[0][binding].addr;
}
static const float *FsUbo( int binding ) {
	return (const float *)(uintptr_t)gs.ubo[1][binding].addr;
}

static inline void Transform( const float *p, float out[4] ) {
	const float *mvp = VsUbo( 0 );
	const Vec4 v = { p[0], p[1], p[2], p[3] };
	out[0] = Dot4( v, mvp + 0 );
	out[1] = Dot4( v, mvp + 4 );
	out[2] = Dot4( v, mvp + 8 );
	out[3] = Dot4( v, mvp + 12 );
}

enum {
	L_COLOR = 0, L_COLOR_MODE, L_TEX_S, L_TEX_T, L_TEX_Q, L_PLANE0, L_PLANE1, L_PLANE2, L_PLANE3
};

static inline const float *Local( int i ) {
	return VsUbo( 2 ) + i * 4;
}
static inline const float *Env( int i ) {
	return VsUbo( 1 ) + i * 4;
}

static void StageColor( const VertexIn &in, float out[4] ) {
	const float *mode = Local( L_COLOR_MODE );
	const float *color = Local( L_COLOR );
	out[0] = ( in.color[0] * mode[0] + mode[1] ) * color[0];
	out[1] = ( in.color[1] * mode[0] + mode[1] ) * color[1];
	out[2] = ( in.color[2] * mode[0] + mode[1] ) * color[2];
	out[3] = ( in.color[3] * mode[2] + mode[3] ) * color[3];
}

typedef void ( *VertexFn )( const VertexIn &in, VertexOut &out );
typedef bool ( *FragmentFn )( const float v[MAX_VARYING][4], const float fragCoord[2], float out[4] );

#define VS( name ) static void VS_##name( const VertexIn &in, VertexOut &out )
#define FS( name ) static bool FS_##name( const float v[MAX_VARYING][4], const float fragCoord[2], float out[4] )

VS( plain ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
}

VS( depth_clip ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
	const Vec4 pv = { p[0], p[1], p[2], p[3] };
	out.clipDistance = Dot4( pv, Local( L_PLANE0 ) );
}

VS( depth_alpha ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	const Vec4 t = { in.texCoord[0], in.texCoord[1], 0.0f, 1.0f };
	Transform( p, out.clip );
	out.v[0][0] = Dot4( t, Local( L_TEX_S ) );
	out.v[0][1] = Dot4( t, Local( L_TEX_T ) );
	const Vec4 pv = { p[0], p[1], p[2], p[3] };
	out.v[1][0] = Dot4( pv, Local( L_PLANE0 ) );
}

VS( shadow ) {
	const float *light = Env( 4 );
	const float w = in.position[3];
	const float p[4] = { in.position[0] - ( 1.0f - w ) * light[0], in.position[1] - ( 1.0f - w ) * light[1], in.position[2] - ( 1.0f - w ) * light[2], w };
	Transform( p, out.clip );
}

VS( stage ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	const Vec4 t = { in.texCoord[0], in.texCoord[1], 0.0f, 1.0f };
	Transform( p, out.clip );
	out.v[0][0] = Dot4( t, Local( L_TEX_S ) );
	out.v[0][1] = Dot4( t, Local( L_TEX_T ) );
	StageColor( in, out.v[1] );
}

VS( stage_normal_cube ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
	memcpy( out.v[0], in.normal, sizeof( float ) * 3 );
	StageColor( in, out.v[1] );
}

VS( stage_tex3_cube ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
	memcpy( out.v[0], in.texCoord, sizeof( float ) * 3 );
	StageColor( in, out.v[1] );
}

VS( stage_screen ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
	const Vec4 t = { out.clip[0], out.clip[1], 0.0f, out.clip[3] };
	out.v[0][0] = Dot4( t, Local( L_TEX_S ) );
	out.v[0][1] = Dot4( t, Local( L_TEX_T ) );
	out.v[0][2] = Dot4( t, Local( L_TEX_Q ) );
	StageColor( in, out.v[1] );
}

VS( interaction ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	const Vec4 pv = { p[0], p[1], p[2], p[3] };
	const Vec4 t = { in.texCoord[0], in.texCoord[1], 0.0f, 1.0f };
	Transform( p, out.clip );

	float toLight[3] = { Env( 4 )[0] - p[0], Env( 4 )[1] - p[1], Env( 4 )[2] - p[2] };
	float toEye[3] = { Env( 5 )[0] - p[0], Env( 5 )[1] - p[1], Env( 5 )[2] - p[2] };
	out.v[0][0] = Dot3( in.tangent, toLight );
	out.v[0][1] = Dot3( in.bitangent, toLight );
	out.v[0][2] = Dot3( in.normal, toLight );
	out.v[1][0] = Dot4( t, Env( 10 ) );
	out.v[1][1] = Dot4( t, Env( 11 ) );
	out.v[1][2] = Dot4( pv, Env( 9 ) );			// falloff
	out.v[2][0] = Dot4( pv, Env( 6 ) );			// projection
	out.v[2][1] = Dot4( pv, Env( 7 ) );
	out.v[2][2] = Dot4( pv, Env( 8 ) );
	out.v[3][0] = Dot4( t, Env( 12 ) );			// diffuse
	out.v[3][1] = Dot4( t, Env( 13 ) );
	out.v[3][2] = Dot4( t, Env( 14 ) );			// specular
	out.v[3][3] = Dot4( t, Env( 15 ) );
	Normalize3( toLight );
	Normalize3( toEye );
	const float half[3] = { toLight[0] + toEye[0], toLight[1] + toEye[1], toLight[2] + toEye[2] };
	out.v[4][0] = Dot3( in.tangent, half );
	out.v[4][1] = Dot3( in.bitangent, half );
	out.v[4][2] = Dot3( in.normal, half );
	out.v[5][0] = in.color[0] * Env( 16 )[0] + Env( 16 )[1];
	out.v[5][1] = in.color[1] * Env( 16 )[0] + Env( 16 )[1];
	out.v[5][2] = in.color[2] * Env( 16 )[0] + Env( 16 )[1];
}

VS( environment ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
	memcpy( out.v[0], in.normal, sizeof( float ) * 3 );
	out.v[1][0] = Env( 5 )[0] - p[0];
	out.v[1][1] = Env( 5 )[1] - p[1];
	out.v[1][2] = Env( 5 )[2] - p[2];
	StageColor( in, out.v[2] );
}

VS( bumpyenvironment ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
	out.v[0][0] = in.texCoord[0];
	out.v[0][1] = in.texCoord[1];
	const float toEye[3] = { Env( 5 )[0] - p[0], Env( 5 )[1] - p[1], Env( 5 )[2] - p[2] };
	out.v[1][0] = Dot3( toEye, Env( 6 ) );
	out.v[1][1] = Dot3( toEye, Env( 7 ) );
	out.v[1][2] = Dot3( toEye, Env( 8 ) );
	for ( int i = 0; i < 3; i++ ) {
		out.v[2 + i][0] = Dot3( in.tangent, Env( 6 + i ) );
		out.v[2 + i][1] = Dot3( in.bitangent, Env( 6 + i ) );
		out.v[2 + i][2] = Dot3( in.normal, Env( 6 + i ) );
	}
	StageColor( in, out.v[5] );
}

VS( fog ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	const Vec4 pv = { p[0], p[1], p[2], p[3] };
	Transform( p, out.clip );
	out.v[0][0] = Dot4( pv, Local( L_PLANE0 ) );
	out.v[0][1] = Dot4( pv, Local( L_PLANE1 ) );
	out.v[1][0] = Dot4( pv, Local( L_PLANE2 ) );
	out.v[1][1] = Dot4( pv, Local( L_PLANE3 ) );
}

VS( blendlight ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	const Vec4 pv = { p[0], p[1], p[2], p[3] };
	Transform( p, out.clip );
	const Vec4 t = { Dot4( pv, Local( L_PLANE0 ) ), Dot4( pv, Local( L_PLANE1 ) ), 0.0f, Dot4( pv, Local( L_PLANE2 ) ) };
	out.v[0][0] = Dot4( t, Local( L_TEX_S ) );
	out.v[0][1] = Dot4( t, Local( L_TEX_T ) );
	out.v[0][2] = t.w;
	out.v[1][0] = Dot4( pv, Local( L_PLANE3 ) );
}

VS( heathaze ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	const Vec4 pv = { p[0], p[1], p[2], p[3] };
	Transform( p, out.clip );
	const float *matrices = VsUbo( 0 );
	out.v[0][0] = in.texCoord[0];
	out.v[0][1] = in.texCoord[1];
	out.v[1][0] = in.texCoord[0] + Local( 0 )[0];
	out.v[1][1] = in.texCoord[1] + Local( 0 )[1];
	const Vec4 projected = { 1.0f, 0.0f, Dot4( pv, matrices + 16 + 8 ), 1.0f };
	const float a = Dot4( projected, matrices + 32 );
	float b = Dot4( projected, matrices + 32 + 12 );
	b = b < 1.0f ? 1.0f : b;
	float scale = a / b;
	scale = scale > 0.02f ? 0.02f : scale;
	for ( int i = 0; i < 4; i++ ) {
		out.v[2][i] = scale * Local( 1 )[i];
		out.v[3][i] = in.color[i];
	}
}

VS( monochrome ) {
	const float p[4] = { in.position[0], in.position[1], in.position[2], 1.0f };
	Transform( p, out.clip );
	out.v[0][0] = in.texCoord[0];
	out.v[0][1] = in.texCoord[1];
}

FS( depth ) {
	return true;
}

FS( color ) {
	memcpy( out, FsUbo( 1 ), sizeof( float ) * 4 );
	return true;
}

FS( depth_alpha ) {
	float t[4];
	Texture2D( 0, v[0][0], v[0][1], t );
	const float *color = FsUbo( 1 );
	const float alpha = t[3] * color[3];
	if ( alpha <= FsUbo( 1 )[4] || v[1][0] < 0.0f ) {
		return false;
	}
	out[0] = color[0]; out[1] = color[1]; out[2] = color[2]; out[3] = alpha;
	return true;
}

FS( stage ) {
	float t[4];
	Texture2D( 0, v[0][0], v[0][1], t );
	for ( int i = 0; i < 4; i++ ) {
		out[i] = t[i] * v[1][i];
	}
	return true;
}

FS( stage_cube ) {
	float t[4];
	TextureCube( 0, v[0], t );
	for ( int i = 0; i < 4; i++ ) {
		out[i] = t[i] * v[1][i];
	}
	return true;
}

FS( stage_screen ) {
	float t[4];
	const float q = v[0][2] != 0.0f ? v[0][2] : 1.0f;
	Texture2D( 0, v[0][0] / q, v[0][1] / q, t );
	for ( int i = 0; i < 4; i++ ) {
		out[i] = t[i] * v[1][i];
	}
	return true;
}

FS( interaction ) {
	float bump[4], toLight[4], projection[4], falloff[4], diffuse[4], specular[4], table[4];
	Texture2D( 1, v[1][0], v[1][1], bump );
	const float normal[3] = { bump[3] * 2.0f - 1.0f, bump[1] * 2.0f - 1.0f, bump[2] * 2.0f - 1.0f };
	TextureCube( 0, v[0], toLight );
	const float light[3] = { toLight[0] * 2.0f - 1.0f, toLight[1] * 2.0f - 1.0f, toLight[2] * 2.0f - 1.0f };
	float dot = Dot3( light, normal );
	dot = dot < 0.0f ? 0.0f : dot;
	const float q = v[2][2] != 0.0f ? v[2][2] : 1.0f;
	Texture2D( 3, v[2][0] / q, v[2][1] / q, projection );
	Texture2D( 2, v[1][2], 0.5f, falloff );
	float half[3] = { v[4][0], v[4][1], v[4][2] };
	Normalize3( half );
	const float specularAngle = Saturate( Dot3( half, normal ) );
	Texture2D( 6, specularAngle, 0.5f, table );
	Texture2D( 5, v[3][2], v[3][3], specular );
	Texture2D( 4, v[3][0], v[3][1], diffuse );
	const float *diffuseColor = FsUbo( 0 );
	const float *specularColor = FsUbo( 0 ) + 4;
	for ( int i = 0; i < 3; i++ ) {
		const float s = table[0] * 2.0f * specularColor[i] * specular[i];
		const float surface = diffuse[i] * diffuseColor[i] + s;
		out[i] = dot * projection[i] * falloff[i] * surface * v[5][i];
	}
	out[3] = 0.0f;
	return true;
}

FS( environment ) {
	float normal[3] = { v[0][0], v[0][1], v[0][2] };
	float toEye[3] = { v[1][0], v[1][1], v[1][2] };
	Normalize3( normal );
	Normalize3( toEye );
	const float d = 2.0f * Dot3( toEye, normal );
	const float reflected[3] = { d * normal[0] - toEye[0], d * normal[1] - toEye[1], d * normal[2] - toEye[2] };
	float t[4];
	TextureCube( 0, reflected, t );
	for ( int i = 0; i < 4; i++ ) {
		out[i] = t[i] * v[2][i];
	}
	return true;
}

FS( bumpyenvironment ) {
	float bump[4];
	Texture2D( 1, v[0][0], v[0][1], bump );
	float local[3] = { bump[3] * 2.0f - 1.0f, bump[1] * 2.0f - 1.0f, bump[2] * 2.0f - 1.0f };
	Normalize3( local );
	const float normal[3] = { Dot3( local, v[2] ), Dot3( local, v[3] ), Dot3( local, v[4] ) };
	float toEye[3] = { v[1][0], v[1][1], v[1][2] };
	Normalize3( toEye );
	const float d = 2.0f * Dot3( toEye, normal );
	const float reflected[3] = { d * normal[0] - toEye[0], d * normal[1] - toEye[1], d * normal[2] - toEye[2] };
	float t[4];
	TextureCube( 0, reflected, t );
	for ( int i = 0; i < 3; i++ ) {
		out[i] = t[i] * v[5][i];
	}
	out[3] = v[5][3];
	return true;
}

FS( fog ) {
	float a[4], b[4];
	Texture2D( 0, v[0][0], v[0][1], a );
	Texture2D( 1, v[1][0], v[1][1], b );
	const float *color = FsUbo( 1 );
	for ( int i = 0; i < 4; i++ ) {
		out[i] = a[i] * b[i] * color[i];
	}
	return true;
}

FS( blendlight ) {
	float a[4], b[4];
	const float q = v[0][2] != 0.0f ? v[0][2] : 1.0f;
	Texture2D( 0, v[0][0] / q, v[0][1] / q, a );
	Texture2D( 1, v[1][0], 0.5f, b );
	const float *color = FsUbo( 1 );
	for ( int i = 0; i < 4; i++ ) {
		out[i] = a[i] * b[i] * color[i];
	}
	return true;
}

static bool Haze( const float v[MAX_VARYING][4], const float fragCoord[2], float out[4], const float *mask ) {
	float n[4];
	Texture2D( 1, v[1][0], v[1][1], n );
	float local[4] = { n[3] * 2.0f - 1.0f, n[1] * 2.0f - 1.0f, n[2] * 2.0f - 1.0f, n[3] * 2.0f - 1.0f };
	if ( mask ) {
		for ( int i = 0; i < 4; i++ ) {
			local[i] *= mask[i];
		}
	}
	const float *env = FsUbo( 0 );
	const float window[2] = { fragCoord[0] - env[7 * 4 + 1], env[7 * 4 + 0] - fragCoord[1] - env[7 * 4 + 2] };
	float s = Saturate( local[0] * v[2][0] + window[0] * env[4] ) * env[0];
	float t = Saturate( local[1] * v[2][1] + window[1] * env[5] ) * env[1];
	float c[4];
	Texture2D( 0, s, t, c );
	out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = 1.0f;
	return true;
}

FS( heathaze ) {
	return Haze( v, fragCoord, out, NULL );
}

FS( heathaze_mask ) {
	float mask[4];
	Texture2D( 2, v[0][0], v[0][1], mask );
	if ( mask[0] - 0.01f < 0.0f || mask[1] - 0.01f < 0.0f ) {
		return false;
	}
	mask[0] -= 0.01f;
	mask[1] -= 0.01f;
	return Haze( v, fragCoord, out, mask );
}

FS( heathaze_mask_vertex ) {
	float mask[4];
	Texture2D( 2, v[0][0], v[0][1], mask );
	mask[0] *= v[3][0];
	mask[1] *= v[3][1];
	if ( mask[0] - 0.01f < 0.0f || mask[1] - 0.01f < 0.0f ) {
		return false;
	}
	mask[0] -= 0.01f;
	mask[1] -= 0.01f;
	return Haze( v, fragCoord, out, mask );
}

FS( monochrome ) {
	float t[4];
	Texture2D( 0, v[0][0], v[0][1], t );
	const float l = ( t[0] + t[1] + t[2] ) * 0.33f;
	const float *color = FsUbo( 1 );
	out[0] = l * color[0]; out[1] = l * color[1]; out[2] = l * color[2]; out[3] = t[3] * color[3];
	return true;
}

static VertexFn FindVertex( const char *name ) {
	static const struct { const char *name; VertexFn fn; } table[] = {
		{ "depth.vert", VS_plain }, { "color.vert", VS_plain }, { "depth_clip.vert", VS_depth_clip }, { "depth_alpha.vert", VS_depth_alpha },
		{ "shadow.vert", VS_shadow }, { "stage.vert", VS_stage }, { "stage_normal_cube.vert", VS_stage_normal_cube },
		{ "stage_tex3_cube.vert", VS_stage_tex3_cube }, { "stage_screen.vert", VS_stage_screen }, { "interaction.vert", VS_interaction },
		{ "environment.vert", VS_environment }, { "bumpyenvironment.vert", VS_bumpyenvironment }, { "fog.vert", VS_fog },
		{ "blendlight.vert", VS_blendlight }, { "heathaze.vert", VS_heathaze }, { "monochrome.vert", VS_monochrome },
	};
	for ( size_t i = 0; i < sizeof( table ) / sizeof( table[0] ); i++ ) {
		if ( !strcmp( table[i].name, name ) ) {
			return table[i].fn;
		}
	}
	Refuse( "no stand-in for the vertex shader %s", name );
	return NULL;
}

static FragmentFn FindFragment( const char *name ) {
	static const struct { const char *name; FragmentFn fn; } table[] = {
		{ "depth.frag", FS_depth }, { "color.frag", FS_color }, { "depth_alpha.frag", FS_depth_alpha }, { "stage.frag", FS_stage },
		{ "stage_cube.frag", FS_stage_cube }, { "stage_screen.frag", FS_stage_screen }, { "interaction.frag", FS_interaction },
		{ "environment.frag", FS_environment }, { "bumpyenvironment.frag", FS_bumpyenvironment }, { "fog.frag", FS_fog },
		{ "blendlight.frag", FS_blendlight }, { "heathaze.frag", FS_heathaze }, { "heathaze_mask.frag", FS_heathaze_mask },
		{ "heathaze_mask_vertex.frag", FS_heathaze_mask_vertex }, { "monochrome.frag", FS_monochrome },
	};
	for ( size_t i = 0; i < sizeof( table ) / sizeof( table[0] ); i++ ) {
		if ( !strcmp( table[i].name, name ) ) {
			return table[i].fn;
		}
	}
	Refuse( "no stand-in for the fragment shader %s", name );
	return NULL;
}

/*
================================================================================
Rasterizing
================================================================================
*/
static float BlendFactor( DkBlendFactor f, const float *src, const float *dst, int channel ) {
	switch ( f ) {
		case DkBlendFactor_Zero:		return 0.0f;
		case DkBlendFactor_One:			return 1.0f;
		case DkBlendFactor_SrcColor:	return src[channel];
		case DkBlendFactor_InvSrcColor:	return 1.0f - src[channel];
		case DkBlendFactor_SrcAlpha:	return src[3];
		case DkBlendFactor_InvSrcAlpha:	return 1.0f - src[3];
		case DkBlendFactor_DstAlpha:	return dst[3];
		case DkBlendFactor_InvDstAlpha:	return 1.0f - dst[3];
		case DkBlendFactor_DstColor:	return dst[channel];
		case DkBlendFactor_InvDstColor:	return 1.0f - dst[channel];
		default:						return 1.0f;
	}
}

static inline bool Compare( DkCompareOp op, float a, float b ) {
	switch ( op ) {
		case DkCompareOp_Never:		return false;
		case DkCompareOp_Less:		return a < b;
		case DkCompareOp_Equal:		return a == b;
		case DkCompareOp_Lequal:	return a <= b;
		case DkCompareOp_Greater:	return a > b;
		case DkCompareOp_NotEqual:	return a != b;
		case DkCompareOp_Gequal:	return a >= b;
		default:					return true;
	}
}

static inline uint8_t StencilApply( DkStencilOp op, uint8_t value, uint8_t ref ) {
	switch ( op ) {
		case DkStencilOp_Zero:		return 0;
		case DkStencilOp_Replace:	return ref;
		case DkStencilOp_Incr:		return value == 255 ? 255 : value + 1;
		case DkStencilOp_Decr:		return value == 0 ? 0 : value - 1;
		case DkStencilOp_Invert:	return ~value;
		case DkStencilOp_IncrWrap:	return value + 1;
		case DkStencilOp_DecrWrap:	return value - 1;
		default:					return value;
	}
}

struct RasterVertex {
	float x, y, z, invW;
	float v[MAX_VARYING][4];
	float clipDistance;
};

static inline float EdgeFn( const RasterVertex &a, const RasterVertex &b, float x, float y ) {
	return ( b.x - a.x ) * ( y - a.y ) - ( b.y - a.y ) * ( x - a.x );
}

static void ToWindow( const VertexOut &in, RasterVertex &out ) {
	const float w = in.clip[3];
	const float invW = 1.0f / w;
	const float ndcX = in.clip[0] * invW;
	const float ndcY = in.clip[1] * invW;
	const float ndcZ = in.clip[2] * invW;
	const DkViewport &vp = gs.viewport;
	out.x = vp.x + vp.width * ( ndcX * 0.5f + 0.5f );
	out.y = vp.y + vp.height * ( 0.5f - ndcY * 0.5f );	// clip space y is up, as on OpenGL, on the first row at the top
	out.z = vp.near + ( vp.far - vp.near ) * ( ndcZ * 0.5f + 0.5f );
	out.invW = invW;
	for ( int i = 0; i < MAX_VARYING; i++ ) {
		for ( int c = 0; c < 4; c++ ) {
			out.v[i][c] = in.v[i][c] * invW;
		}
	}
	out.clipDistance = in.clipDistance * invW;
}

static void DrawTriangle( const VertexOut &a, const VertexOut &b, const VertexOut &c, FragmentFn fragment ) {
	if ( !gs.colorTarget ) {
		return;
	}
	RasterVertex v0, v1, v2;
	ToWindow( a, v0 );
	ToWindow( b, v1 );
	ToWindow( c, v2 );

	// window y grows downwards here, so a triangle counterclockwise on OpenGL's window has a negative area
	const float area = EdgeFn( v0, v1, v2.x, v2.y );
	if ( area == 0.0f ) {
		return;
	}
	const bool ccw = area < 0.0f;
	const bool front = ( gs.rasterizer.frontFace == DkFrontFace_CCW ) ? ccw : !ccw;
	if ( gs.rasterizer.cullMode == DkFace_FrontAndBack ) {
		return;
	}
	if ( ( gs.rasterizer.cullMode == DkFace_Front && front ) || ( gs.rasterizer.cullMode == DkFace_Back && !front ) ) {
		return;
	}

	const FakeImage *target = gs.colorTarget;
	const FakeImage *depthTarget = gs.depthTarget;
	const int width = (int)target->layout.w;
	const int height = (int)target->layout.h;

	float minX = v0.x, maxX = v0.x, minY = v0.y, maxY = v0.y;
	const float xs[2] = { v1.x, v2.x };
	const float ys[2] = { v1.y, v2.y };
	for ( int i = 0; i < 2; i++ ) {
		minX = xs[i] < minX ? xs[i] : minX;
		maxX = xs[i] > maxX ? xs[i] : maxX;
		minY = ys[i] < minY ? ys[i] : minY;
		maxY = ys[i] > maxY ? ys[i] : maxY;
	}
	const int scX0 = (int)gs.scissor.x;
	const int scY0 = (int)gs.scissor.y;
	const int scX1 = (int)( gs.scissor.x + gs.scissor.width );
	const int scY1 = (int)( gs.scissor.y + gs.scissor.height );
	int x0 = (int)floorf( minX ), x1 = (int)ceilf( maxX ), y0 = (int)floorf( minY ), y1 = (int)ceilf( maxY );
	x0 = x0 < scX0 ? scX0 : x0;
	y0 = y0 < scY0 ? scY0 : y0;
	x1 = x1 > scX1 ? scX1 : x1;
	y1 = y1 > scY1 ? scY1 : y1;
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > width ? width : x1;
	y1 = y1 > height ? height : y1;
	if ( x0 >= x1 || y0 >= y1 ) {
		return;
	}

	// nothing carries over from the last triangle's pixels
	for ( int unit = 0; unit < MAX_UNITS; unit++ ) {
		s_unitY[unit] = -1;
	}

	const DkDepthStencilState &ds = gs.depthStencil;
	const DkColorState &cs = gs.color;
	const uint32_t writeMask = *(const uint32_t *)&gs.colorWrite & 0xf;
	const float invArea = 1.0f / area;

	// polygon offset, as far as a picture can tell
	float bias = 0.0f;
	if ( gs.rasterizer.depthBiasEnableMask & DkPolygonFlag_Fill ) {
		const float dzdx = fabsf( ( v1.z - v0.z ) * ( v2.y - v0.y ) - ( v2.z - v0.z ) * ( v1.y - v0.y ) ) * fabsf( invArea );
		const float dzdy = fabsf( ( v2.z - v0.z ) * ( v1.x - v0.x ) - ( v1.z - v0.z ) * ( v2.x - v0.x ) ) * fabsf( invArea );
		bias = gs.depthBiasSlope * ( dzdx > dzdy ? dzdx : dzdy ) + gs.depthBiasConstant / 16777216.0f;
	}

	for ( int y = y0; y < y1; y++ ) {
		for ( int x = x0; x < x1; x++ ) {
			const float px = x + 0.5f, py = y + 0.5f;
			const float w0 = EdgeFn( v1, v2, px, py ) * invArea;
			const float w1 = EdgeFn( v2, v0, px, py ) * invArea;
			const float w2 = EdgeFn( v0, v1, px, py ) * invArea;
			if ( w0 < 0.0f || w1 < 0.0f || w2 < 0.0f ) {
				continue;
			}
			float z = w0 * v0.z + w1 * v1.z + w2 * v2.z + bias;
			z = z < 0.0f ? 0.0f : ( z > 1.0f ? 1.0f : z );

			// clip distance, when the shader has one
			const float invW = w0 * v0.invW + w1 * v1.invW + w2 * v2.invW;
			const float persp = 1.0f / invW;

			uint32_t *depthPixel = NULL;
			float depth = 0.0f;
			uint8_t stencil = 0;
			if ( depthTarget ) {
				depthPixel = (uint32_t *)depthTarget->data + y * depthTarget->layout.w + x;
				depth = ( *depthPixel & 0xffffff ) / 16777215.0f;
				stencil = (uint8_t)( *depthPixel >> 24 );
			}

			if ( gs.depthBounds && depthTarget && ( depth < gs.depthBoundsNear || depth > gs.depthBoundsFar ) ) {
				continue;
			}

			// faces are as above, per fragment only for its stencil
			bool stencilPass = true;
			if ( ds.stencilTestEnable && depthTarget ) {
				const DkCompareOp op = front ? ds.stencilFrontCompareOp : ds.stencilBackCompareOp;
				const uint8_t a = gs.stencilRef & gs.stencilFuncMask;
				const uint8_t b = stencil & gs.stencilFuncMask;
				stencilPass = Compare( op, (float)a, (float)b );
			}
			bool depthPass = true;
			if ( ds.depthTestEnable && depthTarget ) {
				depthPass = Compare( ds.depthCompareOp, z, depth );
				// depth equal passes are fed the same numbers, from the same matrix: a hair's slack for float rounding here
				if ( !depthPass && ds.depthCompareOp == DkCompareOp_Equal ) {
					depthPass = fabsf( z - depth ) < 1e-5f;
				}
			}

			if ( ds.stencilTestEnable && depthTarget ) {
				const DkStencilOp op = !stencilPass ? ( front ? ds.stencilFrontFailOp : ds.stencilBackFailOp )
					: !depthPass ? ( front ? ds.stencilFrontDepthFailOp : ds.stencilBackDepthFailOp )
					: ( front ? ds.stencilFrontPassOp : ds.stencilBackPassOp );
				const uint8_t updated = StencilApply( op, stencil, gs.stencilRef );
				if ( updated != stencil ) {
					*depthPixel = ( *depthPixel & 0xffffff ) | ( (uint32_t)( ( updated & gs.stencilMask ) | ( stencil & ~gs.stencilMask ) ) << 24 );
				}
			}
			if ( !stencilPass || !depthPass ) {
				continue;
			}

			float varyings[MAX_VARYING][4];
			for ( int i = 0; i < MAX_VARYING; i++ ) {
				for ( int c2 = 0; c2 < 4; c2++ ) {
					varyings[i][c2] = ( w0 * v0.v[i][c2] + w1 * v1.v[i][c2] + w2 * v2.v[i][c2] ) * persp;
				}
			}

			s_fragX = x;
			s_fragY = y;
			const float fragCoord[2] = { px, py };
			float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			if ( fragment && !fragment( varyings, fragCoord, color ) ) {
				continue;
			}
			if ( cs.alphaCompareOp != DkCompareOp_Always && !Compare( cs.alphaCompareOp, Saturate( color[3] ), gs.alphaRef ) ) {
				continue;
			}

			if ( ds.depthWriteEnable && depthTarget ) {
				*depthPixel = ( *depthPixel & 0xff000000u ) | (uint32_t)( z * 16777215.0f );
			}

			if ( !fragment || writeMask == 0 ) {
				continue;
			}

			byte *pixel = target->data + ( y * width + x ) * 4;
			float dst[4] = { pixel[0] / 255.0f, pixel[1] / 255.0f, pixel[2] / 255.0f, pixel[3] / 255.0f };
			float result[4];
			const bool blend = cs.blendEnableMask & 1;
			for ( int i = 0; i < 4; i++ ) {
				const float s = Saturate( color[i] );
				if ( blend ) {
					const float sc[4] = { Saturate( color[0] ), Saturate( color[1] ), Saturate( color[2] ), Saturate( color[3] ) };
					const DkBlendFactor sf = i < 3 ? (DkBlendFactor)gs.blend.srcColorBlendFactor : (DkBlendFactor)gs.blend.srcAlphaBlendFactor;
					const DkBlendFactor df = i < 3 ? (DkBlendFactor)gs.blend.dstColorBlendFactor : (DkBlendFactor)gs.blend.dstAlphaBlendFactor;
					result[i] = Saturate( sc[i] * BlendFactor( sf, sc, dst, i ) + dst[i] * BlendFactor( df, sc, dst, i ) );
				} else {
					result[i] = s;
				}
			}
			int probeX, probeY;
			Host_ProbePixel( &probeX, &probeY );
			if ( x == probeX && y == probeY ) {
				printf( "probe %i,%i: %s + %s: out %.3f %.3f %.3f %.3f, was %.3f %.3f %.3f %.3f, now %.3f %.3f %.3f %.3f, blend %s %d %d, mask %x\n", x, y,
					gs.vertexShader->name, gs.fragmentShader->name, color[0], color[1], color[2], color[3], dst[0], dst[1], dst[2], dst[3],
					result[0], result[1], result[2], result[3], blend ? "on" : "off", (int)gs.blend.srcColorBlendFactor, (int)gs.blend.dstColorBlendFactor, writeMask );
				fflush( stdout );
			}
			for ( int i = 0; i < 4; i++ ) {
				if ( writeMask & ( 1u << i ) ) {
					pixel[i] = (byte)( result[i] * 255.0f + 0.5f );
				}
			}
		}
	}
}

static void ClipNear( const VertexOut in[], int count, std::vector<VertexOut> &out ) {
	out.clear();
	for ( int i = 0; i < count; i++ ) {
		const VertexOut &a = in[i];
		const VertexOut &b = in[( i + 1 ) % count];
		const float da = a.clip[2] + a.clip[3];	// z >= -w
		const float db = b.clip[2] + b.clip[3];
		const float wa = a.clip[3] - 1e-4f;
		const float wb = b.clip[3] - 1e-4f;
		const float fa = da < wa ? da : wa;
		const float fb = db < wb ? db : wb;
		if ( fa >= 0.0f ) {
			out.push_back( a );
		}
		if ( ( fa >= 0.0f ) != ( fb >= 0.0f ) ) {
			const float t = fa / ( fa - fb );
			VertexOut m;
			for ( int c = 0; c < 4; c++ ) {
				m.clip[c] = a.clip[c] + ( b.clip[c] - a.clip[c] ) * t;
			}
			for ( int k = 0; k < MAX_VARYING; k++ ) {
				for ( int c = 0; c < 4; c++ ) {
					m.v[k][c] = a.v[k][c] + ( b.v[k][c] - a.v[k][c] ) * t;
				}
			}
			m.clipDistance = a.clipDistance + ( b.clipDistance - a.clipDistance ) * t;
			out.push_back( m );
		}
	}
}

static void FetchVertex( uint32_t index, VertexIn &in ) {
	memset( &in, 0, sizeof( in ) );
	in.position[3] = 1.0f;
	in.color[3] = 1.0f;
	for ( uint32_t a = 0; a < gs.numAttribs; a++ ) {
		const DkVtxAttribState &attrib = gs.attribs[a];
		const uint32_t bufferId = attrib.bufferId;
		const DkVtxBufferState &state = gs.buffers[bufferId];
		const byte *data = (const byte *)(uintptr_t)gs.vtx[bufferId].addr + attrib.offset + index * state.stride;
		float *target = a == 0 ? in.position : a == 1 ? in.texCoord : a == 2 ? in.normal : a == 3 ? in.tangent : a == 4 ? in.bitangent : in.color;
		switch ( attrib.size ) {
			case DkVtxAttribSize_2x32: memcpy( target, data, 8 ); break;
			case DkVtxAttribSize_3x32: memcpy( target, data, 12 ); break;
			case DkVtxAttribSize_4x32: memcpy( target, data, 16 ); break;
			case DkVtxAttribSize_4x8:
				for ( int c = 0; c < 4; c++ ) {
					target[c] = data[c] / 255.0f;
				}
				break;
			default:
				Refuse( "a vertex attribute size this stand-in does not read" );
				break;
		}
	}
}

static void DrawList( uint32_t count, const uint32_t *indexes, uint32_t firstVertex, bool strip ) {
	if ( !gs.vertexShader ) {
		Refuse( "a draw with no shader bound" );
		return;
	}
	s_statDraws++;
	s_statIndexes += count;
	if ( gs.numAttribs == 0 || gs.vtx[0].addr == 0 ) {
		Refuse( "a draw with shader %s and no vertex data", gs.vertexShader->name );
		return;
	}
	for ( int i = 0; i < 3; i++ ) {
		if ( gs.ubo[0][i].addr == 0 ) {
			Refuse( "a draw with vertex uniform buffer %i unbound", i );
			return;
		}
	}
	if ( !RasterizingTo( gs.colorTarget ) ) {
		return;
	}

	const VertexFn vertex = FindVertex( gs.vertexShader->name );
	const FragmentFn fragment = gs.fragmentShader ? FindFragment( gs.fragmentShader->name ) : NULL;
	if ( !vertex ) {
		return;
	}

	const bool clipDistance = strcmp( gs.vertexShader->name, "depth_clip.vert" ) == 0;
	std::vector<VertexOut> clipped;
	VertexOut tri[3];
	for ( uint32_t i = 0; i + 2 < count; i += strip ? 1 : 3 ) {
		for ( int k = 0; k < 3; k++ ) {
			uint32_t at = i + k;
			if ( strip && ( i & 1 ) && k < 2 ) {
				at = i + ( 1 - k );
			}
			const uint32_t index = indexes ? indexes[at] : firstVertex + at;
			VertexIn in;
			FetchVertex( index, in );
			memset( &tri[k], 0, sizeof( tri[k] ) );
			tri[k].clipDistance = 1.0f;
			vertex( in, tri[k] );
		}
		if ( clipDistance && tri[0].clipDistance < 0.0f && tri[1].clipDistance < 0.0f && tri[2].clipDistance < 0.0f ) {
			continue;
		}
		ClipNear( tri, 3, clipped );
		for ( size_t k = 1; k + 1 < clipped.size(); k++ ) {
			DrawTriangle( clipped[0], clipped[k], clipped[k + 1], fragment );
		}
	}
}

void dkCmdBufDraw( DkCmdBuf obj, DkPrimitive prim, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance ) {
	REC( obj, DrawList( vertexCount, NULL, firstVertex, prim == DkPrimitive_TriangleStrip ) );
}

void dkCmdBufDrawIndexed( DkCmdBuf obj, DkPrimitive prim, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance ) {
	if ( prim != DkPrimitive_Triangles ) {
		Refuse( "an indexed draw that is not triangles" );
	}
	REC( obj,
		if ( gs.idxFormat != DkIdxFormat_Uint32 && s_rasterize ) {
			Refuse( "an index format this stand-in does not read" );
		}
		DrawList( indexCount, (const uint32_t *)(uintptr_t)gs.idxAddr + firstIndex, 0, false )
	);
}

void dkCmdBufClearColor( DkCmdBuf obj, uint32_t targetId, uint32_t clearMask, const void *clearData ) {
	float value[4];
	memcpy( value, clearData, sizeof( value ) );
	REC( obj,
		if ( !gs.colorTarget || !RasterizingTo( gs.colorTarget ) ) {
			return;
		}
		const FakeImage *image = gs.colorTarget;
		const uint32_t endY = gs.scissor.y + gs.scissor.height;
		const uint32_t endX = gs.scissor.x + gs.scissor.width;
		for ( uint32_t y = gs.scissor.y; y < endY && y < image->layout.h; y++ ) {
			byte *row = image->data + y * image->layout.w * 4;
			for ( uint32_t x = gs.scissor.x; x < endX && x < image->layout.w; x++ ) {
				for ( int c = 0; c < 4; c++ ) {
					if ( clearMask & ( 1u << c ) ) {
						row[x * 4 + c] = (byte)( Saturate( value[c] ) * 255.0f + 0.5f );
					}
				}
			}
		}
	);
}

void dkCmdBufClearDepthStencil( DkCmdBuf obj, bool clearDepth, float depthValue, uint8_t stencilMask, uint8_t stencilValue ) {
	REC( obj,
		if ( !gs.depthTarget || !RasterizingTo( gs.colorTarget ) ) {
			return;
		}
		const FakeImage *image = gs.depthTarget;
		const uint32_t endY = gs.scissor.y + gs.scissor.height;
		const uint32_t endX = gs.scissor.x + gs.scissor.width;
		const uint32_t depth24 = (uint32_t)( depthValue * 16777215.0f );
		for ( uint32_t y = gs.scissor.y; y < endY && y < image->layout.h; y++ ) {
			uint32_t *row = (uint32_t *)image->data + y * image->layout.w;
			for ( uint32_t x = gs.scissor.x; x < endX && x < image->layout.w; x++ ) {
				uint32_t value = row[x];
				if ( clearDepth ) {
					value = ( value & 0xff000000u ) | depth24;
				}
				const uint8_t stencil = (uint8_t)( value >> 24 );
				value = ( value & 0xffffffu ) | ( (uint32_t)( ( stencilValue & stencilMask ) | ( stencil & ~stencilMask ) ) << 24 );
				row[x] = value;
			}
		}
	);
}
