#pragma once

#include "Audio.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Pyramid.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/**
    Vocoder -- a channel vocoder with the picture as the carrier, for Resolume.

    A channel vocoder splits the carrier into frequency bands and sets each
    band's gain from the matching band of the modulator. Here the carrier is
    the picture, its "frequency" is spatial frequency, and the modulator is
    the audio. The frame is split into a Laplacian pyramid -- eight octave
    bands from the finest detail to the coarsest shapes, plus a residual --
    each band is multiplied by a gain, and the bands are summed back. With
    every gain at 1 the input comes back exactly, to the bit (Pyramid.h says
    why that is not a rounding coincidence).

    What falls out of that:

    - **with audio**, bass pumps the large shapes and treble sparkles the fine
      detail, or the reverse by a switch;
    - **without audio**, a graphic EQ for spatial frequency: a boost at the
      4 px band rings at 4 px, a cut removes detail at that scale only, and a
      tilt is sharpen or soften;
    - **spectrum only** (residual at zero) leaves the picture as its detail
      bands, which is a new kind of edge picture.

    `Shaders.h` has the passes, `Audio.h` the modulator, `Controls.h` what the
    sliders mean, and AGENTS.md the traps.
*/
class Vocoder : public CFFGLPlugin
{
public:
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous, and an implicit unit is what let
	/// the millisecond bug through elsewhere in the fleet.
	void SetClockScaleForTest( double scale );

	/// Run the clock and the audio analysis for one frame at `seconds`, with
	/// no GL. This is the same code ProcessOpenGL runs before it touches a
	/// buffer; `vctest --audio` and `--envelope` drive it directly and read
	/// the results back through the two hooks below.
	void UpdateAudioForTest( double seconds );

	/// The eight envelopes, lowest audio band first.
	void EnvelopesForTest( float out[ vocoder::audio::kBands ] ) const;

	/// The per-band gains the reconstruction pass would receive this frame --
	/// slider x tilt x audio, finest picture band first -- and the residual.
	void BandGainsForTest( float out[ vocoder::controls::kBands ], float& residual ) const;

	Vocoder();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Display-only text still has to accept a write.
	///
	/// `instantiateGL` sets EVERY parameter's default on a fresh instance and
	/// deletes the instance if any set returns FF_FAIL (SDK b1afaf9, FFGL.cpp
	/// ~289), and the base class's SetTextParameter is a stub that returns
	/// FF_FAIL. So a plugin that declares the About text block without
	/// overriding this cannot be instantiated by any real host at all -- while
	/// remaining perfectly happy in every harness in this repo, because they
	/// drive the plugin class directly and never go through plugMain.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;
	void SetSampleRate( unsigned int sampleRate ) override;

	/// The order the host shows them in: the EQ, then what the music does to
	/// it, then the way out.
	enum ParamID : FFUInt32
	{
		//EQ
		PT_BAND_1,
		PT_BAND_2,
		PT_BAND_3,
		PT_BAND_4,
		PT_BAND_5,
		PT_BAND_6,
		PT_BAND_7,
		PT_BAND_8,
		PT_RESIDUAL,
		PT_TILT,
		PT_MASTER,
		PT_CARRIER,

		//Audio. PT_AUDIO is an FFT buffer (FF_TYPE_BUFFER, FF_USAGE_FFT):
		//Resolume shows it as an audio-source picker and writes one spectrum
		//bin per element, low frequencies first.
		PT_AUDIO,
		PT_DRIVE,
		PT_FLOOR,
		PT_ATTACK,
		PT_RELEASE,
		PT_MAPPING,
		PT_MODE,

		//Output
		PT_MIX,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum so nothing before it ever moves.
		//See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// Normalise the host's clock to seconds and return the current time.
	double advanceClock();

	/// Read the spectrum, follow it, and compose this frame's gains.
	void updateAudio( double now );

	/// Which of the pyramid buffers a level lives in. Level 0 is the copy.
	vocoder::PassBuffer& gaussian( int level );

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader reduceShader;
	ffglex::FFGLShader expandVShader;
	ffglex::FFGLShader expandHShader;
	ffglex::FFGLScreenQuad quad;

	//---------------------------------------------------------------------
	// Buffers. G_0 is the copy; gauss[ k ] holds G_{k+1}; temp[ k ] is the
	// half-width intermediate both the reduce and the expand of level k pass
	// through (they are the same size, W_{k+1} x H_k); recon[ k ] holds
	// R_{k+1}. R_L is never stored -- it is r * G_L, and the top expand reads
	// G_L with that gain -- and R_0 is the host's framebuffer.
	//---------------------------------------------------------------------
	vocoder::PassBuffer copyBuffer;
	vocoder::PassBuffer gauss[ vocoder::pyramid::kMaxLevels ];
	vocoder::PassBuffer temp[ vocoder::pyramid::kMaxLevels ];
	vocoder::PassBuffer recon[ vocoder::pyramid::kMaxLevels ];

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live by the rest of the fleet,
	// and the SDK's own Particles sample divides by 1000), while the offline
	// harness -- and any host following the header's silence -- sends
	// seconds. This plugin animates nothing, but the envelope followers'
	// attack and release are in seconds and a thousandfold error in dt makes
	// every one of them a snap. Decide from the ratio of host time to wall
	// time over a few frames and stick.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	//---------------------------------------------------------------------
	// Audio.
	//---------------------------------------------------------------------
	vocoder::audio::Follower follower;
	double audioClock    = -1.0;
	bool audioSeen       = false;
	unsigned int sampleRateSeen = 0;

	/// This frame's composed gains, for the shaders and for the test hook.
	vocoder::controls::Gains gains = {};

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
