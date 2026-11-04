#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec2 vFogCoord;
layout(location = 1) in vec2 vEnterCoord;

layout(binding = 0) uniform sampler2D fogMap;
layout(binding = 1) uniform sampler2D enterMap;

void main() {
	outColor = texture( fogMap, vFogCoord ) * texture( enterMap, vEnterCoord ) * flocal[0];
}
