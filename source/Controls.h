#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the nine that stand for a gain in dB-ish units and
    the two that stand for a time in seconds. That is not a style preference.
    `CFFGLPluginManager::SetParamInfo` clamps a standard default into 0..1
    *before* returning, and `SetParamRange` can only be called afterwards
    because it finds the parameter by ID -- so a parameter declared in seconds
    cannot declare a default in seconds, and 0.2 would silently become 0.2 of
    something else. The conversions live here instead, in one file that the
    plugin and the harness both use, so there is only ever one answer to what a
    slider position means.

    The gain sliders are LINEAR in gain, 0..4x, so unity sits at 0.25. That is
    unusual and deliberate: a graphic EQ's fader is symmetric in dB, but a
    vocoder band's gain is a multiplier that spends most of its life between
    "off" and "as loud as the carrier", and a slider that puts 0x at one end,
    1x a quarter of the way up and 4x at the top gives the cut side a usable
    length and the boost side room to ring. The mapping is in the parameter
    name's neighbourhood in the README, and it is the one thing a user has to
    know about this plugin's sliders.
*/
namespace vocoder::controls
{

/// Picture bands, finest first. Band 0 is the 1 px band, band 7 the 128 px
/// band. Fixed: the pyramid has this many levels and the audio analysis has
/// this many bands so that one maps onto the other.
constexpr int kBands = 8;

/// 0..4x, linear. Unity is 0.25.
float BandGain( float value );

/// The same mapping, for the residual (the coarsest Gaussian).
float ResidualGain( float value );

/// -6..+6 dB per band, zero at 0.5. Positive tilts towards the fine bands,
/// which is a sharpen; negative is a soften. Applied about the middle of the
/// bank, so band 0 and band 7 move by 3.5 slopes in opposite directions.
float TiltDbPerBand( float value );

/// 0..2x, linear, unity at 0.5. An output level, so it is not on the 0..4x
/// scale the band gains use -- it is the last thing that touches the picture
/// and a fader that can only go 2x too loud is the kind one reaches for live.
float MasterGain( float value );

/// 0..24 dB. How far a fully driven audio band swings its picture band above
/// the Floor.
float DriveDb( float value );

/// Attack time constant, 1 ms to 500 ms, geometric.
float AttackSeconds( float value );

/// Release time constant, 20 ms to 2 s, geometric.
float ReleaseSeconds( float value );

/// dB to a linear multiplier.
float DbToGain( float db );

/// The gains the reconstruction pass multiplies each band by: the slider,
/// the tilt and the audio, composed. Audio gains multiply slider gains, so a
/// band the operator has cut stays cut whatever the music does.
///
/// `audio[]` is per picture band (already mapped from audio bands) and is all
/// ones when nothing is driving. `extraTiltDb` is the Dynamics sidechain's
/// contribution, zero in Vocoder mode.
struct Gains
{
	float band[ kBands ];
	float residual;
};
Gains Compose( const float bandSliders[ kBands ], float residualSlider, float tiltSlider,
               const float audio[ kBands ], float extraTiltDbPerBand );

} // namespace vocoder::controls
