#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace ramdac
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}

constexpr double kAdaptMost = 4.0;
constexpr double kRateMost  = 60.0;

const char* const kPaletteNames[] = { "Adaptive", "Custom", "Teletext", "CGA Mode 4", "CGA 16", "Commodore 64", "ZX Spectrum",
	                                  "Amstrad CPC", "NES", "Game Boy", "PICO-8", "Workbench 1.3", "Workbench 2.0" };
const char* const kOrderNames[]      = { "Lightness", "Native" };
const char* const kDacNames[]        = { "24-bit", "18-bit", "12-bit", "9-bit" };
const char* const kDitherNames[]     = { "None", "Bayer 2", "Bayer 4", "Bayer 8", "Blue Noise", "Dot Diffusion" };
const char* const kSwapStyleNames[]  = { "Cut", "Fade", "Ripple" };
const char* const kCycleNames[]      = { "Off", "Forward", "Reverse", "Ping-Pong" };
const char* const kCycleSyncNames[]  = { "Free", "Beat", "Bar" };
const char* const kCycleBlendNames[] = { "Step", "Smooth" };
const char* const kAlphaNames[]      = { "Keep", "Opaque" };

static_assert( sizeof( kPaletteNames ) / sizeof( kPaletteNames[ 0 ] ) == static_cast< size_t >( PaletteId::Count ), "a name per palette" );

ControlInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, nullptr, 0, 0, 0, nullptr };
}
ControlInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, names, count, 0, 0, nullptr };
}
ControlInfo integer( unsigned int id, const char* name, const char* group, int value, int lo, int hi )
{
	return { id, name, group, Kind::Integer, static_cast< float >( value ), nullptr, 0, lo, hi, nullptr };
}
ControlInfo boolean( unsigned int id, const char* name, const char* group, bool value )
{
	return { id, name, group, Kind::Boolean, value ? 1.0f : 0.0f, nullptr, 0, 0, 0, nullptr };
}
ControlInfo text( unsigned int id, const char* name, const char* group, const char* value )
{
	return { id, name, group, Kind::Text, 0.0f, nullptr, 0, 0, 0, value };
}

template< typename E >
float idx( E e )
{
	return static_cast< float >( static_cast< int >( e ) );
}

//---------------------------------------------------------------------------
// The defaults: PICO-8 through Bayer 4 at two pixels a cell, which shows what
// the plugin is the moment it is dropped on a clip; the swap waiting on the
// Commodore 64 with a fade; cycling off, at eight steps a second over the
// whole table when it is turned on. The Custom default is the plugin's own
// eight colours, not anybody's published palette.
//---------------------------------------------------------------------------
const ControlInfo kTable[] = {
	option( PT_PALETTE, "Palette", "Palette", idx( PaletteId::Pico8 ), kPaletteNames, static_cast< int >( PaletteId::Count ) ),
	integer( PT_COLOURS, "Colours", "Palette", 16, 2, kMaxEntries ),
	standard( PT_ADAPT, "Adapt", "Palette", ParamFromAdapt( 0.3 ) ),
	text( PT_CUSTOM, "Custom", "Palette", "#000000 #3B1F4F #8C2F39 #D9662B #F2C14E #7FB069 #2E6F95 #F4F1DE" ),
	option( PT_ORDER, "Order", "Palette", idx( Order::Lightness ), kOrderNames, static_cast< int >( Order::Count ) ),
	option( PT_DAC, "DAC", "Palette", idx( Dac::Bits8 ), kDacNames, static_cast< int >( Dac::Count ) ),

	option( PT_DITHER, "Dither", "Dither", idx( Dither::Bayer4 ), kDitherNames, static_cast< int >( Dither::Count ) ),
	standard( PT_DITHER_AMOUNT, "Dither Amount", "Dither", 1.0f ),
	integer( PT_PIXEL_SIZE, "Pixel Size", "Dither", 2, 1, 16 ),

	option( PT_SWAP_TO, "Swap To", "Swap", idx( PaletteId::C64 ), kPaletteNames, static_cast< int >( PaletteId::Count ) ),
	standard( PT_SWAP, "Swap", "Swap", 0.0f ),
	option( PT_SWAP_STYLE, "Swap Style", "Swap", idx( SwapStyle::Fade ), kSwapStyleNames, static_cast< int >( SwapStyle::Count ) ),

	option( PT_CYCLE, "Cycle", "Cycle", idx( CycleMode::Off ), kCycleNames, static_cast< int >( CycleMode::Count ) ),
	standard( PT_CYCLE_RATE, "Cycle Rate", "Cycle", ParamFromCycleRate( 8.0 ) ),
	option( PT_CYCLE_SYNC, "Cycle Sync", "Cycle", idx( CycleSync::Free ), kCycleSyncNames, static_cast< int >( CycleSync::Count ) ),
	integer( PT_CYCLE_START, "Cycle Start", "Cycle", 0, 0, kMaxEntries - 1 ),
	integer( PT_CYCLE_LENGTH, "Cycle Length", "Cycle", kMaxEntries, 2, kMaxEntries ),
	option( PT_CYCLE_BLEND, "Cycle Blend", "Cycle", idx( CycleBlend::Step ), kCycleBlendNames, static_cast< int >( CycleBlend::Count ) ),

	boolean( PT_FREEZE, "Freeze", "Output", false ),
	option( PT_ALPHA, "Alpha", "Output", idx( AlphaMode::Keep ), kAlphaNames, static_cast< int >( AlphaMode::Count ) ),
	standard( PT_MIX, "Mix", "Output", 1.0f ),
};

static_assert( sizeof( kTable ) / sizeof( kTable[ 0 ] ) == PT_ABOUT_FIRST, "every control needs a row in kTable" );
} // namespace

const ControlInfo& InfoOf( unsigned int id )
{
	return kTable[ std::min< unsigned int >( id, PT_ABOUT_FIRST - 1 ) ];
}

int OptionIndex( float value, int count )
{
	const int index = static_cast< int >( std::lround( value ) );
	return std::clamp( index, 0, count - 1 );
}

int IntegerOf( unsigned int id, float value )
{
	const ControlInfo& info = InfoOf( id );
	const double v          = std::isfinite( value ) ? static_cast< double >( value ) : static_cast< double >( info.defaultValue );
	return std::clamp( static_cast< int >( std::lround( v ) ), info.lo, info.hi );
}

const char* PaletteName( int index )
{
	return kPaletteNames[ std::clamp( index, 0, static_cast< int >( PaletteId::Count ) - 1 ) ];
}

double AdaptFromParam( float value )
{
	const double v = clamp01( value );
	return kAdaptMost * v * v;
}
float ParamFromAdapt( double seconds )
{
	return static_cast< float >( std::sqrt( std::clamp( seconds / kAdaptMost, 0.0, 1.0 ) ) );
}

double CycleRateFromParam( float value )
{
	const double v = clamp01( value );
	return kRateMost * v * v;
}
float ParamFromCycleRate( double steps )
{
	return static_cast< float >( std::sqrt( std::clamp( steps / kRateMost, 0.0, 1.0 ) ) );
}

} // namespace ramdac
