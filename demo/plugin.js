/**
 * Vocoder — browser demo.
 *
 * The five shader constants below are `kVertexShader`, `kCopyShader`,
 * `kReduceShader`, `kExpandVShader` and `kExpandHShader` from
 * `source/Shaders.cpp`, copied across unedited and run in the same 33 passes in
 * the same order as `Vocoder::ProcessOpenGL`. `demo/tools/check_shaders.py`
 * compares them to the C++ character for character and `tools/verify.sh` runs
 * it, because two copies of a shader is exactly the arrangement that drifts.
 *
 * The arithmetic further down — `bandGain`, `residualGain`, `tiltDbPerBand`,
 * `masterGain`, `dbToGain`, `compose`, `reducedSize` and `activeLevels` — is a
 * hand port of `source/Controls.cpp` and `source/Pyramid.cpp`. Ported rather
 * than re-derived, because those files exist so that the plugin and its harness
 * cannot disagree about what a slider means, and a third invented copy here
 * would have nothing checking it. **Nothing checks the port but a reader**: the
 * shader check above compares GLSL text and knows nothing about this.
 *
 * ---------------------------------------------------- what is missing, and why
 *
 * **The audio side, entirely.** The plugin is a channel vocoder: eight audio
 * bands driving eight picture bands, with envelope followers, Drive, Floor,
 * Attack, Release, Mapping and Sidechain Mode. The spectrum reaches the plugin
 * through a Resolume `FF_USAGE_FFT` buffer parameter, a browser has no
 * equivalent, and asking a visitor for their microphone to demonstrate a video
 * effect is not a trade worth making. So the whole Audio group is **absent**
 * rather than present and dead, and it is listed in the disclosure at the foot.
 * `vctest --audio` and `vctest --envelope` in the repository are what measure
 * that half.
 *
 * That leaves the plugin in exactly the state it is in when nothing is routed
 * to it — and that is not an approximation. With no audio, every envelope sits
 * at zero and the default Floor of 1 makes `PictureGains` return eight ones, so
 * `Compose` reduces to slider × tilt, which is what this page computes. What a
 * visitor drives here is a graphic EQ for spatial frequency, which is the
 * plugin's other half and is complete.
 *
 * **The About block** — four buttons that open a browser — is absent for the
 * obvious reason.
 *
 * ------------------------------------------------------------ decisions taken
 *
 * - **Float render targets are required, not degraded to.** Every buffer in the
 *   pyramid is RGBA32F, as in the plugin (`kBufferFormat`). WebGL2 makes that an
 *   opt-in (`EXT_color_buffer_float`), so `needFloat` is set and the page fails
 *   with a visible message where the extension is missing. Quietly falling back
 *   to eight bits would quantise every intermediate and render a *plausible*
 *   wrong picture — which is the one failure mode this whole plugin exists to
 *   avoid.
 * - **No claim of bit-exactness is made here.** The plugin's null is that every
 *   gain at 1× returns the input to the bit, and `vctest --identity` measures
 *   exactly 0 on the author's Mac — but 5.96e-08, one float ULP, on hardware
 *   where the two sides of the cancellation round differently. A browser is a
 *   third such platform and nothing on this page measures anything, so the page
 *   says the picture comes back *unchanged*, offers the "Unity — the null"
 *   preset so a visitor can see it, and points at the harness for the number.
 * - **Bands the raster is too small for are disabled, not left dead.** The
 *   pyramid halves until a level would be under two texels on a side, so a small
 *   canvas genuinely has fewer than eight bands — `activeLevels` below is
 *   `Pyramid.cpp`'s own rule. The sliders for the bands that do not exist are
 *   greyed out and the inspector says how many there are, rather than leaving
 *   controls that look live and do nothing.
 *
 * What this page is NOT: it is the plugin's shaders, not the plugin. No
 * Resolume, no FFGL, no C++ — and GLSL ES 3.00 rather than desktop GL 4.1 core,
 * which the kit's `port()` handles.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// The one backtick in kExpandHShader's comments is escaped, because a backtick
// cannot appear raw inside a JavaScript template literal. check_shaders.py
// decodes that single escape before comparing and rejects any other backslash,
// so the escape cannot hide a difference — the C++ contains no backslash at all.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const COPY = `#version 410 core

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
`;

const REDUCE = `#version 410 core

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
`;

const EXPAND_V = `#version 410 core

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
`;

const EXPAND_H = `#version 410 core

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

	//The output. \`level\` is G_0 here: the copy of the input, in whichever
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
`;

//---------------------------------------------------------------------------
// The pyramid's shape — a port of source/Pyramid.cpp.
//---------------------------------------------------------------------------

/** Bands the plugin has. Fixed; the eighth is the 128 px band. */
const MAX_LEVELS = 8;

/** The size of the next level down: ceil( n / 2 ). */
function reducedSize(n) {
  return Math.floor((n + 1) / 2);
}

/**
 * How many Laplacian levels a picture of this size supports, up to MAX_LEVELS.
 * A level exists while the Gaussian below it is at least two texels in both
 * directions — below that a 5-tap kernel is sampling one texel five times and
 * the band it would produce is noise about nothing.
 *
 * This is why a small canvas has fewer than eight bands, and why the sliders
 * for the ones that do not exist are disabled rather than left looking live.
 */
function activeLevels(width, height) {
  let levels = 0;
  let w = width;
  let h = height;
  for (let k = 0; k < MAX_LEVELS; k += 1) {
    w = reducedSize(w);
    h = reducedSize(h);
    if (Math.min(w, h) < 2) break;
    levels += 1;
  }
  return levels;
}

//---------------------------------------------------------------------------
// What the sliders mean — a port of source/Controls.cpp.
//
// Every one of these is 0..1 in the host, because SetParamInfo clamps an
// FF_TYPE_STANDARD default into 0..1 before a range can be attached, so a
// parameter that stands for a gain cannot declare its default as a gain. The
// conversions live in one file in the plugin for the same reason they live in
// one place here: so there is only ever one answer to what a slider position
// means.
//---------------------------------------------------------------------------

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);

/** 0..4x, linear. Unity is at 0.25 — see the hint on Band 1. */
const bandGain = (value) => lerp(0, 4, value);
/** The same mapping, for the residual (the coarsest Gaussian). */
const residualGain = (value) => lerp(0, 4, value);
/** -6..+6 dB per band, zero at 0.5. Positive tilts towards the fine bands. */
const tiltDbPerBand = (value) => lerp(-6, 6, value);
/** 0..2x, linear, unity at 0.5. An output level, not a band gain. */
const masterGain = (value) => lerp(0, 2, value);
/** dB to a linear multiplier. */
const dbToGain = (db) => Math.pow(10, db / 20);

/**
 * Eight ones. This is what `audio::PictureGains` returns when nothing is
 * routed: every envelope sits at zero and the default Floor of 1 makes each
 * band `1 + 0 * ( drive - 1 )`. So passing ones here is not a simplification of
 * the plugin, it is the plugin with no audio source — which is the only state
 * this page can honestly be in.
 */
const SILENT = [1, 1, 1, 1, 1, 1, 1, 1];

/**
 * `controls::Compose`. The slider, the tilt and the audio, multiplied — never
 * added, so a band cut to 0 stays cut whatever else says.
 */
function compose(bandSliders, residualSlider, tiltSlider, audio, extraTiltDbPerBand) {
  const slope = tiltDbPerBand(tiltSlider) + extraTiltDbPerBand;
  const band = new Array(MAX_LEVELS);

  for (let k = 0; k < MAX_LEVELS; k += 1) {
    // Band 0 is the finest. A positive slope lifts it and cuts band 7 by the
    // same amount: 3.5 slopes either side of the middle of the bank.
    const tiltDb = slope * (3.5 - k);
    band[k] = bandGain(bandSliders[k]) * dbToGain(tiltDb) * audio[k];
  }

  // The tilt and the audio are about the bands; the residual is the picture's
  // DC and its coarsest shapes and neither touches it.
  return { band, residual: residualGain(residualSlider) };
}

/**
 * The per-channel gain vector `gainVector` builds in ProcessOpenGL. In Luma
 * mode only channel 0 (Y) is a band; Cb, Cr and alpha ride through at 1, which
 * reconstructs them exactly, so the picture keeps the input's colour.
 */
function gainVector(g, luma) {
  return [g, luma ? 1 : g, luma ? 1 : g, 1];
}

//---------------------------------------------------------------------------
// The chain.
//---------------------------------------------------------------------------

/**
 * `glUniform2i`, which the kit's `set()` does not cover — it picks the float
 * overloads. The plugin has the same helper for the same reason: every size and
 * every axis in these shaders is an `ivec2`, and handing one a float silently
 * leaves it at whatever it held before.
 */
function setIVec2(gl, program, name, x, y) {
  const location = program.location(name);
  if (location !== null) gl.uniform2i(location, x, y);
}

/** Set once per frame by the renderer, read by the inspector note below. */
let reportLevels = () => {};

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const reduceShader = new Program(gl, VERTEX, REDUCE, 'reduce');
  const expandVShader = new Program(gl, VERTEX, EXPAND_V, 'expand V');
  const expandHShader = new Program(gl, VERTEX, EXPAND_H, 'expand H');

  // Nearest, like the plugin's PassBuffer::Sampling::Nearest. Every read in
  // every shader is a texelFetch at integer coordinates, so the filter never
  // runs — but an RGBA32F texture asked for LINEAR is incomplete in WebGL2
  // without OES_texture_float_linear, which would be a black frame rather than
  // a wrong one.
  const nearest = { filter: 'nearest' };

  // G_0 is the copy; gauss[ k ] holds G_{k+1}; temp[ k ] is the half-width
  // intermediate both the reduce and the expand of level k pass through;
  // recon[ k ] holds R_{k+1}. R_L is never stored — it is r * G_L, and the top
  // expand reads G_L with that gain — and R_0 is the canvas.
  const copyBuffer = new PassBuffer(gl, nearest);
  const gauss = Array.from({ length: MAX_LEVELS }, () => new PassBuffer(gl, nearest));
  const temp = Array.from({ length: MAX_LEVELS }, () => new PassBuffer(gl, nearest));
  const recon = Array.from({ length: MAX_LEVELS }, () => new PassBuffer(gl, nearest));

  const gaussian = (level) => (level <= 0 ? copyBuffer : gauss[level - 1]);

  return {
    render({ input, params, width, height }) {
      const luma = params.option('carrier') === 1;
      const levels = activeLevels(width, height);
      reportLevels(levels, width, height);

      const sliders = [];
      for (let k = 0; k < MAX_LEVELS; k += 1) sliders.push(params.get(`band${k + 1}`));
      const gains = compose(sliders, params.get('residual'), params.get('tilt'), SILENT, 0);

      const levelWidth = [width];
      const levelHeight = [height];
      for (let k = 1; k <= levels; k += 1) {
        levelWidth[k] = reducedSize(levelWidth[k - 1]);
        levelHeight[k] = reducedSize(levelHeight[k - 1]);
      }

      // Every allocation first, before anything binds a texture — the plugin's
      // own rule, for the same reason: allocating a buffer mid-chain can unbind
      // the texture the next pass is about to read, and the symptom is a frame
      // that is correct except on the one frame that resized.
      const F = gl.RGBA32F;
      copyBuffer.ensure(width, height, F);
      for (let k = 0; k < levels; k += 1) {
        gauss[k].ensure(levelWidth[k + 1], levelHeight[k + 1], F);
        temp[k].ensure(levelWidth[k + 1], levelHeight[k], F);
        if (k + 1 < levels) recon[k].ensure(levelWidth[k + 1], levelHeight[k + 1], F);
      }

      gl.disable(gl.BLEND);

      //---------------------------------------------------------------
      // 1. The picture, into a float buffer of ours.
      //---------------------------------------------------------------
      copyBuffer.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      setIVec2(gl, copyShader, 'TargetSize', width, height);
      copyShader.setInt('LumaMode', luma ? 1 : 0);
      quad.draw();

      //---------------------------------------------------------------
      // 2. Reduce: G_0 -> G_1 -> ... -> G_L, two passes each.
      //---------------------------------------------------------------
      for (let k = 0; k < levels; k += 1) {
        const halves = [
          { from: gaussian(k), to: temp[k], axisX: 1, axisY: 0,
            fromW: levelWidth[k], fromH: levelHeight[k], toW: levelWidth[k + 1], toH: levelHeight[k] },
          { from: temp[k], to: gauss[k], axisX: 0, axisY: 1,
            fromW: levelWidth[k + 1], fromH: levelHeight[k], toW: levelWidth[k + 1], toH: levelHeight[k + 1] },
        ];

        for (const half of halves) {
          half.to.bind();
          reduceShader.use();
          bindTexture(gl, 0, half.from.texture);
          reduceShader.setSampler('SourceTexture', 0);
          setIVec2(gl, reduceShader, 'SourceSize', half.fromW, half.fromH);
          setIVec2(gl, reduceShader, 'TargetSize', half.toW, half.toH);
          setIVec2(gl, reduceShader, 'Axis', half.axisX, half.axisY);
          quad.draw();
        }
      }

      //---------------------------------------------------------------
      // 3. Reconstruct, top down.
      //
      //     R_L = r * G_L
      //     R_k = EXPAND( R_{k+1} - g_k G_{k+1} ) + g_k G_k
      //
      // which is why the null is exact in the plugin: at every gain of 1 the
      // thing being expanded is G - G. No Laplacian band is ever materialised.
      //---------------------------------------------------------------
      for (let k = levels - 1; k >= 0; k -= 1) {
        const top = k + 1 === levels;
        const final = k === 0;

        const levelGain = gainVector(gains.band[k], luma);
        const reconGain = gainVector(top ? gains.residual : 1, luma);

        // Vertical: (W_{k+1} x H_{k+1}) -> temp[ k ] (W_{k+1} x H_k).
        {
          const above = top ? gauss[k] : recon[k];

          temp[k].bind();
          expandVShader.use();
          bindTexture(gl, 0, above.texture);
          bindTexture(gl, 1, gauss[k].texture);
          expandVShader.setSampler('ReconTexture', 0);
          expandVShader.setSampler('LevelTexture', 1);
          expandVShader.set('ReconGain', reconGain);
          expandVShader.set('LevelGain', levelGain);
          setIVec2(gl, expandVShader, 'SourceSize', levelWidth[k + 1], levelHeight[k + 1]);
          setIVec2(gl, expandVShader, 'TargetSize', levelWidth[k + 1], levelHeight[k]);
          setIVec2(gl, expandVShader, 'Axis', 0, 1);
          quad.draw();
        }

        // Horizontal: temp[ k ] -> R_k (W_k x H_k), or the canvas.
        if (!final) {
          recon[k - 1].bind();
          expandHShader.use();
          bindTexture(gl, 0, temp[k].texture);
          bindTexture(gl, 1, gaussian(k).texture);
          expandHShader.setSampler('SourceTexture', 0);
          expandHShader.setSampler('LevelTexture', 1);
          expandHShader.set('LevelGain', levelGain);
          setIVec2(gl, expandHShader, 'SourceSize', levelWidth[k + 1], levelHeight[k]);
          setIVec2(gl, expandHShader, 'TargetSize', levelWidth[k], levelHeight[k]);
          setIVec2(gl, expandHShader, 'Axis', 1, 0);
          expandHShader.setInt('Final', 0);
          quad.draw();
        } else {
          // Back to the canvas. The kit bound it and set the viewport before
          // calling us, and every pass since has bound a buffer of its own at
          // another size — which is the same thing ProcessOpenGL has to do with
          // the host's viewport, for the same reason.
          gl.bindFramebuffer(gl.FRAMEBUFFER, null);
          gl.viewport(0, 0, width, height);

          expandHShader.use();
          bindTexture(gl, 0, temp[0].texture);
          bindTexture(gl, 1, copyBuffer.texture);
          expandHShader.setSampler('SourceTexture', 0);
          expandHShader.setSampler('LevelTexture', 1);
          expandHShader.set('LevelGain', levelGain);
          setIVec2(gl, expandHShader, 'SourceSize', levelWidth[1], levelHeight[0]);
          setIVec2(gl, expandHShader, 'TargetSize', width, height);
          setIVec2(gl, expandHShader, 'Axis', 1, 0);
          expandHShader.setInt('Final', 1);
          expandHShader.setInt('LumaMode', luma ? 1 : 0);
          expandHShader.set('Master', masterGain(params.get('master')));
          expandHShader.set('MixAmount', params.get('mix'));
          quad.draw();
        }
      }

      if (levels === 0) {
        // A picture too small for even one band — under three texels on a side,
        // which on this page needs `?size=` and a very small number. The output
        // is r * G_0 through the same output pass: with Axis (0,0) the expand
        // loop reads texel p three times with weights summing to a half,
        // doubled, so the "expand" term is exactly the source texel, and a level
        // gain of r - 1 makes the whole thing r * G_0.
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);

        const levelGain = gainVector(gains.residual - 1, luma);
        levelGain[3] = 0; // alpha: 1 from the expand term, nothing more

        expandHShader.use();
        bindTexture(gl, 0, copyBuffer.texture);
        bindTexture(gl, 1, copyBuffer.texture);
        expandHShader.setSampler('SourceTexture', 0);
        expandHShader.setSampler('LevelTexture', 1);
        expandHShader.set('LevelGain', levelGain);
        setIVec2(gl, expandHShader, 'SourceSize', width, height);
        setIVec2(gl, expandHShader, 'TargetSize', width, height);
        setIVec2(gl, expandHShader, 'Axis', 0, 0);
        expandHShader.setInt('Final', 1);
        expandHShader.setInt('LumaMode', luma ? 1 : 0);
        expandHShader.set('Master', masterGain(params.get('master')));
        expandHShader.set('MixAmount', params.get('mix'));
        quad.draw();
      }
    },
  };
}

//---------------------------------------------------------------------------
// The controls, read out of the plugin's own constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// The Audio group is absent, for the reason at the top of this file, and so is
// the About block, which is four buttons that open a browser.
//---------------------------------------------------------------------------

/** The readout beside a band slider: the multiplier the plugin converts it to. */
const asGain = (v) => `${bandGain(v).toFixed(2)}×`;

const bandNames = [
  'Band 1 (1 px)', 'Band 2 (2 px)', 'Band 3 (4 px)', 'Band 4 (8 px)',
  'Band 5 (16 px)', 'Band 6 (32 px)', 'Band 7 (64 px)', 'Band 8 (128 px)',
];

// A band's name is its level's sampling pitch. Its power actually peaks well
// below that level's Nyquist, and `vctest --band` measures where rather than
// assuming: 2.0, 10.1, 21.5, 43.5, 87.3, 174.5, 347.0 and 677.1 px.
const bandPeaks = [2.0, 10.1, 21.5, 43.5, 87.3, 174.5, 347.0, 677.1];

const bandParams = bandNames.map((name, k) => ({
  id: `band${k + 1}`,
  name,
  type: 'standard',
  default: 0.25,
  group: 'EQ',
  display: asGain,
  hint: k === 0
    ? 'The finest band. 0 to 4×, LINEAR in gain, so unity is a quarter of the way up and not half — a vocoder band spends most of its life between "off" and "as loud as the carrier", so the cut side gets a usable length and the boost side room to ring. Double-click for the default.'
    : `The ${name.match(/\(([^)]+)\)/)[1]} band. 0 to 4×, unity at a quarter. Its power peaks at about ${bandPeaks[k]} px — a band's name is its level's sampling pitch, not where it rings, and vctest --band measures the difference.`,
}));

mountDemo({
  name: 'Vocoder',
  pluginId: 'VC01',
  tagline:
    'A channel vocoder with the picture as the carrier. The frame is split into a Laplacian pyramid — eight octave bands of spatial frequency from one-pixel detail to 128-pixel shapes, plus a residual — each band is multiplied by a gain, and the bands are summed back. With every gain at 1× the picture comes back unchanged, which is why the page opens looking like it is doing nothing: that null is the whole point, and everything else is measured from it.',
  repo: 'https://github.com/stoatworks-labs/vocoder',

  // The stock banner sentence says "same parameters", and that would be an
  // overclaim here: the seven audio parameters are not on this page at all.
  blurb:
    "It is Vocoder's own GLSL, copied from the repository and run in WebGL2 on generated clips in this page — the plugin's spatial-frequency EQ, with the plugin's own controls and the plugin's own maths. The audio side, which is the plugin's headline, is not here and is not faked.",

  // The output carries the input's alpha exactly: every gain on the alpha
  // channel is 1, so it reconstructs to the bit and the "spectrum only" view of
  // a logo keeps its edge.
  showBackdrop: true,

  // Every buffer in the pyramid is RGBA32F, as in the plugin. A float render
  // target is an opt-in in WebGL2 (EXT_color_buffer_float) where desktop GL just
  // has it, and eight bits mid-chain would quantise every intermediate and
  // produce a plausible wrong picture rather than an obvious one. The kit throws
  // and the page says so, rather than degrading.
  needFloat: true,

  params: [
    ...bandParams,

    { id: 'residual', name: 'Residual', type: 'standard', default: 0.25, group: 'EQ',
      display: asGain,
      hint: 'The coarsest Gaussian — the picture\'s DC and its largest shapes. Turn it to 0 and what is left is the picture as its detail bands, which is a new kind of edge picture. Neither the tilt nor the audio touches it.' },
    { id: 'tilt', name: 'Tilt', type: 'standard', default: 0.5, group: 'EQ',
      display: (v) => `${tiltDbPerBand(v) >= 0 ? '+' : ''}${tiltDbPerBand(v).toFixed(1)} dB/band`,
      hint: '±6 dB per band about the middle of the bank. Positive lifts the fine bands and cuts the coarse ones, which is a sharpen; negative is a soften. Band 1 and band 8 move 3.5 slopes in opposite directions.' },
    { id: 'master', name: 'Master', type: 'standard', default: 0.5, group: 'EQ',
      display: (v) => `${masterGain(v).toFixed(2)}×`,
      hint: '0 to 2×, unity at half — an output level, so it is not on the 0..4× scale the band gains use.' },
    { id: 'carrier', name: 'Carrier', type: 'option', default: 0, group: 'EQ',
      elements: ['RGB', 'Luma'],
      hint: 'RGB puts all three channels through the bank. Luma puts only Y through it and lets Cb and Cr ride at unity, which reconstructs them exactly — so the picture keeps the input\'s colour and only its brightness structure is EQ\'d.' },

    { id: 'mix', name: 'Mix', type: 'standard', default: 1, group: 'Output',
      hint: 'Between the input and the reconstruction. On the defaults it is correctly dead, because both sides are the same picture.' },
  ],

  sources: ['detail', 'grid', 'scene', 'bars', 'ramp', 'spot', 'alpha'],

  presets: {
    // A preset is a whole state: anything it does not name goes back to the
    // plugin's own default, so this one is the null exactly.
    'Unity — the null': {},
    'Spectrum only': { residual: 0 },
    'Ring at 4 px': { band3: 1 },
    'Sharpen': { tilt: 1 },
    'Soften': { tilt: 0 },
    'Detail only': { band5: 0, band6: 0, band7: 0, band8: 0, residual: 0 },
    'Shapes only': { band1: 0, band2: 0, band3: 0, band4: 0 },
    'Luma EQ, colour untouched': { carrier: 1, band1: 1, band2: 0.5 },
    'Notch the 16 px band': { band5: 0 },
  },

  differences: [
    'The audio side is not here at all. The plugin is a vocoder: eight audio bands driving these eight picture bands, through envelope followers with their own Drive, Floor, Attack, Release, Mapping and Sidechain Mode. That spectrum reaches the plugin as a Resolume FFT buffer parameter and a browser has no equivalent, so those seven controls are absent rather than present and dead. Everything the audio does is move these sliders — drag Band 8 and you are seeing what a kick drum does; what you cannot see is how it decides.',
    'With nothing routed, though, this is not an approximation of the plugin — it is the plugin. Every envelope sits at zero and the default Floor of 1 makes the audio contribution exactly eight ones, so the gains are slider × tilt, which is what this page computes.',
    'The null is not claimed to be bit-exact here. In the plugin, every gain at 1× returns the input to the bit, and the harness asserts a maximum difference of exactly 0 rather than a tolerance — except on hardware where the two sides of the cancellation round differently, where it reads one float ULP. A browser is a third such platform and nothing on this page measures anything, so all this page can show you is that the picture comes back looking unchanged. Load the "Unity — the null" preset and see.',
    'On a small raster there are fewer than eight bands, and the inspector says how many. The pyramid halves until a level would be under two texels on a side, so a 320 × 180 picture has seven bands and its residual is the 3 × 2 Gaussian. The sliders for bands that do not exist are disabled here, which is what the plugin does with them too — it just has no way to tell you.',
    'The band gains are applied to the premultiplied picture the host hands over, and alpha is never gained. Boost a band hard on the "Shape on transparency" clip and the colour can exceed the alpha carrying it, exactly as it would in Resolume.',
    'The numbers behind all of this — the reconstruction measured at exactly 0, the eight bands measured against an independent CPU implementation of Burt–Adelson at 1.2e-07, and each band\'s peak measured rather than assumed — come from an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The band count, on the page.
//
// `activeLevels` is a fact about the raster, not about the plugin's settings,
// so a visitor who picks 640 × 360 or drops in a small still needs telling that
// some of these sliders now have nothing to act on. A control that is present
// and dead is worse than one that is absent, and these cannot be absent because
// the plugin declares eight of them whatever the picture is.
//---------------------------------------------------------------------------

const note = document.createElement('p');
note.className = 'inspector__note vocoder-levels';

const style = document.createElement('style');
style.textContent = `
  .vocoder-levels { margin-top: 0; }
  .prow.is-absent { opacity: 0.4; }
  .prow.is-absent .prow__slider { cursor: not-allowed; }
`;
document.head.append(style);

let lastLevels = -1;

reportLevels = (levels, width, height) => {
  const body = document.querySelector('.inspector__body');
  if (!body) return; // embed mode: no inspector
  if (!note.isConnected) body.prepend(note);

  if (levels === lastLevels) return;
  lastLevels = levels;

  note.textContent = levels >= MAX_LEVELS
    ? `This raster is ${width} × ${height}, which carries all eight bands.`
    : `This raster is ${width} × ${height}, so the pyramid stops at ${levels} `
      + `level${levels === 1 ? '' : 's'}: band${levels === 7 ? ' 8 does' : `s ${levels + 1}–8 do`} not exist here `
      + `and the residual is the Gaussian at level ${levels}. Their sliders are disabled rather than left looking live.`;

  for (let k = 0; k < MAX_LEVELS; k += 1) {
    const slider = document.getElementById(`p-band${k + 1}`);
    const row = slider?.closest('.prow');
    if (!slider || !row) continue;
    const absent = k >= levels;
    row.classList.toggle('is-absent', absent);
    slider.disabled = absent;
    if (absent) slider.title = `This raster has ${levels} band${levels === 1 ? '' : 's'}, so this one does not exist.`;
    else slider.removeAttribute('title');
  }
};
