#pragma once

/**
    The modulator: a spectrum in, one envelope per band out.

    **Where the audio comes from.** FFGL has no audio path and a plugin cannot
    open one that would still be on the host's clock. What Resolume does
    provide is a buffer parameter declared `FF_USAGE_FFT`, which the host fills
    with one spectrum bin per element once per frame -- so this is a
    *modulation* source at video rate, not a signal source, and everything here
    is built for 50 or 60 updates a second rather than for 48000. The spec's
    "small FFT or bank of biquads" is therefore not needed: the host has
    already done the FFT. What is left to do is turn 64 bins into 8 bands and
    follow them.

    **What the bins mean is an assumption.** FFGL says a buffer with
    `FF_USAGE_FFT` "expects a spectrum" and says nothing about its frequency
    axis, its scale or its units. Nothing in the fleet has measured it either:
    regauss, tinsel and macroblock all split the 64 bins at fixed indices and
    call the bottom slice the woofer. So this file does the same, explicitly:
    it assumes the bins are LINEAR in frequency from 0 to Nyquist, and it
    partitions them at the indices in `kBandEdges`, which are as close to
    geometric as 64 linear bins allow. If Resolume's bins turn out to be
    log-spaced -- which its own analyser display suggests they might be -- the
    partition is still monotone, the bands still do not overlap and every
    check in the harness still holds; only the frequencies quoted in the README
    are wrong. That is the honest state of it until a host run.

    **One number per band.** A band's level is the RMS sum of its bins --
    `sqrt( sum bin^2 )`, clamped to one -- so that a single sine at full scale
    reads as full scale whichever band it lands in and however wide that band
    is, which is what the harness's `--audio` check needs to be able to say
    anything. Then a square root, because bin magnitudes bunch against zero
    and a spectrum used raw moves the picture for the kick drum and for nothing
    else (the same trick regauss and tinsel use). Then an attack/release
    follower, one per band, whose two time constants are the operator's.
*/
namespace vocoder::audio
{

/// The spectrum Resolume delivers. The fleet's figure, and what the buffer
/// parameter is declared with.
constexpr int kBins = 64;

/// Audio bands, lowest first. The same count as the picture bands so that the
/// two map one-to-one, either way round.
constexpr int kBands = 8;

/// Bin index at which each band starts; the last entry closes the top band.
/// Assuming 64 linear bins over 0..22.05 kHz (344 Hz each) these are, in Hz:
/// 0-344, 344-689, 689-1378, 1378-2412, 2412-4134, 4134-6890, 6890-11713,
/// 11713-22050. Ratios of about 1.7 to 2 from the second band up, which is as
/// log-spaced as a linear spectrum this coarse can be: the two lowest octaves
/// of the spec's 60 Hz-15 kHz plan are inside bin 0 and cannot be separated.
extern const int kBandEdges[ kBands + 1 ];

/// How the audio bands land on the picture bands.
enum class Mapping
{
	Direct  = 0, ///< low audio drives the coarse band (bass pumps the shapes)
	Reverse = 1, ///< low audio drives the fine band (bass sparkles the detail)
};

/// What the sidechain does with its envelopes.
enum class Mode
{
	Vocoder  = 0, ///< one envelope per band, each driving its picture band
	Dynamics = 1, ///< one envelope for the lot, driving the Tilt
};

/// The level of one band from the raw bins, 0..1. `count` is how many bins
/// the host actually wrote; a short buffer reads as silence above it.
float BandLevel( const float* bins, int count, int band );

/// Eight attack/release followers.
class Follower
{
public:
	void Reset();

	/// Advance every band towards its level. `dt` is the frame interval in
	/// seconds; zero holds (a paused host is not a decaying one), and a
	/// negative value means "first frame" and snaps. The coefficients are
	/// `1 - exp( -dt / tau )`, which is the exact sampled exponential: a step
	/// reaches 63.2% of its height at exactly `tau`, whatever the frame rate,
	/// and that is what `vctest --envelope` measures.
	void Update( const float levels[ kBands ], double dt, float attackSeconds, float releaseSeconds );

	const float* Envelopes() const
	{
		return env;
	}

	/// The mean of the eight, for the Dynamics mode.
	float Overall() const;

private:
	float env[ kBands ] = {};
};

/// Turn envelopes into the per-picture-band multipliers Controls::Compose
/// wants. Each is `floor + env * ( drive - floor )`, so a silent band sits at
/// Floor (0 = dark, 1 = untouched) and a full one at the Drive gain.
void PictureGains( const float envelopes[ kBands ], Mapping mapping, float driveGain, float floorGain,
                   float out[ kBands ] );

/// The picture band an audio band drives under a mapping.
int PictureBandFor( int audioBand, Mapping mapping );

} // namespace vocoder::audio
