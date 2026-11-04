#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec3 vToEye;
layout(location = 2) in vec3 vRow0;
layout(location = 3) in vec3 vRow1;
layout(location = 4) in vec3 vRow2;
layout(location = 5) in vec4 vColor;

layout(binding = 0) uniform samplerCube environmentMap;
layout(binding = 1) uniform sampler2D bumpMap;

void main() {
	const vec4 bump = texture( bumpMap, vTexCoord );
	const vec3 localNormal = normalize( vec3( bump.a, bump.y, bump.z ) * 2.0 - 1.0 );
	const vec3 normal = vec3( dot( localNormal, vRow0 ), dot( localNormal, vRow1 ), dot( localNormal, vRow2 ) );

	const vec3 toEye = normalize( vToEye );
	const vec3 reflected = 2.0 * dot( toEye, normal ) * normal - toEye;
	outColor = vec4( texture( environmentMap, reflected ).rgb, 1.0 ) * vColor;
}
