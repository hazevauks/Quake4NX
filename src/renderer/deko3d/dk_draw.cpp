/*
===========================================================================

Quake4NX - native deko3D renderer: drawing a view.

The passes are the original renderer's, in its order: depth, then for each light
stencil shadows and interactions, the light scale, ambient stages, fog and blend
lights, post process stages.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../tr_local.h"

#undef BIT
#include <switch.h>

#include "dk_local.h"

static const float *	s_projection;		// current projection, the view's or a depth hack's
static float			s_hackProjection[16];
static float			s_minDepth = 0.0f;
static float			s_maxDepth = 1.0f;
static bool				s_depthOnlyFill = false;	// the color was cleared to black, so the depth pass writes no color

static const float IDENTITY_MATRIX[16] = {
	1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0,
	0, 0, 0, 1
};

/*
================================================================================
Surfaces and matrices
================================================================================
*/
static void Dk_ApplyViewport( void ) {
	Dk_SetViewport( tr.viewportOffset[0] + backEnd.viewDef->viewport.x1, tr.viewportOffset[1] + backEnd.viewDef->viewport.y1,
		backEnd.viewDef->viewport.x2 + 1 - backEnd.viewDef->viewport.x1,
		backEnd.viewDef->viewport.y2 + 1 - backEnd.viewDef->viewport.y1, s_minDepth, s_maxDepth );
}

static ID_INLINE void Dk_ApplyScissor( const idScreenRect &rect ) {
	Dk_SetScissor( backEnd.viewDef->viewport.x1 + rect.x1, backEnd.viewDef->viewport.y1 + rect.y1,
		rect.x2 + 1 - rect.x1, rect.y2 + 1 - rect.y1 );
}

static ID_INLINE void Dk_ScissorForSurface( const drawSurf_t *surf ) {
	if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
		backEnd.currentScissor = surf->scissorRect;
		Dk_ApplyScissor( backEnd.currentScissor );
	}
}

static ID_INLINE void Dk_LoadSpace( const viewEntity_t *space ) {
	Dk_SetMvp( space->modelViewMatrix, s_projection );
}

static float RB_CalcFovForAspect( float fovX, float width, float height ) {
	const float x = width / idMath::Tan( DEG2RAD( fovX ) * 0.5f );
	const float y = idMath::ATan( height, x );
	return RAD2DEG( y * 2.0f );
}

void RB_EnterWeaponDepthHack( void ) {
	memcpy( s_hackProjection, backEnd.viewDef->projectionMatrix, sizeof( s_hackProjection ) );

	const float weaponFovOverride = cl_gunfov.GetFloat();
	if ( weaponFovOverride > 0.0f ) {
		const float viewportWidth = (float)Max( 1, backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1 );
		const float viewportHeight = (float)Max( 1, backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1 );

		float weaponFovX = idMath::ClampFloat( 30.0f, 160.0f, weaponFovOverride );
		float weaponFovY = 0.0f;
		if ( cl_gunfov_adjust.GetBool() ) {
			weaponFovY = RB_CalcFovForAspect( weaponFovX, 4.0f, 3.0f );
			weaponFovX = RB_CalcFovForAspect( weaponFovY, viewportHeight, viewportWidth );
		} else {
			weaponFovY = RB_CalcFovForAspect( weaponFovX, viewportWidth, viewportHeight );
		}
		weaponFovX = idMath::ClampFloat( 1.0f, 179.0f, weaponFovX );
		weaponFovY = idMath::ClampFloat( 1.0f, 179.0f, weaponFovY );
		s_hackProjection[0] = 1.0f / idMath::Tan( DEG2RAD( weaponFovX ) * 0.5f );
		s_hackProjection[5] = 1.0f / idMath::Tan( DEG2RAD( weaponFovY ) * 0.5f );
	}

	s_hackProjection[14] *= 0.25f;
	s_projection = s_hackProjection;
	s_minDepth = 0.0f;
	s_maxDepth = 0.5f;
	Dk_ApplyViewport();
}

void RB_EnterModelDepthHack( float depth ) {
	memcpy( s_hackProjection, backEnd.viewDef->projectionMatrix, sizeof( s_hackProjection ) );
	s_hackProjection[14] -= depth;
	s_projection = s_hackProjection;
}

void RB_LeaveDepthHack( void ) {
	s_projection = backEnd.viewDef->projectionMatrix;
	if ( s_maxDepth != 1.0f ) {
		s_maxDepth = 1.0f;
		Dk_ApplyViewport();
	}
}

/*
================
Draw calls
================
*/
static ID_INLINE DkGpuAddr Dk_CacheAddr( vertCache_t *cache ) {
	return (DkGpuAddr)vertexCache.Position( cache );
}

// indexes go to the GPU's memory once, or every frame for surfaces that change
static ID_INLINE DkGpuAddr Dk_IndexAddr( const srfTriangles_t *tri, int numIndexes ) {
	if ( tri->indexCache ) {
		return Dk_CacheAddr( tri->indexCache );
	}
	vertCache_t *temp = vertexCache.AllocFrameTemp( tri->indexes, numIndexes * sizeof( glIndex_t ) );
	return temp ? temp->gpuAddr : 0;
}

void RB_DrawElementsWithCounters( const srfTriangles_t *tri ) {
	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += tri->numIndexes;
	backEnd.pc.c_drawVertexes += tri->numVerts;

	const DkGpuAddr indexes = Dk_IndexAddr( tri, tri->numIndexes );
	if ( indexes ) {
		Dk_DrawIndexed( indexes, r_singleTriangle.GetBool() ? 3 : tri->numIndexes );
	}
}

void RB_DrawShadowElementsWithCounters( const srfTriangles_t *tri, int numIndexes ) {
	backEnd.pc.c_shadowElements++;
	backEnd.pc.c_shadowIndexes += numIndexes;
	backEnd.pc.c_shadowVertexes += tri->numVerts;

	const DkGpuAddr indexes = Dk_IndexAddr( tri, tri->numIndexes );
	if ( indexes ) {
		Dk_DrawIndexed( indexes, r_singleTriangle.GetBool() ? 3 : numIndexes );
	}
}

static ID_INLINE void Dk_BindDrawVerts( const srfTriangles_t *tri ) {
	Dk_SetVertexFormat( VFMT_DRAWVERT );
	Dk_BindVertexBuffer( 0, Dk_CacheAddr( tri->ambientCache ), tri->ambientCache->size );
}

void RB_RenderTriangleSurface( const srfTriangles_t *tri ) {
	if ( !tri->ambientCache ) {
		return;
	}
	Dk_BindDrawVerts( tri );
	RB_DrawElementsWithCounters( tri );
}

void RB_T_RenderTriangleSurface( const drawSurf_t *surf ) {
	RB_RenderTriangleSurface( surf->geo );
}

/*
================
RB_RenderDrawSurfListWithFunction

The triangle functions can check backEnd.currentSpace != surf->space
to see if they need to perform any new matrix setup.
================
*/
static ID_INLINE void Dk_PrepareSurface( const drawSurf_t *drawSurf ) {
	const bool spaceChanged = drawSurf->space != backEnd.currentSpace;
	const bool hacked = drawSurf->space->weaponDepthHack || drawSurf->space->modelDepthHack != 0.0f;

	if ( drawSurf->space->weaponDepthHack ) {
		RB_EnterWeaponDepthHack();
	}
	if ( drawSurf->space->modelDepthHack != 0.0f ) {
		RB_EnterModelDepthHack( drawSurf->space->modelDepthHack );
	}
	if ( spaceChanged || hacked ) {
		Dk_LoadSpace( drawSurf->space );
	}
	Dk_ScissorForSurface( drawSurf );
}

static ID_INLINE void Dk_FinishSurface( const drawSurf_t *drawSurf ) {
	if ( drawSurf->space->weaponDepthHack || drawSurf->space->modelDepthHack != 0.0f ) {
		RB_LeaveDepthHack();
		Dk_LoadSpace( drawSurf->space );
	}
	backEnd.currentSpace = drawSurf->space;
}

void RB_RenderDrawSurfListWithFunction( drawSurf_t **drawSurfs, int numDrawSurfs, void (*triFunc_)( const drawSurf_t * ) ) {
	backEnd.currentSpace = NULL;
	for ( int i = 0; i < numDrawSurfs; i++ ) {
		Dk_PrepareSurface( drawSurfs[i] );
		triFunc_( drawSurfs[i] );
		Dk_FinishSurface( drawSurfs[i] );
	}
}

void RB_RenderDrawSurfChainWithFunction( const drawSurf_t *drawSurfs, void (*triFunc_)( const drawSurf_t * ) ) {
	backEnd.currentSpace = NULL;
	for ( const drawSurf_t *drawSurf = drawSurfs; drawSurf; drawSurf = drawSurf->nextOnLight ) {
		Dk_PrepareSurface( drawSurf );
		triFunc_( drawSurf );
		Dk_FinishSurface( drawSurf );
	}
}

/*
================
Texture matrices
================
*/
void RB_GetShaderTextureMatrix( const float *shaderRegisters, const textureStage_t *texture, float matrix[16] ) {
	matrix[0] = shaderRegisters[ texture->matrix[0][0] ];
	matrix[4] = shaderRegisters[ texture->matrix[0][1] ];
	matrix[8] = 0;
	matrix[12] = shaderRegisters[ texture->matrix[0][2] ];

	// we attempt to keep scrolls from generating incredibly large texture values, but
	// center rotations and center scales can still generate offsets that need to be > 1
	if ( matrix[12] < -40 || matrix[12] > 40 ) {
		matrix[12] -= (int)matrix[12];
	}

	matrix[1] = shaderRegisters[ texture->matrix[1][0] ];
	matrix[5] = shaderRegisters[ texture->matrix[1][1] ];
	matrix[9] = 0;
	matrix[13] = shaderRegisters[ texture->matrix[1][2] ];
	if ( matrix[13] < -40 || matrix[13] > 40 ) {
		matrix[13] -= (int)matrix[13];
	}

	matrix[2] = 0;
	matrix[6] = 0;
	matrix[10] = 1;
	matrix[14] = 0;

	matrix[3] = 0;
	matrix[7] = 0;
	matrix[11] = 0;
	matrix[15] = 1;
}

void RB_BakeTextureMatrixIntoTexgen( idPlane lightProject[3], const float *textureMatrix ) {
	float genMatrix[16];
	float final[16];

	genMatrix[0] = lightProject[0][0];
	genMatrix[4] = lightProject[0][1];
	genMatrix[8] = lightProject[0][2];
	genMatrix[12] = lightProject[0][3];

	genMatrix[1] = lightProject[1][0];
	genMatrix[5] = lightProject[1][1];
	genMatrix[9] = lightProject[1][2];
	genMatrix[13] = lightProject[1][3];

	genMatrix[2] = 0;
	genMatrix[6] = 0;
	genMatrix[10] = 0;
	genMatrix[14] = 0;

	genMatrix[3] = lightProject[2][0];
	genMatrix[7] = lightProject[2][1];
	genMatrix[11] = lightProject[2][2];
	genMatrix[15] = lightProject[2][3];

	myGlMultMatrix( genMatrix, backEnd.lightTextureMatrix, final );

	lightProject[0][0] = final[0];
	lightProject[0][1] = final[4];
	lightProject[0][2] = final[8];
	lightProject[0][3] = final[12];

	lightProject[1][0] = final[1];
	lightProject[1][1] = final[5];
	lightProject[1][2] = final[9];
	lightProject[1][3] = final[13];
}

void RB_BindVariableStageImage( const textureStage_t *texture, const float *shaderRegisters ) {
	if ( texture->cinematic ) {
		if ( r_skipDynamicTextures.GetBool() ) {
			globalImages->defaultImage->Bind();
			return;
		}
		// offset time by shaderParm[7] (FIXME: make the time offset a parameter of the shader?)
		const cinData_t cin = texture->cinematic->ImageForTime( (int)( 1000 * ( backEnd.viewDef->floatTime + backEnd.viewDef->renderView.shaderParms[11] ) ) );
		if ( cin.image ) {
			globalImages->cinematicImage->UploadScratch( cin.image, cin.imageWidth, cin.imageHeight );
		} else {
			globalImages->blackImage->Bind();
		}
	} else if ( texture->image ) {
		texture->image->Bind();
	}
}

/*
================
RB_BeginDrawingView

Any mirrored or portaled views have already been drawn, so prepare
to actually render the visible surfaces for this view
================
*/
void RB_BeginDrawingView( void ) {
	s_projection = backEnd.viewDef->projectionMatrix;
	s_minDepth = 0.0f;
	s_maxDepth = 1.0f;
	Dk_ApplyViewport();
	Dk_ApplyScissor( backEnd.viewDef->scissor );
	backEnd.currentScissor = backEnd.viewDef->scissor;

	// ensures that depth writes are enabled for the depth clear
	GL_State( GLS_DEFAULT );

	if ( backEnd.viewDef->viewEntitys ) {
		Dk_FlushForDraw();
		Dk_Clear( false, NULL, true, true, 128 );
		Dk_SetDepthTest( true );
	} else {
		Dk_SetDepthTest( false );
		Dk_SetStencilTest( false );
	}

	backEnd.glState.faceCulling = -1;		// force face culling to set next time
	GL_Cull( CT_FRONT_SIDED );
}

/*
================================================================================
Depth
================================================================================
*/
static void RB_T_FillDepthBuffer( const drawSurf_t *surf ) {
	const srfTriangles_t *tri = surf->geo;
	const idMaterial *shader = surf->material;

	if ( !shader->IsDrawn() || !tri->numIndexes || shader->Coverage() == MC_TRANSLUCENT || !tri->ambientCache ) {
		return;
	}

	// a sky surface is a mask for the sky camera's view, drawn before this one: those pixels stay as they are
	if ( shader->IsPortalSky() || shader->GetSort() == SS_PORTAL_SKY ) {
		return;
	}

	const float *regs = surf->shaderRegisters;
	int stage;
	for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
		if ( regs[ shader->GetStage( stage )->conditionRegister ] != 0 ) {
			break;
		}
	}
	if ( stage == shader->GetNumStages() ) {
		return;
	}

	if ( backEnd.viewDef->numClipPlanes && surf->space != backEnd.currentSpace ) {
		idPlane plane;
		R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.viewDef->clipPlanes[0], plane );
		Dk_SetVsLocal( FFV_PLANE0, plane.ToFloatPtr() );
	}

	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		Dk_SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
	}

	// subviews will just down-modulate the color buffer by overbright
	float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const bool isSubview = shader->GetSort() == SS_SUBVIEW;
	if ( isSubview ) {
		GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS );
		color[0] = color[1] = color[2] = 1.0f / backEnd.overBright;
	}

	Dk_BindDrawVerts( tri );

	bool drawSolid = shader->Coverage() == MC_OPAQUE;
	if ( shader->Coverage() == MC_PERFORATED ) {
		// perforated surfaces may have multiple alpha tested stages
		bool didDraw = false;
		Dk_UseFixedProgram( DKP_DEPTH_ALPHA );
		for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
			const shaderStage_t *pStage = shader->GetStage( stage );
			if ( !pStage->hasAlphaTest || regs[ pStage->conditionRegister ] == 0 ) {
				continue;
			}
			didDraw = true;

			color[3] = regs[ pStage->color.registers[3] ];
			if ( color[3] <= 0 ) {
				continue;
			}
			const float alphaRef[4] = { regs[ pStage->alphaTestRegister ], 0.0f, 0.0f, 0.0f };
			Dk_SetFsLocal( 0, color );
			Dk_SetFsLocal( 1, alphaRef );

			float s[4] = { 1, 0, 0, 0 };
			float t[4] = { 0, 1, 0, 0 };
			if ( pStage->texture.hasMatrix ) {
				float m[16];
				RB_GetShaderTextureMatrix( regs, &pStage->texture, m );
				s[0] = m[0]; s[1] = m[4]; s[3] = m[12];
				t[0] = m[1]; t[1] = m[5]; t[3] = m[13];
			}
			Dk_SetVsLocal( FFV_TEX_S, s );
			Dk_SetVsLocal( FFV_TEX_T, t );

			GL_SelectTexture( 0 );
			pStage->texture.image->Bind();

			if ( pStage->privatePolygonOffset ) {
				Dk_SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
			}
			RB_DrawElementsWithCounters( tri );
			if ( pStage->privatePolygonOffset && !shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
				Dk_SetPolygonOffset( false, 0, 0 );
			}
		}
		if ( !didDraw ) {
			drawSolid = true;
		}
	}

	if ( drawSolid ) {
		const bool clipped = backEnd.viewDef->numClipPlanes != 0;
		if ( s_depthOnlyFill && !isSubview ) {
			Dk_UseFixedProgram( clipped ? DKP_DEPTH_CLIP : DKP_DEPTH );
		} else {
			Dk_SetFsLocal( 0, color );
			Dk_UseFixedProgram( clipped ? DKP_COLOR_CLIP : DKP_COLOR );
		}
		RB_DrawElementsWithCounters( tri );
	}

	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		Dk_SetPolygonOffset( false, 0, 0 );
	}
	if ( isSubview ) {
		GL_State( s_depthOnlyFill ? ( GLS_DEPTHFUNC_LESS | GLS_COLORMASK | GLS_ALPHAMASK ) : GLS_DEPTHFUNC_LESS );
	}
}

void RB_STD_FillDepthBuffer( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	// if we are just doing 2D rendering, no need to fill the depth buffer
	if ( !backEnd.viewDef->viewEntitys ) {
		return;
	}

	GL_SelectTexture( 0 );

	// Subviews and the sky camera already drew to the screen, where the surfaces that show them are.
	// With none in sight the screen can be cleared, and this pass needs no color at all.
	s_depthOnlyFill = true;
	for ( int i = 0; i < numDrawSurfs; i++ ) {
		const idMaterial *material = drawSurfs[i]->material;
		if ( material->GetSort() == SS_SUBVIEW || material->GetSort() == SS_PORTAL_SKY || material->IsPortalSky() ) {
			s_depthOnlyFill = false;
			break;
		}
		if ( material->GetSort() > SS_PORTAL_SKY ) {
			break;
		}
	}
	if ( s_depthOnlyFill ) {
		static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		GL_State( GLS_DEPTHFUNC_LESS | GLS_COLORMASK | GLS_ALPHAMASK );
		Dk_FlushForDraw();
		Dk_Clear( true, black, false, false, 0 );
	} else {
		GL_State( GLS_DEPTHFUNC_LESS );
	}

	if ( !backEnd.viewDef->numClipPlanes ) {
		const float noClip[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		Dk_SetVsLocal( FFV_PLANE0, noClip );
	}

	// Stencil test on so the ambient pass and the light passes cannot z-fight.
	Dk_SetStencilTest( true );
	Dk_SetStencilOps( DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Keep );
	Dk_SetStencilFunc( DkCompareOp_Always, 1, 255 );

	RB_RenderDrawSurfListWithFunction( drawSurfs, numDrawSurfs, RB_T_FillDepthBuffer );
}

/*
================================================================================
Interactions
================================================================================
*/
static void Dk_SetInteractionEnv( const drawInteraction_t *din, stageVertexColor_t vertexColor ) {
	Dk_SetVsEnv( PP_LIGHT_ORIGIN, din->localLightOrigin.ToFloatPtr() );
	Dk_SetVsEnv( PP_VIEW_ORIGIN, din->localViewOrigin.ToFloatPtr() );
	Dk_SetVsEnv( PP_LIGHT_PROJECT_S, din->lightProjection[0].ToFloatPtr() );
	Dk_SetVsEnv( PP_LIGHT_PROJECT_T, din->lightProjection[1].ToFloatPtr() );
	Dk_SetVsEnv( PP_LIGHT_PROJECT_Q, din->lightProjection[2].ToFloatPtr() );
	Dk_SetVsEnv( PP_LIGHT_FALLOFF_S, din->lightProjection[3].ToFloatPtr() );
	Dk_SetVsEnv( PP_BUMP_MATRIX_S, din->bumpMatrix[0].ToFloatPtr() );
	Dk_SetVsEnv( PP_BUMP_MATRIX_T, din->bumpMatrix[1].ToFloatPtr() );
	Dk_SetVsEnv( PP_DIFFUSE_MATRIX_S, din->diffuseMatrix[0].ToFloatPtr() );
	Dk_SetVsEnv( PP_DIFFUSE_MATRIX_T, din->diffuseMatrix[1].ToFloatPtr() );
	Dk_SetVsEnv( PP_SPECULAR_MATRIX_S, din->specularMatrix[0].ToFloatPtr() );
	Dk_SetVsEnv( PP_SPECULAR_MATRIX_T, din->specularMatrix[1].ToFloatPtr() );

	// Prey's interaction programs pack the vertex color mode in env[16].xy
	float colorMode[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
	if ( vertexColor == SVC_MODULATE ) {
		colorMode[0] = 1.0f;
		colorMode[1] = 0.0f;
	} else if ( vertexColor == SVC_INVERSE_MODULATE ) {
		colorMode[0] = -1.0f;
		colorMode[1] = 1.0f;
	}
	Dk_SetVsEnv( PP_COLOR_MODULATE, colorMode );
}

static void RB_DrawInteraction( const drawInteraction_t *din ) {
	Dk_SetInteractionEnv( din, din->vertexColor );
	Dk_SetFsEnv( 0, din->diffuseColor.ToFloatPtr() );
	Dk_SetFsEnv( 1, din->specularColor.ToFloatPtr() );

	GL_SelectTextureNoClient( 1 );
	din->bumpImage->Bind();
	GL_SelectTextureNoClient( 2 );
	din->lightFalloffImage->Bind();
	GL_SelectTextureNoClient( 3 );
	din->lightImage->Bind();
	GL_SelectTextureNoClient( 4 );
	din->diffuseImage->Bind();
	GL_SelectTextureNoClient( 5 );
	din->specularImage->Bind();

	RB_DrawElementsWithCounters( din->surf->geo );
}

static void RB_SubmitInteraction( drawInteraction_t *din ) {
	if ( !din->bumpImage ) {
		return;
	}
	if ( !din->diffuseImage || r_skipDiffuse.GetBool() ) {
		din->diffuseImage = globalImages->blackImage;
	}
	if ( !din->specularImage || r_skipSpecular.GetBool() || din->ambientLight ) {
		din->specularImage = globalImages->blackImage;
	}
	if ( r_skipBump.GetBool() ) {
		din->bumpImage = globalImages->flatNormalMap;
	}

	// if we wouldn't draw anything, don't call the Draw function
	if ( ( ( din->diffuseColor[0] > 0 || din->diffuseColor[1] > 0 || din->diffuseColor[2] > 0 ) && din->diffuseImage != globalImages->blackImage )
		|| ( ( din->specularColor[0] > 0 || din->specularColor[1] > 0 || din->specularColor[2] > 0 ) && din->specularImage != globalImages->blackImage ) ) {
		RB_DrawInteraction( din );
	}
}

/*
==================
R_SetDrawInteraction
==================
*/
static void R_SetDrawInteraction( const shaderStage_t *surfaceStage, const float *surfaceRegs, idImage **image, idVec4 matrix[2], float color[4] ) {
	*image = surfaceStage->texture.image;
	if ( surfaceStage->texture.hasMatrix ) {
		matrix[0][0] = surfaceRegs[surfaceStage->texture.matrix[0][0]];
		matrix[0][1] = surfaceRegs[surfaceStage->texture.matrix[0][1]];
		matrix[0][2] = 0;
		matrix[0][3] = surfaceRegs[surfaceStage->texture.matrix[0][2]];

		matrix[1][0] = surfaceRegs[surfaceStage->texture.matrix[1][0]];
		matrix[1][1] = surfaceRegs[surfaceStage->texture.matrix[1][1]];
		matrix[1][2] = 0;
		matrix[1][3] = surfaceRegs[surfaceStage->texture.matrix[1][2]];

		// we attempt to keep scrolls from generating incredibly large texture values, but
		// center rotations and center scales can still generate offsets that need to be > 1
		if ( matrix[0][3] < -40 || matrix[0][3] > 40 ) {
			matrix[0][3] -= (int)matrix[0][3];
		}
		if ( matrix[1][3] < -40 || matrix[1][3] > 40 ) {
			matrix[1][3] -= (int)matrix[1][3];
		}
	} else {
		matrix[0][0] = 1;
		matrix[0][1] = 0;
		matrix[0][2] = 0;
		matrix[0][3] = 0;

		matrix[1][0] = 0;
		matrix[1][1] = 1;
		matrix[1][2] = 0;
		matrix[1][3] = 0;
	}

	if ( color ) {
		for ( int i = 0; i < 4; i++ ) {
			// clamp here, so card with greater range don't look different.
			color[i] = idMath::ClampFloat( 0.0f, 1.0f, surfaceRegs[surfaceStage->color.registers[i]] );
		}
	}
}

/*
================
RB_CreateSingleDrawInteractions

Decomposes a complex light / surface interaction into primitive interactions.
================
*/
static void RB_CreateSingleDrawInteractions( const drawSurf_t *surf ) {
	const idMaterial *surfaceShader = surf->material;
	const float *surfaceRegs = surf->shaderRegisters;
	const viewLight_t *vLight = backEnd.vLight;
	const idMaterial *lightShader = vLight->lightShader;
	const float *lightRegs = vLight->shaderRegisters;
	drawInteraction_t inter;

	if ( r_skipInteractions.GetBool() || !surf->geo || !surf->geo->ambientCache ) {
		return;
	}

	const bool hacked = surf->space->weaponDepthHack || surf->space->modelDepthHack != 0.0f;
	if ( surf->space->weaponDepthHack ) {
		RB_EnterWeaponDepthHack();
	}
	if ( surf->space->modelDepthHack ) {
		RB_EnterModelDepthHack( surf->space->modelDepthHack );
	}
	if ( surf->space != backEnd.currentSpace || hacked ) {
		backEnd.currentSpace = surf->space;
		Dk_LoadSpace( surf->space );
	}
	Dk_ScissorForSurface( surf );

	// Lit decals are polygon offset interaction materials: this pass must use the
	// same offset as the depth fill, or its depth test rejects them.
	if ( surfaceShader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		Dk_SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * surfaceShader->GetPolygonOffset() );
	}

	inter.surf = surf;
	inter.lightFalloffImage = vLight->falloffImage;

	R_GlobalPointToLocal( surf->space->modelMatrix, vLight->globalLightOrigin, inter.localLightOrigin.ToVec3() );
	R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.viewDef->renderView.vieworg, inter.localViewOrigin.ToVec3() );
	inter.localLightOrigin[3] = 0;
	inter.localViewOrigin[3] = 1;
	inter.ambientLight = lightShader->IsAmbientLight();

	// the base projections may be modified by texture matrix on light stages
	idPlane lightProject[4];
	for ( int i = 0; i < 4; i++ ) {
		R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[i], lightProject[i] );
	}

	for ( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ ) {
		const shaderStage_t *lightStage = lightShader->GetStage( lightStageNum );

		// ignore stages that fail the condition
		if ( !lightRegs[ lightStage->conditionRegister ] ) {
			continue;
		}

		inter.lightImage = lightStage->texture.image;

		memcpy( inter.lightProjection, lightProject, sizeof( inter.lightProjection ) );
		if ( lightStage->texture.hasMatrix ) {
			RB_GetShaderTextureMatrix( lightRegs, &lightStage->texture, backEnd.lightTextureMatrix );
			RB_BakeTextureMatrixIntoTexgen( reinterpret_cast<class idPlane *>( inter.lightProjection ), backEnd.lightTextureMatrix );
		}

		inter.bumpImage = NULL;
		inter.specularImage = NULL;
		inter.diffuseImage = NULL;
		inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 0;
		inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;

		float lightColor[4];

		// backEnd.lightScale is calculated so that lightColor[] will never exceed
		// tr.backEndRendererMaxLight
		lightColor[0] = backEnd.lightScale * lightRegs[ lightStage->color.registers[0] ];
		lightColor[1] = backEnd.lightScale * lightRegs[ lightStage->color.registers[1] ];
		lightColor[2] = backEnd.lightScale * lightRegs[ lightStage->color.registers[2] ];
		lightColor[3] = lightRegs[ lightStage->color.registers[3] ];

		for ( int surfaceStageNum = 0; surfaceStageNum < surfaceShader->GetNumStages(); surfaceStageNum++ ) {
			const shaderStage_t *surfaceStage = surfaceShader->GetStage( surfaceStageNum );

			switch ( surfaceStage->lighting ) {
				case SL_AMBIENT:
					break;
				case SL_BUMP:
					if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
						break;
					}
					RB_SubmitInteraction( &inter );
					inter.diffuseImage = NULL;
					inter.specularImage = NULL;
					R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.bumpImage, inter.bumpMatrix, NULL );
					break;
				case SL_DIFFUSE:
					if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
						break;
					}
					if ( inter.diffuseImage ) {
						RB_SubmitInteraction( &inter );
					}
					R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.diffuseImage, inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr() );
					inter.diffuseColor[0] *= lightColor[0];
					inter.diffuseColor[1] *= lightColor[1];
					inter.diffuseColor[2] *= lightColor[2];
					inter.diffuseColor[3] *= lightColor[3];
					inter.vertexColor = surfaceStage->vertexColor;
					break;
				case SL_SPECULAR:
					if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
						break;
					}
					if ( inter.specularImage ) {
						RB_SubmitInteraction( &inter );
					}
					R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.specularImage, inter.specularMatrix, inter.specularColor.ToFloatPtr() );
					inter.specularColor[0] *= lightColor[0];
					inter.specularColor[1] *= lightColor[1];
					inter.specularColor[2] *= lightColor[2];
					inter.specularColor[3] *= lightColor[3];
					inter.vertexColor = surfaceStage->vertexColor;
					break;
			}
		}

		// draw the final interaction
		RB_SubmitInteraction( &inter );
	}

	if ( surfaceShader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		Dk_SetPolygonOffset( false, 0, 0 );
	}
	if ( hacked ) {
		RB_LeaveDepthHack();
		Dk_LoadSpace( surf->space );
	}
}

static void RB_CreateDrawInteractions( const drawSurf_t *surf ) {
	if ( !surf ) {
		return;
	}

	// perform setup here that will be constant for all interactions
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | backEnd.depthFunc );
	Dk_UseFixedProgram( DKP_INTERACTION );

	// texture 0 is the normalization cube map for the vector towards the light
	GL_SelectTextureNoClient( 0 );
	if ( backEnd.vLight->lightShader->IsAmbientLight() ) {
		globalImages->ambientNormalMap->Bind();
	} else {
		globalImages->normalCubeMapImage->Bind();
	}

	// texture 6 is the specular lookup table
	GL_SelectTextureNoClient( 6 );
	globalImages->specularTableImage->Bind();

	backEnd.currentSpace = NULL;
	for ( ; surf; surf = surf->nextOnLight ) {
		if ( surf->geo && surf->geo->ambientCache ) {
			Dk_BindDrawVerts( surf->geo );
			RB_CreateSingleDrawInteractions( surf );
		}
	}

	GL_SelectTexture( 0 );
}

/*
================================================================================
Stencil shadows

One draw per volume, with the two faces' stencil operations set together, where
OpenGL's single sided path needs two to four draws for the same result.
================================================================================
*/
static void Dk_SetShadowStencilOps( bool external ) {
	const bool mirror = backEnd.viewDef->isMirror;
	// the engine's front faces are the GPU's back faces
	if ( external ) {
		// depth pass: count the volume faces in front of the surface
		if ( mirror ) {
			Dk_SetStencilOps( DkStencilOp_Keep, DkStencilOp_IncrWrap, DkStencilOp_Keep, DkStencilOp_DecrWrap );
		} else {
			Dk_SetStencilOps( DkStencilOp_Keep, DkStencilOp_DecrWrap, DkStencilOp_Keep, DkStencilOp_IncrWrap );
		}
	} else {
		// depth fail: safe when the view is inside the volume
		if ( mirror ) {
			Dk_SetStencilOps( DkStencilOp_DecrWrap, DkStencilOp_Keep, DkStencilOp_IncrWrap, DkStencilOp_Keep );
		} else {
			Dk_SetStencilOps( DkStencilOp_IncrWrap, DkStencilOp_Keep, DkStencilOp_DecrWrap, DkStencilOp_Keep );
		}
	}
}

static void RB_T_Shadow( const drawSurf_t *surf ) {
	const srfTriangles_t *tri = surf->geo;

	if ( !tri->shadowCache ) {
		return;
	}

	// the light position, for the program that projects the rear surfaces
	if ( surf->space != backEnd.currentSpace ) {
		idVec4 localLight;
		R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.vLight->globalLightOrigin, localLight.ToVec3() );
		localLight.w = 0.0f;
		Dk_SetVsEnv( PP_LIGHT_ORIGIN, localLight.ToFloatPtr() );
	}

	// we always draw the sil planes, but we may not need to draw the front or rear caps
	int numIndexes;
	bool external = false;

	if ( !r_useExternalShadows.GetInteger() ) {
		numIndexes = tri->numIndexes;
	} else if ( r_useExternalShadows.GetInteger() == 2 ) {
		numIndexes = tri->numShadowIndexesNoCaps;
	} else if ( !( surf->dsFlags & DSF_VIEW_INSIDE_SHADOW ) ) {
		// if we aren't inside the shadow projection, no caps are ever needed
		numIndexes = tri->numShadowIndexesNoCaps;
		external = true;
	} else if ( !backEnd.vLight->viewInsideLight && !( surf->geo->shadowCapPlaneBits & SHADOW_CAP_INFINITE ) ) {
		// if we are inside the shadow projection, but outside the light, and drawing
		// a non-infinite shadow, we can skip some caps
		if ( backEnd.vLight->viewSeesShadowPlaneBits & surf->geo->shadowCapPlaneBits ) {
			// we can see through a rear cap, so we need to draw it, but we can skip the
			// caps on the actual surface
			numIndexes = tri->numShadowIndexesNoFrontCaps;
		} else {
			// we don't need to draw any caps
			numIndexes = tri->numShadowIndexesNoCaps;
		}
		external = true;
	} else {
		// must draw everything
		numIndexes = tri->numIndexes;
	}

	// A surface that could not use the external optimizations has the "no caps" counts
	// forced back to the full count: treat it as internal and keep the robust path.
	if ( numIndexes == tri->numIndexes ) {
		external = false;
	}

	if ( r_useDepthBoundsTest.GetBool() ) {
		Dk_SetDepthBounds( true, surf->scissorRect.zmin, surf->scissorRect.zmax );
	}

	Dk_SetShadowStencilOps( external );
	Dk_BindVertexBuffer( 0, Dk_CacheAddr( tri->shadowCache ), tri->shadowCache->size );
	RB_DrawShadowElementsWithCounters( tri, numIndexes );
}

static void RB_StencilShadowPass( const drawSurf_t *drawSurfs ) {
	if ( !r_shadows.GetBool() || !drawSurfs ) {
		return;
	}

	Dk_UseFixedProgram( DKP_SHADOW );
	Dk_SetVertexFormat( VFMT_SHADOW );

	// don't write to the color buffer, just the stencil buffer
	GL_State( GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS );
	GL_Cull( CT_TWO_SIDED );

	const bool polygonOffset = r_shadowPolygonFactor.GetFloat() || r_shadowPolygonOffset.GetFloat();
	if ( polygonOffset ) {
		Dk_SetPolygonOffset( true, r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );
	}

	Dk_SetStencilFunc( DkCompareOp_Always, 1, 255 );

	RB_RenderDrawSurfChainWithFunction( drawSurfs, RB_T_Shadow );

	if ( polygonOffset ) {
		Dk_SetPolygonOffset( false, 0, 0 );
	}
	if ( r_useDepthBoundsTest.GetBool() ) {
		Dk_SetDepthBounds( false, 0.0f, 1.0f );
	}

	GL_Cull( CT_FRONT_SIDED );

	Dk_SetStencilFunc( DkCompareOp_Gequal, 128, 255 );
	Dk_SetStencilOps( DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Keep );
}

static void RB_DrawInteractions( void ) {
	GL_SelectTexture( 0 );

	for ( viewLight_t *vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next ) {
		backEnd.vLight = vLight;

		// fog and blend lights come later
		if ( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ) {
			continue;
		}
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;
		}

		// clear the stencil buffer if needed
		if ( vLight->globalShadows || vLight->localShadows ) {
			backEnd.currentScissor = vLight->scissorRect;
			if ( r_useScissor.GetBool() ) {
				Dk_ApplyScissor( backEnd.currentScissor );
			}
			Dk_Clear( false, NULL, false, true, 128 );
		} else {
			// no shadows, so no need to read or write the stencil buffer
			Dk_SetStencilFunc( DkCompareOp_Always, 128, 255 );
		}

		RB_StencilShadowPass( vLight->globalShadows );
		RB_CreateDrawInteractions( vLight->localInteractions );
		RB_StencilShadowPass( vLight->localShadows );
		RB_CreateDrawInteractions( vLight->globalInteractions );

		// translucent surfaces never get stencil shadowed
		if ( r_skipTranslucent.GetBool() ) {
			continue;
		}

		Dk_SetStencilFunc( DkCompareOp_Always, 128, 255 );

		backEnd.depthFunc = GLS_DEPTHFUNC_LESS;
		RB_CreateDrawInteractions( vLight->translucentInteractions );
		backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;
	}

	Dk_SetStencilFunc( DkCompareOp_Always, 128, 255 );
	GL_SelectTexture( 0 );
}

/*
================================================================================
Shader passes
================================================================================
*/
void RB_SetProgramEnvironment( void ) {
	float parm[4];

	// screen power of two correction factor, assuming the copy to _currentRender
	// also copied an extra row and column for the bilerp
	const int w = backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1;
	parm[0] = (float)w / globalImages->currentRenderImage->GetOpts().width;
	const int h = backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1;
	parm[1] = (float)h / globalImages->currentRenderImage->GetOpts().height;
	parm[2] = 0.0f;
	parm[3] = 1.0f;
	Dk_SetVsEnv( 0, parm );
	Dk_SetFsEnv( 0, parm );

	// window coord to 0.0 to 1.0 conversion
	parm[0] = 1.0f / w;
	parm[1] = 1.0f / h;
	parm[2] = 0.0f;
	parm[3] = 1.0f;
	Dk_SetFsEnv( 1, parm );

	// for programs that read the fragment's position, measured from the bottom as they expect
	parm[0] = (float)DK_SCREEN_HEIGHT;
	parm[1] = (float)backEnd.viewDef->viewport.x1;
	parm[2] = (float)backEnd.viewDef->viewport.y1;
	parm[3] = 1.0f;
	Dk_SetFsEnv( DK_FS_ENV_SCREEN_SIZE, parm );

	// set eye position in global space
	parm[0] = backEnd.viewDef->renderView.vieworg[0];
	parm[1] = backEnd.viewDef->renderView.vieworg[1];
	parm[2] = backEnd.viewDef->renderView.vieworg[2];
	parm[3] = 1.0f;
	Dk_SetVsEnv( 1, parm );
}

void RB_SetProgramEnvironmentSpace( void ) {
	const viewEntity_t *space = backEnd.currentSpace;
	float parm[4];

	// set eye position in local space
	R_GlobalPointToLocal( space->modelMatrix, backEnd.viewDef->renderView.vieworg, *(idVec3 *)parm );
	parm[3] = 1.0f;
	Dk_SetVsEnv( 5, parm );

	// the model matrix without the view, to transform local vectors to global coordinates
	parm[0] = space->modelMatrix[0];
	parm[1] = space->modelMatrix[4];
	parm[2] = space->modelMatrix[8];
	parm[3] = space->modelMatrix[12];
	Dk_SetVsEnv( 6, parm );
	parm[0] = space->modelMatrix[1];
	parm[1] = space->modelMatrix[5];
	parm[2] = space->modelMatrix[9];
	parm[3] = space->modelMatrix[13];
	Dk_SetVsEnv( 7, parm );
	parm[0] = space->modelMatrix[2];
	parm[1] = space->modelMatrix[6];
	parm[2] = space->modelMatrix[10];
	parm[3] = space->modelMatrix[14];
	Dk_SetVsEnv( 8, parm );
}

static bool RB_StageUsesCurrentRender( const shaderStage_t *stage ) {
	if ( stage == NULL ) {
		return false;
	}
	const idImage *current = globalImages->currentRenderImage;
	if ( stage->texture.image == current ) {
		return true;
	}
	const newShaderStage_t *newStage = stage->newStage;
	if ( newStage ) {
		for ( int i = 0; i < newStage->numFragmentProgramImages; i++ ) {
			if ( newStage->fragmentProgramImages[i] == current ) {
				return true;
			}
		}
	}
	return false;
}

static void Dk_CopyViewToCurrentRender( void ) {
	globalImages->currentRenderImage->CopyFramebuffer( backEnd.viewDef->viewport.x1, backEnd.viewDef->viewport.y1,
		backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1,
		backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1 );
	backEnd.currentRenderCopied = true;
}

/*
================
Dk_SetFixedStageVertexColor

Vertex color can be 1.0, color, or 1.0 - color. A second pass through the
stage's constant color was a texture unit on the old fixed function path.
================
*/
static void Dk_VertexColorMode( stageVertexColor_t vertexColor, float mode[4] ) {
	switch ( vertexColor ) {
		case SVC_MODULATE:
			mode[0] = 1.0f;
			mode[1] = 0.0f;
			mode[2] = 1.0f;
			mode[3] = 0.0f;
			break;
		case SVC_INVERSE_MODULATE:
			mode[0] = -1.0f;
			mode[1] = 1.0f;
			mode[2] = 1.0f;		// alpha modulates, it does not invert
			mode[3] = 0.0f;
			break;
		default:
			mode[0] = 0.0f;
			mode[1] = 1.0f;
			mode[2] = 0.0f;
			mode[3] = 1.0f;
			break;
	}
}

static void Dk_BindStageVerts( const drawSurf_t *surf, int stage, dkVertexFormat_t format ) {
	const srfTriangles_t *tri = surf->geo;
	Dk_SetVertexFormat( format );
	Dk_BindVertexBuffer( 0, Dk_CacheAddr( tri->ambientCache ), tri->ambientCache->size );
	if ( format == VFMT_DRAWVERT_COLOR ) {
		const DkGpuAddr colors = Dk_CacheAddr( surf->decalColorCache ) + surf->decalColorOffset + stage * surf->decalColorStride;
		Dk_BindVertexBuffer( 1, colors, surf->decalColorStride );
	} else if ( format == VFMT_DRAWVERT_TEX3 ) {
		Dk_BindVertexBuffer( 1, Dk_CacheAddr( surf->dynamicTexCoords ), surf->dynamicTexCoords->size );
	}
}

/*
================
Material programs

The families retail materials use, drawn with this renderer's own shaders. Any other program
leaves its stage undrawn, and the log names it once.
================
*/
static void Dk_ReportSkippedProgram( const shaderStage_t *pStage, const idMaterial *material ) {
	static idList<const idMaterial *> reported;
	if ( reported.Find( material ) == NULL ) {
		reported.Append( material );
		const newShaderStage_t *newStage = pStage->newStage;
		common->Printf( "deko3D: material %s has a stage with a program there is no shader for (%s): skipped\n", material->GetName(),
			newStage->glslProgram ? newStage->glslProgramName : Dk_MaterialProgramName( newStage->fragmentProgram ) );
	}
}

static void Dk_BindProgramImages( const newShaderStage_t *newStage ) {
	for ( int i = 0; i < newStage->numFragmentProgramImages; i++ ) {
		if ( newStage->fragmentProgramImages[i] ) {
			GL_SelectTexture( i );
			newStage->fragmentProgramImages[i]->Bind();
		}
	}
	GL_SelectTexture( 0 );
}

static void Dk_SetProgramVertexParms( const newShaderStage_t *newStage, const float *regs ) {
	for ( int i = 0; i < newStage->numVertexParms && i < DK_LOCAL_COUNT; i++ ) {
		float parm[4];
		parm[0] = regs[ newStage->vertexParms[i][0] ];
		parm[1] = regs[ newStage->vertexParms[i][1] ];
		parm[2] = regs[ newStage->vertexParms[i][2] ];
		parm[3] = regs[ newStage->vertexParms[i][3] ];
		Dk_SetVsLocal( i, parm );
	}
}

static void Dk_DrawNewStage( const drawSurf_t *surf, const shaderStage_t *pStage, int stage ) {
	const newShaderStage_t *newStage = pStage->newStage;
	const float *regs = surf->shaderRegisters;

	const vkMaterialProgramFamily_t family = R_GetARBProgramFamily( GL_FRAGMENT_PROGRAM_ARB, newStage->fragmentProgram );
	dkFixedProgram_t program;
	int requiredImages;
	switch ( family ) {
		case VK_MATERIAL_PROGRAM_FAMILY_HEAT_HAZE:
			program = DKP_HEATHAZE;
			requiredImages = 2;
			break;
		case VK_MATERIAL_PROGRAM_FAMILY_HEAT_HAZE_WITH_MASK:
			program = DKP_HEATHAZE_MASK;
			requiredImages = 3;
			break;
		case VK_MATERIAL_PROGRAM_FAMILY_HEAT_HAZE_WITH_MASK_AND_VERTEX:
			program = DKP_HEATHAZE_MASK_VERTEX;
			requiredImages = 3;
			break;
		case VK_MATERIAL_PROGRAM_FAMILY_BUMPY_ENVIRONMENT:
			program = DKP_BUMPY_ENVIRONMENT;
			requiredImages = 2;
			break;
		case VK_MATERIAL_PROGRAM_FAMILY_MONOCHROME:
			program = DKP_MONOCHROME;
			requiredImages = 1;
			break;
		default:
			Dk_ReportSkippedProgram( pStage, surf->material );
			return;
	}
	if ( newStage->numFragmentProgramImages < requiredImages ) {
		return;
	}
	for ( int i = 0; i < requiredImages; i++ ) {
		if ( !newStage->fragmentProgramImages[i] ) {
			return;
		}
	}

	Dk_BindStageVerts( surf, stage, VFMT_DRAWVERT );
	GL_State( pStage->drawStateBits );
	Dk_UseFixedProgram( program );

	if ( program == DKP_MONOCHROME ) {
		const float color[4] = { regs[ pStage->color.registers[0] ], regs[ pStage->color.registers[1] ], regs[ pStage->color.registers[2] ], regs[ pStage->color.registers[3] ] };
		Dk_SetFsLocal( 0, color );
	} else if ( program == DKP_BUMPY_ENVIRONMENT ) {
		// the shader is shared with fixed stages, which leave their own color behind
		static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		static const float noVertexColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		Dk_SetVsLocal( FFV_COLOR, white );
		Dk_SetVsLocal( FFV_COLOR_MODE, noVertexColor );
	} else {
		Dk_SetProgramVertexParms( newStage, regs );
	}
	Dk_BindProgramImages( newStage );

	RB_DrawElementsWithCounters( surf->geo );
}

static idCVar r_dkLogDraws( "r_dkLogDraws", "0", CVAR_RENDERER | CVAR_BOOL, "list each material stage as it draws, in the log. Set it for a frame or two: it writes a lot" );

static void Dk_DrawFixedStage( const drawSurf_t *surf, const shaderStage_t *pStage, int stage ) {
	const float *regs = surf->shaderRegisters;

	float color[4];
	color[0] = regs[ pStage->color.registers[0] ];
	color[1] = regs[ pStage->color.registers[1] ];
	color[2] = regs[ pStage->color.registers[2] ];
	color[3] = regs[ pStage->color.registers[3] ];

	const int blendBits = pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
	// skip the entire stage if an add would be black
	if ( blendBits == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) && color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
		return;
	}
	// skip the entire stage if a blend would be completely transparent
	if ( blendBits == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) && color[3] <= 0 ) {
		return;
	}

	const bool bakedColors = surf->decalColorCache != NULL && stage < surf->decalColorStageCount && surf->decalColorStride > 0;

	float colorMode[4];
	float constant[4] = { color[0], color[1], color[2], color[3] };
	Dk_VertexColorMode( pStage->vertexColor, colorMode );
	if ( bakedColors ) {
		// decal stages baked their color into the per-vertex data already
		constant[0] = constant[1] = constant[2] = constant[3] = 1.0f;
	}
	Dk_SetVsLocal( FFV_COLOR, constant );
	Dk_SetVsLocal( FFV_COLOR_MODE, colorMode );

	if ( r_dkLogDraws.GetBool() ) {
		const idImage *image = pStage->texture.image;
		common->Printf( "stage %i of %s: image %s, color %.3f %.3f %.3f %.3f, vertex color %i, state 0x%llx\n", stage, surf->material->GetName(),
			image ? image->GetName() : "(none)", constant[0], constant[1], constant[2], constant[3], (int)pStage->vertexColor, (unsigned long long)pStage->drawStateBits );
	}

	float s[4] = { 1, 0, 0, 0 };
	float t[4] = { 0, 1, 0, 0 };
	float q[4] = { 0, 0, 0, 1 };
	if ( pStage->texture.hasMatrix ) {
		float m[16];
		RB_GetShaderTextureMatrix( regs, &pStage->texture, m );
		s[0] = m[0]; s[1] = m[4]; s[3] = m[12];
		t[0] = m[1]; t[1] = m[5]; t[3] = m[13];
	}
	Dk_SetVsLocal( FFV_TEX_S, s );
	Dk_SetVsLocal( FFV_TEX_T, t );
	Dk_SetVsLocal( FFV_TEX_Q, q );

	const texgen_t texgen = pStage->texture.texgen;

	dkVertexFormat_t format = bakedColors ? VFMT_DRAWVERT_COLOR : VFMT_DRAWVERT;
	dkFixedProgram_t program = DKP_STAGE;
	switch ( texgen ) {
		case TG_DIFFUSE_CUBE:
			program = DKP_STAGE_NORMAL_CUBE;
			break;
		case TG_SKYBOX_CUBE:
		case TG_WOBBLESKY_CUBE:
			if ( !surf->dynamicTexCoords ) {
				return;
			}
			program = DKP_STAGE_TEX3_CUBE;
			format = VFMT_DRAWVERT_TEX3;
			break;
		case TG_SCREEN:
		case TG_SCREEN2:
		case TG_GLASSWARP:
			program = DKP_STAGE_SCREEN;
			break;
		case TG_REFLECT_CUBE:
			program = surf->material->GetBumpStage() ? DKP_BUMPY_ENVIRONMENT : DKP_ENVIRONMENT;
			break;
		default:
			break;
	}

	Dk_BindStageVerts( surf, stage, format );

	GL_SelectTexture( 0 );
	RB_BindVariableStageImage( &pStage->texture, regs );
	GL_State( pStage->drawStateBits );
	Dk_UseFixedProgram( program );

	if ( program == DKP_BUMPY_ENVIRONMENT ) {
		GL_SelectTexture( 1 );
		surf->material->GetBumpStage()->texture.image->Bind();
		GL_SelectTexture( 0 );
	}

	if ( pStage->privatePolygonOffset ) {
		Dk_SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
	}

	RB_DrawElementsWithCounters( surf->geo );

	if ( pStage->privatePolygonOffset && !surf->material->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		Dk_SetPolygonOffset( false, 0, 0 );
	}
}

void RB_STD_T_RenderShaderPasses( const drawSurf_t *surf ) {
	const srfTriangles_t *tri = surf->geo;
	const idMaterial *shader = surf->material;

	if ( !shader->HasAmbient() || shader->IsPortalSky() || !tri->numIndexes || !tri->ambientCache ) {
		return;
	}

	const bool hacked = surf->space->weaponDepthHack || surf->space->modelDepthHack != 0.0f;
	if ( surf->space->weaponDepthHack ) {
		RB_EnterWeaponDepthHack();
	}
	if ( surf->space->modelDepthHack != 0.0f ) {
		RB_EnterModelDepthHack( surf->space->modelDepthHack );
	}

	// change the matrix if needed
	if ( surf->space != backEnd.currentSpace || hacked ) {
		Dk_LoadSpace( surf->space );
		backEnd.currentSpace = surf->space;
		RB_SetProgramEnvironmentSpace();
	}

	Dk_ScissorForSurface( surf );

	const float *regs = surf->shaderRegisters;

	GL_Cull( shader->GetCullType() );

	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		Dk_SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
	}

	for ( int stage = 0; stage < shader->GetNumStages(); stage++ ) {
		const shaderStage_t *pStage = shader->GetStage( stage );

		if ( regs[ pStage->conditionRegister ] == 0 ) {
			continue;
		}
		// skip the stages involved in lighting
		if ( pStage->lighting != SL_AMBIENT ) {
			continue;
		}
		// skip if the stage is ( GL_ZERO, GL_ONE ), which is used for some alpha masks
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
			continue;
		}
		// materials that read _currentRender without being sorted as post process
		if ( !backEnd.currentRenderCopied && RB_StageUsesCurrentRender( pStage ) ) {
			Dk_CopyViewToCurrentRender();
		}

		if ( pStage->newStage ) {
			Dk_DrawNewStage( surf, pStage, stage );
		} else {
			Dk_DrawFixedStage( surf, pStage, stage );
		}
	}

	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		Dk_SetPolygonOffset( false, 0, 0 );
	}
	if ( hacked ) {
		RB_LeaveDepthHack();
		Dk_LoadSpace( surf->space );
	}
}

int RB_STD_DrawShaderPasses( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	int i;

	// only obey skipAmbient if we are rendering a view
	if ( backEnd.viewDef->viewEntitys && r_skipAmbient.GetBool() ) {
		return numDrawSurfs;
	}

	// if we are about to draw the first surface that needs
	// the rendering in a texture, copy it over
	if ( drawSurfs[0]->material->GetSort() >= SS_POST_PROCESS ) {
		if ( r_skipPostProcess.GetBool() ) {
			return 0;
		}
		Dk_CopyViewToCurrentRender();
	}

	GL_SelectTexture( 0 );
	RB_SetProgramEnvironment();

	backEnd.currentSpace = NULL;
	for ( i = 0; i < numDrawSurfs; i++ ) {
		if ( drawSurfs[i]->material->SuppressInSubview() ) {
			continue;
		}
		// we need to draw the post process shaders after we have drawn the fog lights
		if ( drawSurfs[i]->material->GetSort() >= SS_POST_PROCESS && !backEnd.currentRenderCopied ) {
			break;
		}
		RB_STD_T_RenderShaderPasses( drawSurfs[i] );
	}

	GL_Cull( CT_FRONT_SIDED );
	return i;
}

/*
================================================================================
Fog and blend lights
================================================================================
*/
static void RB_T_BlendLight( const drawSurf_t *surf ) {
	const srfTriangles_t *tri = surf->geo;

	if ( backEnd.currentSpace != surf->space ) {
		idPlane lightProject[4];
		for ( int i = 0; i < 4; i++ ) {
			R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[i], lightProject[i] );
		}
		Dk_SetVsLocal( FFV_PLANE0, lightProject[0].ToFloatPtr() );
		Dk_SetVsLocal( FFV_PLANE1, lightProject[1].ToFloatPtr() );
		Dk_SetVsLocal( FFV_PLANE2, lightProject[2].ToFloatPtr() );
		Dk_SetVsLocal( FFV_PLANE3, lightProject[3].ToFloatPtr() );
	}

	// this gets used for both blend lights and shadow draws
	if ( tri->ambientCache ) {
		Dk_BindDrawVerts( tri );
	} else if ( tri->shadowCache ) {
		return;
	}
	RB_DrawElementsWithCounters( tri );
}

static void RB_BlendLight( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2 ) {
	if ( !drawSurfs || r_skipBlendLights.GetBool() ) {
		return;
	}

	const idMaterial *lightShader = backEnd.vLight->lightShader;
	const float *regs = backEnd.vLight->shaderRegisters;

	Dk_UseFixedProgram( DKP_BLEND_LIGHT );

	// texture 1 will get the falloff texture
	GL_SelectTexture( 1 );
	backEnd.vLight->falloffImage->Bind();

	for ( int i = 0; i < lightShader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = lightShader->GetStage( i );

		if ( !regs[ stage->conditionRegister ] ) {
			continue;
		}

		GL_State( GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL );

		// texture 0 will get the projected texture
		GL_SelectTexture( 0 );
		stage->texture.image->Bind();

		float s[4] = { 1, 0, 0, 0 };
		float t[4] = { 0, 1, 0, 0 };
		if ( stage->texture.hasMatrix ) {
			float m[16];
			RB_GetShaderTextureMatrix( regs, &stage->texture, m );
			s[0] = m[0]; s[1] = m[4]; s[3] = m[12];
			t[0] = m[1]; t[1] = m[5]; t[3] = m[13];
		}
		Dk_SetVsLocal( FFV_TEX_S, s );
		Dk_SetVsLocal( FFV_TEX_T, t );

		// get the modulate values from the light, including alpha, unlike normal lights
		backEnd.lightColor[0] = regs[ stage->color.registers[0] ];
		backEnd.lightColor[1] = regs[ stage->color.registers[1] ];
		backEnd.lightColor[2] = regs[ stage->color.registers[2] ];
		backEnd.lightColor[3] = regs[ stage->color.registers[3] ];
		Dk_SetFsLocal( 0, backEnd.lightColor );

		RB_RenderDrawSurfChainWithFunction( drawSurfs, RB_T_BlendLight );
		RB_RenderDrawSurfChainWithFunction( drawSurfs2, RB_T_BlendLight );
	}

	GL_SelectTexture( 0 );
}

static idPlane fogPlanes[4];

static void RB_T_BasicFog( const drawSurf_t *surf ) {
	if ( backEnd.currentSpace != surf->space ) {
		idPlane local;

		// texture 0: S is the distance in front of the viewer, T is constant
		R_GlobalPlaneToLocal( surf->space->modelMatrix, fogPlanes[0], local );
		local[3] += 0.5f;
		Dk_SetVsLocal( FFV_PLANE0, local.ToFloatPtr() );
		local[0] = local[1] = local[2] = 0.0f;
		local[3] = 0.5f;
		Dk_SetVsLocal( FFV_PLANE1, local.ToFloatPtr() );

		// texture 1: T is the fade plane, S is constant per viewer
		R_GlobalPlaneToLocal( surf->space->modelMatrix, fogPlanes[2], local );
		local[3] += FOG_ENTER;
		Dk_SetVsLocal( FFV_PLANE3, local.ToFloatPtr() );
		R_GlobalPlaneToLocal( surf->space->modelMatrix, fogPlanes[3], local );
		Dk_SetVsLocal( FFV_PLANE2, local.ToFloatPtr() );
	}
	RB_T_RenderTriangleSurface( surf );
}

static void RB_FogPass( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2 ) {
	// create a surface for the light frustom triangles, which are oriented drawn side out
	const srfTriangles_t *frustumTris = backEnd.vLight->frustumTris;

	// if we ran out of vertex cache memory, skip it
	if ( !frustumTris->ambientCache ) {
		return;
	}
	drawSurf_t ds;
	memset( &ds, 0, sizeof( ds ) );
	ds.space = &backEnd.viewDef->worldSpace;
	ds.geo = frustumTris;
	ds.scissorRect = backEnd.viewDef->scissor;

	// find the current color and density of the fog
	const idMaterial *lightShader = backEnd.vLight->lightShader;
	const float *regs = backEnd.vLight->shaderRegisters;
	// assume fog shaders have only a single stage
	const shaderStage_t *stage = lightShader->GetStage( 0 );

	backEnd.lightColor[0] = regs[ stage->color.registers[0] ];
	backEnd.lightColor[1] = regs[ stage->color.registers[1] ];
	backEnd.lightColor[2] = regs[ stage->color.registers[2] ];
	backEnd.lightColor[3] = regs[ stage->color.registers[3] ];

	const float fogColor[4] = { backEnd.lightColor[0], backEnd.lightColor[1], backEnd.lightColor[2], 1.0f };
	Dk_SetFsLocal( 0, fogColor );

	// calculate the falloff planes
	float a;
	// if they left the default value on, set a fog distance of 500
	if ( backEnd.lightColor[3] <= 1.0f ) {
		a = -0.5f / DEFAULT_FOG_DISTANCE;
	} else {
		// otherwise, distance = alpha color
		a = -0.5f / backEnd.lightColor[3];
	}

	GL_State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	Dk_UseFixedProgram( DKP_FOG );

	// texture 0 is the falloff image
	GL_SelectTexture( 0 );
	globalImages->fogImage->Bind();

	fogPlanes[0][0] = a * backEnd.viewDef->worldSpace.modelViewMatrix[2];
	fogPlanes[0][1] = a * backEnd.viewDef->worldSpace.modelViewMatrix[6];
	fogPlanes[0][2] = a * backEnd.viewDef->worldSpace.modelViewMatrix[10];
	fogPlanes[0][3] = a * backEnd.viewDef->worldSpace.modelViewMatrix[14];

	fogPlanes[1][0] = a * backEnd.viewDef->worldSpace.modelViewMatrix[0];
	fogPlanes[1][1] = a * backEnd.viewDef->worldSpace.modelViewMatrix[4];
	fogPlanes[1][2] = a * backEnd.viewDef->worldSpace.modelViewMatrix[8];
	fogPlanes[1][3] = a * backEnd.viewDef->worldSpace.modelViewMatrix[12];

	// texture 1 is the entering plane fade correction
	GL_SelectTexture( 1 );
	globalImages->fogEnterImage->Bind();
	GL_SelectTexture( 0 );

	// T will get a texgen for the fade plane, which is always the "top" plane on unrotated lights
	fogPlanes[2][0] = 0.001f * backEnd.vLight->fogPlane[0];
	fogPlanes[2][1] = 0.001f * backEnd.vLight->fogPlane[1];
	fogPlanes[2][2] = 0.001f * backEnd.vLight->fogPlane[2];
	fogPlanes[2][3] = 0.001f * backEnd.vLight->fogPlane[3];

	// S is based on the view origin
	const float s = backEnd.viewDef->renderView.vieworg * fogPlanes[2].Normal() + fogPlanes[2][3];

	fogPlanes[3][0] = 0;
	fogPlanes[3][1] = 0;
	fogPlanes[3][2] = 0;
	fogPlanes[3][3] = FOG_ENTER + s;

	RB_RenderDrawSurfChainWithFunction( drawSurfs, RB_T_BasicFog );
	RB_RenderDrawSurfChainWithFunction( drawSurfs2, RB_T_BasicFog );

	// the light frustum bounding planes aren't in the depth buffer, so use depthfunc_less instead
	// of depthfunc_equal
	GL_State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_LESS );
	GL_Cull( CT_BACK_SIDED );
	RB_RenderDrawSurfChainWithFunction( &ds, RB_T_BasicFog );
	GL_Cull( CT_FRONT_SIDED );
}

static void RB_STD_FogAllLights( void ) {
	if ( r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0 || backEnd.viewDef->isXraySubview ) {
		return;
	}

	Dk_SetStencilTest( false );

	for ( viewLight_t *vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next ) {
		backEnd.vLight = vLight;

		if ( vLight->lightShader->IsFogLight() ) {
			RB_FogPass( vLight->globalInteractions, vLight->localInteractions );
		} else if ( vLight->lightShader->IsBlendLight() ) {
			RB_BlendLight( vLight->globalInteractions, vLight->localInteractions );
		}
	}

	Dk_SetStencilTest( true );
}

/*
================
RB_STD_LightScale

Perform extra blending passes to multiply the entire buffer by
a floating point value
================
*/
static void RB_STD_LightScale( void ) {
	if ( backEnd.overBright == 1.0f || r_skipLightScale.GetBool() ) {
		return;
	}

	// the scissor may be smaller than the viewport for subviews
	if ( r_useScissor.GetBool() ) {
		Dk_ApplyScissor( backEnd.viewDef->scissor );
		backEnd.currentScissor = backEnd.viewDef->scissor;
	}

	// full screen blends
	float ortho[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };
	Dk_SetMvp( IDENTITY_MATRIX, ortho );

	GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_SRC_COLOR );
	GL_Cull( CT_TWO_SIDED );	// so mirror views also get it
	Dk_SetDepthTest( false );
	Dk_SetStencilTest( false );
	Dk_UseFixedProgram( DKP_COLOR );

	idDrawVert verts[4];
	memset( verts, 0, sizeof( verts ) );
	verts[0].xyz.Set( 0, 0, 0 );
	verts[1].xyz.Set( 0, 1, 0 );
	verts[2].xyz.Set( 1, 0, 0 );
	verts[3].xyz.Set( 1, 1, 0 );
	vertCache_t *quad = vertexCache.AllocFrameTemp( verts, sizeof( verts ) );

	if ( quad ) {
		Dk_SetVertexFormat( VFMT_DRAWVERT );
		Dk_BindVertexBuffer( 0, quad->gpuAddr, quad->size );

		float v = 1;
		while ( idMath::Fabs( v - backEnd.overBright ) > 0.01f ) {	// a little extra slop
			float f = backEnd.overBright / v;
			f /= 2;
			if ( f > 1 ) {
				f = 1;
			}
			const float color[4] = { f, f, f, 1.0f };
			Dk_SetFsLocal( 0, color );
			v = v * f * 2;
			Dk_DrawStrip( 4 );
		}
	}

	Dk_SetDepthTest( true );
	Dk_SetStencilTest( true );
	GL_Cull( CT_FRONT_SIDED );
	backEnd.currentSpace = NULL;
}

/*
================================================================================
The view
================================================================================
*/
void RB_DetermineLightScale( void ) {
	// the light scale will be based on the largest color component of any surface
	// that will be drawn.
	float max = 1.0f;

	for ( viewLight_t *vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next ) {
		// lights with no surfaces or shaderparms may still be present for debug display
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;
		}

		const idMaterial *shader = vLight->lightShader;
		const int numStages = shader->GetNumStages();
		for ( int i = 0; i < numStages; i++ ) {
			const shaderStage_t *stage = shader->GetStage( i );
			for ( int j = 0; j < 3; j++ ) {
				const float v = r_lightScale.GetFloat() * vLight->shaderRegisters[ stage->color.registers[j] ];
				if ( v > max ) {
					max = v;
				}
			}
		}
	}

	backEnd.pc.maxLightValue = max;
	if ( max <= tr.backEndRendererMaxLight ) {
		backEnd.lightScale = r_lightScale.GetFloat();
		backEnd.overBright = 1.0f;
	} else {
		backEnd.lightScale = r_lightScale.GetFloat() * tr.backEndRendererMaxLight / max;
		backEnd.overBright = max / tr.backEndRendererMaxLight;
	}
}

void RB_STD_DrawView( void ) {
	backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;

	drawSurf_t **drawSurfs = (drawSurf_t **)&backEnd.viewDef->drawSurfs[0];
	const int numDrawSurfs = backEnd.viewDef->numDrawSurfs;

	// clear the z buffer, set the projection matrix, etc
	RB_BeginDrawingView();

	// decide how much overbrighting we are going to do
	RB_DetermineLightScale();

	// fill the depth buffer and clear color buffer to black except on subviews
	RB_STD_FillDepthBuffer( drawSurfs, numDrawSurfs );

	// main light renderer
	RB_DrawInteractions();

	// disable stencil shadow test
	Dk_SetStencilFunc( DkCompareOp_Always, 128, 255 );

	// uplight the entire screen to crutch up not having better blending range
	RB_STD_LightScale();

	// now draw any non-light dependent shading passes
	const int processed = RB_STD_DrawShaderPasses( drawSurfs, numDrawSurfs );

	// fog and blend lights
	RB_STD_FogAllLights();

	// now draw any post-processing effects using _currentRender
	if ( processed < numDrawSurfs ) {
		RB_STD_DrawShaderPasses( drawSurfs + processed, numDrawSurfs - processed );
	}

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
}

/*
================================================================================
Back end commands
================================================================================
*/
void RB_SetDefaultGLState( void ) {
	backEnd.glState.forceGlState = true;
	backEnd.glState.faceCulling = -1;
	backEnd.glState.currenttmu = 0;
	GL_State( 0 );
	GL_Cull( CT_TWO_SIDED );
	Dk_SetDepthTest( true );
	Dk_SetStencilTest( true );
	Dk_SetStencilFunc( DkCompareOp_Always, 0, 255 );
}

void RB_LogComment( const char *comment, ... ) {
}

void RB_SetGL2D( void ) {
	static const float ortho[16] = { 2.0f / 640.0f, 0, 0, 0, 0, -2.0f / 480.0f, 0, 0, 0, 0, -2.0f, 0, -1, 1, -1, 1 };

	Dk_SetViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight, 0.0f, 1.0f );
	if ( r_useScissor.GetBool() ) {
		Dk_SetScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	}
	Dk_SetMvp( IDENTITY_MATRIX, ortho );
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	GL_Cull( CT_TWO_SIDED );
	Dk_SetDepthTest( false );
	Dk_SetStencilTest( false );
}

void RB_DrawViewCommand( const void *data ) {
	const drawSurfsCommand_t *cmd = (const drawSurfsCommand_t *)data;

	backEnd.viewDef = cmd->viewDef;

	// we will need to do a new copyTexSubImage of the screen
	// when a SS_POST_PROCESS material is used
	backEnd.currentRenderCopied = false;

	// if there aren't any drawsurfs, do nothing
	if ( !backEnd.viewDef->numDrawSurfs ) {
		return;
	}

	// skip render bypasses everything that has models, assuming
	// them to be 3D views, but leaves 2D rendering visible
	if ( r_skipRender.GetBool() && backEnd.viewDef->viewEntitys ) {
		return;
	}

	backEnd.pc.c_surfaces += backEnd.viewDef->numDrawSurfs;

	RB_STD_DrawView();
}
