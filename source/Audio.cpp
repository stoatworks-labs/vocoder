#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace vocoder::audio
{

const int kBandEdges[ kBands + 1 ] = { 0, 1, 2, 4, 7, 12, 20, 34, 64 };

float BandLevel( const float* bins, int count, int band )
{
	if( band < 0 || band >= kBands || bins == nullptr )
		return 0.0f;

	const int from = kBandEdges[ band ];
	const int to   = std::min( kBandEdges[ band + 1 ], count );

	float energy = 0.0f;
	for( int i = from; i < to; ++i )
	{
		const float magnitude = std::max( 0.0f, bins[ i ] );
		energy += magnitude * magnitude;
	}

	//RMS-summed, so one full-scale sine is full scale in any band; clamped, so
	//a wide band full of noise does not read as several hundred percent.
	const float rms = std::min( 1.0f, std::sqrt( energy ) );

	//sqrt because bin magnitudes bunch hard against zero.
	return std::sqrt( rms );
}

void Follower::Reset()
{
	for( float& e : env )
		e = 0.0f;
}

void Follower::Update( const float levels[ kBands ], double dt, float attackSeconds, float releaseSeconds )
{
	if( dt < 0.0 )
	{
		//First frame: there is no history to filter against, and starting from
		//zero would make every band fade in over its attack time on load.
		for( int k = 0; k < kBands; ++k )
			env[ k ] = levels[ k ];
		return;
	}

	if( dt == 0.0 )
		return;//a paused host is not a decaying one

	const float attack  = 1.0f - std::exp( static_cast< float >( -dt / std::max( 1e-5f, attackSeconds ) ) );
	const float release = 1.0f - std::exp( static_cast< float >( -dt / std::max( 1e-5f, releaseSeconds ) ) );

	for( int k = 0; k < kBands; ++k )
	{
		const float target = levels[ k ];
		const float coeff  = target >= env[ k ] ? attack : release;
		env[ k ] += ( target - env[ k ] ) * coeff;
	}
}

float Follower::Overall() const
{
	float sum = 0.0f;
	for( float e : env )
		sum += e;
	return sum / static_cast< float >( kBands );
}

int PictureBandFor( int audioBand, Mapping mapping )
{
	//Picture band 0 is the finest. Direct puts the lowest audio band on the
	//coarsest picture band, so the kick moves the big shapes.
	return mapping == Mapping::Direct ? ( kBands - 1 - audioBand ) : audioBand;
}

void PictureGains( const float envelopes[ kBands ], Mapping mapping, float driveGain, float floorGain, float out[ kBands ] )
{
	for( int a = 0; a < kBands; ++a )
	{
		const int k = PictureBandFor( a, mapping );
		out[ k ]    = floorGain + envelopes[ a ] * ( driveGain - floorGain );
	}
}

} // namespace vocoder::audio
