#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec3 vProjectionCoord;
layout(location = 1) in float vFalloffS;

layout(binding = 0) uniform sampler2D projectionMap;
layout(binding = 1) uniform sampler2D falloffMap;

void main() {
	outColor = textureProj( projectionMap, vProjectionCoord ) * texture( falloffMap, vec2( vFalloffS, 0.5 ) ) * flocal[0];
}
