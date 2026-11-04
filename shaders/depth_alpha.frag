#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in float vClipDistance;

layout(binding = 0) uniform sampler2D map;

void main() {
	const float alpha = texture( map, vTexCoord ).a * flocal[0].a;
	if ( alpha <= flocal[1].x || vClipDistance < 0.0 ) {
		discard;
	}
	outColor = vec4( flocal[0].rgb, alpha );
}
