#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec3 vTexCoord;
layout(location = 1) in vec4 vColor;

layout(binding = 0) uniform sampler2D map;

void main() {
	outColor = textureProj( map, vTexCoord ) * vColor;
}
