/*
===========================================================================

PreyNX - vertex cache on deko3D memory.

Static surfaces live in large GPU blocks until they are freed or, under memory
pressure, purged oldest first. Dynamic surfaces use one bump allocator per frame
in flight. Every cache entry is just a GPU address.

===========================================================================
*/

#include "../idlib/precompiled.h"
#include "tr_local.h"

#undef BIT
#include <switch.h>

#include "deko3d/dk_local.h"

idVertexCache	vertexCache;

idCVar idVertexCache::r_showVertexCache( "r_showVertexCache", "0", CVAR_INTEGER | CVAR_RENDERER, "" );
idCVar idVertexCache::r_vertexBufferMegs( "r_vertexBufferMegs", "256", CVAR_INTEGER | CVAR_RENDERER | CVAR_INIT, "static vertex memory budget, in MB" );

static const int FRAME_TEMP_BYTES		= 24 * 1024 * 1024;
static const int STATIC_ALIGNMENT		= 16;
static const int TEMP_ALIGNMENT			= 16;

static dkAlloc_t	s_tempAlloc[NUM_VERTEX_FRAMES];
static int			s_tempHead[NUM_VERTEX_FRAMES];
static byte *		s_tempCpu[NUM_VERTEX_FRAMES];
static DkGpuAddr	s_tempGpu[NUM_VERTEX_FRAMES];
static vertCache_t *s_tempBlocks[NUM_VERTEX_FRAMES];		// headers handed out this frame

static ID_INLINE void UnlinkBlock( vertCache_t *block ) {
	block->next->prev = block->prev;
	block->prev->next = block->next;
}

static ID_INLINE void LinkAfter( vertCache_t *head, vertCache_t *block ) {
	block->next = head->next;
	block->prev = head;
	head->next->prev = block;
	head->next = block;
}

/*
==============
idVertexCache::Init
==============
*/
void idVertexCache::Init() {
	staticCountTotal = 0;
	staticAllocTotal = 0;
	staticCountThisFrame = 0;
	staticAllocThisFrame = 0;
	dynamicCountThisFrame = 0;
	dynamicAllocThisFrame = 0;
	currentFrame = 0;
	listNum = 0;
	virtualMemory = false;
	allocatingTempBuffer = false;
	tempOverflow = false;
	frameBytes = FRAME_TEMP_BYTES;

	staticHeaders.next = staticHeaders.prev = &staticHeaders;
	deferredFreeList.next = deferredFreeList.prev = &deferredFreeList;

	for ( int i = 0; i < NUM_VERTEX_FRAMES; i++ ) {
		if ( !dkr.vertexHeap.Alloc( FRAME_TEMP_BYTES, 256, s_tempAlloc[i] ) ) {
			common->FatalError( "deko3D: no memory for the frame vertex buffers" );
		}
		s_tempCpu[i] = (byte *)dkr.vertexHeap.CpuAddr( s_tempAlloc[i] );
		s_tempGpu[i] = dkr.vertexHeap.GpuAddr( s_tempAlloc[i] );
		s_tempHead[i] = 0;
		s_tempBlocks[i] = NULL;
	}
}

void idVertexCache::Shutdown() {
	// a fatal error during startup gets here before Init
	if ( staticHeaders.next == NULL ) {
		return;
	}
	PurgeAll();
	headerAllocator.Shutdown();
}

bool idVertexCache::IsFast() {
	return true;
}

void idVertexCache::ActuallyFree( vertCache_t *block ) {
	if ( block->user ) {
		*block->user = NULL;
		block->user = NULL;
	}
	Dk_DeferFreeVertex( block->alloc );
	staticAllocTotal -= block->size;
	staticCountTotal--;
	UnlinkBlock( block );
	headerAllocator.Free( block );
}

/*
===========
idVertexCache::PurgeAll

Everything goes; the surfaces allocate again the next time they draw.
===========
*/
void idVertexCache::PurgeAll() {
	while ( staticHeaders.next != &staticHeaders ) {
		ActuallyFree( staticHeaders.next );
	}
	while ( deferredFreeList.next != &deferredFreeList ) {
		ActuallyFree( deferredFreeList.next );
	}
}

/*
===========
idVertexCache::Free
===========
*/
void idVertexCache::Free( vertCache_t *block ) {
	if ( !block ) {
		return;
	}
	if ( block->tag == TAG_FREE ) {
		common->Error( "idVertexCache Free: block already freed" );
	}
	if ( block->tag == TAG_TEMP ) {
		return;
	}
	block->user = NULL;
	UnlinkBlock( block );
	LinkAfter( &deferredFreeList, block );
}

/*
===========
idVertexCache::Alloc
===========
*/
void idVertexCache::Alloc( void *data, int size, vertCache_t **buffer, bool indexBuffer ) {
	if ( size <= 0 ) {
		common->Error( "idVertexCache::Alloc: size = %i", size );
	}
	*buffer = NULL;

	// over budget: drop surfaces that have not drawn recently, oldest first
	const int budget = r_vertexBufferMegs.GetInteger() * 1024 * 1024;
	vertCache_t *candidate = staticHeaders.prev;
	while ( staticAllocTotal + size > budget && candidate != &staticHeaders ) {
		vertCache_t *prev = candidate->prev;
		if ( candidate->frameUsed + NUM_VERTEX_FRAMES < currentFrame ) {
			ActuallyFree( candidate );
		}
		candidate = prev;
	}

	dkAlloc_t alloc;
	if ( !dkr.vertexHeap.Alloc( size, STATIC_ALIGNMENT, alloc ) ) {
		common->Printf( "idVertexCache: out of vertex memory, %i MB in use\n", staticAllocTotal >> 20 );
		return;
	}

	vertCache_t *block = headerAllocator.Alloc();
	block->alloc = alloc;
	block->gpuAddr = dkr.vertexHeap.GpuAddr( alloc );
	block->vbo = 0;
	block->virtMem = (void *)block->gpuAddr;
	block->size = size;
	block->offset = 0;
	block->tag = TAG_USED;
	block->user = buffer;
	block->frameUsed = 0;
	block->indexBuffer = indexBuffer;
	block->next = block->prev = NULL;
	LinkAfter( &staticHeaders, block );

	if ( data ) {
		SIMDProcessor->Memcpy( dkr.vertexHeap.CpuAddr( alloc ), data, size );
	}

	staticAllocTotal += size;
	staticAllocThisFrame += size;
	staticCountTotal++;
	staticCountThisFrame++;
	*buffer = block;
}

/*
===========
idVertexCache::Touch
===========
*/
void idVertexCache::Touch( vertCache_t *block ) {
	if ( !block ) {
		common->Error( "idVertexCache Touch: NULL pointer" );
	}
	if ( block->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache Touch: freed pointer" );
	}
	if ( block->tag == TAG_TEMP ) {
		common->FatalError( "idVertexCache Touch: temporary pointer" );
	}
	block->frameUsed = currentFrame;
	UnlinkBlock( block );
	LinkAfter( &staticHeaders, block );
}

/*
===========
idVertexCache::AllocFrameTemp
===========
*/
vertCache_t *idVertexCache::AllocFrameTemp( void *data, int size, bool indexBuffer ) {
	if ( size <= 0 ) {
		common->Error( "idVertexCache::AllocFrameTemp: size = %i", size );
	}

	const int start = ( s_tempHead[listNum] + TEMP_ALIGNMENT - 1 ) & ~( TEMP_ALIGNMENT - 1 );
	if ( start + size > frameBytes ) {
		if ( !tempOverflow ) {
			common->Printf( "idVertexCache: frame temp memory ran out\n" );
			tempOverflow = true;
		}
		return NULL;
	}
	s_tempHead[listNum] = start + size;

	vertCache_t *block = headerAllocator.Alloc();
	block->gpuAddr = s_tempGpu[listNum] + start;
	block->vbo = 0;
	block->virtMem = (void *)block->gpuAddr;
	block->size = size;
	block->offset = start;
	block->tag = TAG_TEMP;
	block->user = NULL;
	block->indexBuffer = indexBuffer;
	block->frameUsed = currentFrame;
	block->next = s_tempBlocks[listNum];
	s_tempBlocks[listNum] = block;

	if ( data ) {
		SIMDProcessor->Memcpy( s_tempCpu[listNum] + start, data, size );
	}

	dynamicAllocThisFrame += size;
	dynamicCountThisFrame++;
	return block;
}

/*
===========
idVertexCache::EndFrame

Called after the frame's commands went to the GPU. The swap waited for the frame
that used the next temp area, so it can be written from here on.
===========
*/
void idVertexCache::EndFrame() {
	if ( r_showVertexCache.GetInteger() != 0 ) {
		common->Printf( "vertex cache: %i MB static (%i new), %i KB dynamic (%i)\n", staticAllocTotal >> 20,
			staticCountThisFrame, dynamicAllocThisFrame >> 10, dynamicCountThisFrame );
	}

	currentFrame = tr.frameCount;
	listNum = currentFrame % NUM_VERTEX_FRAMES;
	staticCountThisFrame = 0;
	staticAllocThisFrame = 0;
	dynamicCountThisFrame = 0;
	dynamicAllocThisFrame = 0;
	tempOverflow = false;

	s_tempHead[listNum] = 0;
	while ( s_tempBlocks[listNum] ) {
		vertCache_t *next = s_tempBlocks[listNum]->next;
		headerAllocator.Free( s_tempBlocks[listNum] );
		s_tempBlocks[listNum] = next;
	}

	while ( deferredFreeList.next != &deferredFreeList ) {
		ActuallyFree( deferredFreeList.next );
	}
}

void *idVertexCache::Position( vertCache_t *buffer ) {
	if ( !buffer || buffer->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache::Position: bad vertCache_t" );
	}
	if ( buffer->tag == TAG_USED ) {
		buffer->frameUsed = currentFrame;
	}
	return (void *)buffer->gpuAddr;
}

void idVertexCache::UnbindIndex() {
}

void idVertexCache::BindArrayBuffer( unsigned int vbo ) {
}

void idVertexCache::BindIndexBuffer( unsigned int vbo ) {
}

void idVertexCache::InvalidateBufferBindings() {
}

void idVertexCache::List( void ) {
	common->Printf( "%i MB in %i static blocks, %i KB of %i in the frame buffer\n", staticAllocTotal >> 20, staticCountTotal,
		s_tempHead[listNum] >> 10, frameBytes >> 10 );
}
