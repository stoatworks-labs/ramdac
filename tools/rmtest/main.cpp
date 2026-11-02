/**
    rmtest -- render Ramdac offline, and measure what its indexed framebuffer
    and its colour table did.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context, and reads the
    host's output back as floats. The picture is measured against the
    palettes as their sources publish them (the harness's own transcription,
    not the plugin's), against OKLab computed by another route (sRGB → XYZ →
    Ottosson's M1, his published test table), and against properties that
    hold whatever the code says: a dither's count of light cells, a swap that
    is a function of the index alone, a cycle that is a rotation.

        rmtest --out /tmp/ramdac.png    the defaults on the harness's moving card
        rmtest --list                   every parameter and its default
        rmtest --pipe                   raw RGBA frames in on stdin, out on stdout
        rmtest --film N                 N frames of the card, raw RGBA on stdout
        rmtest --offline                the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option, a switch or a count STEPS.

    RMTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Adaptive.h"
#include "BlueNoise.h"
#include "Clut.h"
#include "Controls.h"
#include "Dither.h"
#include "Palettes.h"
#include "Ramdac.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ramdac;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Tolerances, each with where it comes from.
//---------------------------------------------------------------------------
/// Half float's spacing at |v|. The GPU stores a half by truncating, not
/// rounding to nearest (Apple's M-series: -0.851020 stored as -1742/2048,
/// where the nearest is -1743/2048), so what is stored is within one of
/// these, not half of one.
double halfUlp( double v )
{
	const double a = std::fabs( v );
	return a >= std::ldexp( 1.0, -14 ) ? std::ldexp( 1.0, std::ilogb( a ) - 10 ) : std::ldexp( 1.0, -24 );
}
/// The GPU's float arithmetic on a colour the harness recomputes in double:
/// at most 64 additions to a running sum under 2 in size, each rounding by at
/// most 2^-23, is 7.6e-6. Knoll's candidates are the longest such sum.
constexpr double kFloatSum = 1e-5;
/// The plugin's direct linear-sRGB -> OKLab matrices against the harness's
/// XYZ route: within this everywhere on a 16-level cube (--oklab measures it).
constexpr double kMatrixRoute = 1e-4;

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "RMTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "rmtest: RMTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
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

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch, a count or a press: cues STEP
/// between keys for these, and everything else ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//===========================================================================
// The harness's own colour science, in double, by another route than the
// plugin's: sRGB to XYZ (IEC 61966-2-1's primaries and D65, as Lindbloom
// tabulates them), then Ottosson's M1 from XYZ, the cube root, M2.
//===========================================================================
double linearOf( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}
double codeOf( double light )
{
	return light <= 0.0031308 ? light * 12.92 : 1.055 * std::pow( light, 1.0 / 2.4 ) - 0.055;
}

struct Ref
{
	double L, a, b;
};

Ref oklabOfXyz( double x, double y, double z )
{
	const double l = std::cbrt( 0.8189330101 * x + 0.3618667424 * y - 0.1288597137 * z );
	const double m = std::cbrt( 0.0329845436 * x + 0.9293118715 * y + 0.0361456387 * z );
	const double s = std::cbrt( 0.0482003018 * x + 0.2643662691 * y + 0.6338517070 * z );
	return { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
		     0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
}

Ref refOklab( double r, double g, double b )
{
	const double x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
	const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
	const double z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
	return oklabOfXyz( x, y, z );
}

double refDistance( const Ref& p, const Ref& q )
{
	const double dL = p.L - q.L, da = p.a - q.a, db = p.b - q.b;
	return dL * dL + da * da + db * db;
}

/// An 8-bit colour, as the harness keeps them.
struct C8
{
	int r, g, b;
	bool operator==( const C8& o ) const
	{
		return r == o.r && g == o.g && b == o.b;
	}
	bool operator<( const C8& o ) const
	{
		return r != o.r ? r < o.r : g != o.g ? g < o.g : b < o.b;
	}
};

C8 c8( uint32_t v )
{
	return { static_cast< int >( ( v >> 16 ) & 255 ), static_cast< int >( ( v >> 8 ) & 255 ), static_cast< int >( v & 255 ) };
}
C8 c8Of( const Rgb8& c )
{
	return { c.r, c.g, c.b };
}

Ref refOf( const C8& c )
{
	return refOklab( linearOf( c.r / 255.0 ), linearOf( c.g / 255.0 ), linearOf( c.b / 255.0 ) );
}

void refLinear( const C8& c, double out[ 3 ] )
{
	out[ 0 ] = linearOf( c.r / 255.0 );
	out[ 1 ] = linearOf( c.g / 255.0 );
	out[ 2 ] = linearOf( c.b / 255.0 );
}

/// The palettes, transcribed again from their sources (ATTRIBUTIONS.md), in
/// each machine's order. The Amiga's are its 12-bit registers ×17.
struct RefPalette
{
	PaletteId id;
	const char* name;
	std::vector< C8 > colours;
};

std::vector< C8 > hexList( std::initializer_list< uint32_t > values )
{
	std::vector< C8 > out;
	for( uint32_t v : values )
		out.push_back( c8( v ) );
	return out;
}
std::vector< C8 > registerList( std::initializer_list< uint32_t > values )
{
	std::vector< C8 > out;
	for( uint32_t v : values )
		out.push_back( { static_cast< int >( ( v >> 8 ) & 15 ) * 17, static_cast< int >( ( v >> 4 ) & 15 ) * 17, static_cast< int >( v & 15 ) * 17 } );
	return out;
}

const std::vector< RefPalette >& refPalettes()
{
	static const std::vector< RefPalette > list = {
		{ PaletteId::Teletext, "Teletext", hexList( { 0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF } ) },
		{ PaletteId::CgaMode4, "CGA Mode 4", hexList( { 0x000000, 0x55FFFF, 0xFF55FF, 0xFFFFFF } ) },
		{ PaletteId::Cga16, "CGA 16",
		  hexList( { 0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
		             0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF } ) },
		{ PaletteId::C64, "Commodore 64",
		  hexList( { 0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43, 0x352879, 0xB8C76F, 0x6F4F25, 0x433900, 0x9A6759, 0x444444,
		             0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595 } ) },
		{ PaletteId::Spectrum, "ZX Spectrum",
		  hexList( { 0x000000, 0x0000D7, 0xD70000, 0xD700D7, 0x00D700, 0x00D7D7, 0xD7D700, 0xD7D7D7, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00,
		             0x00FFFF, 0xFFFF00, 0xFFFFFF } ) },
		{ PaletteId::Cpc, "Amstrad CPC",
		  hexList( { 0x000060, 0xFFFF00, 0x00FFFF, 0xFF0000, 0xFFFFFF, 0x000000, 0x0000FF, 0xFF00FF, 0x006060, 0x606000, 0x6060FF, 0xFF6060,
		             0x00FF00, 0x60FF60 } ) },
		{ PaletteId::Nes, "NES",
		  hexList( { 0x000000, 0x545454, 0x989698, 0xECEEEC, 0x3032EC, 0x8814B0, 0x982220, 0x545A00, 0x087C00, 0x006678, 0x4C9AEC, 0xB062EC,
		             0xEC58B4, 0xD48820, 0x74C400, 0x38CC6C } ) },
		{ PaletteId::GameBoy, "Game Boy", hexList( { 0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F } ) },
		{ PaletteId::Pico8, "PICO-8",
		  hexList( { 0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8, 0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436,
		             0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA } ) },
		{ PaletteId::Workbench13, "Workbench 1.3", registerList( { 0x05A, 0xFFF, 0x002, 0xF80 } ) },
		{ PaletteId::Workbench20, "Workbench 2.0", registerList( { 0xAAA, 0x000, 0xFFF, 0x68B } ) },
	};
	return list;
}

const RefPalette& refPalette( PaletteId id )
{
	for( const RefPalette& p : refPalettes() )
		if( p.id == id )
			return p;
	return refPalettes().front();
}

/// Darkest first by the harness's own L; equal L keeps the machine's order.
std::vector< C8 > lightnessOrdered( std::vector< C8 > colours )
{
	std::stable_sort( colours.begin(), colours.end(), []( const C8& x, const C8& y ) { return refOf( x ).L < refOf( y ).L; } );
	return colours;
}

/// The nearest entry by the harness's route, and how clear the decision was:
/// the runner-up's distance less the winner's, in OKLab. A tie goes to the
/// lower index, as the plugin's does.
int refNearest( const std::vector< Ref >& palette, const Ref& p, double& margin )
{
	int best     = 0;
	double first = 1e300, second = 1e300;
	for( size_t i = 0; i < palette.size(); ++i )
	{
		const double d = refDistance( palette[ i ], p );
		if( d < first )
		{
			second = first;
			first  = d;
			best   = static_cast< int >( i );
		}
		else if( d < second )
			second = d;
	}
	margin = palette.size() < 2 ? 1e300 : std::sqrt( second ) - std::sqrt( first );
	return best;
}

/// Is a nearest-entry decision at linear colour `a` too close to call? The
/// GPU's `a` can be off the harness's by `delta` a channel; the harness's own
/// OKLab says how far that moves the colour there (steep near black, where
/// the cube root is), the matrices add kMatrixRoute, and a gap between two
/// distances moves by at most twice the colour's move.
bool nearTie( const double a[ 3 ], double margin, double delta )
{
	const Ref at = refOklab( a[ 0 ], a[ 1 ], a[ 2 ] );
	double move  = 0.0;
	for( int c = 0; c < 3; ++c )
	{
		double worst = 0.0;
		for( double sign : { -1.0, 1.0 } )
		{
			double b[ 3 ] = { a[ 0 ], a[ 1 ], a[ 2 ] };
			b[ c ]        = std::clamp( b[ c ] + sign * delta, 0.0, 1.0 );
			worst         = std::max( worst, std::sqrt( refDistance( refOklab( b[ 0 ], b[ 1 ], b[ 2 ] ), at ) ) );
		}
		move += worst;
	}
	return margin < 2.0 * ( move + kMatrixRoute );
}

//===========================================================================
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//===========================================================================
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b = 0 )
{
	return pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// Eight bits, as a host's clip is.
float eightBit( double v )
{
	return static_cast< float >( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) / 255.0 );
}

double smooth01( double e0, double e1, double x )
{
	const double t = std::clamp( ( x - e0 ) / ( e1 - e0 ), 0.0, 1.0 );
	return t * t * ( 3.0 - 2.0 * t );
}

/// The look card at t seconds: a sky from indigo to amber with a sun going
/// down through it, two ranges of hills, a lake that throws the sky back with
/// ripples running across it, a band of saturated hues, and a grey wedge --
/// every kind of thing a palette and a dither have to say something about:
/// long smooth gradients, flat fields, saturated edges, and motion.
Floats buildCard( int width, int height, double t = 0.0 )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double aspect = static_cast< double >( width ) / height;
	const double sunX = 0.25 + 0.5 * std::fmod( 0.04 * t, 1.0 ), sunY = 0.78 - 0.18 * std::fmod( 0.04 * t, 1.0 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;//v = 0 at the bottom
			double r = 0, g = 0, b = 0;
			auto sky = [ & ]( double vv, double uu, double& sr, double& sg, double& sb ) {
				const double h = std::clamp( ( vv - 0.42 ) / 0.58, 0.0, 1.0 );
				sr             = 0.98 - 0.86 * h;
				sg             = 0.62 - 0.52 * h + 0.06 * std::sin( 6.0 * uu );
				sb             = 0.30 + 0.30 * h;
				const double dx = ( uu - sunX ) * aspect, dy = vv - sunY;
				const double d  = std::sqrt( dx * dx + dy * dy );
				const double glow = std::exp( -d * 7.0 );
				sr += 0.5 * glow;
				sg += 0.35 * glow;
				sb += 0.1 * glow;
				if( d < 0.07 )
				{
					sr = 1.0;
					sg = 0.93;
					sb = 0.65;
				}
			};
			if( v > 0.42 )
			{
				sky( v, u, r, g, b );
				const double ridge = 0.50 + 0.05 * std::sin( 9.0 * u + 1.0 ) + 0.03 * std::sin( 23.0 * u );
				const double hill  = 0.45 + 0.07 * std::sin( 5.0 * u + 2.0 + 0.0 * t ) + 0.02 * std::sin( 31.0 * u );
				if( v < ridge )
				{
					r = 0.30 * r + 0.10;
					g = 0.30 * g + 0.12;
					b = 0.30 * b + 0.22;
				}
				if( v < hill )
				{
					r = 0.06 + 0.10 * ( v - 0.42 ) / 0.1;
					g = 0.16 + 0.10 * ( v - 0.42 ) / 0.1;
					b = 0.08;
				}
			}
			else if( v > 0.16 )
			{
				//The lake: the sky upside down, broken by ripples that run.
				const double depth  = ( 0.42 - v ) / 0.26;
				const double ripple = 0.012 * std::sin( 80.0 * v * v + 3.0 * t ) * ( 0.3 + depth );
				sky( 0.42 + ( 0.42 - v ) * 1.6, u + ripple, r, g, b );
				r *= 0.75 - 0.3 * depth;
				g *= 0.80 - 0.3 * depth;
				b *= 0.90 - 0.2 * depth;
			}
			else if( v > 0.06 )
			{
				//Saturated hues, rolling.
				const double hue = std::fmod( u + 0.05 * t, 1.0 ) * 6.0;
				const double k   = smooth01( 0.06, 0.11, v );
				const double rr  = std::clamp( std::fabs( hue - 3.0 ) - 1.0, 0.0, 1.0 );
				const double gg  = std::clamp( 2.0 - std::fabs( hue - 2.0 ), 0.0, 1.0 );
				const double bb  = std::clamp( 2.0 - std::fabs( hue - 4.0 ), 0.0, 1.0 );
				r                = rr * k;
				g                = gg * k;
				b                = bb * k;
			}
			else
				r = g = b = u;//the grey wedge, in code values
			float* p = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ]   = eightBit( r );
			p[ 1 ]   = eightBit( g );
			p[ 2 ]   = eightBit( b );
			p[ 3 ]   = 1.0f;
		}
	return card;
}

/// Flat colours in vertical bands `band` pixels wide, each band's colour from
/// the hash: what a dither has to render as a field.
Floats bandCard( int width, int height, int band, uint32_t seed )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const uint32_t k = static_cast< uint32_t >( x / band );
			float* p         = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			for( int c = 0; c < 3; ++c )
				p[ c ] = eightBit( hash01( k * 3u + static_cast< uint32_t >( c ), seed ) );
			p[ 3 ] = 1.0f;
		}
	return card;
}

/// Greys in vertical bands, each its own code value: band k is code (k * 7 + 3)
/// mod 256, so neighbouring bands are not neighbouring greys.
Floats greyBands( int width, int height, int band )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int code = ( ( x / band ) * 37 + 3 ) % 256;
			float* p       = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ] = p[ 1 ] = p[ 2 ] = static_cast< float >( code / 255.0 );
			p[ 3 ]                   = 1.0f;
		}
	return card;
}

/// Every pixel its own colour from the hash, eight bits a gun.
Floats noiseCard( int width, int height, uint32_t seed )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < card.size(); ++i )
		card[ i ] = ( i % 4 == 3 ) ? 1.0f : eightBit( hash01( static_cast< uint32_t >( i ), seed ) );
	return card;
}

/// The card --out and --film render: "look", or "alpha" -- the look card,
/// premultiplied, its left third at coverage 1/2 and its right sixth
/// uncovered -- for the controls that act only on a clip with alpha.
Floats cardFor( const std::string& name, int width, int height, double t )
{
	Floats card = buildCard( width, height, t );
	if( name == "alpha" )
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				const float a = x < width / 3 ? 0.5f : x >= width - width / 6 ? 0.0f : 1.0f;
				float* p      = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
				for( int c = 0; c < 4; ++c )
					p[ c ] *= a;
			}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//Linear, as a host's clip texture is.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// A texture of the plugin's, read back as floats through a framebuffer of
/// the harness's own.
Floats readTexture( GLuint texture, int width, int height )
{
	Floats pixels( static_cast< size_t >( width ) * height * 4 );
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glDeleteFramebuffers( 1, &fbo );
	return pixels;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock and,
// when asked for, a synthetic beat.
//---------------------------------------------------------------------------
struct Rig
{
	RamdacPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame  = 0;
	double fps = 60.0;
	double bpm = 0.0;///< 0: no beat sent
	/// The host's clock: SetTime( origin + seconds x unit ), declared to the
	/// plugin as `clockScale` seconds a unit.
	double timeOrigin = 0.0, timeUnit = 1.0, clockScale = 1.0;
	std::function< void( int ) > beforeFrame;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/ramdac for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( clockScale );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}
	template< typename E >
	void Option( unsigned int id, E value )
	{
		plugin.SetFloatParameter( id, static_cast< float >( static_cast< int >( value ) ) );
	}
	void Text( unsigned int id, const char* value )
	{
		plugin.SetTextParameter( id, value );
	}

	double Seconds( int f ) const
	{
		return static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			if( beforeFrame )
				beforeFrame( frame );
			plugin.SetTime( timeOrigin + Seconds( frame ) * timeUnit );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), static_cast< float >( std::fmod( Seconds( frame ) * bpm / 240.0, 1.0 ) ) );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	Floats Grid()
	{
		return readTexture( plugin.GridTexture(), plugin.GridWidth(), plugin.GridHeight() );
	}
	Floats Indices()
	{
		return readTexture( plugin.IndexTexture(), plugin.GridWidth(), plugin.GridHeight() );
	}
};

int byteOf( float v )
{
	return static_cast< int >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

C8 colourAt( const Floats& image, int width, int x, int y )
{
	const float* p = &image[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
	return { byteOf( p[ 0 ] ), byteOf( p[ 1 ] ), byteOf( p[ 2 ] ) };
}

/// Is a float output exactly an 8-bit code? A palette member written to a
/// float framebuffer is k / 255 to a float's rounding.
bool exactByte( float v )
{
	return std::fabs( v * 255.0f - std::round( v * 255.0f ) ) <= 1e-3f;
}

int indexAt( const Floats& indices, int gw, int cx, int cy )
{
	return static_cast< int >( std::lround( indices[ ( static_cast< size_t >( cy ) * gw + cx ) * 4 ] ) );
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( RamdacPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, or for an option the name of one of
/// its elements, or for a text control its text; anything else is refused,
/// never read as 0 (polyhedral's trap: strtof( "PICO-8" ) is 0, silently).
bool applySetting( RamdacPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_TEXT )
			{
				if( parameter.index >= PT_ABOUT_FIRST )
				{
					error = name + " cannot be set";
					return false;
				}
				plugin.SetTextParameter( parameter.index, value.c_str() );
				return true;
			}
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( RamdacPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock, with a
// beat at --bpm (120 unless told otherwise). --film renders the moving card.
//===========================================================================
int runPipe( int width, int height, double fps, double bpm, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings, bool stillCard )
{
	Rig rig;
	rig.fps = fps;
	rig.bpm = bpm;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	const Floats still = buildCard( width, height, 0.0 );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else
			rig.Upload( stillCard ? still : buildCard( width, height, rig.Seconds( index ) ) );

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks           = 0;    ///< shaders::Hook bits
	bool swapRequantise = false;///< a Cut that quantises the clip to B afresh
	bool cycleReverse   = false;///< Forward and Reverse turned round
	bool absolutePhase  = false;///< the cycle's phase from the host's time as a float
	bool coldAdaptive   = false;///< a fresh median cut every frame, shown at once
	bool resizeClears   = false;///< a new grid forgets the frozen indices
	bool knuthSwapped   = false;///< Knuth's weights swapped
	bool whiteMask      = false;///< --bluenoise judges a white-noise permutation instead
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

void applyPerturb( Rig& rig, const Perturb& perturb )
{
	rig.plugin.SetHooksForTest( perturb.hooks );
	rig.plugin.SetSwapRequantiseForTest( perturb.swapRequantise );
	rig.plugin.SetCycleReverseForTest( perturb.cycleReverse );
	rig.plugin.SetAbsolutePhaseForTest( perturb.absolutePhase );
	rig.plugin.SetColdAdaptiveForTest( perturb.coldAdaptive );
	rig.plugin.SetResizeClearsForTest( perturb.resizeClears );
	rig.plugin.SetKnuthSwappedForTest( perturb.knuthSwapped );
}

/// Every control a check depends on, set: a rig default must never decide
/// what a check measures (honeydew's trap).
void baseSetup( Rig& rig, PaletteId palette, Dither dither, int pixel )
{
	rig.Option( PT_PALETTE, palette );
	rig.Option( PT_DITHER, dither );
	rig.Set( PT_PIXEL_SIZE, static_cast< float >( pixel ) );
	rig.Set( PT_DITHER_AMOUNT, 1.0f );
	rig.Set( PT_COLOURS, 16.0f );
	rig.Set( PT_ADAPT, ParamFromAdapt( 0.3 ) );
	rig.Option( PT_ORDER, Order::Lightness );
	rig.Option( PT_DAC, Dac::Bits8 );
	rig.Option( PT_SWAP_TO, PaletteId::C64 );
	rig.Set( PT_SWAP, 0.0f );
	rig.Option( PT_SWAP_STYLE, SwapStyle::Cut );
	rig.Option( PT_CYCLE, CycleMode::Off );
	rig.Set( PT_CYCLE_RATE, ParamFromCycleRate( 8.0 ) );
	rig.Option( PT_CYCLE_SYNC, CycleSync::Free );
	rig.Set( PT_CYCLE_START, 0.0f );
	rig.Set( PT_CYCLE_LENGTH, 16.0f );
	rig.Option( PT_CYCLE_BLEND, CycleBlend::Step );
	rig.Set( PT_FREEZE, 0.0f );
	rig.Option( PT_ALPHA, AlphaMode::Keep );
	rig.Set( PT_MIX, 1.0f );
}

const char* ditherName( Dither d )
{
	switch( d )
	{
	case Dither::None: return "None";
	case Dither::Bayer2: return "Bayer 2";
	case Dither::Bayer4: return "Bayer 4";
	case Dither::Bayer8: return "Bayer 8";
	case Dither::BlueNoise: return "Blue Noise";
	case Dither::DotDiffusion: return "Dot Diffusion";
	default: return "?";
	}
}
const Dither kDithers[] = { Dither::None, Dither::Bayer2, Dither::Bayer4, Dither::Bayer8, Dither::BlueNoise, Dither::DotDiffusion };

/// The table the plugin uploaded last frame, as a set.
std::set< C8 > tableSet( const RamdacPlugin& plugin )
{
	std::set< C8 > out;
	const Table& t = plugin.LastTable();
	for( int i = 0; i < t.count; ++i )
		out.insert( c8Of( t.entries[ static_cast< size_t >( i ) ] ) );
	return out;
}

/// Each output pixel's index, from its colour and the working palette `p`
/// (distinct colours): -1 where the colour is none of them.
std::vector< int > indexMap( const Floats& out, int w, int h, const std::vector< C8 >& p )
{
	std::map< C8, int > of;
	for( size_t i = 0; i < p.size(); ++i )
		of.emplace( p[ i ], static_cast< int >( i ) );
	std::vector< int > map( static_cast< size_t >( w ) * h, -1 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const auto found = of.find( colourAt( out, w, x, y ) );
			if( found != of.end() )
				map[ static_cast< size_t >( y ) * w + x ] = found->second;
		}
	return map;
}

std::vector< C8 > toC8( const Palette& p )
{
	std::vector< C8 > out;
	for( int i = 0; i < p.count; ++i )
		out.push_back( c8Of( p.entries[ static_cast< size_t >( i ) ] ) );
	return out;
}

//===========================================================================
// --palettes (no GL): the tables against the harness's own transcription.
//===========================================================================
int runPalettes( const Perturb& )
{
	std::printf( "\n=== palettes: every fixed table is its source's values (the harness's own transcription), in its machine's order; "
	             "Lightness order agrees with the harness's OKLab\n" );
	for( const RefPalette& ref : refPalettes() )
	{
		const Palette& p   = LibraryPalette( ref.id );
		int wrong          = 0;
		const bool counted = p.count == static_cast< int >( ref.colours.size() );
		for( int i = 0; counted && i < p.count; ++i )
			wrong += !( c8Of( p.entries[ static_cast< size_t >( i ) ] ) == ref.colours[ static_cast< size_t >( i ) ] );
		const std::vector< C8 > mine = toC8( Ordered( p, Order::Lightness ) ), theirs = lightnessOrdered( ref.colours );
		Check( counted && wrong == 0 && mine == theirs,
		       fmt( "%-14s %2d colours (the source has %zu), %d differ, lightness order %s", ref.name, p.count, ref.colours.size(), wrong,
		            mine == theirs ? "agrees" : "DIFFERS" ) );
	}
	//Teletext's eight are the RGB cube's corners in its colour codes' order
	//(bit 0 red, bit 1 green, bit 2 blue), whatever any table says.
	int corners = 0;
	for( int c = 0; c < 8; ++c )
	{
		const Rgb8& e = LibraryPalette( PaletteId::Teletext ).entries[ static_cast< size_t >( c ) ];
		corners += e.r == ( c & 1 ? 255 : 0 ) && e.g == ( c & 2 ? 255 : 0 ) && e.b == ( c & 4 ? 255 : 0 );
	}
	Check( corners == 8, fmt( "Teletext: %d of 8 entries are corner c of the cube, red bit 0, green bit 1, blue bit 2", corners ) );
	//CGA's sixteen are RGBI: 0xAA a gun on, 0x55 more with intensity, colour 6's green halved.
	int rgbi = 0;
	for( int c = 0; c < 16; ++c )
	{
		const int on = 0xAA, plus = c >= 8 ? 0x55 : 0;
		int r = ( c & 4 ? on : 0 ) + plus, g = ( c & 2 ? on : 0 ) + plus, b = ( c & 1 ? on : 0 ) + plus;
		if( c == 6 )
			g = 0x55;
		const Rgb8& e = LibraryPalette( PaletteId::Cga16 ).entries[ static_cast< size_t >( c ) ];
		rgbi += e.r == r && e.g == g && e.b == b;
	}
	Check( rgbi == 16, fmt( "CGA 16: %d of 16 entries are RGBI with brown, by rule", rgbi ) );
	return Verdict();
}

//===========================================================================
// --custom (no GL): the hex parser.
//===========================================================================
int runCustom( const Perturb& )
{
	std::printf( "\n=== custom: Custom's text parses as it says -- #RRGGBB, RRGGBB, 0xRRGGBB, $RGB and #RGB, separated by spaces, "
	             "commas or semicolons; the rest skipped and counted; fewer than two colours is the default\n" );
	struct Case
	{
		const char* text;
		std::vector< C8 > want;
		int rejected;
		bool usedDefault;
	};
	const Case cases[] = {
		{ "#000000 #FFFFFF", { { 0, 0, 0 }, { 255, 255, 255 } }, 0, false },
		{ "$F80, $05A;#fff\t0x123456 nonsense 12345 ff00ff",
		  { { 255, 136, 0 }, { 0, 85, 170 }, { 255, 255, 255 }, { 18, 52, 86 }, { 255, 0, 255 } },
		  2, false },
		{ "#abcdef,,;  ;#ABCDEF", { { 171, 205, 239 }, { 171, 205, 239 } }, 0, false },
	};
	for( const Case& c : cases )
	{
		int rejected     = -1;
		bool usedDefault = true;
		const Palette p  = ParseCustom( c.text, &rejected, &usedDefault );
		Check( toC8( p ) == c.want && rejected == c.rejected && usedDefault == c.usedDefault,
		       fmt( "\"%s\": %d colours (want %zu), %d not understood (want %d)", c.text, p.count, c.want.size(), rejected, c.rejected ) );
	}
	const Palette fallback = ParseCustom( DefaultCustomText() );
	for( const char* text : { "", "#000000", "nothing at all" } )
	{
		bool usedDefault = false;
		const Palette p  = ParseCustom( text, nullptr, &usedDefault );
		Check( usedDefault && toC8( p ) == toC8( fallback ) && p.count == 8,
		       fmt( "\"%s\": fewer than two colours, so the default's %d", text, p.count ) );
	}
	std::string many;
	for( int i = 0; i < 20; ++i )
		many += fmt( "#%02x%02x%02x ", i * 12, 255 - i * 12, i * 5 );
	int rejected    = 0;
	const Palette p = ParseCustom( many, &rejected );
	Check( p.count == kMaxEntries && rejected == 4 && p.entries[ 15 ].r == 180,
	       fmt( "twenty colours: the first %d kept, %d counted as not used", p.count, rejected ) );
	Check( std::string( InfoOf( PT_CUSTOM ).text ) == DefaultCustomText(), "the Custom control's default is the parser's default" );
	return Verdict();
}

//===========================================================================
// --oklab (no GL): the harness's route reproduces Ottosson's own numbers,
// and the plugin's direct matrices agree with it.
//===========================================================================
int runOklab( const Perturb& )
{
	std::printf( "\n=== oklab: the harness's sRGB -> XYZ -> M1 route reproduces Ottosson's published XYZ test table, and the plugin's "
	             "direct linear-sRGB matrices agree with that route\n" );
	struct Row
	{
		double x, y, z, L, a, b;
	};
	//"A perceptual color space for image processing", Björn Ottosson, 2020:
	//the table of XYZ inputs and OKLab outputs, three decimals each.
	const Row table[] = {
		{ 0.950, 1.000, 1.089, 1.000, 0.000, 0.000 },
		{ 1.000, 0.000, 0.000, 0.450, 1.236, -0.019 },
		{ 0.000, 1.000, 0.000, 0.922, -0.671, 0.263 },
		{ 0.000, 0.000, 1.000, 0.153, -1.415, -0.449 },
	};
	double worst = 0.0;
	for( const Row& r : table )
	{
		const Ref o = oklabOfXyz( r.x, r.y, r.z );
		worst       = std::max( { worst, std::fabs( o.L - r.L ), std::fabs( o.a - r.a ), std::fabs( o.b - r.b ) } );
	}
	//Both columns are printed to three decimals: half the last digit each
	//way, and the inputs' own rounding moves the white row by as much again.
	Check( worst <= 1e-3, fmt( "Ottosson's table: the largest difference is %.1e (bound 1e-3: three printed decimals, in and out)", worst ) );

	double direct = 0.0, back = 0.0;
	for( uint32_t i = 0; i < 4096; ++i )
	{
		const double r = hash01( i, 1 ), g = hash01( i, 2 ), b = hash01( i, 3 );
		const Ref p    = refOklab( r, g, b );
		const Lab q    = OklabOfLinear( r, g, b );
		direct         = std::max( { direct, std::fabs( p.L - q.L ), std::fabs( p.a - q.a ), std::fabs( p.b - q.b ) } );
		double rgb[ 3 ];
		LinearOfOklab( q, rgb );
		back = std::max( { back, std::fabs( rgb[ 0 ] - r ), std::fabs( rgb[ 1 ] - g ), std::fabs( rgb[ 2 ] - b ) } );
	}
	//Lindbloom's sRGB matrix is printed to seven digits, so XYZ is good to
	//~5e-7; through M1, the cube root (steepest near 0) and M2 (rows summing
	//to under 5 in magnitude) that is ~1e-5 in OKLab. 1e-4 leaves a decade;
	//one 8-bit step of L is ~4e-3 near white.
	Check( direct <= 1e-4, fmt( "4096 colours: the plugin's direct OKLab is within %.1e of the XYZ route (bound 1e-4)", direct ) );
	//Ottosson's inverse is printed to ten digits: a round trip to ~1e-9.
	Check( back <= 1e-6, fmt( "4096 colours: OKLab and back to linear sRGB is within %.1e (bound 1e-6)", back ) );
	return Verdict();
}

//===========================================================================
// --bluenoise (no GL): the mask is a permutation, and blue.
//===========================================================================
/// Mean power at radial frequencies 0 < |f| < 8 cycles across the 64-cell
/// mask, over the mean at every non-zero frequency. White noise's spectrum is
/// flat, so it scores 1 in expectation; blue noise has almost nothing low.
double lowPower( const std::vector< double >& m )
{
	constexpr int n = kBlueNoiseSize;
	double mean     = 0.0;
	for( double v : m )
		mean += v;
	mean /= static_cast< double >( m.size() );
	double energy = 0.0;
	for( double v : m )
		energy += ( v - mean ) * ( v - mean );
	//Parseval: the sum of |X|^2 over every frequency is N times the sum of
	//squares, and the DC term is 0 once the mean is out.
	const double total  = static_cast< double >( n * n );
	const double allMean = total * energy / ( total - 1.0 );
	double low = 0.0;
	int count  = 0;
	for( int ky = -7; ky <= 7; ++ky )
		for( int kx = -7; kx <= 7; ++kx )
		{
			const double r = std::sqrt( static_cast< double >( kx * kx + ky * ky ) );
			if( r <= 0.0 || r >= 8.0 )
				continue;
			std::complex< double > sum = 0.0;
			for( int y = 0; y < n; ++y )
				for( int x = 0; x < n; ++x )
				{
					const double phase = -2.0 * 3.14159265358979323846 * ( kx * x + ky * y ) / n;
					sum += ( m[ static_cast< size_t >( y * n + x ) ] - mean ) * std::complex< double >( std::cos( phase ), std::sin( phase ) );
				}
			low += std::norm( sum );
			++count;
		}
	return ( low / count ) / allMean;
}

int runBlueNoise( const Perturb& perturb )
{
	std::printf( "\n=== bluenoise: the mask is every rank 0..4095 once, and its low frequencies hold a small, stated fraction of white "
	             "noise's power -- the mask, and the patterns it thresholds into\n" );
	constexpr int n = kBlueNoiseSize * kBlueNoiseSize;
	std::vector< int > mask( kBlueNoise, kBlueNoise + n );
	if( perturb.whiteMask )
	{
		std::iota( mask.begin(), mask.end(), 0 );
		for( int i = n - 1; i > 0; --i )
			std::swap( mask[ static_cast< size_t >( i ) ], mask[ static_cast< size_t >( pcg( static_cast< uint32_t >( i ) ) % ( i + 1 ) ) ] );
	}
	std::vector< int > sorted = mask;
	std::sort( sorted.begin(), sorted.end() );
	bool permutation = true;
	for( int i = 0; i < n; ++i )
		permutation &= sorted[ static_cast< size_t >( i ) ] == i;
	Check( permutation, "every rank 0..4095 appears exactly once" );

	std::vector< double > values( mask.begin(), mask.end() );
	const double whole = lowPower( values );
	Check( whole < 0.1, fmt( "the mask: low-frequency power %.4f of white noise's (bound 0.1, a tenth)", whole ) );
	for( int level : { 8, 4, 2 } )
	{
		std::vector< double > binary( values.size() );
		for( size_t i = 0; i < binary.size(); ++i )
			binary[ i ] = mask[ i ] < n / level ? 1.0 : 0.0;
		const double p = lowPower( binary );
		Check( p < 0.25, fmt( "the 1/%d pattern: low-frequency power %.4f of white noise's (bound 0.25, a quarter)", level, p ) );
	}
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters; the palette "
	             "lists are the library's names\n" );
	RamdacPlugin plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
	       fmt( "SW Ramdac: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	int listed = 0;
	for( unsigned int id : { static_cast< unsigned int >( PT_PALETTE ), static_cast< unsigned int >( PT_SWAP_TO ) } )
		for( int e = 0; e < static_cast< int >( PaletteId::Count ); ++e )
			listed += std::string( plugin.GetParamElementName( id, static_cast< unsigned int >( e ) ) ) == PaletteName( e );
	int named = 0;
	for( const RefPalette& ref : refPalettes() )
		named += std::string( PaletteName( static_cast< int >( ref.id ) ) ) == ref.name;
	Check( listed == 2 * static_cast< int >( PaletteId::Count ) && named == static_cast< int >( refPalettes().size() ),
	       fmt( "Palette and Swap To list the library in order (%d of %d); every fixed palette is named as its source is (%d of %zu)", listed,
	            2 * static_cast< int >( PaletteId::Count ), named, refPalettes().size() ) );
	return Verdict();
}

//===========================================================================
// --members: every output pixel is one of the table's entries.
//===========================================================================
/// Pixels that are not exactly one of `allowed`, eight bits a gun.
int strangers( const Floats& out, int w, int h, const std::set< C8 >& allowed, std::string& first )
{
	int bad = 0;
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const float* p = &out[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
			const C8 c     = colourAt( out, w, x, y );
			if( exactByte( p[ 0 ] ) && exactByte( p[ 1 ] ) && exactByte( p[ 2 ] ) && allowed.count( c ) )
				continue;
			if( bad++ == 0 )
				first = fmt( "; the first at (%d, %d) is %.5f %.5f %.5f", x, y, p[ 0 ], p[ 1 ], p[ 2 ] );
		}
	return bad;
}

int runMembers( const Perturb& perturb )
{
	std::printf( "\n=== members: every output pixel is exactly one of the table's 8-bit entries -- every palette, every dither, "
	             "mid-cycle, after a cut, mid-fade -- and a fixed palette's table is its source's colours\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		applyPerturb( rig, perturb );
		rig.beforeFrame = [ & ]( int f ) { rig.Upload( buildCard( rig.width, rig.height, 0.37 * f ) ); };
		auto judge      = [ & ]( const std::string& what, const std::set< C8 >* source ) {
            if( !rig.Render( 2 ) )
                return false;
            const std::set< C8 > table = tableSet( rig.plugin );
            std::string first;
            const int bad      = strangers( rig.Output(), rig.width, rig.height, table, first );
            const bool matches = source == nullptr || *source == table;
            Check( bad == 0 && matches, fmt( "%dx%d %-34s %d pixels not in the table%s%s", raster.w, raster.h, what.c_str(), bad,
                                             source && !matches ? ", and the table is NOT the source's colours" : "", first.c_str() ) );
            return true;
		};
		for( const RefPalette& ref : refPalettes() )
		{
			baseSetup( rig, ref.id, Dither::Bayer4, 2 );
			const std::set< C8 > source( ref.colours.begin(), ref.colours.end() );
			if( !judge( std::string( ref.name ) + ", Bayer 4", &source ) )
				return 1;
		}
		for( PaletteId p : { PaletteId::Pico8, PaletteId::Adaptive, PaletteId::Custom } )
			for( Dither d : kDithers )
			{
				baseSetup( rig, p, d, 2 );
				if( !judge( std::string( PaletteName( static_cast< int >( p ) ) ) + ", " + ditherName( d ), nullptr ) )
					return 1;
			}
		baseSetup( rig, PaletteId::Pico8, Dither::Bayer4, 2 );
		rig.Option( PT_CYCLE, CycleMode::Forward );
		rig.Option( PT_CYCLE_BLEND, CycleBlend::Smooth );
		rig.Set( PT_CYCLE_START, 4.0f );
		rig.Set( PT_CYCLE_LENGTH, 9.0f );
		if( !judge( "PICO-8, mid-cycle, Smooth", nullptr ) )
			return 1;
		baseSetup( rig, PaletteId::Pico8, Dither::Bayer8, 2 );
		rig.Set( PT_SWAP, 1.0f );
		if( !judge( "PICO-8 cut to Commodore 64", nullptr ) )
			return 1;
		rig.Option( PT_SWAP_STYLE, SwapStyle::Fade );
		rig.Set( PT_SWAP, 0.4f );
		if( !judge( "PICO-8 fading to Commodore 64, 0.4", nullptr ) )
			return 1;
	}
	return Verdict();
}

//===========================================================================
// --nearest: with Dither None, each cell is the OKLab-nearest entry to its
// mean in linear light.
//===========================================================================
int runNearest( const Perturb& perturb )
{
	std::printf( "\n=== nearest: each cell's mean is the clip's mean in linear light, and with Dither None its index is the entry "
	             "nearest it in OKLab -- the harness's OKLab, by XYZ, in double\n" );
	for( const Raster& raster : kRasters )
		for( int pixel : { 1, 3 } )
			for( PaletteId id : { PaletteId::Pico8, PaletteId::Cga16, PaletteId::C64 } )
			{
				const Floats card = noiseCard( raster.w, raster.h, 11u + static_cast< uint32_t >( id ) );
				Rig rig;
				if( !rig.Init( raster.w, raster.h, &card ) )
					return 1;
				applyPerturb( rig, perturb );
				baseSetup( rig, id, Dither::None, pixel );
				if( !rig.Render( 1 ) )
					return 1;
				const Floats grid = rig.Grid(), indices = rig.Indices();
				const int gw = rig.plugin.GridWidth(), gh = rig.plugin.GridHeight();
				const std::vector< C8 > palette = lightnessOrdered( refPalette( id ).colours );
				std::vector< Ref > labs;
				for( const C8& c : palette )
					labs.push_back( refOf( c ) );
				int wrong = 0, ties = 0, meanWrong = 0;
				double worstMean = 0.0;
				std::string first;
				for( int cy = 0; cy < gh; ++cy )
					for( int cx = 0; cx < gw; ++cx )
					{
						//The mean, as the harness takes it.
						double mean[ 3 ] = { 0, 0, 0 };
						int n            = 0;
						for( int y = cy * pixel; y < std::min( ( cy + 1 ) * pixel, raster.h ); ++y )
							for( int x = cx * pixel; x < std::min( ( cx + 1 ) * pixel, raster.w ); ++x, ++n )
								for( int c = 0; c < 3; ++c )
									mean[ c ] += linearOf( card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + c ] );
						const float* g = &grid[ ( static_cast< size_t >( cy ) * gw + cx ) * 4 ];
						for( int c = 0; c < 3; ++c )
						{
							mean[ c ] /= n;
							//Within one of half float's ulps at the mean, and the
							//float toLinear's ~1e-7: the GPU may truncate when it
							//writes a half, so a whole ulp, not half of one.
							const double ulp = mean[ c ] >= std::ldexp( 1.0, -14 ) ? std::ldexp( 1.0, std::ilogb( mean[ c ] ) - 10 ) : std::ldexp( 1.0, -24 );
							const double err = std::fabs( g[ c ] - mean[ c ] );
							worstMean        = std::max( worstMean, err / ulp );
							meanWrong += err > ulp + 1e-6;
						}
						double margin        = 0.0;
						const int want       = refNearest( labs, refOklab( g[ 0 ], g[ 1 ], g[ 2 ] ), margin );
						const double same[ 3 ] = { g[ 0 ], g[ 1 ], g[ 2 ] };
						//The same half in: only the float matrices' rounding, ~1e-6.
						if( nearTie( same, margin, 1e-6 ) )
						{
							++ties;
							continue;
						}
						const int got = indexAt( indices, gw, cx, cy );
						if( got != want && wrong++ == 0 )
							first = fmt( "; the first at cell (%d, %d) is %d, wants %d", cx, cy, got, want );
					}
				Check( wrong == 0 && meanWrong == 0 && ties < gw * gh / 20,
				       fmt( "%4dx%-4d Pixel Size %d, %-12s %d of %d cells not the nearest (%d near-ties not judged); means within %.2f of a half's ulp%s",
				            raster.w, raster.h, pixel, PaletteName( static_cast< int >( id ) ), wrong, gw * gh - ties, ties, worstMean,
				            first.c_str() ) );
			}
	return Verdict();
}

//===========================================================================
// --tone: an ordered dither on black and white gives every grey its count of
// white cells, and lights them in the matrix's order; on colour, every cell
// is the harness's own Knoll candidate at its rank.
//===========================================================================
/// The 2^k Bayer matrix by its recursive construction, M2n = [[4M, 4M+2],
/// [4M+3, 4M+1]], not by the bit formula the plugin uses.
std::vector< int > bayerMatrix( int k )
{
	std::vector< int > m = { 0 };
	int n                = 1;
	for( int level = 0; level < k; ++level )
	{
		std::vector< int > next( static_cast< size_t >( 4 * n * n ) );
		const int add[ 2 ][ 2 ] = { { 0, 2 }, { 3, 1 } };
		for( int y = 0; y < 2 * n; ++y )
			for( int x = 0; x < 2 * n; ++x )
				next[ static_cast< size_t >( y * 2 * n + x ) ] = 4 * m[ static_cast< size_t >( ( y % n ) * n + x % n ) ] + add[ y / n ][ x / n ];
		m = next;
		n *= 2;
	}
	return m;
}

/// N and the cell's rank, as the dither's documentation defines them.
void rankOf( Dither d, int x, int y, int& n, int& rank )
{
	if( d == Dither::BlueNoise )
	{
		n    = 64;
		rank = kBlueNoise[ ( y & 63 ) * kBlueNoiseSize + ( x & 63 ) ] >> 6;
		return;
	}
	const int k = d == Dither::Bayer2 ? 1 : d == Dither::Bayer4 ? 2 : 3;
	static const std::vector< int > matrices[ 3 ] = { bayerMatrix( 1 ), bayerMatrix( 2 ), bayerMatrix( 3 ) };
	const int size                                = 1 << k;
	n                                             = size * size;
	rank = matrices[ k - 1 ][ static_cast< size_t >( ( y % size ) * size + x % size ) ];
}

int runTone( const Perturb& perturb )
{
	std::printf( "\n=== tone: Bayer 2/4/8 and Blue Noise on black and white over greys: a cell is white exactly when its rank is "
	             "past N - k, k = ceil( N v - 1/8 ) the sigma-delta count (the error stays in (1/8 - 1, 1/8], 1/8 the OKLab "
	             "midpoint); on colour palettes every cell is the harness's own Knoll candidate at its rank\n" );
	const Dither ordered[] = { Dither::Bayer2, Dither::Bayer4, Dither::Bayer8, Dither::BlueNoise };
	//The grey where black and white are equally far in OKLab: L = v^(1/3) for
	//a grey (M1's rows and M2's first row sum to 1 to 1e-8), so 1/2 cubed.
	constexpr double tau = 0.125;
	for( const Raster& raster : kRasters )
	{
		const Floats greys = greyBands( raster.w, raster.h, 16 );
		Rig rig;
		if( !rig.Init( raster.w, raster.h, &greys ) )
			return 1;
		applyPerturb( rig, perturb );
		for( Dither d : ordered )
		{
			baseSetup( rig, PaletteId::Custom, d, 1 );
			rig.Text( PT_CUSTOM, "#000000 #FFFFFF" );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats grid = rig.Grid(), indices = rig.Indices();
			const int gw = rig.plugin.GridWidth(), gh = rig.plugin.GridHeight();
			int wrong = 0, judged = 0, skipped = 0;
			std::string first;
			for( int y = 0; y < gh; ++y )
				for( int x = 0; x < gw; ++x )
				{
					const double v = grid[ ( static_cast< size_t >( y ) * gw + x ) * 4 ];
					int n = 0, rank = 0;
					rankOf( d, x, y, n, rank );
					//An attempt within a float's reach of the midpoint at any of
					//the N steps can go either way: not judged.
					bool onEdge = false;
					for( int i = 1; i <= n && !onEdge; ++i )
					{
						const double f = i * v - tau;
						onEdge           = std::fabs( f - std::round( f ) ) < 1e-4;
					}
					if( onEdge )
					{
						++skipped;
						continue;
					}
					const int k          = std::clamp( static_cast< int >( std::ceil( n * v - tau ) ), 0, n );
					const bool wantWhite = rank >= n - k;
					const bool white     = indexAt( indices, gw, x, y ) == 1;
					++judged;
					if( white != wantWhite && wrong++ == 0 )
						first = fmt( "; the first at (%d, %d), v %.5f, rank %d of %d, wants %d white", x, y, v, rank, n, k );
				}
			Check( wrong == 0 && judged > gw * gh / 2,
			       fmt( "%4dx%-4d %-10s on black and white: %d of %d cells wrong (%d near the midpoint not judged)%s", raster.w, raster.h,
			            ditherName( d ), wrong, judged, skipped, first.c_str() ) );
		}
	}

	for( const Raster& raster : kRasters )
		for( PaletteId id : { PaletteId::Pico8, PaletteId::Cga16 } )
		{
			const Floats bands = bandCard( raster.w, raster.h, 16, 7u + static_cast< uint32_t >( id ) );
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &bands ) )
				return 1;
			applyPerturb( rig, perturb );
			const std::vector< C8 > palette = lightnessOrdered( refPalette( id ).colours );
			std::vector< Ref > labs;
			std::vector< std::array< double, 3 > > lin;
			for( const C8& c : palette )
			{
				labs.push_back( refOf( c ) );
				std::array< double, 3 > l {};
				refLinear( c, l.data() );
				lin.push_back( l );
			}
			for( Dither d : ordered )
			{
				baseSetup( rig, id, d, 2 );
				if( !rig.Render( 1 ) )
					return 1;
				const Floats grid = rig.Grid(), indices = rig.Indices();
				const int gw = rig.plugin.GridWidth(), gh = rig.plugin.GridHeight();
				int wrong = 0, judged = 0, skipped = 0;
				std::string first;
				for( int y = 0; y < gh; ++y )
					for( int x = 0; x < gw; ++x )
					{
						const float* g     = &grid[ ( static_cast< size_t >( y ) * gw + x ) * 4 ];
						const double t[ 3 ] = { g[ 0 ], g[ 1 ], g[ 2 ] };
						int n = 0, rank = 0;
						rankOf( d, x, y, n, rank );
						//Knoll's candidates, the error in linear light.
						std::vector< int > counts( palette.size(), 0 );
						double err[ 3 ] = { 0, 0, 0 };
						bool tie        = false;
						for( int i = 0; i < n && !tie; ++i )
						{
							double a[ 3 ];
							for( int c = 0; c < 3; ++c )
								a[ c ] = std::clamp( t[ c ] + err[ c ], 0.0, 1.0 );
							double margin = 0.0;
							const int e   = refNearest( labs, refOklab( a[ 0 ], a[ 1 ], a[ 2 ] ), margin );
							tie           = nearTie( a, margin, kFloatSum );
							++counts[ static_cast< size_t >( e ) ];
							for( int c = 0; c < 3; ++c )
								err[ c ] += t[ c ] - lin[ static_cast< size_t >( e ) ][ static_cast< size_t >( c ) ];
						}
						if( tie )
						{
							++skipped;
							continue;
						}
						//The working order is darkest first already.
						int seen = 0, want = 0;
						for( size_t e = 0; e < counts.size(); ++e )
						{
							seen += counts[ e ];
							if( rank < seen )
							{
								want = static_cast< int >( e );
								break;
							}
						}
						++judged;
						const int got = indexAt( indices, gw, x, y );
						if( got != want && wrong++ == 0 )
							first = fmt( "; the first at cell (%d, %d) is %d, wants %d", x, y, got, want );
					}
				Check( wrong == 0 && judged > gw * gh / 4,
				       fmt( "%4dx%-4d %-10s %-7s every cell the candidate at its rank: %d of %d wrong (%d with a near-tie among their "
				            "candidates not judged)%s",
				            raster.w, raster.h, ditherName( d ), PaletteName( static_cast< int >( id ) ), wrong, judged, skipped, first.c_str() ) );
			}
		}
	return Verdict();
}

//===========================================================================
// --diffusion: Knuth's recurrence, cell by cell, from what the GPU stored.
//===========================================================================
int runDiffusion( const Perturb& perturb )
{
	std::printf( "\n=== diffusion: every cell is the nearest entry to its colour plus its lower-class neighbours' shares of their "
	             "errors (weight 2 orthogonal, 1 diagonal, over each donor's higher neighbours), and stores what is left -- read "
	             "back from the GPU; the barons are 62 and 63; and the light that comes out is the light that went in, less "
	             "what the barons, the edges and the clamp lost\n" );
	//The harness's own transcription of the paper's matrix (Knuth 1987,
	//figure 2; US 8,077,355 B2's Table 2), not the plugin's: the GPU's gather
	//uses the plugin's, so a wrong entry there is a cell off the recurrence.
	static const int kPaper[ 8 ][ 8 ] = {
		{ 34, 48, 40, 32, 29, 15, 23, 31 }, { 42, 58, 56, 53, 21, 5, 7, 10 },  { 50, 62, 61, 45, 13, 1, 2, 18 },
		{ 38, 46, 54, 37, 25, 17, 9, 26 },  { 28, 14, 22, 30, 35, 49, 41, 33 }, { 20, 4, 6, 11, 43, 59, 57, 52 },
		{ 12, 0, 3, 19, 51, 63, 60, 44 },   { 24, 16, 8, 27, 39, 47, 55, 36 },
	};
	auto classAt = []( int x, int y ) { return kPaper[ ( ( y % 8 ) + 8 ) % 8 ][ ( ( x % 8 ) + 8 ) % 8 ]; };
	int differ = 0;
	for( int y = 0; y < 8; ++y )
		for( int x = 0; x < 8; ++x )
			differ += dither::kKnuthClass[ y ][ x ] != kPaper[ y ][ x ];
	Check( differ == 0, fmt( "the plugin's class matrix is the paper's: %d of 64 entries differ", differ ) );
	auto weight  = []( int dx, int dy ) { return dx == 0 || dy == 0 ? 2.0 : 1.0; };
	auto outgoing = [ & ]( int x, int y ) {
		double total = 0.0;
		for( int dy = -1; dy <= 1; ++dy )
			for( int dx = -1; dx <= 1; ++dx )
				if( ( dx || dy ) && classAt( x + dx, y + dy ) > classAt( x, y ) )
					total += weight( dx, dy );
		return total;
	};
	std::set< int > barons;
	std::vector< bool > classSeen( 64, false );
	for( int y = 0; y < 8; ++y )
		for( int x = 0; x < 8; ++x )
		{
			classSeen[ static_cast< size_t >( classAt( x, y ) ) ] = true;
			if( outgoing( x, y ) == 0.0 )
				barons.insert( classAt( x, y ) );
		}
	Check( std::count( classSeen.begin(), classSeen.end(), true ) == 64 && barons == std::set< int > { 62, 63 },
	       fmt( "the class matrix holds every class 0..63 once; its barons are %s (the paper's: 62 and 63)",
	            barons == std::set< int > { 62, 63 } ? "62 and 63" : "OTHERS" ) );

	struct Case
	{
		const char* what;
		PaletteId palette;
		double grey;//< 0: the look card
	};
	const Case cases[] = {
		{ "black and white, grey 0.18", PaletteId::Custom, 0.18 },
		{ "black and white, grey 0.5", PaletteId::Custom, 0.5 },
		{ "black and white, grey 0.03", PaletteId::Custom, 0.03 },
		{ "PICO-8 on the look card", PaletteId::Pico8, -1.0 },
		{ "Workbench 1.3 on the look card", PaletteId::Workbench13, -1.0 },
	};
	for( const Raster& raster : kRasters )
		for( const Case& c : cases )
		{
			Floats card = c.grey >= 0.0 ? Floats( static_cast< size_t >( raster.w ) * raster.h * 4, 1.0f ) : buildCard( raster.w, raster.h );
			if( c.grey >= 0.0 )
				for( size_t i = 0; i < card.size(); ++i )
					if( i % 4 != 3 )
						card[ i ] = static_cast< float >( codeOf( c.grey ) );
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( rig, perturb );
			baseSetup( rig, c.palette, Dither::DotDiffusion, 1 );
			if( c.palette == PaletteId::Custom )
				rig.Text( PT_CUSTOM, "#000000 #FFFFFF" );
			if( !rig.Render( 1 ) )
				return 1;
			const int gw = rig.plugin.GridWidth(), gh = rig.plugin.GridHeight();
			const int tw = rig.plugin.TilesWide(), th = rig.plugin.TilesHigh();
			const Floats grid = rig.Grid(), indices = rig.Indices();
			const Floats classes = readTexture( rig.plugin.ClassTexture(), 8 * tw, 8 * th );
			const std::vector< C8 > palette = toC8( rig.plugin.LastQuantised() );
			std::vector< Ref > labs;
			std::vector< std::array< double, 3 > > lin;
			for( const C8& p : palette )
			{
				labs.push_back( refOf( p ) );
				std::array< double, 3 > l {};
				refLinear( p, l.data() );
				lin.push_back( l );
			}
			auto stored = [ & ]( int x, int y ) {
				const int cls = classAt( x, y );
				const int tx = ( cls & 7 ) * tw + x / 8, ty = ( cls >> 3 ) * th + y / 8;
				return &classes[ ( static_cast< size_t >( ty ) * 8 * tw + tx ) * 4 ];
			};
			int wrongIndex = 0, wrongError = 0, unequal = 0, ties = 0;
			double lightIn = 0.0, lightOut = 0.0, lost = 0.0;
			std::string first;
			for( int y = 0; y < gh; ++y )
				for( int x = 0; x < gw; ++x )
				{
					const int cls       = classAt( x, y );
					double incoming[ 3 ] = { 0, 0, 0 };
					for( int dy = -1; dy <= 1; ++dy )
						for( int dx = -1; dx <= 1; ++dx )
						{
							const int nx = x + dx, ny = y + dy;
							if( ( !dx && !dy ) || nx < 0 || ny < 0 || nx >= gw || ny >= gh || classAt( nx, ny ) >= cls )
								continue;
							const double share = weight( dx, dy ) / outgoing( nx, ny );
							const float* e     = stored( nx, ny );
							for( int k = 0; k < 3; ++k )
								incoming[ k ] += share * e[ k ];
						}
					const float* g = &grid[ ( static_cast< size_t >( y ) * gw + x ) * 4 ];
					double attempt[ 3 ];
					for( int k = 0; k < 3; ++k )
					{
						const double raw = g[ k ] + incoming[ k ];
						attempt[ k ]     = std::clamp( raw, 0.0, 1.0 );
						lost += std::fabs( raw - attempt[ k ] ) / 3.0;
						lightIn += g[ k ] / 3.0;
					}
					const float* s = stored( x, y );
					const int got  = static_cast< int >( std::lround( s[ 3 ] ) );
					unequal += got != indexAt( indices, gw, x, y );
					lightOut += ( lin[ static_cast< size_t >( std::clamp( got, 0, static_cast< int >( lin.size() ) - 1 ) ) ][ 0 ]
					              + lin[ static_cast< size_t >( std::clamp( got, 0, static_cast< int >( lin.size() ) - 1 ) ) ][ 1 ]
					              + lin[ static_cast< size_t >( std::clamp( got, 0, static_cast< int >( lin.size() ) - 1 ) ) ][ 2 ] )
					            / 3.0;
					double margin  = 0.0;
					const int want = refNearest( labs, refOklab( attempt[ 0 ], attempt[ 1 ], attempt[ 2 ] ), margin );
					if( nearTie( attempt, margin, kFloatSum ) )
					{
						++ties;
						continue;
					}
					if( got != want )
					{
						if( wrongIndex++ == 0 )
							first = fmt( "; the first at cell (%d, %d), class %d, is %d, wants %d", x, y, cls, got, want );
						continue;
					}
					for( int k = 0; k < 3; ++k )
					{
						const double left = attempt[ k ] - lin[ static_cast< size_t >( want ) ][ static_cast< size_t >( k ) ];
						//Half float's truncation of what is stored, and float sums
						//of up to eight shares on the GPU.
						if( std::fabs( s[ k ] - left ) > halfUlp( left ) + kFloatSum )
						{
							if( wrongError++ == 0 && first.empty() )
								first = fmt( "; the first error wrong at cell (%d, %d), class %d: %.6f, wants %.6f", x, y, cls, s[ k ], left );
						}
					}
				}
			Check( wrongIndex == 0 && wrongError == 0 && unequal == 0 && ties < gw * gh / 10,
			       fmt( "%4dx%-4d %-30s %d indices and %d errors off Knuth's recurrence, %d cells gathered wrongly (%d near-ties not "
			            "judged)%s",
			            raster.w, raster.h, c.what, wrongIndex, wrongError, unequal, ties, first.c_str() ) );
			if( c.grey >= 0.0 )
			{
				//Every cell's light comes out as an entry or is carried on as
				//error, except what a baron keeps (two a tile, each under 1),
				//what goes off the grid's edge (at most each edge cell's, under
				//1), and what the clamp cut off.
				const double cells     = static_cast< double >( gw ) * gh;
				const double edgeCells = 2.0 * ( gw + gh );
				const double bound     = ( 2.0 * std::ceil( gw / 8.0 ) * std::ceil( gh / 8.0 ) + edgeCells + lost ) / cells;
				const double drift     = std::fabs( lightOut - lightIn ) / cells;
				Check( drift <= bound, fmt( "%4dx%-4d %-30s the mean light out is %.5f of the mean in %.5f: off by %.5f (bound %.5f: the "
				                            "barons, the edges, the clamp)",
				                            raster.w, raster.h, c.what, lightOut / cells, lightIn / cells, drift, bound ) );
			}
		}
	return Verdict();
}

//===========================================================================
// --still: a still clip gives a still picture.
//===========================================================================
/// Two palettes in OKLab, bit for bit.
bool sameLab( const std::vector< Lab >& a, const std::vector< Lab >& b )
{
	if( a.size() != b.size() )
		return false;
	for( size_t i = 0; i < a.size(); ++i )
		if( a[ i ].L != b[ i ].L || a[ i ].a != b[ i ].a || a[ i ].b != b[ i ].b )
			return false;
	return true;
}

int runStill( const Perturb& perturb )
{
	std::printf( "\n=== still: on a still clip every dither holds still -- frames bit for bit the same, fixed, Custom and Adaptive "
	             "palettes (Adaptive once it has converged, which --adaptive bounds)\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats card = buildCard( raster.w, raster.h, 3.0 );
		Rig rig;
		if( !rig.Init( raster.w, raster.h, &card ) )
			return 1;
		applyPerturb( rig, perturb );
		for( PaletteId p : { PaletteId::Pico8, PaletteId::Custom, PaletteId::Adaptive } )
			for( Dither d : kDithers )
			{
				baseSetup( rig, p, d, 2 );
				//Adaptive: up to 300 frames for the palette to arrive -- what is
				//shown equal to the target, and the target where it was a frame
				//ago (two equal pictures are not enough: near the end the
				//one-pole moves an entry by less than a code a frame) -- then
				//the same test. --adaptive bounds how long it takes.
				int frames = 0;
				std::vector< Lab > lastTarget;
				bool settled = p != PaletteId::Adaptive;
				while( !settled && frames < 300 )
				{
					if( !rig.Render( 1 ) )
						return 1;
					++frames;
					const Adaptive& a = rig.plugin.AdaptiveState();
					settled           = sameLab( a.ShownLab(), a.Target() ) && sameLab( a.Target(), lastTarget );
					lastTarget        = a.Target();
				}
				if( !rig.Render( 2 ) )
					return 1;
				const Floats a = rig.Output();
				if( !rig.Render( 3 ) )
					return 1;
				const Floats b = rig.Output();
				size_t differ  = 0;
				for( size_t i = 0; i < a.size(); ++i )
					differ += a[ i ] != b[ i ];
				Check( settled && differ == 0,
				       fmt( "%4dx%-4d %-12s %-13s three frames apart: %zu values differ%s", raster.w, raster.h, PaletteName( static_cast< int >( p ) ),
				            ditherName( d ), differ, settled ? "" : " (the palette never settled)" ) );
			}
	}
	return Verdict();
}

//===========================================================================
// --swap: a swap keeps every index.
//===========================================================================
using Mat3 = std::array< std::array< double, 3 >, 3 >;

Mat3 multiply( const Mat3& a, const Mat3& b )
{
	Mat3 m {};
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			for( int k = 0; k < 3; ++k )
				m[ i ][ j ] += a[ i ][ k ] * b[ k ][ j ];
	return m;
}

Mat3 inverse( const Mat3& m )
{
	const double det = m[ 0 ][ 0 ] * ( m[ 1 ][ 1 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 1 ] )
	                   - m[ 0 ][ 1 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 0 ] )
	                   + m[ 0 ][ 2 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 1 ] - m[ 1 ][ 1 ] * m[ 2 ][ 0 ] );
	Mat3 r {};
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
		{
			const int a = ( j + 1 ) % 3, b = ( j + 2 ) % 3, c = ( i + 1 ) % 3, d = ( i + 2 ) % 3;
			r[ i ][ j ] = ( m[ a ][ c ] * m[ b ][ d ] - m[ a ][ d ] * m[ b ][ c ] ) / det;
		}
	return r;
}

/// OKLab back to 8 bits by the harness's route: its forward matrices
/// inverted numerically, not Ottosson's printed inverses.
C8 refC8Of( const Ref& lab )
{
	static const Mat3 srgbToXyz = { { { 0.4124564, 0.3575761, 0.1804375 }, { 0.2126729, 0.7151522, 0.0721750 }, { 0.0193339, 0.1191920, 0.9503041 } } };
	static const Mat3 m1        = { { { 0.8189330101, 0.3618667424, -0.1288597137 }, { 0.0329845436, 0.9293118715, 0.0361456387 }, { 0.0482003018, 0.2643662691, 0.6338517070 } } };
	static const Mat3 m2        = { { { 0.2104542553, 0.7936177850, -0.0040720468 }, { 1.9779984951, -2.4285922050, 0.4505937099 }, { 0.0259040371, 0.7827717662, -0.8086757660 } } };
	static const Mat3 toLms     = inverse( multiply( m1, srgbToXyz ) );
	static const Mat3 fromLab   = inverse( m2 );
	const double v[ 3 ] = { lab.L, lab.a, lab.b };
	double lms[ 3 ], rgb[ 3 ];
	for( int i = 0; i < 3; ++i )
	{
		const double root = fromLab[ i ][ 0 ] * v[ 0 ] + fromLab[ i ][ 1 ] * v[ 1 ] + fromLab[ i ][ 2 ] * v[ 2 ];
		lms[ i ]          = root * root * root;
	}
	for( int i = 0; i < 3; ++i )
		rgb[ i ] = toLms[ i ][ 0 ] * lms[ 0 ] + toLms[ i ][ 1 ] * lms[ 1 ] + toLms[ i ][ 2 ] * lms[ 2 ];
	auto gun = [ & ]( double l ) { return static_cast< int >( std::lround( codeOf( std::clamp( l, 0.0, 1.0 ) ) * 255.0 ) ); };
	return { gun( rgb[ 0 ] ), gun( rgb[ 1 ] ), gun( rgb[ 2 ] ) };
}

int runSwap( const Perturb& perturb )
{
	std::printf( "\n=== swap: the picture is quantised to A once; a Cut shows B_j for every pixel of index i (j = floor( (i + 1/2) M / N )), "
	             "measured from the two pictures; under Fade every pixel's colour is a function of its index alone, on the OKLab "
	             "line from A_i to B_j; Ripple cuts the indices in order\n" );
	struct Pair
	{
		PaletteId a, b;
	};
	const Pair pairs[] = { { PaletteId::C64, PaletteId::Pico8 }, { PaletteId::Pico8, PaletteId::GameBoy }, { PaletteId::Cga16, PaletteId::Teletext } };
	for( const Raster& raster : kRasters )
	{
		const Floats card = buildCard( raster.w, raster.h, 5.0 );
		Rig rig;
		if( !rig.Init( raster.w, raster.h, &card ) )
			return 1;
		applyPerturb( rig, perturb );
		for( const Pair& pair : pairs )
		{
			const std::vector< C8 > A = lightnessOrdered( refPalette( pair.a ).colours ), B = lightnessOrdered( refPalette( pair.b ).colours );
			const int n = static_cast< int >( A.size() ), m = static_cast< int >( B.size() );
			auto partner = [ & ]( int i ) { return static_cast< int >( std::floor( ( i + 0.5 ) * m / n ) ); };
			baseSetup( rig, pair.a, Dither::Bayer4, 2 );
			rig.Option( PT_SWAP_TO, pair.b );
			if( !rig.Render( 1 ) )
				return 1;
			const std::vector< int > index = indexMap( rig.Output(), rig.width, rig.height, A );
			const int unmapped             = static_cast< int >( std::count( index.begin(), index.end(), -1 ) );
			const std::string label = fmt( "%4dx%-4d %s -> %s", raster.w, raster.h, PaletteName( static_cast< int >( pair.a ) ),
			                               PaletteName( static_cast< int >( pair.b ) ) );

			//Cut: every pixel B_j(i).
			rig.Set( PT_SWAP, 1.0f );
			if( !rig.Render( 1 ) )
				return 1;
			Floats out = rig.Output();
			int wrong  = 0;
			for( int y = 0; y < rig.height; ++y )
				for( int x = 0; x < rig.width; ++x )
				{
					const int i = index[ static_cast< size_t >( y ) * rig.width + x ];
					wrong += i >= 0 && !( colourAt( out, rig.width, x, y ) == B[ static_cast< size_t >( partner( i ) ) ] );
				}
			Check( unmapped == 0 && wrong == 0, fmt( "%-34s Cut at 1: %d of %d pixels not B_j of their index (%d not in A before)", label.c_str(), wrong,
			                                         rig.width * rig.height, unmapped ) );

			//Fade: a function of the index, on the line.
			rig.Option( PT_SWAP_STYLE, SwapStyle::Fade );
			for( double s : { 0.3, 0.7 } )
			{
				rig.Set( PT_SWAP, static_cast< float >( s ) );
				if( !rig.Render( 1 ) )
					return 1;
				out = rig.Output();
				std::vector< std::set< C8 > > shown( static_cast< size_t >( n ) );
				for( int y = 0; y < rig.height; ++y )
					for( int x = 0; x < rig.width; ++x )
					{
						const int i = index[ static_cast< size_t >( y ) * rig.width + x ];
						if( i >= 0 )
							shown[ static_cast< size_t >( i ) ].insert( colourAt( out, rig.width, x, y ) );
					}
				int many = 0, off = 0;
				std::string first;
				for( int i = 0; i < n; ++i )
				{
					if( shown[ static_cast< size_t >( i ) ].empty() )
						continue;
					many += shown[ static_cast< size_t >( i ) ].size() > 1;
					const Ref x = refOf( A[ static_cast< size_t >( i ) ] ), z = refOf( B[ static_cast< size_t >( partner( i ) ) ] );
					const C8 want = refC8Of( { x.L + s * ( z.L - x.L ), x.a + s * ( z.a - x.a ), x.b + s * ( z.b - x.b ) } );
					const C8 got  = *shown[ static_cast< size_t >( i ) ].begin();
					//One code: the plugin's printed inverse and the harness's
					//numerical one differ by ~1e-5, which can round either way.
					if( std::abs( got.r - want.r ) > 1 || std::abs( got.g - want.g ) > 1 || std::abs( got.b - want.b ) > 1 )
					{
						if( off++ == 0 )
							first = fmt( "; index %d shows %d,%d,%d, wants %d,%d,%d", i, got.r, got.g, got.b, want.r, want.g, want.b );
					}
				}
				Check( many == 0 && off == 0, fmt( "%-34s Fade at %.1f: %d indices show more than one colour, %d off the OKLab line by "
				                                   "more than a code%s",
				                                   label.c_str(), s, many, off, first.c_str() ) );
			}

			//Ripple: index i cut at s >= (i + 1)/(N + 1), at nine places along
			//the swap (two would let N + 2 steps pass for N + 1).
			rig.Option( PT_SWAP_STYLE, SwapStyle::Ripple );
			for( int tenth = 1; tenth <= 9; ++tenth )
			{
				const double s = static_cast< float >( tenth / 10.0 );
				if( std::fabs( s * ( n + 1 ) - std::round( s * ( n + 1 ) ) ) < 1e-5 )
					continue;
				rig.Set( PT_SWAP, static_cast< float >( s ) );
				if( !rig.Render( 1 ) )
					return 1;
				out       = rig.Output();
				int rwrong = 0;
				for( int y = 0; y < rig.height; ++y )
					for( int x = 0; x < rig.width; ++x )
					{
						const int i = index[ static_cast< size_t >( y ) * rig.width + x ];
						if( i < 0 )
							continue;
						const C8 want = s * ( n + 1 ) >= i + 1 ? B[ static_cast< size_t >( partner( i ) ) ] : A[ static_cast< size_t >( i ) ];
						rwrong += !( colourAt( out, rig.width, x, y ) == want );
					}
				Check( rwrong == 0, fmt( "%-34s Ripple at %.1f: %d pixels not cut in index order", label.c_str(), s, rwrong ) );
			}
		}
	}
	return Verdict();
}

//===========================================================================
// --cycle: Deluxe Paint's colour cycling.
//===========================================================================
/// The harness's own: the register offset after `step` steps over `length`.
long long refOffset( CycleMode mode, long long step, int length )
{
	auto mod = []( long long v, long long n ) { return ( ( v % n ) + n ) % n; };
	if( mode == CycleMode::Forward )
		return mod( step, length );
	if( mode == CycleMode::Reverse )
		return mod( -step, length );
	//Ping-Pong: 0, 1, .. L-1, L-2, .. 1, 0, 1, ..
	const long long p = mod( step, 2LL * ( length - 1 ) );
	return p < length ? p : 2LL * ( length - 1 ) - p;
}

int runCycle( const Perturb& perturb )
{
	std::printf( "\n=== cycle: every frame is the still picture with its table rotated -- each index i in the range shows the colour "
	             "of index lo + ((i - lo - offset) mod L), the rest their own -- measured from the pictures; the offset follows "
	             "floor( rate t ), and the beat; a whole period returns bit for bit; Smooth on a whole step is Step\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats card = buildCard( raster.w, raster.h, 8.0 );
		Rig rig;
		rig.bpm = 120.0;
		if( !rig.Init( raster.w, raster.h, &card ) )
			return 1;
		applyPerturb( rig, perturb );
		baseSetup( rig, PaletteId::Pico8, Dither::Bayer4, 2 );
		if( !rig.Render( 1 ) )
			return 1;
		const std::vector< C8 > A      = lightnessOrdered( refPalette( PaletteId::Pico8 ).colours );
		const std::vector< int > index = indexMap( rig.Output(), rig.width, rig.height, A );
		constexpr int lo = 3, length = 9;

		struct Run
		{
			CycleMode mode;
			CycleSync sync;
			float rateParam;
			double perSecond;///< steps a second the run expects
			const char* what;
		};
		//Cycle Rate 0.5 is 15 steps a second and 0.25 is 3.75 (60 v^2): a
		//quarter and an eighth of a step a frame at 60 fps, exact in binary.
		const Run runs[] = {
			{ CycleMode::Forward, CycleSync::Free, 0.5f, 15.0, "Forward, 15 a second" },
			{ CycleMode::Reverse, CycleSync::Free, 0.5f, 15.0, "Reverse, 15 a second" },
			{ CycleMode::PingPong, CycleSync::Free, 0.5f, 15.0, "Ping-Pong, 15 a second" },
			{ CycleMode::Forward, CycleSync::Beat, 0.25f, 3.75 * 2.0, "Forward, 3.75 a beat at 120 bpm" },
			{ CycleMode::Forward, CycleSync::Bar, 0.5f, 15.0 / 2.0, "Forward, 15 a bar at 120 bpm" },
		};
		for( const Run& run : runs )
		{
			Rig r;
			r.bpm = 120.0;
			if( !r.Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( r, perturb );
			baseSetup( r, PaletteId::Pico8, Dither::Bayer4, 2 );
			r.Option( PT_CYCLE, run.mode );
			r.Option( PT_CYCLE_SYNC, run.sync );
			r.Set( PT_CYCLE_RATE, run.rateParam );
			r.Set( PT_CYCLE_START, static_cast< float >( lo ) );
			r.Set( PT_CYCLE_LENGTH, static_cast< float >( length ) );
			int wrong = 0, judged = 0, skipped = 0, many = 0;
			std::string first;
			std::vector< Floats > frames;
			for( int f = 0; f < 100; ++f )
			{
				if( !r.Render( 1 ) )
					return 1;
				const Floats out = r.Output();
				frames.push_back( out );
				const double steps = run.perSecond * r.Seconds( f );
				//The beat reaches the plugin as a float bar phase: a frame
				//within its rounding of a step is not judged.
				if( run.sync != CycleSync::Free && std::fabs( steps - std::round( steps ) ) < 1e-3 )
				{
					++skipped;
					continue;
				}
				const long long offset = refOffset( run.mode, static_cast< long long >( std::floor( steps + 1e-9 ) ), length );
				std::vector< std::set< C8 > > shown( A.size() );
				for( int y = 0; y < r.height; ++y )
					for( int x = 0; x < r.width; ++x )
					{
						const int i = index[ static_cast< size_t >( y ) * r.width + x ];
						if( i >= 0 )
							shown[ static_cast< size_t >( i ) ].insert( colourAt( out, r.width, x, y ) );
					}
				++judged;
				for( int i = 0; i < static_cast< int >( A.size() ); ++i )
				{
					if( shown[ static_cast< size_t >( i ) ].empty() )
						continue;
					many += shown[ static_cast< size_t >( i ) ].size() > 1;
					const int from = i >= lo && i < lo + length ? lo + static_cast< int >( ( ( i - lo - offset ) % length + length ) % length ) : i;
					if( !( *shown[ static_cast< size_t >( i ) ].begin() == A[ static_cast< size_t >( from ) ] ) && wrong++ == 0 )
						first = fmt( "; the first at frame %d, index %d (offset %lld)", f, i, offset );
				}
			}
			Check( wrong == 0 && many == 0 && judged > 50,
			       fmt( "%4dx%-4d %-32s %d of %d frames' indices wrong, %d indices showing two colours (%d frames on a beat's edge not "
			            "judged)%s",
			            raster.w, raster.h, run.what, wrong, judged, many, skipped, first.c_str() ) );
			if( run.sync == CycleSync::Free )
			{
				//A period: L steps (Ping-Pong 2(L - 1)) at a quarter step a frame.
				const int period = 4 * ( run.mode == CycleMode::PingPong ? 2 * ( length - 1 ) : length );
				Check( frames[ 5 ] == frames[ 5 + period ], fmt( "%4dx%-4d %-32s frame 5 and frame %d, a whole period on, are bit for bit "
				                                                 "the same",
				                                                 raster.w, raster.h, run.what, 5 + period ) );
			}
		}

		//Smooth on a whole step is Step; between steps it is neither end.
		Rig step, blend;
		for( Rig* p : { &step, &blend } )
		{
			if( !p->Init( raster.w, raster.h, &card ) )
				return 1;
			applyPerturb( *p, perturb );
			baseSetup( *p, PaletteId::Pico8, Dither::Bayer4, 2 );
			p->Option( PT_CYCLE, CycleMode::Forward );
			p->Set( PT_CYCLE_RATE, 0.5f );
			p->Set( PT_CYCLE_START, static_cast< float >( lo ) );
			p->Set( PT_CYCLE_LENGTH, static_cast< float >( length ) );
		}
		blend.Option( PT_CYCLE_BLEND, CycleBlend::Smooth );
		int onStep = 0, between = 0;
		for( int f = 0; f < 24; ++f )
		{
			if( !step.Render( 1 ) || !blend.Render( 1 ) )
				return 1;
			const bool same = step.Output() == blend.Output();
			if( f % 4 == 0 )
				onStep += same;
			else
				between += !same;
		}
		Check( onStep == 6 && between == 18, fmt( "%4dx%-4d Smooth: %d of 6 whole-step frames equal Step's; %d of 18 frames between "
		                                          "steps differ from it",
		                                          raster.w, raster.h, onStep, between ) );
	}
	return Verdict();
}

//===========================================================================
// --clock: Resolume's clock 499 million ms in.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: a host whose clock has run to 499,217,238 ms renders the same take as one that started at 0 s\n" );
	const Raster raster = kRasters.back();
	const Floats card   = buildCard( raster.w, raster.h, 2.0 );
	for( CycleBlend blend : { CycleBlend::Step, CycleBlend::Smooth } )
	{
		auto take = [ & ]( bool late, std::vector< Floats >& out ) {
			Rig rig;
			if( late )
			{
				rig.timeOrigin = 499217238.0;
				rig.timeUnit   = 1000.0;
				rig.clockScale = 0.001;
			}
			if( !rig.Init( raster.w, raster.h, &card ) )
				return false;
			applyPerturb( rig, perturb );
			baseSetup( rig, PaletteId::Pico8, Dither::Bayer4, 2 );
			rig.Option( PT_CYCLE, CycleMode::Forward );
			rig.Option( PT_CYCLE_BLEND, blend );
			rig.Set( PT_CYCLE_RATE, 0.37f );
			for( int f = 0; f < 240; ++f )
			{
				if( !rig.Render( 1 ) )
					return false;
				if( f % 10 == 9 )
					out.push_back( rig.Output() );
			}
			return true;
		};
		std::vector< Floats > early, late;
		if( !take( false, early ) || !take( true, late ) )
			return 1;
		int worst = 0;
		for( size_t k = 0; k < early.size(); ++k )
			for( size_t i = 0; i < early[ k ].size(); ++i )
				worst = std::max( worst, std::abs( byteOf( early[ k ][ i ] ) - byteOf( late[ k ][ i ] ) ) );
		//Step: the phase is a double sum of rate x dt, and the late take's dt
		//is good to ~1e-10 s, a million times inside kPhaseSnap. Smooth: the
		//same phase's fraction blends in OKLab and rounds to a code, where
		//1e-9 of a step can land a value on either side of a half: one code.
		const int bound = blend == CycleBlend::Step ? 0 : 1;
		Check( worst <= bound, fmt( "Cycle %s at 8.2 steps a second, 240 frames: the largest difference between the takes is %d codes "
		                            "(bound %d)",
		                            blend == CycleBlend::Step ? "Step  " : "Smooth", worst, bound ) );
	}
	return Verdict();
}

//===========================================================================
// --adaptive: the palette from the clip, without flicker.
//===========================================================================
int runAdaptive( const Perturb& perturb )
{
	std::printf( "\n=== adaptive: after a cut to a still, the palette's target settles and what is shown reaches it within the "
	             "one-pole's bound, then the picture holds bit for bit; under per-frame noise no entry moves a tenth of a "
	             "just-noticeable difference a frame; k-means never ends worse than that frame's median cut; and the palette is "
	             "chosen from colours the cells have (thin white slats stay white)\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		applyPerturb( rig, perturb );
		baseSetup( rig, PaletteId::Adaptive, Dither::Bayer4, 2 );
		const Floats still = buildCard( raster.w, raster.h, 41.0 );
		int worse          = 0;
		auto costs         = [ & ]() {
            const Adaptive& a = rig.plugin.AdaptiveState();
            worse += a.Cost() > a.MedianCutCost() * ( 1.0 + 1e-12 ) + 1e-15;
		};
		rig.beforeFrame = [ & ]( int f ) { rig.Upload( buildCard( rig.width, rig.height, 0.5 * f ) ); };
		for( int f = 0; f < 60; ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			costs();
		}
		rig.beforeFrame = nullptr;
		rig.Upload( still );
		//The 0.3 s baseSetup asked for, not the plugin's reading of its control.
		const double tau   = 0.3;
		const double alpha = 1.0 - std::exp( -( 1.0 / rig.fps ) / tau );
		std::vector< std::vector< Lab > > targets, shown;
		std::vector< Floats > outs;
		for( int f = 0; f < 260; ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			costs();
			targets.push_back( rig.plugin.AdaptiveState().Target() );
			shown.push_back( rig.plugin.AdaptiveState().ShownLab() );
			outs.push_back( rig.Output() );
		}
		int settled = static_cast< int >( targets.size() ) - 1;
		while( settled > 0 && sameLab( targets[ static_cast< size_t >( settled - 1 ) ], targets.back() ) )
			--settled;
		double distance = 0.0;
		for( size_t i = 0; i < targets.back().size(); ++i )
			distance = std::max( distance, std::sqrt( DistanceSquared( shown[ static_cast< size_t >( settled ) ][ i ], targets.back()[ i ] ) ) );
		//From the frame the target stops moving, what is shown closes on it
		//by (1 - alpha) a frame and snaps once within kSnap.
		const int bound = settled + 1
		                  + ( distance > Adaptive::kSnap ? static_cast< int >( std::ceil( std::log( Adaptive::kSnap / distance ) / std::log( 1.0 - alpha ) ) ) : 0 );
		int still2 = static_cast< int >( outs.size() ) - 1;
		while( still2 > 0 && outs[ static_cast< size_t >( still2 - 1 ) ] == outs.back() )
			--still2;
		Check( settled <= 30 && still2 <= bound && still2 < static_cast< int >( outs.size() ) - 30,
		       fmt( "%4dx%-4d a cut to a still: the target settles at frame %d (bound 30), %.3f from what is shown; the picture holds "
		            "from frame %d (the one-pole's bound at Adapt 0.3 s: %d)",
		            raster.w, raster.h, settled, distance, still2, bound ) );

		//Noise: two codes a gun, every pixel, every frame.
		Floats noisy = still;
		rig.beforeFrame = [ & ]( int f ) {
			for( size_t i = 0; i < noisy.size(); ++i )
				if( i % 4 != 3 )
					noisy[ i ] = eightBit( still[ i ] + ( std::floor( hash01( static_cast< uint32_t >( i ), static_cast< uint32_t >( f ) ) * 5.0 ) - 2.0 ) / 255.0 );
			rig.Upload( noisy );
		};
		if( !rig.Render( 60 ) )
			return 1;
		int biggest     = 0;
		double furthest = 0.0;
		Palette before  = rig.plugin.AdaptiveState().Shown();
		std::vector< Lab > lastLab = rig.plugin.AdaptiveState().ShownLab();
		for( int f = 0; f < 60; ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			costs();
			const Palette& now = rig.plugin.AdaptiveState().Shown();
			for( int i = 0; i < now.count && i < before.count; ++i )
			{
				const Rgb8 &x = now.entries[ static_cast< size_t >( i ) ], &y = before.entries[ static_cast< size_t >( i ) ];
				biggest = std::max( { biggest, std::abs( x.r - y.r ), std::abs( x.g - y.g ), std::abs( x.b - y.b ) } );
			}
			before = now;
			const std::vector< Lab >& lab = rig.plugin.AdaptiveState().ShownLab();
			if( lab.size() == lastLab.size() )
				for( size_t i = 0; i < lab.size(); ++i )
					furthest = std::max( furthest, std::sqrt( DistanceSquared( lab[ i ], lastLab[ i ] ) ) );
			lastLab = lab;
		}
		//In OKLab, where the one-pole works and where a step is seen: a tenth
		//of its just-noticeable difference (about 0.02) a frame. Codes are
		//reported, not bounded: near the gamut's edge a move of 0.001 can be
		//two codes of one gun (a saturated green's red).
		Check( furthest <= 0.002, fmt( "%4dx%-4d two codes of noise a frame: the most any entry moved in a frame is %.5f in OKLab (bound 0.002, "
		                               "a tenth of a just-noticeable difference), %d codes of a gun",
		                               raster.w, raster.h, furthest, biggest ) );
		Check( worse == 0, fmt( "%4dx%-4d %d frames of 440 ended k-means worse than their median cut", raster.w, raster.h, worse ) );

		//The palette is chosen from the cells it is for. White slats a pixel
		//wide, one column in eight, over PICO-8's navy, at Pixel Size 1: every
		//slat pixel must show white. Means of blocks would be blue-greys that
		//no cell has, and the slats would show the lightest of them.
		rig.beforeFrame = nullptr;
		Floats slats( static_cast< size_t >( rig.width ) * rig.height * 4 );
		for( int y = 0; y < rig.height; ++y )
			for( int x = 0; x < rig.width; ++x )
			{
				float* p     = &slats[ ( static_cast< size_t >( y ) * rig.width + x ) * 4 ];
				const bool w = x % 8 == 0;
				p[ 0 ]       = w ? 1.0f : static_cast< float >( 0x1D / 255.0 );
				p[ 1 ]       = w ? 1.0f : static_cast< float >( 0x2B / 255.0 );
				p[ 2 ]       = w ? 1.0f : static_cast< float >( 0x53 / 255.0 );
				p[ 3 ]       = 1.0f;
			}
		baseSetup( rig, PaletteId::Adaptive, Dither::None, 1 );
		rig.Set( PT_ADAPT, 0.0f );
		rig.Upload( slats );
		if( !rig.Render( 4 ) )
			return 1;
		const Floats out = rig.Output();
		int grey = 0, slatPixels = 0;
		for( int y = 0; y < rig.height; ++y )
			for( int x = 0; x < rig.width; x += 8 )
			{
				const float* p = &out[ ( static_cast< size_t >( y ) * rig.width + x ) * 4 ];
				++slatPixels;
				grey += std::min( { p[ 0 ], p[ 1 ], p[ 2 ] } ) < 1.0f - 4.0f / 255.0f;
			}
		Rgb8 lightest {};
		const Palette& shownNow = rig.plugin.AdaptiveState().Shown();
		for( int i = 0; i < shownNow.count; ++i )
		{
			const Rgb8& e = shownNow.entries[ static_cast< size_t >( i ) ];
			if( e.r + e.g + e.b > lightest.r + lightest.g + lightest.b )
				lightest = e;
		}
		Check( grey == 0, fmt( "%4dx%-4d white slats a pixel wide, one column in eight, over navy: %d of %d slat pixels not white "
		                       "(the lightest entry is #%02X%02X%02X)",
		                       raster.w, raster.h, grey, slatPixels, lightest.r, lightest.g, lightest.b ) );
	}
	return Verdict();
}

//===========================================================================
// --dac: the DAC's grids.
//===========================================================================
int runDac( const Perturb& perturb )
{
	std::printf( "\n=== dac: at 12 bits every channel of every pixel is a multiple of 17 (the Amiga's $RGB, copperlist's Expand12), "
	             "mid-fade, mid-cycle and adaptive too; at 18 bits VGA's (n << 2) | (n >> 4); at 9 bits (n << 5) | (n << 2) | (n >> 1)\n" );
	struct Grid
	{
		Dac dac;
		const char* name;
		std::function< int( int ) > level;
		int levels;
	};
	const Grid grids[] = {
		{ Dac::Bits4, "12-bit", []( int n ) { return n * 17; }, 16 },
		{ Dac::Bits6, "18-bit", []( int n ) { return ( n << 2 ) | ( n >> 4 ); }, 64 },
		{ Dac::Bits3, "9-bit", []( int n ) { return ( n << 5 ) | ( n << 2 ) | ( n >> 1 ); }, 8 },
	};
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		applyPerturb( rig, perturb );
		for( const Grid& g : grids )
		{
			std::set< int > allowed;
			for( int n = 0; n < g.levels; ++n )
				allowed.insert( g.level( n ) );
			struct Case
			{
				const char* what;
				std::function< void() > set;
			};
			const Case cases[] = {
				{ "PICO-8", [ & ] { baseSetup( rig, PaletteId::Pico8, Dither::Bayer4, 2 ); } },
				{ "PICO-8 half way to C64", [ & ] {
					 baseSetup( rig, PaletteId::Pico8, Dither::Bayer4, 2 );
					 rig.Option( PT_SWAP_STYLE, SwapStyle::Fade );
					 rig.Set( PT_SWAP, 0.5f );
				 } },
				{ "PICO-8 cycling Smooth", [ & ] {
					 baseSetup( rig, PaletteId::Pico8, Dither::Bayer4, 2 );
					 rig.Option( PT_CYCLE, CycleMode::Forward );
					 rig.Option( PT_CYCLE_BLEND, CycleBlend::Smooth );
					 rig.Set( PT_CYCLE_RATE, 0.37f );
				 } },
				{ "Adaptive", [ & ] { baseSetup( rig, PaletteId::Adaptive, Dither::BlueNoise, 2 ); } },
			};
			for( const Case& c : cases )
			{
				c.set();
				rig.Option( PT_DAC, g.dac );
				if( !rig.Render( 7 ) )
					return 1;
				const Floats out = rig.Output();
				int off          = 0;
				for( size_t i = 0; i < out.size(); ++i )
					off += i % 4 != 3 && ( !exactByte( out[ i ] ) || !allowed.count( byteOf( out[ i ] ) ) );
				Check( off == 0, fmt( "%4dx%-4d %-7s %-24s %d channels off the grid", raster.w, raster.h, g.name, c.what, off ) );
				if( std::string( c.what ) != "PICO-8" )
					continue;
				//On the grid is not enough (every gun at level 0 is on it): each
				//entry is the harness's own nearest level, gun by gun, a tie to
				//the lower, and every pixel is one of those.
				auto nearest = [ & ]( int code ) {
					int best = 0;
					for( int n = 1; n < g.levels; ++n )
						if( std::abs( g.level( n ) - code ) < std::abs( g.level( best ) - code ) )
							best = n;
					return g.level( best );
				};
				std::multiset< C8 > want, got;
				for( const C8& e : refPalette( PaletteId::Pico8 ).colours )
					want.insert( { nearest( e.r ), nearest( e.g ), nearest( e.b ) } );
				const Table& table = rig.plugin.LastTable();
				for( int i = 0; i < table.count; ++i )
					got.insert( c8Of( table.entries[ static_cast< size_t >( i ) ] ) );
				std::string first;
				const int stray = strangers( out, rig.width, rig.height, std::set< C8 >( want.begin(), want.end() ), first );
				Check( got == want && stray == 0, fmt( "%4dx%-4d %-7s PICO-8's sixteen each at the nearest level a gun: the table %s, %d "
				                                       "pixels not one of them%s",
				                                       raster.w, raster.h, g.name, got == want ? "agrees" : "DOES NOT", stray, first.c_str() ) );
			}
		}
	}
	return Verdict();
}

//===========================================================================
// --freeze and --resize: the index buffer held.
//===========================================================================
/// Each output pixel's position in the table the plugin uploaded (the table's
/// entries must be distinct), -1 for a colour not in it.
std::vector< int > tableIndexMap( Rig& rig )
{
	return indexMap( rig.Output(), rig.width, rig.height, toC8( Palette { rig.plugin.LastTable().count, rig.plugin.LastTable().entries } ) );
}

int runFreeze( const Perturb& perturb )
{
	std::printf( "\n=== freeze: a frozen buffer holds every index while the clip moves and the table cycles on\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		applyPerturb( rig, perturb );
		baseSetup( rig, PaletteId::Pico8, Dither::Bayer4, 2 );
		rig.Option( PT_CYCLE, CycleMode::Forward );
		rig.Set( PT_CYCLE_RATE, 0.5f );
		rig.beforeFrame = [ & ]( int f ) { rig.Upload( buildCard( rig.width, rig.height, 0.4 * f ) ); };
		if( !rig.Render( 30 ) )
			return 1;
		rig.Set( PT_FREEZE, 1.0f );
		if( !rig.Render( 1 ) )
			return 1;
		const Floats heldIndices      = rig.Indices();
		const std::vector< int > held = tableIndexMap( rig );
		const Floats heldPicture      = rig.Output();
		int moved = 0, pictureSame = 0;
		for( int f = 0; f < 40; ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			moved += tableIndexMap( rig ) != held || rig.Indices() != heldIndices;
			pictureSame += rig.Output() == heldPicture;
		}
		rig.Set( PT_FREEZE, 0.0f );
		if( !rig.Render( 1 ) )
			return 1;
		const bool thawed = tableIndexMap( rig ) != held;
		Check( moved == 0 && pictureSame < 40 && thawed,
		       fmt( "%4dx%-4d 40 frozen frames of a moving clip, cycling at 15 a second: %d with an index changed, %d pictures the same "
		            "as the frozen one (the cycle must move it); unfrozen, the indices %s",
		            raster.w, raster.h, moved, pictureSame, thawed ? "follow the clip again" : "DID NOT come back" ) );
	}
	return Verdict();
}

int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: under Freeze, a new raster or a new Pixel Size keeps the frozen indices, each new cell the index of the "
	             "old cell its centre falls in; unfrozen, the cycle's phase and the adaptive palette carry on across a resize\n" );
	const Raster small = kRasters.back(), big = { 2 * small.w, 2 * small.h };
	{
		Rig rig;
		if( !rig.Init( big.w, big.h ) )
			return 1;
		applyPerturb( rig, perturb );
		baseSetup( rig, PaletteId::Pico8, Dither::BlueNoise, 2 );
		if( !rig.Render( 3 ) )
			return 1;
		rig.Set( PT_FREEZE, 1.0f );
		if( !rig.Render( 2 ) )
			return 1;
		auto expectResampled = [ & ]( const Floats& old, int ow, int oh, const char* what ) {
			const Floats now = rig.Indices();
			const int nw = rig.plugin.GridWidth(), nh = rig.plugin.GridHeight();
			//The old cell under the new centre (x + 1/2) ow / nw, exactly: a
			//centre on an edge (every one at 2:1) belongs to the cell above it.
			int wrong = 0, onEdge = 0;
			for( int y = 0; y < nh; ++y )
				for( int x = 0; x < nw; ++x )
				{
					const long long nx = ( 2LL * x + 1 ) * ow, ny = ( 2LL * y + 1 ) * oh;
					onEdge += nx % ( 2LL * nw ) == 0 || ny % ( 2LL * nh ) == 0;
					const int sx = std::min( static_cast< int >( nx / ( 2LL * nw ) ), ow - 1 ), sy = std::min( static_cast< int >( ny / ( 2LL * nh ) ), oh - 1 );
					wrong += indexAt( now, nw, x, y ) != indexAt( old, ow, sx, sy );
				}
			Check( wrong == 0, fmt( "%-46s %dx%d cells from %dx%d: %d not the index under their centre (%d of them on an old cell's edge)", what,
			                        nw, nh, ow, oh, wrong, onEdge ) );
		};
		Floats old = rig.Indices();
		int ow = rig.plugin.GridWidth(), oh = rig.plugin.GridHeight();
		if( !rig.Resize( small.w, small.h ) || !rig.Render( 2 ) )
			return 1;
		expectResampled( old, ow, oh, fmt( "frozen, the raster %dx%d -> %dx%d:", big.w, big.h, small.w, small.h ).c_str() );
		old = rig.Indices();
		ow  = rig.plugin.GridWidth();
		oh  = rig.plugin.GridHeight();
		rig.Set( PT_PIXEL_SIZE, 3.0f );
		if( !rig.Render( 2 ) )
			return 1;
		expectResampled( old, ow, oh, "frozen, Pixel Size 2 -> 3:" );
	}
	{
		Rig rig;
		if( !rig.Init( big.w, big.h ) )
			return 1;
		applyPerturb( rig, perturb );
		baseSetup( rig, PaletteId::Adaptive, Dither::Bayer4, 2 );
		rig.Option( PT_CYCLE, CycleMode::Forward );
		rig.Set( PT_CYCLE_RATE, 0.5f );
		if( !rig.Render( 30 ) )
			return 1;
		const double phase = rig.plugin.CyclePhase();
		if( !rig.Resize( small.w, small.h ) || !rig.Render( 1 ) )
			return 1;
		const bool reseeded    = rig.plugin.AdaptiveState().Reseeded();
		const double carriedOn = rig.plugin.CyclePhase() - phase;
		Check( !reseeded && std::fabs( carriedOn - 0.25 ) < 1e-9,
		       fmt( "unfrozen, the raster %dx%d -> %dx%d: the cycle moved %.4f steps over the change (a frame's 0.25); the adaptive "
		            "palette %s",
		            big.w, big.h, small.w, small.h, carriedOn, reseeded ? "started AGAIN" : "carried on" ) );
	}
	return Verdict();
}

//===========================================================================
// --alpha and --mix.
//===========================================================================
int runAlpha( const Perturb& perturb )
{
	std::printf( "\n=== alpha: the clip is premultiplied: a cell at coverage 1/2 is quantised as its own colour, and shows its entry "
	             "times 1/2 at alpha 1/2 (Keep), or the entry, opaque (Opaque); an uncovered cell is transparent black (Keep)\n" );
	for( const Raster& raster : kRasters )
	{
		//Thirds: opaque bands; the same bands at coverage 1/2, premultiplied;
		//nothing. Bands a multiple of the cell, so no cell straddles two.
		const int third = ( raster.w / 3 ) / 8 * 8;
		Floats card( static_cast< size_t >( raster.w ) * raster.h * 4, 0.0f );
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < 2 * third; ++x )
			{
				const int k    = ( x % third ) / 8;
				float* p       = &card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
				const float a  = x < third ? 1.0f : 0.5f;
				for( int c = 0; c < 3; ++c )
					p[ c ] = eightBit( hash01( static_cast< uint32_t >( k * 3 + c ), 99u ) ) * a;
				p[ 3 ] = a;
			}
		Rig rig;
		if( !rig.Init( raster.w, raster.h, &card ) )
			return 1;
		applyPerturb( rig, perturb );
		for( AlphaMode mode : { AlphaMode::Keep, AlphaMode::Opaque } )
		{
			baseSetup( rig, PaletteId::Pico8, Dither::None, 2 );
			rig.Option( PT_ALPHA, mode );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats out = rig.Output();
			int wrong        = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < third; ++x )
				{
					const float* o = &out[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					const float* h = &out[ ( static_cast< size_t >( y ) * raster.w + x + third ) * 4 ];
					const float* e = &out[ ( static_cast< size_t >( y ) * raster.w + x + 2 * third ) * 4 ];
					if( mode == AlphaMode::Keep )
						wrong += o[ 3 ] != 1.0f || h[ 3 ] != 0.5f || std::fabs( h[ 0 ] - 0.5f * o[ 0 ] ) > 1e-6f || std::fabs( h[ 1 ] - 0.5f * o[ 1 ] ) > 1e-6f
						         || std::fabs( h[ 2 ] - 0.5f * o[ 2 ] ) > 1e-6f || e[ 0 ] != 0.0f || e[ 1 ] != 0.0f || e[ 2 ] != 0.0f || e[ 3 ] != 0.0f;
					else
						wrong += o[ 3 ] != 1.0f || h[ 3 ] != 1.0f || e[ 3 ] != 1.0f || h[ 0 ] != o[ 0 ] || h[ 1 ] != o[ 1 ] || h[ 2 ] != o[ 2 ];
				}
			Check( wrong == 0, fmt( "%4dx%-4d %-6s %d of %d pixel triples wrong", raster.w, raster.h, mode == AlphaMode::Keep ? "Keep" : "Opaque",
			                        wrong, third * raster.h ) );
		}
	}
	return Verdict();
}

int runMix( const Perturb& perturb )
{
	std::printf( "\n=== mix: Mix 0 is the clip bit for bit, Mix 1 the table, and between them the straight mix\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats card = buildCard( raster.w, raster.h, 1.0 );
		Rig rig;
		if( !rig.Init( raster.w, raster.h, &card ) )
			return 1;
		applyPerturb( rig, perturb );
		baseSetup( rig, PaletteId::C64, Dither::Bayer8, 3 );
		if( !rig.Render( 1 ) )
			return 1;
		const Floats full = rig.Output();
		rig.Set( PT_MIX, 0.0f );
		if( !rig.Render( 1 ) )
			return 1;
		const Floats none = rig.Output();
		rig.Set( PT_MIX, 0.25f );
		if( !rig.Render( 1 ) )
			return 1;
		const Floats part = rig.Output();
		double worst      = 0.0;
		for( size_t i = 0; i < part.size(); ++i )
			worst = std::max( worst, static_cast< double >( std::fabs( part[ i ] - ( 0.75f * card[ i ] + 0.25f * full[ i ] ) ) ) );
		Check( none == card, fmt( "%4dx%-4d Mix 0: the clip, %s", raster.w, raster.h, none == card ? "bit for bit" : "NOT bit for bit" ) );
		//A float mix's rounding, a few ulps of 1.
		Check( worst <= 1e-6, fmt( "%4dx%-4d Mix 0.25: within %.1e of a quarter of the table and three of the clip (bound 1e-6)", raster.w,
		                           raster.h, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back, every dither, Adaptive's read-back, a "
	             "resize and Freeze\n" );
	for( Dither dither : kDithers )
	{
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		baseSetup( rig, PaletteId::Adaptive, dither, 2 );
		GLuint hostArray = 0, hostBuffer = 0, hostPack = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		glGenBuffers( 1, &hostPack );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 8; ++frame )
		{
			if( frame == 3 && !rig.Resize( 256, 144 ) )
				return 1;
			if( frame == 5 )
				rig.Set( PT_FREEZE, 1.0f );
			if( frame == 6 && !rig.Resize( 320, 180 ) )
				return 1;
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 0, 0, rig.width, rig.height );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glBindBuffer( GL_PIXEL_PACK_BUFFER, hostPack );
			glPixelStorei( GL_PACK_ALIGNMENT, 8 );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, rig.width, rig.height );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, pack = 0, alignment = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &pack );
			glGetIntegerv( GL_PACK_ALIGNMENT, &alignment );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 0 && viewport[ 1 ] == 0 && viewport[ 2 ] == rig.width && viewport[ 3 ] == rig.height, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( pack == static_cast< GLint >( hostPack ) && alignment == 8, "pixel-pack" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
		glPixelStorei( GL_PACK_ALIGNMENT, 4 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		glDeleteBuffers( 1, &hostPack );
		Check( problems == 0, fmt( "%-13s eight frames, resizes on the fourth and seventh, Freeze from the sixth: viewport, vertex array, "
		                           "array and pixel-pack buffers, program, active unit, framebuffer, blend, scissor, clear colour, colour "
		                           "mask, eight texture units (%d wrong:%s)",
		                           ditherName( dither ), problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --bench: GPU time per frame.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		std::string what;
		std::function< void( Rig& ) > set;
	};
	std::vector< Case > cases;
	for( Dither d : kDithers )
		cases.push_back( { fmt( "PICO-8, %s, Pixel Size 2", ditherName( d ) ), [ d ]( Rig& r ) { baseSetup( r, PaletteId::Pico8, d, 2 ); } } );
	cases.push_back( { "PICO-8, Dot Diffusion, Pixel Size 1", []( Rig& r ) { baseSetup( r, PaletteId::Pico8, Dither::DotDiffusion, 1 ); } } );
	cases.push_back( { "Adaptive 16, Bayer 4, Pixel Size 2", []( Rig& r ) { baseSetup( r, PaletteId::Adaptive, Dither::Bayer4, 2 ); } } );
	cases.push_back( { "PICO-8, Bayer 4, cycling Smooth, fading", []( Rig& r ) {
		                  baseSetup( r, PaletteId::Pico8, Dither::Bayer4, 2 );
		                  r.Option( PT_CYCLE, CycleMode::Forward );
		                  r.Option( PT_CYCLE_BLEND, CycleBlend::Smooth );
		                  r.Option( PT_SWAP_STYLE, SwapStyle::Fade );
		                  r.Set( PT_SWAP, 0.5f );
	                  } } );
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 60 frames of warm-up, median and worst of 60; wall time "
	             "includes Adaptive's read-back and k-means\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			rig.beforeFrame = [ & ]( int f ) {
				if( f % 4 == 0 )
					rig.Upload( buildCard( rig.width, rig.height, rig.Seconds( f ) ) );
			};
			if( !rig.Render( 60 ) )
				return 1;
			constexpr int kTimed = 60;
			std::vector< double > times, cpu;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
				cpu.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			std::sort( times.begin(), times.end() );
			std::sort( cpu.begin(), cpu.end() );
			std::printf( "  %-42s %-6s GPU median %6.2f ms, worst %6.2f  (%5.1f%% of a 60 fps frame); wall %5.2f ms\n", c.what.c_str(),
			             size.name, times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ), cpu[ kTimed / 2 ] );
		}
	glDeleteQueries( 1, &query );
	return 0;
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "palettes", runPalettes, true },
		{ "custom", runCustom, true },
		{ "oklab", runOklab, true },
		{ "bluenoise", runBlueNoise, true },
		{ "names", runNames, true },
		{ "members", runMembers, false },
		{ "nearest", runNearest, false },
		{ "tone", runTone, false },
		{ "diffusion", runDiffusion, false },
		{ "still", runStill, false },
		{ "swap", runSwap, false },
		{ "cycle", runCycle, false },
		{ "clock", runClock, false },
		{ "adaptive", runAdaptive, false },
		{ "dac", runDac, false },
		{ "freeze", runFreeze, false },
		{ "resize", runResize, false },
		{ "alpha", runAlpha, false },
		{ "mix", runMix, false },
		{ "state", runState, false },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "nearest", runNearest, "nearest by distance in code values, not OKLab", []( Perturb& p ) { p.hooks = kHookRgbDistance; } );
	add( "tone", runTone, "Knoll's error kept in OKLab, not in light", []( Perturb& p ) { p.hooks = kHookErrorOklab; } );
	add( "still", runStill, "the dither's threshold moved with the frame count", []( Perturb& p ) { p.hooks = kHookMovingDither; } );
	add( "diffusion", runDiffusion, "Knuth's weights swapped, 1 orthogonal and 2 diagonal", []( Perturb& p ) { p.knuthSwapped = true; } );
	add( "swap", runSwap, "a Cut that quantises the clip to B afresh", []( Perturb& p ) { p.swapRequantise = true; } );
	add( "cycle", runCycle, "Forward and Reverse turned round", []( Perturb& p ) { p.cycleReverse = true; } );
	add( "clock", runClock, "the cycle's phase from the host's time as a float", []( Perturb& p ) { p.absolutePhase = true; } );
	add( "adaptive", runAdaptive, "a fresh median cut every frame, shown at once", []( Perturb& p ) { p.coldAdaptive = true; } );
	add( "adaptive", runAdaptive, "the palette's samples the mean of each block, not one of its cells",
	     []( Perturb& p ) { p.hooks = kHookBlockMean; } );
	add( "resize", runResize, "a new grid forgets the frozen indices", []( Perturb& p ) { p.resizeClears = true; } );
	add( "bluenoise", runBlueNoise, "a white-noise permutation for the mask", []( Perturb& p ) { p.whiteMask = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}

//===========================================================================
// --strip: the colour-cycling figure. Concentric rings, one index each,
// through a cycling table, `count` frames side by side.
//===========================================================================
int runStrip( const std::string& path, int count, const std::vector< std::string >& settings )
{
	constexpr int w = 240, h = 240;
	Floats card( static_cast< size_t >( w ) * h * 4 );
	//Rings of PICO-8's sixteen in lightness order, so each ring is one index.
	const std::vector< C8 > ordered = lightnessOrdered( refPalette( PaletteId::Pico8 ).colours );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const double r = std::hypot( x + 0.5 - w / 2.0, y + 0.5 - h / 2.0 );
			const C8& c    = ordered[ static_cast< size_t >( static_cast< int >( r / 8.0 ) % 16 ) ];
			float* p       = &card[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
			p[ 0 ]         = c.r / 255.0f;
			p[ 1 ]         = c.g / 255.0f;
			p[ 2 ]         = c.b / 255.0f;
			p[ 3 ]         = 1.0f;
		}
	Rig rig;
	if( !rig.Init( w, h, &card ) )
		return 1;
	baseSetup( rig, PaletteId::Pico8, Dither::None, 1 );
	rig.Option( PT_CYCLE, CycleMode::Forward );
	rig.Set( PT_CYCLE_RATE, 0.5f );
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	//Frames a step apart at 15 steps a second and 60 fps.
	constexpr int gap = 4;
	const int sw      = w * count + 4 * ( count - 1 );
	Floats strip( static_cast< size_t >( sw ) * h * 4, 1.0f );
	for( int k = 0; k < count; ++k )
	{
		if( !rig.Render( k == 0 ? 1 : gap ) )
			return 1;
		const Floats out = rig.Output();
		for( int y = 0; y < h; ++y )
			std::memcpy( &strip[ ( static_cast< size_t >( y ) * sw + k * ( w + 4 ) ) * 4 ], &out[ static_cast< size_t >( y ) * w * 4 ],
			             static_cast< size_t >( w ) * 4 * sizeof( float ) );
	}
	if( !writePng( path, sw, h, strip ) )
		return 1;
	std::printf( "wrote %s -- %d frames a step apart\n", path.c_str(), count );
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/ramdac.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 60;
	double fps = 60.0, bpm = 120.0;
	std::string mode, scriptPath;
	int filmFrames = -1, stripFrames = 8;
	bool sizeGiven = false, still = false;
	std::string cardName = "look";
	std::map< int, std::vector< std::string > > later;//--at FRAME "Name=V"

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "rmtest -- render Ramdac offline and measure its indices and its table\n\n"
			             "  --out PATH        render the moving card and write a PNG (default /tmp/ramdac.png)\n"
			             "  --frozen          (with --out) the card does not move\n"
			             "  --card NAME       (with --out) look (default), or alpha: a third at coverage 1/2, a sixth uncovered\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 60)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --bpm N           the synthetic beat (default 120; 0 sends none)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --at N \"Name=V\"  (with --out) set it at frame N instead\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the moving card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n"
			             "  --strip N         (with --out) N frames of a cycling ring card side by side\n\n"
			             "  checks: see tools/verify.sh;  --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--bpm" && hasNext )
			bpm = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--frozen" )
			still = true;
		else if( argument == "--card" && hasNext )
		{
			cardName = argv[ ++i ];
			if( cardName != "look" && cardName != "alpha" )
			{
				std::fprintf( stderr, "--card %s: look or alpha\n", cardName.c_str() );
				return 2;
			}
		}
		else if( argument == "--at" && i + 2 < argc )
		{
			const int at = std::atoi( argv[ ++i ] );
			later[ at ].push_back( argv[ ++i ] );
		}
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--strip" && hasNext )
		{
			mode        = "strip";
			stripFrames = std::clamp( std::atoi( argv[ ++i ] ), 1, 32 );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		RamdacPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, bpm, scriptPath, 0, true, settings, false );
	else if( mode == "film" )
		result = runPipe( width, height, fps, bpm, scriptPath, filmFrames, false, settings, still );
	else if( mode == "strip" )
		result = runStrip( outPath, stripFrames, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		rig.fps = fps;
		rig.bpm = bpm;
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			rig.Upload( cardFor( cardName, width, height, 0.0 ) );
			if( !still )
				rig.beforeFrame = [ & ]( int f ) { rig.Upload( cardFor( cardName, width, height, rig.Seconds( f ) ) ); };
			bool rendered = true;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				const auto due = later.find( f );
				if( due != later.end() )
					for( const std::string& setting : due->second )
					{
						std::string error;
						if( !applySetting( rig.plugin, setting, error ) )
						{
							std::fprintf( stderr, "--at %d %s: %s\n", f, setting.c_str(), error.c_str() );
							return 2;
						}
					}
				rendered = rig.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s); the table holds %d colours\n", outPath.c_str(), width, height,
				             frames, fps, frames / fps, rig.plugin.LastTable().count );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
