#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec3 vToLight;
layout(location = 1) in vec2 vBumpCoord;
layout(location = 2) in vec2 vFalloffCoord;
layout(location = 3) in vec4 vLightCoord;
layout(location = 4) in vec2 vDiffuseCoord;
layout(location = 5) in vec2 vSpecularCoord;
layout(location = 6) in vec3 vHalfAngle;
layout(location = 8) in vec4 vColor;

layout(binding = 0) uniform samplerCube lightCube;
layout(binding = 1) uniform sampler2D bumpMap;
layout(binding = 2) uniform sampler2D falloffMap;
layout(binding = 3) uniform sampler2D projectionMap;
layout(binding = 4) uniform sampler2D diffuseMap;
layout(binding = 5) uniform sampler2D specularMap;
layout(binding = 6) uniform sampler2D specularTable;

void main() {
	// normal maps keep X in alpha, and no renormalizing, as the retail program
	const vec4 bump = texture( bumpMap, vBumpCoord );
	const vec3 normal = vec3( bump.a, bump.y, bump.z ) * 2.0 - 1.0;

	const vec3 toLight = texture( lightCube, vToLight ).xyz * 2.0 - 1.0;
	const vec3 lighting = vec3( max( dot( toLight, normal ), 0.0 ) )
		* textureProj( projectionMap, vLightCoord.xyw ).rgb
		* texture( falloffMap, vFalloffCoord ).rgb;

	const float specularAngle = clamp( dot( normalize( vHalfAngle ), normal ), 0.0, 1.0 );
	const vec3 specular = texture( specularTable, vec2( specularAngle, 0.5 ) ).r * 2.0 * fenv[1].rgb
		* texture( specularMap, vSpecularCoord ).rgb;

	const vec3 surface = texture( diffuseMap, vDiffuseCoord ).rgb * fenv[0].rgb + specular;

	outColor = vec4( lighting * surface * vColor.rgb, 0.0 );
}
