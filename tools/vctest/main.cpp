/**
    vctest -- render Vocoder offline, and check that its pyramid is a pyramid.

    Everything here drives the REAL plugin class -- the one the bundle
    registers -- through the same InitGL / SetTime / ProcessOpenGL sequence a
    host uses, in a headless CGL context, and reads the result back as floats.
    Nothing is re-transcribed: the shaders under test are the shaders that
    ship, and the CPU reference in Pyramid.cpp is compiled into the plugin.

        vctest --out /tmp/frame.png     a picture, on the test card
        vctest --list                   every parameter and its default
        vctest --identity               all gains at 1x returns the input; the
                                        bands and the residual partition it
        vctest --band                   each band is the band-pass the maths
                                        says, against the CPU, and its peak
        vctest --audio                  a sine in audio band k moves picture
                                        band k and no other, both mappings
        vctest --envelope               the followers keep their time constants
        vctest --bench                  ms/frame at 720p, 1080p and 4K

    Four checks, four physics claims in the spec. Each one is something that
    is true or false, and each has already earned its keep -- see AGENTS.md
    for what they caught while this was being built.

    The output framebuffer the plugin draws into is RGBA32F, not the host's
    RGBA8, so a check can see a difference of 1e-6 rather than 1/255. The
    bench uses RGBA8, because that is what the host's is.
*/

#include "Audio.h"
#include "Controls.h"
#include "Pyramid.h"
#include "Vocoder.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vocoder;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Integer hashing, for the noise patch on the card. PCG output mix, exact.
//---------------------------------------------------------------------------
uint32_t hashInt( uint32_t x )
{
	x = x * 747796405u + 2891336453u;
	x = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( x >> 22u ) ^ x;
}

//---------------------------------------------------------------------------
// The test card.
//
// Built so that every one of the eight bands has something to act on: hard
// edges (a disc, a ring, a bar) are broadband; two equal-luminance colour
// fields give the Luma carrier something the RGB one does not see; a soft
// gradient is where the coarse bands and the residual live; and a hashed
// noise patch, bottom right, is white noise in every band at once, which is
// what makes a 1 px band's slider visibly do something at 640x360.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r = 0.0f, g = 0.0f, b = 0.0f;

			//A soft horizontal gradient everywhere.
			r = g = b = 0.10f + 0.30f * u;

			//Disc, left third.
			const float dx1 = ( u - 0.20f ) * ( w / h );
			const float dy1 = v - 0.5f;
			if( std::sqrt( dx1 * dx1 + dy1 * dy1 ) < 0.16f )
				r = g = b = 0.95f;

			//Ring, middle.
			const float dx2 = ( u - 0.50f ) * ( w / h );
			const float dy2 = v - 0.5f;
			const float d2  = std::sqrt( dx2 * dx2 + dy2 * dy2 );
			if( d2 < 0.18f && d2 > 0.12f )
				r = g = b = 0.90f;

			//Bar, right third.
			if( u > 0.72f && u < 0.88f && v > 0.20f && v < 0.62f )
				r = g = b = 1.0f;

			//Two fields of equal luminance, bottom left.
			if( v < 0.16f && u < 0.40f )
			{
				const bool right = u > 0.20f;
				r                = right ? 0.10f : 0.9333f;
				g                = right ? 0.2775f : 0.0f;
				b                = 0.0f;
			}

			//Noise, bottom right. Grey, mid-level, +-0.25.
			if( v > 0.70f && u > 0.66f )
			{
				const uint32_t seed = hashInt( static_cast< uint32_t >( y ) * 2654435761u ^ static_cast< uint32_t >( x ) );
				const float n       = static_cast< float >( seed & 0xFFFFu ) / 65535.0f;
				r = g = b = 0.25f + 0.5f * n;
			}

			unsigned char* px = &image[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			px[ 0 ]           = static_cast< unsigned char >( std::min( 255.0f, r * 255.0f + 0.5f ) );
			px[ 1 ]           = static_cast< unsigned char >( std::min( 255.0f, g * 255.0f + 0.5f ) );
			px[ 2 ]           = static_cast< unsigned char >( std::min( 255.0f, b * 255.0f + 0.5f ) );
			px[ 3 ]           = 255;
		}
	}

	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/**
    One plugin, one input, one output, at one size.

    The input is an RGBA8 texture, which is what a host hands over. The
    output is an RGBA32F framebuffer by default, so a check can read back
    what the plugin computed rather than what fits in a byte; the bench asks
    for RGBA8 because that is the host's, and what the final pass costs
    writing into it is part of the cost.
*/
struct Session
{
	Vocoder plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	bool floatOutput = true;

	bool begin( int w, int h, const std::vector< unsigned char >& rgba, bool floatOut = true )
	{
		width       = w;
		height      = h;
		floatOutput = floatOut;

		//The card is built top row first, GL wants bottom row first. Flipped
		//here, once, so that a PNG of the output is the card the right way up
		//and a float readback (bottom row first) lines up with the card read
		//backwards -- which is what bytesToFloatBottomUp does. The first
		//version of --identity compared against the unflipped card and
		//reported 0.83 for a plugin that was, in fact, exact.
		const std::vector< unsigned char > flipped = flipRows( rgba, w, h );

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL FAILED -- see the diagnostics log for which shader\n" );
			return false;
		}

		glGenTextures( 1, &sourceTexture );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

		glGenTextures( 1, &outputTexture );
		glBindTexture( GL_TEXTURE_2D, outputTexture );
		if( floatOutput )
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr );
		else
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			std::fprintf( stderr, "output framebuffer incomplete\n" );
			return false;
		}
		return true;
	}

	/// One frame at `seconds` on a declared-seconds clock.
	bool frame( double seconds )
	{
		FFGLTextureStruct inputStruct = {};
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

		ProcessOpenGLStruct process = {};
		process.numInputTextures    = 1;
		process.inputTextures       = inputs;
		process.HostFBO             = outputFBO;

		plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
		plugin.SetTime( seconds );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	/// The output, bottom row first, as floats.
	std::vector< float > readFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// The output as bytes, top row first, for a PNG.
	std::vector< unsigned char > readBytesTopDown()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}
};

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
int indexOf( Vocoder& plugin, const char* name )
{
	for( unsigned int i = 0; i < Vocoder::PT_COUNT; ++i )
	{
		const char* declared = plugin.GetParamName( i );
		if( declared != nullptr && std::strcmp( declared, name ) == 0 )
			return static_cast< int >( i );
	}
	return -1;
}

bool applySetting( Vocoder& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	const int index = indexOf( plugin, name.c_str() );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "' (try --list)";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

/// Set a band slider by its 0-based band index (0 = 1 px).
void setBand( Vocoder& plugin, int band, float value )
{
	plugin.SetFloatParameter( Vocoder::PT_BAND_1 + static_cast< unsigned int >( band ), value );
}

//---------------------------------------------------------------------------
// Spectra, written the way the host writes them: one value per element.
//---------------------------------------------------------------------------
void writeSpectrum( Vocoder& plugin, const float bins[ audio::kBins ] )
{
	for( int i = 0; i < audio::kBins; ++i )
		plugin.SetParamElementValue( Vocoder::PT_AUDIO, static_cast< unsigned int >( i ), bins[ i ] );
}

/// A sine at the centre of one audio band, and silence everywhere else.
void sineInBand( Vocoder& plugin, int band, float level )
{
	float bins[ audio::kBins ] = {};
	const int centre           = ( audio::kBandEdges[ band ] + audio::kBandEdges[ band + 1 ] - 1 ) / 2;
	bins[ centre ]             = level;
	writeSpectrum( plugin, bins );
}

/**
    The synthetic programme for `--feed`: something that looks like music
    to the followers. Without it the Audio group is correctly dead offline --
    the host is the only thing that ever supplies bins.

    A kick in the bottom two bins that retriggers every half second and
    decays hard between beats (so Attack has a rising edge to act on at frame
    30 and Release a falling one everywhere else), a steady mid, and a little
    steady hiss up top with a ripple so no two bands read alike.
*/
void feedSpectrum( Vocoder& plugin, float level, double seconds )
{
	const float kick = static_cast< float >( std::exp( -6.0 * std::fmod( seconds, 0.5 ) ) );

	float bins[ audio::kBins ];
	for( int i = 0; i < audio::kBins; ++i )
	{
		const float t = static_cast< float >( i ) / 63.0f;
		float value   = 0.05f * std::exp( -3.0f * t );
		if( i < 2 )
			value += kick;
		if( i >= 4 && i < 20 )
			value += 0.30f;
		if( i >= 20 )
			value += 0.12f + 0.06f * std::sin( 9.0f * t );
		bins[ i ] = std::clamp( value * level, 0.0f, 1.0f );
	}
	writeSpectrum( plugin, bins );
}

//---------------------------------------------------------------------------
// --identity
//---------------------------------------------------------------------------
float maxAbsDifference( const std::vector< float >& a, const std::vector< float >& b, int channels = 3 )
{
	float worst = 0.0f;
	for( size_t i = 0; i + 3 < a.size(); i += 4 )
		for( int c = 0; c < channels; ++c )
			worst = std::max( worst, std::fabs( a[ i + c ] - b[ i + c ] ) );
	return worst;
}

std::vector< float > bytesToFloatBottomUp( const std::vector< unsigned char >& rgba, int width, int height )
{
	//The card is built top row first and the readback comes bottom row first.
	std::vector< float > out( rgba.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
	{
		const unsigned char* from = rgba.data() + static_cast< size_t >( height - 1 - y ) * stride;
		float* to                 = out.data() + static_cast< size_t >( y ) * stride;
		for( size_t i = 0; i < stride; ++i )
			to[ i ] = static_cast< float >( from[ i ] ) / 255.0f;
	}
	return out;
}

int runIdentity()
{
	// One ULP of a float32 just below 1.0 is 2^-24; allow 2^-23 so a single
	// rounding difference on either side of the cancellation passes and a real
	// reconstruction error, which is orders of magnitude larger, does not.
	constexpr float kIdentityUlp = 1.1920929e-07f;

	struct Size
	{
		const char* name;
		int w, h;
		float tolerance;
	};
	const Size sizes[] = {
		{ "1280x720", 1280, 720, 1e-3f },
		{ "1920x1080", 1920, 1080, 1e-3f },
		{ "3840x2160", 3840, 2160, 2e-3f },
	};

	int failures = 0;
	std::printf( "%-10s %-6s %-14s %-14s %-14s\n", "size", "bands", "identity", "luma identity", "partition" );

	for( const Size& size : sizes )
	{
		const std::vector< unsigned char > card = buildCard( size.w, size.h );

		//A low-contrast copy for the partition, so no cut and no boost can
		//clip in the output pass -- the plugin clamps to the host's range, as
		//it must, and a partition that clips is not a partition.
		std::vector< unsigned char > soft = card;
		for( size_t i = 0; i < soft.size(); ++i )
			if( i % 4 != 3 )
				soft[ i ] = static_cast< unsigned char >( 102 + ( static_cast< int >( soft[ i ] ) * 51 + 127 ) / 255 );

		const std::vector< float > reference     = bytesToFloatBottomUp( card, size.w, size.h );
		const std::vector< float > softReference = bytesToFloatBottomUp( soft, size.w, size.h );
		const int levels                          = pyramid::ActiveLevels( size.w, size.h );

		//(a) Every gain at 1x: the defaults. Expected to be EXACT.
		float identity = -1.0f;
		{
			Session s;
			if( !s.begin( size.w, size.h, card ) || !s.frame( 0.0 ) )
				return 1;
			identity = maxAbsDifference( s.readFloat(), reference, 4 );
			s.end();
		}

		//(b) The same through the Luma carrier: a Y/Cb/Cr round trip on top.
		float lumaIdentity = -1.0f;
		{
			Session s;
			s.plugin.SetFloatParameter( Vocoder::PT_CARRIER, 1.0f );
			if( !s.begin( size.w, size.h, card ) || !s.frame( 0.0 ) )
				return 1;
			lumaIdentity = maxAbsDifference( s.readFloat(), reference, 4 );
			s.end();
		}

		//(c) The partition. Cut one band at a time and sum the L pictures.
		//Each cut removes one term of the decomposition, so
		//
		//    sum_k ( in - L_k ) = L*in - sum_k L_k = (L-1)*in + G_L
		//
		//and G_L -- the picture with every band cut and the residual left
		//alone -- is one more render. This is the check that the bands are a
		//decomposition of the picture rather than merely something that
		//vanishes when every gain is one: it goes through every expand at
		//every level with a NON-ZERO difference, which (a) never does.
		//
		//The residual is deliberately not one of the cuts. It is the picture's
		//DC, so cutting it puts half the frame below zero, and the output pass
		//clamps to the host's range -- as it must. The first version of this
		//check summed it in anyway and reported 0.06 against a plugin that
		//partitions exactly; the clamp was doing precisely its job.
		float partition = -1.0f;
		{
			std::vector< float > sum;
			for( int cut = 0; cut < levels; ++cut )
			{
				Session s;
				setBand( s.plugin, cut, 0.0f );
				if( !s.begin( size.w, size.h, soft ) || !s.frame( 0.0 ) )
					return 1;
				const std::vector< float > out = s.readFloat();
				s.end();

				if( sum.empty() )
					sum.assign( out.size(), 0.0f );
				for( size_t i = 0; i < out.size(); ++i )
					sum[ i ] += out[ i ];
			}

			//G_L, expanded back up: every band cut, residual untouched.
			std::vector< float > coarse;
			{
				Session s;
				for( int k = 0; k < levels; ++k )
					setBand( s.plugin, k, 0.0f );
				if( !s.begin( size.w, size.h, soft ) || !s.frame( 0.0 ) )
					return 1;
				coarse = s.readFloat();
				s.end();
			}

			std::vector< float > expected( softReference.size() );
			for( size_t i = 0; i < expected.size(); ++i )
				expected[ i ] = softReference[ i ] * static_cast< float >( levels - 1 ) + coarse[ i ];
			partition = maxAbsDifference( sum, expected, 3 );
		}

		// Exactness here is structural: at unity gain the reconstruction's
		// expanded term is G - G, so the difference cancels bit for bit -- but
		// only while BOTH paths compute G the same way. On this Mac they do and
		// the measurement is a literal 0. On GitHub's macOS runner, which has no
		// accelerated GL context, it comes back 5.96e-08 -- one ULP of a float
		// near 1.0, the signature of the two paths rounding differently (an FMA
		// contracted on one side and not the other is enough to do it). So the
		// check allows one ULP, and the run still PRINTS the value, which is
		// what says whether this machine cancelled exactly or only to the last
		// bit. Tightening this back to `== 0.0f` makes the suite a statement
		// about one GPU rather than about the algorithm.
		const bool ok = identity <= kIdentityUlp && lumaIdentity <= size.tolerance && partition <= size.tolerance;
		if( !ok )
			++failures;

		std::printf( "%-10s %-6d %-14.3g %-14.3g %-14.3g %s\n", size.name, levels, identity, lumaIdentity,
		             partition, ok ? "ok" : "FAILED" );
	}

	std::printf( "\nidentity within one float ULP (1.19e-07), and 0 where the two paths round\n"
	             "identically; luma identity and partition within 1e-3 (2e-3 at 4K)\n" );
	std::printf( "%s\n", failures == 0 ? "identity: all ok" : "identity: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --band
//---------------------------------------------------------------------------

/// In-place radix-2 FFT. N must be a power of two.
void fft( std::vector< std::complex< double > >& a )
{
	const size_t n = a.size();
	for( size_t i = 1, j = 0; i < n; ++i )
	{
		size_t bit = n >> 1;
		for( ; j & bit; bit >>= 1 )
			j ^= bit;
		j ^= bit;
		if( i < j )
			std::swap( a[ i ], a[ j ] );
	}
	for( size_t len = 2; len <= n; len <<= 1 )
	{
		const double angle = -2.0 * M_PI / static_cast< double >( len );
		const std::complex< double > wlen( std::cos( angle ), std::sin( angle ) );
		for( size_t i = 0; i < n; i += len )
		{
			std::complex< double > w( 1.0, 0.0 );
			for( size_t j = 0; j < len / 2; ++j )
			{
				const std::complex< double > u = a[ i + j ];
				const std::complex< double > v = a[ i + j + len / 2 ] * w;
				a[ i + j ]                     = u + v;
				a[ i + j + len / 2 ]           = u - v;
				w *= wlen;
			}
		}
	}
}

/// The spatial period, in pixels, at which a signal's power spectrum peaks.
/// Parabolic interpolation on log power around the peak bin.
double peakPeriod( const std::vector< float >& signal )
{
	const size_t n = signal.size();
	std::vector< std::complex< double > > a( n );
	for( size_t i = 0; i < n; ++i )
		a[ i ] = std::complex< double >( signal[ i ], 0.0 );
	fft( a );

	std::vector< double > power( n / 2 + 1 );
	for( size_t f = 0; f <= n / 2; ++f )
		power[ f ] = std::norm( a[ f ] );

	size_t peak = 1;
	for( size_t f = 2; f <= n / 2; ++f )
		if( power[ f ] > power[ peak ] )
			peak = f;

	double refined = static_cast< double >( peak );
	if( peak > 1 && peak < n / 2 )
	{
		const double l = std::log( power[ peak - 1 ] + 1e-30 );
		const double c = std::log( power[ peak ] + 1e-30 );
		const double r = std::log( power[ peak + 1 ] + 1e-30 );
		const double d = l - 2.0 * c + r;
		if( d < 0.0 )
			refined += 0.5 * ( l - r ) / d;
	}
	return static_cast< double >( n ) / refined;
}

int runBand()
{
	//Wide enough for the 128 px band to show a few cycles of ringing, tall
	//enough for all eight levels (1024 halves to 4 eight times).
	constexpr int kW = 2048, kH = 1024;
	constexpr int kLine = kW / 2;

	//A vertical line, on a grey field. Grey so that the cut of a band -- which
	//is signed -- never reaches the output pass's clamp: the field is 0.5, the
	//line is 0.75, and no band of a 0.25 step can move the picture by more
	//than 0.25.
	std::vector< unsigned char > picture( static_cast< size_t >( kW ) * kH * 4, 128 );
	for( int y = 0; y < kH; ++y )
	{
		unsigned char* px = &picture[ ( static_cast< size_t >( y ) * kW + kLine ) * 4 ];
		px[ 0 ] = px[ 1 ] = px[ 2 ] = 191;
		for( int x = 0; x < kW; ++x )
			picture[ ( static_cast< size_t >( y ) * kW + x ) * 4 + 3 ] = 255;
	}

	//The same row, as the CPU sees it.
	std::vector< float > signal( kW, 128.0f / 255.0f );
	signal[ kLine ] = 191.0f / 255.0f;

	const int levels = pyramid::ActiveLevels( kW, kH );
	if( levels != pyramid::kMaxLevels )
	{
		std::printf( "expected %d levels at %dx%d, got %d\n", pyramid::kMaxLevels, kW, kH, levels );
		return 1;
	}

	int failures = 0;
	double previousPeriod = 0.0;

	std::printf( "%-5s %-9s %-12s %-14s %-11s %-9s %s\n", "band", "pitch px", "GPU vs CPU", "peak period", "period/pitch", "step", "" );

	for( int band = 0; band < levels; ++band )
	{
		//Cut this band alone: out = picture - L_band. Same gains on the CPU.
		float gains[ pyramid::kMaxLevels ];
		for( float& g : gains )
			g = 1.0f;
		gains[ band ] = 0.0f;

		const std::vector< float > cpu = pyramid::Reconstruct( signal, gains, 1.0f, levels );

		Session s;
		setBand( s.plugin, band, 0.0f );
		if( !s.begin( kW, kH, picture ) || !s.frame( 0.0 ) )
			return 1;
		const std::vector< float > out = s.readFloat();
		s.end();

		//The row through the middle, red channel.
		std::vector< float > gpu( kW );
		const size_t rowStart = static_cast< size_t >( kH / 2 ) * kW * 4;
		float worst           = 0.0f;
		for( int x = 0; x < kW; ++x )
		{
			gpu[ x ] = out[ rowStart + static_cast< size_t >( x ) * 4 ];
			worst    = std::max( worst, std::fabs( gpu[ x ] - cpu[ static_cast< size_t >( x ) ] ) );
		}

		//The band itself is what the cut removed: (signal - out) / step.
		std::vector< float > response( kW );
		for( int x = 0; x < kW; ++x )
			response[ x ] = ( signal[ x ] - gpu[ x ] ) / ( 63.0f / 255.0f );

		const double period = peakPeriod( response );
		const double pitch  = std::ldexp( 1.0, band );//the level's sampling pitch: 1, 2, 4 ... 128 px
		const double ratio  = previousPeriod > 0.0 ? period / previousPeriod : 0.0;
		previousPeriod      = period;

		//Three claims.
		//
		//The shipped shader computes what Pyramid.cpp computes, to float
		//precision.
		//
		//The band is an octave band-pass in the right place. The generating
		//kernel's response is cos^4( w/2 ), so one reduce-and-expand is
		//cos^8( w/2 ) and the finest band, 1 - cos^8( w/2 ), peaks at Nyquist:
		//a period of exactly 2 px. Every band above it is that same shape
		//scaled by its level's pitch and filtered by the expands beneath it,
		//and its power peaks at a period of about 5.4 x pitch -- 2.7 times the
		//pitch's own Nyquist. That figure is the physics of a Burt-Adelson
		//band, measured here rather than assumed, and it is the reason the
		//README says the "4 px" band rings with a period of about 21 px.
		//
		//And the peaks double from band to band.
		const bool agree = worst <= 1e-5f;
		const bool tuned = band == 0 ? std::fabs( period - 2.0 ) <= 0.05
		                             : ( period / pitch >= 4.8 && period / pitch <= 6.0 );
		const bool steps = band < 2 || ( ratio >= 1.85 && ratio <= 2.2 );
		const bool ok    = agree && tuned && steps;
		if( !ok )
			++failures;

		std::printf( "%-5d %-9.0f %-12.2g %-14.2f %-11.2f %-9.2f %s%s%s\n", band + 1, pitch, worst, period,
		             period / pitch, ratio, ok ? "ok" : "FAILED", agree ? "" : " (GPU disagrees with CPU)",
		             tuned ? "" : " (peak off)" );
	}

	std::printf( "\npitch is the level's sampling interval, which is what the band's name says. The band's\n"
	             "power peaks at a period of 2 px for band 1 and about 5.4 x pitch above that: an octave\n"
	             "band of a Burt-Adelson pyramid is centred well below its level's Nyquist.\n" );
	std::printf( "%s\n", failures == 0 ? "band: all ok" : "band: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --audio
//---------------------------------------------------------------------------
int runAudio()
{
	int failures = 0;

	//No GL: the gains the reconstruction would receive.
	std::printf( "gains, no GL. Drive 12 dB, Floor 1: the driven band reads %.3f, every other reads 1.\n",
	             controls::DbToGain( controls::DriveDb( 0.5f ) ) );
	std::printf( "%-8s %-6s %-8s %-8s %s\n", "mapping", "audio", "picture", "gain", "" );

	for( int m = 0; m < 2; ++m )
	{
		const auto mapping = static_cast< audio::Mapping >( m );
		for( int a = 0; a < audio::kBands; ++a )
		{
			Vocoder plugin;
			plugin.SetFloatParameter( Vocoder::PT_MAPPING, static_cast< float >( m ) );
			plugin.SetFloatParameter( Vocoder::PT_ATTACK, 0.0f );//1 ms: settles in a frame
			sineInBand( plugin, a, 1.0f );
			for( int frame = 0; frame < 4; ++frame )
				plugin.UpdateAudioForTest( frame / 60.0 );

			float gains[ controls::kBands ];
			float residual = 0.0f;
			plugin.BandGainsForTest( gains, residual );

			const int expected   = audio::PictureBandFor( a, mapping );
			const float drive    = controls::DbToGain( controls::DriveDb( 0.5f ) );
			bool ok              = std::fabs( gains[ expected ] - drive ) <= 1e-3f && std::fabs( residual - 1.0f ) <= 1e-6f;
			for( int k = 0; k < controls::kBands; ++k )
				if( k != expected && std::fabs( gains[ k ] - 1.0f ) > 1e-6f )
					ok = false;

			if( !ok )
				++failures;
			std::printf( "%-8s %-6d %-8d %-8.3f %s\n", m == 0 ? "Direct" : "Reverse", a + 1, expected + 1,
			             gains[ expected ], ok ? "ok" : "FAILED" );
		}
	}

	//With GL: the picture the audio makes is the picture the slider makes.
	//Floor 0, so every undriven band is silent, and a Drive of 6 dB, so the
	//driven band lands at a gain the slider can be set to exactly: the same
	//frame rendered with Band k at that gain and no audio must match to the
	//float. This is the check that the gain reaches the right level of the
	//pyramid, not just the right slot in an array.
	const float drivePosition = 0.25f;//6 dB
	const float driveGain     = controls::DbToGain( controls::DriveDb( drivePosition ) );
	std::printf( "\nthrough the picture, 1280x720. Floor 0, Drive %.2f dB: audio in band k against the slider at %.3fx.\n",
	             controls::DriveDb( drivePosition ), driveGain );
	std::printf( "%-8s %-6s %-8s %-12s %s\n", "mapping", "audio", "picture", "max diff", "" );

	const std::vector< unsigned char > card = buildCard( 1280, 720 );

	struct Case
	{
		int mapping, audioBand;
	};
	const Case cases[] = { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 0, 5 }, { 0, 6 }, { 0, 7 }, { 1, 0 }, { 1, 7 } };

	for( const Case& c : cases )
	{
		const int expected = audio::PictureBandFor( c.audioBand, static_cast< audio::Mapping >( c.mapping ) );

		std::vector< float > byAudio, bySlider;
		{
			Session s;
			s.plugin.SetFloatParameter( Vocoder::PT_MAPPING, static_cast< float >( c.mapping ) );
			s.plugin.SetFloatParameter( Vocoder::PT_FLOOR, 0.0f );
			s.plugin.SetFloatParameter( Vocoder::PT_DRIVE, drivePosition );
			s.plugin.SetFloatParameter( Vocoder::PT_ATTACK, 0.0f );
			sineInBand( s.plugin, c.audioBand, 1.0f );
			if( !s.begin( 1280, 720, card ) )
				return 1;
			for( int frame = 0; frame < 4; ++frame )
				if( !s.frame( frame / 60.0 ) )
					return 1;
			byAudio = s.readFloat();
			s.end();
		}
		{
			Session s;
			for( int k = 0; k < controls::kBands; ++k )
				setBand( s.plugin, k, k == expected ? driveGain / 4.0f : 0.0f );
			if( !s.begin( 1280, 720, card ) || !s.frame( 0.0 ) )
				return 1;
			bySlider = s.readFloat();
			s.end();
		}

		const float worst = maxAbsDifference( byAudio, bySlider, 4 );
		const bool ok     = worst <= 1e-4f;
		if( !ok )
			++failures;
		std::printf( "%-8s %-6d %-8d %-12.2g %s\n", c.mapping == 0 ? "Direct" : "Reverse", c.audioBand + 1,
		             expected + 1, worst, ok ? "ok" : "FAILED" );
	}

	std::printf( "\n%s\n", failures == 0 ? "audio: all ok" : "audio: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --envelope
//---------------------------------------------------------------------------

/// Time at which a sampled exponential crosses a level, interpolating in
/// the log domain between the two frames either side -- which is exact for
/// an exponential, so the frame rate does not limit the measurement.
double crossing( const std::vector< double >& times, const std::vector< double >& values, double level, bool rising )
{
	for( size_t i = 1; i < values.size(); ++i )
	{
		const bool crossed = rising ? ( values[ i - 1 ] < level && values[ i ] >= level )
		                            : ( values[ i - 1 ] > level && values[ i ] <= level );
		if( !crossed )
			continue;

		//Distance from the asymptote, which is 1 on the way up and 0 on the
		//way down; log-linear between the samples.
		const double a = rising ? 1.0 - values[ i - 1 ] : values[ i - 1 ];
		const double b = rising ? 1.0 - values[ i ] : values[ i ];
		const double c = rising ? 1.0 - level : level;
		if( a <= 0.0 || b <= 0.0 || c <= 0.0 )
			return times[ i ];
		const double t = ( std::log( a ) - std::log( c ) ) / ( std::log( a ) - std::log( b ) );
		return times[ i - 1 ] + t * ( times[ i ] - times[ i - 1 ] );
	}
	return -1.0;
}

int runEnvelope()
{
	constexpr double kFps  = 60.0;
	constexpr int kOnset   = 30;  //frames of silence first, so dt is established
	constexpr int kBurst   = 120; //two seconds on
	constexpr int kTotal   = 400;

	struct Setting
	{
		float attack, release;
	};
	const Setting settings[] = { { 0.5f, 0.5f }, { 0.8f, 0.3f }, { 0.2f, 0.9f } };

	int failures = 0;
	std::printf( "%-9s %-9s %-11s %-11s %-9s %-9s %s\n", "attack", "measured", "release", "measured", "err a", "err r", "" );

	for( const Setting& setting : settings )
	{
		Vocoder plugin;
		plugin.SetFloatParameter( Vocoder::PT_ATTACK, setting.attack );
		plugin.SetFloatParameter( Vocoder::PT_RELEASE, setting.release );

		std::vector< double > times, values;
		for( int frame = 0; frame < kTotal; ++frame )
		{
			const bool on = frame >= kOnset && frame < kOnset + kBurst;
			sineInBand( plugin, 0, on ? 1.0f : 0.0f );
			plugin.UpdateAudioForTest( frame / kFps );

			float env[ audio::kBands ];
			plugin.EnvelopesForTest( env );
			times.push_back( frame / kFps );
			values.push_back( env[ 0 ] );
		}

		const double tauA = controls::AttackSeconds( setting.attack );
		const double tauR = controls::ReleaseSeconds( setting.release );

		//The step's reference is the frame BEFORE the first one that filters
		//it. The spectrum for frame N is written and then consumed by frame
		//N's own update, which advances the follower by a whole frame from
		//frame N-1's time -- so in the sampled system the step arrived at
		//frame N-1. Measured from the onset frame itself, every attack reads
		//one frame fast, which at 60 fps is most of a short attack: the first
		//version of this check reported 0.0057 s for a 0.0224 s time constant
		//and 0.1833 for 0.2, both exactly one frame early.
		const double onset   = ( kOnset - 1 ) / kFps;
		const double offset  = ( kOnset + kBurst - 1 ) / kFps;
		const double riseAt  = crossing( times, values, 1.0 - std::exp( -1.0 ), true ) - onset;
		const double fallAt  = crossing( times, values, std::exp( -1.0 ), false ) - offset;
		const double errorA  = std::fabs( riseAt - tauA ) / tauA;
		const double errorR  = std::fabs( fallAt - tauR ) / tauR;

		const bool ok = errorA <= 0.10 && errorR <= 0.10;
		if( !ok )
			++failures;
		std::printf( "%-9.4f %-9.4f %-11.4f %-11.4f %-9.1f%% %-9.1f%% %s\n", tauA, riseAt, tauR, fallAt,
		             errorA * 100.0, errorR * 100.0, ok ? "ok" : "FAILED" );
	}

	std::printf( "\nseconds to 63.2%% of a step up, and to 36.8%% of a step down, at 60 fps; within 10%%.\n" );
	std::printf( "%s\n", failures == 0 ? "envelope: all ok" : "envelope: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames )
{
	const std::vector< unsigned char > card = buildCard( width, height );
	Session s;
	//A little audio, so the cost includes the sidechain arithmetic.
	if( !s.begin( width, height, card, false ) )
		return -1.0;

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
	{
		feedSpectrum( s.plugin, 1.0f, frame / 60.0 );
		s.frame( frame / 60.0 );
	}
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
	{
		feedSpectrum( s.plugin, 1.0f, ( warmup + frame ) / 60.0 );
		s.frame( ( warmup + frame ) / 60.0 );
	}
	glFinish();
	const auto end = std::chrono::steady_clock::now();
	s.end();

	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

int runBench( int frames )
{
	struct Size
	{
		const char* name;
		int w, h;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides, RGBA8 output.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( size.w, size.h, frames );
		if( ms < 0.0 )
			return 1;
		std::printf( "%s     %7.3f       %8.0f            %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0 );
	}
	std::printf( "\nOne copy, then two reduce passes and two expand passes per level, all at\n"
	             "RGBA32F: 33 passes for eight levels, most of them tiny. The cost is\n"
	             "per frame whether or not anything moved.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"vctest -- render and check the Vocoder effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin (default /tmp/vocoder.png)\n"
		"  --card PATH       write the test card alone\n"
		"  --size WxH        size (default 1280x720); also --width N --height N\n"
		"  --frames N        frames to render before reading back (default 8)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1. Repeatable.\n"
		"  --feed L          feed a synthetic spectrum at overall level L (0..1).\n"
		"                    Without it the Audio controls are correctly dead --\n"
		"                    the host is the only thing that ever supplies bins.\n"
		"  --list            print every parameter and its default, then exit\n"
		"\n"
		"  --identity        all gains at 1x returns the input exactly; the bands partition it\n"
		"  --band            each band against the CPU pyramid, and where its power peaks\n"
		"  --audio           a sine in audio band k drives picture band k and no other\n"
		"  --envelope        the followers' attack and release, measured\n"
		"  --bench           ms/frame at 720p, 1080p and 4K\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/vocoder.png";
	std::string cardPath;
	std::vector< std::string > settings;
	int width    = 1280;
	int height   = 720;
	int frames   = 8;
	double fps   = 60.0;
	float feed   = -1.0f;
	bool wantList = false, wantIdentity = false, wantBand = false, wantAudio = false, wantEnvelope = false,
	     wantBench = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--feed" && hasNext )
			feed = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--identity" )
			wantIdentity = true;
		else if( argument == "--band" )
			wantBand = true;
		else if( argument == "--audio" )
			wantAudio = true;
		else if( argument == "--envelope" )
			wantEnvelope = true;
		else if( argument == "--bench" )
			wantBench = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !cardPath.empty() )
	{
		const std::vector< unsigned char > card = buildCard( width, height );
		if( !writePng( cardPath, width, height, card ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	//The two checks that need no GL run before a context exists, so they
	//still answer on a machine where creating one fails.
	if( wantList )
	{
		Vocoder plugin;
		std::printf( "%-3s %-16s %s\n", "id", "name", "default" );
		for( unsigned int i = 0; i < Vocoder::PT_COUNT; ++i )
		{
			const char* name = plugin.GetParamName( i );
			std::printf( "%-3u %-16s %.4f\n", i, name ? name : "?", plugin.GetFloatParameter( i ) );
		}
		return 0;
	}
	if( wantEnvelope )
		return runEnvelope();

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	if( wantIdentity )
		result = runIdentity();
	else if( wantBand )
		result = runBand();
	else if( wantAudio )
		result = runAudio();
	else if( wantBench )
		result = runBench( std::max( frames, 60 ) );
	else
	{
		//A still. Several frames, not one: the followers need a few to settle
		//when there is a feed, and the clock unit needs two to be declared.
		const std::vector< unsigned char > card = buildCard( width, height );
		Session s;
		for( const std::string& setting : settings )
		{
			std::string error;
			if( !applySetting( s.plugin, setting, error ) )
			{
				std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
				return 2;
			}
		}
		if( !s.begin( width, height, card, false ) )
			return 1;

		for( int frame = 0; frame < frames; ++frame )
		{
			const double seconds = frame / fps;
			if( feed >= 0.0f )
				feedSpectrum( s.plugin, feed, seconds );
			if( !s.frame( seconds ) )
			{
				std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
				return 1;
			}
		}

		const std::vector< unsigned char > image = s.readBytesTopDown();
		s.end();
		if( !writePng( outPath, width, height, image ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
