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

// texcoord 0-6 as locations 0-6, color as 8, as the retail program has them
layout(location = 0) out vec4 vToLight;
layout(location = 1) out vec4 vBumpCoord;
layout(location = 2) out vec4 vFalloffCoord;
layout(location = 3) out vec4 vLightCoord;
layout(location = 4) out vec4 vDiffuseCoord;
layout(location = 5) out vec4 vSpecularCoord;
layout(location = 6) out vec4 vHalfAngle;
layout(location = 8) out vec4 vColor;

void main() {
	const vec4 p = vec4( inPosition, 1.0 );
	const vec4 t = vec4( inTexCoord, 0.0, 1.0 );
	const mat3 toTangent = mat3(
		inTangent.x, inBitangent.x, inNormal.x,
		inTangent.y, inBitangent.y, inNormal.y,
		inTangent.z, inBitangent.z, inNormal.z );

	gl_Position = TRANSFORM( p );

	const vec3 lightVector = env[4].xyz - inPosition;
	vToLight = vec4( toTangent * lightVector, 1.0 );

	vBumpCoord = vec4( dot( t, env[10] ), dot( t, env[11] ), 0.0, 1.0 );
	vFalloffCoord = vec4( dot( p, env[9] ), 0.5, 0.0, 1.0 );
	vLightCoord = vec4( dot( p, env[6] ), dot( p, env[7] ), 0.0, dot( p, env[8] ) );
	vDiffuseCoord = vec4( dot( t, env[12] ), dot( t, env[13] ), 0.0, 1.0 );
	vSpecularCoord = vec4( dot( t, env[14] ), dot( t, env[15] ), 0.0, 1.0 );

	vHalfAngle = vec4( toTangent * ( normalize( lightVector ) + normalize( env[5].xyz - inPosition ) ), 1.0 );

	vColor = vec4( inColor.rgb * env[16].x + env[16].y, 1.0 );
}
