#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec3 inBitangent;
layout(location = 5) in vec4 inColor;

layout(std140, binding = 0) uniform Matrices {
	vec4 mvp[4];
	vec4 modelView[4];
	vec4 projection[4];
	vec4 textureMatrix[4];
};
layout(std140, binding = 1) uniform VsEnv {
	vec4 env[24];
};
layout(std140, binding = 2) uniform VsLocal {
	vec4 local[16];
};

// fixed function inputs, dkFixedVertexParm_t in src/renderer/deko3d/dk_local.h
#define COLOR		local[0]
#define COLOR_MODE	local[1]
#define TEX_S		local[2]
#define TEX_T		local[3]
#define TEX_Q		local[4]
#define PLANE0		local[5]
#define PLANE1		local[6]
#define PLANE2		local[7]
#define PLANE3		local[8]

#define TRANSFORM( p ) vec4( dot( p, mvp[0] ), dot( p, mvp[1] ), dot( p, mvp[2] ), dot( p, mvp[3] ) )

layout(location = 0) out vec3 vTexCoord;
layout(location = 1) out vec4 vColor;

void main() {
	const vec4 p = vec4( inPosition, 1.0 );
	gl_Position = TRANSFORM( p );
	// screen coordinates through the stage's texture matrix
	const vec4 t = vec4( gl_Position.x, gl_Position.y, 0.0, gl_Position.w );
	vTexCoord = vec3( dot( t, TEX_S ), dot( t, TEX_T ), dot( t, TEX_Q ) );
	vColor = vec4( inColor.rgb * COLOR_MODE.x + COLOR_MODE.y, inColor.a * COLOR_MODE.z + COLOR_MODE.w ) * COLOR;
}
