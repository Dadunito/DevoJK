/*
===========================================================================
DevoGI core - the lighting bake, shared by the DevoJK renderer and DevoQ3Map2

This is the public interface of devogi_core.cpp. The same two files are
compiled into the game's renderer (which bakes a map on its first load) and
into the DevoQ3Map2 compiler (which can bake a map ahead of time with
"-devogi"). Because both run exactly the same code with the same settings,
a .devogi file made by the compiler is accepted by the game as-is.

The core knows nothing about the game engine or the compiler. Everything it
needs from the outside world goes through a small "host" (dgHost below):
printing, a clock, loading images, finding shader text and showing progress.

If you change devogi_core.cpp or this header, copy both files to the other
project too (DevoJK: codemp/rd-vulkan/, DevoQ3Map2: src/), and bump
DEVOGI_VERSION in the .cpp when old .devogi files should be rebuilt.

Part of DevoJK by Devoted, licensed under GPLv2.
===========================================================================
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Every setting of the bake. The defaults here are the game's defaults: if a player
// never touched an r_devoGI_* setting, a bake made with these values matches theirs.
struct dgSettings {
	int		scale		= 4;		// lightmap resolution multiplier (1, 2, 4 or 8)
	int		rays		= 128;		// rays per bounce-light sample
	int		bounces		= 8;		// light bounces
	int		liveBounces	= 24;		// light bounces of the bounce-only light used by live lighting (1-16)
	float	aa			= 2.0f;		// shadow anti-aliasing width, in texels
	float	sheen		= 0.12f;	// baked glossy sheen
	float	intensity	= 1.0f;		// bounce light strength
	float	exposure	= 1.0f;		// overall brightness
	float	sky			= 0.5f;		// ambient light from visible sky
	float	saturation	= 1.6f;		// colour bleeding strength
	float	ao			= 1.0f;		// ambient occlusion on bounce light
	float	aoDistance	= 64.0f;	// ambient occlusion range in game units
	float	aoDirect	= 0.15f;	// contact darkening when a map can't be relit
	float	glow		= 4000.0f;	// starting strength of glowing lamps without q3map_surfacelight
	int		relight		= 1;		// trace direct light from the map's own lights
	int		entityLights = 1;		// use light entities left in the BSP
	int		threads		= 0;		// worker threads (0 = all CPU cores)
	int		gpu			= 1;		// trace the bake's rays on the GPU when there is one (same result, faster)
};

// What the core needs from whoever runs it (the renderer or the compiler).
struct dgHost {
	// prints one line of text (already formatted, ends with a newline)
	void		(*print)( const char *text );
	// a clock in milliseconds, for timing messages
	int			(*milliseconds)( void );
	// loads an image as 8-bit RGBA, name without extension; true on success
	bool		(*loadImage)( const char *name, uint8_t **rgba, int *width, int *height );
	// frees an image returned by loadImage
	void		(*freeImage)( uint8_t *rgba );
	// the text of a shader right after its name (starting at its "{"), or NULL.
	// Must pick the same definition the game does when several exist.
	const char	*(*shaderText)( const char *shaderName );
	// optional: progress of the bake, 0..1 overall. Called only from the thread that
	// called DG_Bake, about ten times a second (force = a new step just started).
	void		(*progress)( const char *mapName, const char *stage, float overall, bool force );
};

// Must be called once before anything else.
void		DG_SetHost( const dgHost *host );

/*
Light sources. Call DG_ExtractLights once per map (the BSP file in memory, header
first). It finds lamp shaders, glowing lamps, the sun and light entities.
*/
void		DG_ExtractLights( const char *mapName, const uint8_t *bsp, const dgSettings &settings );

struct dgLightInfo {
	float	origin[3];
	float	color[3];		// brightest channel = 1
	float	photons;		// strength
	float	radius;			// reach
};
int			DG_NumLights( void );
void		DG_GetLight( int index, dgLightInfo &out );
bool		DG_GetSun( float dir[3], float color[3], float *intensity );	// false: no sun
const char	*DG_LightsMapName( void );

/*
The bake. DG_ChooseScale picks the real resolution (r_devoGI_scale limited by
what the GPU can hold, maxScale). The two hashes are the cache key: the BSP
itself, and the settings + version + the text of the map's shaders (call
DG_ExtractLights first).
*/
/*
A light fixture for real-time lighting: neighbouring glowing patches of one lamp
shader merged into one lamp, or a light entity. Its strength is already
calibrated and exposed, so a renderer reproduces the bake's direct light with

    light = rgb * cosR * cosE / max( distance^2, 16 )

where cosR is the angle at the lit surface, and cosE the angle at the lamp
(-dot( normal, toSurface ), or 1 for an omni light with a zero normal). A lamp
with halfU/halfV > 0 is a glowing rectangle (axisU and normal x axisU span it),
which is what makes its shadows soft. Past "reach" it is too weak to matter.
*/
struct dgFixture {
	float	origin[3];
	float	normal[3];		// facing of the glowing surface; 0 0 0 = shines everywhere
	float	axisU[3];		// the rectangle's first edge direction
	float	halfU, halfV;	// half the rectangle's size (0 = a point)
	float	rgb[3];			// calibrated strength per colour channel
	float	reach;			// distance where it stops mattering
	int32_t	shader;			// BSP shader index it comes from, -1 for a light entity
};

/*
Everything a renderer needs to light a map live instead of using the full bake:
direct light comes from the fixtures and the sun, bounce light from these
lightmaps (surfaces) and probes (models and players).
*/
struct dgLiveLighting {
	bool					relit = false;		// false: the map couldn't be relit, use the full bake
	// bounce light only, RGBA: RGB linear in lightmap units (same layout as the
	// lightmaps, at bouncePageSize pixels per page - bounce light is smooth), and
	// A = how much of the sun each spot sees (0..255), the sun's baked shadow
	int						bouncePageSize = 0;
	std::vector<uint8_t>	bounce;
	// a grid of light probes: bounce light arriving from 6 directions (an "ambient cube":
	// +X -X +Y -Y +Z -Z, RGB each, half floats). For a normal n the light is
	//   n.x^2 * cube[n.x > 0 ? +X : -X] + n.y^2 * cube[..Y] + n.z^2 * cube[..Z]
	float					probeOrigin[3] = { 0, 0, 0 };	// position of probe (0,0,0)
	float					probeSpacing = 64.0f;
	int						probeDims[3] = { 0, 0, 0 };		// probes along x, y, z (x fastest)
	std::vector<uint16_t>	probes;				// 18 half floats per probe
	std::vector<uint8_t>	probeValid;			// 0 = inside a wall (filled from neighbours)
	std::vector<uint8_t>	probeSun;			// how much of the sun each probe sees, 0..255
	std::vector<uint16_t>	probesDirect;		// the direct light (lamps, sun) the same way: 18 half floats
												// per probe; bounce + direct = all the light, for models
	std::vector<dgFixture>	fixtures;
	bool					hasSun = false;
	float					sunDir[3] = { 0, 0, 1 };	// towards the sun
	float					sunRgb[3] = { 0, 0, 0 };	// calibrated: light = sunRgb * cosR when lit
};

/*
A surface that got its own spot in the lightmaps. The original compiler lets
surfaces whose old light came out identical share one spot in a lightmap page
(two stair steps, two bench ends). DevoGI lights every surface on its own, so
each sharing surface (after the first) moves to a new page added after the
BSP's own pages. A renderer applies these moves to the loaded BSP before it
reads the surfaces (DG_ApplyLightmapMoves).
*/
struct dgLightmapMove {
	int32_t	surf;			// BSP surface number
	int32_t	page;			// its new lightmap page
	int32_t	dx, dy;			// how far its lightmap coordinates shift, in pixels of a 128x128 page
};

struct dgResult {
	int						pageSize = 128;		// pixels per side of each lightmap page
	std::vector<uint8_t>	lightmaps;			// all pages, RGB: the BSP's pages, then any new ones
	std::vector<uint8_t>	vertexColors;		// RGBA per BSP draw vertex (may be empty)
	std::vector<dgLightmapMove>	lightmapMoves;	// surfaces moved onto the new pages
	dgLiveLighting			live;				// the same lighting split for real-time use
};

// number of lightmap pages in a result (the BSP's own plus the new ones)
int			DG_NumPages( const dgResult &result );
// moves surfaces to their own lightmap spots (see dgLightmapMove), in a BSP loaded in memory
void		DG_ApplyLightmapMoves( uint8_t *bsp, const std::vector<dgLightmapMove> &moves );

int			DG_ChooseScale( const dgSettings &settings, int maxScale );
uint64_t	DG_BspHash( const uint8_t *bsp );
uint64_t	DG_SettingsHash( const dgSettings &settings, int scale );
bool		DG_Bake( const char *mapName, const uint8_t *bsp, const dgSettings &settings, int scale, dgResult &result );

/*
The engine's tokenizer (COM_ParseExt): skips spaces and comments, handles quoted
strings, and with allowLineBreaks = false stops at the end of a line (empty token).
For hosts that index shader scripts themselves, so they split them exactly like the
game does. Returns a buffer that is reused by the next call on the same thread.
*/
const char	*DG_ParseToken( const char **text, bool allowLineBreaks );

/*
The .devogi cache file: a small header, the lightmap pages, the vertex colours,
then tagged sections with the live-lighting split (bounce lightmaps, probes, lamps).
DG_ReadCache only accepts a file made for exactly this BSP, settings and size.
*/
std::vector<uint8_t>	DG_WriteCache( uint64_t bspHash, uint64_t settingsHash, const dgResult &result );
bool					DG_ReadCache( const uint8_t *data, size_t length, const uint8_t *bsp,
							uint64_t bspHash, uint64_t settingsHash, int scale, dgResult &result );
