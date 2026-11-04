/*
===========================================================================

Quake4NX - native deko3D renderer: images.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../tr_local.h"

#undef BIT
#include <switch.h>

#include "dk_local.h"

struct dkImageData_t {
	DkImage			image;
	DkImageView		view;
	DkImageFormat	format;
	dkAlloc_t		alloc;
	bool			copyTarget;
};

static DkImageFormat Dk_ImageFormat( textureFormat_t format ) {
	switch ( format ) {
		case FMT_RGBA8:			return DkImageFormat_RGBA8_Unorm;
		case FMT_XRGB8:			return DkImageFormat_RGBA8_Unorm;
		case FMT_RGB565:		return DkImageFormat_RGB565_Unorm;
		case FMT_ALPHA:			return DkImageFormat_R8_Unorm;
		case FMT_L8A8:			return DkImageFormat_RG8_Unorm;
		case FMT_LUM8:			return DkImageFormat_R8_Unorm;
		case FMT_INT8:			return DkImageFormat_R8_Unorm;
		case FMT_DXT1:			return DkImageFormat_RGBA_BC1;
		case FMT_DXT5:			return DkImageFormat_RGBA_BC3;
		case FMT_BC7:			return DkImageFormat_RGBA_BC7_Unorm;
		case FMT_RGBA16F:		return DkImageFormat_RGBA16_Float;
		case FMT_SRGBA8:		return DkImageFormat_RGBA8_Unorm_sRGB;
		case FMT_DEPTH:			return DkImageFormat_Z24X8;
		case FMT_DEPTH_STENCIL:	return DkImageFormat_Z24S8;
		case FMT_X16:			return DkImageFormat_R16_Unorm;
		case FMT_Y16_X16:		return DkImageFormat_RG16_Unorm;
		default:
			common->Error( "unhandled image format %d", format );
			return DkImageFormat_RGBA8_Unorm;
	}
}

// OpenGL's luminance and alpha formats
static void Dk_ImageSwizzle( const idImageOpts &opts, textureUsage_t usage, DkImageSwizzle swizzle[4] ) {
	swizzle[0] = DkImageSwizzle_Red;
	swizzle[1] = DkImageSwizzle_Green;
	swizzle[2] = DkImageSwizzle_Blue;
	swizzle[3] = DkImageSwizzle_Alpha;

	if ( opts.colorFormat == CFM_GREEN_ALPHA ) {
		swizzle[0] = swizzle[1] = swizzle[2] = DkImageSwizzle_One;
		swizzle[3] = DkImageSwizzle_Green;
		return;
	}
	switch ( opts.format ) {
		case FMT_XRGB8:
			swizzle[3] = DkImageSwizzle_One;
			break;
		case FMT_ALPHA:
			swizzle[0] = swizzle[1] = swizzle[2] = DkImageSwizzle_One;
			swizzle[3] = DkImageSwizzle_Red;
			break;
		case FMT_LUM8:
		case FMT_X16:
			swizzle[0] = swizzle[1] = swizzle[2] = DkImageSwizzle_Red;
			swizzle[3] = DkImageSwizzle_One;
			break;
		case FMT_INT8:
			swizzle[0] = swizzle[1] = swizzle[2] = swizzle[3] = DkImageSwizzle_Red;
			break;
		case FMT_L8A8:
		case FMT_Y16_X16:
			swizzle[0] = swizzle[1] = swizzle[2] = DkImageSwizzle_Red;
			swizzle[3] = DkImageSwizzle_Green;
			break;
		default:
			// the game's programs read a normal map's X from alpha (RXGB, retail .dds files)
			if ( usage == TD_BUMP && opts.colorFormat != CFM_NORMAL_DXT5 ) {
				swizzle[3] = DkImageSwizzle_Red;
			}
			break;
	}
}

/*
========================
Dk_FixTileSize

deko3D 0.5.0 picks an image's tile height from 1.5 times its height, and its layout
shrinks it for the first level, but the descriptor and the engines use the unshrunk
one. Where they differ, for example BC textures 24 to 32 pixels tall, the second
column of blocks lands on the next mip and its upload overwrites it. Asking for the
shrunk tile makes them agree.

Rows are the first level's height, in blocks.
========================
*/
static void Dk_FixTileSize( DkImageLayoutMaker &maker, uint32_t rows ) {
	const uint32_t gobs = ( rows + rows / 2 + 7 ) / 8;
	const uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
	uint32_t shrunk = tile;
	while ( shrunk && ( 8u << ( shrunk - 1 ) ) >= rows ) {
		shrunk--;
	}
	if ( shrunk != tile ) {
		maker.flags |= DkImageFlags_CustomTileSize;
		maker.tileSize = (DkTileSize)shrunk;
	}
}

/*
========================
idImage::AllocImage
========================
*/
void idImage::AllocImage() {
	PurgeImage();

	if ( !glConfig.isInitialized ) {
		return;
	}

	dkData = new dkImageData_t;
	memset( dkData, 0, sizeof( *dkData ) );
	dkData->format = Dk_ImageFormat( opts.format );

	DkImageLayoutMaker maker;
	dkImageLayoutMakerDefaults( &maker, dkr.device );
	maker.type = ( opts.textureType == TT_CUBIC ) ? DkImageType_Cubemap : DkImageType_2D;
	maker.format = dkData->format;
	maker.dimensions[0] = opts.width;
	maker.dimensions[1] = ( opts.textureType == TT_CUBIC ) ? opts.width : opts.height;
	maker.dimensions[2] = ( opts.textureType == TT_CUBIC ) ? 6 : 0;
	storageGeneration++;
	maker.mipLevels = opts.numLevels;
	// images the engine copies the screen into go through the 2D engine, and a render target is one of those too
	if ( !IsCompressed() && opts.numLevels == 1 && opts.textureType == TT_2D ) {
		maker.flags |= DkImageFlags_Usage2DEngine | DkImageFlags_UsageRender;
		dkData->copyTarget = true;
	}

	Dk_FixTileSize( maker, IsCompressed() ? ( maker.dimensions[1] + 3 ) / 4 : maker.dimensions[1] );

	DkImageLayout layout;
	dkImageLayoutInitialize( &layout, &maker );

	if ( !dkr.imageHeap.Alloc( (uint32_t)dkImageLayoutGetSize( &layout ), dkImageLayoutGetAlignment( &layout ), dkData->alloc ) ) {
		common->FatalError( "deko3D: out of image memory for %s (%ux%u), %u MB in use",
			GetName(), opts.width, opts.height, dkr.imageHeap.BytesUsed() >> 20 );
	}
	dkImageInitialize( &dkData->image, &layout, dkData->alloc.block, dkData->alloc.offset );

	dkImageViewDefaults( &dkData->view, &dkData->image );
	Dk_ImageSwizzle( opts, usage, dkData->view.swizzle );

	texnum = Dk_AllocImageDescriptor();
	Dk_WriteImageDescriptor( texnum, dkData->view );
	SetTexParameters();
}

/*
========================
idImage::PurgeImage
========================
*/
void idImage::PurgeImage() {
	if ( dkData ) {
		Dk_DeferFreeImage( dkData->alloc, texnum );
		delete dkData;
		dkData = NULL;
	}
	texnum = TEXTURE_NOT_LOADED;
}

/*
========================
idImage::SubImageUpload

Width and height are in pixels, multiples of four for compressed data except
at the small mips.
========================
*/
void idImage::SubImageUpload( int mipLevel, int x, int y, int z, int width, int height, const void *pic, int pixelPitch ) const {
	if ( !dkData ) {
		return;
	}

	uint32_t rowBytes;
	uint32_t rows;
	if ( IsCompressed() ) {
		rowBytes = ( ( width + 3 ) / 4 ) * ( ( opts.format == FMT_DXT1 ) ? 8 : 16 );
		rows = ( height + 3 ) / 4;
	} else {
		rowBytes = width * BitsForFormat( opts.format ) / 8;
		rows = height;
	}
	const uint32_t size = rowBytes * rows;

	DkGpuAddr source;
	void *space = Dk_UploadSpace( size, 16, source );
	if ( opts.format == FMT_RGB565 ) {
		// the loader writes these big endian, as OpenGL's unpack swap expected
		const uint16_t *src = (const uint16_t *)pic;
		uint16_t *dst = (uint16_t *)space;
		for ( uint32_t i = 0; i < size / 2; i++ ) {
			dst[i] = (uint16_t)( ( src[i] << 8 ) | ( src[i] >> 8 ) );
		}
	} else if ( pixelPitch != 0 ) {
		const uint32_t sourceStride = pixelPitch * BitsForFormat( opts.format ) / 8;
		for ( uint32_t row = 0; row < rows; row++ ) {
			memcpy( (byte *)space + row * rowBytes, (const byte *)pic + row * sourceStride, rowBytes );
		}
	} else {
		memcpy( space, pic, size );
	}

	DkImageView view;
	dkImageViewDefaults( &view, &dkData->image );
	view.mipLevelOffset = mipLevel;
	view.mipLevelCount = 1;
	if ( opts.textureType == TT_CUBIC ) {
		view.layerOffset = z;
		view.layerCount = 1;
	}

	DkCopyBuf buffer = { source, rowBytes, rowBytes * rows };
	DkImageRect rect = { (uint32_t)x, (uint32_t)y, 0, (uint32_t)width, (uint32_t)height, 1 };
	dkCmdBufCopyBufferToImage( dkr.uploadCmd, &buffer, &view, &rect, 0 );
}

bool idImage::ReadPixelsRGBA8( idList<byte> &pixels ) {
	return false;
}

void idImage::SetPixel( int mipLevel, int x, int y, const void *data, int dataSize ) {
	SubImageUpload( mipLevel, x, y, 0, 1, 1, data );
}

/*
========================
idImage::SetTexParameters

The sampler is part of the handle a bind sets.
========================
*/
void idImage::SetTexParameters() {
	samplerIndex = Dk_SamplerIndex( filter, repeat );
}

void idImage::RefreshSamplerState() {
	SetTexParameters();
}

void idImage::Resize( int width, int height ) {
	if ( opts.width == width && opts.height == height ) {
		return;
	}
	opts.width = width;
	opts.height = height;
	AllocImage();
}

/*
==============
idImage::Bind
==============
*/
void idImage::Bind() {
	if ( !IsLoaded() ) {
		ActuallyLoadImage( true );
	}
	backEnd.glState.tmu[backEnd.glState.currenttmu].textureType = opts.textureType;
	Dk_SetTexture( backEnd.glState.currenttmu, dkMakeTextureHandle( texnum, samplerIndex ) );
}

void idImageManager::BindNull() {
}

void idImageManager::UnbindAll() {
}

/*
================
Render targets
================
*/
static const DkImageView *s_targetColor = NULL;

bool Dk_BindImageTarget( idImage *color, idImage *depth, int cubeFace ) {
	dkImageData_t *colorData = color ? color->GetDkData() : NULL;
	dkImageData_t *depthData = depth ? depth->GetDkData() : NULL;
	if ( color && !color->IsLoaded() ) {
		color->ActuallyLoadImage( true );
		colorData = color->GetDkData();
	}
	if ( depth && !depth->IsLoaded() ) {
		depth->ActuallyLoadImage( true );
		depthData = depth->GetDkData();
	}
	if ( !colorData && !depthData ) {
		return false;
	}
	Dk_SubmitUploads();
	s_targetColor = colorData ? &colorData->view : NULL;
	dkCmdBufBindRenderTargets( dkr.cmd, colorData ? &s_targetColor : NULL, colorData ? 1 : 0, depthData ? &depthData->view : NULL );
	Dk_SetTargetMetrics( color ? color->GetUploadHeight() : depth->GetUploadHeight(), true );
	return true;
}

void Dk_CopyRenderTarget( idRenderTexture *source, idRenderTexture *dest ) {
	if ( !source || !dest || source->GetNumColorImages() == 0 || dest->GetNumColorImages() == 0 ) {
		return;
	}
	dkImageData_t *from = source->GetColorImage( 0 )->GetDkData();
	dkImageData_t *to = dest->GetColorImage( 0 )->GetDkData();
	if ( !from || !to ) {
		return;
	}
	Dk_FlushForDraw();
	Dk_ColorTargetBarrier();
	DkImageRect srcRect = { 0, 0, 0, (uint32_t)source->GetWidth(), (uint32_t)source->GetHeight(), 1 };
	DkImageRect dstRect = { 0, 0, 0, (uint32_t)dest->GetWidth(), (uint32_t)dest->GetHeight(), 1 };
	dkCmdBufBlitImage( dkr.cmd, &from->view, &srcRect, &to->view, &dstRect, DkBlitFlag_FilterLinear, 0 );
	dkCmdBufBarrier( dkr.cmd, DkBarrier_Primitives, DkInvalidateFlags_Image );
}

/*
====================
idImage::CopyFramebuffer

The screen's rows are stored top first and a texture's bottom first, so the
copy flips it. Programs then read _currentRender as they do on OpenGL.
====================
*/
bool idImage::CopyFramebuffer( int x, int y, int imageWidth, int imageHeight, int cubeFace ) {
	if ( imageWidth <= 0 || imageHeight <= 0 || dkr.imageSlot < 0 ) {
		return false;
	}
	if ( !dkData || !dkData->copyTarget || opts.width != imageWidth || opts.height != imageHeight || opts.format != FMT_RGBA8 ) {
		opts.width = imageWidth;
		opts.height = imageHeight;
		opts.format = FMT_RGBA8;
		opts.colorFormat = CFM_DEFAULT;
		opts.numLevels = 1;
		opts.textureType = TT_2D;
		AllocImage();
		Dk_SubmitUploads();
	}

	Dk_FlushForDraw();
	Dk_ColorTargetBarrier();

	const bool screen = Dk_TargetIsScreen();
	const DkImageView *source = screen ? &dkr.colorViews[dkr.imageSlot] : s_targetColor;
	const int top = screen ? DK_SCREEN_HEIGHT - y - imageHeight : y;
	DkImageRect srcRect = { (uint32_t)x, (uint32_t)top, 0, (uint32_t)imageWidth, (uint32_t)imageHeight, 1 };
	DkImageRect dstRect = { 0, 0, 0, (uint32_t)imageWidth, (uint32_t)imageHeight, 1 };
	dkCmdBufBlitImage( dkr.cmd, source, &srcRect, &dkData->view, &dstRect, screen ? DkBlitFlag_FlipY : 0, 0 );
	dkCmdBufBarrier( dkr.cmd, DkBarrier_Primitives, DkInvalidateFlags_Image );

	backEnd.c_copyFrameBuffer++;
	return true;
}

// the stock materials that sample the depth buffer are among the programs this renderer skips
bool idImage::CopyDepthbuffer( int x, int y, int imageWidth, int imageHeight, int cubeFace ) {
	return false;
}

/*
====================
idImage::UploadScratch

A cinematic's frame, every time it changes.
====================
*/
void idImage::UploadScratch( const byte *data, int cols, int rows ) {
	if ( !dkData || opts.width != cols || opts.height != rows || opts.format != FMT_RGBA8 || opts.textureType != TT_2D ) {
		opts.textureType = TT_2D;
		opts.format = FMT_RGBA8;
		opts.colorFormat = CFM_DEFAULT;
		opts.width = cols;
		opts.height = rows;
		opts.numLevels = 1;
		AllocImage();
	}
	SubImageUpload( 0, 0, 0, 0, cols, rows, data );
	Bind();
}
