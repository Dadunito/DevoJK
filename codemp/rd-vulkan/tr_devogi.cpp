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
static cvar_t *r_devoGI_sunShadow;
static cvar_t *r_devoGI_sunShadowSize;
static cvar_t *r_devoGI_sunShadowRange;
static cvar_t *r_devoGI_sunShadowRangeFar;

void R_DevoGI_Register( void )
{
	r_devoGI				= ri.Cvar_Get( "r_devoGI",				"1",	CVAR_ARCHIVE | CVAR_LATCH, "Baked global illumination: maps get new lighting on their first load, cached in maps/<name>.devogi\n 0: off\n 1: on" );
	r_devoGI_scale			= ri.Cvar_Get( "r_devoGI_scale",		"4",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI lightmap resolution multiplier (1, 2, 4 or 8). Higher gives sharper shadows and corners but bakes slower and uses more memory" );
	r_devoGI_rays			= ri.Cvar_Get( "r_devoGI_rays",			"128",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI rays per bounce-light sample (8-1024). More is smoother but bakes slower" );
	r_devoGI_bounces		= ri.Cvar_Get( "r_devoGI_bounces",		"8",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI light bounces (1-16)" );
	r_devoGI_aa				= ri.Cvar_Get( "r_devoGI_aa",			"2.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI shadow anti-aliasing: width of the area each texel's shadow samples come from, in texels (0.5 = sharp, 2 = smooth, up to 4)" );
	r_devoGI_sheen			= ri.Cvar_Get( "r_devoGI_sheen",		"0.12",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI baked sheen: every surface faintly picks up the faces right in front of it, like a slightly glossy finish (0 = off, up to 0.5)" );
	r_devoGI_intensity		= ri.Cvar_Get( "r_devoGI_intensity",	"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI bounce light strength" );
	r_devoGI_exposure		= ri.Cvar_Get( "r_devoGI_exposure",		"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI overall brightness (1 = match the original map's brightness)" );
	r_devoGI_sky			= ri.Cvar_Get( "r_devoGI_sky",			"0.5",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI soft ambient light from visible sky (0 = none)" );
	r_devoGI_saturation		= ri.Cvar_Get( "r_devoGI_saturation",	"1.6",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI colour bleeding strength: saturation of light reflected off surfaces (1 = texture average colour)" );
	r_devoGI_ao				= ri.Cvar_Get( "r_devoGI_ao",			"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient occlusion on bounce light: keeps corners and crevices dark (0 = off, 1 = full, up to 2)" );
	r_devoGI_aoDistance		= ri.Cvar_Get( "r_devoGI_aoDistance",	"64",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient occlusion range in game units" );
	r_devoGI_aoDirect		= ri.Cvar_Get( "r_devoGI_aoDirect",		"0.15",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI contact darkening of the old lightmap, only used when a map cannot be relit (0 = off, 1 = full)" );
	r_devoGI_relight		= ri.Cvar_Get( "r_devoGI_relight",		"1",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI: trace the direct light again from the map's own lights, with soft shadows, replacing the old lightmap (only its brightness is used as a reference)\n 0: keep the old lightmap's direct light" );
	r_devoGI_entityLights	= ri.Cvar_Get( "r_devoGI_entityLights",	"1",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI uses light entities kept in the BSP as light sources" );
	r_devoGI_threads		= ri.Cvar_Get( "r_devoGI_threads",		"0",	CVAR_ARCHIVE, "DevoGI bake threads (0 = all CPU cores)" );
	r_devoGI_progress		= ri.Cvar_Get( "r_devoGI_progress",		"1",	CVAR_ARCHIVE, "DevoGI: show a progress screen with a percentage while a map bakes" );
	r_devoGI_live			= ri.Cvar_Get( "r_devoGI_live",			"0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI real-time lighting (work in progress): the map's lamps and the sun are drawn live, only bounce light comes from the bake\n 0: full bake (fastest)\n 1: live lamps and sun (no lamp shadows yet)" );
	r_devoGI_sunShadow		= ri.Cvar_Get( "r_devoGI_sunShadow",	"1",	CVAR_ARCHIVE, "DevoGI live lighting: real-time sun shadows from everything, players and moving objects included (0 = only the baked sun shadows)" );
	r_devoGI_sunShadowSize	= ri.Cvar_Get( "r_devoGI_sunShadowSize",	"2048",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI live lighting: sun shadow map resolution (1024, 2048 or 4096 - sharper but slower)" );
	r_devoGI_sunShadowRange	= ri.Cvar_Get( "r_devoGI_sunShadowRange",	"1024",	CVAR_ARCHIVE, "DevoGI live lighting: how far around the player real-time sun shadows reach, in game units (further away the baked shadows take over)" );
	r_devoGI_sunShadowRangeFar = ri.Cvar_Get( "r_devoGI_sunShadowRangeFar", "4096", CVAR_ARCHIVE, "DevoGI live lighting: reach of the second, coarser sun shadow map, in game units (beyond it the baked shadows take over)" );
	r_devoGI_gpu			= ri.Cvar_Get( "r_devoGI_gpu",			"1",	CVAR_ARCHIVE, "DevoGI: trace the bake's rays on the graphics card (much faster, same result; falls back to the CPU when it can't)" );
	r_devoGI_rebuild		= ri.Cvar_Get( "r_devoGI_rebuild",		"0",	CVAR_TEMP, "Set to 1 to force DevoGI to bake the next map that loads again" );
	r_devoLights_glow		= ri.Cvar_Get( "r_devoLights_glow",		"4000",	CVAR_ARCHIVE | CVAR_LATCH, "DevoLights: starting strength of glowing lamp textures without q3map_surfacelight (the bake then matches each lamp type to the original map); 0 = they give no light" );
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

// lightmap bytes -> the GPU's lightmap values (the map's overbright shift, as R_LoadLightmaps does)
static float R_DevoGI_Shift( void ) {
	return powf( 2.0f, (float)( r_mapOverBrightBits->integer - tr.overbrightBits ) );
}

byte *R_DevoGI_Process( const char *mapName, const byte *fileBase, const dheader_t *header, int maxScale, int *outPageSize )
{
	*outPageSize = 128;
	gi_vertexColors.clear();
	gi_liveActive = false;
	gi_live = dgLiveLighting();
	gi_liveAlpha.clear();
	gi_shadowValid[0] = gi_shadowValid[1] = false;
	if ( !r_devoGI || !r_devoGI->integer )
		return NULL;
	if ( header->lumps[LUMP_LIGHTMAPS].filelen < 128 * 128 * 3 )
		return NULL;	// maps with external lightmap images are not supported yet

	DG_SetHost( &gi_host );
	const dgSettings settings = R_DevoGI_Settings();
	const int scale = DG_ChooseScale( settings, maxScale );

	char base[MAX_QPATH], cacheName[MAX_QPATH];
	COM_StripExtension( mapName, base, sizeof( base ) );
	Com_sprintf( cacheName, sizeof( cacheName ), "%s.devogi", base );
	const uint64_t bspHash = DG_BspHash( fileBase );
	const uint64_t settingsHash = DG_SettingsHash( settings, scale );

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
		if ( !DG_Bake( mapName, fileBase, settings, scale, result ) )
			return NULL;
		const std::vector<byte> file = DG_WriteCache( bspHash, settingsHash, result );
		ri.FS_WriteFile( cacheName, file.data(), (int)file.size() );
		ri.Printf( PRINT_ALL, "DevoGI: saved %s\n", cacheName );
	}

	gi_vertexColors.swap( result.vertexColors );

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
Light for a model (player, weapon, item) from the 8 light probes around it:
the bounce light arriving from all around (the average of the 6 directions each
probe stores), and how much of the sun it sees. Direct light from the lamps and
the sun is added per pixel on the GPU. Values are in the 0..255 range the
renderer uses for model lighting. Returns false when live lighting is off.
*/
qboolean R_DevoGI_EntityLighting( const vec3_t point, vec3_t ambient, vec3_t directed, float *sunVis )
{
	if ( !gi_liveActive || gi_live.probes.empty() )
		return qfalse;
	const int *dims = gi_live.probeDims;
	float f[3];
	int c0[3];
	for ( int k = 0; k < 3; k++ ) {
		const float g = ( point[k] - gi_live.probeOrigin[k] ) / gi_live.probeSpacing;
		c0[k] = Com_Clampi( 0, Q_max( 0, dims[k] - 2 ), (int)floorf( g ) );
		f[k] = Com_Clamp( 0.0f, 1.0f, g - c0[k] );
	}
	float sum[3] = { 0, 0, 0 }, sun = 0, wsum = 0;
	for ( int corner = 0; corner < 8; corner++ ) {
		const int x = Q_min( dims[0] - 1, c0[0] + ( corner & 1 ) );
		const int y = Q_min( dims[1] - 1, c0[1] + ( ( corner >> 1 ) & 1 ) );
		const int z = Q_min( dims[2] - 1, c0[2] + ( corner >> 2 ) );
		float w = ( ( corner & 1 ) ? f[0] : 1 - f[0] ) * ( ( corner & 2 ) ? f[1] : 1 - f[1] ) * ( ( corner & 4 ) ? f[2] : 1 - f[2] );
		const size_t i = ( (size_t)z * dims[1] + y ) * dims[0] + x;
		if ( !gi_live.probeValid[i] )
			w *= 0.05f;		// a probe inside a wall only counts if nothing better is near
		for ( int a = 0; a < 6; a++ )
			for ( int k = 0; k < 3; k++ )
				sum[k] += w * R_DevoGI_Half( gi_live.probes[i * 18 + a * 3 + k] ) / 6.0f;
		sun += w * ( gi_live.probeSun.size() > i ? gi_live.probeSun[i] / 255.0f : 1.0f );
		wsum += w;
	}
	if ( wsum <= 0 )
		return qfalse;
	const float shift = R_DevoGI_Shift();
	for ( int k = 0; k < 3; k++ ) {
		ambient[k] = sum[k] / wsum * shift;
		directed[k] = 0;	// lamps and the sun are added per pixel instead
	}
	*sunVis = sun / wsum;
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

	const float nearRange = Com_Clamp( 256.0f, 8192.0f, r_devoGI_sunShadowRange->value );
	const float range = cascade == 0 ? nearRange
		: Com_Clamp( nearRange * 1.5f, 32768.0f, r_devoGI_sunShadowRangeFar->value );	// half the square's side
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

	// centre of the square: a little ahead of the player, snapped to whole shadow-map pixels
	const float texel = 2.0f * range / size;
	vec3_t centre;
	VectorMA( mainView->ori.origin, range * 0.4f, mainView->ori.axis[0], centre );
	const float cx = floorf( DotProduct( centre, left ) / texel ) * texel;
	const float cy = floorf( DotProduct( centre, up ) / texel ) * texel;
	const float cz = DotProduct( centre, forward );
	VectorScale( left, cx, centre );
	VectorMA( centre, cy, up, centre );
	VectorMA( centre, cz, forward, centre );

	Com_Memset( out, 0, sizeof( *out ) );
	out->viewportX = cascade * size;	// both cascades side by side in one image
	out->viewportY = 0;
	out->viewportWidth = out->viewportHeight = size;
	out->scissorX = out->viewportX;
	out->scissorY = 0;
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
	const int cascade = view->devoShadow - 1;
	if ( cascade < 0 || cascade > 1 )
		return;
	// the same transform the GPU used to draw the view (see get_mvp_transform)
	float proj[16];
	Com_Memcpy( proj, view->projectionMatrix, sizeof( proj ) );
	proj[5] = -proj[5];
	myGlMultMatrix( view->world.modelViewMatrix, proj, gi_shadowMvp[cascade] );
	gi_shadowValid[cascade] = true;
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

	// the sun's real-time shadow maps, when both cascades have been drawn
	if ( gi_shadowValid[0] && gi_shadowValid[1] && r_devoGI_sunShadow->integer && gi_live.hasSun ) {
		Com_Memcpy( u->devo_shadow_mvp0, gi_shadowMvp[0], sizeof( gi_shadowMvp[0] ) );
		Com_Memcpy( u->devo_shadow_mvp1, gi_shadowMvp[1], sizeof( gi_shadowMvp[1] ) );
		u->devo_shadow_params[0] = 1.0f;
		u->devo_shadow_params[1] = 1.0f / ( 2.0f * vk_devoShadow.size );	// the image is two cascades wide
		u->devo_shadow_params[2] = 1.0f / (float)vk_devoShadow.size;
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
		if ( d2 > fx.reach * fx.reach )
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
renderer builds its surfaces from it (called from RE_LoadWorldMap_Actual).
===========================================================================
*/

void R_DevoGI_PatchVertexColors( byte *fileBase, const dheader_t *header )
{
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
