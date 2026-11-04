#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vToEye;
layout(location = 2) in vec4 vColor;

layout(binding = 0) uniform samplerCube environmentMap;

void main() {
	const vec3 normal = normalize( vNormal );
	const vec3 toEye = normalize( vToEye );
	const vec3 reflected = 2.0 * dot( toEye, normal ) * normal - toEye;
	outColor = texture( environmentMap, reflected ) * vColor;
}
