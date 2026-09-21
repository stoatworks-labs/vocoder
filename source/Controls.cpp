#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace vocoder::controls
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for a time constant: the difference between 10 ms
/// and 20 ms is a different feel and the difference between 1.80 s and 1.81 s
/// is not.
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}
} // namespace

float BandGain( float value )
{
	return lerp( 0.0f, 4.0f, value );
}

float ResidualGain( float value )
{
	return lerp( 0.0f, 4.0f, value );
}

float TiltDbPerBand( float value )
{
	return lerp( -6.0f, 6.0f, value );
}

float MasterGain( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float DriveDb( float value )
{
	return lerp( 0.0f, 24.0f, value );
}

float AttackSeconds( float value )
{
	return geometric( 0.001f, 0.5f, value );
}

float ReleaseSeconds( float value )
{
	return geometric( 0.020f, 2.0f, value );
}

float DbToGain( float db )
{
	return std::pow( 10.0f, db / 20.0f );
}

Gains Compose( const float bandSliders[ kBands ], float residualSlider, float tiltSlider,
               const float audio[ kBands ], float extraTiltDbPerBand )
{
	Gains out {};

	const float slope = TiltDbPerBand( tiltSlider ) + extraTiltDbPerBand;

	for( int k = 0; k < kBands; ++k )
	{
		//Band 0 is the finest. A positive slope lifts it and cuts band 7 by the
		//same amount: 3.5 slopes either side of the middle of the bank.
		const float tiltDb = slope * ( 3.5f - static_cast< float >( k ) );

		//Multiplied, never added. A band at 0x on its slider is silent whatever
		//the tilt and the music say -- which is what lets "cut the 4 px band"
		//mean exactly that while the vocoder runs on the others.
		out.band[ k ] = BandGain( bandSliders[ k ] ) * DbToGain( tiltDb ) * audio[ k ];
	}

	//The tilt and the audio are about the bands. The residual is the picture's
	//DC and its coarsest shapes, and neither touches it: turning the residual
	//down is the "spectrum only" view and that is a slider's job alone.
	out.residual = ResidualGain( residualSlider );
	return out;
}

} // namespace vocoder::controls
