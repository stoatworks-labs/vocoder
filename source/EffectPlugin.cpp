/**
	The FF_EFFECT registration, and nothing else.

	**This file is listed directly in the effect target, not in the shared
	object library.** `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in a static archive
	the linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.

	    nm -gU Vocoder.bundle/Contents/MacOS/Vocoder | grep plugMain

	That is also why the shared code is an OBJECT library rather than a STATIC
	one, and it is why `oxbow probe` is the check that proves a bundle
	actually registers anything.

	`VC01`: four characters, unique across the fleet. The display name is
	`SW Vocoder`, ten characters against a field of sixteen that the host
	truncates silently.
*/
#include "Vocoder.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Vocoder >,                                 // Create method
	"VC01",                                                   // Plugin unique ID of maximum length 4
	"SW Vocoder",                                             // Plugin name
	2,                                                        // API major version number
	1,                                                        // API minor version number
	0,                                                        // Plugin major version number
	1,                                                        // Plugin minor version number
	FF_EFFECT,                                                // Plugin type
	"A channel vocoder with the picture as the carrier. The frame is split into eight octave bands of "
	"spatial frequency, from one-pixel detail to 128-pixel shapes, plus a residual; each band gets a gain; "
	"the bands are summed back. With everything at 1x the picture comes back exactly.\n\n"
	"Without audio it is a graphic EQ for detail: cut the 4 px band and detail at that scale alone goes, "
	"boost it and the picture rings at 4 px, tilt it and it sharpens or softens. Turn the Residual down "
	"and what is left is the picture as its detail bands.\n\n"
	"Route audio to it and the eight audio bands drive the eight picture bands: bass pumps the big shapes "
	"and treble sparkles the fine detail, or the reverse by the Mapping switch. Drive is how far, Floor is "
	"what a silent band does, Attack and Release are the followers.\n\n"
	"Band sliders are 0 to 4x with 1x at a quarter.",// Plugin description
	"Vocoder FFGL effect"                                     // About
);

extern "C" const char* VocoderEffectBuildStamp()
{
	return "vocoder " VOCODER_VERSION " effect, built " __DATE__ " " __TIME__;
}
