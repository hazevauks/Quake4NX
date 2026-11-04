#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec2 vTexCoord;

layout(binding = 0) uniform sampler2D map;

void main() {
	const vec4 sampled = texture( map, vTexCoord );
	outColor = vec4( vec3( dot( sampled.rgb, vec3( 0.33 ) ) ), sampled.a ) * flocal[0];
}
