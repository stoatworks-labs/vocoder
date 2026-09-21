#include "Vocoder.h"

#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace vocoder;

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kBandNames[ controls::kBands ] = {
	"Band 1 (1 px)", "Band 2 (2 px)", "Band 3 (4 px)", "Band 4 (8 px)",
	"Band 5 (16 px)", "Band 6 (32 px)", "Band 7 (64 px)", "Band 8 (128 px)",
};
const char* const kCarrierNames[] = { "RGB", "Luma" };
const char* const kMappingNames[] = { "Direct", "Reverse" };
const char* const kModeNames[]    = { "Vocoder", "Dynamics" };

/// The pyramid's buffer format. 32-bit float, not 16: the identity is exact
/// in any format (see Pyramid.h), but the partition check in `vctest
/// --identity` sums nine separately reconstructed bands and the `--band`
/// check compares a row against a float CPU reference, and both want the
/// rounding to be a tenth of a per-mille, not a tenth of a percent. The cost
/// is bandwidth; `vctest --bench` has the number.
constexpr GLint kBufferFormat = GL_RGBA32F;

/// Seconds of host time a single frame is allowed to advance the followers
/// by. The host's clock jumps when the composition is scrubbed, when a clip
/// is retriggered, and by however long the machine was asleep; an unclamped
/// delta turns any of those into every envelope snapping.
constexpr double kMaxFrameDelta = 0.25;

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}
} // namespace

//---------------------------------------------------------------------------
Vocoder::Vocoder()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The host drives the followers' clock where it can, so that rendering the
	//same frame twice gives the same picture twice.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter, so
	// these assignments are what the host is told the defaults are.
	//
	// They add up to an identity: every band at 1x, no tilt, unity master,
	// and a Floor of 1 so that with no audio routed the sidechain multiplies
	// everything by one. Dropped on a layer, the plugin does nothing until
	// somebody moves something, which for an EQ is the only sane null.
	//---------------------------------------------------------------------
	for( int k = 0; k < controls::kBands; ++k )
		params[ PT_BAND_1 + k ] = 0.25f;//1x
	params[ PT_RESIDUAL ] = 0.25f;     //1x
	params[ PT_TILT ]     = 0.5f;      //flat
	params[ PT_MASTER ]   = 0.5f;      //1x
	params[ PT_CARRIER ]  = 0.0f;      //RGB

	params[ PT_DRIVE ]   = 0.5f;//12 dB
	params[ PT_FLOOR ]   = 1.0f;//audio only adds
	params[ PT_ATTACK ]  = 0.5f;//about 22 ms
	params[ PT_RELEASE ] = 0.5f;//200 ms
	params[ PT_MAPPING ] = 0.0f;//Direct: bass pumps the shapes
	params[ PT_MODE ]    = 0.0f;//Vocoder

	params[ PT_MIX ] = 1.0f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every numeric parameter is a plain 0..1 float even where it stands for a
	// gain or a time. SetParamInfo clamps an FF_TYPE_STANDARD default into
	// 0..1 *before* a range can be attached (SDK b1afaf9), so a parameter
	// declared in seconds cannot declare a default in seconds. The
	// conversions live in Controls.cpp.
	//---------------------------------------------------------------------
	for( int k = 0; k < controls::kBands; ++k )
		SetParamInfof( PT_BAND_1 + k, kBandNames[ k ], FF_TYPE_STANDARD );
	SetParamInfof( PT_RESIDUAL, "Residual", FF_TYPE_STANDARD );
	SetParamInfof( PT_TILT, "Tilt", FF_TYPE_STANDARD );
	SetParamInfof( PT_MASTER, "Master", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_CARRIER, "Carrier", 2, params[ PT_CARRIER ] );
	for( int i = 0; i < 2; ++i )
		SetParamElementInfo( PT_CARRIER, i, kCarrierNames[ i ], static_cast< float >( i ) );

	// Audio. An FFT buffer: Resolume shows it as an audio-source picker and
	// writes one spectrum bin per element. Element defaults are zero on
	// purpose -- with no audio routed, every envelope sits at zero and every
	// band at Floor.
	SetBufferParamInfo( PT_AUDIO, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );

	SetParamInfof( PT_DRIVE, "Drive", FF_TYPE_STANDARD );
	SetParamInfof( PT_FLOOR, "Floor", FF_TYPE_STANDARD );
	SetParamInfof( PT_ATTACK, "Attack", FF_TYPE_STANDARD );
	SetParamInfof( PT_RELEASE, "Release", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_MAPPING, "Mapping", 2, params[ PT_MAPPING ] );
	for( int i = 0; i < 2; ++i )
		SetParamElementInfo( PT_MAPPING, i, kMappingNames[ i ], static_cast< float >( i ) );

	SetOptionParamInfo( PT_MODE, "Sidechain Mode", 2, params[ PT_MODE ] );
	for( int i = 0; i < 2; ++i )
		SetParamElementInfo( PT_MODE, i, kModeNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//Grouped the way Resolume shows them. SetParamGroup collapses RUNS of
	//consecutive same-group ids, so each group has to be contiguous in the
	//enum -- which they are, by construction.
	for( FFUInt32 i = PT_BAND_1; i <= PT_CARRIER; ++i )
		SetParamGroup( i, "EQ" );
	for( FFUInt32 i = PT_AUDIO; i <= PT_MODE; ++i )
		SetParamGroup( i, "Audio" );
	SetParamGroup( PT_MIX, "Output" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	//The About header is provisional (see StoatworksAbout.h). If a regenerated
	//copy adds a guide link the button count changes, and this is where it is
	//noticed rather than in a host that shows one parameter too many.
	static_assert( stoatworks::about::kParamCount == 4,
	               "the About block changed size: check PT_COUNT and the sweep's About cut-off" );

	//Compose the defaults once so the test hooks have something before the
	//first frame.
	float silent[ controls::kBands ];
	for( float& g : silent )
		g = 1.0f;
	float sliders[ controls::kBands ];
	for( int k = 0; k < controls::kBands; ++k )
		sliders[ k ] = params[ PT_BAND_1 + k ];
	gains = controls::Compose( sliders, params[ PT_RESIDUAL ], params[ PT_TILT ], silent, 0.0f );

	FFGLLog::LogToHost( "Created Vocoder effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Vocoder::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &reduceShader, kReduceShader, "reduce" },
		{ &expandVShader, kExpandVShader, "expandV" },
		{ &expandHShader, kExpandHShader, "expandH" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect simply
		//does nothing in Resolume, with no message anywhere. These two lines
		//are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Vocoder: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Vocoder: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised, " + std::to_string( pyramid::kMaxLevels ) + " bands" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
double Vocoder::advanceClock()
{
	// Normalise the host's clock to seconds. steady_clock says how much real
	// time passed, the host says how much host time passed, and the ratio
	// names the unit outright -- 1 for seconds, 1000 for milliseconds, and
	// nothing plausible in between. Several frames rather than one, so a
	// single odd frame cannot decide it alone.
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				diag::info( std::string( "host clock unit decided: " )
				            + ( clockScale == 1.0 ? "seconds" : "milliseconds" ) );
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate, where assuming
	// seconds would be a thousand times fast on Resolume.
	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now )
		            + " sampleRate=" + std::to_string( sampleRateSeen ) );

	return now;
}

//---------------------------------------------------------------------------
void Vocoder::updateAudio( double now )
{
	float bins[ audio::kBins ] = {};
	int count                  = 0;

	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info != nullptr )
	{
		count = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
		float peak = 0.0f;
		for( int i = 0; i < count; ++i )
		{
			bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
			peak      = std::max( peak, bins[ i ] );
		}

		//Once, when the first non-zero spectrum arrives. Whether an audio
		//source is routed at all is the first question anyone asks when the
		//sidechain seems to do nothing, and nothing else in FFGL answers it.
		if( !audioSeen && peak > 0.0f )
		{
			audioSeen = true;
			diag::info( "audio input active: " + std::to_string( count ) + " bins, peak "
			            + std::to_string( peak ) );
		}
	}

	float levels[ audio::kBands ];
	for( int a = 0; a < audio::kBands; ++a )
		levels[ a ] = audio::BandLevel( bins, count, a );

	//The followers' own clock, off the normalised one. First frame snaps; a
	//clock that has not moved holds; a jump is clamped.
	double dt = -1.0;
	if( audioClock >= 0.0 )
		dt = std::clamp( now - audioClock, 0.0, kMaxFrameDelta );
	audioClock = now;

	follower.Update( levels, dt,
	                 controls::AttackSeconds( params[ PT_ATTACK ] ),
	                 controls::ReleaseSeconds( params[ PT_RELEASE ] ) );

	//Compose this frame's gains.
	const auto mapping = static_cast< audio::Mapping >( std::lround( params[ PT_MAPPING ] ) );
	const auto mode    = static_cast< audio::Mode >( std::lround( params[ PT_MODE ] ) );
	const float drive  = controls::DbToGain( controls::DriveDb( params[ PT_DRIVE ] ) );

	float perBand[ controls::kBands ];
	float extraTilt = 0.0f;

	if( mode == audio::Mode::Dynamics )
	{
		//One envelope for the lot, and it drives the Tilt: at full level the
		//finest band is lifted by Drive dB and the coarsest cut by the same,
		//which is a sharpen that follows the music. Floor and Mapping have no
		//meaning here and do nothing.
		for( float& g : perBand )
			g = 1.0f;
		extraTilt = follower.Overall() * controls::DriveDb( params[ PT_DRIVE ] ) / 3.5f;
	}
	else
	{
		audio::PictureGains( follower.Envelopes(), mapping, drive, params[ PT_FLOOR ], perBand );
	}

	float sliders[ controls::kBands ];
	for( int k = 0; k < controls::kBands; ++k )
		sliders[ k ] = params[ PT_BAND_1 + k ];

	gains = controls::Compose( sliders, params[ PT_RESIDUAL ], params[ PT_TILT ], perBand, extraTilt );
}

//---------------------------------------------------------------------------
PassBuffer& Vocoder::gaussian( int level )
{
	return level <= 0 ? copyBuffer : gauss[ level - 1 ];
}

//---------------------------------------------------------------------------
FFResult Vocoder::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( picture.Width );
	const int height = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//
	//`ScopedFBOBinding` restores the framebuffer binding and *only* the
	//framebuffer binding -- it does not touch the viewport (SDK b1afaf9,
	//FFGLScopedFBOBinding.cpp). Every pass's ResizeViewPort() leaks into the
	//next, and the output pass, which draws to the host's own framebuffer and
	//so has no buffer of its own to size itself from, would inherit the last
	//expand's viewport -- which here is the full picture, by luck, and luck is
	//not a viewport.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const double now = advanceClock();
	updateAudio( now );

	const int levels   = pyramid::ActiveLevels( width, height );
	const bool luma    = std::lround( params[ PT_CARRIER ] ) == 1;

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens here, before anything binds a texture. That is
	// not tidiness: ffglex::FFGLFBO::Initialise sizes its new colour texture
	// under a ScopedTextureBinding, and every ffglex Scoped* binding *clears*
	// to 0 on scope exit rather than restoring what was there. Allocating a
	// buffer therefore unbinds the input texture from the active unit, and the
	// symptom is the dangerous part -- correct on every frame except the one
	// that allocates.
	//---------------------------------------------------------------------
	int levelWidth[ pyramid::kMaxLevels + 1 ];
	int levelHeight[ pyramid::kMaxLevels + 1 ];
	levelWidth[ 0 ]  = width;
	levelHeight[ 0 ] = height;
	for( int k = 1; k <= levels; ++k )
	{
		levelWidth[ k ]  = pyramid::ReducedSize( levelWidth[ k - 1 ] );
		levelHeight[ k ] = pyramid::ReducedSize( levelHeight[ k - 1 ] );
	}

	bool allocated = copyBuffer.Ensure( width, height, kBufferFormat, PassBuffer::Sampling::Nearest );
	for( int k = 0; k < levels && allocated; ++k )
	{
		allocated = gauss[ k ].Ensure( levelWidth[ k + 1 ], levelHeight[ k + 1 ], kBufferFormat, PassBuffer::Sampling::Nearest )
		            && temp[ k ].Ensure( levelWidth[ k + 1 ], levelHeight[ k ], kBufferFormat, PassBuffer::Sampling::Nearest );
		//R_{k+1} is stored for k+1 < L only; R_L is read straight off G_L.
		if( allocated && k + 1 < levels )
			allocated = recon[ k ].Ensure( levelWidth[ k + 1 ], levelHeight[ k + 1 ], kBufferFormat, PassBuffer::Sampling::Nearest );
	}

	if( !allocated )
	{
		diag::error( "could not allocate the pyramid buffers" );
		return FF_FAIL;
	}

	//Per-channel gains. In Luma mode only channel 0 (Y) is a band; Cb, Cr
	//and alpha ride through at 1, which reconstructs them exactly. In RGB
	//mode the three colour channels share a gain and alpha is still 1.
	auto gainVector = [ & ]( float g, float* out ) {
		out[ 0 ] = g;
		out[ 1 ] = luma ? 1.0f : g;
		out[ 2 ] = luma ? 1.0f : g;
		out[ 3 ] = 1.0f;
	};

	//---------------------------------------------------------------------
	// 1. The picture, into a float buffer of ours.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		copyShader.Set( "InputTexture", 0 );
		glUniform2i( copyShader.FindUniform( "TargetSize" ), width, height );
		copyShader.Set( "LumaMode", luma ? 1 : 0 );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. Reduce: G_0 -> G_1 -> ... -> G_L, two passes each.
	//---------------------------------------------------------------------
	for( int k = 0; k < levels; ++k )
	{
		struct Half
		{
			PassBuffer* from;
			PassBuffer* to;
			int axisX, axisY;
			int fromW, fromH, toW, toH;
		};
		const Half halves[] = {
			{ &gaussian( k ), &temp[ k ], 1, 0, levelWidth[ k ], levelHeight[ k ], levelWidth[ k + 1 ], levelHeight[ k ] },
			{ &temp[ k ], &gauss[ k ], 0, 1, levelWidth[ k + 1 ], levelHeight[ k ], levelWidth[ k + 1 ], levelHeight[ k + 1 ] },
		};

		for( const Half& half : halves )
		{
			ScopedFBOBinding fbo( half.to->GetGLID(), ScopedFBOBinding::RB_REVERT );
			half.to->ResizeViewPort();
			ScopedShaderBinding shader( reduceShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( half.from->TextureID() );

			reduceShader.Set( "SourceTexture", 0 );
			glUniform2i( reduceShader.FindUniform( "SourceSize" ), half.fromW, half.fromH );
			glUniform2i( reduceShader.FindUniform( "TargetSize" ), half.toW, half.toH );
			glUniform2i( reduceShader.FindUniform( "Axis" ), half.axisX, half.axisY );
			quad.Draw();
		}
	}

	//---------------------------------------------------------------------
	// 3. Reconstruct, top down. R_L = r G_L is never materialised: the top
	//    expandV reads G_L as its "recon" with the residual gain. Bands the
	//    picture is too small for do not exist, and their sliders do nothing
	//    -- which is the honest answer, and what the sweep is told.
	//---------------------------------------------------------------------
	for( int k = levels - 1; k >= 0; --k )
	{
		const bool top   = ( k + 1 == levels );
		const bool final = ( k == 0 );

		float levelGain[ 4 ], reconGain[ 4 ];
		gainVector( gains.band[ k ], levelGain );
		gainVector( top ? gains.residual : 1.0f, reconGain );

		//Vertical: (W_{k+1} x H_{k+1}) -> temp[ k ] (W_{k+1} x H_k).
		{
			PassBuffer& above = top ? gauss[ k ] : recon[ k ];

			ScopedFBOBinding fbo( temp[ k ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			temp[ k ].ResizeViewPort();
			ScopedShaderBinding shader( expandVShader.GetGLID() );
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding reconTexture( above.TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding levelTexture( gauss[ k ].TextureID() );

			expandVShader.Set( "ReconTexture", 0 );
			expandVShader.Set( "LevelTexture", 1 );
			expandVShader.Set( "ReconGain", reconGain[ 0 ], reconGain[ 1 ], reconGain[ 2 ], reconGain[ 3 ] );
			expandVShader.Set( "LevelGain", levelGain[ 0 ], levelGain[ 1 ], levelGain[ 2 ], levelGain[ 3 ] );
			glUniform2i( expandVShader.FindUniform( "SourceSize" ), levelWidth[ k + 1 ], levelHeight[ k + 1 ] );
			glUniform2i( expandVShader.FindUniform( "TargetSize" ), levelWidth[ k + 1 ], levelHeight[ k ] );
			glUniform2i( expandVShader.FindUniform( "Axis" ), 0, 1 );
			quad.Draw();
		}

		//Horizontal: temp[ k ] -> R_k (W_k x H_k), or the host's framebuffer.
		if( !final )
		{
			ScopedFBOBinding fbo( recon[ k - 1 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			recon[ k - 1 ].ResizeViewPort();
			ScopedShaderBinding shader( expandHShader.GetGLID() );
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding sourceTexture( temp[ k ].TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding levelTexture( gaussian( k ).TextureID() );

			expandHShader.Set( "SourceTexture", 0 );
			expandHShader.Set( "LevelTexture", 1 );
			expandHShader.Set( "LevelGain", levelGain[ 0 ], levelGain[ 1 ], levelGain[ 2 ], levelGain[ 3 ] );
			glUniform2i( expandHShader.FindUniform( "SourceSize" ), levelWidth[ k + 1 ], levelHeight[ k ] );
			glUniform2i( expandHShader.FindUniform( "TargetSize" ), levelWidth[ k ], levelHeight[ k ] );
			glUniform2i( expandHShader.FindUniform( "Axis" ), 1, 0 );
			expandHShader.Set( "Final", 0 );
			quad.Draw();
		}
		else
		{
			//Back to the host's viewport. See the note where it was captured.
			glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

			ScopedShaderBinding shader( expandHShader.GetGLID() );
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding sourceTexture( temp[ 0 ].TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding levelTexture( copyBuffer.TextureID() );

			expandHShader.Set( "SourceTexture", 0 );
			expandHShader.Set( "LevelTexture", 1 );
			expandHShader.Set( "LevelGain", levelGain[ 0 ], levelGain[ 1 ], levelGain[ 2 ], levelGain[ 3 ] );
			glUniform2i( expandHShader.FindUniform( "SourceSize" ), levelWidth[ 1 ], levelHeight[ 0 ] );
			glUniform2i( expandHShader.FindUniform( "TargetSize" ), width, height );
			glUniform2i( expandHShader.FindUniform( "Axis" ), 1, 0 );
			expandHShader.Set( "Final", 1 );
			expandHShader.Set( "LumaMode", luma ? 1 : 0 );
			expandHShader.Set( "Master", controls::MasterGain( params[ PT_MASTER ] ) );
			expandHShader.Set( "MixAmount", params[ PT_MIX ] );
			quad.Draw();
		}
	}

	if( levels == 0 )
	{
		//A picture too small for even one band -- under three texels on a side.
		//The output is r * G_0 and nothing else, through the same output pass:
		//with Axis (0,0) the expand loop reads texel p three times with weights
		//summing to a half, doubled, so the "expand" term is exactly the source
		//texel -- and feeding it the copy with a level gain of r - 1 makes the
		//whole thing r * G_0. A corner case, handled without a fifth shader.
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		float levelGain[ 4 ];
		gainVector( gains.residual - 1.0f, levelGain );
		levelGain[ 3 ] = 0.0f;//alpha: 1 from the expand term, nothing more

		ScopedShaderBinding shader( expandHShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding sourceTexture( copyBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding levelTexture( copyBuffer.TextureID() );

		expandHShader.Set( "SourceTexture", 0 );
		expandHShader.Set( "LevelTexture", 1 );
		expandHShader.Set( "LevelGain", levelGain[ 0 ], levelGain[ 1 ], levelGain[ 2 ], levelGain[ 3 ] );
		glUniform2i( expandHShader.FindUniform( "SourceSize" ), width, height );
		glUniform2i( expandHShader.FindUniform( "TargetSize" ), width, height );
		glUniform2i( expandHShader.FindUniform( "Axis" ), 0, 0 );
		expandHShader.Set( "Final", 1 );
		expandHShader.Set( "LumaMode", luma ? 1 : 0 );
		expandHShader.Set( "Master", controls::MasterGain( params[ PT_MASTER ] ) );
		expandHShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Vocoder::DeInitGL()
{
	copyShader.FreeGLResources();
	reduceShader.FreeGLResources();
	expandVShader.FreeGLResources();
	expandHShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	for( int k = 0; k < pyramid::kMaxLevels; ++k )
	{
		gauss[ k ].Destroy();
		temp[ k ].Destroy();
		recon[ k ].Destroy();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Vocoder::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before the params[] write below -- there is no value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Vocoder::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Vocoder::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Vocoder::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Vocoder::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Vocoder::SetSampleRate( unsigned int rate )
{
	if( rate != sampleRateSeen )
	{
		sampleRateSeen = rate;
		diag::info( "host sample rate " + std::to_string( rate ) );
	}
	CFFGLPlugin::SetSampleRate( rate );
}

//---------------------------------------------------------------------------
void Vocoder::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Vocoder::UpdateAudioForTest( double seconds )
{
	//Seconds, declared. Left to the calibration, a harness that calls this
	//in a tight loop delivers frame deltas of microseconds of wall time,
	//nothing ever votes, and the followers sit on a clock that does not move.
	clockScale = 1.0;
	hostTime   = seconds;
	updateAudio( advanceClock() );
}

void Vocoder::EnvelopesForTest( float out[ audio::kBands ] ) const
{
	const float* env = follower.Envelopes();
	for( int a = 0; a < audio::kBands; ++a )
		out[ a ] = env[ a ];
}

void Vocoder::BandGainsForTest( float out[ controls::kBands ], float& residual ) const
{
	for( int k = 0; k < controls::kBands; ++k )
		out[ k ] = gains.band[ k ];
	residual = gains.residual;
}
