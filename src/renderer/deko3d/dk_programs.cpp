/*
===========================================================================

Quake4NX - native deko3D renderer: shader modules.

The programs are all this renderer's own, built into the NRO's romfs: there is no
compiler on the console. Materials name retail ARB programs and openQ4 GLSL programs
by file. Those it has an equivalent for draw with that; a stage with any other is
skipped, and named once in the log.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../tr_local.h"

#undef BIT
#include <switch.h>

#include "dk_local.h"

static const uint32_t SHADER_CODE_SIZE	= 16 * 1024 * 1024;

#ifndef Q4NX_ROMFS_SHADERS		// tools/hostcheck has its own
#define Q4NX_ROMFS_SHADERS		"romfs:/shaders/"
#endif
static const char *ROMFS_SHADER_PATH	= Q4NX_ROMFS_SHADERS;

typedef struct {
	const char *	vertex;
	const char *	fragment;
} dkFixedProgramDef_t;

static const dkFixedProgramDef_t s_fixedDefs[DKP_COUNT] = {
	{ "depth", "depth" },
	{ "depth_clip", "depth" },
	{ "depth_alpha", "depth_alpha" },
	{ "stage", "stage" },
	{ "stage_normal_cube", "stage_cube" },
	{ "stage_tex3_cube", "stage_cube" },
	{ "stage_screen", "stage_screen" },
	{ "color", "color" },
	{ "depth_clip", "color" },
	{ "shadow", "depth" },
	{ "interaction", "interaction" },
	{ "environment", "environment" },
	{ "bumpyenvironment", "bumpyenvironment" },
	{ "fog", "fog" },
	{ "blendlight", "blendlight" },
	{ "heathaze", "heathaze" },
	{ "heathaze", "heathaze_mask" },
	{ "heathaze", "heathaze_mask_vertex" },
	{ "monochrome", "monochrome" },
};

typedef struct {
	char			name[64];
	GLenum			target;
	DkShader		shader;
	bool			loaded;
	bool			missing;
} dkMaterialProgram_t;

static const int MAX_MATERIAL_PROGRAMS = 200;

static DkMemBlock			s_codeBlock;
static uint32_t				s_codeHead;
static DkShader				s_fixedVertex[DKP_COUNT];
static DkShader				s_fixedFragment[DKP_COUNT];
static dkMaterialProgram_t	s_materialPrograms[MAX_MATERIAL_PROGRAMS];
static int					s_numMaterialPrograms;

static bool Dk_LoadShaderFile( const char *path, DkShader &shader ) {
	FILE *f = fopen( path, "rb" );
	if ( !f ) {
		return false;
	}
	fseek( f, 0, SEEK_END );
	const uint32_t size = (uint32_t)ftell( f );
	fseek( f, 0, SEEK_SET );

	const uint32_t offset = ( s_codeHead + DK_SHADER_CODE_ALIGNMENT - 1 ) & ~( DK_SHADER_CODE_ALIGNMENT - 1 );
	if ( offset + size + DK_SHADER_CODE_UNUSABLE_SIZE > SHADER_CODE_SIZE ) {
		fclose( f );
		common->FatalError( "deko3D: out of shader code memory" );
	}

	void *code = (byte *)dkMemBlockGetCpuAddr( s_codeBlock ) + offset;
	const bool ok = fread( code, 1, size, f ) == size;
	fclose( f );
	if ( !ok ) {
		return false;
	}
	s_codeHead = offset + size;
	Dk_DescriptorsChanged();	// the next submission drops the code cache too

	DkShaderMaker maker;
	dkShaderMakerDefaults( &maker, s_codeBlock, offset );
	dkShaderInitialize( &shader, &maker );
	return dkShaderIsValid( &shader );
}

static bool Dk_LoadNamedShader( const char *name, const char *stage, DkShader &shader ) {
	const idStr romfsPath = va( "%s%s.%s.dksh", ROMFS_SHADER_PATH, name, stage );
	return Dk_LoadShaderFile( romfsPath.c_str(), shader );
}

void Dk_InitPrograms( void ) {
	DkMemBlockMaker maker;
	dkMemBlockMakerDefaults( &maker, dkr.device, SHADER_CODE_SIZE );
	maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
	s_codeBlock = dkMemBlockCreate( &maker );
	s_codeHead = 0;

	// materials keep their program indexes across a renderer restart
	for ( int i = 0; i < s_numMaterialPrograms; i++ ) {
		s_materialPrograms[i].loaded = false;
		s_materialPrograms[i].missing = false;
	}

	// a forwarder may not say where the NRO is
	if ( R_FAILED( romfsInit() ) && R_FAILED( romfsMountFromFsdev( "sdmc:/switch/quake4/quake4.nro", 0, "romfs" ) ) ) {
		common->FatalError( "deko3D: the NRO's romfs could not be mounted" );
	}
	for ( int i = 0; i < DKP_COUNT; i++ ) {
		if ( !Dk_LoadNamedShader( s_fixedDefs[i].vertex, "vert", s_fixedVertex[i] ) ) {
			common->FatalError( "deko3D: missing shader %s.vert", s_fixedDefs[i].vertex );
		}
		if ( !Dk_LoadNamedShader( s_fixedDefs[i].fragment, "frag", s_fixedFragment[i] ) ) {
			common->FatalError( "deko3D: missing shader %s.frag", s_fixedDefs[i].fragment );
		}
	}
}

void Dk_ShutdownPrograms( void ) {
	romfsExit();
	if ( s_codeBlock ) {
		dkMemBlockDestroy( s_codeBlock );
		s_codeBlock = NULL;
	}
}

void Dk_UseFixedProgram( dkFixedProgram_t program ) {
	Dk_SetProgram( &s_fixedVertex[program], &s_fixedFragment[program] );
}

const DkShader *Dk_FixedVertexShader( dkFixedProgram_t program ) {
	return &s_fixedVertex[program];
}

/*
============================================================================
Material programs

Materials ask for programs by file name, in any case, and a file can hold both
a vertex and a fragment program. The index goes straight into the material; the
shader is loaded the first time a stage draws with it.
============================================================================
*/
int R_FindARBProgram( GLenum target, const char *program ) {
	idStr stripped = program;
	stripped.StripFileExtension();
	stripped.ToLower();

	for ( int i = 0; i < s_numMaterialPrograms; i++ ) {
		if ( s_materialPrograms[i].target == target && !idStr::Cmp( s_materialPrograms[i].name, stripped.c_str() ) ) {
			return i + 1;
		}
	}
	if ( s_numMaterialPrograms == MAX_MATERIAL_PROGRAMS ) {
		common->Error( "R_FindARBProgram: too many programs" );
	}

	dkMaterialProgram_t &entry = s_materialPrograms[s_numMaterialPrograms++];
	idStr::Copynz( entry.name, stripped.c_str(), sizeof( entry.name ) );
	entry.target = target;
	entry.loaded = false;
	entry.missing = false;
	return s_numMaterialPrograms;
}

const char *Dk_MaterialProgramName( int programIndex ) {
	if ( programIndex <= 0 || programIndex > s_numMaterialPrograms ) {
		return "";
	}
	return s_materialPrograms[programIndex - 1].name;
}

static vkMaterialProgramFamily_t Dk_ProgramFamily( const char *name ) {
	if ( !idStr::Icmp( name, "heathaze" ) ) {
		return VK_MATERIAL_PROGRAM_FAMILY_HEAT_HAZE;
	}
	// no program by this name is in the retail data: the material's reference dangles, and the plain mask is the closest
	if ( !idStr::Icmp( name, "heathazewithmask" ) || !idStr::Icmp( name, "heathazegraywithmask" ) ) {
		return VK_MATERIAL_PROGRAM_FAMILY_HEAT_HAZE_WITH_MASK;
	}
	if ( !idStr::Icmp( name, "heathazewithmaskandvertex" ) ) {
		return VK_MATERIAL_PROGRAM_FAMILY_HEAT_HAZE_WITH_MASK_AND_VERTEX;
	}
	if ( !idStr::Icmp( name, "bumpyenvironment" ) ) {
		return VK_MATERIAL_PROGRAM_FAMILY_BUMPY_ENVIRONMENT;
	}
	if ( !idStr::Icmp( name, "monochrome" ) ) {
		return VK_MATERIAL_PROGRAM_FAMILY_MONOCHROME;
	}
	return VK_MATERIAL_PROGRAM_FAMILY_UNKNOWN;
}

vkMaterialProgramFamily_t R_GetARBProgramFamily( unsigned int target, unsigned int handle ) {
	if ( handle == 0 || handle > (unsigned int)s_numMaterialPrograms || s_materialPrograms[handle - 1].target != target ) {
		return VK_MATERIAL_PROGRAM_FAMILY_UNKNOWN;
	}
	return Dk_ProgramFamily( s_materialPrograms[handle - 1].name );
}

bool R_IsARBProgramValid( unsigned int target, unsigned int handle ) {
	return R_GetARBProgramFamily( target, handle ) != VK_MATERIAL_PROGRAM_FAMILY_UNKNOWN;
}

vkGLSLProgramFamily_t R_GetGLSLProgramFamily( const char *program ) {
	return VK_GLSL_PROGRAM_FAMILY_UNKNOWN;
}

bool R_ValidateGLSLProgram( newShaderStage_t *stage ) {
	if ( stage == NULL || !stage->glslProgram ) {
		return false;
	}
	stage->glslProgramLoaded = true;
	stage->glslProgramValid = false;
	stage->glslProgramGeneration = tr.glContextGeneration;
	stage->glslProgramObject = 0;
	stage->glslVertexShaderObject = 0;
	stage->glslFragmentShaderObject = 0;
	return false;
}

void R_ReloadARBPrograms_f( const idCmdArgs &args ) {
}

void R_ReloadGLSLPrograms_f( const idCmdArgs &args ) {
}

void R_ReportShaderPrograms_f( const idCmdArgs &args ) {
	for ( int i = 0; i < s_numMaterialPrograms; i++ ) {
		common->Printf( "%3d %s %s %s\n", i + 1, s_materialPrograms[i].target == GL_VERTEX_PROGRAM_ARB ? "vertex  " : "fragment",
			Dk_ProgramFamily( s_materialPrograms[i].name ) != VK_MATERIAL_PROGRAM_FAMILY_UNKNOWN ? "has an equivalent" : "skipped          ",
			s_materialPrograms[i].name );
	}
}

void R_ARB2_Init( void ) {
	glConfig.allowARB2Path = true;
}
