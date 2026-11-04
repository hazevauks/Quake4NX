#version 460

layout(location = 0) in vec4 inPosition;

layout(std140, binding = 0) uniform Matrices {
	vec4 mvp[4];
	vec4 modelView[4];
	vec4 projection[4];
	vec4 textureMatrix[4];
};
layout(std140, binding = 1) uniform VsEnv {
	vec4 env[24];
};

void main() {
	// vertices with w = 0 go to infinity, away from the light
	const vec4 p = vec4( inPosition.xyz - ( 1.0 - inPosition.w ) * env[4].xyz, inPosition.w );
	gl_Position = vec4( dot( p, mvp[0] ), dot( p, mvp[1] ), dot( p, mvp[2] ), dot( p, mvp[3] ) );
}
