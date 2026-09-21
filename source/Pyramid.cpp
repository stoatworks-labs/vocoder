#include "Pyramid.h"

#include <algorithm>

namespace vocoder::pyramid
{

int ReducedSize( int n )
{
	return ( n + 1 ) / 2;
}

int ActiveLevels( int width, int height )
{
	int levels = 0;
	int w = width, h = height;
	for( int k = 0; k < kMaxLevels; ++k )
	{
		w = ReducedSize( w );
		h = ReducedSize( h );
		if( std::min( w, h ) < 2 )
			break;
		++levels;
	}
	return levels;
}

void Reduce( const std::vector< float >& in, std::vector< float >& out )
{
	const int n    = static_cast< int >( in.size() );
	const int half = ReducedSize( n );
	out.assign( static_cast< size_t >( half ), 0.0f );

	for( int x = 0; x < half; ++x )
	{
		float sum = 0.0f;
		for( int i = 0; i < 5; ++i )
		{
			const int q = std::clamp( 2 * x + i - 2, 0, n - 1 );
			sum += kKernel[ i ] * in[ static_cast< size_t >( q ) ];
		}
		out[ static_cast< size_t >( x ) ] = sum;
	}
}

void Expand( const std::vector< float >& in, int outSize, std::vector< float >& out )
{
	const int n = static_cast< int >( in.size() );
	out.assign( static_cast< size_t >( outSize ), 0.0f );

	for( int x = 0; x < outSize; ++x )
	{
		//The adjoint of Reduce: sample x reads every coarse sample x' whose
		//kernel index x - 2x' + 2 lands inside the kernel. That is three taps
		//for an even x and two for an odd one, and doubling makes both sum to
		//one -- see the header.
		const int m = x / 2;
		float sum   = 0.0f;
		for( int j = -1; j <= 1; ++j )
		{
			const int xp  = m + j;
			const int idx = x - 2 * xp + 2;
			if( idx < 0 || idx > 4 )
				continue;
			const int q = std::clamp( xp, 0, n - 1 );
			sum += kKernel[ idx ] * in[ static_cast< size_t >( q ) ];
		}
		out[ static_cast< size_t >( x ) ] = 2.0f * sum;
	}
}

std::vector< float > Reconstruct( const std::vector< float >& signal, const float gains[ kMaxLevels ],
                                  float residual, int levels )
{
	levels = std::clamp( levels, 0, kMaxLevels );

	//G_0 .. G_L.
	std::vector< std::vector< float > > gauss( static_cast< size_t >( levels ) + 1 );
	gauss[ 0 ] = signal;
	for( int k = 0; k < levels; ++k )
		Reduce( gauss[ static_cast< size_t >( k ) ], gauss[ static_cast< size_t >( k ) + 1 ] );

	//R_L = r * G_L, then down the recursion.
	std::vector< float > recon = gauss[ static_cast< size_t >( levels ) ];
	for( float& v : recon )
		v *= residual;

	std::vector< float > difference, expanded;
	for( int k = levels - 1; k >= 0; --k )
	{
		const std::vector< float >& above = gauss[ static_cast< size_t >( k ) + 1 ];
		const std::vector< float >& here  = gauss[ static_cast< size_t >( k ) ];

		difference.resize( above.size() );
		for( size_t i = 0; i < above.size(); ++i )
			difference[ i ] = recon[ i ] - gains[ k ] * above[ i ];

		Expand( difference, static_cast< int >( here.size() ), expanded );

		recon.resize( here.size() );
		for( size_t i = 0; i < here.size(); ++i )
			recon[ i ] = expanded[ i ] + gains[ k ] * here[ i ];
	}

	return recon;
}

} // namespace vocoder::pyramid
