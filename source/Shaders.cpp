#include "Shaders.h"

namespace vocoder
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 target space. Every pass turns this into an
	//integer texel of its own target: uv * TargetSize lands on x + 0.5 for
	//texel x, and the truncation is exact. That is how the final pass finds
	//its pixel in the host's framebuffer whatever viewport origin the host
	//gave it, and it is why nothing here reads gl_FragCoord.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform ivec2 TargetSize;   //the picture, which is also this pass's target
uniform int LumaMode;       //1: write Y, Cb, Cr, A instead of R, G, B, A

in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 p = clamp( ivec2( uv * vec2( TargetSize ) ), ivec2( 0 ), TargetSize - 1 );

	//texelFetch, not texture(): the host's texture may be larger than the
	//picture (hence MaxUV in every other FFGL plugin) and may be set to
	//filter; an integer fetch of texel (x, y) is the picture's texel (x, y)
	//regardless of either, because FFGL puts the picture in the bottom-left
	//of the hardware texture.
	vec4 c = texelFetch( InputTexture, p, 0 );

	if( LumaMode == 1 )
	{
		//BT.709 luma, and colour differences that invert exactly: R = Y + Cr,
		//B = Y + Cb, G from what is left. Premultiplied in, premultiplied out
		//-- these are linear in the pixel, so alpha rides along untouched.
		float y = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		c = vec4( y, c.b - y, c.r - y, c.a );
	}

	fragColor = c;
}
)";

//---------------------------------------------------------------------------
// Pass 2: reduce, one axis at a time.
//---------------------------------------------------------------------------
const char* const kReduceShader = R"(#version 410 core

uniform sampler2D SourceTexture;
uniform ivec2 SourceSize;
uniform ivec2 TargetSize;
uniform ivec2 Axis;         //(1,0) halves the width, (0,1) halves the height

in vec2 uv;
out vec4 fragColor;

//Burt and Adelson's generating kernel. Mirrored in Pyramid.h.
const float W[ 5 ] = float[ 5 ]( 0.0625, 0.25, 0.375, 0.25, 0.0625 );

void main()
{
	ivec2 p = clamp( ivec2( uv * vec2( TargetSize ) ), ivec2( 0 ), TargetSize - 1 );

	//Output x' reads source 2x' - 2 .. 2x' + 2 along the axis. The other
	//coordinate passes straight through.
	ivec2 base  = p * ( ivec2( 1 ) + Axis );
	ivec2 limit = SourceSize - 1;

	vec4 sum = vec4( 0.0 );
	for( int i = 0; i < 5; ++i )
	{
		ivec2 q = clamp( base + Axis * ( i - 2 ), ivec2( 0 ), limit );
		sum += W[ i ] * texelFetch( SourceTexture, q, 0 );
	}

	fragColor = sum;
}
)";

//---------------------------------------------------------------------------
// Pass 3: the vertical half of expand, on the difference.
//---------------------------------------------------------------------------
const char* const kExpandVShader = R"(#version 410 core

uniform sampler2D ReconTexture;   //R_{k+1}, or G_L at the top
uniform sampler2D LevelTexture;   //G_{k+1}
uniform vec4 ReconGain;           //1, or the residual gain at the top
uniform vec4 LevelGain;           //g_k, per channel
uniform ivec2 SourceSize;         //the level above: W_{k+1} x H_{k+1}
uniform ivec2 TargetSize;         //W_{k+1} x H_k
uniform ivec2 Axis;               //(0,1): this pass expands vertically

in vec2 uv;
out vec4 fragColor;

const float W[ 5 ] = float[ 5 ]( 0.0625, 0.25, 0.375, 0.25, 0.0625 );

void main()
{
	ivec2 p = clamp( ivec2( uv * vec2( TargetSize ) ), ivec2( 0 ), TargetSize - 1 );

	//EXPAND is REDUCE's adjoint: output x reads every coarse sample x' whose
	//kernel index x - 2x' + 2 is inside the kernel. Three taps when x is
	//even, two when it is odd; doubled, both parities sum to one. Written as
	//a loop over the three candidates with the out-of-kernel one skipped,
	//rather than as a branch on parity, so both parities run the same code.
	int x = p.x * Axis.x + p.y * Axis.y;
	int m = x / 2;

	ivec2 across = p - Axis * x;   //the coordinate this pass does not touch
	ivec2 limit  = SourceSize - 1;

	vec4 sum = vec4( 0.0 );
	for( int j = -1; j <= 1; ++j )
	{
		int xp  = m + j;
		int idx = x - 2 * xp + 2;
		if( idx < 0 || idx > 4 )
			continue;
		ivec2 q = clamp( across + Axis * xp, ivec2( 0 ), limit );

		//The difference, at every tap. When ReconGain and LevelGain are both
		//one this is G_{k+1} - G_{k+1}: exactly zero, and the whole expand is
		//exactly zero, and the reconstruction is exact. See Pyramid.h.
		vec4 recon = ReconGain * texelFetch( ReconTexture, q, 0 );
		vec4 level = LevelGain * texelFetch( LevelTexture, q, 0 );
		sum += W[ idx ] * ( recon - level );
	}

	fragColor = 2.0 * sum;
}
)";

//---------------------------------------------------------------------------
// Pass 4: the horizontal half of expand, plus g_k * G_k. Also the output.
//---------------------------------------------------------------------------
const char* const kExpandHShader = R"(#version 410 core

uniform sampler2D SourceTexture;  //the vertically expanded difference
uniform sampler2D LevelTexture;   //G_k
uniform vec4 LevelGain;           //g_k, per channel
uniform ivec2 SourceSize;         //W_{k+1} x H_k
uniform ivec2 TargetSize;         //W_k x H_k
uniform ivec2 Axis;               //(1,0): this pass expands horizontally

//The output pass only.
uniform int Final;
uniform int LumaMode;
uniform float Master;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

const float W[ 5 ] = float[ 5 ]( 0.0625, 0.25, 0.375, 0.25, 0.0625 );

vec4 toRgb( vec4 c )
{
	//The inverse of the copy pass's transform, exactly.
	float y = c.x;
	float r = y + c.z;
	float b = y + c.y;
	float g = ( y - 0.2126 * r - 0.0722 * b ) / 0.7152;
	return vec4( r, g, b, c.a );
}

void main()
{
	ivec2 p = clamp( ivec2( uv * vec2( TargetSize ) ), ivec2( 0 ), TargetSize - 1 );

	int x = p.x * Axis.x + p.y * Axis.y;
	int m = x / 2;

	ivec2 across = p - Axis * x;
	ivec2 limit  = SourceSize - 1;

	vec4 sum = vec4( 0.0 );
	for( int j = -1; j <= 1; ++j )
	{
		int xp  = m + j;
		int idx = x - 2 * xp + 2;
		if( idx < 0 || idx > 4 )
			continue;
		ivec2 q = clamp( across + Axis * xp, ivec2( 0 ), limit );
		sum += W[ idx ] * texelFetch( SourceTexture, q, 0 );
	}

	vec4 level = texelFetch( LevelTexture, p, 0 );
	vec4 recon = 2.0 * sum + LevelGain * level;

	if( Final == 0 )
	{
		fragColor = recon;
		return;
	}

	//The output. `level` is G_0 here: the copy of the input, in whichever
	//space the copy pass wrote it.
	vec4 original = level;
	if( LumaMode == 1 )
	{
		recon    = toRgb( recon );
		original = toRgb( original );
	}

	recon.rgb *= Master;

	vec4 result = mix( original, recon, MixAmount );

	//Clipped at the host's range and nothing cleverer. A band boosted past
	//what the carrier can hold clips, as it would on a mixing desk. Alpha is
	//not touched: every gain on it was one, so it is the input's alpha to the
	//bit, and the "spectrum only" view of a logo keeps its edge.
	fragColor = vec4( clamp( result.rgb, 0.0, 1.0 ), result.a );
}
)";

} // namespace vocoder
