/*
===========================================================================
DevoJK - DevoGI and DevoLights
Baked global illumination and relighting for Jedi Academy maps

Hello! This file gives old Quake 3 / Jedi Academy maps new, high resolution
lighting, without touching the map files themselves. It is the engine-free
core of the bake: the DevoJK renderer runs it while a map loads, and the
DevoQ3Map2 compiler runs exactly the same code with "-devogi" to bake ahead
of time. See devogi_core.h for how it talks to the outside world.
Please read, share and modify it freely (GPLv2).

---------------------------------------------------------------------------
The big picture
---------------------------------------------------------------------------

A compiled map (.bsp) stores its lighting in "lightmaps": small 128x128
images, one texel per ~16 game units, that the map compiler (q3map2) baked
from the map's lights. They are low resolution and have blocky shadows,
and the lights themselves are thrown away by the compiler.

On the first load of a map we:

  1. Find the map's light sources again (DevoLights, at the bottom of this
     file): lamp shaders with q3map_surfacelight, glowing lamp textures
     whose light entities the compiler removed, the sun from the sky
     shader, and any light entities still in the BSP.
  2. Build a ray tracer over the map's triangles (a BVH).
  3. Give every lightmap texel, at 2x/4x/8x the original resolution, a
     position and normal in the world. Texels that ended up inside walls
     are pushed back out into the room.
  4. Measure how bright each kind of lamp has to be, so the level keeps the
     mood its designers gave it: the old lightmap is the reference.
  5. Trace direct light with soft shadows from every lamp and the sun.
  6. Trace bounced light (global illumination) for several bounces.
  7. Match the overall exposure to the original level, write the new
     lightmaps and also re-light vertex-lit models and vertex colours.
  8. Also save the same light split for real-time lighting: bounce-only
     lightmaps, a grid of light probes for models, and the lamps merged
     into fixtures with their calibrated strength (see "Splitting the
     light for real-time use").
  9. Hand back the result; the host saves it as maps/<name>.devogi, so later
     loads skip all of this. The cache is rebuilt automatically when the
     BSP, its shaders or any bake setting changes.

The renderer then uses the new lightmaps exactly like the old ones, so
there is no runtime cost at all. The split in step 8 is for a renderer that
draws direct light live with soft shadows and only takes the bounce light
from the bake.

Ideas inspired by Berk Emre Saribas' Panko-Renderer (MIT) and by q3map2's
own light code; the implementation here is written for the Q3 BSP format.

This file is part of DevoJK by Devoted, licensed under GPLv2.
===========================================================================
*/

#include "devogi_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/*
===========================================================================
Engine-free basics

The bake was born inside the renderer and uses a handful of the engine's
little helpers and the BSP file layout. They are repeated here, written to
behave exactly like the engine's own (q_shared.c, qfiles.h), so this file
compiles on its own in the renderer and in the map compiler alike.
Everything sits in an anonymous namespace so it never clashes with the
host's versions of the same names.
===========================================================================
*/

namespace {

typedef unsigned char byte;
const bool qtrue = true, qfalse = false;

#define Q_min( x, y ) ( ( x ) < ( y ) ? ( x ) : ( y ) )
#define Q_max( x, y ) ( ( x ) > ( y ) ? ( x ) : ( y ) )
#define DEG2RAD( a ) ( ( ( a ) * 3.14159265358979323846f ) / 180.0f )

#define MAX_QPATH		64
#define MAX_KEY			32
#define DG_MAX_TOKEN	1024

inline float Com_Clamp( float min, float max, float value ) {
	return value < min ? min : ( value > max ? max : value );
}
inline int Com_Clampi( int min, int max, int value ) {
	return value < min ? min : ( value > max ? max : value );
}

// numbers in shader scripts are floats (atof itself gives a double)
inline float dg_atof( const char *s ) {
	return (float)strtod( s, NULL );
}
#define atof dg_atof

// BSP files are little-endian, like every PC the game runs on
inline int LittleLong( int l ) { return l; }
inline float LittleFloat( float f ) { return f; }

inline int Q_stricmpn( const char *s1, const char *s2, int n ) {
	if ( !s1 ) return s2 ? -1 : 0;
	if ( !s2 ) return 1;
	for ( ; n > 0; n--, s1++, s2++ ) {
		int c1 = (unsigned char)*s1, c2 = (unsigned char)*s2;
		if ( c1 >= 'a' && c1 <= 'z' ) c1 -= 'a' - 'A';
		if ( c2 >= 'a' && c2 <= 'z' ) c2 -= 'a' - 'A';
		if ( c1 != c2 ) return c1 < c2 ? -1 : 1;
		if ( !c1 ) return 0;
	}
	return 0;
}
inline int Q_stricmp( const char *s1, const char *s2 ) {
	return Q_stricmpn( s1, s2, 99999 );
}

inline void Q_strncpyz( char *dest, const char *src, int destsize ) {
	if ( !dest || destsize < 1 )
		return;
	strncpy( dest, src ? src : "", destsize - 1 );
	dest[destsize - 1] = 0;
}

void Com_sprintf( char *dest, int size, const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( dest, size, fmt, ap );
	va_end( ap );
	dest[size - 1] = 0;
}

// "textures/foo/bar.tga" -> "textures/foo/bar" (same rules as the engine)
void COM_StripExtension( const char *in, char *out, int destsize ) {
	const char *dot = strrchr( in, '.' ), *slash;
	if ( dot && ( !( slash = strrchr( in, '/' ) ) || slash < dot ) )
		destsize = ( destsize < dot - in + 1 ? destsize : (int)( dot - in + 1 ) );
	if ( in == out && destsize > 1 )
		out[destsize - 1] = '\0';
	else
		Q_strncpyz( out, in, destsize );
}

// "maps/ffa3.bsp" -> "ffa3.bsp"
char *COM_SkipPath( char *pathname ) {
	char *last = pathname;
	while ( *pathname ) {
		if ( *pathname == '/' )
			last = pathname + 1;
		pathname++;
	}
	return last;
}

/*
The engine's tokenizer, token for token: skips spaces and // and block comments,
handles "quoted strings", and with allowLineBreaks = false stops at the end of
the line (returning an empty token). The token buffer is per thread, so shader
parsing is safe even if someone calls it from worker threads.
*/
const char *SkipWhitespace( const char *data, bool *hasNewLines ) {
	int c;
	while ( ( c = *(const unsigned char *)data ) <= ' ' ) {
		if ( !c )
			return NULL;
		if ( c == '\n' )
			*hasNewLines = true;
		data++;
	}
	return data;
}

char *COM_ParseExt( const char **data_p, bool allowLineBreaks ) {
	static thread_local char token[DG_MAX_TOKEN];
	int c = 0, len = 0;
	bool hasNewLines = false;
	const char *data = *data_p;
	token[0] = 0;
	if ( !data ) {
		*data_p = NULL;
		return token;
	}
	while ( 1 ) {
		data = SkipWhitespace( data, &hasNewLines );
		if ( !data ) {
			*data_p = NULL;
			return token;
		}
		if ( hasNewLines && !allowLineBreaks ) {
			*data_p = data;
			return token;
		}
		c = *data;
		if ( c == '/' && data[1] == '/' ) {
			data += 2;
			while ( *data && *data != '\n' )
				data++;
		} else if ( c == '/' && data[1] == '*' ) {
			data += 2;
			while ( *data && ( *data != '*' || data[1] != '/' ) )
				data++;
			if ( *data )
				data += 2;
		} else {
			break;
		}
	}
	if ( c == '\"' ) {
		data++;
		while ( 1 ) {
			c = *data++;
			if ( c == '\"' || !c ) {
				token[len] = 0;
				*data_p = data;
				return token;
			}
			if ( len < DG_MAX_TOKEN - 1 )
				token[len++] = (char)c;
		}
	}
	do {
		if ( len < DG_MAX_TOKEN - 1 )
			token[len++] = (char)c;
		data++;
		c = *data;
	} while ( c > 32 );
	token[len] = 0;
	*data_p = data;
	return token;
}

// ---- the Q3 / JA BSP layout (qfiles.h) ----

struct lump_t {
	int		fileofs, filelen;
};

#define LUMP_ENTITIES		0
#define LUMP_SHADERS		1
#define LUMP_DRAWVERTS		10
#define LUMP_DRAWINDEXES	11
#define LUMP_SURFACES		13
#define LUMP_LIGHTMAPS		14
#define HEADER_LUMPS		18

struct dheader_t {
	int		ident;
	int		version;
	lump_t	lumps[HEADER_LUMPS];
};

struct dshader_t {
	char	shader[MAX_QPATH];
	int		surfaceFlags;
	int		contentFlags;
};

#define MAXLIGHTMAPS		4
#define LS_LSNONE			0xff
#define LIGHTMAP_BY_VERTEX	-3

struct mapVert_t {
	float	xyz[3];
	float	st[2];
	float	lightmap[MAXLIGHTMAPS][2];
	float	normal[3];
	byte	color[MAXLIGHTMAPS][4];
};

enum { MST_BAD, MST_PLANAR, MST_PATCH, MST_TRIANGLE_SOUP, MST_FLARE };

struct dsurface_t {
	int		shaderNum;
	int		fogNum;
	int		surfaceType;
	int		firstVert;
	int		numVerts;
	int		firstIndex;
	int		numIndexes;
	byte	lightmapStyles[MAXLIGHTMAPS], vertexStyles[MAXLIGHTMAPS];
	int		lightmapNum[MAXLIGHTMAPS];
	int		lightmapX[MAXLIGHTMAPS], lightmapY[MAXLIGHTMAPS];
	int		lightmapWidth, lightmapHeight;
	float	lightmapOrigin[3];
	float	lightmapVecs[3][3];
	int		patchWidth;
	int		patchHeight;
};

static_assert( sizeof( mapVert_t ) == 80, "mapVert_t must match the BSP file" );
static_assert( sizeof( dsurface_t ) == 148, "dsurface_t must match the BSP file" );

// surface and content flags (surfaceflags.h)
#define SURF_SKY				0x00002000u
#define SURF_NODRAW				0x00200000u
#define CONTENTS_LAVA			0x00000002u
#define CONTENTS_WATER			0x00000004u
#define CONTENTS_FOG			0x00000008u
#define CONTENTS_SLIME			0x00020000u
#define CONTENTS_TRANSLUCENT	0x80000000u

} // namespace


#include <algorithm>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Bump this whenever the bake changes in a way that should invalidate old .devogi files.
#define DEVOGI_VERSION		15

#define DEVOGI_BSP_LM_SIZE	128			// lightmap page size stored in every Q3/JA BSP
#define DEVOGI_LM_SIZE		gi_lmSize	// page size we are baking right now (128 * scale)
#define DEVOGI_PATCH_STEPS	16			// how finely curved patches are cut into triangles
#define DEVOGI_RAY_TMIN		1.5f		// hits closer than this are ignored, so a curved
										// surface does not shadow itself between its facets
#define DEVOGI_INF			1e30f

static int gi_lmSize = DEVOGI_BSP_LM_SIZE;

// New style-0 vertex colours (rgba per BSP draw vertex), filled by the bake and handed
// back in dgResult::vertexColors (the renderer copies them into the map data).
static std::vector<byte> gi_vertexColors;

/*
===========================================================================
Settings and host

The settings of the bake in progress (copied in by DG_Bake / DG_ExtractLights)
and the host that prints, loads images and finds shader text for us.
===========================================================================
*/

static dgSettings		g_set;
static const dgHost		*g_host;

void DG_SetHost( const dgHost *host ) {
	g_host = host;
}

static void dg_print( const char *fmt, ... ) {
	char buf[1024];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	if ( g_host && g_host->print )
		g_host->print( buf );
}

static int dg_ms( void ) {
	return ( g_host && g_host->milliseconds ) ? g_host->milliseconds() : 0;
}

static void dg_load_image( const char *name, byte **pic, int *w, int *h ) {
	*pic = NULL;
	*w = *h = 0;
	if ( g_host && g_host->loadImage && !g_host->loadImage( name, pic, w, h ) )
		*pic = NULL;
}

static void dg_free_image( byte *pic ) {
	if ( pic && g_host && g_host->freeImage )
		g_host->freeImage( pic );
}

/*
===========================================================================
Small math helpers

A tiny vector type so the bake reads naturally (a + b, v * s). Nothing
fancy: three floats and the usual operations.
===========================================================================
*/

struct gvec3 {
	float x, y, z;
	gvec3() : x(0), y(0), z(0) {}
	gvec3( float a, float b, float c ) : x(a), y(b), z(c) {}
	explicit gvec3( const float *v ) : x(v[0]), y(v[1]), z(v[2]) {}
	gvec3 operator+( const gvec3 &o ) const { return gvec3( x + o.x, y + o.y, z + o.z ); }
	gvec3 operator-( const gvec3 &o ) const { return gvec3( x - o.x, y - o.y, z - o.z ); }
	gvec3 operator*( float s ) const { return gvec3( x * s, y * s, z * s ); }
	gvec3 operator*( const gvec3 &o ) const { return gvec3( x * o.x, y * o.y, z * o.z ); }
	gvec3 &operator+=( const gvec3 &o ) { x += o.x; y += o.y; z += o.z; return *this; }
	float operator[]( int i ) const { return i == 0 ? x : ( i == 1 ? y : z ); }
};

static inline float gdot( const gvec3 &a, const gvec3 &b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline gvec3 gcross( const gvec3 &a, const gvec3 &b ) { return gvec3( a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x ); }
static inline gvec3 gnorm( const gvec3 &a ) { float l = sqrtf( gdot( a, a ) ); return l > 1e-12f ? a * ( 1.0f / l ) : gvec3( 0, 0, 1 ); }
static inline gvec3 gmin( const gvec3 &a, const gvec3 &b ) { return gvec3( Q_min( a.x, b.x ), Q_min( a.y, b.y ), Q_min( a.z, b.z ) ); }
static inline gvec3 gmax( const gvec3 &a, const gvec3 &b ) { return gvec3( Q_max( a.x, b.x ), Q_max( a.y, b.y ), Q_max( a.z, b.z ) ); }

// perceived brightness of a colour (Rec. 709 weights)
static inline float gi_luma( const gvec3 &c ) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// Stable random numbers: every texel seeds its own sequence from its index, so baking the
// same map twice gives exactly the same picture (no shimmering between rebakes).
static inline uint32_t gi_hash( uint32_t x ) {
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static inline float gi_rand( uint32_t &state ) {
	state = gi_hash( state + 0x9e3779b9U );
	return ( state >> 8 ) * ( 1.0f / 16777216.0f );
}

// two directions perpendicular to n (and to each other): a little coordinate frame on a surface
static void gi_basis( const gvec3 &n, gvec3 &t, gvec3 &b ) {
	if ( fabsf( n.z ) < 0.999f )
		t = gnorm( gcross( gvec3( 0, 0, 1 ), n ) );
	else
		t = gnorm( gcross( gvec3( 1, 0, 0 ), n ) );
	b = gcross( n, t );
}

// 64-bit FNV-1a hash, used for cache keys
static uint64_t gi_fnv( uint64_t h, const void *data, size_t len ) {
	const byte *p = (const byte *)data;
	for ( size_t i = 0; i < len; i++ ) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

/*
===========================================================================
DevoLights data

The map's light sources, filled by R_DevoLights_Extract when a map starts
loading. Both the bake and the live preview (r_devoLights) use them.

Every light belongs to a "group": all patches of one lamp shader share a
group, all light entities share one, and the sun has its own. The bake
measures one brightness per group (see "Calibration" below), so e.g. all
"desert/light_yellow" lamps get the same, well matched strength.
===========================================================================
*/

struct devoLight_t {
	gvec3	origin;
	gvec3	normal;		// which way the glowing surface faces (zero for point lights)
	gvec3	color;		// colour, brightest channel = 1
	float	photons;	// strength, in q3map2's units
	float	radius;		// distance where it fades out
	float	size;		// edge length of the glowing patch (0 for point lights)
	int		shader;		// emitting shader, -1 for light entities
	int		group;		// calibration group (see above)
};

static std::vector<devoLight_t>	gi_lights;
static bool						gi_hasSun;
static gvec3					gi_sunDir;			// pointing towards the sun
static gvec3					gi_sunColor;
static float					gi_sunIntensity;
static int						gi_numGroups = 2;	// shaders + light entities + sun
static int						gi_entityGroup = 0;
static int						gi_sunGroup = 1;
static char						gi_lightsMap[MAX_QPATH];
static std::vector<std::string>	gi_groupNames;		// readable name per group, for the bake log
static uint64_t					gi_lightsHash;		// changes whenever the light sources change

/*
===========================================================================
Geometry: the map as triangles

We read the BSP's draw surfaces (flat faces, triangle soups from models,
and curved Bezier patches) into one big triangle list that the ray tracer
uses. Every triangle remembers which surface it came from.
===========================================================================
*/

struct giVert {
	gvec3	xyz;
	gvec3	normal;
	float	lm[MAXLIGHTMAPS][2];	// lightmap coordinates, per light style slot (0..1 across a page)
	float	color[MAXLIGHTMAPS][3];	// q3map2's baked vertex colours, per light style slot
};

struct giTri {
	gvec3	v0, e1, e2;		// corner and two edges, ready for ray intersection
	gvec3	n;				// facing direction (unit length)
	int		surf;			// BSP surface it belongs to
	int		vert[3];		// its corners in giScene::verts
};

struct giSurf {
	int		shader;
	int		lightmapNum[MAXLIGHTMAPS];		// lightmap page per style slot (or a LIGHTMAP_* code)
	int		lightmapStyle[MAXLIGHTMAPS];	// light style in each slot (0 = always on, 255 = unused)
	bool	occluder;						// does it block light?
	bool	sky;
};

struct giShader {
	int		surfaceFlags;
	int		contentFlags;
	gvec3	albedo;		// how much light the surface reflects, per colour channel
};

struct giBVHNode {
	gvec3	mins, maxs;
	int		left;		// first child node, or first triangle for a leaf
	int		count;		// triangles in a leaf, 0 for inner nodes
};

struct giScene {
	std::vector<giVert>		verts;
	std::vector<giTri>		tris;
	std::vector<giSurf>		surfs;
	std::vector<giShader>	shaders;
	std::vector<giBVHNode>	nodes;
	std::vector<int>		triIndex;
};

// evaluates one 3x3 block of a Q3 Bezier patch at (u, v)
static giVert gi_patch_eval( const giVert *c[9], float u, float v ) {
	float bu[3] = { ( 1 - u ) * ( 1 - u ), 2 * u * ( 1 - u ), u * u };
	float bv[3] = { ( 1 - v ) * ( 1 - v ), 2 * v * ( 1 - v ), v * v };
	giVert out;
	memset( &out, 0, sizeof( out ) );
	for ( int j = 0; j < 3; j++ ) {
		for ( int i = 0; i < 3; i++ ) {
			const giVert *p = c[j * 3 + i];
			float w = bu[i] * bv[j];
			out.xyz += p->xyz * w;
			out.normal += p->normal * w;
			for ( int s = 0; s < MAXLIGHTMAPS; s++ ) {
				out.lm[s][0] += p->lm[s][0] * w;
				out.lm[s][1] += p->lm[s][1] * w;
				out.color[s][0] += p->color[s][0] * w;
				out.color[s][1] += p->color[s][1] * w;
				out.color[s][2] += p->color[s][2] * w;
			}
		}
	}
	out.normal = gnorm( out.normal );
	return out;
}

static void gi_add_tri( giScene &scene, int surf, int a, int b, int c ) {
	giTri t;
	t.v0 = scene.verts[a].xyz;
	t.e1 = scene.verts[b].xyz - t.v0;
	t.e2 = scene.verts[c].xyz - t.v0;
	gvec3 n = gcross( t.e1, t.e2 );
	if ( gdot( n, n ) < 1e-8f )
		return;	// zero-area sliver: skip
	t.n = gnorm( n );
	// Q3 faces are wound clockwise, so the cross product points *into* the surface.
	// We simply make the face normal agree with the BSP's own vertex normals.
	const gvec3 vn = scene.verts[a].normal + scene.verts[b].normal + scene.verts[c].normal;
	if ( gdot( t.n, vn ) < 0 )
		t.n = t.n * -1.0f;
	t.surf = surf;
	t.vert[0] = a; t.vert[1] = b; t.vert[2] = c;
	scene.tris.push_back( t );
}

static void gi_load_vert( giVert &out, const mapVert_t &in ) {
	out.xyz = gvec3( LittleFloat( in.xyz[0] ), LittleFloat( in.xyz[1] ), LittleFloat( in.xyz[2] ) );
	out.normal = gnorm( gvec3( LittleFloat( in.normal[0] ), LittleFloat( in.normal[1] ), LittleFloat( in.normal[2] ) ) );
	for ( int s = 0; s < MAXLIGHTMAPS; s++ ) {
		out.lm[s][0] = LittleFloat( in.lightmap[s][0] );
		out.lm[s][1] = LittleFloat( in.lightmap[s][1] );
		out.color[s][0] = in.color[s][0];
		out.color[s][1] = in.color[s][1];
		out.color[s][2] = in.color[s][2];
	}
}

static void gi_build_geometry( giScene &scene, const byte *fileBase, const dheader_t *header ) {
	const lump_t &ls = header->lumps[LUMP_SURFACES];
	const lump_t &lv = header->lumps[LUMP_DRAWVERTS];
	const lump_t &li = header->lumps[LUMP_DRAWINDEXES];

	const dsurface_t *in = (const dsurface_t *)( fileBase + ls.fileofs );
	const mapVert_t *dv = (const mapVert_t *)( fileBase + lv.fileofs );
	const int *indexes = (const int *)( fileBase + li.fileofs );
	const int numSurfs = ls.filelen / sizeof( dsurface_t );
	const int numVerts = lv.filelen / sizeof( mapVert_t );
	const int numIndexes = li.filelen / sizeof( int );

	scene.surfs.resize( numSurfs );

	for ( int i = 0; i < numSurfs; i++ ) {
		const dsurface_t &s = in[i];
		giSurf &gs = scene.surfs[i];
		int shaderNum = LittleLong( s.shaderNum );
		if ( shaderNum < 0 || shaderNum >= (int)scene.shaders.size() )
			shaderNum = 0;
		gs.shader = shaderNum;
		for ( int k = 0; k < MAXLIGHTMAPS; k++ ) {
			gs.lightmapNum[k] = LittleLong( s.lightmapNum[k] );
			gs.lightmapStyle[k] = s.lightmapStyles[k];
		}
		const giShader &sh = scene.shaders[shaderNum];
		gs.sky = ( sh.surfaceFlags & SURF_SKY ) != 0;
		// water, fog, glass and other see-through surfaces let light pass
		gs.occluder = !( sh.surfaceFlags & SURF_NODRAW )
			&& !( sh.contentFlags & ( CONTENTS_WATER | CONTENTS_FOG | CONTENTS_TRANSLUCENT | CONTENTS_LAVA | CONTENTS_SLIME ) );

		const int type = LittleLong( s.surfaceType );
		const int firstVert = LittleLong( s.firstVert );
		const int nv = LittleLong( s.numVerts );
		if ( firstVert < 0 || nv <= 0 || firstVert + nv > numVerts )
			continue;

		if ( type == MST_PLANAR || type == MST_TRIANGLE_SOUP ) {
			// flat faces and model triangles: already triangles, just copy them
			const int firstIndex = LittleLong( s.firstIndex );
			const int ni = LittleLong( s.numIndexes );
			if ( firstIndex < 0 || firstIndex + ni > numIndexes )
				continue;
			const int base = (int)scene.verts.size();
			for ( int v = 0; v < nv; v++ ) {
				giVert gv;
				gi_load_vert( gv, dv[firstVert + v] );
				scene.verts.push_back( gv );
			}
			for ( int k = 0; k + 2 < ni; k += 3 ) {
				int a = LittleLong( indexes[firstIndex + k] );
				int b = LittleLong( indexes[firstIndex + k + 1] );
				int c = LittleLong( indexes[firstIndex + k + 2] );
				if ( a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv )
					continue;
				gi_add_tri( scene, i, base + a, base + b, base + c );
			}
		}
		else if ( type == MST_PATCH ) {
			// curved patches: a grid of control points, cut into small triangles here
			const int w = LittleLong( s.patchWidth );
			const int h = LittleLong( s.patchHeight );
			if ( w < 3 || h < 3 || w * h > nv || !( w & 1 ) || !( h & 1 ) )
				continue;
			std::vector<giVert> ctrl( w * h );
			for ( int v = 0; v < w * h; v++ )
				gi_load_vert( ctrl[v], dv[firstVert + v] );

			const int steps = DEVOGI_PATCH_STEPS;
			for ( int py = 0; py + 2 < h; py += 2 ) {
				for ( int px = 0; px + 2 < w; px += 2 ) {
					const giVert *c[9];
					for ( int j = 0; j < 3; j++ )
						for ( int k = 0; k < 3; k++ )
							c[j * 3 + k] = &ctrl[( py + j ) * w + px + k];
					const int base = (int)scene.verts.size();
					for ( int j = 0; j <= steps; j++ )
						for ( int k = 0; k <= steps; k++ )
							scene.verts.push_back( gi_patch_eval( c, k / (float)steps, j / (float)steps ) );
					for ( int j = 0; j < steps; j++ ) {
						for ( int k = 0; k < steps; k++ ) {
							int v00 = base + j * ( steps + 1 ) + k;
							int v10 = v00 + 1;
							int v01 = v00 + steps + 1;
							int v11 = v01 + 1;
							gi_add_tri( scene, i, v00, v10, v11 );
							gi_add_tri( scene, i, v00, v11, v01 );
						}
					}
				}
			}
		}
		// flares and broken surfaces have no geometry
	}
}

/*
===========================================================================
Ray tracer

A bounding volume hierarchy (BVH): triangles are sorted into nested boxes,
so a ray only has to test the few triangles in the boxes it passes through.
Built once per bake, then shared read-only by all worker threads.
===========================================================================
*/

static void gi_tri_bounds( const giTri &t, gvec3 &mins, gvec3 &maxs ) {
	gvec3 b = t.v0 + t.e1, c = t.v0 + t.e2;
	mins = gmin( t.v0, gmin( b, c ) );
	maxs = gmax( t.v0, gmax( b, c ) );
}

static void gi_build_bvh( giScene &scene ) {
	const int n = (int)scene.tris.size();
	scene.triIndex.resize( n );
	for ( int i = 0; i < n; i++ )
		scene.triIndex[i] = i;
	scene.nodes.clear();
	scene.nodes.reserve( n * 2 + 1 );
	if ( !n )
		return;

	std::vector<gvec3> centers( n ), tmins( n ), tmaxs( n );
	for ( int i = 0; i < n; i++ ) {
		gi_tri_bounds( scene.tris[i], tmins[i], tmaxs[i] );
		centers[i] = ( tmins[i] + tmaxs[i] ) * 0.5f;
	}

	struct buildItem { int node, first, count; };
	std::vector<buildItem> stack;
	scene.nodes.push_back( giBVHNode() );
	stack.push_back( { 0, 0, n } );

	// split every box in half along its longest axis until at most 4 triangles remain
	while ( !stack.empty() ) {
		buildItem it = stack.back();
		stack.pop_back();

		gvec3 mins( DEVOGI_INF, DEVOGI_INF, DEVOGI_INF ), maxs( -DEVOGI_INF, -DEVOGI_INF, -DEVOGI_INF );
		gvec3 cmins = mins, cmaxs = maxs;
		for ( int i = it.first; i < it.first + it.count; i++ ) {
			int t = scene.triIndex[i];
			mins = gmin( mins, tmins[t] );
			maxs = gmax( maxs, tmaxs[t] );
			cmins = gmin( cmins, centers[t] );
			cmaxs = gmax( cmaxs, centers[t] );
		}
		giBVHNode &node = scene.nodes[it.node];
		node.mins = mins;
		node.maxs = maxs;

		if ( it.count <= 4 ) {
			node.left = it.first;
			node.count = it.count;
			continue;
		}

		gvec3 ext = cmaxs - cmins;
		int axis = ( ext.x > ext.y && ext.x > ext.z ) ? 0 : ( ext.y > ext.z ? 1 : 2 );
		int mid = it.first + it.count / 2;
		std::nth_element( scene.triIndex.begin() + it.first, scene.triIndex.begin() + mid,
			scene.triIndex.begin() + it.first + it.count,
			[&]( int a, int b ) { return centers[a][axis] < centers[b][axis]; } );

		int left = (int)scene.nodes.size();
		scene.nodes.push_back( giBVHNode() );
		scene.nodes.push_back( giBVHNode() );
		scene.nodes[it.node].left = left;
		scene.nodes[it.node].count = 0;
		stack.push_back( { left, it.first, mid - it.first } );
		stack.push_back( { left + 1, mid, it.first + it.count - mid } );
	}
}

static inline bool gi_ray_box( const gvec3 &o, const gvec3 &inv, const gvec3 &mins, const gvec3 &maxs, float tmax ) {
	float t0 = 0, t1 = tmax;
	for ( int a = 0; a < 3; a++ ) {
		float tn = ( mins[a] - o[a] ) * inv[a];
		float tf = ( maxs[a] - o[a] ) * inv[a];
		if ( tn > tf ) std::swap( tn, tf );
		t0 = tn > t0 ? tn : t0;
		t1 = tf < t1 ? tf : t1;
		if ( t0 > t1 )
			return false;
	}
	return true;
}

struct giHit {
	float	t;		// distance along the ray
	int		tri;	// triangle that was hit
	float	u, v;	// where on that triangle (barycentric)
};

// Finds the nearest hit along a ray (or any hit, for shadow rays when anyHit is set).
// Triangles are two-sided, so we also find the back of walls - that is how we notice
// a point that sits inside geometry.
static bool gi_trace( const giScene &scene, const gvec3 &o, const gvec3 &d, float tmax, giHit &hit, bool anyHit, float tmin = 0.01f ) {
	if ( scene.nodes.empty() )
		return false;
	gvec3 inv( 1.0f / ( fabsf( d.x ) > 1e-9f ? d.x : 1e-9f ),
			   1.0f / ( fabsf( d.y ) > 1e-9f ? d.y : 1e-9f ),
			   1.0f / ( fabsf( d.z ) > 1e-9f ? d.z : 1e-9f ) );
	hit.t = tmax;
	hit.tri = -1;

	int stack[64];
	int sp = 0;
	stack[sp++] = 0;
	while ( sp ) {
		const giBVHNode &node = scene.nodes[stack[--sp]];
		if ( !gi_ray_box( o, inv, node.mins, node.maxs, hit.t ) )
			continue;
		if ( node.count ) {
			for ( int i = node.left; i < node.left + node.count; i++ ) {
				const int ti = scene.triIndex[i];
				const giTri &t = scene.tris[ti];
				if ( !scene.surfs[t.surf].occluder && !scene.surfs[t.surf].sky )
					continue;
				// Moller-Trumbore ray / triangle test
				gvec3 p = gcross( d, t.e2 );
				float det = gdot( t.e1, p );
				if ( fabsf( det ) < 1e-9f )
					continue;
				float idet = 1.0f / det;
				gvec3 s = o - t.v0;
				float u = gdot( s, p ) * idet;
				if ( u < 0 || u > 1 )
					continue;
				gvec3 q = gcross( s, t.e1 );
				float v = gdot( d, q ) * idet;
				if ( v < 0 || u + v > 1 )
					continue;
				float tt = gdot( t.e2, q ) * idet;
				if ( tt > tmin && tt < hit.t ) {
					hit.t = tt;
					hit.tri = ti;
					hit.u = u;
					hit.v = v;
					if ( anyHit )
						return true;
				}
			}
		}
		else if ( sp < 62 ) {
			stack[sp++] = node.left;
			stack[sp++] = node.left + 1;
		}
	}
	return hit.tri >= 0;
}

/*
===========================================================================
Shader helpers: surface colours and the sky's sun colour
===========================================================================
*/

// the text of a shader, from the host (the game's or the compiler's shader scripts)
static const char *R_DevoGI_FindShaderText( const char *shaderName ) {
	return ( g_host && g_host->shaderText ) ? g_host->shaderText( shaderName ) : NULL;
}

// first real texture of a shader ("map"/"clampmap" stage), or the shader name itself
static void gi_shader_texture( const char *shaderName, char *out, int outSize ) {
	Q_strncpyz( out, shaderName, outSize );
	const char *p = R_DevoGI_FindShaderText( shaderName );
	if ( !p )
		return;
	int depth = 0;
	while ( 1 ) {
		const char *token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		if ( token[0] == '{' ) { depth++; continue; }
		if ( token[0] == '}' ) { if ( --depth <= 0 ) break; continue; }
		if ( depth >= 2 && ( !Q_stricmp( token, "map" ) || !Q_stricmp( token, "clampmap" ) ) ) {
			token = COM_ParseExt( &p, qfalse );
			if ( token[0] && token[0] != '$' && token[0] != '*' ) {
				Q_strncpyz( out, token, outSize );
				return;
			}
		}
	}
}

// Average colour of a surface's texture = how it tints the light it reflects.
static gvec3 gi_shader_albedo( const char *shaderName ) {
	char texName[MAX_QPATH];
	gi_shader_texture( shaderName, texName, sizeof( texName ) );
	COM_StripExtension( texName, texName, sizeof( texName ) );

	byte *pic = NULL;
	int w = 0, h = 0;
	dg_load_image( texName, &pic, &w, &h );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic )
			dg_free_image( pic );
		return gvec3( 0.5f, 0.5f, 0.5f );	// unknown texture: a neutral grey
	}

	double r = 0, g = 0, b = 0, a = 0;
	const int step = ( w * h > 65536 ) ? 4 : 1;
	for ( int i = 0; i < w * h; i += step ) {
		const float al = pic[i * 4 + 3] / 255.0f;
		r += pic[i * 4 + 0] * al;
		g += pic[i * 4 + 1] * al;
		b += pic[i * 4 + 2] * al;
		a += al;
	}
	dg_free_image( pic );
	if ( a < 1 )
		return gvec3( 0.5f, 0.5f, 0.5f );

	gvec3 c( (float)( r / a / 255.0 ), (float)( g / a / 255.0 ), (float)( b / a / 255.0 ) );
	// like q3map2's radiosity: nothing reflects more than 90%, so light can't build up forever
	float m = Q_max( c.x, Q_max( c.y, c.z ) );
	if ( m > 0.9f )
		c = c * ( 0.9f / m );
	return c;
}

// the sun colour of a sky shader (from q3map_sun / q3map_sunExt / sun), brightest channel = 1
static bool gi_sky_sun_color( const char *shaderName, gvec3 &tint ) {
	const char *p = R_DevoGI_FindShaderText( shaderName );
	if ( !p )
		return false;
	int depth = 0;
	while ( 1 ) {
		const char *token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			return false;
		if ( token[0] == '{' ) { depth++; continue; }
		if ( token[0] == '}' ) { if ( --depth <= 0 ) return false; continue; }
		if ( !Q_stricmp( token, "q3map_sun" ) || !Q_stricmp( token, "q3map_sunExt" ) || !Q_stricmp( token, "sun" ) ) {
			float c[3];
			for ( int i = 0; i < 3; i++ )
				c[i] = atof( COM_ParseExt( &p, qfalse ) );
			const float m = Q_max( c[0], Q_max( c[1], c[2] ) );
			if ( m <= 1e-4f )
				return false;
			tint = gvec3( c[0] / m, c[1] / m, c[2] / m );
			return true;
		}
	}
}

/*
===========================================================================
Light entities kept in the BSP

Usually q3map2 removes "light" entities from the compiled map, but maps
compiled with -keeplights (or by DevoQ3Map2) still have them.
===========================================================================
*/

struct giPointLight {
	gvec3	origin;
	gvec3	color;		// colour already multiplied by strength
	float	radius;
	bool	linear;
};

static void gi_parse_lights( const char *ents, int len, std::vector<giPointLight> &lights ) {
	if ( !ents || len <= 0 )
		return;
	std::vector<char> text( ents, ents + len );
	text.push_back( 0 );
	const char *p = text.data();

	while ( 1 ) {
		const char *token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		if ( token[0] != '{' )
			continue;

		char classname[MAX_QPATH] = "";
		float origin[3] = { 0, 0, 0 }, color[3] = { 1, 1, 1 };
		float intensity = 300;
		int style = 0, spawnflags = 0;
		bool hasTarget = false;

		while ( 1 ) {
			char key[MAX_KEY];
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] || token[0] == '}' )
				break;
			Q_strncpyz( key, token, sizeof( key ) );
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] )
				break;
			if ( !Q_stricmp( key, "classname" ) )
				Q_strncpyz( classname, token, sizeof( classname ) );
			else if ( !Q_stricmp( key, "origin" ) )
				sscanf( token, "%f %f %f", &origin[0], &origin[1], &origin[2] );
			else if ( !Q_stricmp( key, "light" ) || !Q_stricmp( key, "_light" ) )
				intensity = atof( token );
			else if ( !Q_stricmp( key, "_color" ) || !Q_stricmp( key, "color" ) )
				sscanf( token, "%f %f %f", &color[0], &color[1], &color[2] );
			else if ( !Q_stricmp( key, "style" ) )
				style = atoi( token );
			else if ( !Q_stricmp( key, "spawnflags" ) )
				spawnflags = atoi( token );
			else if ( !Q_stricmp( key, "targetname" ) )
				hasTarget = true;
		}

		if ( Q_stricmp( classname, "light" ) )
			continue;
		// switchable / flickering lights live in their own light style and are left alone
		if ( style != 0 || hasTarget || intensity <= 0 )
			continue;

		float m = Q_max( color[0], Q_max( color[1], color[2] ) );
		if ( m > 1.0f ) { color[0] /= m; color[1] /= m; color[2] /= m; }

		giPointLight l;
		l.origin = gvec3( origin );
		l.linear = ( spawnflags & 1 ) != 0;
		// q3map2's point light: strength * 7500, fading with the square of the distance
		const float photons = intensity * ( l.linear ? 1.0f : 7500.0f );
		l.color = gvec3( color ) * photons;
		l.radius = l.linear ? intensity * 8.0f : sqrtf( photons );
		lights.push_back( l );
	}
}

/*
===========================================================================
Lightmap texels

A lightmap page is a grid of texels. For every texel that a surface covers
we work out where it is in the world (pos), which way it faces (normal) and
how big it is in game units (size). Everything below works on these.
===========================================================================
*/

struct giTexel {
	gvec3	pos;		// sample point in the world
	gvec3	normal;		// smooth shading normal
	gvec3	faceNormal;	// normal of the flat triangle underneath (used for ray offsets)
	float	size;		// edge length of this texel in game units
	bool	edge;		// its centre lies just outside its triangle (a border texel)
	int		page;
	int		x, y;
	int		style;		// light style slot of the owning surface (0..3)
	int		styleValue;	// the light style in that slot (0 = always on)
	int		surf;
};

struct giBake {
	giScene						scene;
	std::vector<giTexel>		texels;
	std::vector<int>			owner;		// per page pixel: which texel lives there (-1 = none)
	std::vector<int>			bspSurf;	// per original 128x128 page pixel: owning surface, or -1
											// (also -1 for border texels buried in a wall)
	std::vector<float>			direct;		// direct light, per page pixel rgb
	std::vector<float>			light;		// direct + bounce light so far, per page pixel rgb
	std::vector<float>			indirect;	// bounce light per texel: rgb + ambient occlusion
	std::vector<float>			sunVis;		// how much of the sun each texel sees, 0..1 (for live light)
	int							numPages;
	gvec3						skyColor;
	float						texelSize;	// typical texel size (median), for filter radii
};

// Draws one triangle into its lightmap page, creating a texel for each pixel it covers.
static void gi_rasterize_tri( giBake &bake, const giTri &t, int style, int page ) {
	const giScene &scene = bake.scene;
	const giVert *v[3] = { &scene.verts[t.vert[0]], &scene.verts[t.vert[1]], &scene.verts[t.vert[2]] };
	float px[3], py[3];
	for ( int i = 0; i < 3; i++ ) {
		px[i] = v[i]->lm[style][0] * DEVOGI_LM_SIZE;
		py[i] = v[i]->lm[style][1] * DEVOGI_LM_SIZE;
	}
	const float area = ( px[1] - px[0] ) * ( py[2] - py[0] ) - ( px[2] - px[0] ) * ( py[1] - py[0] );
	if ( fabsf( area ) < 1e-6f )
		return;

	// texel size = sqrt( world area / lightmap area ) of this triangle
	const gvec3 cr = gcross( t.e1, t.e2 );
	const float worldArea = 0.5f * sqrtf( gdot( cr, cr ) );
	const float texelSize = sqrtf( worldArea / ( 0.5f * fabsf( area ) ) );

	int x0 = Q_max( 0, (int)floorf( Q_min( px[0], Q_min( px[1], px[2] ) ) - 1 ) );
	int y0 = Q_max( 0, (int)floorf( Q_min( py[0], Q_min( py[1], py[2] ) ) - 1 ) );
	int x1 = Q_min( DEVOGI_LM_SIZE - 1, (int)ceilf( Q_max( px[0], Q_max( px[1], px[2] ) ) + 1 ) );
	int y1 = Q_min( DEVOGI_LM_SIZE - 1, (int)ceilf( Q_max( py[0], Q_max( py[1], py[2] ) ) + 1 ) );

	// a little slack so the border texels (half outside the triangle) get covered too
	const float slack = 0.75f / sqrtf( fabsf( area ) );

	for ( int y = y0; y <= y1; y++ ) {
		for ( int x = x0; x <= x1; x++ ) {
			const float cx = x + 0.5f, cy = y + 0.5f;
			float w1 = ( ( cx - px[0] ) * ( py[2] - py[0] ) - ( px[2] - px[0] ) * ( cy - py[0] ) ) / area;
			float w2 = ( ( px[1] - px[0] ) * ( cy - py[0] ) - ( cx - px[0] ) * ( py[1] - py[0] ) ) / area;
			float w0 = 1 - w1 - w2;
			if ( w0 < -slack || w1 < -slack || w2 < -slack )
				continue;
			const int pix = ( page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x;
			const bool inside = w0 >= 0 && w1 >= 0 && w2 >= 0;
			if ( bake.owner[pix] >= 0 && !inside )
				continue;	// already taken; only a texel truly inside its triangle may replace it
			w0 = Q_max( 0.0f, w0 ); w1 = Q_max( 0.0f, w1 ); w2 = Q_max( 0.0f, w2 );
			const float ws = w0 + w1 + w2;
			w0 /= ws; w1 /= ws; w2 /= ws;

			giTexel tx;
			tx.pos = v[0]->xyz * w0 + v[1]->xyz * w1 + v[2]->xyz * w2;
			tx.normal = gnorm( v[0]->normal * w0 + v[1]->normal * w1 + v[2]->normal * w2 );
			if ( gdot( tx.normal, t.n ) < 0.2f )
				tx.normal = t.n;	// broken smoothing normals: use the flat face normal
			tx.faceNormal = t.n;
			tx.size = texelSize;
			tx.edge = !inside;
			tx.page = page;
			tx.x = x;
			tx.y = y;
			tx.style = style;
			tx.styleValue = scene.surfs[t.surf].lightmapStyle[style];
			tx.surf = t.surf;

			if ( bake.owner[pix] >= 0 ) {
				bake.texels[bake.owner[pix]] = tx;
			} else {
				bake.owner[pix] = (int)bake.texels.size();
				bake.texels.push_back( tx );
			}
		}
	}
}

static void gi_build_texels( giBake &bake ) {
	const giScene &scene = bake.scene;
	bake.owner.assign( bake.numPages * DEVOGI_LM_SIZE * DEVOGI_LM_SIZE, -1 );
	for ( const giTri &t : scene.tris ) {
		const giSurf &s = scene.surfs[t.surf];
		if ( s.sky )
			continue;
		for ( int k = 0; k < MAXLIGHTMAPS; k++ ) {
			if ( s.lightmapStyle[k] == LS_LSNONE )
				continue;
			const int page = s.lightmapNum[k];
			if ( page < 0 || page >= bake.numPages )
				continue;
			gi_rasterize_tri( bake, t, k, page );
		}
	}
}

/*
===========================================================================
Bake progress screen

The bake can take a while. While a big step runs, the worker threads do the
work and the calling thread only reports progress to the host: the game
draws a progress screen, the compiler prints a percentage. (The game must
draw from its main thread, which is why the calling thread does it.)
===========================================================================
*/

static bool			gi_progressOn;
static const char	*gi_progressStage = "";
static float		gi_progressBase, gi_progressSpan;
static int			gi_progressLastDraw, gi_progressLastPrint;
static char			gi_progressMap[MAX_QPATH];

static void gi_progress_draw( float stageFrac, bool force ) {
	if ( !gi_progressOn )
		return;
	const float overall = Com_Clamp( 0.0f, 1.0f, gi_progressBase + gi_progressSpan * Com_Clamp( 0.0f, 1.0f, stageFrac ) );
	const int pct = (int)( overall * 100.0f );
	const int now = dg_ms();

	if ( pct / 10 != gi_progressLastPrint / 10 || force ) {
		dg_print( "DevoGI: baking %s: %s %d%%\n", gi_progressMap, gi_progressStage, pct );
		gi_progressLastPrint = pct;
	}

	if ( !force && now - gi_progressLastDraw < 100 )
		return;	// ten updates a second is plenty
	gi_progressLastDraw = now;
	if ( g_host && g_host->progress )
		g_host->progress( gi_progressMap, gi_progressStage, overall, force );
}

// starts a step that covers [base, base + span] of the whole bake (0..1)
static void gi_progress_stage( const char *name, float base, float span ) {
	gi_progressOn = true;
	gi_progressStage = name;
	gi_progressBase = base;
	gi_progressSpan = span;
	gi_progress_draw( 0.0f, true );
}

static void gi_progress_end( void ) {
	if ( gi_progressOn ) {
		gi_progressBase = 1.0f;
		gi_progressSpan = 0.0f;
		gi_progressStage = "done";
		gi_progress_draw( 1.0f, true );
	}
	gi_progressOn = false;
}

/*
===========================================================================
Threads

gi_parallel_for( n, func ) runs func(0) .. func(n-1) on all CPU cores, in
chunks of 256. With report = true the loop is a whole progress step: the
workers take every core and the main thread draws the progress screen.
===========================================================================
*/

template<typename F>
static void gi_parallel_for( int count, F func, bool report = false ) {
	int threads = g_set.threads;
	if ( threads <= 0 )
		threads = (int)std::thread::hardware_concurrency();
	threads = Com_Clampi( 1, 64, threads );

	std::atomic<int> next( 0 ), finished( 0 );
	auto worker = [&]() {
		while ( 1 ) {
			const int start = next.fetch_add( 256 );
			if ( start >= count )
				break;
			const int end = Q_min( count, start + 256 );
			for ( int i = start; i < end; i++ )
				func( i );
			finished.fetch_add( end - start );
		}
	};
	std::vector<std::thread> pool;
	if ( report && gi_progressOn && count > 0 ) {
		for ( int i = 0; i < threads; i++ )
			pool.emplace_back( worker );
		while ( finished.load() < count ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
			gi_progress_draw( (float)finished.load() / count, false );
		}
	} else {
		for ( int i = 1; i < threads; i++ )
			pool.emplace_back( worker );
		worker();
	}
	for ( std::thread &t : pool )
		t.join();
}

/*
===========================================================================
World-space neighbourhoods

Lightmap pages are a patchwork: two polygons that touch in the world are
usually far apart in the lightmap. If we smoothed in lightmap space, every
polygon would be smoothed on its own and its edges would show as seams.

So all smoothing here works in the *world*: points go into a grid of cubes
(a spatial hash), and "neighbours" are the points within a radius in the
world, whatever polygon they belong to. Only points facing the same way
(and lying in the same plane) are mixed, so light never leaks around
corners.
===========================================================================
*/

struct giSpatialHash {
	float					cell = 1.0f;		// cube size in game units
	std::vector<int>		items;				// item indices, sorted by cube
	std::vector<uint64_t>	slotKey;			// open-addressing table: cube key ...
	std::vector<int>		slotStart;			// ... -> first entry in items
	std::vector<int>		slotCount;			// ... -> number of entries
	size_t					mask = 0;

	// a cube's coordinates packed exactly into 63 bits (21 bits per axis)
	static uint64_t key( int x, int y, int z ) {
		return ( (uint64_t)( x + ( 1 << 20 ) ) << 42 ) | ( (uint64_t)( y + ( 1 << 20 ) ) << 21 ) | (uint64_t)( z + ( 1 << 20 ) );
	}
	static size_t mix( uint64_t k ) {
		k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
		return (size_t)k;
	}

	void build( const std::vector<gvec3> &pos, const std::vector<int> &which, float cellSize ) {
		cell = Q_max( 0.5f, cellSize );
		const float inv = 1.0f / cell;
		std::vector<std::pair<uint64_t, int>> keyed( which.size() );
		for ( size_t i = 0; i < which.size(); i++ ) {
			const gvec3 &p = pos[which[i]];
			keyed[i] = std::make_pair( key( (int)floorf( p.x * inv ), (int)floorf( p.y * inv ), (int)floorf( p.z * inv ) ), which[i] );
		}
		std::sort( keyed.begin(), keyed.end() );
		items.resize( keyed.size() );
		size_t distinct = 0;
		for ( size_t i = 0; i < keyed.size(); i++ ) {
			items[i] = keyed[i].second;
			if ( i == 0 || keyed[i].first != keyed[i - 1].first )
				distinct++;
		}
		size_t size = 16;
		while ( size < distinct * 2 )
			size *= 2;
		mask = size - 1;
		slotKey.assign( size, ~0ULL );
		slotStart.assign( size, 0 );
		slotCount.assign( size, 0 );
		for ( size_t i = 0; i < keyed.size(); ) {
			size_t j = i;
			while ( j < keyed.size() && keyed[j].first == keyed[i].first )
				j++;
			size_t s = mix( keyed[i].first ) & mask;
			while ( slotKey[s] != ~0ULL )
				s = ( s + 1 ) & mask;
			slotKey[s] = keyed[i].first;
			slotStart[s] = (int)i;
			slotCount[s] = (int)( j - i );
			i = j;
		}
	}

	// calls f( item ) for everything in the 27 cubes around p (radius up to one cube)
	template<typename F>
	void query( const gvec3 &p, F f ) const {
		if ( items.empty() )
			return;
		const float inv = 1.0f / cell;
		const int cx = (int)floorf( p.x * inv ), cy = (int)floorf( p.y * inv ), cz = (int)floorf( p.z * inv );
		for ( int z = cz - 1; z <= cz + 1; z++ )
			for ( int y = cy - 1; y <= cy + 1; y++ )
				for ( int x = cx - 1; x <= cx + 1; x++ ) {
					const uint64_t k = key( x, y, z );
					size_t s = mix( k ) & mask;
					while ( slotKey[s] != ~0ULL ) {
						if ( slotKey[s] == k ) {
							for ( int i = slotStart[s]; i < slotStart[s] + slotCount[s]; i++ )
								f( items[i] );
							break;
						}
						s = ( s + 1 ) & mask;
					}
				}
	}
};

// may two texels share light? same light style, facing the same way, lying in the same plane
static inline bool gi_texels_compatible( const giTexel &a, const giTexel &b, float planeTolerance ) {
	return a.styleValue == b.styleValue
		&& gdot( a.normal, b.normal ) > 0.8f	// up to ~37 degrees apart: blends across bevels and curves
		&& fabsf( gdot( b.pos - a.pos, a.faceNormal ) ) <= planeTolerance;
}

/*
Smooths a per-texel value in world space with a soft (Gaussian-like) falloff.
Used on the traced direct light: it anti-aliases stair-stepped shadow edges and
removes sampling noise, and because it works across polygons, neighbouring
polygons end up with matching light at their shared edge (no seams).
*/
template<typename T>
static void gi_world_smooth( const giBake &bake, const giSpatialHash &hash, std::vector<T> &values, float radius ) {
	std::vector<T> out( values.size() );
	const float r2 = radius * radius;
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		T sum = T();
		float wsum = 0;
		hash.query( tx.pos, [&]( int o ) {
			const giTexel &ot = bake.texels[o];
			const gvec3 d = ot.pos - tx.pos;
			const float d2 = gdot( d, d );
			if ( d2 > r2 || !gi_texels_compatible( tx, ot, radius * 0.75f ) )
				return;
			const float w = 1.0f - d2 / r2;	// soft falloff towards the edge of the radius
			sum += values[o] * w;
			wsum += w;
		} );
		out[i] = wsum > 0 ? sum * ( 1.0f / wsum ) : values[i];
	} );
	values.swap( out );
}

/*
===========================================================================
Keeping sample points out of walls

A lightmap texel is a little square. Its centre often lands *inside* a
wall or under a trim that sits on the floor: floors are not cut where
detail geometry stands on them, and border texels poke over the edge of
their polygon. A sample point inside geometry sees light from the wrong
side (a bright leaking line) or no light at all (a black edge).

So we look a little way along the surface in four directions. If a ray
hits the *back* of a wall, we are inside it: the point moves out through
that wall into the open, just past its surface.
===========================================================================
*/

static void gi_fix_texel_positions( giBake &bake ) {
	const giScene &scene = bake.scene;
	std::atomic<int> moved( 0 );
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		giTexel &tx = bake.texels[i];
		gvec3 t, b;
		gi_basis( tx.faceNormal, t, b );
		const float reach = tx.size * 0.75f + 1.0f;	// the corner of a texel is ~0.7 texels away
		for ( int pass = 0; pass < 2; pass++ ) {
			const gvec3 o = tx.pos + tx.faceNormal * 0.1f;
			const gvec3 dirs[4] = { t, t * -1.0f, b, b * -1.0f };
			bool fixedOne = false;
			for ( const gvec3 &d : dirs ) {
				giHit hit;
				if ( !gi_trace( scene, o, d, reach, hit, false ) )
					continue;
				const giTri &ht = scene.tris[hit.tri];
				if ( scene.surfs[ht.surf].sky || gdot( ht.n, d ) <= 0 )
					continue;	// a wall's front face: we are outside it, all good
				// we hit the back of a wall: step out through it, a little past its surface
				tx.pos = o + d * ( hit.t + 0.5f ) - tx.faceNormal * 0.1f;
				fixedOne = true;
				break;
			}
			if ( !fixedOne )
				break;
			moved.fetch_add( 1 );
		}
	} );
	dg_print( "DevoGI: moved %d texel sample points out of walls\n", moved.load() );
}

/*
===========================================================================
Upsampling the original lightmaps

Used for light styles we don't relight (switchable lamps) and for maps we
can't relight at all: the old 128x128 light is enlarged smoothly, mixing
only texels of the same surface so light never jumps between unrelated
polygons that happen to sit next to each other in the page.
===========================================================================
*/

static void gi_sample_bsp_page( const byte *src, int page, float fx, float fy, float out[3] ) {
	const int N = DEVOGI_BSP_LM_SIZE;
	fx = Com_Clamp( 0.0f, (float)( N - 1 ), fx );
	fy = Com_Clamp( 0.0f, (float)( N - 1 ), fy );
	const int x0 = (int)fx, y0 = (int)fy;
	const int x1 = Q_min( N - 1, x0 + 1 ), y1 = Q_min( N - 1, y0 + 1 );
	const float ax = fx - x0, ay = fy - y0;
	const byte *p00 = src + ( ( page * N + y0 ) * N + x0 ) * 3;
	const byte *p10 = src + ( ( page * N + y0 ) * N + x1 ) * 3;
	const byte *p01 = src + ( ( page * N + y1 ) * N + x0 ) * 3;
	const byte *p11 = src + ( ( page * N + y1 ) * N + x1 ) * 3;
	for ( int c = 0; c < 3; c++ )
		out[c] = ( p00[c] * ( 1 - ax ) + p10[c] * ax ) * ( 1 - ay ) + ( p01[c] * ( 1 - ax ) + p11[c] * ax ) * ay;
}

// which surface owns each texel of the *original* 128x128 pages
static void gi_build_bsp_owner( giBake &bake ) {
	const int saved = gi_lmSize;
	gi_lmSize = DEVOGI_BSP_LM_SIZE;
	std::vector<giTexel> savedTexels;
	savedTexels.swap( bake.texels );
	gi_build_texels( bake );
	bake.bspSurf.assign( bake.owner.size(), -1 );

	// a border texel whose centre is buried in a wall was baked black by q3map2:
	// leave it out, so those dark seams don't get enlarged too
	const giScene &scene = bake.scene;
	std::vector<char> buried( bake.texels.size(), 0 );
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		if ( !tx.edge )
			return;
		gvec3 t, b;
		gi_basis( tx.faceNormal, t, b );
		const gvec3 o = tx.pos + tx.faceNormal * 0.25f;
		const gvec3 dirs[5] = { tx.faceNormal, t, t * -1.0f, b, b * -1.0f };
		for ( const gvec3 &d : dirs ) {
			giHit hit;
			if ( gi_trace( scene, o, d, 4.0f, hit, false ) && !scene.surfs[scene.tris[hit.tri].surf].sky
				&& gdot( scene.tris[hit.tri].n, d ) > 0 ) {
				buried[i] = 1;
				return;
			}
		}
	} );

	for ( size_t i = 0; i < bake.owner.size(); i++ )
		if ( bake.owner[i] >= 0 && !buried[bake.owner[i]] )
			bake.bspSurf[i] = bake.texels[bake.owner[i]].surf;
	bake.texels.swap( savedTexels );
	bake.owner.clear();
	gi_lmSize = saved;
}

static void gi_upsample_direct( giBake &bake, const byte *src ) {
	const int N = DEVOGI_BSP_LM_SIZE;
	const float scale = (float)DEVOGI_LM_SIZE / N;
	bake.direct.assign( (size_t)bake.numPages * DEVOGI_LM_SIZE * DEVOGI_LM_SIZE * 3, 0.0f );

	gi_parallel_for( bake.numPages * DEVOGI_LM_SIZE, [&]( int row ) {
		const int page = row / DEVOGI_LM_SIZE;
		const int y = row % DEVOGI_LM_SIZE;
		for ( int x = 0; x < DEVOGI_LM_SIZE; x++ ) {
			const int pix = ( page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x;
			float *out = &bake.direct[(size_t)pix * 3];
			const float fx = ( x + 0.5f ) / scale - 0.5f;
			const float fy = ( y + 0.5f ) / scale - 0.5f;
			const int owner = bake.owner[pix];
			if ( owner < 0 ) {
				gi_sample_bsp_page( src, page, fx, fy, out );
				continue;
			}
			const int surf = bake.texels[owner].surf;
			const int x0 = (int)floorf( fx ), y0 = (int)floorf( fy );
			const float ax = fx - x0, ay = fy - y0;
			float sum[3] = { 0, 0, 0 }, wsum = 0;
			for ( int j = 0; j < 2; j++ ) {
				for ( int i = 0; i < 2; i++ ) {
					const int sx = Com_Clampi( 0, N - 1, x0 + i ), sy = Com_Clampi( 0, N - 1, y0 + j );
					if ( bake.bspSurf[( page * N + sy ) * N + sx] != surf )
						continue;
					const float w = ( i ? ax : 1 - ax ) * ( j ? ay : 1 - ay ) + 1e-4f;
					const byte *p = src + ( ( page * N + sy ) * N + sx ) * 3;
					sum[0] += p[0] * w; sum[1] += p[1] * w; sum[2] += p[2] * w;
					wsum += w;
				}
			}
			if ( wsum <= 0 ) {
				// nothing of this surface right here: look a bit further (skips buried seams)
				const int cx = (int)( fx + 0.5f ), cy = (int)( fy + 0.5f );
				for ( int j = -2; j <= 2; j++ ) {
					for ( int i = -2; i <= 2; i++ ) {
						const int sx = Com_Clampi( 0, N - 1, cx + i ), sy = Com_Clampi( 0, N - 1, cy + j );
						if ( bake.bspSurf[( page * N + sy ) * N + sx] != surf )
							continue;
						const float w = 1.0f / ( 1.0f + i * i + j * j );
						const byte *p = src + ( ( page * N + sy ) * N + sx ) * 3;
						sum[0] += p[0] * w; sum[1] += p[1] * w; sum[2] += p[2] * w;
						wsum += w;
					}
				}
			}
			if ( wsum > 0 ) {
				out[0] = sum[0] / wsum; out[1] = sum[1] / wsum; out[2] = sum[2] / wsum;
			} else {
				const int sx = Com_Clampi( 0, N - 1, (int)( fx + 0.5f ) ), sy = Com_Clampi( 0, N - 1, (int)( fy + 0.5f ) );
				const byte *p = src + ( ( page * N + sy ) * N + sx ) * 3;
				out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
			}
		}
	} );
}

/*
===========================================================================
Direct light: soft shadows from lamps and the sun

For a point in the world we ask every nearby light "can you see me, and how
much of you?". Lamps are glowing *areas*, not points: we pick sample points
spread evenly across the lamp, and each one that the point can see adds its
share of light. Partly hidden lamps give a soft shadow edge (penumbra).

To keep this fast, every light first gets 4 probe rays. If they all agree
(fully lit or fully shadowed - true for most of the map) we are done; only
points in a penumbra, where the probes disagree, trace the full set.
===========================================================================
*/

// lights sorted into a coarse grid, so each point only looks at lights that can reach it
struct giLightGrid {
	float							cell;
	gvec3							origin;
	int								nx, ny, nz;
	std::vector<std::vector<int>>	cells;
};

static void gi_build_light_grid( giLightGrid &g ) {
	g.cell = 256.0f;
	g.cells.clear();
	g.nx = g.ny = g.nz = 0;
	if ( gi_lights.empty() )
		return;
	gvec3 mins( DEVOGI_INF, DEVOGI_INF, DEVOGI_INF ), maxs( -DEVOGI_INF, -DEVOGI_INF, -DEVOGI_INF );
	for ( const devoLight_t &lt : gi_lights ) {
		const gvec3 r( lt.radius, lt.radius, lt.radius );
		mins = gmin( mins, lt.origin - r );
		maxs = gmax( maxs, lt.origin + r );
	}
	g.origin = mins;
	auto dims = [&]() {
		g.nx = Q_max( 1, (int)ceilf( ( maxs.x - mins.x ) / g.cell ) );
		g.ny = Q_max( 1, (int)ceilf( ( maxs.y - mins.y ) / g.cell ) );
		g.nz = Q_max( 1, (int)ceilf( ( maxs.z - mins.z ) / g.cell ) );
	};
	dims();
	while ( (size_t)g.nx * g.ny * g.nz > 262144 ) {	// keep it small on huge maps
		g.cell *= 2.0f;
		dims();
	}
	g.cells.resize( (size_t)g.nx * g.ny * g.nz );
	for ( int i = 0; i < (int)gi_lights.size(); i++ ) {
		const devoLight_t &lt = gi_lights[i];
		const int x0 = Com_Clampi( 0, g.nx - 1, (int)( ( lt.origin.x - lt.radius - g.origin.x ) / g.cell ) );
		const int y0 = Com_Clampi( 0, g.ny - 1, (int)( ( lt.origin.y - lt.radius - g.origin.y ) / g.cell ) );
		const int z0 = Com_Clampi( 0, g.nz - 1, (int)( ( lt.origin.z - lt.radius - g.origin.z ) / g.cell ) );
		const int x1 = Com_Clampi( 0, g.nx - 1, (int)( ( lt.origin.x + lt.radius - g.origin.x ) / g.cell ) );
		const int y1 = Com_Clampi( 0, g.ny - 1, (int)( ( lt.origin.y + lt.radius - g.origin.y ) / g.cell ) );
		const int z1 = Com_Clampi( 0, g.nz - 1, (int)( ( lt.origin.z + lt.radius - g.origin.z ) / g.cell ) );
		for ( int z = z0; z <= z1; z++ )
			for ( int y = y0; y <= y1; y++ )
				for ( int x = x0; x <= x1; x++ )
					g.cells[( (size_t)z * g.ny + y ) * g.nx + x].push_back( i );
	}
}

/*
Direct light arriving at one point. Instead of returning a total, it calls
emit( group, rgb ) for every light group that reaches the point. The full bake
multiplies each group by its calibrated brightness; the calibration step uses
the per-group numbers to work out those brightnesses in the first place.
*/
template<typename EMIT>
static void gi_direct_point( const giBake &bake, const giLightGrid &grid, const gvec3 &pos, const gvec3 &normal,
	const gvec3 &faceNormal, uint32_t seed, float footprint, EMIT emit ) {
	const giScene &scene = bake.scene;
	uint32_t rng = gi_hash( seed ^ 0xA5A5A5A5u );
	const gvec3 o = pos + faceNormal * 1.0f;	// start rays just above the surface

	/*
	Anti-aliasing: a texel is a little square, not a point. Every shadow sample starts
	from a different spot inside that square (footprint = its size), so a shadow edge
	crossing the texel shades it partly - smooth edges instead of stair-steps.
	The 4 probe rays start from the texel's 4 quarters, so an edge anywhere inside the
	texel makes them disagree and the texel gets the full set of samples.
	*/
	gvec3 ft, fb;
	gi_basis( faceNormal, ft, fb );
	// the sampling area is r_devoGI_aa texels wide: wider than the texel itself, it acts
	// like a soft filter over the neighbours too, which hides the texel grid completely
	const float fp = footprint * Com_Clamp( 0.25f, 4.0f, g_set.aa );
	auto origin = [&]( float ux, float uy ) -> gvec3 {
		if ( fp <= 0 )
			return o;
		const gvec3 off = ft * ( ux * fp ) + fb * ( uy * fp );
		const float len = sqrtf( gdot( off, off ) );
		if ( len < 0.01f )
			return o;
		// a spread-out start must not end up inside a wall next to the texel (that would
		// leak light, or darken the corner): if the way there is blocked, use the centre
		giHit h;
		if ( gi_trace( scene, o, off * ( 1.0f / len ), len + 0.5f, h, true ) )
			return o;
		return o + off;
	};

	if ( grid.nx ) {
		const int cx = (int)( ( pos.x - grid.origin.x ) / grid.cell );
		const int cy = (int)( ( pos.y - grid.origin.y ) / grid.cell );
		const int cz = (int)( ( pos.z - grid.origin.z ) / grid.cell );
		if ( cx >= 0 && cy >= 0 && cz >= 0 && cx < grid.nx && cy < grid.ny && cz < grid.nz ) {
			for ( int li : grid.cells[( (size_t)cz * grid.ny + cy ) * grid.nx + cx] ) {
				const devoLight_t &lt = gi_lights[li];
				gvec3 L = lt.origin - o;
				const float d2 = gdot( L, L );
				if ( d2 > lt.radius * lt.radius || d2 < 1e-4f )
					continue;
				const float d = sqrtf( d2 );
				L = L * ( 1.0f / d );
				// very close to a big lamp the angles change across it, so no early rejection there
				const bool nearPatch = lt.size > 0 && d < lt.size;
				const float cosR = gdot( normal, L );
				if ( cosR <= 0 && !nearPatch )
					continue;	// the light is behind the surface
				float cosE = 1.0f;
				if ( lt.size > 0 ) {
					cosE = -gdot( lt.normal, L );
					if ( cosE <= 0 && !nearPatch )
						continue;	// we are behind the glowing surface
				}
				// a rough estimate from the lamp's centre, only to skip lights too weak to matter
				const float dd2 = Q_max( d2, lt.size * lt.size * 0.25f + 16.0f );
				if ( !nearPatch && lt.photons * cosR * cosE / dd2 < 0.25f )
					continue;

				// how many shadow samples a penumbra gets: more when we are close to the lamp.
				// Very close to a lamp, light changes fast across a texel: no shortcut there,
				// every texel gets the full set (that was the noise around small coloured lamps).
				int samples = 4;
				bool alwaysFull = false;
				if ( lt.size > 0 ) {
					if ( d < lt.size * 2.0f || d < 48.0f ) { samples = 36; alwaysFull = true; }
					else if ( d < lt.size * 4.0f ) samples = 16;
					else if ( d < lt.size * 8.0f ) samples = 9;
				}
				gvec3 et, eb;
				if ( lt.size > 0 )
					gi_basis( lt.normal, et, eb );
				// the lamp's own fixture right around the glowing part must not shadow it
				const float emitterClearance = 4.0f + lt.size * 0.25f;

				// one sample: from spot (ux, uy) of the texel to spot (jx, jy) of the lamp
				// (all -0.5..0.5 each way)
				auto lampSample = [&]( float ux, float uy, float jx, float jy, float &sum, int &vis, int &tested ) {
					gvec3 target = lt.origin;
					if ( lt.size > 0 )
						target += et * ( jx * lt.size ) + eb * ( jy * lt.size );
					const gvec3 so = origin( ux, uy );
					gvec3 Ls = target - so;
					const float ds2 = gdot( Ls, Ls );
					if ( ds2 < 1.0f )
						return;
					const float ds = sqrtf( ds2 );
					Ls = Ls * ( 1.0f / ds );
					const float cr = gdot( normal, Ls );
					const float ce = lt.size > 0 ? -gdot( lt.normal, Ls ) : 1.0f;
					if ( cr <= 0 || ce <= 0 )
						return;
					tested++;
					// each sample has its own distance and angles (a lamp is not one point)
					const float contrib = lt.photons * cr * ce / Q_max( ds2, 16.0f );
					giHit hit;
					if ( ds - emitterClearance <= DEVOGI_RAY_TMIN
						|| !gi_trace( scene, so, Ls, ds - emitterClearance, hit, true, DEVOGI_RAY_TMIN ) ) {
						sum += contrib;
						vis++;
					}
				};

				float sum = 0;
				int vis = 0, tested = 0, taken = 0;
				if ( !alwaysFull ) {
					// 4 probes: from the texel's 4 quarters, towards the lamp's 4 quarters
					for ( int s = 0; s < 4; s++, taken++ ) {
						const float qx = ( ( s & 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f;
						const float qy = ( ( s >> 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f;
						const float jx = lt.size > 0 ? ( ( ( s >> 1 ) ^ 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f : 0.0f;
						const float jy = lt.size > 0 ? ( ( s & 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f : 0.0f;
						lampSample( qx, qy, jx, jy, sum, vis, tested );
					}
				}
				if ( alwaysFull || ( vis > 0 && vis < tested ) ) {
					// a penumbra (or very near the lamp): evenly spread (stratified) samples
					// across both the texel and the lamp; texel spots are shuffled against
					// lamp spots so the two patterns don't line up
					const int strata = (int)ceilf( sqrtf( (float)samples ) );
					const uint32_t shuffle = gi_hash( seed ^ (uint32_t)li );
					for ( int s = 0; s < samples; s++, taken++ ) {
						const int t = (int)( ( (uint32_t)s * 7u + shuffle ) % (uint32_t)samples );
						const float ux = ( ( t % strata ) + gi_rand( rng ) ) / strata - 0.5f;
						const float uy = ( ( ( t / strata ) % strata ) + gi_rand( rng ) ) / strata - 0.5f;
						const float jx = lt.size > 0 ? ( ( s % strata ) + gi_rand( rng ) ) / strata - 0.5f : 0.0f;
						const float jy = lt.size > 0 ? ( ( ( s / strata ) % strata ) + gi_rand( rng ) ) / strata - 0.5f : 0.0f;
						lampSample( ux, uy, jx, jy, sum, vis, tested );
					}
				}
				if ( sum > 0 )
					emit( lt.group, lt.color * ( sum / (float)Q_max( 1, taken ) ) );
			}
		}
	}

	if ( gi_hasSun ) {
		const float cosS = gdot( normal, gi_sunDir );
		if ( cosS > 0 ) {
			// the sun is a small disc (~1.5 degrees), which gives sunlight soft edges too
			gvec3 st, sb;
			gi_basis( gi_sunDir, st, sb );
			// one sample: from spot (ux, uy) of the texel towards spot (jx, jy) of the sun disc
			auto sunVisible = [&]( float ux, float uy, float jx, float jy ) {
				const gvec3 dir = gnorm( gi_sunDir + st * ( jx * 0.05f ) + sb * ( jy * 0.05f ) );
				giHit hit;
				// sunlight arrives if the ray leaves the map or reaches a sky surface
				return !gi_trace( scene, origin( ux, uy ), dir, 16384.0f, hit, false, DEVOGI_RAY_TMIN )
					|| scene.surfs[scene.tris[hit.tri].surf].sky;
			};
			// 4 probes from the texel's corners towards opposite edges of the sun disc
			int visible = 0, samples = 4;
			const float px[4] = { -0.45f, 0.45f, -0.45f, 0.45f }, py[4] = { -0.45f, -0.45f, 0.45f, 0.45f };
			for ( int s = 0; s < 4; s++ )
				visible += sunVisible( px[s], py[s], -px[s], -py[s] ) ? 1 : 0;
			if ( visible > 0 && visible < 4 ) {
				// a shadow edge crosses this texel: 8x8 samples spread over texel and sun
				const int g = 8;
				visible = 0;
				samples = g * g;
				const uint32_t shuffle = gi_hash( seed ^ 0x51ED270Bu );
				for ( int s = 0; s < samples; s++ ) {
					const int t = (int)( ( (uint32_t)s * 13u + shuffle ) % (uint32_t)samples );
					const float ux = ( ( t % g ) + gi_rand( rng ) ) / g - 0.5f;
					const float uy = ( ( t / g ) + gi_rand( rng ) ) / g - 0.5f;
					const float jx = ( ( s % g ) + gi_rand( rng ) ) / g - 0.5f;
					const float jy = ( ( s / g ) + gi_rand( rng ) ) / g - 0.5f;
					visible += sunVisible( ux, uy, jx, jy ) ? 1 : 0;
				}
			}
			if ( visible )
				emit( gi_sunGroup, gi_sunColor * ( gi_sunIntensity * cosS * visible / (float)samples ) );
		}
	}
}

/*
===========================================================================
Calibration: how bright should each kind of lamp be?

q3map2 and the shader files give lights in different units, glowing lamp
textures had their light entities removed entirely, and the old lightmap
also holds q3map2's own bounce light. So instead of guessing, we ask the
old lightmap: "how bright did the designers make the places this lamp
lights?"

We take a sample of texels (about 1 in 12), compute how much each light
group would add there at strength 1, and find the strengths that best
reproduce the old lightmap: a least-squares fit, kept positive, and gently
pulled towards one common strength so a lamp type seen in only a few
places can't go wild. Result: every lamp type gets its own, matched
brightness - no more blown-out lamps next to dark ones.
===========================================================================
*/

struct giCalibSample {
	float					target;		// old lightmap brightness at this texel
	std::vector<std::pair<int, float>>	parts;	// (group, brightness at strength 1)
};

static void gi_calibrate( const giBake &bake, const giLightGrid &grid, const std::vector<float> &origLuma,
	std::vector<float> &groupScale ) {
	const int G = gi_numGroups;
	groupScale.assign( G, 1.0f );

	// 1. measure: a stable sample of always-on, non-border texels
	std::vector<int> pick;
	for ( int i = 0; i < (int)bake.texels.size(); i++ ) {
		const giTexel &tx = bake.texels[i];
		if ( tx.styleValue == 0 && !tx.edge && ( gi_hash( (uint32_t)i * 2654435761u ) % 12u ) == 0 )
			pick.push_back( i );
	}
	std::vector<giCalibSample> samples( pick.size() );
	gi_parallel_for( (int)pick.size(), [&]( int k ) {
		const int i = pick[k];
		const giTexel &tx = bake.texels[i];
		giCalibSample &cs = samples[k];
		cs.target = origLuma[i];
		gi_direct_point( bake, grid, tx.pos, tx.normal, tx.faceNormal, (uint32_t)i * 2654435761u, 0.0f,
			[&]( int group, const gvec3 &rgb ) {
				const float l = gi_luma( rgb );
				for ( auto &p : cs.parts )
					if ( p.first == group ) { p.second += l; return; }
				cs.parts.push_back( std::make_pair( group, l ) );
			} );
	}, true );

	// 2. normal equations of the least-squares problem: (A^T A) x = A^T b
	std::vector<double> AtA( (size_t)G * G, 0.0 ), Atb( G, 0.0 );
	double sumSS = 0, sumBS = 0;		// for one shared strength (the "prior")
	for ( const giCalibSample &cs : samples ) {
		double total = 0;
		for ( const auto &p : cs.parts ) {
			Atb[p.first] += cs.target * p.second;
			total += p.second;
			for ( const auto &q : cs.parts )
				AtA[(size_t)p.first * G + q.first] += (double)p.second * q.second;
		}
		sumSS += total * total;
		sumBS += cs.target * total;
	}
	if ( sumSS <= 0 )
		return;	// no light reaches the sampled texels: nothing to calibrate
	const double prior = Q_max( 1e-4, sumBS / sumSS );

	// 3. solve: projected Gauss-Seidel with a gentle pull (ridge) towards the shared strength
	double trace = 0;
	int used = 0;
	for ( int g = 0; g < G; g++ )
		if ( AtA[(size_t)g * G + g] > 0 ) { trace += AtA[(size_t)g * G + g]; used++; }
	const double ridge = used ? 0.02 * trace / used : 0.0;
	std::vector<double> x( G, prior );
	for ( int iter = 0; iter < 400; iter++ ) {
		for ( int g = 0; g < G; g++ ) {
			const double diag = AtA[(size_t)g * G + g] + ridge;
			if ( diag <= 0 )
				continue;
			double r = Atb[g] + ridge * prior;
			for ( int h = 0; h < G; h++ )
				if ( h != g )
					r -= AtA[(size_t)g * G + h] * x[h];
			// never negative, at most 8x the shared strength; 0 is allowed, so something that
			// looked like a lamp but never lit anything in the original map is switched off
			x[g] = Com_Clamp( 0.0f, (float)( prior * 8.0 ), (float)( r / diag ) );
		}
	}
	for ( int g = 0; g < G; g++ )
		groupScale[g] = (float)x[g];

	dg_print( "DevoGI: calibrated %d light groups from %d samples (shared strength x%.3f)\n",
		used, (int)samples.size(), prior );

	// the bake log lists every lamp type and the brightness it got, relative to the
	// shared strength (1.00 = average, 0.00 = switched off): handy to see which lamps work
	std::vector<int> lightsPerGroup( G, 0 );
	for ( const devoLight_t &lt : gi_lights )
		if ( lt.group >= 0 && lt.group < G )
			lightsPerGroup[lt.group]++;
	for ( int g = 0; g < G; g++ ) {
		const bool sun = g == gi_sunGroup;
		if ( !lightsPerGroup[g] && !( sun && gi_hasSun ) )
			continue;
		const char *name = sun ? "the sun" : ( g < (int)gi_groupNames.size() ? gi_groupNames[g].c_str() : "?" );
		dg_print( "DevoGI:   %-48s x%.2f  (%d patches%s)\n", name, groupScale[g] / prior,
			lightsPerGroup[g], AtA[(size_t)g * G + g] > 0 ? "" : ", never seen - kept at average" );
	}
}

/*
===========================================================================
Bounce light (global illumination)

Light that hits a surface bounces on, tinted by the surface's colour. For a
point we shoot rays over the half-sphere above it; wherever a ray lands, the
light already there (times that surface's colour) is light arriving back at
our point. Doing this a few times = a few bounces.

The same rays also measure ambient occlusion: how much nearby geometry
surrounds the point. It keeps corners and crevices pleasantly dark.

Bounce light changes slowly across a surface, so we only gather it at a
grid of points (spaced a few texels apart) and fill in everything else from
the nearest gathered points in world space - fast, smooth and seam-free.
===========================================================================
*/

// light leaving the point where a bounce ray landed (for one light style)
static gvec3 gi_hit_radiance( const giBake &bake, const giHit &hit, int styleValue ) {
	const giScene &scene = bake.scene;
	const giTri &t = scene.tris[hit.tri];
	const giSurf &s = scene.surfs[t.surf];
	if ( s.sky )
		return styleValue == 0 ? bake.skyColor : gvec3();	// the open sky glows softly

	const giVert &a = scene.verts[t.vert[0]];
	const giVert &b = scene.verts[t.vert[1]];
	const giVert &c = scene.verts[t.vert[2]];
	const float w0 = 1 - hit.u - hit.v, w1 = hit.u, w2 = hit.v;

	// the hit surface's slot that carries the same light style
	int slot = -1;
	for ( int k = 0; k < MAXLIGHTMAPS; k++ ) {
		if ( s.lightmapStyle[k] == styleValue ) {
			slot = k;
			break;
		}
	}
	if ( slot < 0 )
		return gvec3();

	gvec3 L;
	const int page = s.lightmapNum[slot];
	if ( page >= 0 && page < bake.numPages ) {
		// a lightmapped surface: read the light we have computed so far
		const float fx = ( a.lm[slot][0] * w0 + b.lm[slot][0] * w1 + c.lm[slot][0] * w2 ) * DEVOGI_LM_SIZE;
		const float fy = ( a.lm[slot][1] * w0 + b.lm[slot][1] * w1 + c.lm[slot][1] * w2 ) * DEVOGI_LM_SIZE;
		const int x = Com_Clampi( 0, DEVOGI_LM_SIZE - 1, (int)fx );
		const int y = Com_Clampi( 0, DEVOGI_LM_SIZE - 1, (int)fy );
		const float *px = &bake.light[( ( (size_t)page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x ) * 3];
		L = gvec3( px[0], px[1], px[2] );
	}
	else if ( page == LIGHTMAP_BY_VERTEX ) {
		// a vertex-lit model: use its vertex colours
		L = gvec3( a.color[slot][0] * w0 + b.color[slot][0] * w1 + c.color[slot][0] * w2,
				   a.color[slot][1] * w0 + b.color[slot][1] * w1 + c.color[slot][1] * w2,
				   a.color[slot][2] * w0 + b.color[slot][2] * w1 + c.color[slot][2] * w2 );
	}
	return L * scene.shaders[s.shader].albedo;
}

// bounce light and ambient occlusion at one point: out[0..2] = rgb, out[3] = 1 open .. 0 enclosed
static void gi_gather_point( const giBake &bake, const gvec3 &pos, const gvec3 &normal, const gvec3 &faceNormal,
	int styleValue, uint32_t seed, int rays, float out[4], bool allowSheen = true ) {
	const giScene &scene = bake.scene;
	const float aoDist = Q_max( 8.0f, g_set.aoDistance );
	gvec3 t, b;
	gi_basis( normal, t, b );
	uint32_t rng = gi_hash( seed );
	// rays start just off the flat face (a curved surface's smooth normal could point into
	// its own neighbouring facets)
	const gvec3 o = pos + faceNormal * 1.0f;
	gvec3 sum;
	float occlusion = 0;
	for ( int r = 0; r < rays; r++ ) {
		// cosine-weighted direction: more rays near the normal, where light counts most
		const float u1 = gi_rand( rng ), u2 = gi_rand( rng );
		const float rr = sqrtf( u1 ), phi = 6.2831853f * u2;
		const float lx = rr * cosf( phi ), ly = rr * sinf( phi ), lz = sqrtf( Q_max( 0.0f, 1.0f - u1 ) );
		gvec3 d = gnorm( t * lx + b * ly + normal * lz );
		const float below = gdot( d, faceNormal );
		if ( below < 0 )
			d = gnorm( d - faceNormal * ( 2.0f * below ) );	// mirrored back above the surface
		giHit hit;
		if ( !gi_trace( scene, o, d, 8192.0f, hit, false, DEVOGI_RAY_TMIN ) )
			continue;	// left the map
		const giTri &ht = scene.tris[hit.tri];
		const bool sky = scene.surfs[ht.surf].sky;
		if ( !sky && hit.t < aoDist )
			occlusion += 1.0f - hit.t / aoDist;	// near geometry blocks more
		if ( !sky && gdot( ht.n, d ) > 0 )
			continue;	// hit the back of something: no light comes from there
		sum += gi_hit_radiance( bake, hit, styleValue );
	}
	const float inv = 1.0f / rays;
	out[0] = sum.x * inv;
	out[1] = sum.y * inv;
	out[2] = sum.z * inv;
	out[3] = 1.0f - occlusion * inv;

	/*
	Baked sheen. Real mirror reflections depend on where you look from, and a lightmap
	stores one colour per spot, so it can't hold them. What it *can* hold is a glossy
	hint: a narrow cone of rays straight out from the surface picks up the faces right
	in front of it, and a small share of that is mixed in - so a floor in front of a
	bright wall, or a wall facing a red one, gets that faint, polished pick-up.
	(The renderer's cubemaps, r_cubeMapping, are the real view-dependent reflections.)
	*/
	const float sheen = Com_Clamp( 0.0f, 0.5f, g_set.sheen );
	if ( sheen > 0 && allowSheen ) {
		const int coneRays = Q_max( 4, rays / 8 );
		gvec3 cone;
		int got = 0;
		for ( int r = 0; r < coneRays; r++ ) {
			// within ~20 degrees of the normal
			const float u1 = gi_rand( rng ) * 0.12f, u2 = gi_rand( rng );
			const float rr = sqrtf( u1 ), phi = 6.2831853f * u2;
			gvec3 d = gnorm( t * ( rr * cosf( phi ) ) + b * ( rr * sinf( phi ) ) + normal * sqrtf( 1.0f - u1 ) );
			giHit hit;
			if ( !gi_trace( scene, o, d, 1024.0f, hit, false, DEVOGI_RAY_TMIN ) )
				continue;
			const giTri &ht = scene.tris[hit.tri];
			if ( !scene.surfs[ht.surf].sky && gdot( ht.n, d ) > 0 )
				continue;
			// nearer faces reflect more strongly than far ones
			const float fade = 1.0f / ( 1.0f + hit.t / 256.0f );
			cone += gi_hit_radiance( bake, hit, styleValue ) * fade;
			got++;
		}
		if ( got ) {
			cone = cone * ( 1.0f / coneRays );
			out[0] = out[0] * ( 1.0f - sheen ) + cone.x * sheen * 2.0f;
			out[1] = out[1] * ( 1.0f - sheen ) + cone.y * sheen * 2.0f;
			out[2] = out[2] * ( 1.0f - sheen ) + cone.z * sheen * 2.0f;
		}
	}
}

static void gi_gather( giBake &bake, const giSpatialHash &gridHash, const std::vector<int> &gridPoints,
	const std::vector<gvec3> &texelPos, float radius, int rays ) {
	const size_t n = bake.texels.size();
	std::vector<float> gathered( n * 4, 0.0f );

	// 1. gather at the grid points (the expensive part)
	gi_parallel_for( (int)gridPoints.size(), [&]( int k ) {
		const int i = gridPoints[k];
		const giTexel &tx = bake.texels[i];
		gi_gather_point( bake, tx.pos, tx.normal, tx.faceNormal, tx.styleValue, (uint32_t)i * 9781u + 1u, rays, &gathered[(size_t)i * 4] );
	}, true );

	// 2. every texel takes a smooth, distance-weighted mix of the grid points around it in
	// the world. This removes the noise and fills the gaps in one go, across polygons.
	bake.indirect.assign( n * 4, 0.0f );
	const float r2 = radius * radius;
	std::vector<char> lonely( n, 0 );
	gi_parallel_for( (int)n, [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		float sum[4] = { 0, 0, 0, 0 }, wsum = 0;
		gridHash.query( tx.pos, [&]( int o ) {
			const giTexel &ot = bake.texels[o];
			const gvec3 d = ot.pos - tx.pos;
			const float d2 = gdot( d, d );
			if ( d2 > r2 || !gi_texels_compatible( tx, ot, radius * 0.5f ) )
				return;
			const float w = 1.0f - d2 / r2;
			for ( int c = 0; c < 4; c++ )
				sum[c] += gathered[(size_t)o * 4 + c] * w;
			wsum += w;
		} );
		if ( wsum > 0 ) {
			for ( int c = 0; c < 4; c++ )
				bake.indirect[(size_t)i * 4 + c] = sum[c] / wsum;
		} else {
			lonely[i] = 1;	// no grid point nearby (a thin strip): gather it directly below
		}
	} );
	gi_parallel_for( (int)n, [&]( int i ) {
		if ( !lonely[i] )
			return;
		const giTexel &tx = bake.texels[i];
		gi_gather_point( bake, tx.pos, tx.normal, tx.faceNormal, tx.styleValue, (uint32_t)i * 9781u + 1u, rays, &bake.indirect[(size_t)i * 4] );
	} );
	(void)texelPos;
}

static bool gi_relit;	// direct light was traced with real shadows (no fake contact darkening needed)

// light = direct + bounce, with ambient occlusion keeping corners dark
static void gi_compose( giBake &bake, float intensity ) {
	const float aoIndirect = Com_Clamp( 0.0f, 2.0f, g_set.ao );
	const float aoDirect = gi_relit ? 0.0f : Com_Clamp( 0.0f, 1.0f, g_set.aoDirect );
	bake.light = bake.direct;
	for ( size_t i = 0; i < bake.texels.size(); i++ ) {
		const giTexel &tx = bake.texels[i];
		float *px = &bake.light[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
		const float ao = Com_Clamp( 0.0f, 1.0f, bake.indirect[i * 4 + 3] );
		const float bounceScale = intensity * powf( ao, aoIndirect * 2.0f );
		const float directScale = 1.0f - aoDirect * ( 1.0f - ao );
		px[0] = px[0] * directScale + bake.indirect[i * 4 + 0] * bounceScale;
		px[1] = px[1] * directScale + bake.indirect[i * 4 + 1] * bounceScale;
		px[2] = px[2] * directScale + bake.indirect[i * 4 + 2] * bounceScale;
	}
}

// grows lit texels into the empty pixels around them, so the GPU's texture filtering at
// polygon edges never blends in black
static void gi_dilate( giBake &bake, std::vector<float> &img, std::vector<char> *filledOut = NULL ) {
	std::vector<char> filled( bake.owner.size() );
	for ( size_t i = 0; i < bake.owner.size(); i++ )
		filled[i] = bake.owner[i] >= 0;
	const int passes = Q_max( 2, DEVOGI_LM_SIZE / DEVOGI_BSP_LM_SIZE / 2 );
	for ( int pass = 0; pass < passes; pass++ ) {
		std::vector<char> next = filled;
		for ( int p = 0; p < bake.numPages; p++ ) {
			for ( int y = 0; y < DEVOGI_LM_SIZE; y++ ) {
				for ( int x = 0; x < DEVOGI_LM_SIZE; x++ ) {
					const size_t pix = ( (size_t)p * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x;
					if ( filled[pix] )
						continue;
					float s[3] = { 0, 0, 0 };
					int n = 0;
					for ( int dy = -1; dy <= 1; dy++ ) {
						for ( int dx = -1; dx <= 1; dx++ ) {
							const int xx = x + dx, yy = y + dy;
							if ( xx < 0 || yy < 0 || xx >= DEVOGI_LM_SIZE || yy >= DEVOGI_LM_SIZE )
								continue;
							const size_t q = ( (size_t)p * DEVOGI_LM_SIZE + yy ) * DEVOGI_LM_SIZE + xx;
							if ( !filled[q] )
								continue;
							s[0] += img[q * 3]; s[1] += img[q * 3 + 1]; s[2] += img[q * 3 + 2];
							n++;
						}
					}
					if ( n ) {
						img[pix * 3] = s[0] / n; img[pix * 3 + 1] = s[1] / n; img[pix * 3 + 2] = s[2] / n;
						next[pix] = 1;
					}
				}
			}
		}
		filled.swap( next );
	}
	if ( filledOut )
		filledOut->swap( filled );
}

/*
Soft shoulder for the final colours: up to 170 everything is unchanged, above
that light rolls off smoothly towards 255 instead of clipping to flat white.
*/
static inline void gi_shoulder( float c[3] ) {
	const float knee = 170.0f, room = 255.0f - knee;
	const float m = Q_max( c[0], Q_max( c[1], c[2] ) );
	if ( m > knee ) {
		const float mm = knee + room * ( 1.0f - expf( -( m - knee ) / room ) );
		const float s = mm / m;
		c[0] *= s; c[1] *= s; c[2] *= s;
	}
}

/*
===========================================================================
Cache file (maps/<name>.devogi in the home path)

  header:   magic "DVGI", version, BSP hash, settings hash, sizes
  then:     the new lightmap pages (rgb bytes)
  then:     new vertex colours, one rgba per BSP draw vertex (optional)

The BSP hash covers every lump, so any change to the map triggers a new
bake. The settings hash covers the version, every bake setting and the
map's light sources.
===========================================================================
*/

#pragma pack(push, 1)
struct devogiHeader_t {
	char		magic[4];		// "DVGI"
	int32_t		version;
	uint64_t	bspHash;
	uint64_t	settingsHash;
	int32_t		lightmapBytes;
	int32_t		pageSize;		// baked page width/height (128 * scale)
	int32_t		numVertexColors;// rgba entries after the lightmaps (one per draw vertex, or 0)
};
#pragma pack(pop)

static uint64_t gi_bsp_hash( const byte *fileBase, const dheader_t *header ) {
	uint64_t h = 1469598103934665603ULL;
	for ( int i = 0; i < HEADER_LUMPS; i++ ) {
		const lump_t &l = header->lumps[i];
		h = gi_fnv( h, &l.filelen, sizeof( l.filelen ) );
		if ( l.filelen > 0 )
			h = gi_fnv( h, fileBase + l.fileofs, l.filelen );
	}
	return h;
}

static uint64_t gi_settings_hash( int scale ) {
	char buf[320];
	Com_sprintf( buf, sizeof( buf ), "aa%g sh%g v%d ex%g r%d b%d i%g s%g e%d ao%g aod%g aor%g sat%g x%d rl%d gl%g lh%llx", g_set.aa, g_set.sheen, DEVOGI_VERSION, g_set.exposure,
		Com_Clampi( 8, 1024, g_set.rays ), Com_Clampi( 1, 16, g_set.bounces ),
		g_set.intensity, g_set.sky, g_set.entityLights,
		g_set.ao, g_set.aoDirect, g_set.aoDistance, g_set.saturation, scale,
		g_set.relight, g_set.glow, (unsigned long long)( g_set.relight ? gi_lightsHash : 0 ) );
	return gi_fnv( 1469598103934665603ULL, buf, strlen( buf ) );
}

/*
===========================================================================
Splitting the light for real-time use

The full bake above mixes everything into one lightmap: fast, but frozen.
Real-time lighting keeps the two parts apart, like modern games do:

  - direct light is drawn live by the renderer, from every lamp and the
    sun, with shadow maps. It needs the lamps themselves: neighbouring
    glowing patches of a lamp are merged into one "fixture", with the
    brightness the calibration found, so live light matches the bake.
  - bounce light is expensive and changes little, so it stays baked: in
    "bounce lightmaps" for the map's surfaces, and in a grid of light
    probes for everything that moves (players, weapons, doors).

Nothing here changes the full bake; this only saves the parts separately.
===========================================================================
*/

// float -> 16-bit half float (round to nearest; no NaNs needed here)
static uint16_t gi_float_to_half( float f ) {
	uint32_t x;
	memcpy( &x, &f, 4 );
	const uint32_t sign = ( x >> 16 ) & 0x8000u;
	int exp = (int)( ( x >> 23 ) & 0xff ) - 127 + 15;
	uint32_t mant = x & 0x7fffffu;
	if ( exp <= 0 ) {
		if ( exp < -10 )
			return (uint16_t)sign;					// too small: zero
		mant |= 0x800000u;							// a denormal half
		const int shift = 14 - exp;
		uint32_t h = mant >> shift;
		if ( ( mant >> ( shift - 1 ) ) & 1u )
			h++;
		return (uint16_t)( sign | h );
	}
	if ( exp >= 31 )
		return (uint16_t)( sign | 0x7bffu );		// too big: the largest half
	uint32_t h = sign | ( (uint32_t)exp << 10 ) | ( mant >> 13 );
	if ( mant & 0x1000u )
		h++;										// round (may carry into the exponent: fine)
	return (uint16_t)h;
}

/*
Bounce lightmaps: the bounce part of every texel (with its ambient occlusion and
exposure, exactly as the full bake mixed it in), stored at half the resolution of
the full lightmaps - bounce light is soft, so that loses nothing visible.
The alpha channel holds how much of the sun each spot sees (0..255): until the
renderer has its own sun shadows, that baked mask is the sun's shadow.
*/
static void gi_build_bounce_maps( giBake &bake, dgLiveLighting &live, bool relight, float exposure, float intensity ) {
	const float aoIndirect = Com_Clamp( 0.0f, 2.0f, g_set.ao );
	std::vector<float> img( bake.owner.size() * 3, 0.0f );
	for ( size_t i = 0; i < bake.texels.size(); i++ ) {
		const giTexel &tx = bake.texels[i];
		const float ao = Com_Clamp( 0.0f, 1.0f, bake.indirect[i * 4 + 3] );
		float k = intensity * powf( ao, aoIndirect * 2.0f );
		if ( relight && tx.styleValue == 0 )
			k *= exposure;
		float *px = &img[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
		px[0] = bake.indirect[i * 4 + 0] * k;
		px[1] = bake.indirect[i * 4 + 1] * k;
		px[2] = bake.indirect[i * 4 + 2] * k;
	}
	std::vector<char> filled;
	gi_dilate( bake, img, &filled );

	// the sun's visibility goes into the alpha channel, grown into empty pixels the same way
	std::vector<float> vis( bake.owner.size() * 3, 0.0f );
	if ( !bake.sunVis.empty() ) {
		for ( size_t i = 0; i < bake.texels.size(); i++ ) {
			const giTexel &tx = bake.texels[i];
			vis[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3] = bake.sunVis[i];
		}
		gi_dilate( bake, vis );
	}

	// average each 2x2 block, counting only pixels that hold light (empty space would
	// darken the edges of every polygon)
	const int f = DEVOGI_LM_SIZE > DEVOGI_BSP_LM_SIZE ? 2 : 1;
	const int bs = DEVOGI_LM_SIZE / f;
	live.bouncePageSize = bs;
	live.bounce.assign( (size_t)bake.numPages * bs * bs * 4, 0 );
	for ( int p = 0; p < bake.numPages; p++ ) {
		for ( int y = 0; y < bs; y++ ) {
			for ( int x = 0; x < bs; x++ ) {
				float sum[4] = { 0, 0, 0, 0 };
				int n = 0;
				for ( int dy = 0; dy < f; dy++ ) {
					for ( int dx = 0; dx < f; dx++ ) {
						const size_t q = ( (size_t)p * DEVOGI_LM_SIZE + y * f + dy ) * DEVOGI_LM_SIZE + x * f + dx;
						if ( !filled[q] )
							continue;
						sum[0] += img[q * 3]; sum[1] += img[q * 3 + 1]; sum[2] += img[q * 3 + 2];
						sum[3] += vis[q * 3];
						n++;
					}
				}
				if ( !n )
					continue;
				uint8_t *o = &live.bounce[( ( (size_t)p * bs + y ) * bs + x ) * 4];
				for ( int k = 0; k < 3; k++ )
					o[k] = (uint8_t)Com_Clampi( 0, 255, (int)( sum[k] / n + 0.5f ) );
				o[3] = (uint8_t)Com_Clampi( 0, 255, (int)( sum[3] / n * 255.0f + 0.5f ) );
			}
		}
	}
}

/*
Light probes: a 3D grid over the whole map, 64 units apart (wider on huge maps).
Each probe looks in 6 directions and stores the bounce light coming from each -
an "ambient cube". A player model reads the 8 probes around it and lights every
part of itself from the matching directions, so a red wall on its left tints its
left side. Probes inside walls or outside the map are marked invalid and get the
average of their valid neighbours, so a renderer can sample anywhere.
*/
static void gi_build_probes( giBake &bake, dgLiveLighting &live, float intensity, int rays ) {
	const giScene &scene = bake.scene;
	if ( scene.verts.empty() )
		return;
	gvec3 mins( DEVOGI_INF, DEVOGI_INF, DEVOGI_INF ), maxs( -DEVOGI_INF, -DEVOGI_INF, -DEVOGI_INF );
	for ( const giVert &v : scene.verts ) {
		mins = gmin( mins, v.xyz );
		maxs = gmax( maxs, v.xyz );
	}

	// 64 units apart, wider if the map is so big that the grid would get huge
	float spacing = 64.0f;
	int dims[3];
	while ( 1 ) {
		for ( int k = 0; k < 3; k++ ) {
			const float lo = k == 0 ? mins.x : ( k == 1 ? mins.y : mins.z );
			const float hi = k == 0 ? maxs.x : ( k == 1 ? maxs.y : maxs.z );
			dims[k] = Q_max( 1, (int)ceilf( ( hi - lo ) / spacing ) + 1 );
		}
		if ( (size_t)dims[0] * dims[1] * dims[2] <= 400000 || spacing > 1024 )
			break;
		spacing *= 1.25f;
	}
	const size_t count = (size_t)dims[0] * dims[1] * dims[2];
	live.probeSpacing = spacing;
	live.probeOrigin[0] = mins.x; live.probeOrigin[1] = mins.y; live.probeOrigin[2] = mins.z;
	for ( int k = 0; k < 3; k++ )
		live.probeDims[k] = dims[k];

	std::vector<float> cube( count * 18, 0.0f );
	std::vector<uint8_t> valid( count, 0 );
	std::vector<uint8_t> sunVis( count, 0 );
	const gvec3 axes[6] = { gvec3( 1, 0, 0 ), gvec3( -1, 0, 0 ), gvec3( 0, 1, 0 ), gvec3( 0, -1, 0 ), gvec3( 0, 0, 1 ), gvec3( 0, 0, -1 ) };
	const int probeRays = Q_max( 32, rays / 2 );	// a probe has no neighbours to blend with

	gi_progress_stage( "Light probes for models", 0.96f, 0.035f );
	gi_parallel_for( (int)count, [&]( int i ) {
		const int x = (int)( i % dims[0] ), y = (int)( ( i / dims[0] ) % dims[1] ), z = (int)( i / ( (size_t)dims[0] * dims[1] ) );
		const gvec3 pos( mins.x + x * spacing, mins.y + y * spacing, mins.z + z * spacing );

		// inside a wall or outside the sealed map, rays mostly hit the backs of faces
		int bad = 0, tested = 0;
		for ( int k = 0; k < 14; k++ ) {
			gvec3 d = k < 6 ? axes[k] : gnorm( gvec3( ( k & 1 ) ? 1.0f : -1.0f, ( k & 2 ) ? 1.0f : -1.0f, ( k & 4 ) ? 1.0f : -1.0f ) );
			giHit hit;
			tested++;
			if ( !gi_trace( scene, pos, d, 16384.0f, hit, false ) ) {
				bad++;	// escaped: we are outside the map
				continue;
			}
			const giTri &t = scene.tris[hit.tri];
			if ( !scene.surfs[t.surf].sky && gdot( t.n, d ) > 0 )
				bad++;
		}
		if ( bad * 10 > tested * 3 )
			return;
		valid[i] = 1;
		if ( gi_hasSun ) {
			// how much of the sun is visible from here: 16 rays spread over the sun's disc
			gvec3 st, sb;
			gi_basis( gi_sunDir, st, sb );
			int seen = 0;
			for ( int k = 0; k < 16; k++ ) {
				const float jx = ( ( k & 3 ) + 0.5f ) / 4.0f - 0.5f, jy = ( ( k >> 2 ) + 0.5f ) / 4.0f - 0.5f;
				const gvec3 d = gnorm( gi_sunDir + st * ( jx * 0.05f ) + sb * ( jy * 0.05f ) );
				giHit hit;
				if ( !gi_trace( scene, pos, d, 16384.0f, hit, false ) || scene.surfs[scene.tris[hit.tri].surf].sky )
					seen++;
			}
			sunVis[i] = (uint8_t)( seen * 255 / 16 );
		}
		for ( int a = 0; a < 6; a++ ) {
			float g[4];
			gi_gather_point( bake, pos, axes[a], axes[a], 0, (uint32_t)i * 2654435761u + (uint32_t)a * 97u, probeRays, g, false );
			cube[(size_t)i * 18 + a * 3 + 0] = g[0] * intensity;
			cube[(size_t)i * 18 + a * 3 + 1] = g[1] * intensity;
			cube[(size_t)i * 18 + a * 3 + 2] = g[2] * intensity;
		}
	}, true );

	// invalid probes take the average of their valid (or already filled) neighbours
	std::vector<uint8_t> have = valid;
	for ( int pass = 0; pass < 8; pass++ ) {
		std::vector<uint8_t> next = have;
		bool changed = false;
		for ( size_t i = 0; i < count; i++ ) {
			if ( have[i] )
				continue;
			const int x = (int)( i % dims[0] ), y = (int)( ( i / dims[0] ) % dims[1] ), z = (int)( i / ( (size_t)dims[0] * dims[1] ) );
			float sum[18] = {};
			int sunSum = 0;
			int n = 0;
			const int nb[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
			for ( const auto &o : nb ) {
				const int xx = x + o[0], yy = y + o[1], zz = z + o[2];
				if ( xx < 0 || yy < 0 || zz < 0 || xx >= dims[0] || yy >= dims[1] || zz >= dims[2] )
					continue;
				const size_t j = ( (size_t)zz * dims[1] + yy ) * dims[0] + xx;
				if ( !have[j] )
					continue;
				for ( int k = 0; k < 18; k++ )
					sum[k] += cube[j * 18 + k];
				sunSum += sunVis[j];
				n++;
			}
			if ( n ) {
				for ( int k = 0; k < 18; k++ )
					cube[i * 18 + k] = sum[k] / n;
				sunVis[i] = (uint8_t)( sunSum / n );
				next[i] = 1;
				changed = true;
			}
		}
		have.swap( next );
		if ( !changed )
			break;
	}

	live.probes.resize( count * 18 );
	for ( size_t k = 0; k < count * 18; k++ )
		live.probes[k] = gi_float_to_half( cube[k] );
	live.probeValid.swap( valid );
	live.probeSun.swap( sunVis );

	size_t numValid = 0;
	for ( uint8_t v : live.probeValid )
		numValid += v;
	dg_print( "DevoGI: %d x %d x %d light probes, %d units apart, %d in open space\n",
		dims[0], dims[1], dims[2], (int)spacing, (int)numValid );
}

/*
Fixtures: the lamps for real-time light. The bake cuts every glowing surface into
small patches (that's what makes its shadows soft); a renderer wants one light per
lamp instead. Patches of the same shader that face the same way and touch are one
lamp: they are merged, keeping their total strength, centre and size.
*/
static void gi_build_fixtures( dgLiveLighting &live, float exposure, const std::vector<float> &groupScale ) {
	auto scaleOf = [&]( int group ) {
		return group >= 0 && group < (int)groupScale.size() ? groupScale[group] : 1.0f;
	};
	const int n = (int)gi_lights.size();

	// union-find over the patches
	std::vector<int> parent( n );
	for ( int i = 0; i < n; i++ )
		parent[i] = i;
	std::function<int( int )> root = [&]( int i ) { return parent[i] == i ? i : ( parent[i] = root( parent[i] ) ); };
	for ( int i = 0; i < n; i++ ) {
		const devoLight_t &a = gi_lights[i];
		if ( a.shader < 0 )
			continue;	// light entities stay single lights
		for ( int j = i + 1; j < n; j++ ) {
			const devoLight_t &b = gi_lights[j];
			if ( b.shader != a.shader || gdot( a.normal, b.normal ) < 0.9f )
				continue;
			const gvec3 d = a.origin - b.origin;
			const float link = Q_max( a.size, b.size ) * 1.5f + 8.0f;
			if ( gdot( d, d ) > link * link )
				continue;
			parent[root( i )] = root( j );
		}
	}

	std::vector<std::vector<int>> members( n );
	for ( int i = 0; i < n; i++ )
		members[root( i )].push_back( i );

	live.fixtures.clear();
	for ( int r = 0; r < n; r++ ) {
		const std::vector<int> &m = members[r];
		if ( m.empty() )
			continue;
		const devoLight_t &first = gi_lights[m[0]];
		const float strength = scaleOf( first.group ) * exposure;
		float photons = 0;
		gvec3 centre, normal;
		for ( int i : m ) {
			const devoLight_t &lt = gi_lights[i];
			photons += lt.photons;
			centre += lt.origin * lt.photons;
			normal += lt.normal * lt.photons;
		}
		if ( photons <= 0 || strength <= 0 )
			continue;	// a lamp the calibration switched off
		centre = centre * ( 1.0f / photons );

		dgFixture fx = {};
		fx.origin[0] = centre.x; fx.origin[1] = centre.y; fx.origin[2] = centre.z;
		fx.shader = first.shader;
		const gvec3 rgb = first.color * ( photons * strength );
		fx.rgb[0] = rgb.x; fx.rgb[1] = rgb.y; fx.rgb[2] = rgb.z;

		float reach = 0;
		if ( first.size > 0 ) {
			// a glowing rectangle: its extent along two edges, around the centre
			const gvec3 nn = gnorm( normal );
			gvec3 u, v;
			gi_basis( nn, u, v );
			float hu = 0, hv = 0;
			for ( int i : m ) {
				const devoLight_t &lt = gi_lights[i];
				const gvec3 d = lt.origin - centre;
				hu = Q_max( hu, fabsf( gdot( d, u ) ) + lt.size * 0.5f );
				hv = Q_max( hv, fabsf( gdot( d, v ) ) + lt.size * 0.5f );
				reach = Q_max( reach, sqrtf( gdot( d, d ) ) + lt.radius );
			}
			fx.normal[0] = nn.x; fx.normal[1] = nn.y; fx.normal[2] = nn.z;
			fx.axisU[0] = u.x; fx.axisU[1] = u.y; fx.axisU[2] = u.z;
			fx.halfU = hu;
			fx.halfV = hv;
		} else {
			reach = first.radius;	// a light entity: shines everywhere from one point
		}
		fx.reach = reach;
		live.fixtures.push_back( fx );
	}

	live.hasSun = gi_hasSun;
	if ( gi_hasSun ) {
		const gvec3 rgb = gi_sunColor * ( gi_sunIntensity * scaleOf( gi_sunGroup ) * exposure );
		live.sunDir[0] = gi_sunDir.x; live.sunDir[1] = gi_sunDir.y; live.sunDir[2] = gi_sunDir.z;
		live.sunRgb[0] = rgb.x; live.sunRgb[1] = rgb.y; live.sunRgb[2] = rgb.z;
	}
	dg_print( "DevoGI: %d light patches merged into %d lamps for real-time light%s\n",
		n, (int)live.fixtures.size(), gi_hasSun ? ", plus the sun" : "" );
}

static void gi_build_live( giBake &bake, dgLiveLighting &live, bool relight, float exposure, float intensity,
	int rays, const std::vector<float> &groupScale ) {
	live = dgLiveLighting();
	live.relit = relight;
	gi_build_bounce_maps( bake, live, relight, exposure, intensity );
	if ( relight )
		gi_build_fixtures( live, exposure, groupScale );
	gi_build_probes( bake, live, intensity, rays );
}

/*
===========================================================================
DG_Bake - the whole bake

Fills result with the new lightmap pages (128 * scale pixels each, in the
BSP's page order) and vertex colours. Returns false if the map can't be
baked (no lightmaps inside the BSP).
===========================================================================
*/

bool DG_Bake( const char *mapName, const byte *fileBase, const dgSettings &settings, int scale, dgResult &result )
{
	g_set = settings;
	const dheader_t *header = (const dheader_t *)fileBase;
	result = dgResult();
	gi_vertexColors.clear();

	const int numDrawVerts = header->lumps[LUMP_DRAWVERTS].filelen / (int)sizeof( mapVert_t );

	const lump_t &ll = header->lumps[LUMP_LIGHTMAPS];
	const int srcPageBytes = DEVOGI_BSP_LM_SIZE * DEVOGI_BSP_LM_SIZE * 3;
	if ( ll.filelen < srcPageBytes )
		return false;	// maps with external lightmap images are not supported yet
	const int numPages = ll.filelen / srcPageBytes;
	const byte *srcLightmaps = fileBase + ll.fileofs;

	const int pageSize = DEVOGI_BSP_LM_SIZE * scale;
	const int pageBytes = pageSize * pageSize * 3;
	const int outBytes = numPages * pageBytes;

	// ---- 2. bake ----
	const int startTime = dg_ms();
	const int rays = Com_Clampi( 8, 1024, g_set.rays );
	const int bounces = Com_Clampi( 1, 16, g_set.bounces );
	const float intensity = Q_max( 0.0f, g_set.intensity );

	giBake *bakePtr = new giBake();
	giBake &bake = *bakePtr;
	bake.numPages = numPages;

	// surface colours (how each material tints the light it reflects)
	{
		const lump_t &l = header->lumps[LUMP_SHADERS];
		const dshader_t *in = (const dshader_t *)( fileBase + l.fileofs );
		const int count = l.filelen / sizeof( dshader_t );
		bake.scene.shaders.resize( Q_max( 1, count ) );
		const float sat = Com_Clamp( 0.0f, 4.0f, g_set.saturation );
		for ( int i = 0; i < count; i++ ) {
			giShader &s = bake.scene.shaders[i];
			s.surfaceFlags = LittleLong( in[i].surfaceFlags );
			s.contentFlags = LittleLong( in[i].contentFlags );
			s.albedo = ( s.surfaceFlags & SURF_SKY ) ? gvec3() : gi_shader_albedo( in[i].shader );
			// averaged texture colours look muddy: a saturation boost makes colour bleeding visible
			const float lum = gi_luma( s.albedo );
			gvec3 c( lum + ( s.albedo.x - lum ) * sat, lum + ( s.albedo.y - lum ) * sat, lum + ( s.albedo.z - lum ) * sat );
			c = gmax( c, gvec3( 0, 0, 0 ) );
			const float m = Q_max( c.x, Q_max( c.y, c.z ) );
			if ( m > 0.9f )
				c = c * ( 0.9f / m );
			s.albedo = c;
		}
	}

	// the open sky glows softly, tinted like the sun
	{
		gvec3 tint( 1, 1, 1 );
		const lump_t &l = header->lumps[LUMP_SHADERS];
		const dshader_t *in = (const dshader_t *)( fileBase + l.fileofs );
		const int count = l.filelen / sizeof( dshader_t );
		for ( int i = 0; i < count; i++ ) {
			if ( !( LittleLong( in[i].surfaceFlags ) & SURF_SKY ) )
				continue;
			if ( gi_sky_sun_color( in[i].shader, tint ) )
				break;
		}
		bake.skyColor = tint * ( 64.0f * Q_max( 0.0f, g_set.sky ) );
	}

	COM_StripExtension( COM_SkipPath( (char *)mapName ), gi_progressMap, sizeof( gi_progressMap ) );
	gi_progressLastPrint = -100;
	gi_progress_stage( "Preparing geometry", 0.0f, 0.03f );

	gi_build_geometry( bake.scene, fileBase, header );
	gi_build_bvh( bake.scene );
	gi_build_bsp_owner( bake );		// at the BSP's own 128 resolution
	gi_lmSize = pageSize;
	gi_build_texels( bake );		// at bake resolution

	// typical texel size: the base for every smoothing radius
	{
		std::vector<float> sizes;
		sizes.reserve( bake.texels.size() / 16 + 1 );
		for ( size_t i = 0; i < bake.texels.size(); i += 16 )
			sizes.push_back( bake.texels[i].size );
		if ( !sizes.empty() ) {
			std::nth_element( sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end() );
			bake.texelSize = Com_Clamp( 0.5f, 64.0f, sizes[sizes.size() / 2] );
		} else {
			bake.texelSize = 16.0f / scale;
		}
	}

	dg_print( "DevoGI: baking %s: %d lightmap pages at %dx%d, %d texels (~%.1f units), %d triangles, %d rays x %d bounces\n",
		mapName, numPages, pageSize, pageSize, (int)bake.texels.size(), bake.texelSize, (int)bake.scene.tris.size(), rays, bounces );

	gi_progress_stage( "Keeping samples out of walls", 0.03f, 0.02f );
	gi_fix_texel_positions( bake );

	// world-space neighbourhoods of all texels (for smoothing the direct light)
	std::vector<gvec3> texelPos( bake.texels.size() );
	std::vector<int> allTexels( bake.texels.size() );
	for ( size_t i = 0; i < bake.texels.size(); i++ ) {
		texelPos[i] = bake.texels[i].pos;
		allTexels[i] = (int)i;
	}
	// final smoothing grows with the anti-aliasing setting
	const float directRadius = bake.texelSize * ( 1.0f + Com_Clamp( 0.25f, 4.0f, g_set.aa ) );
	giSpatialHash texelHash;
	texelHash.build( texelPos, allTexels, directRadius );

	// the old light, enlarged: kept for switchable light styles, and the brightness reference
	gi_upsample_direct( bake, srcLightmaps );

	// ---- direct light, traced from the map's own lights ----
	const bool relight = g_set.relight && ( !gi_lights.empty() || gi_hasSun );
	gi_relit = relight;
	giLightGrid lightGrid;
	std::vector<float> origLuma;		// old lightmap brightness per texel
	std::vector<float> groupScale;		// calibrated strength per light group
	if ( relight ) {
		gi_build_light_grid( lightGrid );
		const size_t n = bake.texels.size();
		origLuma.assign( n, 0.0f );
		for ( size_t i = 0; i < n; i++ ) {
			const giTexel &tx = bake.texels[i];
			const float *px = &bake.direct[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			origLuma[i] = gi_luma( gvec3( px[0], px[1], px[2] ) );
		}

		gi_progress_stage( "Matching lamp brightness", 0.05f, 0.07f );
		gi_calibrate( bake, lightGrid, origLuma, groupScale );

		gi_progress_stage( "Direct light and soft shadows", 0.12f, 0.33f );
		std::vector<gvec3> directLight( n );
		bake.sunVis.assign( n, 0.0f );
		gi_parallel_for( (int)n, [&]( int i ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 )
				return;	// switchable lamps keep their original light
			gvec3 total, sun;
			gi_direct_point( bake, lightGrid, tx.pos, tx.normal, tx.faceNormal, (uint32_t)i * 2654435761u, tx.size,
				[&]( int group, const gvec3 &rgb ) {
					total += rgb * groupScale[group];
					if ( group == gi_sunGroup )
						sun += rgb;
				} );
			directLight[i] = total;
			// the sun's visibility: what arrived, divided by what would arrive with nothing in
			// the way (a real-time renderer draws the sun live and uses this as its shadow)
			if ( gi_hasSun ) {
				const float full = gi_luma( gi_sunColor ) * gi_sunIntensity * gdot( tx.normal, gi_sunDir );
				bake.sunVis[i] = full > 1e-4f ? Com_Clamp( 0.0f, 1.0f, gi_luma( sun ) / full ) : 0.0f;
			}
		}, true );
		gi_world_smooth( bake, texelHash, bake.sunVis, directRadius );

		// gentle world-space smoothing: soft, anti-aliased shadow edges with no polygon seams
		gi_world_smooth( bake, texelHash, directLight, directRadius );

		for ( size_t i = 0; i < n; i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 )
				continue;
			float *px = &bake.direct[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			px[0] = directLight[i].x; px[1] = directLight[i].y; px[2] = directLight[i].z;
		}
		dg_print( "DevoGI: direct light from %d lights%s done (%.1fs)\n",
			(int)gi_lights.size(), gi_hasSun ? " and the sun" : "", ( dg_ms() - startTime ) / 1000.0f );
	}
	else if ( g_set.entityLights ) {
		// can't relight: keep the old light, but add any light entities still in the BSP
		std::vector<giPointLight> lights;
		const lump_t &le = header->lumps[LUMP_ENTITIES];
		gi_parse_lights( (const char *)( fileBase + le.fileofs ), le.filelen, lights );
		if ( !lights.empty() ) {
			const giScene &scene = bake.scene;
			gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
				const giTexel &tx = bake.texels[i];
				if ( tx.styleValue != 0 )
					return;
				gvec3 sum;
				const gvec3 o = tx.pos + tx.normal * 0.5f;
				for ( const giPointLight &l : lights ) {
					gvec3 d = l.origin - o;
					float dist2 = gdot( d, d );
					if ( dist2 > l.radius * l.radius || dist2 < 1.0f )
						continue;
					float dist = sqrtf( dist2 );
					d = d * ( 1.0f / dist );
					float cosT = gdot( d, tx.normal );
					if ( cosT <= 0 )
						continue;
					giHit hit;
					if ( gi_trace( scene, o, d, dist - 1.0f, hit, true ) )
						continue;
					float atten = l.linear ? Q_max( 0.0f, 1.0f - dist / l.radius ) : 1.0f / dist2;
					sum += l.color * ( atten * cosT );
				}
				float *px = &bake.direct[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
				// the old lightmap may already hold these lights: keep the brighter, never add twice
				px[0] = Q_max( px[0], sum.x );
				px[1] = Q_max( px[1], sum.y );
				px[2] = Q_max( px[2], sum.z );
			} );
		}
	}

	// ---- bounce light ----
	// grid points for gathering: one texel every few, in each lightmap direction
	const int stride = Q_max( 1, pageSize / ( DEVOGI_BSP_LM_SIZE * 2 ) );
	std::vector<int> gridPoints;
	for ( size_t i = 0; i < bake.texels.size(); i++ ) {
		const giTexel &tx = bake.texels[i];
		if ( ( tx.x % stride ) == 0 && ( tx.y % stride ) == 0 )
			gridPoints.push_back( (int)i );
	}
	const float bounceRadius = bake.texelSize * stride * 2.5f;
	giSpatialHash gridHash;
	gridHash.build( texelPos, gridPoints, bounceRadius );

	bake.light = bake.direct;
	static char bounceStage[64];
	const float bounceBase = 0.45f, bounceSpan = 0.47f;
	for ( int b = 0; b < bounces; b++ ) {
		Com_sprintf( bounceStage, sizeof( bounceStage ), "Bounce light %d of %d", b + 1, bounces );
		gi_progress_stage( bounceStage, bounceBase + bounceSpan * b / bounces, bounceSpan / bounces );
		// later bounces carry less and less energy, so fewer rays are enough for them
		const int bounceRays = b == 0 ? rays : ( b < 3 ? rays / 2 : rays / 4 );
		gi_gather( bake, gridHash, gridPoints, texelPos, bounceRadius, Q_max( 16, bounceRays ) );
		gi_compose( bake, intensity );
		dg_print( "DevoGI: bounce %d/%d done (%.1fs)\n", b + 1, bounces, ( dg_ms() - startTime ) / 1000.0f );
	}

	// ---- exposure: one factor for the whole map ----
	// The lamps were matched to the old lightmap *before* bounce light; with bounces on top
	// the level is a little brighter, so one overall factor brings it back to the original
	// brightness (and r_devoGI_exposure on top). A single factor can't create blotches.
	float exposure = Q_max( 0.0f, g_set.exposure );
	if ( relight ) {
		double on = 0, nn = 0;
		for ( size_t i = 0; i < bake.texels.size(); i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 || tx.edge )
				continue;
			const float *px = &bake.light[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			const double nv = gi_luma( gvec3( px[0], px[1], px[2] ) );
			on += origLuma[i] * nv;
			nn += nv * nv;
		}
		const float match = nn > 0 ? Com_Clamp( 0.6f, 1.6f, (float)( on / nn ) ) : 1.0f;
		exposure *= match;
		for ( size_t i = 0; i < bake.texels.size(); i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 )
				continue;
			float *px = &bake.light[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			px[0] *= exposure; px[1] *= exposure; px[2] *= exposure;
		}
		dg_print( "DevoGI: exposure x%.2f (match x%.2f, r_devoGI_exposure %.2f)\n", exposure, match, g_set.exposure );
	}

	gi_dilate( bake, bake.light );

	// ---- vertex colours: models and rgbGen vertex stages ----
	{
		const lump_t &lv = header->lumps[LUMP_DRAWVERTS];
		const lump_t &ls = header->lumps[LUMP_SURFACES];
		const mapVert_t *dv = (const mapVert_t *)( fileBase + lv.fileofs );
		const dsurface_t *ds = (const dsurface_t *)( fileBase + ls.fileofs );
		const int numSurfs = ls.filelen / sizeof( dsurface_t );

		std::vector<int> list;						// vertices of vertex-lit models: traced
		std::vector<std::pair<int, int>> lmList;	// (vertex, page) of lightmapped surfaces: sampled
		std::vector<char> mark( numDrawVerts, 0 );
		for ( int s = 0; s < numSurfs; s++ ) {
			if ( LittleLong( ds[s].surfaceType ) == MST_FLARE )
				continue;
			const int shader = LittleLong( ds[s].shaderNum );
			if ( shader >= 0 && shader < (int)bake.scene.shaders.size() && ( bake.scene.shaders[shader].surfaceFlags & SURF_SKY ) )
				continue;
			const int lmNum = LittleLong( ds[s].lightmapNum[0] );
			const bool vertexLit = lmNum == LIGHTMAP_BY_VERTEX;
			// lightmapped surfaces carry vertex colours too, used by shader stages with
			// rgbGen vertex / exactVertex (glass, decals, overlays): they follow the new light
			const bool lightmapped = lmNum >= 0 && lmNum < numPages && ds[s].lightmapStyles[0] == 0;
			if ( !vertexLit && !lightmapped )
				continue;
			const int first = LittleLong( ds[s].firstVert ), nv = LittleLong( ds[s].numVerts );
			for ( int v = first; v < first + nv && v < numDrawVerts; v++ ) {
				if ( v >= 0 && !mark[v] ) {
					mark[v] = 1;
					if ( vertexLit )
						list.push_back( v );
					else
						lmList.push_back( std::make_pair( v, lmNum ) );
				}
			}
		}

		if ( !list.empty() || !lmList.empty() ) {
			gi_vertexColors.resize( (size_t)numDrawVerts * 4 );
			for ( int v = 0; v < numDrawVerts; v++ )
				memcpy( &gi_vertexColors[(size_t)v * 4], dv[v].color[0], 4 );

			// lightmapped surfaces: read the new lightmap at each vertex
			gi_parallel_for( (int)lmList.size(), [&]( int i ) {
				const int v = lmList[i].first, page = lmList[i].second;
				const float fx = Com_Clamp( 0.0f, (float)( DEVOGI_LM_SIZE - 1 ), LittleFloat( dv[v].lightmap[0][0] ) * DEVOGI_LM_SIZE - 0.5f );
				const float fy = Com_Clamp( 0.0f, (float)( DEVOGI_LM_SIZE - 1 ), LittleFloat( dv[v].lightmap[0][1] ) * DEVOGI_LM_SIZE - 0.5f );
				const int x0 = (int)fx, y0 = (int)fy;
				const int x1 = Q_min( DEVOGI_LM_SIZE - 1, x0 + 1 ), y1 = Q_min( DEVOGI_LM_SIZE - 1, y0 + 1 );
				const float ax = fx - x0, ay = fy - y0;
				auto at = [&]( int x, int y, int k ) { return bake.light[( ( (size_t)page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x ) * 3 + k]; };
				float c[3];
				for ( int k = 0; k < 3; k++ )
					c[k] = ( at( x0, y0, k ) * ( 1 - ax ) + at( x1, y0, k ) * ax ) * ( 1 - ay ) + ( at( x0, y1, k ) * ( 1 - ax ) + at( x1, y1, k ) * ax ) * ay;
				gi_shoulder( c );
				for ( int k = 0; k < 3; k++ )
					gi_vertexColors[(size_t)v * 4 + k] = (byte)Com_Clampi( 0, 255, (int)( c[k] + 0.5f ) );
			} );

			// vertex-lit models (crates, ships, lamps...): the same direct and bounce light per vertex
			const float aoIndirect = Com_Clamp( 0.0f, 2.0f, g_set.ao );
			const float aoDirect = Com_Clamp( 0.0f, 1.0f, g_set.aoDirect );
			gi_progress_stage( "Models and vertex colours", 0.92f, 0.04f );
			gi_parallel_for( (int)list.size(), [&]( int i ) {
				const int v = list[i];
				giVert gv;
				gi_load_vert( gv, dv[v] );
				float g[4];
				// twice the rays: a vertex has no neighbours to average with
				gi_gather_point( bake, gv.xyz, gv.normal, gv.normal, 0, (uint32_t)v * 7919u + 13u, rays * 2, g );
				const float ao = Com_Clamp( 0.0f, 1.0f, g[3] );
				const float bounceScale = intensity * powf( ao, aoIndirect * 2.0f );
				float c[3];
				if ( relight ) {
					gvec3 total;
					gi_direct_point( bake, lightGrid, gv.xyz, gv.normal, gv.normal, (uint32_t)v * 40503u + 7u, 0.0f,
						[&]( int group, const gvec3 &rgb ) { total += rgb * groupScale[group]; } );
					// (the bounce light is gathered from surfaces that are already exposed,
					// so only the direct part gets the exposure factor)
					c[0] = total.x * exposure + g[0] * bounceScale;
					c[1] = total.y * exposure + g[1] * bounceScale;
					c[2] = total.z * exposure + g[2] * bounceScale;
				} else {
					const float directScale = 1.0f - aoDirect * ( 1.0f - ao );
					for ( int k = 0; k < 3; k++ )
						c[k] = dv[v].color[0][k] * directScale + g[k] * bounceScale;
				}
				gi_shoulder( c );
				for ( int k = 0; k < 3; k++ )
					gi_vertexColors[(size_t)v * 4 + k] = (byte)Com_Clampi( 0, 255, (int)( c[k] + 0.5f ) );
			}, true );
			dg_print( "DevoGI: lit %d vertices of models, %d vertex colours of lightmapped surfaces\n",
				(int)list.size(), (int)lmList.size() );
		}
	}

	// ---- the same lighting, split for real-time use (see gi_build_live) ----
	gi_build_live( bake, result.live, relight, exposure, intensity, rays, groupScale );

	// ---- 3. back to lightmap bytes ----
	result.lightmaps.assign( outBytes, 0 );
	byte *out = result.lightmaps.data();
	for ( size_t pix = 0; pix < bake.owner.size(); pix++ ) {
		float rgb[3] = { bake.light[pix * 3], bake.light[pix * 3 + 1], bake.light[pix * 3 + 2] };
		gi_shoulder( rgb );
		out[pix * 3 + 0] = (byte)Com_Clampi( 0, 255, (int)( rgb[0] + 0.5f ) );
		out[pix * 3 + 1] = (byte)Com_Clampi( 0, 255, (int)( rgb[1] + 0.5f ) );
		out[pix * 3 + 2] = (byte)Com_Clampi( 0, 255, (int)( rgb[2] + 0.5f ) );
	}

	// pages no surface uses (deluxemaps: light directions) are simply enlarged
	{
		std::vector<char> used( numPages, 0 );
		for ( const giSurf &s : bake.scene.surfs )
			for ( int k = 0; k < MAXLIGHTMAPS; k++ )
				if ( s.lightmapNum[k] >= 0 && s.lightmapNum[k] < numPages )
					used[s.lightmapNum[k]] = 1;
		for ( int p = 0; p < numPages; p++ ) {
			if ( used[p] )
				continue;
			for ( int y = 0; y < pageSize; y++ ) {
				for ( int x = 0; x < pageSize; x++ ) {
					float c[3];
					gi_sample_bsp_page( srcLightmaps, p, ( x + 0.5f ) / scale - 0.5f, ( y + 0.5f ) / scale - 0.5f, c );
					byte *o = out + ( ( (size_t)p * pageSize + y ) * pageSize + x ) * 3;
					o[0] = (byte)( c[0] + 0.5f ); o[1] = (byte)( c[1] + 0.5f ); o[2] = (byte)( c[2] + 0.5f );
				}
			}
		}
	}

	delete bakePtr;
	gi_lmSize = DEVOGI_BSP_LM_SIZE;

	gi_progress_end();
	dg_print( "DevoGI: baked %s in %.1fs\n", mapName, ( dg_ms() - startTime ) / 1000.0f );
	result.pageSize = pageSize;
	result.vertexColors.swap( gi_vertexColors );
	return true;
}

/*
===========================================================================
DevoLights: finding the map's own light sources

q3map2 bakes these into the lightmaps and then throws them away. We find
them again, the way q3map2 found them in the first place:

  - Surface lights: shaders with q3map_surfacelight. Every surface using
    one is cut into patches of q3map_lightsubdivide units; each patch
    becomes a glowing light with q3map2's strength formula
    (surfacelight * area * 0.25).
  - Glowing lamps without q3map_surfacelight: many JA lamp textures have a
    bright "glow" (or additive) stage but were lit by light entities that
    the compiler removed. We treat the glowing stage as the light source.
  - The sun: direction, colour and strength from the sky shader's
    q3map_sun / q3map_sunExt line.
  - "light" entities still in the BSP.

The exact strengths don't matter too much: the bake calibrates every lamp
type against the original lightmap anyway.
===========================================================================
*/

struct giLightShader {
	float	value;		// q3map_surfacelight (or the inferred glow strength)
	float	subdivide;	// q3map_lightsubdivide: patch size in game units
	gvec3	color;
};

// average colour of an image (brightest channel = 1), and optionally its average brightness 0..1
static bool gi_image_color( const char *name, gvec3 &out, float *brightness = NULL ) {
	char texName[MAX_QPATH];
	Q_strncpyz( texName, name, sizeof( texName ) );
	COM_StripExtension( texName, texName, sizeof( texName ) );
	byte *pic = NULL;
	int w = 0, h = 0;
	dg_load_image( texName, &pic, &w, &h );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic )
			dg_free_image( pic );
		return false;
	}
	double r = 0, g = 0, b = 0;
	const int step = ( w * h > 65536 ) ? 4 : 1;
	int n = 0;
	for ( int i = 0; i < w * h; i += step, n++ ) {
		r += pic[i * 4 + 0];
		g += pic[i * 4 + 1];
		b += pic[i * 4 + 2];
	}
	dg_free_image( pic );
	const float m = (float)Q_max( r, Q_max( g, b ) );
	if ( m <= 0 )
		return false;
	out = gvec3( (float)( r / m ), (float)( g / m ), (float)( b / m ) );
	if ( brightness )
		*brightness = (float)( m / ( n * 255.0 ) );
	return true;
}

// reads a shader: does it emit light, how strongly, in which colour?
static bool gi_parse_light_shader( const char *shaderName, giLightShader &ls ) {
	ls.value = 0;
	ls.subdivide = 120.0f;	// q3map2's default
	ls.color = gvec3( 1, 1, 1 );
	const char *p = R_DevoGI_FindShaderText( shaderName );
	if ( !p )
		return false;

	char lightImage[MAX_QPATH] = "", editorImage[MAX_QPATH] = "", firstMap[MAX_QPATH] = "";
	bool hasRGB = false;
	float rgb[3] = { 1, 1, 1 };
	int depth = 0;

	// stage by stage: we remember the first glowing stage (a "glow" keyword, or an
	// additive blend that is not a reflection) and whether there is a lightmap stage
	bool hasLightmapStage = false, autosprite = false;
	bool decal = false;				// polygonOffset: a decal sitting on a wall, not a lamp
	bool litStage = false;			// a stage lit by vertex light (rgbGen vertex & co): not self-lit
	bool firstStageBlended = false;	// the first stage blends over something: not a solid lamp face
	int stageIndex = -1;
	char glowMap[MAX_QPATH] = "";
	bool glowKeyword = false;
	char stMap[MAX_QPATH] = "";
	bool stLightmap = false, stAdditive = false, stGlow = false, stEnv = false;
	auto endStage = [&]() {
		if ( stLightmap )
			hasLightmapStage = true;
		else if ( stMap[0] && !stEnv && ( stGlow || stAdditive ) && !glowMap[0] ) {
			Q_strncpyz( glowMap, stMap, sizeof( glowMap ) );
			glowKeyword = stGlow;
		}
	};
	auto rememberMap = [&]( const char *name ) {
		Q_strncpyz( stMap, name, sizeof( stMap ) );
		if ( !firstMap[0] )
			Q_strncpyz( firstMap, name, sizeof( firstMap ) );
	};

	while ( 1 ) {
		const char *token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		if ( token[0] == '{' ) {
			depth++;
			if ( depth == 2 ) {	// a new stage starts
				stageIndex++;
				stMap[0] = 0;
				stLightmap = stAdditive = stGlow = stEnv = false;
			}
			continue;
		}
		if ( token[0] == '}' ) {
			if ( depth == 2 )
				endStage();
			if ( --depth <= 0 )
				break;
			continue;
		}
		if ( depth == 2 ) {
			if ( !Q_stricmp( token, "map" ) || !Q_stricmp( token, "clampmap" ) ) {
				token = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( token, "$lightmap" ) )
					stLightmap = true;
				else if ( token[0] && token[0] != '$' && token[0] != '*' )
					rememberMap( token );
			}
			else if ( !Q_stricmp( token, "animmap" ) || !Q_stricmp( token, "clampanimmap" ) ) {
				COM_ParseExt( &p, qfalse );	// frame rate
				token = COM_ParseExt( &p, qfalse );
				if ( token[0] && token[0] != '$' )
					rememberMap( token );
			}
			else if ( !Q_stricmp( token, "glow" ) )
				stGlow = true;
			else if ( !Q_stricmp( token, "blendfunc" ) ) {
				char a[64], b[64] = "";
				Q_strncpyz( a, COM_ParseExt( &p, qfalse ), sizeof( a ) );
				if ( Q_stricmp( a, "add" ) && Q_stricmp( a, "filter" ) && Q_stricmp( a, "blend" ) )
					Q_strncpyz( b, COM_ParseExt( &p, qfalse ), sizeof( b ) );
				if ( !Q_stricmp( a, "add" ) || ( !Q_stricmp( b, "GL_ONE" ) && ( !Q_stricmp( a, "GL_ONE" ) || !Q_stricmp( a, "GL_SRC_ALPHA" ) ) ) )
					stAdditive = true;
				if ( stageIndex == 0 )
					firstStageBlended = true;
			}
			else if ( !Q_stricmp( token, "rgbgen" ) ) {
				const char *g = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( g, "vertex" ) || !Q_stricmp( g, "exactvertex" ) || !Q_stricmp( g, "lightingdiffuse" )
					|| !Q_stricmp( g, "oneminusvertex" ) || !Q_stricmp( g, "lightingdiffuseentity" ) )
					litStage = true;
			}
			else if ( !Q_stricmp( token, "tcgen" ) ) {
				if ( !Q_stricmp( COM_ParseExt( &p, qfalse ), "environment" ) )
					stEnv = true;	// a shiny reflection layer, not light
			}
			continue;
		}
		if ( depth == 1 ) {
			if ( !Q_stricmp( token, "polygonoffset" ) ) {
				decal = true;
				continue;
			}
			if ( !Q_stricmp( token, "deformvertexes" ) || !Q_stricmp( token, "deform" ) ) {
				const char *t = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmpn( t, "autosprite", 10 ) || !Q_stricmp( t, "flare" ) )
					autosprite = true;	// coronas and flare sprites are glare, not light
				continue;
			}
			if ( !Q_stricmp( token, "q3map_surfacelight" ) || !Q_stricmp( token, "surfacelight" ) )
				ls.value = atof( COM_ParseExt( &p, qfalse ) );
			else if ( !Q_stricmp( token, "q3map_lightsubdivide" ) )
				ls.subdivide = atof( COM_ParseExt( &p, qfalse ) );
			else if ( !Q_stricmp( token, "q3map_lightRGB" ) ) {
				for ( int i = 0; i < 3; i++ )
					rgb[i] = atof( COM_ParseExt( &p, qfalse ) );
				hasRGB = true;
			}
			else if ( !Q_stricmp( token, "q3map_lightimage" ) )
				Q_strncpyz( lightImage, COM_ParseExt( &p, qfalse ), sizeof( lightImage ) );
			else if ( !Q_stricmp( token, "qer_editorimage" ) )
				Q_strncpyz( editorImage, COM_ParseExt( &p, qfalse ), sizeof( editorImage ) );
		}
	}

	/*
	No q3map_surfacelight, but the shader clearly glows - Raven lit those places with
	light entities that the compiler removed, so we bring the light back:
	  - a glowing stage: an explicit "glow" stage, or an additive (light-adding) stage
	    that isn't a reflection, on any surface (lamp textures and lamp models)
	  - a fullbright lamp face: no lightmap and no vertex lighting at all, a solid first
	    stage, and a bright texture (e.g. plain white ceiling light panels)
	Decals (polygonOffset) and flare sprites are left out. Glow textures are mostly
	black, so their average brightness tells how much of the surface really glows.
	The strength is only a starting point: calibration matches every lamp type to the
	original map afterwards, and can switch a false lamp off completely.
	*/
	if ( ls.value <= 0 && !autosprite && !decal && g_set.glow > 0 ) {
		float brightness = 0;
		gvec3 c;
		if ( glowMap[0] && gi_image_color( glowMap, c, &brightness ) && brightness > 0.01f ) {
			ls.value = g_set.glow * brightness;
			ls.subdivide = 32.0f;	// small fixtures: small patches
			ls.color = c;
			return true;
		}
		if ( !hasLightmapStage && !litStage && !firstStageBlended && firstMap[0]
			&& gi_image_color( firstMap, c, &brightness ) && brightness > 0.45f ) {
			ls.value = g_set.glow * brightness;
			ls.subdivide = 32.0f;
			ls.color = c;
			return true;
		}
	}
	if ( !firstMap[0] && glowMap[0] )
		Q_strncpyz( firstMap, glowMap, sizeof( firstMap ) );
	if ( ls.value <= 0 )
		return false;
	if ( ls.subdivide < 8.0f )
		ls.subdivide = 8.0f;

	// the light's colour, picked like q3map2 does: lightRGB, else the light image,
	// editor image, first stage texture, or the shader name as a texture
	if ( hasRGB ) {
		const float m = Q_max( rgb[0], Q_max( rgb[1], rgb[2] ) );
		if ( m > 0 )
			ls.color = gvec3( rgb[0] / m, rgb[1] / m, rgb[2] / m );
	} else {
		const char *candidates[4] = { lightImage, editorImage, firstMap, shaderName };
		for ( const char *c : candidates ) {
			if ( c[0] && gi_image_color( c, ls.color ) )
				break;
		}
	}
	return true;
}

// the sun of a sky shader: q3map_sun / q3map_sunExt / sun  r g b intensity azimuth elevation
static bool gi_parse_sun( const char *shaderName, gvec3 &color, float &intensity, gvec3 &dir ) {
	const char *p = R_DevoGI_FindShaderText( shaderName );
	if ( !p )
		return false;
	int depth = 0;
	while ( 1 ) {
		const char *token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			return false;
		if ( token[0] == '{' ) { depth++; continue; }
		if ( token[0] == '}' ) { if ( --depth <= 0 ) return false; continue; }
		if ( depth == 1 && ( !Q_stricmp( token, "q3map_sun" ) || !Q_stricmp( token, "q3map_sunExt" ) || !Q_stricmp( token, "sun" ) ) ) {
			float c[3];
			for ( int i = 0; i < 3; i++ )
				c[i] = atof( COM_ParseExt( &p, qfalse ) );
			intensity = atof( COM_ParseExt( &p, qfalse ) );
			const float az = DEG2RAD( atof( COM_ParseExt( &p, qfalse ) ) );
			const float el = DEG2RAD( atof( COM_ParseExt( &p, qfalse ) ) );
			const float m = Q_max( c[0], Q_max( c[1], c[2] ) );
			color = m > 0 ? gvec3( c[0] / m, c[1] / m, c[2] / m ) : gvec3( 1, 1, 1 );
			dir = gnorm( gvec3( cosf( az ) * cosf( el ), sinf( az ) * cosf( el ), sinf( el ) ) );
			return intensity > 0;
		}
	}
}

void DG_ExtractLights( const char *mapName, const byte *fileBase, const dgSettings &settings )
{
	g_set = settings;
	const dheader_t *header = (const dheader_t *)fileBase;
	gi_lights.clear();
	gi_hasSun = false;
	Q_strncpyz( gi_lightsMap, mapName, sizeof( gi_lightsMap ) );
	const int startTime = dg_ms();

	// shaders: which ones emit light, and where is the sun?
	const lump_t &l = header->lumps[LUMP_SHADERS];
	const dshader_t *in = (const dshader_t *)( fileBase + l.fileofs );
	const int numShaders = l.filelen / sizeof( dshader_t );
	std::vector<giLightShader> lightShaders( Q_max( 1, numShaders ) );
	std::vector<char> emits( Q_max( 1, numShaders ), 0 );
	int numEmitting = 0;

	// calibration groups: one per shader, then light entities, then the sun
	gi_entityGroup = Q_max( 1, numShaders );
	gi_sunGroup = gi_entityGroup + 1;
	gi_numGroups = gi_sunGroup + 1;
	gi_groupNames.assign( gi_numGroups, std::string() );
	for ( int i = 0; i < numShaders; i++ )
		gi_groupNames[i] = in[i].shader;
	gi_groupNames[gi_entityGroup] = "light entities";
	gi_groupNames[gi_sunGroup] = "the sun";

	giScene scene;
	scene.shaders.resize( Q_max( 1, numShaders ) );
	for ( int i = 0; i < numShaders; i++ ) {
		giShader &s = scene.shaders[i];
		s.surfaceFlags = LittleLong( in[i].surfaceFlags );
		s.contentFlags = LittleLong( in[i].contentFlags );
		s.albedo = gvec3( 0.5f, 0.5f, 0.5f );
		if ( s.surfaceFlags & SURF_SKY ) {
			if ( !gi_hasSun && gi_parse_sun( in[i].shader, gi_sunColor, gi_sunIntensity, gi_sunDir ) )
				gi_hasSun = true;
			continue;
		}
		if ( gi_parse_light_shader( in[i].shader, lightShaders[i] ) ) {
			emits[i] = 1;
			numEmitting++;
		}
	}

	// surface lights: cut every emitting surface into patches of the shader's subdivide size
	if ( numEmitting ) {
		gi_build_geometry( scene, fileBase, header );

		struct accum_t { gvec3 c, n; float area; int shader; };
		std::vector<accum_t> patches;
		// a small hash table: (surface, cube of subdivide size) -> patch
		std::vector<int> table( 1 << 16, -1 );
		std::vector<uint64_t> tableKeys( 1 << 16, 0 );

		for ( const giTri &t : scene.tris ) {
			const int sh = scene.surfs[t.surf].shader;
			if ( sh < 0 || sh >= numShaders || !emits[sh] )
				continue;
			const float area = 0.5f * sqrtf( gdot( gcross( t.e1, t.e2 ), gcross( t.e1, t.e2 ) ) );
			const gvec3 c = t.v0 + ( t.e1 + t.e2 ) * ( 1.0f / 3.0f );
			const float sub = lightShaders[sh].subdivide;
			const int64_t cx = (int64_t)floorf( c.x / sub ), cy = (int64_t)floorf( c.y / sub ), cz = (int64_t)floorf( c.z / sub );
			const uint64_t key = gi_fnv( gi_fnv( 1469598103934665603ULL, &t.surf, sizeof( t.surf ) ),
				&cx, sizeof( cx ) ) ^ ( (uint64_t)cy * 73856093ULL ) ^ ( (uint64_t)cz * 19349663ULL );
			size_t slot = key & ( table.size() - 1 );
			int idx = -1;
			while ( table[slot] >= 0 ) {
				if ( tableKeys[slot] == key ) { idx = table[slot]; break; }
				slot = ( slot + 1 ) & ( table.size() - 1 );
			}
			if ( idx < 0 ) {
				if ( patches.size() * 2 >= table.size() ) {
					// table getting full: double it and re-insert everything
					std::vector<int> nt( table.size() * 2, -1 );
					std::vector<uint64_t> nk( table.size() * 2, 0 );
					for ( size_t s = 0; s < table.size(); s++ ) {
						if ( table[s] < 0 ) continue;
						size_t ns = tableKeys[s] & ( nt.size() - 1 );
						while ( nt[ns] >= 0 ) ns = ( ns + 1 ) & ( nt.size() - 1 );
						nt[ns] = table[s]; nk[ns] = tableKeys[s];
					}
					table.swap( nt ); tableKeys.swap( nk );
					slot = key & ( table.size() - 1 );
					while ( table[slot] >= 0 ) slot = ( slot + 1 ) & ( table.size() - 1 );
				}
				idx = (int)patches.size();
				patches.push_back( { gvec3(), gvec3(), 0.0f, sh } );
				table[slot] = idx;
				tableKeys[slot] = key;
			}
			accum_t &a = patches[idx];
			a.c += c * area;
			a.n += t.n * area;
			a.area += area;
		}

		for ( const accum_t &a : patches ) {
			if ( a.area < 1.0f )
				continue;
			devoLight_t lt;
			lt.normal = gnorm( a.n );
			lt.origin = a.c * ( 1.0f / a.area ) + lt.normal * 4.0f;	// just in front of the surface
			lt.color = lightShaders[a.shader].color;
			lt.photons = lightShaders[a.shader].value * a.area * 0.25f;
			lt.radius = Com_Clamp( 32.0f, 4096.0f, sqrtf( lt.photons ) );
			lt.size = sqrtf( a.area );
			lt.shader = a.shader;
			lt.group = a.shader;
			gi_lights.push_back( lt );
		}
	}

	// light entities kept in the BSP
	{
		std::vector<giPointLight> ents;
		const lump_t &le = header->lumps[LUMP_ENTITIES];
		gi_parse_lights( (const char *)( fileBase + le.fileofs ), le.filelen, ents );
		for ( const giPointLight &e : ents ) {
			devoLight_t lt;
			lt.origin = e.origin;
			lt.normal = gvec3();
			const float m = Q_max( e.color.x, Q_max( e.color.y, e.color.z ) );
			lt.color = m > 0 ? e.color * ( 1.0f / m ) : gvec3( 1, 1, 1 );
			lt.photons = m;
			lt.radius = Com_Clamp( 32.0f, 4096.0f, e.radius );
			lt.size = 0;
			lt.shader = -1;
			lt.group = gi_entityGroup;
			gi_lights.push_back( lt );
		}
	}

	/*
	A fingerprint of everything the lights are made from: the name and the text of
	every shader the map uses (token by token, so comments and spacing don't count).
	It is part of the cache key, so a new shader pack makes maps bake again. It only
	depends on inputs, never on computed values, so the game and the compiler always
	agree on it.
	*/
	gi_lightsHash = 1469598103934665603ULL;
	for ( int i = 0; i < numShaders; i++ ) {
		gi_lightsHash = gi_fnv( gi_lightsHash, in[i].shader, strlen( in[i].shader ) + 1 );
		const char *p = R_DevoGI_FindShaderText( in[i].shader );
		if ( !p )
			continue;
		int depth = 0;
		while ( 1 ) {
			const char *token = COM_ParseExt( &p, qtrue );
			if ( !token[0] )
				break;
			gi_lightsHash = gi_fnv( gi_lightsHash, token, strlen( token ) + 1 );
			if ( token[0] == '{' ) depth++;
			else if ( token[0] == '}' && --depth <= 0 ) break;
		}
	}

	dg_print( "DevoLights: %s: %d lights from %d emitting shaders, %s (%dms)\n",
		mapName, (int)gi_lights.size(), numEmitting, gi_hasSun ? "sun found" : "no sun",
		dg_ms() - startTime );
}

/*
===========================================================================
The public side: resolution, cache key, cache file, light accessors
(see devogi_core.h)
===========================================================================
*/

// r_devoGI_scale rounded down to 1, 2, 4 or 8, within what the GPU can hold (maxScale)
int DG_ChooseScale( const dgSettings &settings, int maxScale )
{
	int scale = 1;
	while ( scale * 2 <= Com_Clampi( 1, 8, settings.scale ) && scale * 2 <= maxScale )
		scale *= 2;
	return scale;
}

uint64_t DG_BspHash( const byte *bsp )
{
	return gi_bsp_hash( bsp, (const dheader_t *)bsp );
}

uint64_t DG_SettingsHash( const dgSettings &settings, int scale )
{
	g_set = settings;
	return gi_settings_hash( scale );
}

/*
After the lightmaps and vertex colours, the file holds tagged sections, so new
kinds of data can be added later without breaking the layout:

  "BNCE"  int32 bouncePageSize, int32 relit, then the bounce lightmaps (RGBA,
          A = sun visibility)
  "PRBE"  float origin[3], float spacing, int32 dims[3], then 18 half floats
          per probe, one valid byte per probe, one sun-visibility byte per probe
  "LITE"  int32 count, count x dgFixture, int32 hasSun, float sunDir[3],
          float sunRgb[3]

Each section is: 4-byte tag, int32 size of what follows, the data.
*/
static void gi_put( std::vector<byte> &f, const void *data, size_t size ) {
	const size_t at = f.size();
	f.resize( at + size );
	if ( size )
		memcpy( f.data() + at, data, size );
}

static void gi_put_i32( std::vector<byte> &f, int32_t v ) {
	gi_put( f, &v, 4 );
}

// starts a section: returns where its size goes, filled in by gi_section_end
static size_t gi_section_begin( std::vector<byte> &f, const char tag[4] ) {
	gi_put( f, tag, 4 );
	gi_put_i32( f, 0 );
	return f.size();
}

static void gi_section_end( std::vector<byte> &f, size_t start ) {
	const int32_t size = (int32_t)( f.size() - start );
	memcpy( f.data() + start - 4, &size, 4 );
}

std::vector<byte> DG_WriteCache( uint64_t bspHash, uint64_t settingsHash, const dgResult &result )
{
	const int numVC = (int)( result.vertexColors.size() / 4 );
	devogiHeader_t h;
	memcpy( h.magic, "DVGI", 4 );
	h.version = DEVOGI_VERSION;
	h.bspHash = bspHash;
	h.settingsHash = settingsHash;
	h.lightmapBytes = (int32_t)result.lightmaps.size();
	h.pageSize = result.pageSize;
	h.numVertexColors = numVC;

	std::vector<byte> file;
	gi_put( file, &h, sizeof( h ) );
	gi_put( file, result.lightmaps.data(), result.lightmaps.size() );
	gi_put( file, result.vertexColors.data(), (size_t)numVC * 4 );

	const dgLiveLighting &live = result.live;
	size_t s = gi_section_begin( file, "BNCE" );
	gi_put_i32( file, live.bouncePageSize );
	gi_put_i32( file, live.relit ? 1 : 0 );
	gi_put( file, live.bounce.data(), live.bounce.size() );
	gi_section_end( file, s );

	s = gi_section_begin( file, "PRBE" );
	gi_put( file, live.probeOrigin, sizeof( live.probeOrigin ) );
	gi_put( file, &live.probeSpacing, 4 );
	gi_put( file, live.probeDims, sizeof( live.probeDims ) );
	gi_put( file, live.probes.data(), live.probes.size() * 2 );
	gi_put( file, live.probeValid.data(), live.probeValid.size() );
	gi_put( file, live.probeSun.data(), live.probeSun.size() );
	gi_section_end( file, s );

	s = gi_section_begin( file, "LITE" );
	gi_put_i32( file, (int32_t)live.fixtures.size() );
	gi_put( file, live.fixtures.data(), live.fixtures.size() * sizeof( dgFixture ) );
	gi_put_i32( file, live.hasSun ? 1 : 0 );
	gi_put( file, live.sunDir, sizeof( live.sunDir ) );
	gi_put( file, live.sunRgb, sizeof( live.sunRgb ) );
	gi_section_end( file, s );
	return file;
}

// reads the tagged sections (unknown ones are skipped); false if one is damaged
static bool gi_read_sections( const byte *p, const byte *end, dgLiveLighting &live ) {
	live = dgLiveLighting();
	while ( end - p >= 8 ) {
		char tag[4];
		int32_t size;
		memcpy( tag, p, 4 );
		memcpy( &size, p + 4, 4 );
		p += 8;
		if ( size < 0 || size > end - p )
			return false;
		const byte *q = p, *qend = p + size;
		auto get = [&]( void *out, size_t n ) {
			if ( (size_t)( qend - q ) < n )
				return false;
			memcpy( out, q, n );
			q += n;
			return true;
		};
		if ( !memcmp( tag, "BNCE", 4 ) ) {
			int32_t bs = 0, relit = 0;
			if ( !get( &bs, 4 ) || !get( &relit, 4 ) )
				return false;
			live.bouncePageSize = bs;
			live.relit = relit != 0;
			live.bounce.assign( q, qend );
		}
		else if ( !memcmp( tag, "PRBE", 4 ) ) {
			if ( !get( live.probeOrigin, 12 ) || !get( &live.probeSpacing, 4 ) || !get( live.probeDims, 12 ) )
				return false;
			const size_t count = (size_t)Q_max( 0, live.probeDims[0] ) * Q_max( 0, live.probeDims[1] ) * Q_max( 0, live.probeDims[2] );
			if ( (size_t)( qend - q ) != count * 18 * 2 + count * 2 )
				return false;
			live.probes.resize( count * 18 );
			get( live.probes.data(), count * 36 );
			live.probeValid.assign( q, q + count );
			live.probeSun.assign( q + count, qend );
		}
		else if ( !memcmp( tag, "LITE", 4 ) ) {
			int32_t n = 0, sun = 0;
			if ( !get( &n, 4 ) || n < 0 || (size_t)( qend - q ) < (size_t)n * sizeof( dgFixture ) )
				return false;
			live.fixtures.resize( n );
			get( live.fixtures.data(), (size_t)n * sizeof( dgFixture ) );
			if ( !get( &sun, 4 ) || !get( live.sunDir, 12 ) || !get( live.sunRgb, 12 ) )
				return false;
			live.hasSun = sun != 0;
		}
		p = qend;
	}
	return p == end;
}

// only accepts a file made for exactly this BSP, these settings and this resolution
bool DG_ReadCache( const byte *data, size_t length, const byte *bsp,
	uint64_t bspHash, uint64_t settingsHash, int scale, dgResult &result )
{
	if ( !data || length < sizeof( devogiHeader_t ) )
		return false;
	devogiHeader_t h;
	memcpy( &h, data, sizeof( h ) );

	const dheader_t *header = (const dheader_t *)bsp;
	const int numDrawVerts = header->lumps[LUMP_DRAWVERTS].filelen / (int)sizeof( mapVert_t );
	const int numPages = header->lumps[LUMP_LIGHTMAPS].filelen / ( DEVOGI_BSP_LM_SIZE * DEVOGI_BSP_LM_SIZE * 3 );
	const int pageSize = DEVOGI_BSP_LM_SIZE * scale;
	const size_t outBytes = (size_t)numPages * pageSize * pageSize * 3;

	if ( memcmp( h.magic, "DVGI", 4 ) || h.version != DEVOGI_VERSION
		|| h.bspHash != bspHash || h.settingsHash != settingsHash
		|| (size_t)h.lightmapBytes != outBytes || h.pageSize != pageSize
		|| ( h.numVertexColors != 0 && h.numVertexColors != numDrawVerts )
		|| length < sizeof( devogiHeader_t ) + outBytes + (size_t)h.numVertexColors * 4 )
		return false;

	const byte *p = data + sizeof( devogiHeader_t );
	const byte *sections = p + outBytes + (size_t)h.numVertexColors * 4;
	if ( !gi_read_sections( sections, data + length, result.live ) )
		return false;
	result.pageSize = pageSize;
	result.lightmaps.assign( p, p + outBytes );
	result.vertexColors.assign( p + outBytes, sections );
	return true;
}

int DG_NumLights( void )
{
	return (int)gi_lights.size();
}

void DG_GetLight( int index, dgLightInfo &out )
{
	const devoLight_t &lt = gi_lights[index];
	out.origin[0] = lt.origin.x; out.origin[1] = lt.origin.y; out.origin[2] = lt.origin.z;
	out.color[0] = lt.color.x; out.color[1] = lt.color.y; out.color[2] = lt.color.z;
	out.photons = lt.photons;
	out.radius = lt.radius;
}

bool DG_GetSun( float dir[3], float color[3], float *intensity )
{
	if ( !gi_hasSun )
		return false;
	dir[0] = gi_sunDir.x; dir[1] = gi_sunDir.y; dir[2] = gi_sunDir.z;
	color[0] = gi_sunColor.x; color[1] = gi_sunColor.y; color[2] = gi_sunColor.z;
	*intensity = gi_sunIntensity;
	return true;
}

const char *DG_LightsMapName( void )
{
	return gi_lightsMap;
}

// the engine's tokenizer, for hosts that read shader scripts themselves (see devogi_core.h)
const char *DG_ParseToken( const char **text, bool allowLineBreaks )
{
	return COM_ParseExt( text, allowLineBreaks );
}
