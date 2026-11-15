/*
===========================================================================
DevoJK - DevoGI and DevoLights, the renderer side
Baked global illumination and relighting for Jedi Academy maps

Hello! The bake itself lives in devogi_core.cpp, which knows nothing about
the game: the very same file is also built into the DevoQ3Map2 compiler, so
mappers and players can bake a map ahead of time ("-devogi") and get a
.devogi file that the game accepts as if it had baked it itself.

This file is the glue between that core and the renderer:

  - the r_devoGI_* and r_devoLights_* settings (cvars), turned into the
    core's dgSettings,
  - the "host": printing to the console, loading images with the
    renderer's own loader, finding shader text the way the renderer does,
    and drawing the progress screen while a map bakes,
  - the cache file maps/<name>.devogi: read it if it matches, otherwise
    bake and write it,
  - copying the new vertex colours into the map data,
  - the live light preview (r_devoLights 1) and the "devolights" command.

See tr_local.h for where the renderer calls into this file.

This file is part of DevoJK by Devoted, licensed under GPLv2.
===========================================================================
*/

#include "tr_local.h"
#include "devogi_core.h"

#include <algorithm>
#include <vector>

/*
===========================================================================
Settings (cvars)

All r_devoGI_* settings marked LATCH take effect on the next map load; any
change makes that map bake again, because the settings are part of the
cache key. DevoQ3Map2's -devogi options have the same names and defaults.
===========================================================================
*/

static cvar_t *r_devoGI;
static cvar_t *r_devoGI_rays;
static cvar_t *r_devoGI_bounces;
static cvar_t *r_devoGI_intensity;
static cvar_t *r_devoGI_sky;
static cvar_t *r_devoGI_entityLights;
static cvar_t *r_devoGI_threads;
static cvar_t *r_devoGI_rebuild;
static cvar_t *r_devoGI_ao;
static cvar_t *r_devoGI_aoDirect;
static cvar_t *r_devoGI_aoDistance;
static cvar_t *r_devoGI_saturation;
static cvar_t *r_devoGI_scale;
static cvar_t *r_devoGI_relight;
static cvar_t *r_devoGI_exposure;
static cvar_t *r_devoGI_progress;
static cvar_t *r_devoGI_sheen;
static cvar_t *r_devoGI_aa;
static cvar_t *r_devoLights_glow;
static cvar_t *r_devoLights;
static cvar_t *r_devoLights_max;
static cvar_t *r_devoLights_intensity;
static cvar_t *r_devoLights_range;
static cvar_t *r_devoGI_live;
static cvar_t *r_devoGI_gpu;
static cvar_t *r_devoGI_liveBounces;
static cvar_t *r_devoGI_liveSky;
static cvar_t *r_devoGI_sunShadow;
static cvar_t *r_devoGI_sunShadowSize;
static cvar_t *r_devoGI_sunShadowRange;
static cvar_t *r_devoGI_sunShadowRangeFar;
static cvar_t *r_devoGI_lampShadows;
static cvar_t *r_devoGI_prebake;
static cvar_t *r_devoGI_prebakeSet;
static cvar_t *r_devoGI_lampShadowSize;
static cvar_t *r_devoGI_lampShadowRange;

void R_DevoGI_Register( void )
{
	r_devoGI				= ri.Cvar_Get( "r_devoGI",				"1",	CVAR_ARCHIVE | CVAR_LATCH, "Baked global illumination: maps get new lighting on their first load, cached in maps/<name>.devogi\n 0: off\n 1: on" );
	r_devoGI_scale			= ri.Cvar_Get( "r_devoGI_scale",		"12",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI lightmap resolution multiplier (1 to 16, the same for every surface). Higher gives sharper shadows and corners but bakes slower and uses more memory" );
	r_devoGI_rays			= ri.Cvar_Get( "r_devoGI_rays",			"128",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI rays per bounce-light sample (8-1024). More is smoother but bakes slower" );
	r_devoGI_bounces		= ri.Cvar_Get( "r_devoGI_bounces",		"16",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI light bounces (1-16)" );
	r_devoGI_aa				= ri.Cvar_Get( "r_devoGI_aa",			"2.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI shadow anti-aliasing: width of the area each texel's shadow samples come from, in texels (0.5 = sharp, 2 = smooth, up to 4)" );
	r_devoGI_sheen			= ri.Cvar_Get( "r_devoGI_sheen",		"0.12",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI baked sheen: every surface faintly picks up the faces right in front of it, like a slightly glossy finish (0 = off, up to 0.5)" );
	r_devoGI_intensity		= ri.Cvar_Get( "r_devoGI_intensity",	"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI bounce light strength" );
	r_devoGI_exposure		= ri.Cvar_Get( "r_devoGI_exposure",		"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI overall brightness (1 = match the original map's brightness)" );
	r_devoGI_sky			= ri.Cvar_Get( "r_devoGI_sky",			"0.5",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI soft ambient light from visible sky (0 = none)" );
	r_devoGI_saturation		= ri.Cvar_Get( "r_devoGI_saturation",	"1.6",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI colour bleeding strength: saturation of light reflected off surfaces (1 = texture average colour)" );
	r_devoGI_ao				= ri.Cvar_Get( "r_devoGI_ao",			"1.6",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient occlusion on bounce light: keeps corners and crevices dark (0 = off, 2 = full)" );
	r_devoGI_aoDistance		= ri.Cvar_Get( "r_devoGI_aoDistance",	"256",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient occlusion range in game units (wider = softer)" );
	r_devoGI_aoDirect		= ri.Cvar_Get( "r_devoGI_aoDirect",		"0.15",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI contact darkening of the old lightmap, only used when a map cannot be relit (0 = off, 1 = full)" );
	r_devoGI_relight		= ri.Cvar_Get( "r_devoGI_relight",		"1",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI: trace the direct light again from the map's own lights, with soft shadows, replacing the old lightmap (only its brightness is used as a reference)\n 0: keep the old lightmap's direct light" );
	r_devoGI_entityLights	= ri.Cvar_Get( "r_devoGI_entityLights",	"1",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI uses light entities kept in the BSP as light sources" );
	r_devoGI_threads		= ri.Cvar_Get( "r_devoGI_threads",		"0",	CVAR_ARCHIVE, "DevoGI bake threads (0 = all CPU cores)" );
	r_devoGI_progress		= ri.Cvar_Get( "r_devoGI_progress",		"1",	CVAR_ARCHIVE, "DevoGI: show a progress screen with a percentage while a map bakes" );
	r_devoGI_live			= ri.Cvar_Get( "r_devoGI_live",			"0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI performance mode\n 0: low spec - everything baked, fastest\n 1: high spec - live sun and light sources with real-time shadows, only bounce light is baked" );
	r_devoGI_prebake		= ri.Cvar_Get( "r_devoGI_prebake",		"1",	CVAR_ARCHIVE, "DevoGI: bake a map's lighting in the game when it has none yet (0 = never bake in the game, use the Precompile tool; maps without a baked file keep their original lighting)" );
	r_devoGI_prebakeSet		= ri.Cvar_Get( "r_devoGI_prebakeSet",	"0",	CVAR_ARCHIVE, "DevoGI: which versions to bake when a map loads\n 0: only the one the current performance mode needs\n 1: high spec too\n 2: low spec too\n 3: both, so switching mode never waits" );
	r_devoGI_sunShadow		= ri.Cvar_Get( "r_devoGI_sunShadow",	"1",	CVAR_ARCHIVE, "DevoGI live lighting: real-time sun shadows from everything, players and moving objects included (0 = only the baked sun shadows)" );
	r_devoGI_sunShadowSize	= ri.Cvar_Get( "r_devoGI_sunShadowSize",	"2048",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI live lighting: sun shadow map resolution (1024, 2048 or 4096 - sharper but slower)" );
	r_devoGI_sunShadowRange	= ri.Cvar_Get( "r_devoGI_sunShadowRange",	"768",	CVAR_ARCHIVE, "DevoGI live lighting: reach of the sharp sun shadow map around the player, in game units (256-2048; the coarser one takes over further away)" );
	r_devoGI_sunShadowRangeFar = ri.Cvar_Get( "r_devoGI_sunShadowRangeFar", "4096", CVAR_ARCHIVE, "DevoGI live lighting: reach of the second, coarser sun shadow map, in game units (up to 8192; beyond it the baked shadows take over)" );
	r_devoGI_lampShadows	= ri.Cvar_Get( "r_devoGI_lampShadows",	"4",	CVAR_ARCHIVE, "DevoGI live lighting: how many light sources right by the player (the map's lights, sabers, blaster bolts) throw the shadows of moving things - players, weapons, doors (0 = none, up to 4)" );
	r_devoGI_lampShadowSize	= ri.Cvar_Get( "r_devoGI_lampShadowSize",	"512",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI live lighting: resolution of each near-light shadow view (256, 512 or 1024)" );
	r_devoGI_lampShadowRange = ri.Cvar_Get( "r_devoGI_lampShadowRange", "1024", CVAR_ARCHIVE, "DevoGI live lighting: lights closer to the camera than this (game units) may throw moving things' shadows" );
	r_devoGI_liveBounces	= ri.Cvar_Get( "r_devoGI_liveBounces",	"24",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI: light bounces of the bounce-only light that live lighting (r_devoGI_live 1) uses (1-32); the full bake keeps r_devoGI_bounces" );
	r_devoGI_liveSky		= ri.Cvar_Get( "r_devoGI_liveSky",		"6",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI: sky light in the live layer (r_devoGI_live 1), times r_devoGI_sky: with no sun baked in, the sky all around lights the shade" );
	r_devoGI_gpu			= ri.Cvar_Get( "r_devoGI_gpu",			"1",	CVAR_ARCHIVE, "DevoGI: trace the bake's rays on the graphics card (much faster, same result; falls back to the CPU when it can't)" );
	r_devoGI_rebuild		= ri.Cvar_Get( "r_devoGI_rebuild",		"0",	CVAR_TEMP, "Set to 1 to force DevoGI to bake the next map that loads again" );
	r_devoLights_glow		= ri.Cvar_Get( "r_devoLights_glow",		"4000",	CVAR_ARCHIVE | CVAR_LATCH, "DevoLights: starting strength of glowing light-source textures without q3map_surfacelight (the bake then matches each lamp type to the original map); 0 = they give no light" );
	r_devoLights			= ri.Cvar_Get( "r_devoLights",			"0",	CVAR_ARCHIVE, "DevoLights: preview the map's own lights as live dynamic lights (no shadows yet)\n 0: off\n 1: on" );
	r_devoLights_max		= ri.Cvar_Get( "r_devoLights_max",		"48",	CVAR_ARCHIVE, "DevoLights preview: most map lights used per frame (the nearest and brightest visible ones)" );
	r_devoLights_intensity	= ri.Cvar_Get( "r_devoLights_intensity","1.0",	CVAR_ARCHIVE, "DevoLights preview: brightness of map lights" );
	r_devoLights_range		= ri.Cvar_Get( "r_devoLights_range",	"1.0",	CVAR_ARCHIVE, "DevoLights preview: reach of map lights" );
}

// the cvars as the core's settings (one place, so nothing can be forgotten)
static dgSettings R_DevoGI_Settings( void )
{
	dgSettings s;
	s.scale			= r_devoGI_scale->integer;
	s.rays			= r_devoGI_rays->integer;
	s.bounces		= r_devoGI_bounces->integer;
	s.aa			= r_devoGI_aa->value;
	s.sheen			= r_devoGI_sheen->value;
	s.intensity		= r_devoGI_intensity->value;
	s.exposure		= r_devoGI_exposure->value;
	s.sky			= r_devoGI_sky->value;
	s.saturation	= r_devoGI_saturation->value;
	s.ao			= r_devoGI_ao->value;
	s.aoDistance	= r_devoGI_aoDistance->value;
	s.aoDirect		= r_devoGI_aoDirect->value;
	s.glow			= r_devoLights_glow->value;
	s.relight		= r_devoGI_relight->integer;
	s.entityLights	= r_devoGI_entityLights->integer;
	s.threads		= r_devoGI_threads->integer;
	s.gpu			= r_devoGI_gpu->integer;
	s.liveBounces	= r_devoGI_liveBounces->integer;
	s.liveSky		= r_devoGI_liveSky->value;
	s.liveOnly		= r_devoGI_live->integer ? 1 : 0;	// live lighting needs only the live layer
	return s;
}

/*
===========================================================================
The host: what the core gets from the renderer
===========================================================================
*/

static void Host_Print( const char *text ) {
	ri.Printf( PRINT_ALL, "%s", text );
}

static int Host_Milliseconds( void ) {
	return ri.Milliseconds();
}

// the renderer's own image loader: same files, same search order as the game uses
static bool Host_LoadImage( const char *name, uint8_t **rgba, int *width, int *height ) {
	byte *pic = NULL;
	int w = 0, h = 0;
	R_LoadImage( name, &pic, &w, &h );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic )
			Z_Free( pic );
		return false;
	}
	*rgba = pic;
	*width = w;
	*height = h;
	return true;
}

static void Host_FreeImage( uint8_t *rgba ) {
	Z_Free( rgba );
}

static const char *Host_ShaderText( const char *shaderName ) {
	return R_DevoGI_FindShaderText( shaderName );	// tr_shader.cpp
}

/*
The progress screen. The bake runs inside map loading, which would otherwise
freeze the screen, so the core calls this from the main thread about ten times
a second while its worker threads do the work: map name, current step, and a
bar with the overall percentage.
*/
static int gi_progressFont;

static void Host_Progress( const char *mapName, const char *stage, float overall, bool force ) {
	if ( !r_devoGI_progress->integer || !tr.registered || !tr.whiteShader )
		return;
	if ( !gi_progressFont )
		gi_progressFont = RE_RegisterFont( "ocr_a" );

	// a plain 640x480 virtual screen, like the game's own menus
	RE_BeginFrame( STEREO_CENTER );
	const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float frame[4] = { 0.35f, 0.35f, 0.4f, 1.0f };
	const float fill[4] = { 0.95f, 0.75f, 0.35f, 1.0f };
	const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const float grey[4] = { 0.7f, 0.7f, 0.7f, 1.0f };
	const qhandle_t white_h = tr.whiteShader->index;

	RE_SetColor( black );
	RE_StretchPic( 0, 0, 640, 480, 0, 0, 1, 1, white_h );
	RE_SetColor( frame );
	RE_StretchPic( 118, 238, 404, 20, 0, 0, 1, 1, white_h );
	RE_SetColor( black );
	RE_StretchPic( 120, 240, 400, 16, 0, 0, 1, 1, white_h );
	RE_SetColor( fill );
	RE_StretchPic( 120, 240, 400.0f * overall, 16, 0, 0, 1, 1, white_h );
	RE_SetColor( NULL );

	if ( gi_progressFont ) {
		RE_Font_DrawString( 120, 196, va( "Baking lighting: %s", mapName ), white, gi_progressFont, -1, 1.0f );
		RE_Font_DrawString( 120, 216, va( "%s", stage ), grey, gi_progressFont, -1, 0.8f );
		RE_Font_DrawString( 480, 216, va( "%3d%%", (int)( overall * 100.0f ) ), white, gi_progressFont, -1, 0.8f );
		RE_Font_DrawString( 120, 268, "First load only - the result is saved for next time.", grey, gi_progressFont, -1, 0.6f );
	}

	int frontEnd = 0, backEnd = 0;
	RE_EndFrame( &frontEnd, &backEnd );
}

static const dgHost gi_host = {
	Host_Print, Host_Milliseconds, Host_LoadImage, Host_FreeImage, Host_ShaderText, Host_Progress
};

/*
===========================================================================
R_DevoGI_Process - called from R_LoadLightmaps

Returns a Z_Malloc'd lightmap lump in the BSP's page layout but with pages
of *outPageSize (128 * scale) pixels, or NULL to keep the original
lightmaps. maxScale is the largest page growth the GPU can hold.

Cache file maps/<name>.devogi (in the home path, or shipped next to the map):
the BSP hash covers every lump, the settings hash covers the bake version,
every setting and the text of the map's shaders. If either differs, or the
file is missing, the map is baked again and the file rewritten.
===========================================================================
*/

// new vertex colours, from the bake or the cache, until R_DevoGI_PatchVertexColors
static std::vector<byte> gi_vertexColors;
// surfaces that get their own lightmap spot, also applied in R_DevoGI_PatchVertexColors
static std::vector<dgLightmapMove> gi_lightmapMoves;
// DevoGI lit this map: its faceted round walls get smooth normals too (R_DevoGI_PatchVertexColors)
static bool gi_smoothMapNormals;

/*
Real-time lighting (r_devoGI_live 1). The bake's light split is kept here for the
whole map: the lamps and the sun are handed to the GPU every frame (see
R_DevoGI_FillLights), models read the light probes, and the lightmaps the GPU
gets hold bounce light only, with the sun's baked shadow in their alpha channel.
*/
static bool					gi_liveActive;
static dgLiveLighting		gi_live;
static std::vector<byte>	gi_liveAlpha;		// sun visibility per bounce-lightmap pixel
static int					gi_livePageSize;

// the sun's real-time shadow maps, two cascades (see R_DevoGI_SunShadowView)
static float				gi_shadowMvp[2][16];	// world -> each cascade, as last drawn
static float				gi_shadowTexelWorld[2];	// size of one shadow-map pixel, in game units
static bool					gi_shadowValid[2];

/*
Near-lamp shadows, this frame: the lights chosen for them (R_DevoGI_LampShadowViews,
front end) and, per cube side, the shadow-map tile it was drawn into (-1 = nothing
moving on that side, so it wasn't drawn). The backend fills in each tile's matrix
as it draws it (R_DevoGI_SunShadowDrawn) and the light list picks them up.
*/
struct giLampShadow {
	vec3_t	origin;		// where the light is (matches its entry in the light list)
	float	reach;
	int		tile[6];	// +X -X +Y -Y +Z -Z
};
static giLampShadow			gi_lampShadow[DEVO_LAMP_SHADOW_MAX];
static int					gi_numLampShadows;
static float				gi_lampShadowMvp[DEVO_LAMP_SHADOW_MAX * 6][16];
static bool					gi_lampShadowValid[DEVO_LAMP_SHADOW_MAX * 6];
static const float			gi_lampShadowFov = 96.0f;	// a little over 90: the sides overlap,
														// so soft-edge lookups never run off a tile

// lightmap bytes -> the GPU's lightmap values (the map's overbright shift, as R_LoadLightmaps does)
static float R_DevoGI_Shift( void ) {
	return powf( 2.0f, (float)( r_mapOverBrightBits->integer - tr.overbrightBits ) );
}

/*
Maps made for rend2 (the modern OpenJK renderer) already bring their own,
better lighting: HDR lightmaps (maps/<name>/lm_0000.hdr), an HDR light grid
(lightgrid.raw), HDR vertex light (vertlight.raw) and reflection probes
(cubemaps/<name>/env.json). DevoGI on top of those would only fight them, so it
stays off for such maps - it is meant for the older maps that have none of this.
*/
static qboolean R_DevoGI_IsRend2Map( const char *mapName )
{
	char base[MAX_QPATH];
	COM_StripExtension( COM_SkipPath( (char *)mapName ), base, sizeof( base ) );
	static const char *patterns[] = {
		"maps/%s/lm_0000.hdr", "maps/%s/lm_0000.tga", "maps/%s/lightgrid.raw",
		"maps/%s/vertlight.raw", "cubemaps/%s/env.json",
	};
	for ( const char *pattern : patterns ) {
		char f[MAX_QPATH];
		Com_sprintf( f, sizeof( f ), pattern, base );
		if ( ri.FS_ReadFile( f, NULL ) > 0 ) {
			ri.Printf( PRINT_ALL, "DevoGI: %s brings its own rend2 lighting (%s), DevoGI stays off\n", base, f );
			return qtrue;
		}
	}
	return qfalse;
}

byte *R_DevoGI_Process( const char *mapName, const byte *fileBase, const dheader_t *header, int maxScale, int *outPageSize, int *outNumPages )
{
	*outPageSize = 128;
	*outNumPages = 0;
	gi_vertexColors.clear();
	gi_lightmapMoves.clear();
	gi_smoothMapNormals = false;
	gi_liveActive = false;
	gi_live = dgLiveLighting();
	gi_liveAlpha.clear();
	gi_shadowValid[0] = gi_shadowValid[1] = false;
	gi_numLampShadows = 0;
	if ( !r_devoGI || !r_devoGI->integer )
		return NULL;
	if ( R_DevoGI_IsRend2Map( mapName ) )
		return NULL;
	if ( header->lumps[LUMP_LIGHTMAPS].filelen < 128 * 128 * 3 )
		return NULL;	// maps with external lightmap images are not supported yet

	DG_SetHost( &gi_host );
	const dgSettings settings = R_DevoGI_Settings();
	const int scale = DG_ChooseScale( settings, maxScale );

	char base[MAX_QPATH], cacheName[MAX_QPATH];
	COM_StripExtension( mapName, base, sizeof( base ) );
	// the full bake and the live bake are different files, so switching r_devoGI_live
	// back and forth never has to bake again
	Com_sprintf( cacheName, sizeof( cacheName ), r_devoGI_live->integer ? "%s.live.devogi" : "%s.devogi", base );
	const uint64_t bspHash = DG_BspHash( fileBase );
	const uint64_t settingsHash = DG_SettingsHash( settings, scale );

	// ---- 0. prebake: also prepare the other performance mode's file, if asked to ----
	// (first, so the two bakes never sit in memory at the same time)
	const int prebakeSet = r_devoGI_prebakeSet->integer;
	const bool live = r_devoGI_live->integer != 0;
	const bool wantOther = r_devoGI_prebake->integer
		&& ( prebakeSet == 3 || ( prebakeSet == 1 && !live ) || ( prebakeSet == 2 && live ) );
	if ( wantOther ) {
		dgSettings other = settings;
		other.liveOnly = live ? 0 : 1;
		const int otherScale = DG_ChooseScale( other, maxScale );
		const uint64_t otherHash = DG_SettingsHash( other, otherScale );
		char otherName[MAX_QPATH];
		Com_sprintf( otherName, sizeof( otherName ), live ? "%s.devogi" : "%s.live.devogi", base );
		// read only the file's header to see whether it is still good
		bool upToDate = false;
		fileHandle_t f;
		if ( ri.FS_FOpenFileRead( otherName, &f, qfalse ) > 0 && f ) {
			std::vector<byte> head( DG_CacheHeaderSize() );
			const int got = ri.FS_Read( head.data(), (int)head.size(), f );
			ri.FS_FCloseFile( f );
			upToDate = got == (int)head.size() && DG_CacheHeaderMatches( head.data(), head.size(), DG_BspHash( fileBase ), otherHash );
		}
		if ( !upToDate ) {
			ri.Printf( PRINT_ALL, "DevoGI: prebaking %s for the other performance mode\n", otherName );
			dgResult otherResult;
			if ( DG_Bake( mapName, fileBase, other, otherScale, otherResult ) ) {
				const std::vector<byte> file = DG_WriteCache( DG_BspHash( fileBase ), otherHash, otherResult );
				ri.FS_WriteFile( otherName, file.data(), (int)file.size() );
				ri.Printf( PRINT_ALL, "DevoGI: saved %s\n", otherName );
			}
		}
	}

	dgResult result;
	bool haveResult = false;

	// ---- 1. is there a baked file for exactly this map and these settings? ----
	if ( !r_devoGI_rebuild->integer ) {
		void *cache = NULL;
		const int len = ri.FS_ReadFile( cacheName, &cache );
		if ( cache ) {
			haveResult = DG_ReadCache( (const byte *)cache, len > 0 ? (size_t)len : 0, fileBase, bspHash, settingsHash, scale, result );
			ri.FS_FreeFile( cache );
			if ( haveResult )
				ri.Printf( PRINT_ALL, "DevoGI: using baked lighting from %s (%dx%d lightmaps)\n", cacheName, result.pageSize, result.pageSize );
			else
				ri.Printf( PRINT_ALL, "DevoGI: %s is out of date (map, shaders or settings changed), baking again\n", cacheName );
		}
	}
	ri.Cvar_Set( "r_devoGI_rebuild", "0" );

	// ---- 2. no: bake it, and save the result for next time ----
	if ( !haveResult ) {
		if ( !r_devoGI_prebake->integer ) {
			ri.Printf( PRINT_ALL, "DevoGI: no baked lighting for %s and in-game baking is off (r_devoGI_prebake 0): original lighting\n", base );
			return NULL;
		}
		if ( !DG_Bake( mapName, fileBase, settings, scale, result ) )
			return NULL;
		const std::vector<byte> file = DG_WriteCache( bspHash, settingsHash, result );
		ri.FS_WriteFile( cacheName, file.data(), (int)file.size() );
		ri.Printf( PRINT_ALL, "DevoGI: saved %s\n", cacheName );
	}

	gi_vertexColors.swap( result.vertexColors );
	// the bake may have given some surfaces their own lightmap spot on new pages
	// (see dgLightmapMove): the renderer loads that many pages, and the surfaces
	// are moved before it builds them (R_DevoGI_PatchVertexColors)
	gi_lightmapMoves.swap( result.lightmapMoves );
	*outNumPages = DG_NumPages( result );
	gi_smoothMapNormals = true;

	// ---- 3a. real-time lighting: the GPU gets bounce light only, the lamps come live ----
	const int numPages = (int)( result.lightmaps.size() / ( (size_t)result.pageSize * result.pageSize * 3 ) );
	const int bs = result.live.bouncePageSize;
	// live light is drawn by the renderer's GPU light path (r_normalMapping/r_specularMapping
	// on, r_dlightMethod 1); without it the full bake is the only option
	const bool gpuLights = vk.useGPUDLight ? true : false;
	if ( r_devoGI_live->integer && !gpuLights )
		ri.Printf( PRINT_ALL, "DevoGI: real-time lighting needs r_normalMapping 1, r_specularMapping 1 and r_dlightMethod 1; using the full bake\n" );
	if ( r_devoGI_live->integer && gpuLights && result.live.relit && bs > 0
		&& result.live.bounce.size() == (size_t)numPages * bs * bs * 4 ) {
		const size_t pixels = (size_t)numPages * bs * bs;
		byte *out = (byte *)Z_Malloc( (int)( pixels * 3 ), TAG_BSP, qfalse );
		gi_liveAlpha.resize( pixels );
		for ( size_t i = 0; i < pixels; i++ ) {
			out[i * 3 + 0] = result.live.bounce[i * 4 + 0];
			out[i * 3 + 1] = result.live.bounce[i * 4 + 1];
			out[i * 3 + 2] = result.live.bounce[i * 4 + 2];
			gi_liveAlpha[i] = result.live.bounce[i * 4 + 3];
		}
		result.live.bounce.clear();
		result.live.bounce.shrink_to_fit();
		gi_live = std::move( result.live );
		gi_livePageSize = bs;
		gi_liveActive = true;
		ri.Printf( PRINT_ALL, "DevoGI: real-time lighting: %d lamps%s live, bounce light baked (%dx%d)\n",
			(int)gi_live.fixtures.size(), gi_live.hasSun ? " and the sun" : "", bs, bs );
		*outPageSize = bs;
		return out;
	}
	if ( r_devoGI_live->integer && gpuLights )
		ri.Printf( PRINT_ALL, "DevoGI: real-time lighting needs a relit map; using the full bake\n" );

	// ---- 3b. the full bake: hand the lightmaps to R_LoadLightmaps ----
	// (the light probes stay: models are lit from them, see R_DevoGI_EntityLighting)
	result.live.bounce.clear();
	result.live.bounce.shrink_to_fit();
	gi_live = std::move( result.live );
	byte *out = (byte *)Z_Malloc( (int)result.lightmaps.size(), TAG_BSP, qfalse );
	memcpy( out, result.lightmaps.data(), result.lightmaps.size() );
	*outPageSize = result.pageSize;
	return out;
}

/*
===========================================================================
Real-time lighting (r_devoGI_live 1)

Stage 2 of moving DevoGI to live light: the lamps and the sun are drawn by
the GPU every frame, per pixel, with the same strength the bake gave them,
while bounce light still comes from the bake. Lamp shadows come next; until
then the sun uses its baked shadow (the alpha channel of the lightmaps, and
the light probes for models).
===========================================================================
*/

// the sun's baked visibility for one lightmap pixel, 0..1 (1 when live lighting is off)
float R_DevoGI_LightmapAlpha( int page, int pixel )
{
	if ( !gi_liveActive || gi_liveAlpha.empty() )
		return 1.0f;
	const size_t i = (size_t)page * gi_livePageSize * gi_livePageSize + pixel;
	return i < gi_liveAlpha.size() ? gi_liveAlpha[i] / 255.0f : 1.0f;
}

qboolean R_DevoGI_LiveActive( void )
{
	return gi_liveActive ? qtrue : qfalse;
}

// a half float (from the probes) -> float
static float R_DevoGI_Half( uint16_t h )
{
	const uint32_t sign = ( h & 0x8000u ) << 16;
	int exp = ( h >> 10 ) & 0x1f;
	uint32_t mant = h & 0x3ffu;
	uint32_t bits;
	if ( exp == 0 ) {
		if ( !mant )
			bits = sign;
		else {	// a denormal: normalise it
			exp = 1;
			while ( !( mant & 0x400u ) ) { mant <<= 1; exp--; }
			mant &= 0x3ffu;
			bits = sign | ( (uint32_t)( exp + 112 ) << 23 ) | ( mant << 13 );
		}
	} else {
		bits = sign | ( (uint32_t)( exp + 112 ) << 23 ) | ( mant << 13 );
	}
	float f;
	memcpy( &f, &bits, 4 );
	return f;
}

/*
Light for a model (player, weapon, item) from the 8 light probes around it, so
models get exactly the light the walls around them got, instead of the map's
old light grid from q3map2. Each probe holds the light arriving from 6
directions; blended between the 8 probes, that "ambient cube" becomes the
game's model lighting: an ambient part (what the model's darkest side gets), a
directed part, and the direction it comes from.
  - full bake (r_devoGI_live 0): bounce + direct light (lamps, sun)
  - live lighting: bounce light only; lamps and the sun are added per pixel
Values are in the 0..255 range the renderer uses for model lighting. Returns
false when the map has no DevoGI light probes.
*/
qboolean R_DevoGI_EntityLighting( const vec3_t point, vec3_t ambient, vec3_t directed, vec3_t lightDir, float *sunVis )
{
	if ( gi_live.probes.empty() || gi_live.probeValid.empty() )
		return qfalse;
	const bool withDirect = !gi_liveActive && gi_live.probesDirect.size() == gi_live.probes.size();
	const int *dims = gi_live.probeDims;
	float f[3];
	int c0[3];
	for ( int k = 0; k < 3; k++ ) {
		const float g = ( point[k] - gi_live.probeOrigin[k] ) / gi_live.probeSpacing;
		c0[k] = Com_Clampi( 0, Q_max( 0, dims[k] - 2 ), (int)floorf( g ) );
		f[k] = Com_Clamp( 0.0f, 1.0f, g - c0[k] );
	}
	float cube[6][3] = {}, sun = 0, wsum = 0;
	for ( int corner = 0; corner < 8; corner++ ) {
		const int x = Q_min( dims[0] - 1, c0[0] + ( corner & 1 ) );
		const int y = Q_min( dims[1] - 1, c0[1] + ( ( corner >> 1 ) & 1 ) );
		const int z = Q_min( dims[2] - 1, c0[2] + ( corner >> 2 ) );
		float w = ( ( corner & 1 ) ? f[0] : 1 - f[0] ) * ( ( corner & 2 ) ? f[1] : 1 - f[1] ) * ( ( corner & 4 ) ? f[2] : 1 - f[2] );
		const size_t i = ( (size_t)z * dims[1] + y ) * dims[0] + x;
		if ( !gi_live.probeValid[i] )
			w *= 0.05f;		// a probe inside a wall only counts if nothing better is near
		for ( int a = 0; a < 6; a++ ) {
			for ( int k = 0; k < 3; k++ ) {
				float v = R_DevoGI_Half( gi_live.probes[i * 18 + a * 3 + k] );
				if ( withDirect )
					v += R_DevoGI_Half( gi_live.probesDirect[i * 18 + a * 3 + k] );
				cube[a][k] += w * v;
			}
		}
		sun += w * ( gi_live.probeSun.size() > i ? gi_live.probeSun[i] / 255.0f : 1.0f );
		wsum += w;
	}
	if ( wsum <= 0 )
		return qfalse;
	const float shift = R_DevoGI_Shift() / wsum;
	for ( int a = 0; a < 6; a++ )
		for ( int k = 0; k < 3; k++ )
			cube[a][k] *= shift;
	*sunVis = sun / wsum;

	if ( !withDirect ) {
		// live lighting: only the soft bounce light, the same from all sides
		for ( int k = 0; k < 3; k++ ) {
			ambient[k] = ( cube[0][k] + cube[1][k] + cube[2][k] + cube[3][k] + cube[4][k] + cube[5][k] ) / 6.0f;
			directed[k] = 0;
		}
		return qtrue;
	}

	// where most light comes from: the brightness difference between opposite sides
	auto luma = []( const float *c ) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
	vec3_t dir = { luma( cube[0] ) - luma( cube[1] ), luma( cube[2] ) - luma( cube[3] ), luma( cube[4] ) - luma( cube[5] ) };
	if ( VectorNormalize( dir ) < 1e-3f )
		VectorSet( dir, 0, 0, 1 );
	// the ambient cube seen from a direction n: sum of n.x^2 * (+X or -X side) and so on
	auto look = [&]( const vec3_t n, float out[3] ) {
		for ( int k = 0; k < 3; k++ ) {
			out[k] = n[0] * n[0] * cube[n[0] > 0 ? 0 : 1][k]
				   + n[1] * n[1] * cube[n[1] > 0 ? 2 : 3][k]
				   + n[2] * n[2] * cube[n[2] > 0 ? 4 : 5][k];
		}
	};
	vec3_t back = { -dir[0], -dir[1], -dir[2] };
	float lit[3], shade[3];
	look( dir, lit );
	look( back, shade );
	for ( int k = 0; k < 3; k++ ) {
		ambient[k] = shade[k];							// the dark side gets this
		directed[k] = Q_max( 0.0f, lit[k] - shade[k] );	// the lit side gets this on top
	}
	VectorCopy( dir, lightDir );
	return qtrue;
}

/*
The lamps for this frame. The GPU's light list holds 64 lights and the game's
own dynamic lights (blaster bolts, sabers) come first, so we pick the lamps
that matter most from where the camera is: in reach, in a part of the map the
camera can see, and bright for their distance. Each lamp fades out smoothly
before its reach, so lamps entering or leaving the list don't pop.
*/
/*
Real-time sun shadows. Once per frame the scene is drawn from the sun into a depth
image (see vk_devo_shadow.cpp), twice: a sharp "cascade" for the area around the
player and a coarser one that reaches across the whole map. Each view is
orthographic - parallel rays, like real sunlight - over a square in front of the
sun, and the square only ever moves in steps of exactly one shadow-map pixel, so
shadow edges stay perfectly still while the player walks around.
*/
qboolean R_DevoGI_SunShadowView( const viewParms_t *mainView, viewParms_t *out, int cascade )
{
	if ( !gi_liveActive || !gi_live.hasSun || !r_devoGI_sunShadow->integer || !vk_devoShadow.active
		|| ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) || cascade < 0 || cascade > 1 )
		return qfalse;

	// The shadow map has a fixed number of pixels, so the wider a cascade reaches, the
	// bigger each pixel gets. Both are kept in a range where a shadow pixel stays small
	// (a few game units at most at 2048): with a far cascade of 16000 one pixel was 16
	// units, thin casters vanished and shadows jumped as the player moved.
	const float nearRange = Com_Clamp( 256.0f, 2048.0f, r_devoGI_sunShadowRange->value );
	const float range = cascade == 0 ? nearRange
		: Com_Clamp( nearRange * 2.0f, 8192.0f, r_devoGI_sunShadowRangeFar->value );	// half the square's side
	const float distance = range + 4096.0f;		// the sun's camera sits this far away, above every caster
	const int size = (int)vk_devoShadow.size;

	// the sun camera's axes: forward points from the sun to the ground
	vec3_t forward, left, up, hint = { 0, 0, 1 };
	VectorScale( gi_live.sunDir, -1.0f, forward );
	if ( fabsf( forward[2] ) > 0.99f )
		VectorSet( hint, 1, 0, 0 );
	CrossProduct( hint, forward, left );
	VectorNormalize( left );
	CrossProduct( forward, left, up );

	// centre of the square: a little ahead of the player (level, not up or down where the
	// player looks), snapped to whole shadow-map pixels
	const float texel = 2.0f * range / size;
	vec3_t centre, ahead = { mainView->ori.axis[0][0], mainView->ori.axis[0][1], 0.0f };
	if ( VectorNormalize( ahead ) < 0.01f )
		VectorClear( ahead );
	VectorMA( mainView->ori.origin, range * 0.4f, ahead, centre );
	const float cx = floorf( DotProduct( centre, left ) / texel ) * texel;
	const float cy = floorf( DotProduct( centre, up ) / texel ) * texel;
	const float cz = DotProduct( centre, forward );
	VectorScale( left, cx, centre );
	VectorMA( centre, cy, up, centre );
	VectorMA( centre, cz, forward, centre );

	Com_Memset( out, 0, sizeof( *out ) );
	out->viewportX = cascade * size;	// both cascades side by side in one image, on top
	out->viewportY = (int)vk_devoShadow.height - size;	// (viewports count from the bottom)
	out->viewportWidth = out->viewportHeight = size;
	out->scissorX = out->viewportX;
	out->scissorY = out->viewportY;
	out->scissorWidth = out->scissorHeight = size;
	out->portalView = PV_NONE;
	out->fovX = out->fovY = 90.0f;		// unused: the projection is orthographic
	VectorMA( centre, -distance, forward, out->ori.origin );
	VectorCopy( forward, out->ori.axis[0] );
	VectorCopy( left, out->ori.axis[1] );
	VectorCopy( up, out->ori.axis[2] );
	// out in the void the camera sees no PVS, so nothing is hidden from the sun
	VectorCopy( out->ori.origin, out->pvsOrigin );
	out->zNear = 16.0f;
	out->devoShadow = cascade + 1;
	out->devoShadowRange = range;

	gi_shadowTexelWorld[cascade] = texel;
	return qtrue;
}

// the backend has drawn one of the sun's views: remember how to find a world position in it
void R_DevoGI_SunShadowDrawn( const viewParms_t *view )
{
	// the same transform the GPU used to draw the view (see get_mvp_transform)
	float proj[16];
	Com_Memcpy( proj, view->projectionMatrix, sizeof( proj ) );
	proj[5] = -proj[5];

	if ( DEVO_LAMP_VIEW( *view ) ) {	// a side of a near lamp's cube
		const int tile = view->devoShadow - 3;
		if ( tile >= DEVO_LAMP_SHADOW_MAX * 6 )
			return;
		myGlMultMatrix( view->world.modelViewMatrix, proj, gi_lampShadowMvp[tile] );
		gi_lampShadowValid[tile] = true;
		return;
	}

	const int cascade = view->devoShadow - 1;
	if ( cascade < 0 || cascade > 1 )
		return;
	myGlMultMatrix( view->world.modelViewMatrix, proj, gi_shadowMvp[cascade] );
	gi_shadowValid[cascade] = true;
}

/*
Near-lamp shadows (the third lamp tier). Cheap on purpose: only lights close to the
camera, only when something that moves - a player (the camera's own included), a
weapon, a door - is within their reach, and only the sides of the light's cube that
something moves in. Those views draw just the moving things, a few thousand
triangles. The level's shadows from the lamps are baked; these add the movers'.
Lights: the map's lamps that have a baked mask channel (their live light can take
the baked light back out where a mover blocks it) and the game's own lights -
sabers, blaster bolts. Fills in 'out' with the views to draw, returns how many.
*/
int R_DevoGI_LampShadowViews( viewParms_t *out, int maxViews )
{
	gi_numLampShadows = 0;
	Com_Memset( gi_lampShadowValid, 0, sizeof( gi_lampShadowValid ) );
	const int wanted = Com_Clampi( 0, DEVO_LAMP_SHADOW_MAX, r_devoGI_lampShadows->integer );
	if ( !gi_liveActive || !vk_devoShadow.active || !tr.world || wanted <= 0
		|| ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) )
		return 0;

	// the things that move: a sphere around each
	struct mover { vec3_t centre; float radius; };
	static std::vector<mover> movers;
	movers.clear();
	for ( int i = 0; i < tr.refdef.num_entities; i++ ) {
		const refEntity_t &e = tr.refdef.entities[i].e;
		if ( e.reType != RT_MODEL || ( e.renderfx & ( RF_FIRST_PERSON | RF_DEPTHHACK | RF_NOSHADOW ) ) )
			continue;
		mover m;
		const model_t *model = R_GetModelByHandle( e.hModel );
		if ( model && model->type == MOD_BRUSH && model->data.bmodel ) {
			const bmodel_t *bm = model->data.bmodel;
			vec3_t mid, half;
			VectorAdd( bm->bounds[0], bm->bounds[1], mid );
			VectorScale( mid, 0.5f, mid );
			VectorSubtract( bm->bounds[1], mid, half );
			VectorAdd( e.origin, mid, m.centre );
			m.radius = VectorLength( half );
		} else {
			// a player stands on its origin and is about 64 tall; weapons and items are smaller
			VectorCopy( e.origin, m.centre );
			m.centre[2] += 16.0f;
			m.radius = 48.0f;
		}
		movers.push_back( m );
	}
	if ( movers.empty() )
		return 0;

	// the lights that may throw shadows, nearest to the camera first
	struct candidate { float dist; vec3_t origin; float reach; };
	static std::vector<candidate> cand;
	cand.clear();
	const float range = Q_max( 64.0f, r_devoGI_lampShadowRange->value );
	auto consider = [&]( const vec3_t origin, float reach ) {
		const float dist = Distance( origin, tr.refdef.vieworg );
		if ( dist > range + reach || reach < 8.0f )
			return;
		if ( !R_inPVS( tr.refdef.vieworg, origin, NULL ) )
			return;
		candidate c;
		c.dist = dist;
		VectorCopy( origin, c.origin );
		c.reach = reach;
		cand.push_back( c );
	};
	for ( const dgFixture &fx : gi_live.fixtures ) {
		if ( gi_live.lampsBaked && fx.maskChannel < 0 )
			continue;	// a lamp kept baked only: it has no live light to shade
		consider( fx.origin, fx.reach );
	}
	for ( int i = 0; i < tr.refdef.num_dlights; i++ )
		consider( tr.refdef.dlights[i].origin, tr.refdef.dlights[i].radius );
	std::sort( cand.begin(), cand.end(), []( const candidate &a, const candidate &b ) { return a.dist < b.dist; } );

	static const vec3_t faceAxis[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	const float spread = tanf( DEG2RAD( gi_lampShadowFov * 0.5f ) );
	const int tile = (int)vk_devoShadow.lampTile;
	const int perRow = (int)vk_devoShadow.lampTilesPerRow;
	int views = 0;

	for ( const candidate &c : cand ) {
		if ( gi_numLampShadows >= wanted )
			break;
		// which sides of the light's cube have something moving in them
		int sides = 0;
		for ( const mover &m : movers ) {
			vec3_t d;
			VectorSubtract( m.centre, c.origin, d );
			if ( VectorLength( d ) > c.reach + m.radius )
				continue;
			for ( int f = 0; f < 6; f++ ) {
				const float *a = faceAxis[f];
				const float along = DotProduct( d, a );
				if ( along < -m.radius )
					continue;
				// the other two directions must fit inside the side's 96 degree view
				bool inside = true;
				for ( int k = 0; k < 3 && inside; k++ ) {
					if ( a[k] != 0.0f )
						continue;
					if ( fabsf( d[k] ) > Q_max( along, 0.0f ) * spread + m.radius * 1.42f )
						inside = false;
				}
				if ( inside )
					sides |= 1 << f;
			}
		}
		if ( !sides )
			continue;
		int needed = 0;
		for ( int f = 0; f < 6; f++ )
			needed += ( sides >> f ) & 1;
		if ( views + needed > maxViews )
			break;

		giLampShadow &ls = gi_lampShadow[gi_numLampShadows++];
		VectorCopy( c.origin, ls.origin );
		ls.reach = c.reach;
		for ( int f = 0; f < 6; f++ ) {
			ls.tile[f] = -1;
			if ( !( sides & ( 1 << f ) ) )
				continue;
			const int t = views;
			ls.tile[f] = t;

			viewParms_t &v = out[views++];
			Com_Memset( &v, 0, sizeof( v ) );
			// the tile, in the rows below the sun's cascades (viewports count from the bottom)
			const int x = ( t % perRow ) * tile;
			const int yTop = (int)vk_devoShadow.size + ( t / perRow ) * tile;
			v.viewportX = x;
			v.viewportY = (int)vk_devoShadow.height - yTop - tile;
			v.viewportWidth = v.viewportHeight = tile;
			v.scissorX = v.viewportX;
			v.scissorY = v.viewportY;
			v.scissorWidth = v.scissorHeight = tile;
			v.portalView = PV_NONE;
			v.fovX = v.fovY = gi_lampShadowFov;
			// looking out of the cube's side: forward, left and up as the game uses them
			vec3_t up = { 0, 0, 1 };
			if ( f >= 4 )
				VectorSet( up, 1, 0, 0 );
			VectorCopy( c.origin, v.ori.origin );
			VectorCopy( faceAxis[f], v.ori.axis[0] );
			CrossProduct( up, v.ori.axis[0], v.ori.axis[1] );
			CrossProduct( v.ori.axis[0], v.ori.axis[1], v.ori.axis[2] );
			VectorCopy( c.origin, v.pvsOrigin );
			v.zNear = 2.0f;
			v.devoShadow = 3 + t;
			v.devoShadowRange = c.reach;
		}
	}
	return views;
}

// the lamp shadow masks of lightmap page 'page' (RGBA, size x size), while they
// wait to be uploaded next to the lightmaps; NULL when there are none
const byte *R_DevoGI_LampMask( int page, int size )
{
	if ( !gi_liveActive || !gi_live.lampsBaked || size != gi_livePageSize || page < 0 || size <= 0 )
		return NULL;
	const size_t pageBytes = (size_t)size * size * 4;
	if ( (size_t)( page + 1 ) * pageBytes > gi_live.lampMask.size() )
		return NULL;
	return &gi_live.lampMask[(size_t)page * pageBytes];
}

void R_DevoGI_FreeLampMasks( void )
{
	gi_live.lampMask.clear();
	gi_live.lampMask.shrink_to_fit();
}

void R_DevoGI_FillLights( const trRefdef_t *refdef, vkUniformLight_t *u )
{
	u->devo_sun_dir[3] = 0.0f;
	u->devo_shadow_params[0] = 0.0f;
	if ( !gi_liveActive || ( refdef->rdflags & RDF_NOWORLDMODEL ) || !tr.world )
		return;
	// the sun's own view only needs depth: no lighting there, and above all the shaders
	// must not read the shadow map while it is being drawn
	if ( backEnd.viewParms.devoShadow )
		return;
	const float shift = R_DevoGI_Shift() / 255.0f;	// lightmap units -> GPU colour

	if ( gi_live.hasSun ) {
		VectorCopy( gi_live.sunDir, u->devo_sun_dir );
		u->devo_sun_dir[3] = 1.0f;
		VectorScale( gi_live.sunRgb, shift, u->devo_sun_color );
	} else {
		u->devo_sun_dir[3] = 1.0f;	// live lighting on, but no sun
		VectorClear( u->devo_sun_color );
	}
	// one lightmap step in GPU light: the shader rolls bright light off above 170 steps,
	// like the bake does (gi_shoulder), so live sun on top of bounce light doesn't clip
	u->devo_sun_color[3] = shift;
	// lamp tiers: the lamps' light and soft shadows are baked into the live layer, so on
	// lightmapped surfaces a live lamp only adds its highlights, inside its baked mask
	u->devo_shadow_offset[3] = gi_live.lampsBaked ? 1.0f : 0.0f;
	// how much the final gamma pass scales the picture up (overbright), so the shader knows
	// where on-screen white is and can roll the brightest live light off before it clips
	u->devo_shadow_offset[2] = (float)( 1 << tr.overbrightBits );

	// one pixel of the shadow image (the sun's cascades and the near-lamp tiles share it)
	if ( vk_devoShadow.active ) {
		u->devo_shadow_params[1] = 1.0f / (float)vk_devoShadow.width;
		u->devo_shadow_params[2] = 1.0f / (float)vk_devoShadow.height;
	}

	// near-lamp shadows drawn this frame (see R_DevoGI_LampShadowViews): their tiles and
	// matrices; the lights themselves get their slot in the list further down
	int lampSlots = 0;
	if ( vk_devoShadow.active ) {
		const float tileWorld = 2.0f * tanf( DEG2RAD( gi_lampShadowFov * 0.5f ) ) / (float)vk_devoShadow.lampTile;
		for ( int k = 0; k < gi_numLampShadows; k++ ) {
			const giLampShadow &ls = gi_lampShadow[k];
			float tiles[6];
			for ( int f = 0; f < 6; f++ ) {
				const int t = ls.tile[f];
				tiles[f] = ( t >= 0 && gi_lampShadowValid[t] ) ? (float)t : -1.0f;
				if ( tiles[f] >= 0.0f )
					Com_Memcpy( u->devo_lamp_shadow_mvp[t], gi_lampShadowMvp[t], sizeof( gi_lampShadowMvp[t] ) );
			}
			float *sides = u->devo_lamp_shadow[k * 2], *more = u->devo_lamp_shadow[k * 2 + 1];
			sides[0] = tiles[0]; sides[1] = tiles[1]; sides[2] = tiles[2]; sides[3] = tiles[3];
			more[0] = tiles[4]; more[1] = tiles[5];
			// lookups pushed off the surface by one and a half shadow pixels, which grow
			// with the distance from the light
			more[2] = tileWorld * 1.5f;
			more[3] = 0.0f;
		}
		lampSlots = gi_numLampShadows;
		u->devo_lamp_shadow_params[0] = (float)lampSlots;
		u->devo_lamp_shadow_params[1] = (float)vk_devoShadow.lampTile / (float)vk_devoShadow.width;
		u->devo_lamp_shadow_params[2] = (float)vk_devoShadow.lampTile / (float)vk_devoShadow.height;
		u->devo_lamp_shadow_params[3] = (float)vk_devoShadow.lampTilesPerRow;
	}
	// the slot of the light at 'origin', or -1 (light entries carry it in origin.w)
	auto lampSlot = [&]( const vec3_t origin ) -> int {
		for ( int k = 0; k < lampSlots; k++ )
			if ( DistanceSquared( origin, gi_lampShadow[k].origin ) < 0.01f )
				return k;
		return -1;
	};
	// the game's own lights (sabers, blaster bolts) are already in the list
	for ( uint32_t i = 0; i < u->num_lights; i++ ) {
		const int k = lampSlot( u->light[i].origin );
		if ( k >= 0 )
			u->light[i].origin[3] = 100.0f * ( k + 1 );
	}

	// the sun's real-time shadow maps, when both cascades have been drawn
	if ( gi_shadowValid[0] && gi_shadowValid[1] && r_devoGI_sunShadow->integer && gi_live.hasSun ) {
		Com_Memcpy( u->devo_shadow_mvp0, gi_shadowMvp[0], sizeof( gi_shadowMvp[0] ) );
		Com_Memcpy( u->devo_shadow_mvp1, gi_shadowMvp[1], sizeof( gi_shadowMvp[1] ) );
		u->devo_shadow_params[0] = 1.0f;
		u->devo_shadow_params[3] = 0.85f;	// start blending to the next cascade here
		// push lookups off the surface by about one and a half pixels (no "acne")
		u->devo_shadow_offset[0] = gi_shadowTexelWorld[0] * 1.5f;
		u->devo_shadow_offset[1] = gi_shadowTexelWorld[1] * 1.5f;
	}

	const int maxLights = (int)ARRAY_LEN( u->light );
	const int room = maxLights - (int)u->num_lights;
	if ( room <= 0 )
		return;

	static std::vector<std::pair<float, int>> cand;
	cand.clear();
	for ( int i = 0; i < (int)gi_live.fixtures.size(); i++ ) {
		const dgFixture &fx = gi_live.fixtures[i];
		vec3_t d;
		VectorSubtract( fx.origin, refdef->vieworg, d );
		const float d2 = DotProduct( d, d );
		// a lamp matters when its light reaches something in view - not only when the
		// camera itself stands within its reach (that made lamps pop on as the player
		// walked up to them): its sphere of light must touch the view frustum
		bool outside = false;
		for ( int p = 0; p < 4 && !outside; p++ ) {
			const cplane_t &pl = backEnd.viewParms.frustum[p];
			if ( DotProduct( fx.origin, pl.normal ) - pl.dist < -fx.reach )
				outside = true;
		}
		if ( outside )
			continue;
		if ( !R_inPVS( refdef->vieworg, fx.origin, NULL ) )
			continue;	// in a part of the map the camera can't see
		const float power = Q_max( fx.rgb[0], Q_max( fx.rgb[1], fx.rgb[2] ) );
		cand.push_back( std::make_pair( power / ( d2 + 128.0f * 128.0f ), i ) );
	}
	if ( (int)cand.size() > room ) {
		std::nth_element( cand.begin(), cand.begin() + room, cand.end(),
			[]( const std::pair<float, int> &a, const std::pair<float, int> &b ) { return a.first > b.first; } );
		cand.resize( room );
	}

	for ( const auto &c : cand ) {
		const dgFixture &fx = gi_live.fixtures[c.second];
		vkUniformLightEntry_t &e = u->light[u->num_lights++];
		VectorCopy( fx.origin, e.origin );
		const bool area = fx.halfU > 0 || fx.halfV > 0;
		e.origin[3] = area ? 1.0f : 2.0f;	// light type: 1 = glowing surface, 2 = shines everywhere
		// lamp tiers: its channel in the baked lamp shadow masks, as type + 10 * ( channel + 1 )
		if ( gi_live.lampsBaked && fx.maskChannel >= 0 && fx.maskChannel < 4 )
			e.origin[3] += 10.0f * ( fx.maskChannel + 1 );
		// near-lamp shadows: its slot, as + 100 * ( slot + 1 )
		const int slot = lampSlot( fx.origin );
		if ( slot >= 0 )
			e.origin[3] += 100.0f * ( slot + 1 );
		VectorScale( fx.rgb, shift, e.color );
		e.radius = fx.reach;
		VectorCopy( fx.normal, e.normal );
		// a glowing rectangle seen from close by: its light spreads over its area, like
		// a disc of the same size (this keeps the light finite right at the lamp)
		e.normal[3] = 16.0f + 4.0f * fx.halfU * fx.halfV / (float)M_PI;
	}
}

/*
===========================================================================
R_DevoGI_PatchVertexColors

Copies the baked vertex colours into the loaded map data, right before the
renderer builds its surfaces from it (called from RE_LoadWorldMap_Actual), and
moves the surfaces that got their own lightmap spot (see dgLightmapMove).
===========================================================================
*/

void R_DevoGI_PatchVertexColors( byte *fileBase, const dheader_t *header )
{
	// surfaces that shared a lightmap spot with another one, or get more detail, move
	DG_ApplyLightmapMoves( fileBase, gi_lightmapMoves );
	// round walls made of flat faces shade smoothly, as in the bake
	if ( gi_smoothMapNormals )
		DG_SmoothMapNormals( fileBase );
	gi_smoothMapNormals = false;
	gi_lightmapMoves.clear();
	gi_lightmapMoves.shrink_to_fit();

	const lump_t &lv = header->lumps[LUMP_DRAWVERTS];
	const int numVerts = lv.filelen / (int)sizeof( mapVert_t );
	if ( gi_vertexColors.size() == (size_t)numVerts * 4 ) {
		mapVert_t *dv = (mapVert_t *)( fileBase + lv.fileofs );
		for ( int v = 0; v < numVerts; v++ ) {
			dv[v].color[0][0] = gi_vertexColors[(size_t)v * 4 + 0];
			dv[v].color[0][1] = gi_vertexColors[(size_t)v * 4 + 1];
			dv[v].color[0][2] = gi_vertexColors[(size_t)v * 4 + 2];
		}
	}
	gi_vertexColors.clear();
	gi_vertexColors.shrink_to_fit();
}

/*
===========================================================================
DevoLights - the map's own light sources

R_DevoLights_Extract finds them once per map (devogi_core.cpp does the
work); the bake uses them, and so do the preview and the list below.
===========================================================================
*/

void R_DevoLights_Extract( const char *mapName, const byte *fileBase, const dheader_t *header )
{
	(void)header;	// the core reads the header from the start of the file
	DG_SetHost( &gi_host );
	if ( r_devoGI && r_devoGI->integer && R_DevoGI_IsRend2Map( mapName ) ) {
		DG_ExtractLights( mapName, NULL, R_DevoGI_Settings() );	// no lights: nothing for DevoGI to do
		return;
	}
	DG_ExtractLights( mapName, fileBase, R_DevoGI_Settings() );
}

/*
Live preview (r_devoLights 1): adds the nearest and brightest map lights that
the camera can see as the renderer's ordinary dynamic lights. No shadows yet -
this is a preview of where the real-time lighting work is heading.
*/
void R_DevoLights_AddToScene( const vec3_t viewOrg, int freeSlots )
{
	const int numLights = DG_NumLights();
	if ( !r_devoLights || !r_devoLights->integer || !numLights || freeSlots <= 0 )
		return;
	const int maxLights = Q_min( freeSlots, Com_Clampi( 0, 64, r_devoLights_max->integer ) );
	if ( maxLights <= 0 )
		return;

	const float rangeScale = Q_max( 0.05f, r_devoLights_range->value );
	std::vector<std::pair<float, int>> cand;
	cand.reserve( 256 );
	dgLightInfo lt;
	for ( int i = 0; i < numLights; i++ ) {
		DG_GetLight( i, lt );
		vec3_t d;
		VectorSubtract( lt.origin, viewOrg, d );
		const float dist2 = DotProduct( d, d );
		const float reach = lt.radius * rangeScale * 1.5f;
		if ( dist2 > reach * reach )
			continue;
		if ( !R_inPVS( viewOrg, lt.origin, NULL ) )
			continue;	// in a part of the map the camera can't see
		cand.push_back( std::make_pair( lt.photons / ( dist2 + 128.0f * 128.0f ), i ) );
	}
	if ( (int)cand.size() > maxLights ) {
		std::nth_element( cand.begin(), cand.begin() + maxLights, cand.end(),
			[]( const std::pair<float, int> &a, const std::pair<float, int> &b ) { return a.first > b.first; } );
		cand.resize( maxLights );
	}

	const float intensity = Q_max( 0.0f, r_devoLights_intensity->value );
	for ( const auto &c : cand ) {
		DG_GetLight( c.second, lt );
		RE_AddLightToScene( lt.origin, lt.radius * rangeScale, lt.color[0] * intensity, lt.color[1] * intensity, lt.color[2] * intensity );
	}
}

// console command "devolights": lists the sun and the strongest lights of the current map
void R_DevoLights_List_f( void )
{
	const int numLights = DG_NumLights();
	const char *map = DG_LightsMapName();
	ri.Printf( PRINT_ALL, "DevoLights for %s: %d lights\n", map && map[0] ? map : "(no map)", numLights );

	float dir[3], color[3], intensity;
	if ( DG_GetSun( dir, color, &intensity ) )
		ri.Printf( PRINT_ALL, "  sun: dir ( %.2f %.2f %.2f ) colour ( %.2f %.2f %.2f ) intensity %.0f\n",
			dir[0], dir[1], dir[2], color[0], color[1], color[2], intensity );
	else
		ri.Printf( PRINT_ALL, "  sun: none\n" );

	std::vector<dgLightInfo> lights( numLights );
	std::vector<int> order( numLights );
	for ( int i = 0; i < numLights; i++ ) {
		DG_GetLight( i, lights[i] );
		order[i] = i;
	}
	std::sort( order.begin(), order.end(), [&]( int a, int b ) { return lights[a].photons > lights[b].photons; } );
	const int show = Q_min( 30, numLights );
	for ( int k = 0; k < show; k++ ) {
		const dgLightInfo &lt = lights[order[k]];
		ri.Printf( PRINT_ALL, "  %3d: ( %6.0f %6.0f %6.0f ) radius %5.0f colour ( %.2f %.2f %.2f )\n",
			order[k], lt.origin[0], lt.origin[1], lt.origin[2], lt.radius, lt.color[0], lt.color[1], lt.color[2] );
	}
	if ( numLights > show )
		ri.Printf( PRINT_ALL, "  ... %d more\n", numLights - show );
}
