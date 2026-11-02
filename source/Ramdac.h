#pragma once

#include "Adaptive.h"
#include "Clut.h"
#include "Controls.h"
#include "Dither.h"
#include "Palettes.h"
#include "PassBuffer.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

namespace ramdac
{
/**
    The plugin: an indexed framebuffer and its RAMDAC.

    Each frame the clip is averaged into a grid of cells, every cell is
    quantised to the index of a palette entry (Shaders.h's passes), and the
    picture is the colour table looked up by those indices. The table is
    rebuilt on the CPU every frame (Clut.h): swap, fade and cycle are writes to
    it, and the indices never hear about them. Freeze keeps the indices and
    lets the table go on. AGENTS.md says why each part is built as it is.
*/
class RamdacPlugin : public CFFGLPlugin
{
public:
	RamdacPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;
	/// The About line must accept its own default -- instantiateGL pushes
	/// every declared default back through the setters and deletes the
	/// instance if one fails, and CFFGLPlugin's is a stub that fails -- and
	/// Custom is a real text control.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these;
	// they exist so `rmtest` can measure what the shipping passes produced,
	// and show that its checks fail against a wrong model.
	//-------------------------------------------------------------------
	/// The harness DECLARES its clock unit rather than letting the plugin
	/// vote on it against the wall clock: it renders hundreds of frames a
	/// second, which the vote would read as milliseconds.
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}
	/// --swap's negative: a Cut that quantises the clip to B afresh.
	void SetSwapRequantiseForTest( bool on )
	{
		swapRequantise = on;
	}
	/// --cycle's negative: Forward and Reverse turned round.
	void SetCycleReverseForTest( bool on )
	{
		cycleReverse = on;
	}
	/// --clock's negative: the cycle's phase from the host's time as a float.
	void SetAbsolutePhaseForTest( bool on )
	{
		absolutePhase = on;
	}
	/// --adaptive's negative: a fresh median cut every frame, shown at once.
	void SetColdAdaptiveForTest( bool on )
	{
		adaptive.SetColdForTest( on );
	}
	/// --resize's negative: a new grid forgets the frozen indices.
	void SetResizeClearsForTest( bool on )
	{
		resizeClears = on;
	}
	/// --diffusion's negative: Knuth's weights swapped, 1 orthogonal and 2 diagonal.
	void SetKnuthSwappedForTest( bool on )
	{
		knuthSwapped = on;
	}

	/// The table the display pass was given last frame.
	const Table& LastTable() const
	{
		return lastTable;
	}
	/// The palette the picture was last quantised to, in the working order.
	const Palette& LastQuantised() const
	{
		return lastQuantised;
	}
	const Adaptive& AdaptiveState() const
	{
		return adaptive;
	}
	double CyclePhase() const
	{
		return cycle.Phase();
	}
	GLuint GridTexture() const
	{
		return grid.TextureID();
	}
	GLuint IndexTexture() const
	{
		return indices[ currentIndex ].TextureID();
	}
	GLuint ClassTexture() const
	{
		return classes[ 0 ].TextureID();
	}
	int GridWidth() const
	{
		return gridWidth;
	}
	int GridHeight() const
	{
		return gridHeight;
	}
	/// A class block's size, in tiles.
	int TilesWide() const
	{
		return ( gridWidth + 7 ) / 8;
	}
	int TilesHigh() const
	{
		return ( gridHeight + 7 ) / 8;
	}

private:
	void UpdateClock();
	void refreshCustom();
	Palette resolve( PaletteId id, int dacBits, Order order ) const;
	bool ensureIndices( GLsizei gw, GLsizei gh, bool frozen );
	void uploadPalette( ffglex::FFGLShader& shader, const Palette& p );
	void drawGrid( const FFGLTextureStruct& source, int cell );
	bool runAdaptive( int colours, double dt, int dacBits );
	void drawQuantise( const Palette& p, Dither dither, float amount );
	void drawDiffusion( const Palette& p, float amount );
	double cycleUnits( CycleSync sync, double dt );

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader gridShader;
	ffglex::FFGLShader analysisShader;
	ffglex::FFGLShader quantiseShader;
	ffglex::FFGLShader diffuseShader;
	ffglex::FFGLShader settleShader;
	ffglex::FFGLShader gatherShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLShader resampleShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer grid;          ///< RGBA16F, a texel a cell: linear light and coverage
	PassBuffer analysis;      ///< RGBA32F, at most 96x54: the adaptive palette's samples
	PassBuffer indices[ 2 ];  ///< RGBA16F, a texel a cell: the index and coverage; two only so a resize under Freeze can resample
	PassBuffer classes[ 2 ];  ///< RGBA16F, Dot Diffusion's 64 blocks: decided so far, and the class being decided
	int currentIndex   = 0;
	bool indicesValid  = false;
	GLsizei gridWidth  = 0;
	GLsizei gridHeight = 0;
	GLuint maskTexture = 0;   ///< the blue-noise mask, R32F 64x64
	std::vector< float > readback;

	Adaptive adaptive;
	CycleClock cycle;
	Table lastTable;
	Palette lastQuantised;
	std::array< dither::ClassPlan, dither::kClasses > plan {};
	std::array< dither::ClassPlan, dither::kClasses > swappedPlan {};

	//-------------------------------------------------------------------
	// Custom: typed on the host's thread, parsed on the render thread.
	//-------------------------------------------------------------------
	std::mutex textMutex;
	std::string customText;
	std::string textReturn;
	bool customDirty = true;
	Palette customPalette;

	//-------------------------------------------------------------------
	// Time. See rosette: the host's clock unit is voted on against the wall
	// clock, because Resolume has sent both seconds and milliseconds.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	bool settledJump    = false;
	long frameCount     = 0;

	//-------------------------------------------------------------------
	// The beat, as SetBeatInfo last said.
	//-------------------------------------------------------------------
	float bpm          = 120.0f;
	float barPhase     = 0.0f;
	float lastBarPhase = -1.0f;
	bool hostBeatSeen  = false;
	float cycleBar     = -1.0f;///< the bar phase the cycle last counted from

	int hooks           = 0;
	bool swapRequantise = false;
	bool cycleReverse   = false;
	bool absolutePhase  = false;
	bool resizeClears   = false;
	bool knuthSwapped   = false;
};

} // namespace ramdac
