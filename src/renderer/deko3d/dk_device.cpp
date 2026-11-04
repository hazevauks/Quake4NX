/*
===========================================================================

Quake4NX - native deko3D renderer: device, memory, swapchain and frame pacing.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../tr_local.h"

#undef BIT
#include <switch.h>

#include "dk_local.h"
#include "../../sys/switch/switch_local.h"

dkGlobals_t	dkr;

static const uint32_t CMD_MEMORY_PER_FRAME	= 8 * 1024 * 1024;
static const uint32_t CMD_MEMORY_OVERFLOW	= 8 * 1024 * 1024;
static const uint32_t UPLOAD_CMD_MEMORY		= 1024 * 1024;
static const uint32_t UPLOAD_STAGING_SIZE	= 32 * 1024 * 1024;
static const uint32_t IMAGE_BLOCK_SIZE		= 64 * 1024 * 1024;
static const uint32_t IMAGE_BYTES_MAX		= 1536u * 1024 * 1024;	// the budget in docs/architecture.md
static const uint32_t VERTEX_BLOCK_SIZE		= 64 * 1024 * 1024;
static const uint32_t VERTEX_BYTES_MAX		= 512 * 1024 * 1024;

// a command can write a little past what it reserves, so each piece of command memory keeps a gap
static const uint32_t CMD_MEMORY_GAP		= 256;

static const int MAX_IMAGE_DESCRIPTORS		= DK_MAX_IMAGE_DESCRIPTORS;
static const int NUM_FILTERS				= 3;
static const int NUM_REPEATS				= 6;
static const int NUM_SAMPLERS				= DK_NUM_SAMPLERS;

static idCVar r_dkZcull( "r_dkZcull", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_INIT, "use the GPU's coarse depth culling (read at startup)" );

static DkMemBlock	s_cmdBlock;
static DkMemBlock	s_uploadCmdBlock;
static DkMemBlock	s_stagingBlock;
static DkMemBlock	s_framebufferBlock;
static DkFence		s_frameFences[DK_FRAMES_IN_FLIGHT];
static bool			s_frameFenceUsed[DK_FRAMES_IN_FLIGHT];
static bool			s_descriptorsDirty = false;
static uint32_t		s_overflowUsed = 0;

static uint32_t		s_stagingHead = 0;
static DkFence		s_stagingFence;
static bool			s_stagingFenceUsed = false;
static bool			s_uploadsPending = false;
static int			s_uploadCommands = 0;

static int			s_freeDescriptors[MAX_IMAGE_DESCRIPTORS];
static int			s_numFreeDescriptors = 0;

typedef struct {
	dkAlloc_t		alloc;
	int				descriptor;
	bool			isImage;
	unsigned int	frame;
} dkDeferredFree_t;

static idList<dkDeferredFree_t>	s_deferredFrees;

/*
============================================================================
idDkHeap
============================================================================
*/
idDkHeap::idDkHeap() {
	device = NULL;
	numBlocks = 0;
	bytesUsed = 0;
}

void idDkHeap::Init( DkDevice device_, uint32_t flags_, uint32_t blockSize_, uint32_t maxBytes ) {
	device = device_;
	flags = flags_;
	blockSize = blockSize_;
	maxBlocks = Min( (uint32_t)MAX_BLOCKS, maxBytes / blockSize_ );
	bytesUsed = 0;
	numBlocks = 0;
}

void idDkHeap::Shutdown() {
	for ( int i = 0; i < numBlocks; i++ ) {
		dkMemBlockDestroy( blocks[i] );
		freeRanges[i].Clear();
	}
	numBlocks = 0;
	bytesUsed = 0;
}

bool idDkHeap::AddBlock() {
	if ( numBlocks >= (int)maxBlocks ) {
		return false;
	}
	DkMemBlockMaker maker;
	dkMemBlockMakerDefaults( &maker, device, blockSize );
	maker.flags = flags;
	blocks[numBlocks] = dkMemBlockCreate( &maker );
	if ( !blocks[numBlocks] ) {
		return false;
	}
	freeRange_t all = { 0, blockSize };
	freeRanges[numBlocks].Clear();
	freeRanges[numBlocks].SetGranularity( 256 );
	freeRanges[numBlocks].Append( all );
	numBlocks++;
	return true;
}

bool idDkHeap::Alloc( uint32_t size, uint32_t align, dkAlloc_t &alloc ) {
	if ( size == 0 || size > blockSize ) {
		return false;
	}
	for ( int attempt = 0; attempt < 2; attempt++ ) {
		for ( int b = 0; b < numBlocks; b++ ) {
			idList<freeRange_t> &ranges = freeRanges[b];
			for ( int i = 0; i < ranges.Num(); i++ ) {
				const uint32_t start = ( ranges[i].offset + align - 1 ) & ~( align - 1 );
				const uint32_t pad = start - ranges[i].offset;
				if ( pad + size > ranges[i].size ) {
					continue;
				}
				const uint32_t rest = ranges[i].size - pad - size;
				const uint32_t restOffset = start + size;
				if ( pad ) {
					ranges[i].size = pad;
					if ( rest ) {
						freeRange_t after = { restOffset, rest };
						ranges.Insert( after, i + 1 );
					}
				} else if ( rest ) {
					ranges[i].offset = restOffset;
					ranges[i].size = rest;
				} else {
					ranges.RemoveIndex( i );
				}
				alloc.block = blocks[b];
				alloc.blockIndex = b;
				alloc.offset = start;
				alloc.size = size;
				bytesUsed += size;
				return true;
			}
		}
		if ( !AddBlock() ) {
			return false;
		}
	}
	return false;
}

void idDkHeap::Free( const dkAlloc_t &alloc ) {
	idList<freeRange_t> &ranges = freeRanges[alloc.blockIndex];
	bytesUsed -= alloc.size;

	int i = 0;
	while ( i < ranges.Num() && ranges[i].offset < alloc.offset ) {
		i++;
	}
	freeRange_t freed = { alloc.offset, alloc.size };
	if ( i > 0 && ranges[i - 1].offset + ranges[i - 1].size == freed.offset ) {
		ranges[i - 1].size += freed.size;
		if ( i < ranges.Num() && ranges[i - 1].offset + ranges[i - 1].size == ranges[i].offset ) {
			ranges[i - 1].size += ranges[i].size;
			ranges.RemoveIndex( i );
		}
	} else if ( i < ranges.Num() && freed.offset + freed.size == ranges[i].offset ) {
		ranges[i].offset = freed.offset;
		ranges[i].size += freed.size;
	} else {
		ranges.Insert( freed, i );
	}
}

void *idDkHeap::CpuAddr( const dkAlloc_t &alloc ) const {
	return (byte *)dkMemBlockGetCpuAddr( alloc.block ) + alloc.offset;
}

DkGpuAddr idDkHeap::GpuAddr( const dkAlloc_t &alloc ) const {
	return dkMemBlockGetGpuAddr( alloc.block ) + alloc.offset;
}

/*
============================================================================
Command memory

Each frame has its own slice. A frame that outgrows it takes more from the
overflow area of the same slice, and runs out only if the scene is enormous.
============================================================================
*/
static uint32_t Dk_FrameCmdOffset( int frame ) {
	return frame * ( CMD_MEMORY_PER_FRAME + CMD_MEMORY_OVERFLOW );
}

static void Dk_CmdOutOfMemory( void *userData, DkCmdBuf cmdbuf, size_t minReqSize ) {
	const uint32_t size = 1024 * 1024;
	if ( s_overflowUsed + size > CMD_MEMORY_OVERFLOW || minReqSize > size ) {
		common->FatalError( "deko3D: the frame's command memory ran out" );
	}
	const uint32_t offset = Dk_FrameCmdOffset( dkr.frameIndex ) + CMD_MEMORY_PER_FRAME + s_overflowUsed;
	dkCmdBufAddMemory( cmdbuf, s_cmdBlock, offset, size - CMD_MEMORY_GAP );
	s_overflowUsed += size;
}

static void Dk_ResetFrameCommands( void ) {
	s_overflowUsed = 0;
	dkCmdBufClear( dkr.cmd );
	dkCmdBufAddMemory( dkr.cmd, s_cmdBlock, Dk_FrameCmdOffset( dkr.frameIndex ), CMD_MEMORY_PER_FRAME - CMD_MEMORY_GAP );
}

// only the debug library reports through this; the release one ends the process silently
static void Dk_DebugError( void *userData, const char *context, DkResult result, const char *message ) {
	common->Printf( "deko3D error %d in %s: %s\n", result, context ? context : "?", message ? message : "" );
	Switch_FlushLog();
	if ( result != DkResult_Success ) {
		Sys_Error( "deko3D error %d in %s: %s", result, context ? context : "?", message ? message : "" );
	}
}

static void Dk_CheckQueue( const char *where ) {
	if ( dkQueueIsInErrorState( dkr.queue ) ) {
		Switch_FlushLog();
		Sys_Error( "deko3D: the GPU reported a fault (%s), after %u frames", where, dkr.frameCount );
	}
}

/*
============================================================================
Descriptors
============================================================================
*/
int Dk_AllocImageDescriptor( void ) {
	if ( s_numFreeDescriptors == 0 ) {
		common->FatalError( "deko3D: out of image descriptors" );
	}
	return s_freeDescriptors[--s_numFreeDescriptors];
}

void Dk_WriteImageDescriptor( int index, const DkImageView &view ) {
	DkImageDescriptor *descriptors = (DkImageDescriptor *)dkMemBlockGetCpuAddr( dkr.imageDescriptorBlock );
	dkImageDescriptorInitialize( &descriptors[index], &view, false, false );
	s_descriptorsDirty = true;
}

void Dk_DescriptorsChanged( void ) {
	s_descriptorsDirty = true;
}

int Dk_SamplerIndex( textureFilter_t filter, textureRepeat_t repeat ) {
	return (int)filter * NUM_REPEATS + (int)repeat;
}

static void Dk_InitSamplers( void ) {
	DkSamplerDescriptor *descriptors = (DkSamplerDescriptor *)dkMemBlockGetCpuAddr( dkr.samplerDescriptorBlock );
	for ( int f = 0; f < NUM_FILTERS; f++ ) {
		for ( int r = 0; r < NUM_REPEATS; r++ ) {
			DkSampler sampler;
			dkSamplerDefaults( &sampler );
			switch ( (textureFilter_t)f ) {
				case TF_LINEAR:
					sampler.minFilter = sampler.magFilter = DkFilter_Linear;
					sampler.mipFilter = DkMipFilter_None;
					break;
				case TF_NEAREST:
					sampler.minFilter = sampler.magFilter = DkFilter_Nearest;
					sampler.mipFilter = DkMipFilter_None;
					break;
				default:
					sampler.minFilter = sampler.magFilter = DkFilter_Linear;
					sampler.mipFilter = DkMipFilter_Linear;
					sampler.maxAnisotropy = 2.0f;
					break;
			}
			DkWrapMode wrap = DkWrapMode_Repeat;
			switch ( (textureRepeat_t)r ) {
				case TR_MIRRORED_REPEAT:
					wrap = DkWrapMode_MirroredRepeat;
					break;
				case TR_CLAMP:
					wrap = DkWrapMode_ClampToEdge;
					break;
				case TR_CLAMP_TO_BORDER:
				case TR_CLAMP_TO_ZERO_ALPHA:
					wrap = DkWrapMode_ClampToBorder;
					sampler.borderColor[0].value_f = sampler.borderColor[1].value_f = sampler.borderColor[2].value_f = 0.0f;
					sampler.borderColor[3].value_f = 0.0f;
					break;
				case TR_CLAMP_TO_ZERO:
					wrap = DkWrapMode_ClampToBorder;
					sampler.borderColor[0].value_f = sampler.borderColor[1].value_f = sampler.borderColor[2].value_f = 0.0f;
					sampler.borderColor[3].value_f = 1.0f;
					break;
				default:
					break;
			}
			sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] = wrap;
			dkSamplerDescriptorInitialize( &descriptors[f * NUM_REPEATS + r], &sampler );
		}
	}
}

/*
============================================================================
Uploads
============================================================================
*/
void Dk_SubmitUploads( void ) {
	if ( !s_uploadsPending && !s_descriptorsDirty ) {
		return;
	}
	// new images need their descriptors read again, new shaders their code, and the cache of
	// what was overwritten must go, with nothing still using it
	dkCmdBufBarrier( dkr.uploadCmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader );
	dkQueueSubmitCommands( dkr.queue, dkCmdBufFinishList( dkr.uploadCmd ) );
	dkQueueSignalFence( dkr.queue, &s_stagingFence, false );
	dkQueueFlush( dkr.queue );
	s_stagingFenceUsed = true;
	s_uploadsPending = false;
	s_descriptorsDirty = false;
}

static void Dk_ResetUploadCommands( void ) {
	s_uploadCommands = 0;
	dkCmdBufClear( dkr.uploadCmd );
	dkCmdBufAddMemory( dkr.uploadCmd, s_uploadCmdBlock, 0, UPLOAD_CMD_MEMORY - CMD_MEMORY_GAP );
}

void *Dk_UploadSpace( uint32_t size, uint32_t align, DkGpuAddr &gpuAddr ) {
	if ( size > UPLOAD_STAGING_SIZE ) {
		common->FatalError( "deko3D: transfer of %u bytes is too large", size );
	}
	// a copy takes a bit over 100 bytes of command memory: a map's textures, mip by mip, would outgrow it
	const int MAX_UPLOAD_COMMANDS = ( UPLOAD_CMD_MEMORY / 2 ) / 128;

	uint32_t start = ( s_stagingHead + align - 1 ) & ~( align - 1 );
	if ( start + size > UPLOAD_STAGING_SIZE || ++s_uploadCommands > MAX_UPLOAD_COMMANDS ) {
		// everything queued must finish before the staging memory is used again
		Dk_SubmitUploads();
		if ( s_stagingFenceUsed ) {
			dkFenceWait( &s_stagingFence, -1 );
			s_stagingFenceUsed = false;
		}
		Dk_ResetUploadCommands();
		start = 0;
	} else if ( !s_uploadsPending && s_stagingFenceUsed && dkFenceWait( &s_stagingFence, 0 ) == DkResult_Success ) {
		s_stagingFenceUsed = false;
		Dk_ResetUploadCommands();
	}
	s_stagingHead = start + size;
	s_uploadsPending = true;
	if ( s_uploadCommands == 0 ) {
		s_uploadCommands = 1;
	}
	gpuAddr = dkMemBlockGetGpuAddr( s_stagingBlock ) + start;
	return (byte *)dkMemBlockGetCpuAddr( s_stagingBlock ) + start;
}

/*
============================================================================
Deferred frees
============================================================================
*/
void Dk_DeferFreeImage( dkAlloc_t alloc, int descriptorIndex ) {
	dkDeferredFree_t entry;
	entry.alloc = alloc;
	entry.descriptor = descriptorIndex;
	entry.isImage = true;
	entry.frame = dkr.frameCount;
	s_deferredFrees.Append( entry );
}

void Dk_DeferFreeVertex( dkAlloc_t alloc ) {
	dkDeferredFree_t entry;
	entry.alloc = alloc;
	entry.descriptor = -1;
	entry.isImage = false;
	entry.frame = dkr.frameCount;
	s_deferredFrees.Append( entry );
}

static void Dk_RunDeferredFrees( bool all ) {
	for ( int i = 0; i < s_deferredFrees.Num(); i++ ) {
		const dkDeferredFree_t &entry = s_deferredFrees[i];
		if ( !all && dkr.frameCount - entry.frame < (unsigned int)DK_FRAMES_IN_FLIGHT + 1 ) {
			continue;
		}
		if ( entry.isImage ) {
			dkr.imageHeap.Free( entry.alloc );
			s_freeDescriptors[s_numFreeDescriptors++] = entry.descriptor;
		} else {
			dkr.vertexHeap.Free( entry.alloc );
		}
		s_deferredFrees.RemoveIndex( i );
		i--;
	}
}

/*
============================================================================
Frames
============================================================================
*/
void Dk_BeginFrame( void ) {
	if ( dkr.imageSlot >= 0 ) {
		return;
	}
	Dk_SubmitUploads();
	Dk_CheckQueue( "acquire" );
	dkr.imageSlot = dkQueueAcquireImage( dkr.queue, dkr.swapchain );
	Dk_ResetState();
	Dk_BindRenderTarget();
}

void Dk_Present( void ) {
	if ( dkr.imageSlot < 0 ) {
		Dk_BeginFrame();
	}
	Dk_SubmitUploads();
	Dk_CheckQueue( "present" );

	dkCmdBufDiscardDepthStencil( dkr.cmd );
	dkCmdBufSignalFence( dkr.cmd, &s_frameFences[dkr.frameIndex], true );
	dkQueueSubmitCommands( dkr.queue, dkCmdBufFinishList( dkr.cmd ) );
	dkQueuePresentImage( dkr.queue, dkr.swapchain, dkr.imageSlot );
	s_frameFenceUsed[dkr.frameIndex] = true;
	dkr.imageSlot = -1;
	dkr.frameCount++;

	// this slice of command and vertex memory is free once its last frame is done
	dkr.frameIndex = ( dkr.frameIndex + 1 ) % DK_FRAMES_IN_FLIGHT;
	if ( s_frameFenceUsed[dkr.frameIndex] ) {
		dkFenceWait( &s_frameFences[dkr.frameIndex], -1 );
	}
	Dk_RunDeferredFrees( false );
	Dk_ResetFrameCommands();
}

void Dk_WaitIdle( void ) {
	Dk_SubmitUploads();
	dkQueueWaitIdle( dkr.queue );
}

/*
============================================================================
Init
============================================================================
*/
static DkMemBlock Dk_MakeBlock( uint32_t size, uint32_t flags ) {
	DkMemBlockMaker maker;
	dkMemBlockMakerDefaults( &maker, dkr.device, ( size + DK_MEMBLOCK_ALIGNMENT - 1 ) & ~( DK_MEMBLOCK_ALIGNMENT - 1 ) );
	maker.flags = flags;
	return dkMemBlockCreate( &maker );
}

/*
================
Dk_ReadPixels

Screenshots and save previews. What was drawn so far goes to the GPU, and the
next commands carry on in the same frame. Rows come out bottom first, as OpenGL's.
================
*/
bool Dk_ReadPixels( int x, int y, int width, int height, void *pixels, int components ) {
	if ( dkr.imageSlot < 0 || width <= 0 || height <= 0 ) {
		return false;
	}
	const uint32_t rowBytes = width * 4;
	DkMemBlock block = Dk_MakeBlock( rowBytes * height, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached );
	if ( !block ) {
		return false;
	}

	Dk_FlushForDraw();
	dkCmdBufBarrier( dkr.cmd, DkBarrier_Fragments, 0 );
	DkImageRect rect = { (uint32_t)x, (uint32_t)( DK_SCREEN_HEIGHT - y - height ), 0, (uint32_t)width, (uint32_t)height, 1 };
	DkCopyBuf buffer = { dkMemBlockGetGpuAddr( block ), rowBytes, (uint32_t)height };
	dkCmdBufCopyImageToBuffer( dkr.cmd, &dkr.colorViews[dkr.imageSlot], &rect, &buffer, 0 );
	dkCmdBufBarrier( dkr.cmd, DkBarrier_Full, 0 );
	Dk_SubmitUploads();
	dkQueueSubmitCommands( dkr.queue, dkCmdBufFinishList( dkr.cmd ) );
	dkQueueWaitIdle( dkr.queue );

	const byte *source = (const byte *)dkMemBlockGetCpuAddr( block );
	byte *out = (byte *)pixels;
	const int outRow = ( width * components + 3 ) & ~3;
	for ( int row = 0; row < height; row++ ) {
		const byte *in = source + ( height - 1 - row ) * rowBytes;
		if ( components == 4 ) {
			memcpy( out + row * outRow, in, rowBytes );
		} else {
			byte *to = out + row * outRow;
			for ( int column = 0; column < width; column++ ) {
				to[column * 3 + 0] = in[column * 4 + 0];
				to[column * 3 + 1] = in[column * 4 + 1];
				to[column * 3 + 2] = in[column * 4 + 2];
			}
		}
	}
	dkMemBlockDestroy( block );
	return true;
}

static void Dk_CreateFramebuffers( void ) {
	DkImageLayoutMaker maker;
	dkImageLayoutMakerDefaults( &maker, dkr.device );
	maker.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
	maker.format = DkImageFormat_Z24S8;
	maker.dimensions[0] = DK_SCREEN_WIDTH;
	maker.dimensions[1] = DK_SCREEN_HEIGHT;
	DkImageLayout depthLayout;
	dkImageLayoutInitialize( &depthLayout, &maker );

	maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
	maker.format = DkImageFormat_RGBA8_Unorm;
	DkImageLayout colorLayout;
	dkImageLayoutInitialize( &colorLayout, &maker );

	const uint32_t depthAlign = dkImageLayoutGetAlignment( &depthLayout );
	const uint32_t depthSize = ( dkImageLayoutGetSize( &depthLayout ) + depthAlign - 1 ) & ~( depthAlign - 1 );
	const uint32_t colorAlign = dkImageLayoutGetAlignment( &colorLayout );
	const uint32_t colorSize = ( dkImageLayoutGetSize( &colorLayout ) + colorAlign - 1 ) & ~( colorAlign - 1 );

	const uint32_t offsetColor = ( depthSize + colorAlign - 1 ) & ~( colorAlign - 1 );
	s_framebufferBlock = Dk_MakeBlock( offsetColor + colorSize * DK_SWAP_IMAGES, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image );

	dkImageInitialize( &dkr.depthImage, &depthLayout, s_framebufferBlock, 0 );
	dkImageViewDefaults( &dkr.depthView, &dkr.depthImage );

	const DkImage *swapImages[DK_SWAP_IMAGES];
	for ( int i = 0; i < DK_SWAP_IMAGES; i++ ) {
		dkImageInitialize( &dkr.colorImages[i], &colorLayout, s_framebufferBlock, offsetColor + i * colorSize );
		dkImageViewDefaults( &dkr.colorViews[i], &dkr.colorImages[i] );
		swapImages[i] = &dkr.colorImages[i];
	}

	DkSwapchainMaker swapMaker;
	dkSwapchainMakerDefaults( &swapMaker, dkr.device, nwindowGetDefault(), swapImages, DK_SWAP_IMAGES );
	dkr.swapchain = dkSwapchainCreate( &swapMaker );
}

bool Dk_Init( void ) {
	memset( &dkr.device, 0, offsetof( dkGlobals_t, imageHeap ) );
	dkr.imageSlot = -1;

	DkDeviceMaker deviceMaker;
	dkDeviceMakerDefaults( &deviceMaker );
	deviceMaker.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginUpperLeft;
	deviceMaker.cbDebug = Dk_DebugError;
	dkr.device = dkDeviceCreate( &deviceMaker );
	if ( !dkr.device ) {
		return false;
	}

	s_cmdBlock = Dk_MakeBlock( DK_FRAMES_IN_FLIGHT * ( CMD_MEMORY_PER_FRAME + CMD_MEMORY_OVERFLOW ), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached );
	s_uploadCmdBlock = Dk_MakeBlock( UPLOAD_CMD_MEMORY, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached );
	s_stagingBlock = Dk_MakeBlock( UPLOAD_STAGING_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached );
	dkr.imageDescriptorBlock = Dk_MakeBlock( MAX_IMAGE_DESCRIPTORS * sizeof( DkImageDescriptor ), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached );
	dkr.samplerDescriptorBlock = Dk_MakeBlock( NUM_SAMPLERS * sizeof( DkSamplerDescriptor ), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached );
	dkr.uniformBlock = Dk_MakeBlock( 64 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached );

	dkr.imageHeap.Init( dkr.device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, IMAGE_BLOCK_SIZE, IMAGE_BYTES_MAX );
	dkr.vertexHeap.Init( dkr.device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, VERTEX_BLOCK_SIZE, VERTEX_BYTES_MAX );

	s_numFreeDescriptors = 0;
	for ( int i = MAX_IMAGE_DESCRIPTORS - 1; i >= 0; i-- ) {
		s_freeDescriptors[s_numFreeDescriptors++] = i;
	}
	Dk_InitSamplers();

	DkQueueMaker queueMaker;
	dkQueueMakerDefaults( &queueMaker, dkr.device );
	queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | ( r_dkZcull.GetBool() ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull );
	queueMaker.commandMemorySize = 1024 * 1024;
	queueMaker.flushThreshold = queueMaker.commandMemorySize / 8;
	dkr.queue = dkQueueCreate( &queueMaker );

	DkCmdBufMaker cmdMaker;
	dkCmdBufMakerDefaults( &cmdMaker, dkr.device );
	cmdMaker.cbAddMem = Dk_CmdOutOfMemory;
	dkr.cmd = dkCmdBufCreate( &cmdMaker );
	dkCmdBufMakerDefaults( &cmdMaker, dkr.device );
	dkr.uploadCmd = dkCmdBufCreate( &cmdMaker );

	Dk_ResetFrameCommands();
	Dk_ResetUploadCommands();
	Dk_CreateFramebuffers();
	Dk_InitState();
	Dk_InitPrograms();

	common->Printf( "deko3D: %ix%i, %i swapchain images, %i frames in flight\n", DK_SCREEN_WIDTH, DK_SCREEN_HEIGHT, DK_SWAP_IMAGES, DK_FRAMES_IN_FLIGHT );
	return true;
}

void Dk_Shutdown( void ) {
	if ( !dkr.device ) {
		return;
	}
	dkQueueWaitIdle( dkr.queue );
	Dk_RunDeferredFrees( true );
	s_deferredFrees.Clear();

	Dk_ShutdownPrograms();
	dkSwapchainDestroy( dkr.swapchain );
	dkQueueDestroy( dkr.queue );
	dkCmdBufDestroy( dkr.cmd );
	dkCmdBufDestroy( dkr.uploadCmd );
	dkr.imageHeap.Shutdown();
	dkr.vertexHeap.Shutdown();
	dkMemBlockDestroy( dkr.uniformBlock );
	dkMemBlockDestroy( dkr.samplerDescriptorBlock );
	dkMemBlockDestroy( dkr.imageDescriptorBlock );
	dkMemBlockDestroy( s_stagingBlock );
	dkMemBlockDestroy( s_uploadCmdBlock );
	dkMemBlockDestroy( s_cmdBlock );
	dkMemBlockDestroy( s_framebufferBlock );
	dkDeviceDestroy( dkr.device );
	dkr.device = NULL;
	for ( int i = 0; i < DK_FRAMES_IN_FLIGHT; i++ ) {
		s_frameFenceUsed[i] = false;
	}
	s_stagingFenceUsed = false;
	s_uploadsPending = false;
	s_stagingHead = 0;
}
