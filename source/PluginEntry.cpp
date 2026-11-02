#include "Ramdac.h"

/**
    The one registration.

    This file is listed directly in the Ramdac MODULE target, not in
    ramdac_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC
    archive the linker is entitled to drop the whole translation unit --
    giving a bundle that loads, exports `plugMain`, and reports that it
    contains no plugins. The core stays an OBJECT library for the same reason.

    The name is `SW Ramdac`, nine characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so. `oxbow probe` is what reads it back the way a host does.
*/
namespace
{
class RamdacEffect : public ramdac::RamdacPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< RamdacEffect >,// Create method
	"RM01",                       // Plugin unique ID of maximum length 4
	"SW Ramdac",                  // Plugin name
	2,                            // API major version number
	1,                            // API minor version number
	0,                            // Plugin major version number
	1,                            // Plugin minor version number
	FF_EFFECT,                    // Plugin type
	"Indexed colour: the clip as a palette's indices, and the palette as a table the indices look up.\n\nEvery pixel "
	"becomes one of 8 or 16 colours -- a machine's (CGA, EGA, C64, Spectrum, CPC, NES, Game Boy, PICO-8, teletext, "
	"Workbench), your own, or chosen from the clip -- nearest in OKLab, dithered by Bayer, blue noise or Knuth's dot "
	"diffusion. Then the table changes and the indices do not: a swap to another palette keeps every edge and every "
	"dither dot where it was, a fade goes index by index, and colour cycling rotates a range of the table as Deluxe "
	"Paint did, so water runs and fire flickers in a picture that never moves.",
	"Ramdac FFGL effect"// About
);

extern "C" const char* RamdacBuildStamp()
{
	return "ramdac " RAMDAC_VERSION ", built " __DATE__ " " __TIME__;
}
