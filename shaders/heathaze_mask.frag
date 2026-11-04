#version 460

layout(std140, binding = 0) uniform FsEnv {
	vec4 fenv[8];
};
layout(std140, binding = 1) uniform FsLocal {
	vec4 flocal[16];
};

layout(location = 0) out vec4 outColor;

// fenv[0]		the viewport to _currentRender scale
// fenv[1].xy	1 / the viewport's size
// fenv[7]		the screen's height in x, the viewport's left in y and its bottom in z

layout(location = 0) in vec2 vMaskTexCoord;
layout(location = 1) in vec2 vScrollTexCoord;
layout(location = 2) in vec4 vDeformScale;
layout(location = 3) in vec4 vColor;

layout(binding = 0) uniform sampler2D currentRender;
layout(binding = 1) uniform sampler2D normalMap;
layout(binding = 2) uniform sampler2D maskMap;

void main() {
	// a mask below 0.01 kills the fragment: the effect stays in the authored blob
	vec4 mask = texture( maskMap, vMaskTexCoord );
	mask.xy -= 0.01;
	if ( mask.x < 0.0 || mask.y < 0.0 ) {
		discard;
	}

	vec4 localNormal = texture( normalMap, vScrollTexCoord );
	localNormal.x = localNormal.a;	// normal maps keep X in alpha
	localNormal = localNormal * 2.0 - 1.0;
	localNormal *= mask;

	// measured from the bottom, as the copy of the screen is
	const vec2 window = vec2( gl_FragCoord.x - fenv[7].y, fenv[7].x - gl_FragCoord.y - fenv[7].z );
	vec4 screen = vec4( window * fenv[1].xy, 0.0, 1.0 );
	screen = clamp( localNormal * vDeformScale + screen, 0.0, 1.0 );
	screen *= fenv[0];

	outColor = vec4( texture( currentRender, screen.xy ).rgb, 1.0 );
}
