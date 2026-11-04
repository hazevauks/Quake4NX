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

#define TRANSFORM( p ) vec4( dot( p, mvp[0] ), dot( p, mvp[1] ), dot( p, mvp[2] ), dot( p, mvp[3] ) )

// heatHaze.vfp and heatHazeWithMask.vfp, the part they share. The distance scaling is the retail program's: how
// much one unit of eye space X shrinks at this depth, so a plume keeps its strength at two meters and at twenty.
// The MAX guards polygons crossing the view plane, the MIN clamps the deformation up close.
//
// local[0]	the normal map's scroll
// local[1]	the deformation's size

layout(location = 0) out vec2 vMaskTexCoord;
layout(location = 1) out vec2 vScrollTexCoord;
layout(location = 2) out vec4 vDeformScale;
layout(location = 3) out vec4 vColor;

void main() {
	const vec4 p = vec4( inPosition, 1.0 );
	gl_Position = TRANSFORM( p );

	vColor = inColor;
	vMaskTexCoord = inTexCoord;
	vScrollTexCoord = inTexCoord + local[0].xy;

	const vec4 projected = vec4( 1.0, 0.0, dot( p, modelView[2] ), 1.0 );
	const float distanceScale = min( dot( projected, projection[0] ) / max( dot( projected, projection[3] ), 1.0 ), 0.02 );
	vDeformScale = vec4( distanceScale ) * local[1];
}
