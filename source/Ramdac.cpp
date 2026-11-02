#include "Ramdac.h"

#include "BlueNoise.h"
#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace ffglex;

namespace ramdac
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by: a stalled host must
/// not spin the cycle through a dozen steps in one frame.
constexpr double kMaxFrameDelta = 0.25;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void setInt2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( shader.FindUniform( name ), x, y );
}

template< typename E >
E optionOf( const float* params, unsigned int id )
{
	return static_cast< E >( OptionIndex( params[ id ], static_cast< int >( E::Count ) ) );
}
} // namespace

//---------------------------------------------------------------------------
static_assert( PT_COUNT - PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
RamdacPlugin::RamdacPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The cycle runs in steps a second, so the plugin has to know what a
	//second is.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. A STANDARD parameter is a
	// plain 0..1 float (SetParamInfo clamps its default before a range could
	// be attached, SDK b1afaf9); an INTEGER is exempt, so it gets its range.
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const ControlInfo& info = InfoOf( id );
		params[ id ]          = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		case Kind::Integer:
			SetParamInfo( id, info.name, FF_TYPE_INTEGER, info.defaultValue );
			SetParamRange( id, static_cast< float >( info.lo ), static_cast< float >( info.hi ) );
			break;
		case Kind::Boolean: SetParamInfo( id, info.name, FF_TYPE_BOOLEAN, info.defaultValue >= 0.5f ); break;
		case Kind::Text: SetParamInfo( id, info.name, FF_TYPE_TEXT, info.text ); break;
		}
		SetParamGroup( id, info.group );
	}
	customText = InfoOf( PT_CUSTOM ).text;

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	plan        = dither::BuildPlan( false );
	swappedPlan = dither::BuildPlan( true );
}

//---------------------------------------------------------------------------
FFResult RamdacPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		shaders::Pass pass;
		const char* name;
	};
	const Stage stages[] = {
		{ &gridShader, shaders::Pass::Grid, "grid" },
		{ &analysisShader, shaders::Pass::Analysis, "analysis" },
		{ &quantiseShader, shaders::Pass::Quantise, "quantise" },
		{ &diffuseShader, shaders::Pass::Diffuse, "diffuse" },
		{ &settleShader, shaders::Pass::Settle, "settle" },
		{ &gatherShader, shaders::Pass::Gather, "gather" },
		{ &displayShader, shaders::Pass::Display, "display" },
		{ &resampleShader, shaders::Pass::Resample, "resample" },
	};
	for( const Stage& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the plugin simply
		//does nothing in Resolume. These lines are the only record of which
		//pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Ramdac: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//The blue-noise mask: ranks 0..4095 as floats, exact.
	std::vector< float > mask( static_cast< size_t >( kBlueNoiseSize ) * kBlueNoiseSize );
	for( size_t i = 0; i < mask.size(); ++i )
		mask[ i ] = static_cast< float >( kBlueNoise[ i ] );
	glGenTextures( 1, &maskTexture );
	glBindTexture( GL_TEXTURE_2D, maskTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, kBlueNoiseSize, kBlueNoiseSize, 0, GL_RED, GL_FLOAT, mask.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	glBindTexture( GL_TEXTURE_2D, 0 );

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void RamdacPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;
	//Resolume has been seen sending both seconds and milliseconds through
	//SetTime, and nothing in the call says which. Vote on it against the wall
	//clock for a few frames, then stop asking. (Rosette's code, by way of
	//millpond, schlieren and slitscan.)
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				//The clock is about to jump from wall time to the host's own,
				//which can be hours in: no dt across the jump.
				settledJump = true;
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

//---------------------------------------------------------------------------
void RamdacPlugin::refreshCustom()
{
	std::lock_guard< std::mutex > lock( textMutex );
	if( !customDirty )
		return;
	int rejected     = 0;
	bool usedDefault = false;
	customPalette    = ParseCustom( customText, &rejected, &usedDefault );
	if( rejected > 0 || usedDefault )
		diag::info( "Custom: " + std::to_string( customPalette.count ) + " colours, " + std::to_string( rejected ) + " not understood"
		            + ( usedDefault ? ", fewer than two, so the default" : "" ) );
	customDirty = false;
}

Palette RamdacPlugin::resolve( PaletteId id, int dacBits, Order order ) const
{
	Palette p;
	if( id == PaletteId::Adaptive )
		p = adaptive.Shown();
	else if( id == PaletteId::Custom )
		p = customPalette;
	else
		p = LibraryPalette( id );
	if( p.count <= 0 )
	{
		//Adaptive before it has seen a frame: one black entry, not nothing.
		p.count         = 1;
		p.entries[ 0 ] = Rgb8 {};
	}
	return Ordered( ThroughDac( p, dacBits ), order );
}

//---------------------------------------------------------------------------
bool RamdacPlugin::ensureIndices( GLsizei gw, GLsizei gh, bool frozen )
{
	PassBuffer& current = indices[ currentIndex ];
	if( current.IsValid() && gw == gridWidth && gh == gridHeight )
		return true;

	if( frozen && current.IsValid() && !resizeClears )
	{
		//A new grid under Freeze keeps the frozen picture, each new cell the
		//index of the old cell its centre falls in. (photofinish shipped a
		//resize that cleared its film.)
		PassBuffer& next = indices[ 1 - currentIndex ];
		if( !next.Ensure( gw, gh, GL_RGBA16F, PassBuffer::Sampling::Nearest ) )
			return false;
		{
			ScopedFBOBinding fbo( next.GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, gw, gh );
			ScopedShaderBinding shader( resampleShader.GetGLID() );
			bindUnit( 0, current.TextureID() );
			resampleShader.Set( "Frozen", 0 );
			setInt2( resampleShader, "OldSize", gridWidth, gridHeight );
			setInt2( resampleShader, "NewSize", gw, gh );
			quad.Draw();
			unbindTextureUnits( 1 );
		}
		current.Destroy();
		currentIndex = 1 - currentIndex;
	}
	else
	{
		current.Destroy();
		if( !current.Ensure( gw, gh, GL_RGBA16F, PassBuffer::Sampling::Nearest ) )
			return false;
		//Under the negative control a frozen picture is now index 0
		//everywhere, and stays so.
		indicesValid = indicesValid && frozen;
	}
	gridWidth  = gw;
	gridHeight = gh;
	return true;
}

//---------------------------------------------------------------------------
void RamdacPlugin::uploadPalette( FFGLShader& shader, const Palette& p )
{
	float lin[ kMaxEntries * 3 ] = {}, lab[ kMaxEntries * 3 ] = {}, code[ kMaxEntries * 3 ] = {};
	int order[ kMaxEntries ]     = {};
	const auto lightness         = LightnessOrder( p );
	for( int i = 0; i < kMaxEntries; ++i )
	{
		order[ i ] = lightness[ static_cast< size_t >( i ) ];
		if( i >= p.count )
			continue;
		const Rgb8& c       = p.entries[ static_cast< size_t >( i ) ];
		const double cv[ 3 ] = { c.r / 255.0, c.g / 255.0, c.b / 255.0 };
		const Lab l          = OklabOf( c );
		const double lv[ 3 ] = { l.L, l.a, l.b };
		for( int k = 0; k < 3; ++k )
		{
			lin[ i * 3 + k ]  = static_cast< float >( LinearOfCode( cv[ k ] ) );
			code[ i * 3 + k ] = static_cast< float >( cv[ k ] );
			lab[ i * 3 + k ]  = static_cast< float >( lv[ k ] );
		}
	}
	shader.Set( "Count", std::max( p.count, 1 ) );
	glUniform3fv( shader.FindUniform( "PalLin" ), kMaxEntries, lin );
	glUniform3fv( shader.FindUniform( "PalLab" ), kMaxEntries, lab );
	glUniform3fv( shader.FindUniform( "PalCode" ), kMaxEntries, code );
	glUniform1iv( shader.FindUniform( "LightOrder" ), kMaxEntries, order );
	shader.Set( "Hooks", hooks );
}

//---------------------------------------------------------------------------
void RamdacPlugin::drawGrid( const FFGLTextureStruct& source, int cell )
{
	ScopedFBOBinding fbo( grid.GetGLID(), ScopedFBOBinding::RB_REVERT );
	glViewport( 0, 0, gridWidth, gridHeight );
	ScopedShaderBinding shader( gridShader.GetGLID() );
	bindUnit( 0, source.Handle );
	gridShader.Set( "InputTexture", 0 );
	setInt2( gridShader, "Size", static_cast< int >( source.Width ), static_cast< int >( source.Height ) );
	gridShader.Set( "Cell", cell );
	quad.Draw();
	unbindTextureUnits( 1 );
}

//---------------------------------------------------------------------------
bool RamdacPlugin::runAdaptive( int colours, double dt, int dacBits )
{
	const int aw = analysis.GetWidth(), ah = analysis.GetHeight();
	readback.resize( static_cast< size_t >( aw ) * ah * 4 );
	{
		ScopedFBOBinding fbo( analysis.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, aw, ah );
		{
			ScopedShaderBinding shader( analysisShader.GetGLID() );
			bindUnit( 0, grid.TextureID() );
			analysisShader.Set( "Grid", 0 );
			analysisShader.Set( "Hooks", hooks );
			setInt2( analysisShader, "GridSize", gridWidth, gridHeight );
			setInt2( analysisShader, "AnalysisSize", aw, ah );
			quad.Draw();
			unbindTextureUnits( 1 );
		}
		//Synchronous, as nesolume's and receipt's read-backs are. A host
		//that left a pixel-pack buffer bound would have it written into.
		GLint packBuffer = 0, packAlignment = 4;
		glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer );
		glGetIntegerv( GL_PACK_ALIGNMENT, &packAlignment );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
		glPixelStorei( GL_PACK_ALIGNMENT, 4 );
		glReadPixels( 0, 0, aw, ah, GL_RGBA, GL_FLOAT, readback.data() );
		glPixelStorei( GL_PACK_ALIGNMENT, packAlignment );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, static_cast< GLuint >( packBuffer ) );
	}
	adaptive.Update( readback.data(), aw * ah, colours, AdaptFromParam( params[ PT_ADAPT ] ), dt, dacBits );
	return true;
}

//---------------------------------------------------------------------------
void RamdacPlugin::drawQuantise( const Palette& p, Dither dither, float amount )
{
	ScopedFBOBinding fbo( indices[ currentIndex ].GetGLID(), ScopedFBOBinding::RB_REVERT );
	glViewport( 0, 0, gridWidth, gridHeight );
	ScopedShaderBinding shader( quantiseShader.GetGLID() );
	bindUnit( 0, grid.TextureID() );
	bindUnit( 1, maskTexture );
	quantiseShader.Set( "Grid", 0 );
	quantiseShader.Set( "Mask", 1 );
	quantiseShader.Set( "DitherIndex", static_cast< int >( dither ) );
	quantiseShader.Set( "Amount", amount );
	quantiseShader.Set( "Frame", static_cast< int >( frameCount & 0xFFFF ) );
	uploadPalette( quantiseShader, p );
	quad.Draw();
	unbindTextureUnits( 2 );
}

//---------------------------------------------------------------------------
void RamdacPlugin::drawDiffusion( const Palette& p, float amount )
{
	const int tw = TilesWide(), th = TilesHigh();
	const auto& classPlan = knuthSwapped ? swappedPlan : plan;
	auto blockOf = [ & ]( int cls ) { return std::make_pair( ( cls & 7 ) * tw, ( cls >> 3 ) * th ); };

	for( int cls = 0; cls < dither::kClasses; ++cls )
	{
		const dither::ClassPlan& c = classPlan[ static_cast< size_t >( cls ) ];
		const auto block           = blockOf( cls );
		{
			ScopedFBOBinding fbo( classes[ 1 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( block.first, block.second, tw, th );
			ScopedShaderBinding shader( diffuseShader.GetGLID() );
			bindUnit( 0, grid.TextureID() );
			bindUnit( 1, classes[ 0 ].TextureID() );
			diffuseShader.Set( "Grid", 0 );
			diffuseShader.Set( "Classes", 1 );
			setInt2( diffuseShader, "GridSize", gridWidth, gridHeight );
			setInt2( diffuseShader, "ClassPos", c.x, c.y );
			setInt2( diffuseShader, "BlockOrigin", block.first, block.second );
			diffuseShader.Set( "Amount", amount );
			int offsets[ 16 ] = {}, texels[ 16 ] = {};
			float shares[ 8 ] = {};
			for( int i = 0; i < c.donorCount; ++i )
			{
				const dither::Donor& d = c.donors[ static_cast< size_t >( i ) ];
				const auto from        = blockOf( d.donorClass );
				offsets[ i * 2 ]       = d.dx;
				offsets[ i * 2 + 1 ]   = d.dy;
				texels[ i * 2 ]        = from.first + d.tileDx;
				texels[ i * 2 + 1 ]    = from.second + d.tileDy;
				shares[ i ]            = static_cast< float >( d.share );
			}
			diffuseShader.Set( "DonorCount", c.donorCount );
			glUniform2iv( diffuseShader.FindUniform( "DonorOffset" ), 8, offsets );
			glUniform2iv( diffuseShader.FindUniform( "DonorTexel" ), 8, texels );
			glUniform1fv( diffuseShader.FindUniform( "DonorShare" ), 8, shares );
			uploadPalette( diffuseShader, p );
			quad.Draw();
			unbindTextureUnits( 2 );
		}
		{
			ScopedFBOBinding fbo( classes[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( block.first, block.second, tw, th );
			ScopedShaderBinding shader( settleShader.GetGLID() );
			bindUnit( 0, classes[ 1 ].TextureID() );
			settleShader.Set( "Fresh", 0 );
			quad.Draw();
			unbindTextureUnits( 1 );
		}
	}

	int classOf[ 64 ];
	for( int y = 0; y < 8; ++y )
		for( int x = 0; x < 8; ++x )
			classOf[ y * 8 + x ] = dither::kKnuthClass[ y ][ x ];
	ScopedFBOBinding fbo( indices[ currentIndex ].GetGLID(), ScopedFBOBinding::RB_REVERT );
	glViewport( 0, 0, gridWidth, gridHeight );
	ScopedShaderBinding shader( gatherShader.GetGLID() );
	bindUnit( 0, classes[ 0 ].TextureID() );
	bindUnit( 1, grid.TextureID() );
	gatherShader.Set( "Classes", 0 );
	gatherShader.Set( "Grid", 1 );
	setInt2( gatherShader, "Tiles", tw, th );
	glUniform1iv( gatherShader.FindUniform( "ClassOf" ), 64, classOf );
	quad.Draw();
	unbindTextureUnits( 2 );
}

//---------------------------------------------------------------------------
double RamdacPlugin::cycleUnits( CycleSync sync, double dt )
{
	//Beats from the host's own bar when it sends one, counting its wraps, so
	//a step lands where the transport says; otherwise its tempo on our clock.
	double beats = dt * std::max( static_cast< double >( bpm ), 1.0 ) / 60.0;
	if( hostBeatSeen )
	{
		double d = cycleBar >= 0.0f ? static_cast< double >( barPhase ) - static_cast< double >( cycleBar ) : 0.0;
		if( d < -0.5 )
			d += 1.0;
		beats = 4.0 * std::max( d, 0.0 );
	}
	cycleBar = barPhase;
	switch( sync )
	{
	case CycleSync::Beat: return beats;
	case CycleSync::Bar: return beats / 4.0;
	default: return dt;
	}
}

//---------------------------------------------------------------------------
FFResult RamdacPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	if( source.Width == 0 || source.Height == 0 )
		return FF_FAIL;

	//The host's viewport and blend state, put back however this returns.
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// Time and the beat.
	//---------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	const double dt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow         = now;
	++frameCount;

	if( barPhase != lastBarPhase && lastBarPhase >= 0.0f )
		hostBeatSeen = true;
	lastBarPhase = barPhase;

	//---------------------------------------------------------------------
	// The controls.
	//---------------------------------------------------------------------
	refreshCustom();
	const int cell         = IntegerOf( PT_PIXEL_SIZE, params[ PT_PIXEL_SIZE ] );
	const GLsizei width    = static_cast< GLsizei >( source.Width );
	const GLsizei height   = static_cast< GLsizei >( source.Height );
	const GLsizei gw       = ( width + cell - 1 ) / cell;
	const GLsizei gh       = ( height + cell - 1 ) / cell;
	const auto paletteA    = optionOf< PaletteId >( params, PT_PALETTE );
	const auto paletteB    = optionOf< PaletteId >( params, PT_SWAP_TO );
	const auto ditherMode  = optionOf< Dither >( params, PT_DITHER );
	const auto order       = optionOf< Order >( params, PT_ORDER );
	const int dacBits      = DacBits( optionOf< Dac >( params, PT_DAC ) );
	const float amount     = std::clamp( params[ PT_DITHER_AMOUNT ], 0.0f, 1.0f );
	const double swap      = std::clamp( static_cast< double >( params[ PT_SWAP ] ), 0.0, 1.0 );
	const auto style       = optionOf< SwapStyle >( params, PT_SWAP_STYLE );
	const bool frozen      = params[ PT_FREEZE ] >= 0.5f && indicesValid;
	const bool adaptiveRun = !frozen && ( paletteA == PaletteId::Adaptive || paletteB == PaletteId::Adaptive );

	//---------------------------------------------------------------------
	// Every buffer before anything is bound for the frame: every
	// ffglex::Scoped* binding clears to 0 on exit rather than restoring, and
	// FFGLFBO::Initialise binds under one.
	//---------------------------------------------------------------------
	bool ok = ensureIndices( gw, gh, frozen );
	if( ok && !frozen )
		ok = grid.Ensure( gw, gh, GL_RGBA16F, PassBuffer::Sampling::Nearest );
	if( ok && adaptiveRun )
		ok = analysis.Ensure( std::min( gw, shaders::kAnalysisWidth ), std::min( gh, shaders::kAnalysisHeight ), GL_RGBA32F,
		                      PassBuffer::Sampling::Nearest );
	if( ok && !frozen && ditherMode == Dither::DotDiffusion )
		ok = classes[ 0 ].Ensure( 8 * TilesWide(), 8 * TilesHigh(), GL_RGBA16F, PassBuffer::Sampling::Nearest )
		     && classes[ 1 ].Ensure( 8 * TilesWide(), 8 * TilesHigh(), GL_RGBA16F, PassBuffer::Sampling::Nearest );
	if( !ok )
	{
		diag::error( "could not allocate the grid" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1-2. The grid, and from it the adaptive palette.
	//---------------------------------------------------------------------
	if( !frozen )
		drawGrid( source, cell );
	if( adaptiveRun )
		runAdaptive( IntegerOf( PT_COLOURS, params[ PT_COLOURS ] ), dt, dacBits );

	const Palette a = resolve( paletteA, dacBits, order );
	const Palette b = resolve( paletteB, dacBits, order );
	//The --swap negative: past the cut, the clip quantised to B afresh.
	const bool requantise = swapRequantise && style == SwapStyle::Cut && swap >= 0.5;

	//---------------------------------------------------------------------
	// 3. The indices.
	//---------------------------------------------------------------------
	if( !frozen )
	{
		lastQuantised = requantise ? b : a;
		if( ditherMode == Dither::DotDiffusion )
			drawDiffusion( lastQuantised, amount );
		else
			drawQuantise( lastQuantised, ditherMode, amount );
		indicesValid = true;
	}

	//---------------------------------------------------------------------
	// 4. The table, in double.
	//---------------------------------------------------------------------
	const auto sync   = optionOf< CycleSync >( params, PT_CYCLE_SYNC );
	const double rate = CycleRateFromParam( params[ PT_CYCLE_RATE ] );
	cycle.Advance( rate, cycleUnits( sync, dt ) );

	TableInputs in;
	in.a              = requantise ? b : a;
	in.b              = b;
	in.swap           = requantise ? 0.0 : swap;
	in.style          = style;
	in.cycle          = optionOf< CycleMode >( params, PT_CYCLE );
	in.start          = IntegerOf( PT_CYCLE_START, params[ PT_CYCLE_START ] );
	in.length         = IntegerOf( PT_CYCLE_LENGTH, params[ PT_CYCLE_LENGTH ] );
	in.blend          = optionOf< CycleBlend >( params, PT_CYCLE_BLEND );
	in.phase          = cycle.Phase();
	in.dacBits        = dacBits;
	in.reverseForTest = cycleReverse;
	//The --clock negative: the phase from the host's clock as a float, which
	//500,000 seconds in cannot tell one frame from the next.
	if( absolutePhase )
		in.phase = static_cast< double >( static_cast< float >( rate ) * static_cast< float >( now ) );
	lastTable = BuildTable( in );

	//---------------------------------------------------------------------
	// 5. The picture, into the host's framebuffer and viewport.
	//---------------------------------------------------------------------
	float table[ kMaxEntries * 3 ] = {};
	for( int i = 0; i < lastTable.count; ++i )
	{
		const Rgb8& c        = lastTable.entries[ static_cast< size_t >( i ) ];
		table[ i * 3 ]       = c.r / 255.0f;
		table[ i * 3 + 1 ]   = c.g / 255.0f;
		table[ i * 3 + 2 ]   = c.b / 255.0f;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( displayShader.GetGLID() );
		bindUnit( 0, indices[ currentIndex ].TextureID() );
		bindUnit( 1, source.Handle );
		displayShader.Set( "Indices", 0 );
		displayShader.Set( "InputTexture", 1 );
		setInt2( displayShader, "Size", width, height );
		setInt2( displayShader, "ViewOrigin", hostViewport[ 0 ], hostViewport[ 1 ] );
		displayShader.Set( "Cell", cell );
		displayShader.Set( "TableCount", std::max( lastTable.count, 1 ) );
		glUniform3fv( displayShader.FindUniform( "Table" ), kMaxEntries, table );
		displayShader.Set( "AlphaIndex", static_cast< int >( optionOf< AlphaMode >( params, PT_ALPHA ) ) );
		displayShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		quad.Draw();
		unbindTextureUnits( 2 );
		glActiveTexture( GL_TEXTURE0 );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult RamdacPlugin::DeInitGL()
{
	for( FFGLShader* s : { &gridShader, &analysisShader, &quantiseShader, &diffuseShader, &settleShader, &gatherShader, &displayShader,
	                       &resampleShader } )
		s->FreeGLResources();
	quad.Release();
	grid.Destroy();
	analysis.Destroy();
	indices[ 0 ].Destroy();
	indices[ 1 ].Destroy();
	classes[ 0 ].Destroy();
	classes[ 1 ].Destroy();
	if( maskTexture )
		glDeleteTextures( 1, &maskTexture );
	maskTexture  = 0;
	indicesValid = false;
	gridWidth = gridHeight = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult RamdacPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void RamdacPlugin::SetBeatInfo( float newBpm, float newBarPhase )
{
	CFFGLPlugin::SetBeatInfo( newBpm, newBarPhase );
	if( newBpm > 0.0f && std::isfinite( newBpm ) )
		bpm = newBpm;
	if( std::isfinite( newBarPhase ) )
		barPhase = newBarPhase - std::floor( newBarPhase );
}

char* RamdacPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	if( index == PT_CUSTOM )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		textReturn = customText;
		return const_cast< char* >( textReturn.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult RamdacPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	if( index == PT_CUSTOM )
	{
		const std::string incoming = value != nullptr ? value : "";
		std::lock_guard< std::mutex > lock( textMutex );
		if( incoming != customText )
		{
			customText  = incoming;
			customDirty = true;
		}
		return FF_SUCCESS;
	}
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult RamdacPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;
	if( index == PT_CUSTOM )
		return FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float RamdacPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_ABOUT_FIRST )
		return 0.0f;
	return params[ index ];
}

} // namespace ramdac
