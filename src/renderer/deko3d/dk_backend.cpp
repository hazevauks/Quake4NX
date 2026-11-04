/*
===========================================================================

Quake4NX - native deko3D renderer: the frame's commands, render targets, and the
entry points the shared front end expects from the back end it replaced.

Quake 4's game code would draw the scene to a render texture and resolve it
through post-process materials. On the Switch it draws straight to the screen
(game/Game_render.cpp), so those passes cost nothing, and render targets here
serve what is left: a subview's target and the like.

The link surface at the end is upstream's, from its Vulkan module, which replaced
the same OpenGL files. None of the OpenGL subsystems it stands in for exist here.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../tr_local.h"
#include "../RenderModuleAPI.h"
#include "../GLStateCache.h"
#include "../GLDebugScope.h"
#include "../ShadowMapArb2Parity.h"
#include "../ModernShadowMaps.h"
#include "../ModernGLShaderLibrary.h"
#include "../RendererUpload.h"
#include "../ModernGLExecutor.h"
#include "../ModernLightImageAtlas.h"
#include "../RenderGraphResources.h"
#include "../RendererMetrics.h"
#include "../SavePreviewCapture.h"
#include "../../imagetools/DXT/DXTCodec.h"

#undef BIT
#include <switch.h>

#include "dk_local.h"

static idCVar r_dkLogPerf( "r_dkLogPerf", "0", CVAR_RENDERER | CVAR_BOOL, "log back end time and draw counts once a second" );

void R_PublishCompressionCapsToImageTools( void );

/*
================
Dk_ForceSettings

What needs a pass this renderer does not have is off, whatever a config says, so the
front end does not prepare for it.
================
*/
static void Dk_ForceSettings( void ) {
	static const char *off[] = {
		"r_useShadowMap", "r_shadowMapCSM", "r_ssao", "r_bloom", "r_hdrToneMap", "r_crt", "r_softParticles",
		"r_enhancedMaterials", "r_temporalAA", "r_rendererDynamicResolution", "r_pbrMaterials",
		"r_rendererReflectionProbes", "r_rendererClusteredDecals", "r_rendererFroxelVolumetrics",
		"r_rendererSSR", "r_rendererSSGI", "r_gpuSkinning", "r_rendererModernQuality",
		"r_rendererSharedGui", "r_rendererSharedInWorldGui", "r_rendererSharedCinematicPost",
		"r_rendererSharedSpecialFrame", "r_rendererSharedWorldAmbient", "r_rendererSharedWorldInteraction",
		"r_rendererSharedWorldFogBlend", "r_rendererSharedSubview", "r_usePostLightingStack",
		NULL
	};
	for ( int i = 0; off[i]; i++ ) {
		if ( cvarSystem->Find( off[i] ) ) {
			cvarSystem->SetCVarInteger( off[i], 0 );
		}
	}
}

/*
================
Dk_InitRenderDevice

Where the OpenGL path would make a window and a context.
================
*/
bool Dk_InitRenderDevice( void ) {
	common->Printf( "----- Dk_InitRenderDevice -----\n" );

	if ( !Dk_Init() ) {
		common->Warning( "deko3D could not start" );
		return false;
	}

	glConfig.renderer_string = "Tegra X1";
	glConfig.vendor_string = "NVIDIA";
	glConfig.version_string = "deko3D";
	glConfig.extensions_string = "";
	glConfig.wgl_extensions_string = "";
	glConfig.gpuClass = RendererGpuClass::Integrated;
	glConfig.dedicatedVideoMemoryMB = 0;

	glConfig.maxTextureSize = 8192;
	glConfig.maxTextureUnits = 8;
	glConfig.maxTextureCoords = 8;
	glConfig.maxTextureImageUnits = DK_MAX_TEXTURE_UNITS;

	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;

	glConfig.vidWidth = DK_SCREEN_WIDTH;
	glConfig.vidHeight = DK_SCREEN_HEIGHT;
	glConfig.uiViewportX = 0;
	glConfig.uiViewportY = 0;
	glConfig.uiViewportWidth = DK_SCREEN_WIDTH;
	glConfig.uiViewportHeight = DK_SCREEN_HEIGHT;
	glConfig.isFullscreen = true;

	engineWindowState.vidWidth = DK_SCREEN_WIDTH;
	engineWindowState.vidHeight = DK_SCREEN_HEIGHT;
	engineWindowState.uiViewportX = 0;
	engineWindowState.uiViewportY = 0;
	engineWindowState.uiViewportWidth = DK_SCREEN_WIDTH;
	engineWindowState.uiViewportHeight = DK_SCREEN_HEIGHT;
	engineWindowState.displayFrequency = 60;
	engineWindowState.isFullscreen = true;
	engineWindowState.displayScale = 1.0f;
	engineWindowState.pixelDensityX = 1.0f;
	engineWindowState.pixelDensityY = 1.0f;

	// the front end's standard path is what this back end consumes
	glConfig.allowARB2Path = true;
	glConfig.textureNonPowerOfTwoAvailable = true;
	glConfig.textureCompressionAvailable = true;
	glConfig.bptcTextureCompressionAvailable = false;
	glConfig.cubeMapAvailable = true;
	R_PublishCompressionCapsToImageTools();

	Dk_ForceSettings();

	glConfig.isInitialized = true;
	Sys_InitInput();

	common->Printf( "deko3D renderer initialized\n" );
	return true;
}

/*
================
GLimp entry points
================
*/
bool GLimp_Init( glimpParms_t parms ) {
	return Dk_InitRenderDevice();
}

bool GLimp_SetScreenParms( glimpParms_t parms ) {
	return true;
}

void GLimp_Shutdown( void ) {
	Dk_Shutdown();
	glConfig.isInitialized = false;
}

void GLimp_ReleaseForExit( void ) {
	Dk_Shutdown();
}

void GLimp_SwapBuffers( void ) {
	Dk_Present();
}

void GLimp_ActivateContext( void ) {
}

void GLimp_DeactivateContext( void ) {
}

bool GLimp_EnsureActiveContext( const char *operation ) {
	return true;
}

void *GLimp_ExtensionPointer( const char *name ) {
	return NULL;
}

void GLimp_SetGamma( unsigned short red[256], unsigned short green[256], unsigned short blue[256] ) {
}

bool GLimp_UseNativeGammaRamps( void ) {
	return false;
}

void GLimp_EnableLogging( bool enable ) {
}

void GLimp_PreserveWindowOnShutdown( bool preserve ) {
}

void GLimp_WakeBackEnd( void *data ) {
}

void GLimp_FrontEndSleep( void ) {
}

void *GLimp_BackEndSleep( void ) {
	return NULL;
}

bool GLimp_SpawnRenderThread( void ( *function )( void ) ) {
	return false;
}

bool Dk_DrawableSize( int &width, int &height ) {
	width = DK_SCREEN_WIDTH;
	height = DK_SCREEN_HEIGHT;
	return true;
}

/*
================
The display's gamma
================
*/
void RB_ShutdownScreenBlit( void ) {
}

/*
===============================================================================
The commands of a frame
===============================================================================
*/
static void RB_CopyRenderCommand( const copyRenderCommand_t *cmd ) {
	if ( r_skipCopyTexture.GetBool() || cmd->image == NULL ) {
		return;
	}
	if ( cmd->copyDepth ) {
		cmd->image->CopyDepthbuffer( cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight, cmd->cubeFace );
	} else {
		cmd->image->CopyFramebuffer( cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight, cmd->cubeFace );
	}
}

void RB_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	if ( !glConfig.isInitialized || ( cmds->commandId == RC_NOP && !cmds->next ) ) {
		return;
	}

	const int startTime = Sys_Milliseconds();

	backEnd.renderTexture = NULL;
	backEnd.feedbackRenderTexture = NULL;
	backEnd.postProcessTexelSize = tr.postProcessTexelSize;
	backEnd.postProcessSourceColorSpace = tr.postProcessSourceColorSpace;
	backEnd.postProcessSMAAQuality = tr.postProcessSMAAQuality;

	for ( ; cmds != NULL; cmds = (const emptyCommand_t *)cmds->next ) {
		// the next screen image is acquired only when there is something to draw on it
		if ( dkr.imageSlot < 0 && cmds->commandId != RC_NOP ) {
			Dk_BeginFrame();
			RB_SetDefaultGLState();
		}

		switch ( cmds->commandId ) {
			case RC_NOP:
				break;
			case RC_SET_BUFFER: {
				// the swapchain's images keep older frames, which a view that does not cover the screen would show
				static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
				backEnd.frameCount = ( (const setBufferCommand_t *)cmds )->frameCount;
				Dk_SetScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
				Dk_ClearRenderTarget( true, black, false, 1.0f );
				break;
			}
			case RC_DRAW_VIEW:
				RB_DrawViewCommand( cmds );
				break;
			case RC_SET_RENDERTEXTURE: {
				const setRenderTargetCommand_t *cmd = (const setRenderTargetCommand_t *)cmds;
				if ( cmd->renderTexture != NULL ) {
					if ( cmd->renderTexture->MakeCurrent() ) {
						backEnd.renderTexture = cmd->renderTexture;
						backEnd.feedbackRenderTexture = cmd->feedbackRenderTexture;
					}
				} else {
					idRenderTexture::BindNull();
					backEnd.renderTexture = NULL;
					backEnd.feedbackRenderTexture = NULL;
				}
				break;
			}
			case RC_CLEAR_RENDERTARGET: {
				const renderClearBufferCommand_t *cmd = (const renderClearBufferCommand_t *)cmds;
				Dk_ClearRenderTarget( cmd->clearColor, cmd->clearColorValue.ToFloatPtr(), cmd->clearDepth, cmd->clearDepthValue );
				break;
			}
			case RC_RESOLVE_MSAA: {
				// nothing is multisampled: only the copy is left
				const resolveRenderTargetCommand_t *cmd = (const resolveRenderTargetCommand_t *)cmds;
				if ( cmd->msaaRenderTexture != cmd->destRenderTexture ) {
					Dk_CopyRenderTarget( cmd->msaaRenderTexture, cmd->destRenderTexture );
				}
				break;
			}
			case RC_SET_POSTPROCESS_SOURCE_SIZE:
				backEnd.postProcessTexelSize = ( (const setPostProcessSourceSizeCommand_t *)cmds )->texelSize;
				break;
			case RC_SET_POSTPROCESS_SOURCE_COLOR_SPACE:
				backEnd.postProcessSourceColorSpace = ( (const setPostProcessSourceColorSpaceCommand_t *)cmds )->colorSpace;
				break;
			case RC_SET_POSTPROCESS_SMAA_QUALITY:
				backEnd.postProcessSMAAQuality = ( (const setPostProcessSMAAQualityCommand_t *)cmds )->quality;
				break;
			case RC_COPY_RENDER:
				RB_CopyRenderCommand( (const copyRenderCommand_t *)cmds );
				break;
			case RC_SWAP_BUFFERS:
				RB_QueueSavePreview();
				// a capture flushes a frame to read it back, and that frame must not be shown
				if ( !tr.takingScreenshot ) {
					GLimp_SwapBuffers();
				}
				break;
			default:
				// RC_DRAW_SPECIAL_EFFECTS, RC_RESOLVE_TEMPORAL_PRESENTATION, RC_PRESENT_SCALED_SCENE, RC_VR_TARGET:
				// none of them is ever asked for with the settings Dk_ForceSettings holds
				break;
		}
	}

	backEnd.pc.msec = Sys_Milliseconds() - startTime;

	if ( r_dkLogPerf.GetBool() ) {
		static int lastLog = 0;
		const int now = Sys_Milliseconds();
		if ( now - lastLog >= 1000 ) {
			common->Printf( "back end %i ms, %i surfaces, %i draws, %i shadow draws, images %u MB, vertexes %u MB\n", backEnd.pc.msec,
				backEnd.pc.c_surfaces, backEnd.pc.c_drawElements, backEnd.pc.c_shadowElements, dkr.imageHeap.BytesUsed() >> 20, dkr.vertexHeap.BytesUsed() >> 20 );
			lastLog = now;
		}
	}
}

/*
===============================================================================
Render targets
===============================================================================
*/
idRenderTexture::idRenderTexture( idImage *colorImage, idImage *depthImage_ ) {
	deviceHandle = 0;
	deviceHandleGeneration = 0;
	validatedCubeFaces = 0;
	cachedDepthHandle = 0;
	cachedDepthGeneration = 0;
	depthImage = depthImage_;
	if ( colorImage ) {
		AddRenderImage( colorImage );
	}
}

idRenderTexture::~idRenderTexture() {
}

bool idRenderTexture::Resize( int width, int height ) {
	if ( GetWidth() == width && GetHeight() == height ) {
		return true;
	}
	for ( int i = 0; i < colorImages.Num(); i++ ) {
		colorImages[i]->Resize( width, height );
	}
	if ( depthImage ) {
		depthImage->Resize( width, height );
	}
	return true;
}

bool idRenderTexture::EnsureDeviceHandle( void ) {
	return true;
}

bool idRenderTexture::MakeCurrent( void ) {
	return MakeCurrent( 0 );
}

bool idRenderTexture::MakeCurrent( int cubeFace ) {
	return Dk_BindImageTarget( colorImages.Num() > 0 ? colorImages[0] : NULL, depthImage, cubeFace );
}

void idRenderTexture::BindNull( void ) {
	Dk_BindScreenTarget();
}

unsigned int idRenderTexture::GetDeviceHandle( void ) {
	return deviceHandle;
}

void idRenderTexture::SetDebugLabel( const char *label ) {
	debugLabel = label ? label : "";
}

void idRenderTexture::AddRenderImage( idImage *image ) {
	colorImages.Append( image );
}

bool idRenderTexture::InitRenderTexture( void ) {
	return true;
}

static idRenderTexture *s_defaultRenderTarget = NULL;

void R_SetDefaultRenderTarget( idRenderTexture *target ) {
	s_defaultRenderTarget = target;
}

idRenderTexture *R_GetDefaultRenderTarget( void ) {
	return s_defaultRenderTarget;
}

unsigned int R_DefaultFramebufferHandle( void ) {
	return 0;
}

unsigned int R_DefaultColorBuffer( void ) {
	return 0;
}

void R_SetDefaultDrawAndReadBuffers( void ) {
}

void R_SetDefaultRenderTargetPremultiplied( bool premultiplied ) {
}

bool R_PremultipliedDefaultTargetBound( void ) {
	return false;
}

/*
===============================================================================
Link surface
===============================================================================
*/
bool RB_ShadowMapBuildArb2ParityState( const viewLight_t *vLight, const viewDef_t *viewDef, int shadowMapSize, shadowMapArb2ParityState_t &state ) {
	(void)vLight; (void)viewDef; (void)shadowMapSize;
	memset( &state, 0, sizeof( state ) );
	return false;
}

bool RB_ShadowMapEstimateArb2CacheOwnership( const viewLight_t *vLight, const viewDef_t *viewDef, shadowMapArb2CacheEstimate_t &estimate ) {
	(void)vLight; (void)viewDef;
	memset( &estimate, 0, sizeof( estimate ) );
	return false;
}

bool RB_ShadowMapProjectedAtlasSlotForLight( const viewLight_t *vLight,
		const viewDef_t *viewDef, shadowMapArb2AtlasSlot_t &slot ) {
	(void)vLight; (void)viewDef;
	memset( &slot, 0, sizeof( slot ) );
	return false;
}

bool RB_ShadowMapProjectedAtlasSlotMarkUsed( int lightDefIndex,
		int signature, std::uint64_t storageGeneration,
		int cellX, int cellY, int cellSpan ) {
	(void)lightDefIndex; (void)signature; (void)cellX; (void)cellY;
	(void)storageGeneration; (void)cellSpan;
	return false;
}

bool RB_ShadowMapPointCubeForLight( const viewLight_t *vLight,
		const viewDef_t *viewDef, shadowMapArb2PointCube_t &cube ) {
	(void)vLight; (void)viewDef;
	memset( &cube, 0, sizeof( cube ) );
	return false;
}

void RB_ShadowMapPointCubeMarkUsed( int lightDefIndex ) {
	(void)lightDefIndex;
}

bool RB_ModernShadowMapsBegin( const viewDef_t *, int, int, int, int ) { return false; }
bool RB_ModernShadowMapRender( const viewDef_t *, struct modernShadowLightDescriptor_s & ) { return false; }
bool RB_ModernShadowMapBindings( rendererShadowTextureBindings_t & ) { return false; }
void RB_ModernShadowMapsShutdown() {}

bool RB_ShadowMapArb2ReceiverFallbackSelfTest( void ) {
		return true;
}

// --- GL debug scope/labels ---
idGLDebugScope::idGLDebugScope( const char *name, unsigned int id ) {
	(void)name; (void)id;
}

idGLDebugScope::~idGLDebugScope() {
}

void R_GLDebug_LabelBuffer( unsigned int name, const char *label ) {
	(void)name; (void)label;
}

void R_GLDebug_LabelTexture( unsigned int name, const char *label ) {
	(void)name; (void)label;
}

void R_GLDebug_LabelProgram( unsigned int name, const char *label ) {
	(void)name; (void)label;
}

void R_GLDebug_LabelVertexArray( unsigned int name, const char *label ) {
	(void)name; (void)label;
}

// --- GL state cache ---
idGLStateCache::idGLStateCache() {
}

bool idGLStateCache::UseProgram( GLuint program ) { (void)program; return false; }
bool idGLStateCache::BindVertexArray( GLuint vertexArray ) { (void)vertexArray; return false; }
bool idGLStateCache::BindBuffer( GLenum target, GLuint buffer ) { (void)target; (void)buffer; return false; }
bool idGLStateCache::BindBufferBase( GLenum target, GLuint index, GLuint buffer ) { (void)target; (void)index; (void)buffer; return false; }
bool idGLStateCache::BindBuffersBase( GLenum target, GLuint first, GLsizei count, const GLuint *buffers ) { (void)target; (void)first; (void)count; (void)buffers; return false; }
bool idGLStateCache::ActiveTextureUnit( int unit ) { (void)unit; return false; }
bool idGLStateCache::BindTexture( int unit, GLenum target, GLuint texture ) { (void)unit; (void)target; (void)texture; return false; }
bool idGLStateCache::BindFramebuffer( GLenum target, GLuint framebuffer ) { (void)target; (void)framebuffer; return false; }
bool idGLStateCache::SetBlendEnabled( bool enabled ) { (void)enabled; return false; }
bool idGLStateCache::SetDepthTestEnabled( bool enabled ) { (void)enabled; return false; }
bool idGLStateCache::SetDepthMask( GLboolean mask ) { (void)mask; return false; }
bool idGLStateCache::SetStencilTestEnabled( bool enabled ) { (void)enabled; return false; }
bool idGLStateCache::SetScissorTestEnabled( bool enabled ) { (void)enabled; return false; }
bool idGLStateCache::SetCullFaceEnabled( bool enabled ) { (void)enabled; return false; }
bool idGLStateCache::SetViewport( GLint x, GLint y, GLsizei width, GLsizei height ) { (void)x; (void)y; (void)width; (void)height; return false; }
bool idGLStateCache::SetScissor( GLint x, GLint y, GLsizei width, GLsizei height ) { (void)x; (void)y; (void)width; (void)height; return false; }
bool idGLStateCache::SetColorMask( GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha ) { (void)red; (void)green; (void)blue; (void)alpha; return false; }

idGLStateCache &R_GLStateCache( void ) {
	static idGLStateCache cache;
	return cache;
}

void R_GLStateCache_InvalidateAll( const char *reason ) {
	(void)reason;
}

// --- GL shader library ---
const modernGLShaderLibraryStats_t &R_ModernGLShaderLibrary_Stats( void ) {
	static modernGLShaderLibraryStats_t stats;
	memset( &stats, 0, sizeof( stats ) );
	return stats;
}

const modernGLShaderProgramInfo_t *R_ModernGLShaderLibrary_FindProgram( modernGLShaderProgramKind_t kind, int preferredGLSLVersion ) {
	(void)kind; (void)preferredGLSLVersion;
	return NULL;
}

// --- upload ring stats ---
bool R_RendererUpload_QueryStorage(rendererUploadStorage_t&) { return false; }

const rendererUploadStats_t &R_RendererUpload_Stats( void ) {
	static rendererUploadStats_t stats;
	memset( &stats, 0, sizeof( stats ) );
	return stats;
}



void RB_ResetARB2InteractionHandoffBreadcrumb( void ) {
}

void RB_ResetAppleGL21RouteCounters( void ) {
}

void RB_ReportAppleGL21RouteCounters( void ) {
}

idImage *RB_ResolveGLSLShaderTextureImage( const newShaderStage_t *stage,
		int slot, const drawInteraction_t *din ) {
	if ( stage == NULL || slot < 0 || slot >= stage->numShaderTextures ) {
		return NULL;
	}

	switch ( stage->shaderTextureBindings[ slot ] ) {
		case GLSL_SHADERTEXTURE_LIGHT_FALLOFF:
			return din != NULL && din->lightFalloffImage != NULL
					? din->lightFalloffImage : globalImages->whiteImage;
		case GLSL_SHADERTEXTURE_LIGHT_IMAGE:
			return din != NULL && din->lightImage != NULL
					? din->lightImage : globalImages->whiteImage;
		case GLSL_SHADERTEXTURE_AMBIENT_NORMAL_MAP:
			return globalImages->ambientNormalMap != NULL
					? globalImages->ambientNormalMap : globalImages->defaultImage;
		case GLSL_SHADERTEXTURE_NORMAL_CUBE_MAP:
			return globalImages->normalCubeMapImage != NULL
					? globalImages->normalCubeMapImage : globalImages->defaultImage;
		case GLSL_SHADERTEXTURE_SPECULAR_TABLE:
			return globalImages->specularTableImage != NULL
					? globalImages->specularTableImage : globalImages->defaultImage;
		case GLSL_SHADERTEXTURE_IMAGE:
		default:
			if ( din != NULL && RB_FlatDiffuseSurfaceActive( din->surf )
					&& idStr::Icmp( stage->shaderTextureNames[ slot ], "DiffuseMap" ) == 0 ) {
				return globalImages->whiteImage;
			}
			return stage->shaderTextureImages[ slot ];
	}
}

// KHR_debug output layer
bool R_GLDebugOutput_Available( void ) {
	return false;
}

bool R_GLDebugOutput_Registered( void ) {
	return false;
}

void R_GLDebugOutput_Init( void ) {
}

void R_GLDebugOutput_Shutdown( void ) {
}

void R_GLDebugOutput_FlushMessages( void ) {
}

modernLightAtlasReject_t R_ModernLightImageAtlas_Acquire( const idImage *image, float rect[4] ) {
	(void)image;
	if ( rect != NULL ) {
		rect[0] = rect[1] = rect[2] = rect[3] = 0.0f;
	}
	return MODERN_LIGHT_ATLAS_REJECT_UNAVAILABLE;
}

// modern GL executor
void R_ModernGLExecutor_Init( const renderBackendCaps_t &caps, const renderFeatureSet_t &features ) {
	(void)caps; (void)features;
}

void R_ModernGLExecutor_Shutdown( void ) {
}

void R_ModernGLExecutor_InvalidatePlans( void ) {
}

void R_ModernGLExecutor_PrintGfxInfo( void ) {
}

bool R_ModernGLExecutor_LinearScreenshot( const char *fileName ) {
	(void)fileName;
	return false;
}

bool R_ModernGLExecutor_ModernVisibleRequestedForPost( void ) {
	return false;
}

bool R_ModernGLExecutor_PBRLinearSceneActive( void ) { return false; }

const modernGLExecutorStats_t &R_ModernGLExecutor_Stats( void ) {
	static modernGLExecutorStats_t stats;
	memset( &stats, 0, sizeof( stats ) );
	return stats;
}

bool R_ModernGLShaderLibrary_Reload( void ) {
	return false;
}

// render-graph transient resources
void R_RenderGraphResources_Init( const renderBackendCaps_t &caps, const renderFeatureSet_t &features ) {
	(void)caps; (void)features;
}

void R_RenderGraphResources_Shutdown( void ) {
}

void R_RenderGraphResources_PrintGfxInfo( void ) {
	common->Printf( "deko3D memory: images %u MB, vertexes %u MB\n", dkr.imageHeap.BytesUsed() >> 20, dkr.vertexHeap.BytesUsed() >> 20 );
}

void R_RenderGraphResources_DumpLatest( void ) {
}

// upload ring
void R_RendererUpload_Init( const renderBackendCaps_t &caps ) {
	(void)caps;
}

void R_RendererUpload_Shutdown( void ) {
}

void R_RendererUpload_BeginFrame( int frameCount ) {
	(void)frameCount;
}

void R_RendererUpload_EndFrame( void ) {
}

bool R_RendererUpload_AllocFrameTemp( void *data, int bytes, int alignment, rendererUploadAllocation_t &allocation ) {
	(void)data; (void)bytes; (void)alignment;
	memset( &allocation, 0, sizeof( allocation ) );
	return false;
}

bool R_RendererUpload_AllocStaticBuffer( void *data, int bytes, bool indexBuffer, bool dynamic, unsigned int &handle ) {
	(void)data; (void)bytes; (void)indexBuffer; (void)dynamic;
	handle = 0;
	return false;
}

void R_RendererUpload_FreeStaticBuffer( unsigned int &handle, int bytes, bool indexBuffer ) {
	(void)bytes; (void)indexBuffer;
	handle = 0;
}

bool R_RendererUpload_DynamicFrameBridgeAvailable( void ) {
	return false;
}

int R_RendererUpload_FrameCapacity( void ) {
	return 0;
}

void R_RendererUpload_RecordLegacyStall( void ) {
}

void R_RendererUpload_RecordLegacyUpload( int bytes ) {
	(void)bytes;
}

bool RB_ShadowMapResourcesKnownGood( bool pointLight ) {
	(void)pointLight;
	return false;
}

// shadow-map / post-process resource teardown hooks

void RB_ShutdownShadowMapResources( void ) {
}

void RB_ShutdownScenePostProcess( void ) {
}

void RB_ShowImages( void ) {
}

// GL-subsystem self-test commands
#define DK_SELFTEST_STUB( name ) \
	bool name( void ) { \
		common->Printf( #name ": not applicable on deko3D\n" ); \
		return true; \
	}

DK_SELFTEST_STUB( RendererDeferredResolve_RunSelfTest )
DK_SELFTEST_STUB( RendererForwardPlus_RunSelfTest )
DK_SELFTEST_STUB( RendererGBuffer_RunSelfTest )
DK_SELFTEST_STUB( RendererPBRVisible_RunSelfTest )
DK_SELFTEST_STUB( RendererGLStateCache_RunSelfTest )
DK_SELFTEST_STUB( RendererGpuDriven_RunSelfTest )
DK_SELFTEST_STUB( RendererLightImageAtlas_RunSelfTest )
DK_SELFTEST_STUB( RendererLowOverhead_RunSelfTest )
DK_SELFTEST_STUB( RendererModernCompatibility_RunSelfTest )
DK_SELFTEST_STUB( RendererModernGLExecutor_RunSelfTest )
DK_SELFTEST_STUB( RendererModernGLShaderLibrary_RunSelfTest )
DK_SELFTEST_STUB( RendererModernVisibility_RunSelfTest )
DK_SELFTEST_STUB( RendererModernVisible_RunSelfTest )
DK_SELFTEST_STUB( RendererPassOwnership_RunSelfTest )
DK_SELFTEST_STUB( RendererRenderGraphResource_RunSelfTest )
DK_SELFTEST_STUB( RendererUpload_RunSelfTest )
DK_SELFTEST_STUB( RendererVisiblePath_RunSelfTest )

/*
================================================================================
What other back ends have, and this one does not ask for
================================================================================
*/
const renderWindowServices_t *Sys_GetRenderWindowServices( void ) {
	return NULL;	// no window to resize or move
}

bool R_VR_BackendCanPresent( void ) {
	return false;
}

bool R_VR_QueryGraphicsBinding( renderVRGraphicsBinding_t &binding ) {
	return false;
}

void R_VR_ShutdownTargets( void ) {
}

void R_BackendGpuSkinning_Init( const renderBackendCaps_t &caps ) {
}

void R_BackendGpuSkinning_Shutdown( void ) {
}

bool R_BackendGpuSkinning_PrepareAmbientCache( srfTriangles_t *tri, bool needsLighting ) {
	return false;
}

void R_BackendGpuSkinning_PrintGfxInfo( void ) {
}

bool RB_UnderwaterViewAvailable( void ) {
	return false;
}

bool RB_DrawSurfHasSoftParticleStage( const drawSurf_t *surf ) {
	return false;	// no depth texture to sample
}
