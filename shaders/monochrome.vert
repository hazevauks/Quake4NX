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

layout(location = 0) out vec2 vTexCoord;

void main() {
	gl_Position = TRANSFORM( vec4( inPosition, 1.0 ) );
	vTexCoord = inTexCoord;
}
