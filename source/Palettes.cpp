#include "Palettes.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ramdac
{
namespace
{
Rgb8 hex( uint32_t v )
{
	return { static_cast< uint8_t >( v >> 16 ), static_cast< uint8_t >( v >> 8 ), static_cast< uint8_t >( v ) };
}

/// A 12-bit $RGB register, ×17 a gun.
Rgb8 reg12( uint32_t v )
{
	return { static_cast< uint8_t >( ( ( v >> 8 ) & 15 ) * 17 ), static_cast< uint8_t >( ( ( v >> 4 ) & 15 ) * 17 ),
		     static_cast< uint8_t >( ( v & 15 ) * 17 ) };
}

template< size_t N >
Palette fromHex( const uint32_t ( &values )[ N ] )
{
	static_assert( N <= kMaxEntries, "a palette is at most sixteen colours" );
	Palette p;
	p.count = static_cast< int >( N );
	for( size_t i = 0; i < N; ++i )
		p.entries[ i ] = hex( values[ i ] );
	return p;
}

template< size_t N >
Palette fromRegisters( const uint32_t ( &values )[ N ] )
{
	Palette p;
	p.count = static_cast< int >( N );
	for( size_t i = 0; i < N; ++i )
		p.entries[ i ] = reg12( values[ i ] );
	return p;
}

//---------------------------------------------------------------------------
// The tables, in each machine's own order. Sources in ATTRIBUTIONS.md.
//---------------------------------------------------------------------------

/// Teletext's eight, in its colour codes' order (black, red, green, yellow,
/// blue, magenta, cyan, white): teletext's `palette( c )`.
const uint32_t kTeletext[] = { 0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF };

/// CGA 320x200, palette 1 with the intensity bit: background black, then
/// light cyan, light magenta, white.
const uint32_t kCgaMode4[] = { 0x000000, 0x55FFFF, 0xFF55FF, 0xFFFFFF };

/// CGA's sixteen RGBI colours as the IBM 5153 showed them, with colour 6's
/// green halved (brown). EGA's default palette is the same sixteen.
const uint32_t kCga16[] = { 0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	                        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF };

/// The VIC-II's sixteen as Philip "Pepto" Timmermann measured them (2001).
const uint32_t kC64[] = { 0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
	                      0x6F4F25, 0x433900, 0x9A6759, 0x444444, 0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595 };

/// The ULA's eight at the basic level and the bright seven (bright black is
/// black), nesolume's levels: 0xD7 basic, 0xFF bright. Colour order GRB-coded
/// as the Spectrum numbers them: black, blue, red, magenta, green, cyan,
/// yellow, white.
const uint32_t kSpectrum[] = { 0x000000, 0x0000D7, 0xD70000, 0xD700D7, 0x00D700, 0x00D7D7, 0xD7D700, 0xD7D7D7,
	                           0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF };

/// Mode 0's inks 0-13 at power-on (firmware colours 1, 24, 20, 6, 26, 0, 2,
/// 8, 10, 12, 14, 16, 18, 22: SOFT968, Appendix V), each gun at MAME's three
/// levels 0x00, 0x60, 0xFF. Inks 14 and 15 flash at power-on (1/24 and 11/16)
/// and are left out.
const uint32_t kCpc[] = { 0x000060, 0xFFFF00, 0x00FFFF, 0xFF0000, 0xFFFFFF, 0x000000, 0x0000FF,
	                      0xFF00FF, 0x006060, 0x606000, 0x6060FF, 0xFF6060, 0x00FF00, 0x60FF60 };

/// Sixteen of the 2C02's 64, nesolume's table: $0F $00 $10 $20 (black and the
/// greys), then $12 $14 $16 $18 $1A $1C (row 1, every other hue) and $21 $23
/// $25 $27 $29 $2B (row 2, the hues between).
const uint32_t kNes[] = { 0x000000, 0x545454, 0x989698, 0xECEEEC, 0x3032EC, 0x8814B0, 0x982220, 0x545A00,
	                      0x087C00, 0x006678, 0x4C9AEC, 0xB062EC, 0xEC58B4, 0xD48820, 0x74C400, 0x38CC6C };

/// The DMG's four, nesolume's, darkest first as the BGP register numbers
/// them from 3 down: index 0 here is shade 3.
const uint32_t kGameBoy[] = { 0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F };

/// PICO-8's sixteen, in its colour numbers' order.
const uint32_t kPico8[] = { 0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
	                        0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA };

/// Workbench 1.x's four colour registers ($RGB): blue, white, black, orange.
const uint32_t kWorkbench13[] = { 0x05A, 0xFFF, 0x002, 0xF80 };

/// Workbench 2.x's four: grey, black, white, blue.
const uint32_t kWorkbench20[] = { 0xAAA, 0x000, 0xFFF, 0x68B };

const char* const kDefaultCustom = "#000000 #3B1F4F #8C2F39 #D9662B #F2C14E #7FB069 #2E6F95 #F4F1DE";

int hexDigit( char c )
{
	if( c >= '0' && c <= '9' )
		return c - '0';
	if( c >= 'a' && c <= 'f' )
		return c - 'a' + 10;
	if( c >= 'A' && c <= 'F' )
		return c - 'A' + 10;
	return -1;
}

bool parseColour( std::string token, Rgb8& out )
{
	if( !token.empty() && ( token[ 0 ] == '#' || token[ 0 ] == '$' ) )
		token.erase( 0, 1 );
	else if( token.size() > 2 && token[ 0 ] == '0' && ( token[ 1 ] == 'x' || token[ 1 ] == 'X' ) )
		token.erase( 0, 2 );
	if( token.size() != 6 && token.size() != 3 )
		return false;
	uint32_t v = 0;
	for( char c : token )
	{
		const int d = hexDigit( c );
		if( d < 0 )
			return false;
		v = ( v << 4 ) | static_cast< uint32_t >( d );
	}
	out = token.size() == 6 ? hex( v ) : reg12( v );
	return true;
}
} // namespace

//---------------------------------------------------------------------------
double LinearOfCode( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}

double CodeOfLinear( double light )
{
	return light <= 0.0031308 ? light * 12.92 : 1.055 * std::pow( light, 1.0 / 2.4 ) - 0.055;
}

Lab OklabOfLinear( double r, double g, double b )
{
	const double l = std::cbrt( 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b );
	const double m = std::cbrt( 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b );
	const double s = std::cbrt( 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b );
	return { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
		     0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
}

void LinearOfOklab( const Lab& lab, double rgb[ 3 ] )
{
	const double l_ = lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
	const double m_ = lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
	const double s_ = lab.L - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
	const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
	rgb[ 0 ] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
	rgb[ 1 ] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
	rgb[ 2 ] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

Lab OklabOf( const Rgb8& c )
{
	return OklabOfLinear( LinearOfCode( c.r / 255.0 ), LinearOfCode( c.g / 255.0 ), LinearOfCode( c.b / 255.0 ) );
}

Rgb8 Rgb8OfOklab( const Lab& lab )
{
	double rgb[ 3 ];
	LinearOfOklab( lab, rgb );
	Rgb8 out;
	uint8_t* guns[ 3 ] = { &out.r, &out.g, &out.b };
	for( int i = 0; i < 3; ++i )
		*guns[ i ] = static_cast< uint8_t >( std::lround( std::clamp( CodeOfLinear( std::clamp( rgb[ i ], 0.0, 1.0 ) ), 0.0, 1.0 ) * 255.0 ) );
	return out;
}

double DistanceSquared( const Lab& x, const Lab& y )
{
	const double dL = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
	return dL * dL + da * da + db * db;
}

//---------------------------------------------------------------------------
int DacBits( Dac dac )
{
	switch( dac )
	{
	case Dac::Bits6: return 6;
	case Dac::Bits4: return 4;
	case Dac::Bits3: return 3;
	default: return 8;
	}
}

int DacExpand( int level, int bits )
{
	if( bits >= 8 )
		return std::clamp( level, 0, 255 );
	//The level's bits written out again and again until eight are filled:
	//×17 at four bits, VGA's (n << 2) | (n >> 4) at six.
	int v = 0, filled = 0;
	while( filled < 8 )
	{
		v = ( v << bits ) | level;
		filled += bits;
	}
	return v >> ( filled - 8 );
}

int DacRound( int code, int bits )
{
	code = std::clamp( code, 0, 255 );
	if( bits >= 8 )
		return code;
	int best = 0, bestError = 256;
	for( int level = 0; level < ( 1 << bits ); ++level )
	{
		const int error = std::abs( DacExpand( level, bits ) - code );
		//A tie goes to the lower level.
		if( error < bestError )
		{
			bestError = error;
			best      = level;
		}
	}
	return DacExpand( best, bits );
}

Rgb8 DacRound( const Rgb8& c, int bits )
{
	return { static_cast< uint8_t >( DacRound( c.r, bits ) ), static_cast< uint8_t >( DacRound( c.g, bits ) ),
		     static_cast< uint8_t >( DacRound( c.b, bits ) ) };
}

//---------------------------------------------------------------------------
const Palette& LibraryPalette( PaletteId id )
{
	static const Palette empty;
	static const Palette teletext    = fromHex( kTeletext );
	static const Palette cgaMode4    = fromHex( kCgaMode4 );
	static const Palette cga16       = fromHex( kCga16 );
	static const Palette c64         = fromHex( kC64 );
	static const Palette spectrum    = fromHex( kSpectrum );
	static const Palette cpc         = fromHex( kCpc );
	static const Palette nes         = fromHex( kNes );
	static const Palette gameBoy     = fromHex( kGameBoy );
	static const Palette pico8       = fromHex( kPico8 );
	static const Palette workbench13 = fromRegisters( kWorkbench13 );
	static const Palette workbench20 = fromRegisters( kWorkbench20 );
	switch( id )
	{
	case PaletteId::Teletext: return teletext;
	case PaletteId::CgaMode4: return cgaMode4;
	case PaletteId::Cga16: return cga16;
	case PaletteId::C64: return c64;
	case PaletteId::Spectrum: return spectrum;
	case PaletteId::Cpc: return cpc;
	case PaletteId::Nes: return nes;
	case PaletteId::GameBoy: return gameBoy;
	case PaletteId::Pico8: return pico8;
	case PaletteId::Workbench13: return workbench13;
	case PaletteId::Workbench20: return workbench20;
	default: return empty;
	}
}

const char* DefaultCustomText()
{
	return kDefaultCustom;
}

Palette ParseCustom( const std::string& text, int* rejected, bool* usedDefault )
{
	Palette p;
	int bad = 0;
	std::string token;
	auto flush = [ & ]() {
		if( token.empty() )
			return;
		Rgb8 c;
		if( parseColour( token, c ) && p.count < kMaxEntries )
			p.entries[ static_cast< size_t >( p.count++ ) ] = c;
		else
			++bad;
		token.clear();
	};
	for( char ch : text )
	{
		if( std::isspace( static_cast< unsigned char >( ch ) ) || ch == ',' || ch == ';' )
			flush();
		else
			token += ch;
	}
	flush();
	if( rejected )
		*rejected = bad;
	const bool fallback = p.count < 2;
	if( usedDefault )
		*usedDefault = fallback;
	if( fallback )
		return ParseCustom( kDefaultCustom );
	return p;
}

std::array< int, kMaxEntries > LightnessOrder( const Palette& p )
{
	std::array< int, kMaxEntries > order {};
	std::array< double, kMaxEntries > lightness {};
	for( int i = 0; i < kMaxEntries; ++i )
	{
		order[ static_cast< size_t >( i ) ] = i;
		if( i < p.count )
			lightness[ static_cast< size_t >( i ) ] = OklabOf( p.entries[ static_cast< size_t >( i ) ] ).L;
	}
	std::stable_sort( order.begin(), order.begin() + p.count,
	                  [ & ]( int x, int y ) { return lightness[ static_cast< size_t >( x ) ] < lightness[ static_cast< size_t >( y ) ]; } );
	return order;
}

Palette Ordered( const Palette& p, Order order )
{
	if( order == Order::Native )
		return p;
	const auto permutation = LightnessOrder( p );
	Palette out;
	out.count = p.count;
	for( int i = 0; i < p.count; ++i )
		out.entries[ static_cast< size_t >( i ) ] = p.entries[ static_cast< size_t >( permutation[ static_cast< size_t >( i ) ] ) ];
	return out;
}

Palette ThroughDac( const Palette& p, int bits )
{
	Palette out = p;
	for( int i = 0; i < p.count; ++i )
		out.entries[ static_cast< size_t >( i ) ] = DacRound( p.entries[ static_cast< size_t >( i ) ], bits );
	return out;
}

} // namespace ramdac
