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
#include "devogi_gpu.h"

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
#define LUMP_MODELS			7
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

struct dmodel_t {
	float	mins[3], maxs[3];
	int		firstSurface, numSurfaces;
	int		firstBrush, numBrushes;
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
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Bump this whenever the bake changes in a way that should invalidate old .devogi files.
#define DEVOGI_VERSION		21

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
/*
A point on the sun's disc, for soft sun shadows: (jx, jy) in -0.5..0.5 is spread
evenly over a round disc (Shirley's concentric mapping) about 1 degree across - the
real sun is 0.53 degrees, a little more keeps distant shadow edges smooth.
*/
static inline void gi_sun_disc( float jx, float jy, float &dx, float &dy ) {
	const float a = jx * 2.0f, b = jy * 2.0f;	// -1..1
	float r, phi;
	if ( a == 0.0f && b == 0.0f ) { dx = dy = 0.0f; return; }
	if ( fabsf( a ) > fabsf( b ) ) { r = a; phi = 0.78539816f * ( b / a ); }
	else { r = b; phi = 1.57079633f - 0.78539816f * ( a / b ); }
	const float radius = 0.0087f;	// radians: half of 1 degree
	dx = r * cosf( phi ) * radius;
	dy = r * sinf( phi ) * radius;
}

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
	// the glowing patch's real shape, for its shadow rays: a rectangle on the
	// surface (a long thin strip stays long and thin). halfU 0: not known, use size
	gvec3	rectCentre;
	gvec3	axisU, axisV;
	float	halfU = 0, halfV = 0;
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
static std::vector<std::string>	gi_groupNames;
static std::vector<std::string>	gi_emitTexture;		// per shader: the texture a big unlit surface glows with ("" = none)		// readable name per group, for the bake log
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
	float	st[2];					// texture coordinates (where a glowing texture is sampled)
	float	lm[MAXLIGHTMAPS][2];	// lightmap coordinates, per light style slot (0..1 across a page)
	float	color[MAXLIGHTMAPS][3];	// q3map2's baked vertex colours, per light style slot
	int		bspVert;				// the BSP draw vertex it is (-1: made here, e.g. a patch point)
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
	// a big unlit surface (a fake sky box, a large light panel): it shines with its own
	// texture, pixel by pixel - brighter pixels give more light (see gi_emit_radiance)
	int					emitW = 0, emitH = 0;
	std::vector<gvec3>	emit;
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
			out.st[0] += p->st[0] * w;
			out.st[1] += p->st[1] * w;
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
	out.bspVert = -1;
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
	out.st[0] = LittleFloat( in.st[0] );
	out.st[1] = LittleFloat( in.st[1] );
	for ( int s = 0; s < MAXLIGHTMAPS; s++ ) {
		out.lm[s][0] = LittleFloat( in.lightmap[s][0] );
		out.lm[s][1] = LittleFloat( in.lightmap[s][1] );
		out.color[s][0] = in.color[s][0];
		out.color[s][1] = in.color[s][1];
		out.color[s][2] = in.color[s][2];
	}
}

/*
Smooth shading. Round walls and pillars in old maps are usually flat brush faces
standing in a circle, and each face has one flat normal - so light falls on
every face a step brighter or darker than the next, in visible stripes. q3map2
can smooth this ("phong" shading); here it is done for every map: where flat
faces meet at less than 40 degrees, the corners they share get the average
normal of those faces (weighted by area), and light runs in a smooth gradient
across them. Real corners stay sharp. Patches and models keep their own smooth
normals, which the BSP already has.
*/
static void gi_smooth_normals( giScene &scene, const dsurface_t *in, int numSurfs ) {
	const float cosLimit = 0.766f;	// cos( 40 degrees )
	const size_t nv = scene.verts.size();

	// the flat face each vertex of a planar surface belongs to
	std::vector<int> vertTri( nv, -1 );
	for ( size_t t = 0; t < scene.tris.size(); t++ ) {
		const giTri &tri = scene.tris[t];
		if ( LittleLong( in[tri.surf].surfaceType ) != MST_PLANAR || scene.surfs[tri.surf].sky )
			continue;
		for ( int k = 0; k < 3; k++ )
			if ( vertTri[tri.vert[k]] < 0 )
				vertTri[tri.vert[k]] = (int)t;
	}

	// every triangle corner, found by its position (to an eighth of a unit)
	auto key = []( const gvec3 &p ) {
		const int64_t x = (int64_t)floorf( p.x * 8.0f + 0.5f ), y = (int64_t)floorf( p.y * 8.0f + 0.5f ), z = (int64_t)floorf( p.z * 8.0f + 0.5f );
		return (uint64_t)( x * 73856093LL ) ^ (uint64_t)( y * 19349663LL ) ^ (uint64_t)( z * 83492791LL );
	};
	std::unordered_map<uint64_t, std::vector<int>> corners;
	corners.reserve( scene.tris.size() * 2 );
	for ( size_t t = 0; t < scene.tris.size(); t++ ) {
		const giTri &tri = scene.tris[t];
		if ( LittleLong( in[tri.surf].surfaceType ) != MST_PLANAR || scene.surfs[tri.surf].sky )
			continue;
		for ( int k = 0; k < 3; k++ )
			corners[key( scene.verts[tri.vert[k]].xyz )].push_back( (int)t );
	}

	for ( size_t v = 0; v < nv; v++ ) {
		if ( vertTri[v] < 0 )
			continue;
		giVert &gv = scene.verts[v];
		const gvec3 own = scene.tris[vertTri[v]].n;
		if ( gdot( gv.normal, own ) < 0.999f )
			continue;	// already smoothed by the map compiler: keep it
		const auto it = corners.find( key( gv.xyz ) );
		if ( it == corners.end() )
			continue;
		gvec3 sum;
		for ( int t : it->second ) {
			const giTri &tri = scene.tris[t];
			if ( gdot( tri.n, own ) < cosLimit )
				continue;	// a real corner: stays sharp
			const gvec3 cr = gcross( tri.e1, tri.e2 );
			sum += tri.n * sqrtf( gdot( cr, cr ) );	// bigger faces count more
		}
		if ( gdot( sum, sum ) > 1e-8f )
			gv.normal = gnorm( sum );
	}
	(void)numSurfs;
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

	// the world is model 0; doors, lifts and other movers are models 1 and up. Like
	// q3map2, only the world blocks light: a closed door must not leave the floor
	// under it and the faces inside its frame in the dark.
	int worldFirst = 0, worldCount = numSurfs;
	{
		const lump_t &lm = header->lumps[LUMP_MODELS];
		if ( lm.filelen >= (int)sizeof( dmodel_t ) ) {
			const dmodel_t *models = (const dmodel_t *)( fileBase + lm.fileofs );
			worldFirst = LittleLong( models[0].firstSurface );
			worldCount = LittleLong( models[0].numSurfaces );
		}
	}

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
			&& !( sh.contentFlags & ( CONTENTS_WATER | CONTENTS_FOG | CONTENTS_TRANSLUCENT | CONTENTS_LAVA | CONTENTS_SLIME ) )
			&& i >= worldFirst && i < worldFirst + worldCount;	// movers don't block light

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
				gv.bspVert = firstVert + v;
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
			for ( int v = 0; v < w * h; v++ ) {
				gi_load_vert( ctrl[v], dv[firstVert + v] );
				ctrl[v].bspVert = -1;
			}

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

	gi_smooth_normals( scene, in, numSurfs );
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

	// split every box (with the surface area heuristic, below) until at most 4 triangles remain
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

		/*
		Where to split: the surface area heuristic (SAH). A ray hits a box with a chance
		that grows with the box's surface area, so the best split keeps big boxes with
		few triangles and many triangles in small boxes. 16 candidate planes per axis
		are tried ("binning"); the cheapest wins. A tree built this way sends a ray
		through far fewer boxes than simply cutting the triangles in half.
		*/
		const int BINS = 16;
		int axis = -1, splitBin = -1;
		float bestCost = DEVOGI_INF;
		for ( int ax = 0; ax < 3; ax++ ) {
			const float lo = cmins[ax], hi = cmaxs[ax];
			if ( hi - lo < 1e-4f )
				continue;
			const float scale = BINS / ( hi - lo );
			gvec3 bmin[BINS], bmax[BINS];
			int bcount[BINS] = {};
			for ( int k = 0; k < BINS; k++ ) {
				bmin[k] = gvec3( DEVOGI_INF, DEVOGI_INF, DEVOGI_INF );
				bmax[k] = gvec3( -DEVOGI_INF, -DEVOGI_INF, -DEVOGI_INF );
			}
			for ( int i = it.first; i < it.first + it.count; i++ ) {
				const int t = scene.triIndex[i];
				const int k = Com_Clampi( 0, BINS - 1, (int)( ( centers[t][ax] - lo ) * scale ) );
				bcount[k]++;
				bmin[k] = gmin( bmin[k], tmins[t] );
				bmax[k] = gmax( bmax[k], tmaxs[t] );
			}
			// sweep from the right, then from the left: cost of every split plane
			auto area = []( const gvec3 &mn, const gvec3 &mx ) {
				const gvec3 e = mx - mn;
				return e.x < 0 ? 0.0f : e.x * e.y + e.y * e.z + e.z * e.x;
			};
			float rightArea[BINS];
			int rightCount[BINS];
			gvec3 rmn( DEVOGI_INF, DEVOGI_INF, DEVOGI_INF ), rmx( -DEVOGI_INF, -DEVOGI_INF, -DEVOGI_INF );
			int rc = 0;
			for ( int k = BINS - 1; k > 0; k-- ) {
				rc += bcount[k];
				rmn = gmin( rmn, bmin[k] );
				rmx = gmax( rmx, bmax[k] );
				rightArea[k] = area( rmn, rmx );
				rightCount[k] = rc;
			}
			gvec3 lmn( DEVOGI_INF, DEVOGI_INF, DEVOGI_INF ), lmx( -DEVOGI_INF, -DEVOGI_INF, -DEVOGI_INF );
			int lc = 0;
			for ( int k = 1; k < BINS; k++ ) {
				lc += bcount[k - 1];
				lmn = gmin( lmn, bmin[k - 1] );
				lmx = gmax( lmx, bmax[k - 1] );
				if ( !lc || !rightCount[k] )
					continue;
				const float cost = lc * area( lmn, lmx ) + rightCount[k] * rightArea[k];
				if ( cost < bestCost ) {
					bestCost = cost;
					axis = ax;
					splitBin = k;
				}
			}
		}

		int mid;
		if ( axis >= 0 ) {
			const float lo = cmins[axis], scale = BINS / ( cmaxs[axis] - cmins[axis] );
			mid = (int)( std::partition( scene.triIndex.begin() + it.first, scene.triIndex.begin() + it.first + it.count,
				[&]( int t ) { return Com_Clampi( 0, BINS - 1, (int)( ( centers[t][axis] - lo ) * scale ) ) < splitBin; } )
				- scene.triIndex.begin() );
		} else {
			mid = it.first;
		}
		if ( mid <= it.first || mid >= it.first + it.count ) {
			// no useful plane (all centres in one spot): cut the list in half
			gvec3 ext = cmaxs - cmins;
			axis = ( ext.x > ext.y && ext.x > ext.z ) ? 0 : ( ext.y > ext.z ? 1 : 2 );
			mid = it.first + it.count / 2;
			std::nth_element( scene.triIndex.begin() + it.first, scene.triIndex.begin() + mid,
				scene.triIndex.begin() + it.first + it.count,
				[&]( int a, int b ) { return centers[a][axis] < centers[b][axis]; } );
		}

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
// how much farther a face seen from behind counts in gi_trace (and the GPU tracer),
// so a face that looks at the ray wins over a back face in the same spot
#define DEVOGI_BACKFACE_BIAS 0.25f

static bool gi_trace( const giScene &scene, const gvec3 &o, const gvec3 &d, float tmax, giHit &hit, bool anyHit, float tmin = 0.01f ) {
	if ( scene.nodes.empty() )
		return false;
	gvec3 inv( 1.0f / ( fabsf( d.x ) > 1e-9f ? d.x : 1e-9f ),
			   1.0f / ( fabsf( d.y ) > 1e-9f ? d.y : 1e-9f ),
			   1.0f / ( fabsf( d.z ) > 1e-9f ? d.z : 1e-9f ) );
	hit.t = tmax;
	hit.tri = -1;
	float bestScore = tmax;	// distance of the best hit so far, back faces pushed back a little

	int stack[64];
	int sp = 0;
	stack[sp++] = 0;
	while ( sp ) {
		const giBVHNode &node = scene.nodes[stack[--sp]];
		if ( !gi_ray_box( o, inv, node.mins, node.maxs, bestScore ) )
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
				// Faces seen from behind count as a little farther away than they are.
				// Maps often have two faces in the same spot facing opposite ways (a floor
				// and the underside of the slab on it, a door frame against a step). The
				// ray hits both at the same distance, and picking the back one at random
				// made whole faces black. This way the face that looks at the ray wins.
				const float score = !anyHit && gdot( d, t.n ) > 0 ? tt + DEVOGI_BACKFACE_BIAS : tt;	// shadow rays take any hit
				if ( tt > tmin && score < bestScore ) {
					bestScore = score;
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
		if ( !Q_stricmp( token, "q3map_sun" ) || !Q_stricmp( token, "q3map_sunExt" ) || !Q_stricmp( token, "sun" )
			|| !Q_stricmp( token, "q3gl2_sun" ) ) {
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
The sky as a light source. Its light comes from the sky box images themselves
(skyParms <farbox>: six faces _rt _bk _lf _ft _up _dn), looked up by direction,
so a bright horizon, a sunset side or a dark storm half light the level the way
they look. Each face is boiled down to 16x16 average colours. Without images the
sky shines in one colour (its first texture's average, or the sun's colour).
*/
struct giSky {
	bool	images = false;
	gvec3	face[6][16][16];
	gvec3	uniform = gvec3( 0.5f, 0.5f, 0.5f );
	float	scale = 0.0f;		// image colour (0..1) -> light, in lightmap units
};
static giSky gi_sky;

// loads one sky box face into 16x16 averages; false if the image is missing
static bool gi_sky_face( const char *name, gvec3 out[16][16] ) {
	byte *pic = NULL;
	int w = 0, h = 0;
	dg_load_image( name, &pic, &w, &h );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic )
			dg_free_image( pic );
		return false;
	}
	for ( int by = 0; by < 16; by++ ) {
		for ( int bx = 0; bx < 16; bx++ ) {
			double r = 0, g = 0, b = 0;
			int n = 0;
			const int x0 = bx * w / 16, x1 = Q_max( x0 + 1, ( bx + 1 ) * w / 16 );
			const int y0 = by * h / 16, y1 = Q_max( y0 + 1, ( by + 1 ) * h / 16 );
			const int step = Q_max( 1, ( x1 - x0 ) / 8 );
			for ( int y = y0; y < y1; y += step )
				for ( int x = x0; x < x1; x += step, n++ ) {
					const byte *px = &pic[( (size_t)y * w + x ) * 4];
					r += px[0]; g += px[1]; b += px[2];
				}
			out[by][bx] = n ? gvec3( (float)( r / n / 255.0 ), (float)( g / n / 255.0 ), (float)( b / n / 255.0 ) ) : gvec3();
		}
	}
	dg_free_image( pic );
	return true;
}

// reads a sky shader: its sky box images (skyParms) or, failing that, its first texture
static void gi_sky_setup( const char *shaderName, const gvec3 &tint ) {
	gi_sky.images = false;
	gi_sky.uniform = tint;
	const char *p = R_DevoGI_FindShaderText( shaderName );
	char farbox[MAX_QPATH] = "";
	int depth = 0;
	while ( p ) {
		const char *token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		if ( token[0] == '{' ) { depth++; continue; }
		if ( token[0] == '}' ) { if ( --depth <= 0 ) break; continue; }
		if ( depth == 1 && !Q_stricmp( token, "skyparms" ) ) {
			token = COM_ParseExt( &p, qfalse );
			if ( token[0] && token[0] != '-' )
				Q_strncpyz( farbox, token, sizeof( farbox ) );
		}
	}
	if ( farbox[0] ) {
		// the face order and orientation of Q3's sky box (tr_sky.c)
		static const char *suffix[6] = { "rt", "bk", "lf", "ft", "up", "dn" };
		bool all = true;
		for ( int f = 0; f < 6 && all; f++ ) {
			char name[MAX_QPATH];
			Com_sprintf( name, sizeof( name ), "%s_%s", farbox, suffix[f] );
			all = gi_sky_face( name, gi_sky.face[f] );
		}
		gi_sky.images = all;
	}
	if ( !gi_sky.images ) {
		char tex[MAX_QPATH];
		gi_shader_texture( shaderName, tex, sizeof( tex ) );
		if ( Q_stricmp( tex, shaderName ) ) {
			gvec3 c = gi_shader_albedo( tex );
			if ( c.x + c.y + c.z > 0.05f )
				gi_sky.uniform = c;
		}
	}
}

// the light the sky sends from direction d (pointing away from the scene, into the sky)
static gvec3 gi_sky_radiance( const gvec3 &d ) {
	if ( !gi_sky.images )
		return gi_sky.uniform * gi_sky.scale;
	// which face, and where on it (the inverse of tr_sky.c's MakeSkyVec)
	const float ax = fabsf( d.x ), ay = fabsf( d.y ), az = fabsf( d.z );
	int f;
	float s, t;
	if ( ax >= ay && ax >= az ) {
		if ( d.x > 0 ) { f = 0; s = -d.y / ax; t = d.z / ax; }
		else { f = 1; s = d.y / ax; t = d.z / ax; }
	} else if ( ay >= az ) {
		if ( d.y > 0 ) { f = 2; s = d.x / ay; t = d.z / ay; }
		else { f = 3; s = -d.x / ay; t = d.z / ay; }
	} else {
		if ( d.z > 0 ) { f = 4; s = -d.y / az; t = -d.x / az; }
		else { f = 5; s = -d.y / az; t = d.x / az; }
	}
	const int x = Com_Clampi( 0, 15, (int)( ( s + 1.0f ) * 0.5f * 16.0f ) );
	const int y = Com_Clampi( 0, 15, (int)( ( 1.0f - ( t + 1.0f ) * 0.5f ) * 16.0f ) );
	return gi_sky.face[f][y][x] * gi_sky.scale;
}

// the light a glowing (unlit) surface sends out at texture coordinates (s, t)
static inline gvec3 gi_emit_radiance( const giShader &sh, float s, float t ) {
	const float fs = s - floorf( s ), ft = t - floorf( t );
	const int x = Com_Clampi( 0, sh.emitW - 1, (int)( fs * sh.emitW ) );
	const int y = Com_Clampi( 0, sh.emitH - 1, (int)( ft * sh.emitH ) );
	return sh.emit[(size_t)y * sh.emitW + x];
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
	float	sizeMax;	// its longer side: a stretched texel (a steep slope whose lightmap
						// was projected from above) is much longer one way than the other
	bool	edge;		// its centre lies just outside its triangle (a border texel)
	bool	buried;		// its centre was inside solid geometry (a floor running on under a wall):
						// nobody sees it, it takes the light of its nearest visible neighbour
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
	// how far one pixel to the right / one pixel down goes in the world
	const gvec3 perX = ( t.e1 * ( py[2] - py[0] ) - t.e2 * ( py[1] - py[0] ) ) * ( 1.0f / area );
	const gvec3 perY = ( t.e2 * ( px[1] - px[0] ) - t.e1 * ( px[2] - px[0] ) ) * ( 1.0f / area );
	const float texelSizeMax = Q_max( texelSize, Q_max( sqrtf( gdot( perX, perX ) ), sqrtf( gdot( perY, perY ) ) ) );

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
			tx.sizeMax = texelSizeMax;
			tx.edge = !inside;
			tx.buried = false;
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
Surfaces that share a spot in the lightmap

When the original compiler (q3map2) saw two surfaces whose light came out
exactly the same - two identical stair steps, the two ends of a bench - it
stored that light once and pointed both surfaces at it. Fine for the old
lighting, but DevoGI lights every surface on its own: one spot can only hold
one surface's light, so the other surface showed light from somewhere else
(often black). Here every sharing surface after the first gets its own spot,
on new pages added after the BSP's own. The renderer moves the surfaces the
same way when it loads the map (DG_ApplyLightmapMoves).
===========================================================================
*/

#define DEVOGI_MOVE_PAD	2	// empty pixels kept around each moved surface (at 128 per page)

// calls fn( x, y, inside ) for every pixel of a 128x128 page that a triangle covers
// (the same coverage gi_rasterize_tri uses, including its slack for border pixels);
// inside: the pixel's centre lies clearly inside the triangle, not just near its edge
template<typename F>
static void gi_cover_tri_128( const giScene &scene, const giTri &t, F fn ) {
	const int N = DEVOGI_BSP_LM_SIZE;
	float px[3], py[3];
	for ( int i = 0; i < 3; i++ ) {
		px[i] = scene.verts[t.vert[i]].lm[0][0] * N;
		py[i] = scene.verts[t.vert[i]].lm[0][1] * N;
	}
	const float area = ( px[1] - px[0] ) * ( py[2] - py[0] ) - ( px[2] - px[0] ) * ( py[1] - py[0] );
	if ( fabsf( area ) < 1e-6f )
		return;
	const float slack = 0.75f / sqrtf( fabsf( area ) );
	const float margin = 0.05f / sqrtf( fabsf( area ) );	// a twentieth of a pixel inside
	const int x0 = Q_max( 0, (int)floorf( Q_min( px[0], Q_min( px[1], px[2] ) ) - 1 ) );
	const int y0 = Q_max( 0, (int)floorf( Q_min( py[0], Q_min( py[1], py[2] ) ) - 1 ) );
	const int x1 = Q_min( N - 1, (int)ceilf( Q_max( px[0], Q_max( px[1], px[2] ) ) + 1 ) );
	const int y1 = Q_min( N - 1, (int)ceilf( Q_max( py[0], Q_max( py[1], py[2] ) ) + 1 ) );
	for ( int y = y0; y <= y1; y++ ) {
		for ( int x = x0; x <= x1; x++ ) {
			const float cx = x + 0.5f, cy = y + 0.5f;
			const float w1 = ( ( cx - px[0] ) * ( py[2] - py[0] ) - ( px[2] - px[0] ) * ( cy - py[0] ) ) / area;
			const float w2 = ( ( px[1] - px[0] ) * ( cy - py[0] ) - ( cx - px[0] ) * ( py[1] - py[0] ) ) / area;
			const float w0 = 1 - w1 - w2;
			if ( w0 >= -slack && w1 >= -slack && w2 >= -slack )
				fn( x, y, w0 > margin && w1 > margin && w2 > margin );
		}
	}
}

/*
Finds the sharing surfaces and moves them (in the bake's own copy of the
geometry), along with every surface that gets more detail (detail[surf] > 1:
that many times more lightmap pixels each way, see gi_detail_scales). Returns the new number of lightmap pages; "moves" says what moved.
"srcOut" gets the BSP's own 128x128 lightmaps with every moved surface's old
light copied to its new spot: the old light is still used for light styles and
for matching the lamps' brightness.
*/
static int gi_unshare_lightmaps( giScene &scene, const byte *src, int bspPages, const std::vector<int> &detail,
	std::vector<dgLightmapMove> &moves, std::vector<byte> &srcOut ) {
	const int N = DEVOGI_BSP_LM_SIZE;
	moves.clear();
	srcOut.assign( src, src + (size_t)bspPages * N * N * 3 );
	const int numSurfs = (int)scene.surfs.size();

	// maps with deluxemaps (light directions on every odd page) are left alone:
	// their page numbering has to stay as it is
	bool anyOdd = false;
	for ( const giSurf &gs : scene.surfs )
		if ( gs.lightmapNum[0] >= 0 && ( gs.lightmapNum[0] & 1 ) )
			anyOdd = true;
	if ( bspPages > 1 && !anyOdd )
		return bspPages;

	// each surface's triangles (they were added surface by surface)
	std::vector<int> firstTri( numSurfs + 1, 0 );
	for ( const giTri &t : scene.tris )
		firstTri[t.surf + 1]++;
	for ( int i = 0; i < numSurfs; i++ )
		firstTri[i + 1] += firstTri[i];

	// walk the surfaces, biggest first: the first one to cover a pixel owns it. A surface is
	// a sharer if most of its pixels are already owned by another one, or if any pixel
	// well inside it is also well inside another surface (the old compiler fits small
	// surfaces into unused corners of big odd-shaped ones; two surfaces truly sharing
	// a pixel, not just touching at a border, only happens when the light is shared).
	std::vector<int> owner( (size_t)bspPages * N * N, -1 );
	std::vector<int> ownerInside( (size_t)bspPages * N * N, -1 );	// the same, clearly inside only
	std::vector<int> toMove;
	std::vector<int> mine, mineInside;

	// biggest surfaces first: of two that share, the bigger one keeps its spot and the
	// smaller one moves (a thin face drawn inside a big one's block can't claim it)
	std::vector<std::pair<float, int>> bySize( numSurfs );
	for ( int s = 0; s < numSurfs; s++ ) {
		float lmArea = 0;
		for ( int ti = firstTri[s]; ti < firstTri[s + 1]; ti++ ) {
			const giTri &t = scene.tris[ti];
			const float *a = scene.verts[t.vert[0]].lm[0], *b = scene.verts[t.vert[1]].lm[0], *c = scene.verts[t.vert[2]].lm[0];
			lmArea += 0.5f * fabsf( ( b[0] - a[0] ) * ( c[1] - a[1] ) - ( c[0] - a[0] ) * ( b[1] - a[1] ) );
		}
		bySize[s] = std::make_pair( -lmArea, s );
	}
	std::sort( bySize.begin(), bySize.end() );

	for ( const std::pair<float, int> &entry : bySize ) {
		const int s = entry.second;
		const giSurf &gs = scene.surfs[s];
		const int page = gs.lightmapNum[0];
		if ( page < 0 || page >= bspPages || gs.lightmapStyle[0] == LS_LSNONE || gs.sky )
			continue;
		bool extraStyles = false;	// switchable lights: more slots that would have to move too
		for ( int k = 1; k < MAXLIGHTMAPS; k++ )
			if ( gs.lightmapStyle[k] != LS_LSNONE )
				extraStyles = true;
		mine.clear();
		mineInside.clear();
		int taken = 0, takenInside = 0;
		for ( int ti = firstTri[s]; ti < firstTri[s + 1]; ti++ ) {
			gi_cover_tri_128( scene, scene.tris[ti], [&]( int x, int y, bool inside ) {
				const size_t pix = ( (size_t)page * N + y ) * N + x;
				if ( inside && ownerInside[pix] != s ) {
					if ( ownerInside[pix] >= 0 ) {
						takenInside++;
					} else {
						ownerInside[pix] = s;
						mineInside.push_back( (int)pix );
					}
				}
				if ( owner[pix] == s )
					return;
				if ( owner[pix] >= 0 ) {
					taken++;
				} else {
					owner[pix] = s;
					mine.push_back( (int)pix );
				}
			} );
		}
		const int covered = taken + (int)mine.size();
		if ( !extraStyles && covered > 0 && ( taken * 2 >= covered || takenInside >= 2 ) ) {
			for ( int pix : mine )
				owner[pix] = -1;	// give back what it grabbed: it gets a spot of its own
			for ( int pix : mineInside )
				ownerInside[pix] = -1;
			toMove.push_back( s );
		} else if ( !extraStyles && covered > 0 && s < (int)detail.size() && detail[s] > 1 ) {
			toMove.push_back( s );	// moves for more detail (it keeps its claim, so sharers still move)
		}
	}
	if ( toMove.empty() )
		return bspPages;

	// the lightmap rectangle of each surface to move: x0, y0, w0, h0 its block on the old
	// page, scale how much bigger it gets, w, h its new size with an empty border
	struct moveRect_t { int surf, x0, y0, w0, h0, scale, w, h; };
	std::vector<moveRect_t> rects;
	for ( int s : toMove ) {
		float mins[2] = { 1e9f, 1e9f }, maxs[2] = { -1e9f, -1e9f };
		for ( int ti = firstTri[s]; ti < firstTri[s + 1]; ti++ ) {
			for ( int k = 0; k < 3; k++ ) {
				const giVert &v = scene.verts[scene.tris[ti].vert[k]];
				for ( int c = 0; c < 2; c++ ) {
					mins[c] = Q_min( mins[c], v.lm[0][c] * N );
					maxs[c] = Q_max( maxs[c], v.lm[0][c] * N );
				}
			}
		}
		moveRect_t r;
		r.surf = s;
		r.x0 = (int)floorf( mins[0] );
		r.y0 = (int)floorf( mins[1] );
		r.w0 = Q_max( 1, (int)ceilf( maxs[0] ) - r.x0 );
		r.h0 = Q_max( 1, (int)ceilf( maxs[1] ) - r.y0 );
		r.scale = s < (int)detail.size() ? Q_max( 1, detail[s] ) : 1;
		while ( r.scale > 1 && Q_max( r.w0, r.h0 ) * r.scale + 2 * DEVOGI_MOVE_PAD > N )
			r.scale /= 2;	// too big for a page at that detail
		r.w = r.w0 * r.scale + 2 * DEVOGI_MOVE_PAD;
		r.h = r.h0 * r.scale + 2 * DEVOGI_MOVE_PAD;
		if ( r.w > N || r.h > N )
			continue;	// can't happen in a valid map
		rects.push_back( r );
	}

	// pack them onto new pages in rows ("shelves"), tallest first
	std::sort( rects.begin(), rects.end(), []( const moveRect_t &a, const moveRect_t &b ) {
		return a.h != b.h ? a.h > b.h : a.surf < b.surf;
	} );
	int page = bspPages, shelfX = 0, shelfY = 0, shelfH = 0;
	for ( const moveRect_t &r : rects ) {
		if ( shelfX + r.w > N ) {	// row full: start the next one
			shelfY += shelfH;
			shelfX = 0;
			shelfH = 0;
		}
		if ( shelfY + r.h > N ) {	// page full: start the next one
			page++;
			shelfX = shelfY = shelfH = 0;
		}
		// new coordinate = old * scale + d (in pixels of a 128 page), so that the old
		// block's corner lands just inside the border of the new spot
		dgLightmapMove m;
		m.surf = r.surf;
		m.page = page;
		m.dx = shelfX + DEVOGI_MOVE_PAD - r.x0 * r.scale;
		m.dy = shelfY + DEVOGI_MOVE_PAD - r.y0 * r.scale;
		m.scale = r.scale;
		moves.push_back( m );

		// its old light goes along (enlarged with it), border included
		if ( srcOut.size() < (size_t)( page + 1 ) * N * N * 3 )
			srcOut.resize( (size_t)( page + 1 ) * N * N * 3, 0 );
		const int from = scene.surfs[r.surf].lightmapNum[0];
		for ( int y = 0; y < r.h; y++ ) {
			const int sy = (int)floorf( ( y + 0.5f - DEVOGI_MOVE_PAD ) / r.scale ) + r.y0;
			for ( int x = 0; x < r.w; x++ ) {
				const int sx = (int)floorf( ( x + 0.5f - DEVOGI_MOVE_PAD ) / r.scale ) + r.x0;
				if ( sx < 0 || sy < 0 || sx >= N || sy >= N )
					continue;
				memcpy( &srcOut[( ( (size_t)page * N + shelfY + y ) * N + shelfX + x ) * 3],
					&src[( ( (size_t)from * N + sy ) * N + sx ) * 3], 3 );
			}
		}
		shelfX += r.w;
		shelfH = Q_max( shelfH, r.h );
	}
	std::sort( moves.begin(), moves.end(), []( const dgLightmapMove &a, const dgLightmapMove &b ) { return a.surf < b.surf; } );

	// move them in our copy of the geometry
	std::vector<char> shifted( scene.verts.size(), 0 );
	for ( const dgLightmapMove &m : moves ) {
		scene.surfs[m.surf].lightmapNum[0] = m.page;
		for ( int ti = firstTri[m.surf]; ti < firstTri[m.surf + 1]; ti++ ) {
			for ( int k = 0; k < 3; k++ ) {
				const int v = scene.tris[ti].vert[k];
				if ( shifted[v] )
					continue;
				shifted[v] = 1;
				scene.verts[v].lm[0][0] = scene.verts[v].lm[0][0] * m.scale + m.dx / (float)N;
				scene.verts[v].lm[0][1] = scene.verts[v].lm[0][1] * m.scale + m.dy / (float)N;
			}
		}
	}
	return page + 1;
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

	// calls f( item ) for everything in the cubes within "radius" of p
	template<typename F>
	void queryRadius( const gvec3 &p, float radius, F f ) const {
		if ( items.empty() )
			return;
		const float inv = 1.0f / cell;
		const int x0 = (int)floorf( ( p.x - radius ) * inv ), x1 = (int)floorf( ( p.x + radius ) * inv );
		const int y0 = (int)floorf( ( p.y - radius ) * inv ), y1 = (int)floorf( ( p.y + radius ) * inv );
		const int z0 = (int)floorf( ( p.z - radius ) * inv ), z1 = (int)floorf( ( p.z + radius ) * inv );
		for ( int z = z0; z <= z1; z++ )
			for ( int y = y0; y <= y1; y++ )
				for ( int x = x0; x <= x1; x++ ) {
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
	const float aa = 1.0f + Com_Clamp( 0.25f, 4.0f, g_set.aa );
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		// a few of its own texels wide: where the lightmap is stretched, a light change
		// squeezed into one long texel shows as a staircase; where a surface got more
		// detail (gi_detail_scales), a smaller radius keeps its shadows crisp
		const float r = Com_Clamp( Q_min( radius, 1.0f ), Q_max( radius, 24.0f ), tx.sizeMax * aa );
		const float r2 = r * r;
		T sum = T();
		float wsum = 0;
		hash.queryRadius( tx.pos, r, [&]( int o ) {
			const giTexel &ot = bake.texels[o];
			const gvec3 d = ot.pos - tx.pos;
			const float d2 = gdot( d, d );
			if ( d2 > r2 || !gi_texels_compatible( tx, ot, r * 0.75f ) )
				return;
			if ( ot.buried && !tx.buried )
				return;	// light from inside a wall must not spread to visible texels
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

/*
Moves one sample point out of walls and away from them (both steps described
below). pos: the point, on its surface; faceNormal: the surface's facing; size:
how big an area the point stands for (a texel's size); surf: its own surface,
which never counts as a wall. Returns 1 if it had to leave a wall, 2 if it was
only padded away from one, 0 if it was fine.
*/
static int gi_fix_point( const giScene &scene, gvec3 &pos, const gvec3 &faceNormal, float size, int surf ) {
	// how close a sample point may come to another surface: more than the 1.5 units that
	// rays skip at their start (DEVOGI_RAY_TMIN), or rays would slip through that surface
	const float margin = DEVOGI_RAY_TMIN + 1.0f;
	int result = 0;
	{
		gvec3 t, b;
		gi_basis( faceNormal, t, b );
		const float s = 0.70710678f;
		const gvec3 dirs[8] = { t, t * -1.0f, b, b * -1.0f,
			( t + b ) * s, ( t - b ) * s, ( b - t ) * s, ( t + b ) * -s };

		// 1. inside a wall: leave it through its nearest side (the closest back face)
		// the corner of a texel is ~0.7 texels away; look a little further for safety
		const float reach = size * 1.0f + 2.0f;
		for ( int pass = 0; pass < 2; pass++ ) {
			const gvec3 o = pos + faceNormal * 0.1f;
			float best = DEVOGI_INF;
			gvec3 bestDir;
			for ( const gvec3 &d : dirs ) {
				giHit hit;
				if ( !gi_trace( scene, o, d, reach, hit, false ) )
					continue;
				const giTri &ht = scene.tris[hit.tri];
				if ( scene.surfs[ht.surf].sky || gdot( ht.n, d ) <= 0 )
					continue;	// a wall's front face: we are outside it, all good
				if ( hit.t < best ) {
					best = hit.t;
					bestDir = d;
				}
			}
			if ( best >= DEVOGI_INF )
				break;
			// step out through it, with a safety margin past its surface
			pos = o + bestDir * ( best + Q_max( margin, size * 0.5f ) ) - faceNormal * 0.1f;
			result = 1;
		}

		/*
		2. Padding: a point right where its ceiling meets a wall (a polygon's edge texels
		sit exactly on that line) is too close to the wall: rays leaving it skip the
		first 1.5 units, slip through the wall and bring back light from the other side
		- a thin bright line along the corner. So the point slides along its own surface
		until it is at least 'margin' away from every wall around it.
		*/
		for ( int pass = 0; pass < 2; pass++ ) {
			const gvec3 o = pos + faceNormal * 0.5f;
			gvec3 push;
			bool close = false;
			for ( const gvec3 &d : dirs ) {
				giHit hit;
				if ( !gi_trace( scene, o, d, margin, hit, false ) )
					continue;
				const giTri &ht = scene.tris[hit.tri];
				if ( scene.surfs[ht.surf].sky || ht.surf == surf )
					continue;
				if ( gdot( ht.n, faceNormal ) > 0.95f )
					continue;	// a surface lying flat in our own plane is not a wall
				push += d * -( margin - hit.t );
				close = true;
			}
			if ( !close )
				break;
			// stay on our own plane: only slide along it
			push = push - faceNormal * gdot( push, faceNormal );
			const float len = sqrtf( gdot( push, push ) );
			if ( len < 1e-3f )
				break;
			const float step = Q_min( len, margin );
			pos += push * ( step / len );
			if ( !result )
				result = 2;
		}
	}
	return result;
}

static void gi_fix_texel_positions( giBake &bake ) {
	std::atomic<int> moved( 0 ), padded( 0 );
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		giTexel &tx = bake.texels[i];
		const int r = gi_fix_point( bake.scene, tx.pos, tx.faceNormal, tx.size, tx.surf );
		// a texel well inside its polygon that still starts inside a wall lies where its
		// surface runs on under that wall: whichever way it was pushed out, it may have
		// come out on the far side, in another room
		tx.buried = r == 1 && !tx.edge;
		if ( r == 1 )
			moved.fetch_add( 1 );
		else if ( r == 2 )
			padded.fetch_add( 1 );
	} );
	dg_print( "DevoGI: moved %d texel sample points out of walls, %d away from corners\n", moved.load(), padded.load() );
}

/*
For every buried texel (see giTexel::buried), the nearest texel of the same
surface that is not buried, searched outwards in the lightmap page; -1 if none
within reach. Its light is copied over at the end of the bake, so the GPU's
texture filtering at a wall's foot blends in this room's light, not the next.
*/
static std::vector<int> gi_buried_sources( const giBake &bake ) {
	std::vector<int> src( bake.texels.size(), -1 );
	const int reach = 12;
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		if ( !tx.buried )
			return;
		int best = -1, bestD = INT32_MAX;
		for ( int dy = -reach; dy <= reach; dy++ ) {
			for ( int dx = -reach; dx <= reach; dx++ ) {
				const int d = dx * dx + dy * dy;
				if ( d >= bestD || d > reach * reach )
					continue;
				const int x = tx.x + dx, y = tx.y + dy;
				if ( x < 0 || y < 0 || x >= DEVOGI_LM_SIZE || y >= DEVOGI_LM_SIZE )
					continue;
				const int o = bake.owner[( (size_t)tx.page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x];
				if ( o < 0 || bake.texels[o].buried || bake.texels[o].surf != tx.surf )
					continue;
				best = o;
				bestD = d;
			}
		}
		src[i] = best;
	} );
	return src;
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
How much of a glowing rectangle a point sees (its "form factor", 0..1), exactly,
with Lambert's formula for polygons: for each edge, the angle it spans as seen
from the point, times how much that edge's plane faces the point's normal. Parts
of the rectangle behind the point's surface are cut off first. The light from a
lamp is then strength * pi * formFactor / area - no random samples, so no noise.
*/
static float gi_rect_form_factor( const gvec3 &p, const gvec3 &n, const gvec3 corner[4] ) {
	// keep only the part in front of the point's surface (Sutherland-Hodgman clipping)
	gvec3 poly[8];
	int count = 0;
	for ( int i = 0; i < 4; i++ ) {
		const gvec3 &a = corner[i], &b = corner[( i + 1 ) & 3];
		const float da = gdot( a - p, n ) - 0.01f, db = gdot( b - p, n ) - 0.01f;
		if ( da >= 0 )
			poly[count++] = a;
		if ( ( da >= 0 ) != ( db >= 0 ) )
			poly[count++] = a + ( b - a ) * ( da / ( da - db ) );
	}
	if ( count < 3 )
		return 0.0f;
	float sum = 0;
	for ( int i = 0; i < count; i++ ) {
		const gvec3 a = gnorm( poly[i] - p ), b = gnorm( poly[( i + 1 ) % count] - p );
		const gvec3 c = gcross( a, b );
		const float len = sqrtf( gdot( c, c ) );
		if ( len < 1e-7f )
			continue;
		const float angle = acosf( Com_Clamp( -1.0f, 1.0f, gdot( a, b ) ) );
		sum += angle * gdot( c, n ) / len;
	}
	return Q_min( 1.0f, fabsf( sum ) / 6.2831853f );
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
				// shadow rays end this close to the lamp: samples on the real glowing
				// rectangle sit just 1 unit off its surface, so only rounding is skipped.
				// (A lamp without a known shape is sampled on a square 4 units out, whose
				// own fixture around it must not shadow it.)
				const bool hasRect = lt.size > 0 && lt.halfU > 0;
				const float emitterClearance = hasRect ? 0.5f : 4.0f + lt.size * 0.25f;

				// one sample: from spot (ux, uy) of the texel to spot (jx, jy) of the lamp
				// (all -0.5..0.5 each way)
				auto lampSample = [&]( float ux, float uy, float jx, float jy, float &sum, float &all, int &vis, int &tested ) {
					gvec3 target = lt.origin;
					if ( hasRect )
						target = lt.rectCentre + lt.normal * 1.0f + lt.axisU * ( jx * 2.0f * lt.halfU ) + lt.axisV * ( jy * 2.0f * lt.halfV );
					else if ( lt.size > 0 )
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
					all += contrib;
					giHit hit;
					if ( ds - emitterClearance <= DEVOGI_RAY_TMIN
						|| !gi_trace( scene, so, Ls, ds - emitterClearance, hit, true, DEVOGI_RAY_TMIN ) ) {
						sum += contrib;
						vis++;
					}
				};

				float sum = 0, all = 0;	// light of the samples that got through / of all of them
				int vis = 0, tested = 0, taken = 0;
				if ( !alwaysFull ) {
					// 4 probes: from the texel's 4 quarters, towards the lamp's 4 quarters
					for ( int s = 0; s < 4; s++, taken++ ) {
						const float qx = ( ( s & 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f;
						const float qy = ( ( s >> 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f;
						const float jx = lt.size > 0 ? ( ( ( s >> 1 ) ^ 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f : 0.0f;
						const float jy = lt.size > 0 ? ( ( s & 1 ) + gi_rand( rng ) ) * 0.5f - 0.5f : 0.0f;
						lampSample( qx, qy, jx, jy, sum, all, vis, tested );
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
						lampSample( ux, uy, jx, jy, sum, all, vis, tested );
					}
				}
				if ( sum <= 0 )
					continue;
				if ( hasRect ) {
					// Close to a lamp, the light changes very fast across it, and averaging
					// random samples there gave blotchy, noisy light. So the samples only say
					// how much of the lamp is hidden (sum / all), and the light itself is
					// computed exactly. Unshadowed, the result is perfectly smooth.
					const gvec3 corner[4] = {
						lt.rectCentre - lt.axisU * lt.halfU - lt.axisV * lt.halfV,
						lt.rectCentre + lt.axisU * lt.halfU - lt.axisV * lt.halfV,
						lt.rectCentre + lt.axisU * lt.halfU + lt.axisV * lt.halfV,
						lt.rectCentre - lt.axisU * lt.halfU + lt.axisV * lt.halfV };
					if ( gdot( o - lt.rectCentre, lt.normal ) <= 0 )
						continue;	// behind the glowing surface
					const float rectArea = 4.0f * lt.halfU * lt.halfV;
					const float exact = lt.photons * 3.14159265f * gi_rect_form_factor( o, normal, corner ) / rectArea;
					emit( lt.group, lt.color * ( exact * Q_min( 1.0f, sum / all ) ) );
				} else {
					emit( lt.group, lt.color * ( sum / (float)Q_max( 1, taken ) ) );
				}
			}
		}
	}

	if ( gi_hasSun ) {
		const float cosS = gdot( normal, gi_sunDir );
		if ( cosS > 0 ) {
			// the sun is a small round disc (about 1 degree, see gi_sun_disc): soft sun edges
			gvec3 st, sb;
			gi_basis( gi_sunDir, st, sb );
			// one sample: from spot (ux, uy) of the texel towards spot (jx, jy) of the sun disc
			auto sunVisible = [&]( float ux, float uy, float jx, float jy ) {
				float dx, dy;
				gi_sun_disc( jx, jy, dx, dy );
				const gvec3 dir = gnorm( gi_sunDir + st * dx + sb * dy );
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
static gvec3 gi_hit_radiance( const giBake &bake, const giHit &hit, int styleValue, const gvec3 &dir ) {
	const giScene &scene = bake.scene;
	const giTri &t = scene.tris[hit.tri];
	const giSurf &s = scene.surfs[t.surf];
	if ( s.sky )
		return styleValue == 0 ? gi_sky_radiance( dir ) : gvec3();	// the open sky

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
	const giShader &sh = scene.shaders[s.shader];
	L = L * sh.albedo;
	// a glowing surface adds its own light, from the texture pixel that was hit
	if ( sh.emitW && styleValue == 0 ) {
		const float es = a.st[0] * w0 + b.st[0] * w1 + c.st[0] * w2;
		const float et = a.st[1] * w0 + b.st[1] * w1 + c.st[1] * w2;
		L += gi_emit_radiance( sh, es, et );
	}
	return L;
}

/*
Bounce light at one point ("a gather"): rays go out over the half-sphere above the
surface, each brings back the light of whatever it hits, and the average is the
light arriving there. Out of the same rays comes ambient occlusion (how much nearby
geometry surrounds the point). It is done in three steps, so the expensive middle
one can run on the CPU or, in big batches, on the GPU (devogi_gpu.cpp):

  1. gi_gather_rays: make the rays (always the same for the same point and seed)
  2. trace them: gi_trace on the CPU, or DG_GpuTrace for thousands of points at once
  3. gi_gather_shade: turn the hits into light
*/

// one gather to do: where, which way, how many rays, and where the answer goes
struct giGatherJob {
	gvec3		pos, normal, faceNormal;
	int			styleValue;
	uint32_t	seed;
	int			rays;
	bool		sheen;		// add the glossy sheen cone (not for light probes)
	float		*out;		// rgb + ambient occlusion (1 open .. 0 enclosed)
};

static inline int gi_gather_cone_rays( const giGatherJob &job ) {
	const float sheen = Com_Clamp( 0.0f, 0.5f, g_set.sheen );
	return ( sheen > 0 && job.sheen ) ? Q_max( 4, job.rays / 8 ) : 0;
}

// 1. the rays of one gather: job.rays bounce rays, then the sheen cone rays
static void gi_gather_rays( const giGatherJob &job, dgGpuRay *out ) {
	gvec3 t, b;
	gi_basis( job.normal, t, b );
	uint32_t rng = gi_hash( job.seed );
	// rays start just off the flat face (a curved surface's smooth normal could point into
	// its own neighbouring facets)
	const gvec3 o = job.pos + job.faceNormal * 1.0f;
	auto put = [&]( dgGpuRay &r, const gvec3 &d, float tmax ) {
		r.origin[0] = o.x; r.origin[1] = o.y; r.origin[2] = o.z;
		r.dir[0] = d.x; r.dir[1] = d.y; r.dir[2] = d.z;
		r.tmin = DEVOGI_RAY_TMIN;
		r.tmax = tmax;
	};
	for ( int r = 0; r < job.rays; r++ ) {
		// cosine-weighted direction: more rays near the normal, where light counts most
		const float u1 = gi_rand( rng ), u2 = gi_rand( rng );
		const float rr = sqrtf( u1 ), phi = 6.2831853f * u2;
		const float lx = rr * cosf( phi ), ly = rr * sinf( phi ), lz = sqrtf( Q_max( 0.0f, 1.0f - u1 ) );
		gvec3 d = gnorm( t * lx + b * ly + job.normal * lz );
		const float below = gdot( d, job.faceNormal );
		if ( below < 0 )
			d = gnorm( d - job.faceNormal * ( 2.0f * below ) );	// mirrored back above the surface
		put( out[r], d, 8192.0f );
	}
	// the sheen cone: within ~20 degrees of the normal
	const int cone = gi_gather_cone_rays( job );
	for ( int r = 0; r < cone; r++ ) {
		const float u1 = gi_rand( rng ) * 0.12f, u2 = gi_rand( rng );
		const float rr = sqrtf( u1 ), phi = 6.2831853f * u2;
		const gvec3 d = gnorm( t * ( rr * cosf( phi ) ) + b * ( rr * sinf( phi ) ) + job.normal * sqrtf( 1.0f - u1 ) );
		put( out[job.rays + r], d, 1024.0f );
	}
}

// 3. the light of one gather from its traced rays
static void gi_gather_shade( const giBake &bake, const giGatherJob &job, const dgGpuRay *rays, const dgGpuHit *hits ) {
	const giScene &scene = bake.scene;
	const float aoDist = Q_max( 8.0f, g_set.aoDistance );
	auto hitOf = [&]( const dgGpuHit &h ) { giHit g; g.t = h.t; g.tri = h.tri; g.u = h.u; g.v = h.v; return g; };

	gvec3 sum;
	float occlusion = 0;
	for ( int r = 0; r < job.rays; r++ ) {
		if ( hits[r].tri < 0 )
			continue;	// left the map
		const giHit hit = hitOf( hits[r] );
		const giTri &ht = scene.tris[hit.tri];
		const gvec3 d( rays[r].dir[0], rays[r].dir[1], rays[r].dir[2] );
		const bool sky = scene.surfs[ht.surf].sky;
		if ( !sky && hit.t < aoDist )
			occlusion += 1.0f - hit.t / aoDist;	// near geometry blocks more
		if ( !sky && gdot( ht.n, d ) > 0 )
			continue;	// hit the back of something: no light comes from there
		sum += gi_hit_radiance( bake, hit, job.styleValue, d );
	}
	float *out = job.out;
	const float inv = 1.0f / job.rays;
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
	const int coneRays = gi_gather_cone_rays( job );
	if ( coneRays ) {
		const float sheen = Com_Clamp( 0.0f, 0.5f, g_set.sheen );
		gvec3 cone;
		int got = 0;
		for ( int r = job.rays; r < job.rays + coneRays; r++ ) {
			if ( hits[r].tri < 0 )
				continue;
			const giHit hit = hitOf( hits[r] );
			const giTri &ht = scene.tris[hit.tri];
			const gvec3 d( rays[r].dir[0], rays[r].dir[1], rays[r].dir[2] );
			if ( !scene.surfs[ht.surf].sky && gdot( ht.n, d ) > 0 )
				continue;
			// nearer faces reflect more strongly than far ones
			const float fade = 1.0f / ( 1.0f + hit.t / 256.0f );
			cone += gi_hit_radiance( bake, hit, job.styleValue, d ) * fade;
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

// 2 on the CPU: traces the rays of one ray list with gi_trace
static void gi_trace_cpu( const giScene &scene, const dgGpuRay *rays, dgGpuHit *hits, int count, bool anyHit = false ) {
	for ( int r = 0; r < count; r++ ) {
		const gvec3 o( rays[r].origin[0], rays[r].origin[1], rays[r].origin[2] );
		const gvec3 d( rays[r].dir[0], rays[r].dir[1], rays[r].dir[2] );
		giHit hit;
		if ( gi_trace( scene, o, d, rays[r].tmax, hit, anyHit, rays[r].tmin ) ) {
			hits[r].t = hit.t; hits[r].u = hit.u; hits[r].v = hit.v; hits[r].tri = hit.tri;
		} else {
			hits[r].t = rays[r].tmax; hits[r].u = hits[r].v = 0; hits[r].tri = -1;
		}
	}
}

// one gather on the CPU, all three steps
static void gi_gather_point( const giBake &bake, const gvec3 &pos, const gvec3 &normal, const gvec3 &faceNormal,
	int styleValue, uint32_t seed, int rays, float out[4], bool allowSheen = true ) {
	giGatherJob job = { pos, normal, faceNormal, styleValue, seed, rays, allowSheen, out };
	const int total = rays + gi_gather_cone_rays( job );
	static thread_local std::vector<dgGpuRay> rayBuf;
	static thread_local std::vector<dgGpuHit> hitBuf;
	rayBuf.resize( total );
	hitBuf.resize( total );
	gi_gather_rays( job, rayBuf.data() );
	gi_trace_cpu( bake.scene, rayBuf.data(), hitBuf.data(), total );
	gi_gather_shade( bake, job, rayBuf.data(), hitBuf.data() );
}

/*
Many gathers at once. On the GPU, the rays of thousands of points are traced in one
go (a few million rays per batch); on the CPU every point is done on its own. Both
give the same result. Progress is reported for big runs.
*/
static bool gi_gpuActive;	// the GPU tracer is running for this bake
static float gi_bounceBlurRadius;	// how wide the bounce light is blurred (gi_blur_grid), in game units

static void gi_gather_many( const giBake &bake, const std::vector<giGatherJob> &jobs, bool report ) {
	if ( !gi_gpuActive ) {
		gi_parallel_for( (int)jobs.size(), [&]( int i ) {
			const giGatherJob &j = jobs[i];
			gi_gather_point( bake, j.pos, j.normal, j.faceNormal, j.styleValue, j.seed, j.rays, j.out, j.sheen );
		}, report );
		return;
	}

	const size_t maxRays = 4u * 1024u * 1024u;		// rays per round trip to the GPU
	std::vector<size_t> first( jobs.size() + 1, 0 );
	for ( size_t i = 0; i < jobs.size(); i++ )
		first[i + 1] = first[i] + jobs[i].rays + gi_gather_cone_rays( jobs[i] );

	std::vector<dgGpuRay> rays;
	std::vector<dgGpuHit> hits;
	size_t start = 0;
	while ( start < jobs.size() ) {
		size_t end = start + 1;
		while ( end < jobs.size() && first[end + 1] - first[start] <= maxRays )
			end++;
		const size_t base = first[start], count = first[end] - base;
		rays.resize( count );
		hits.resize( count );

		gi_parallel_for( (int)( end - start ), [&]( int k ) {
			gi_gather_rays( jobs[start + k], &rays[first[start + k] - base] );
		} );
		if ( !DG_GpuTrace( rays.data(), hits.data(), count, false ) ) {
			// the GPU gave up: this batch and the rest on the CPU
			gi_gpuActive = false;
			gi_parallel_for( (int)( jobs.size() - start ), [&]( int k ) {
				const giGatherJob &j = jobs[start + k];
				gi_gather_point( bake, j.pos, j.normal, j.faceNormal, j.styleValue, j.seed, j.rays, j.out, j.sheen );
			} );
			return;
		}
		gi_parallel_for( (int)( end - start ), [&]( int k ) {
			const size_t at = first[start + k] - base;
			gi_gather_shade( bake, jobs[start + k], &rays[at], &hits[at] );
		} );

		start = end;
		if ( report )
			gi_progress_draw( (float)start / jobs.size(), false );
	}
}

// traces any number of rays, on the GPU when it runs, otherwise on all CPU cores
static void gi_trace_many( const giBake &bake, const dgGpuRay *rays, dgGpuHit *hits, size_t count, bool anyHit ) {
	if ( gi_gpuActive && DG_GpuTrace( rays, hits, count, anyHit ) )
		return;
	gi_gpuActive = false;
	const int chunk = 4096;
	gi_parallel_for( (int)( ( count + chunk - 1 ) / chunk ), [&]( int c ) {
		const size_t first = (size_t)c * chunk;
		const int n = (int)Q_min( (size_t)chunk, count - first );
		gi_trace_cpu( bake.scene, rays + first, hits + first, n, anyHit );
	} );
}

/*
Blurring the bounce light. Bounce light is soft by nature, but traced with a
limited number of rays it comes out grainy - so it is blurred, a lot, across the
whole neighbourhood of every sample point. A blur that wide would also mix light
from the other side of a thin wall into a room (two spots on one floor plane
either side of a wall look like neighbours), so two points are only mixed when a
straight line between them is free: light never blurs through walls. The test
rays are traced in big batches (on the GPU when it runs). Three passes of radius R
blur about as much as one pass of 1.7 R, at a third of the cost and memory.
*/
static void gi_blur_grid( const giBake &bake, const std::vector<int> &gridPoints, const std::vector<gvec3> &texelPos,
	std::vector<float> &gathered, float radius, int passes ) {
	if ( gridPoints.empty() || radius <= 0 )
		return;
	giSpatialHash hash;
	hash.build( texelPos, gridPoints, radius );
	const float r2 = radius * radius;

	// the neighbours of every grid point (same kind of surface, within the radius)
	std::vector<std::vector<int>> near( gridPoints.size() );
	std::vector<int> slotOf( bake.texels.size(), -1 );
	for ( size_t k = 0; k < gridPoints.size(); k++ )
		slotOf[gridPoints[k]] = (int)k;
	gi_parallel_for( (int)gridPoints.size(), [&]( int k ) {
		const giTexel &tx = bake.texels[gridPoints[k]];
		hash.query( tx.pos, [&]( int o ) {
			if ( o == gridPoints[k] )
				return;
			const giTexel &ot = bake.texels[o];
			const gvec3 d = ot.pos - tx.pos;
			if ( gdot( d, d ) > r2 || !gi_texels_compatible( tx, ot, radius * 0.5f ) )
				return;
			near[k].push_back( slotOf[o] );
		} );
	} );

	// which of those can see each other (tested once, used for every pass)
	std::vector<size_t> first( gridPoints.size() + 1, 0 );
	for ( size_t k = 0; k < gridPoints.size(); k++ )
		first[k + 1] = first[k] + near[k].size();
	std::vector<char> visible( first.back(), 1 );
	{
		const size_t batchPairs = 4u * 1024u * 1024u;
		std::vector<dgGpuRay> rays;
		std::vector<dgGpuHit> hits;
		size_t start = 0;
		while ( start < gridPoints.size() ) {
			size_t end = start;
			while ( end < gridPoints.size() && first[end + 1] - first[start] <= batchPairs )
				end++;
			if ( end == start )
				end = start + 1;
			const size_t base = first[start], count = first[end] - base;
			rays.resize( count );
			hits.resize( count );
			gi_parallel_for( (int)( end - start ), [&]( int kk ) {
				const size_t k = start + kk;
				const giTexel &a = bake.texels[gridPoints[k]];
				const gvec3 from = a.pos + a.faceNormal * 2.0f;
				for ( size_t j = 0; j < near[k].size(); j++ ) {
					const giTexel &b = bake.texels[gridPoints[near[k][j]]];
					const gvec3 to = b.pos + b.faceNormal * 2.0f;
					gvec3 d = to - from;
					const float len = sqrtf( gdot( d, d ) );
					d = len > 1e-4f ? d * ( 1.0f / len ) : a.faceNormal;
					dgGpuRay &r = rays[first[k] + j - base];
					r.origin[0] = from.x; r.origin[1] = from.y; r.origin[2] = from.z;
					r.dir[0] = d.x; r.dir[1] = d.y; r.dir[2] = d.z;
					r.tmin = 0.05f;
					r.tmax = Q_max( 0.1f, len - 0.25f );
				}
			} );
			gi_trace_many( bake, rays.data(), hits.data(), count, true );
			for ( size_t q = 0; q < count; q++ )
				visible[base + q] = hits[q].tri < 0;
			start = end;
		}
	}

	// the blur itself: a soft, distance-weighted mix of the visible neighbours
	std::vector<float> value( gridPoints.size() * 4 ), next( gridPoints.size() * 4 );
	for ( size_t k = 0; k < gridPoints.size(); k++ )
		memcpy( &value[k * 4], &gathered[(size_t)gridPoints[k] * 4], 4 * sizeof( float ) );
	for ( int pass = 0; pass < passes; pass++ ) {
		gi_parallel_for( (int)gridPoints.size(), [&]( int k ) {
			const giTexel &tx = bake.texels[gridPoints[k]];
			float sum[4] = { value[k * 4], value[k * 4 + 1], value[k * 4 + 2], value[k * 4 + 3] };
			float wsum = 1.0f;
			for ( size_t j = 0; j < near[k].size(); j++ ) {
				if ( !visible[first[k] + j] )
					continue;
				const int o = near[k][j];
				const gvec3 d = bake.texels[gridPoints[o]].pos - tx.pos;
				const float f = 1.0f - gdot( d, d ) / r2;
				const float w = f * f;
				for ( int c = 0; c < 4; c++ )
					sum[c] += value[(size_t)o * 4 + c] * w;
				wsum += w;
			}
			for ( int c = 0; c < 4; c++ )
				next[(size_t)k * 4 + c] = sum[c] / wsum;
		} );
		value.swap( next );
	}
	for ( size_t k = 0; k < gridPoints.size(); k++ )
		memcpy( &gathered[(size_t)gridPoints[k] * 4], &value[k * 4], 4 * sizeof( float ) );
}

static void gi_gather( giBake &bake, const giSpatialHash &gridHash, const std::vector<int> &gridPoints,
	const std::vector<gvec3> &texelPos, float radius, int rays ) {
	const size_t n = bake.texels.size();
	std::vector<float> gathered( n * 4, 0.0f );

	// 1. gather at the grid points (the expensive part)
	{
		std::vector<giGatherJob> jobs( gridPoints.size() );
		for ( size_t k = 0; k < gridPoints.size(); k++ ) {
			const int i = gridPoints[k];
			const giTexel &tx = bake.texels[i];
			jobs[k] = { tx.pos, tx.normal, tx.faceNormal, tx.styleValue, (uint32_t)i * 9781u + 1u, rays, true, &gathered[(size_t)i * 4] };
		}
		gi_gather_many( bake, jobs, true );
	}

	// 1b. blur it, a lot, but never through walls
	gi_blur_grid( bake, gridPoints, texelPos, gathered, gi_bounceBlurRadius, 3 );

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
	{
		std::vector<giGatherJob> jobs;
		for ( size_t i = 0; i < n; i++ ) {
			if ( !lonely[i] )
				continue;
			const giTexel &tx = bake.texels[i];
			jobs.push_back( { tx.pos, tx.normal, tx.faceNormal, tx.styleValue, (uint32_t)i * 9781u + 1u, rays, true, &bake.indirect[i * 4] } );
		}
		gi_gather_many( bake, jobs, false );
	}
}

static bool gi_relit;	// direct light was traced with real shadows (no fake contact darkening needed)

// light = direct + bounce, with ambient occlusion keeping corners dark
/*
How much ambient occlusion darkens bounce light, from the occlusion ao (1 open .. 0
enclosed). The bounce gather already sees the geometry around a spot (a ray that
hits a wall brings that wall's light instead of the sky's), so occlusion on top of
it is only a gentle extra: at r_devoGI_ao 1, a spot half enclosed keeps 75% of its
bounce light. (It used to be ao squared, which left floors next to walls far too
dark - they are almost always near something.)
*/
static inline float gi_ao_factor( float ao ) {
	const float strength = Com_Clamp( 0.0f, 2.0f, g_set.ao ) * 0.5f;	// ao 1.6 (the default) = 80%
	return Q_max( 0.0f, 1.0f - strength * ( 1.0f - Com_Clamp( 0.0f, 1.0f, ao ) ) );
}

static void gi_compose( giBake &bake, float intensity ) {
	const float aoDirect = gi_relit ? 0.0f : Com_Clamp( 0.0f, 1.0f, g_set.aoDirect );
	bake.light = bake.direct;
	for ( size_t i = 0; i < bake.texels.size(); i++ ) {
		const giTexel &tx = bake.texels[i];
		float *px = &bake.light[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
		const float ao = Com_Clamp( 0.0f, 1.0f, bake.indirect[i * 4 + 3] );
		const float bounceScale = intensity * gi_ao_factor( ao );
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
	// as many pixels as one original lightmap texel is wide (8 at 8x): the GPU's texture
	// filtering never reaches past a polygon into black
	const int passes = Q_max( 2, DEVOGI_LM_SIZE / DEVOGI_BSP_LM_SIZE );
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
	Com_sprintf( buf, sizeof( buf ), "aa%g sh%g v%d ex%g r%d b%d i%g s%g ls%g e%d ao%g aod%g aor%g sat%g x%d rl%d gl%g lh%llx", g_set.aa, g_set.sheen, DEVOGI_VERSION, g_set.exposure,
		Com_Clampi( 8, 1024, g_set.rays ), Com_Clampi( 1, 16, g_set.bounces ) * 100 + Com_Clampi( 1, 32, g_set.liveBounces ),
		g_set.intensity, g_set.sky, g_set.liveSky, g_set.entityLights,
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
	std::vector<float> img( bake.owner.size() * 3, 0.0f );
	for ( size_t i = 0; i < bake.texels.size(); i++ ) {
		const giTexel &tx = bake.texels[i];
		const float ao = Com_Clamp( 0.0f, 1.0f, bake.indirect[i * 4 + 3] );
		float k = intensity * gi_ao_factor( ao );
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
static void gi_build_probes( giBake &bake, dgLiveLighting &live, float intensity, int rays,
	const giLightGrid *lightGrid, const std::vector<float> &groupScale, float exposure ) {
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
				float dx, dy;
				gi_sun_disc( jx, jy, dx, dy );
				const gvec3 d = gnorm( gi_sunDir + st * dx + sb * dy );
				giHit hit;
				if ( !gi_trace( scene, pos, d, 16384.0f, hit, false ) || scene.surfs[scene.tris[hit.tri].surf].sky )
					seen++;
			}
			sunVis[i] = (uint8_t)( seen * 255 / 16 );
		}
	}, true );

	// the bounce light from 6 directions of every probe in open space, all at once
	{
		std::vector<float> g( count * 6 * 4, 0.0f );
		std::vector<giGatherJob> jobs;
		for ( size_t i = 0; i < count; i++ ) {
			if ( !valid[i] )
				continue;
			const int x = (int)( i % dims[0] ), y = (int)( ( i / dims[0] ) % dims[1] ), z = (int)( i / ( (size_t)dims[0] * dims[1] ) );
			const gvec3 pos( mins.x + x * spacing, mins.y + y * spacing, mins.z + z * spacing );
			for ( int a = 0; a < 6; a++ )
				jobs.push_back( { pos, axes[a], axes[a], 0, (uint32_t)i * 2654435761u + (uint32_t)a * 97u, probeRays, false, &g[( i * 6 + a ) * 4] } );
		}
		gi_gather_many( bake, jobs, false );
		for ( size_t i = 0; i < count; i++ )
			for ( int a = 0; a < 6; a++ )
				for ( int k = 0; k < 3; k++ )
					cube[i * 18 + a * 3 + k] = g[( i * 6 + a ) * 4 + k] * intensity;
	}

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

	/*
	The direct light too (lamps and the sun, with their shadows), from the same 6
	directions: with it, the probes hold all the light at that spot, and models
	(players, weapons, items) can be lit exactly like the walls around them - instead
	of by the map's old light grid from q3map2.
	*/
	live.probesDirect.assign( count * 18, 0 );
	if ( lightGrid ) {
		std::vector<float> direct( count * 18, 0.0f );
		gi_parallel_for( (int)count, [&]( int i ) {
			if ( !live.probeValid[i] )
				return;
			const int x = (int)( i % dims[0] ), y = (int)( ( i / dims[0] ) % dims[1] ), z = (int)( i / ( (size_t)dims[0] * dims[1] ) );
			const gvec3 pos( mins.x + x * spacing, mins.y + y * spacing, mins.z + z * spacing );
			for ( int a = 0; a < 6; a++ ) {
				gvec3 total;
				gi_direct_point( bake, *lightGrid, pos, axes[a], axes[a], (uint32_t)i * 40503u + (uint32_t)a * 131u, 0.0f,
					[&]( int group, const gvec3 &rgb ) {
						total += rgb * ( group < (int)groupScale.size() ? groupScale[group] : 1.0f );
					} );
				direct[(size_t)i * 18 + a * 3 + 0] = total.x * exposure;
				direct[(size_t)i * 18 + a * 3 + 1] = total.y * exposure;
				direct[(size_t)i * 18 + a * 3 + 2] = total.z * exposure;
			}
		} );
		for ( size_t k = 0; k < count * 18; k++ )
			live.probesDirect[k] = gi_float_to_half( direct[k] );
	}

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

static void gi_build_live( giBake &bake, dgLiveLighting &live, const giLightGrid *lightGrid, bool relight, float exposure, float intensity,
	int rays, const std::vector<float> &groupScale ) {
	live = dgLiveLighting();
	live.relit = relight;
	gi_build_bounce_maps( bake, live, relight, exposure, intensity );
	if ( relight )
		gi_build_fixtures( live, exposure, groupScale );
	gi_build_probes( bake, live, intensity, rays, relight ? lightGrid : NULL, groupScale, exposure );
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
	const int bspPages = ll.filelen / srcPageBytes;
	int numPages = bspPages;		// grows if surfaces that share a lightmap spot get their own
	const byte *srcLightmaps = fileBase + ll.fileofs;
	std::vector<byte> movedSrc;	// the BSP's lightmaps with those surfaces' old light moved along

	const int pageSize = DEVOGI_BSP_LM_SIZE * scale;
	const int pageBytes = pageSize * pageSize * 3;

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

	// the open sky: its light from its sky box images (or one colour), see gi_sky_setup.
	// At r_devoGI_sky 0.5 a white sky pixel gives 128 - shade outdoors then gets roughly
	// a fifth of full sunlight, like a clear day.
	{
		gvec3 tint( 1, 1, 1 );
		const lump_t &l = header->lumps[LUMP_SHADERS];
		const dshader_t *in = (const dshader_t *)( fileBase + l.fileofs );
		const int count = l.filelen / sizeof( dshader_t );
		int skyShader = -1;
		for ( int i = 0; i < count; i++ ) {
			if ( !( LittleLong( in[i].surfaceFlags ) & SURF_SKY ) )
				continue;
			if ( skyShader < 0 )
				skyShader = i;
			if ( gi_sky_sun_color( in[i].shader, tint ) ) {
				skyShader = i;
				break;
			}
		}
		gi_sky = giSky();
		gi_sky.scale = 256.0f * Q_max( 0.0f, g_set.sky );
		if ( skyShader >= 0 )
			gi_sky_setup( in[skyShader].shader, tint );
		bake.skyColor = gi_sky.uniform * gi_sky.scale;
		dg_print( "DevoGI: sky light from %s\n", gi_sky.images ? "its sky box images" : "one colour" );

		// big unlit surfaces glow with their own texture (see DG_ExtractLights)
		int glowing = 0;
		for ( int i = 0; i < count && i < (int)gi_emitTexture.size(); i++ ) {
			if ( gi_emitTexture[i].empty() )
				continue;
			byte *pic = NULL;
			int w = 0, h = 0;
			dg_load_image( gi_emitTexture[i].c_str(), &pic, &w, &h );
			if ( !pic || w <= 0 || h <= 0 ) {
				if ( pic )
					dg_free_image( pic );
				continue;
			}
			giShader &sh = bake.scene.shaders[i];
			sh.emitW = Q_min( w, 64 );
			sh.emitH = Q_min( h, 64 );
			sh.emit.assign( (size_t)sh.emitW * sh.emitH, gvec3() );
			for ( int y = 0; y < sh.emitH; y++ ) {
				for ( int x = 0; x < sh.emitW; x++ ) {
					// average the block of pixels this cell covers
					const int x0 = x * w / sh.emitW, x1 = Q_max( x0 + 1, ( x + 1 ) * w / sh.emitW );
					const int y0 = y * h / sh.emitH, y1 = Q_max( y0 + 1, ( y + 1 ) * h / sh.emitH );
					double r = 0, g = 0, b = 0;
					int n = 0;
					for ( int yy = y0; yy < y1; yy++ )
						for ( int xx = x0; xx < x1; xx++, n++ ) {
							const byte *px = &pic[( (size_t)yy * w + xx ) * 4];
							r += px[0]; g += px[1]; b += px[2];
						}
					sh.emit[(size_t)y * sh.emitW + x] = gvec3( (float)( r / n ), (float)( g / n ), (float)( b / n ) ) * ( gi_sky.scale / 255.0f );
				}
			}
			dg_free_image( pic );
			glowing++;
		}
		if ( glowing )
			dg_print( "DevoGI: %d big unlit surfaces glow with their own texture\n", glowing );
	}

	COM_StripExtension( COM_SkipPath( (char *)mapName ), gi_progressMap, sizeof( gi_progressMap ) );
	gi_progressLastPrint = -100;
	gi_progress_stage( "Preparing geometry", 0.0f, 0.03f );

	gi_build_geometry( bake.scene, fileBase, header );
	gi_build_bvh( bake.scene );	// (the lightmap moves below don't change the tree)
	// (every surface keeps the same resolution: no extra detail for some, detail = none)
	numPages = gi_unshare_lightmaps( bake.scene, srcLightmaps, bspPages, std::vector<int>(),
		result.lightmapMoves, movedSrc );
	if ( numPages > bspPages ) {
		srcLightmaps = movedSrc.data();
		bake.numPages = numPages;
		dg_print( "DevoGI: %d surfaces shared a lightmap spot with another one, they get their own on %d new page%s\n",
			(int)result.lightmapMoves.size(), numPages - bspPages, numPages - bspPages > 1 ? "s" : "" );
	}
	const int outBytes = numPages * pageBytes;

	// the GPU tracer: the BVH and the triangles go to the GPU once, then the bounce
	// light (the bulk of the work) is traced there in big batches
	gi_gpuActive = false;
	if ( g_set.gpu ) {
		const giScene &sc = bake.scene;
		std::vector<float> nodes( sc.nodes.size() * 8 ), tris( sc.triIndex.size() * 12 );
		for ( size_t i = 0; i < sc.nodes.size(); i++ ) {
			const giBVHNode &nd = sc.nodes[i];
			float *o = &nodes[i * 8];
			int32_t left = nd.left, cnt = nd.count;
			o[0] = nd.mins.x; o[1] = nd.mins.y; o[2] = nd.mins.z; memcpy( &o[3], &left, 4 );
			o[4] = nd.maxs.x; o[5] = nd.maxs.y; o[6] = nd.maxs.z; memcpy( &o[7], &cnt, 4 );
		}
		for ( size_t k = 0; k < sc.triIndex.size(); k++ ) {
			const int32_t ti = sc.triIndex[k];
			const giTri &t = sc.tris[ti];
			const bool blocks = sc.surfs[t.surf].occluder || sc.surfs[t.surf].sky;
			float *o = &tris[k * 12];
			o[0] = t.v0.x; o[1] = t.v0.y; o[2] = t.v0.z; memcpy( &o[3], &ti, 4 );
			o[4] = t.e1.x; o[5] = t.e1.y; o[6] = t.e1.z; o[7] = blocks ? 1.0f : 0.0f;
			// which side of e1 x e2 is the front: lets the GPU spot back faces for free
			o[8] = t.e2.x; o[9] = t.e2.y; o[10] = t.e2.z; o[11] = gdot( gcross( t.e1, t.e2 ), t.n ) > 0 ? 1.0f : -1.0f;
		}
		gi_gpuActive = DG_GpuBegin( nodes.data(), sc.nodes.size(), tris.data(), sc.triIndex.size(),
			[]( const char *text ) { dg_print( "%s", text ); } );
	}
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
	// developer aid: DEVOGI_DEBUG_SURF=<surface number> prints that surface's sample
	// points (the first 12, or with DEVOGI_DEBUG_ROW=<y> those in that lightmap row)
	auto debugSurf = [&]( const char *when ) {
		const char *env = getenv( "DEVOGI_DEBUG_SURF" );
		if ( !env )
			return;
		const int want = atoi( env );
		const char *rowEnv = getenv( "DEVOGI_DEBUG_ROW" );
		const int row = rowEnv ? atoi( rowEnv ) : -1;
		int shown = 0;
		for ( size_t i = 0; i < bake.texels.size() && shown < ( row >= 0 ? 64 : 12 ); i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.surf != want || ( row >= 0 && tx.y != row ) )
				continue;
			const size_t px = ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x;
			dg_print( "DevoGI debug %s: surf %d texel %d,%d pos (%.1f %.1f %.1f) n (%.2f %.2f %.2f) face (%.2f %.2f %.2f) edge %d size %.2f light %.1f\n",
				when, want, tx.x, tx.y, tx.pos.x, tx.pos.y, tx.pos.z, tx.normal.x, tx.normal.y, tx.normal.z,
				tx.faceNormal.x, tx.faceNormal.y, tx.faceNormal.z, tx.edge ? 1 : 0, tx.size,
				bake.light.size() > px * 3 ? bake.light[px * 3] : -1.0f );
			if ( bake.indirect.size() > i * 4 && bake.direct.size() > px * 3 )
				dg_print( "    direct %.1f %.1f %.1f  bounce %.1f %.1f %.1f  open %.2f\n",
					bake.direct[px * 3], bake.direct[px * 3 + 1], bake.direct[px * 3 + 2],
					bake.indirect[i * 4], bake.indirect[i * 4 + 1], bake.indirect[i * 4 + 2], bake.indirect[i * 4 + 3] );
			shown++;
		}
		if ( !shown )
			dg_print( "DevoGI debug %s: surf %d has no texels\n", when, want );
	};
	debugSurf( "placed" );
	gi_fix_texel_positions( bake );
	debugSurf( "fixed" );

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
	std::vector<gvec3> sunDirect;		// the sun's share of each texel's direct light (calibrated)
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
		sunDirect.assign( n, gvec3() );
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
			sunDirect[i] = sun * groupScale[gi_sunGroup];
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
		gi_world_smooth( bake, texelHash, sunDirect, directRadius );

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
	// the wide blur of the bounce light: three passes of 32 units at 8x (about 55 units
	// in all), never less than 24 units per pass at low resolutions
	gi_bounceBlurRadius = Com_Clamp( 24.0f, 48.0f, bake.texelSize * stride * 4.0f );
	giSpatialHash gridHash;
	gridHash.build( texelPos, gridPoints, bounceRadius );

	/*
	Two bounce chains.
	  - The full bake (r_devoGI_live 0): direct light from the lamps and the sun,
	    bounced 'bounces' times.
	  - Live lighting (r_devoGI_live 1) draws the sun and the lamps itself and takes
	    only bounce light from the bake. There the sun must not be baked in at all,
	    not even its bounce: the live layer is light from the sky all around (with its
	    ambient occlusion) and the lamps' bounce, like a skylight. So a second chain
	    starts again from the direct light without the sun and bounces 'liveBounces'
	    times (24 by default), filling deep rooms and corridors a little more each time.
	The full bake's light is put aside after its own chain, so it stays exactly as it was.
	*/
	const int liveBounces = relight ? Com_Clampi( 1, 32, g_set.liveBounces ) : 0;
	const int totalBounces = bounces + liveBounces;
	std::vector<float> fullLight;	// the full bake's light, after its own last bounce
	const float fullSkyScale = gi_sky.scale;

	bake.light = bake.direct;
	static char bounceStage[64];
	const float bounceBase = 0.45f, bounceSpan = 0.47f;
	for ( int b = 0; b < totalBounces; b++ ) {
		if ( b == bounces ) {
			// the full bake is done: keep its light, then start the live chain without the sun
			fullLight = bake.light;
			for ( size_t i = 0; i < bake.texels.size() && i < sunDirect.size(); i++ ) {
				const giTexel &tx = bake.texels[i];
				if ( tx.styleValue != 0 )
					continue;
				float *px = &bake.direct[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
				px[0] = Q_max( 0.0f, px[0] - sunDirect[i].x );
				px[1] = Q_max( 0.0f, px[1] - sunDirect[i].y );
				px[2] = Q_max( 0.0f, px[2] - sunDirect[i].z );
			}
			bake.light = bake.direct;
			// ...and with more sky: it is what lights the shade there
			gi_sky.scale *= Q_max( 0.0f, g_set.liveSky );
		}
		const int chainBounce = b < bounces ? b : b - bounces;
		Com_sprintf( bounceStage, sizeof( bounceStage ), b < bounces ? "Bounce light %d of %d" : "Live bounce light %d of %d",
			chainBounce + 1, b < bounces ? bounces : liveBounces );
		gi_progress_stage( bounceStage, bounceBase + bounceSpan * b / totalBounces, bounceSpan / totalBounces );
		// later bounces carry less and less energy, so fewer rays are enough for them
		const int bounceRays = chainBounce == 0 ? rays : ( chainBounce < 3 ? rays / 2 : rays / 4 );
		gi_gather( bake, gridHash, gridPoints, texelPos, bounceRadius, Q_max( 16, bounceRays ) );
		gi_compose( bake, intensity );
		dg_print( "DevoGI: %s bounce %d/%d done (%.1fs)\n", b < bounces ? "full" : "live",
			chainBounce + 1, b < bounces ? bounces : liveBounces, ( dg_ms() - startTime ) / 1000.0f );
	}
	// the full bake goes on with its own light; the bounce-only light for live lighting
	// (bake.indirect, see gi_build_bounce_maps) is the live chain's, without the sun
	if ( !fullLight.empty() ) {
		bake.light.swap( fullLight );
		fullLight.clear();
		fullLight.shrink_to_fit();
		gi_sky.scale = fullSkyScale;	// back to the full bake's sky (models, probes)
	}
	sunDirect.clear();
	sunDirect.shrink_to_fit();

	// buried texels (a floor running on under a wall) take the light of their nearest
	// visible neighbour, in the full bake and in the live bounce light
	{
		const std::vector<int> buriedSrc = gi_buried_sources( bake );
		int copied = 0;
		for ( size_t i = 0; i < bake.texels.size(); i++ ) {
			const int o = buriedSrc[i];
			if ( o < 0 )
				continue;
			const giTexel &tx = bake.texels[i], &ot = bake.texels[o];
			const size_t a = ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x;
			const size_t c = ( (size_t)ot.page * DEVOGI_LM_SIZE + ot.y ) * DEVOGI_LM_SIZE + ot.x;
			for ( int k = 0; k < 3; k++ )
				bake.light[a * 3 + k] = bake.light[c * 3 + k];
			for ( int k = 0; k < 4; k++ )
				bake.indirect[i * 4 + k] = bake.indirect[(size_t)o * 4 + k];
			if ( !bake.sunVis.empty() )
				bake.sunVis[i] = bake.sunVis[o];
			copied++;
		}
		dg_print( "DevoGI: %d texels under walls take their light from the visible side\n", copied );
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
	debugSurf( "lit" );

	// ---- vertex colours: models and rgbGen vertex stages ----
	{
		const lump_t &lv = header->lumps[LUMP_DRAWVERTS];
		const lump_t &ls = header->lumps[LUMP_SURFACES];
		const mapVert_t *dv = (const mapVert_t *)( fileBase + lv.fileofs );
		const dsurface_t *ds = (const dsurface_t *)( fileBase + ls.fileofs );
		const int numSurfs = ls.filelen / sizeof( dsurface_t );

		std::vector<int> list;						// vertices of vertex-lit models: traced
		std::vector<int> listSurf;					// ...and the surface each one belongs to
		std::vector<std::pair<int, int>> lmList;	// (vertex, surface) of lightmapped surfaces: sampled
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
					if ( vertexLit ) {
						list.push_back( v );
						listSurf.push_back( s );
					} else {
						lmList.push_back( std::make_pair( v, s ) );
					}
				}
			}
		}

		if ( !list.empty() || !lmList.empty() ) {
			gi_vertexColors.resize( (size_t)numDrawVerts * 4 );
			for ( int v = 0; v < numDrawVerts; v++ )
				memcpy( &gi_vertexColors[(size_t)v * 4], dv[v].color[0], 4 );

			// where each surface's light is now: surfaces that shared a lightmap spot moved
			std::vector<int> moveOf( numSurfs, -1 );
			for ( size_t m = 0; m < result.lightmapMoves.size(); m++ )
				moveOf[result.lightmapMoves[m].surf] = (int)m;

			// lightmapped surfaces: read the new lightmap at each vertex
			gi_parallel_for( (int)lmList.size(), [&]( int i ) {
				const int v = lmList[i].first, surf = lmList[i].second;
				int page = LittleLong( ds[surf].lightmapNum[0] );
				float u = LittleFloat( dv[v].lightmap[0][0] ), w = LittleFloat( dv[v].lightmap[0][1] );
				if ( moveOf[surf] >= 0 ) {
					const dgLightmapMove &m = result.lightmapMoves[moveOf[surf]];
					page = m.page;
					u = u * m.scale + m.dx / (float)DEVOGI_BSP_LM_SIZE;
					w = w * m.scale + m.dy / (float)DEVOGI_BSP_LM_SIZE;
				}
				const float fx = Com_Clamp( 0.0f, (float)( DEVOGI_LM_SIZE - 1 ), u * DEVOGI_LM_SIZE - 0.5f );
				const float fy = Com_Clamp( 0.0f, (float)( DEVOGI_LM_SIZE - 1 ), w * DEVOGI_LM_SIZE - 0.5f );
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
			const float aoDirect = Com_Clamp( 0.0f, 1.0f, g_set.aoDirect );
			gi_progress_stage( "Models and vertex colours", 0.92f, 0.04f );
			// where each vertex samples its light: like lightmap texels, a vertex sunk into
			// a wall or the floor (models are often pushed into them) first steps out of it
			// and away from it, or it would see only the inside of that wall and come out dark
			std::vector<gvec3> samplePos( list.size() );
			gi_parallel_for( (int)list.size(), [&]( int i ) {
				giVert gv;
				gi_load_vert( gv, dv[list[i]] );
				samplePos[i] = gv.xyz;
				gi_fix_point( bake.scene, samplePos[i], gv.normal, 8.0f, listSurf[i] );
			} );

			// bounce light at every vertex first, all at once (four times the rays: a vertex
			// has no neighbours to average with)
			std::vector<float> gathered( list.size() * 4, 0.0f );
			{
				std::vector<giGatherJob> jobs( list.size() );
				for ( size_t i = 0; i < list.size(); i++ ) {
					giVert gv;
					gi_load_vert( gv, dv[list[i]] );
					jobs[i] = { samplePos[i], gv.normal, gv.normal, 0, (uint32_t)list[i] * 7919u + 13u, rays * 4, true, &gathered[i * 4] };
				}
				gi_gather_many( bake, jobs, false );
			}
			gi_parallel_for( (int)list.size(), [&]( int i ) {
				const int v = list[i];
				giVert gv;
				gi_load_vert( gv, dv[v] );
				const float *g = &gathered[(size_t)i * 4];
				const float ao = Com_Clamp( 0.0f, 1.0f, g[3] );
				const float bounceScale = intensity * gi_ao_factor( ao );
				float c[3];
				if ( relight ) {
					gvec3 total;
					gi_direct_point( bake, lightGrid, samplePos[i], gv.normal, gv.normal, (uint32_t)v * 40503u + 7u, 0.0f,
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
	gi_build_live( bake, result.live, &lightGrid, relight, exposure, intensity, rays, groupScale );

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

	DG_GpuEnd();
	gi_gpuActive = false;
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
	bool	unlit;		// no lightmap and no vertex light: it shows its texture as it is
	char	texture[MAX_QPATH];	// its main texture
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
	ls.unlit = false;
	ls.texture[0] = 0;
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
		/*
		An unlit surface - no lightmap and no vertex light, so it shows its texture as it
		is - is treated as a light: a fake sky cube, a lamp the designers simply left
		unlit so it pops out. The lighter the texture, the brighter. (Very dark ones are
		left alone.) Small ones become lamps with soft shadows; big ones glow through
		the bounce light instead (see DG_ExtractLights).
		*/
		if ( !hasLightmapStage && !litStage && !firstStageBlended && firstMap[0]
			&& gi_image_color( firstMap, c, &brightness ) && brightness > 0.05f ) {
			ls.value = g_set.glow * brightness;
			ls.subdivide = 32.0f;
			ls.color = c;
			ls.unlit = true;
			Q_strncpyz( ls.texture, firstMap, sizeof( ls.texture ) );
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
		if ( depth == 1 && ( !Q_stricmp( token, "q3map_sun" ) || !Q_stricmp( token, "q3map_sunExt" ) || !Q_stricmp( token, "sun" )
			|| !Q_stricmp( token, "q3gl2_sun" ) ) ) {
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
	gi_lightsHash = 0;
	if ( !fileBase )
		return;		// no map: no lights (DevoGI is off for it)
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
	gi_emitTexture.assign( Q_max( 1, numShaders ), std::string() );
	if ( numEmitting ) {
		gi_build_geometry( scene, fileBase, header );

		// big unlit surfaces (a whole fake sky box, a large glowing panel) would become
		// thousands of little lamps: instead they glow through the bounce light, pixel
		// by pixel, like the real sky does
		{
			std::vector<float> surfArea( scene.surfs.size(), 0.0f );
			for ( const giTri &t : scene.tris )
				surfArea[t.surf] += 0.5f * sqrtf( gdot( gcross( t.e1, t.e2 ), gcross( t.e1, t.e2 ) ) );
			for ( size_t si = 0; si < scene.surfs.size(); si++ ) {
				const int sh = scene.surfs[si].shader;
				if ( sh < 0 || sh >= numShaders || !emits[sh] || !lightShaders[sh].unlit )
					continue;
				if ( surfArea[si] > 256.0f * 256.0f ) {
					gi_emitTexture[sh] = lightShaders[sh].texture;
					emits[sh] = 0;
					numEmitting--;
				}
			}
		}

		struct accum_t { gvec3 c, n; float area; int shader; gvec3 mins, maxs; };
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
				patches.push_back( { gvec3(), gvec3(), 0.0f, sh, gvec3( 1e30f, 1e30f, 1e30f ), gvec3( -1e30f, -1e30f, -1e30f ) } );
				table[slot] = idx;
				tableKeys[slot] = key;
			}
			accum_t &a = patches[idx];
			a.c += c * area;
			a.n += t.n * area;
			a.area += area;
			const gvec3 corners[3] = { t.v0, t.v0 + t.e1, t.v0 + t.e2 };
			for ( const gvec3 &v : corners ) {
				a.mins = gmin( a.mins, v );
				a.maxs = gmax( a.maxs, v );
			}
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

			// its real rectangle: of the world axes lying in the lamp's plane, the one it is
			// longest along is the rectangle's first edge (maps are mostly built on the grid)
			{
				const gvec3 n = lt.normal;
				gvec3 corner[8];
				for ( int k = 0; k < 8; k++ )
					corner[k] = gvec3( ( k & 1 ) ? a.maxs.x : a.mins.x, ( k & 2 ) ? a.maxs.y : a.mins.y, ( k & 4 ) ? a.maxs.z : a.mins.z );
				auto extent = [&]( const gvec3 &axis, float &lo, float &hi ) {
					lo = 1e30f; hi = -1e30f;
					for ( const gvec3 &c : corner ) {
						lo = Q_min( lo, gdot( c, axis ) );
						hi = Q_max( hi, gdot( c, axis ) );
					}
				};
				const gvec3 world[3] = { gvec3( 1, 0, 0 ), gvec3( 0, 1, 0 ), gvec3( 0, 0, 1 ) };
				float best = -1;
				for ( const gvec3 &e : world ) {
					const gvec3 p = e - n * gdot( e, n );
					if ( gdot( p, p ) < 0.1f )
						continue;	// (nearly) along the normal
					const gvec3 u = gnorm( p );
					float lo, hi;
					extent( u, lo, hi );
					if ( hi - lo > best ) {
						best = hi - lo;
						lt.axisU = u;
					}
				}
				if ( best > 0 ) {
					lt.axisV = gnorm( gcross( n, lt.axisU ) );
					float ulo, uhi, vlo, vhi;
					extent( lt.axisU, ulo, uhi );
					extent( lt.axisV, vlo, vhi );
					const gvec3 centroid = a.c * ( 1.0f / a.area );
					// the rectangle's middle, on the glowing surface
					lt.rectCentre = centroid + lt.axisU * ( ( ulo + uhi ) * 0.5f - gdot( centroid, lt.axisU ) )
						+ lt.axisV * ( ( vlo + vhi ) * 0.5f - gdot( centroid, lt.axisV ) );
					lt.halfU = Q_max( 0.25f, ( uhi - ulo ) * 0.5f );
					lt.halfV = Q_max( 0.25f, ( vhi - vlo ) * 0.5f );
				}
			}
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

// r_devoGI_scale (1 to 16), within what the GPU can hold (maxScale)
int DG_ChooseScale( const dgSettings &settings, int maxScale )
{
	// any whole number from 1 to 16: the same resolution increase for every surface,
	// as long as one page still fits in a GPU texture (128 * scale <= its size limit)
	return Com_Clampi( 1, Q_max( 1, Q_min( 16, maxScale ) ), settings.scale );
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
          per probe (bounce light), one valid byte per probe, one sun-visibility
          byte per probe, then 18 half floats per probe (direct light)
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
	gi_put( file, live.probesDirect.data(), live.probesDirect.size() * 2 );
	gi_section_end( file, s );

	s = gi_section_begin( file, "LITE" );
	gi_put_i32( file, (int32_t)live.fixtures.size() );
	gi_put( file, live.fixtures.data(), live.fixtures.size() * sizeof( dgFixture ) );
	gi_put_i32( file, live.hasSun ? 1 : 0 );
	gi_put( file, live.sunDir, sizeof( live.sunDir ) );
	gi_put( file, live.sunRgb, sizeof( live.sunRgb ) );
	gi_section_end( file, s );

	// surfaces that got their own lightmap spot (see dgLightmapMove)
	s = gi_section_begin( file, "MOVE" );
	gi_put_i32( file, (int32_t)result.lightmapMoves.size() );
	gi_put( file, result.lightmapMoves.data(), result.lightmapMoves.size() * sizeof( dgLightmapMove ) );
	gi_section_end( file, s );
	return file;
}

// reads the tagged sections (unknown ones are skipped); false if one is damaged
static bool gi_read_sections( const byte *p, const byte *end, dgResult &result ) {
	dgLiveLighting &live = result.live;
	live = dgLiveLighting();
	result.lightmapMoves.clear();
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
			if ( (size_t)( qend - q ) != count * 18 * 2 * 2 + count * 2 )
				return false;
			live.probes.resize( count * 18 );
			get( live.probes.data(), count * 36 );
			live.probeValid.assign( q, q + count );
			live.probeSun.assign( q + count, q + count * 2 );
			q += count * 2;
			live.probesDirect.resize( count * 18 );
			get( live.probesDirect.data(), count * 36 );
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
		else if ( !memcmp( tag, "MOVE", 4 ) ) {
			int32_t n = 0;
			if ( !get( &n, 4 ) || n < 0 || (size_t)( qend - q ) != (size_t)n * sizeof( dgLightmapMove ) )
				return false;
			result.lightmapMoves.resize( n );
			get( result.lightmapMoves.data(), (size_t)n * sizeof( dgLightmapMove ) );
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
	const int numSurfs = header->lumps[LUMP_SURFACES].filelen / (int)sizeof( dsurface_t );
	const int bspPages = header->lumps[LUMP_LIGHTMAPS].filelen / ( DEVOGI_BSP_LM_SIZE * DEVOGI_BSP_LM_SIZE * 3 );
	const int pageSize = DEVOGI_BSP_LM_SIZE * scale;
	const size_t pageBytes = (size_t)pageSize * pageSize * 3;
	// the BSP's own pages, plus any new ones for surfaces that got their own spot
	const size_t outBytes = h.lightmapBytes > 0 ? (size_t)h.lightmapBytes : 0;
	const int numPages = (int)( outBytes / pageBytes );

	if ( memcmp( h.magic, "DVGI", 4 ) || h.version != DEVOGI_VERSION
		|| h.bspHash != bspHash || h.settingsHash != settingsHash
		|| outBytes % pageBytes != 0 || numPages < bspPages || h.pageSize != pageSize
		|| ( h.numVertexColors != 0 && h.numVertexColors != numDrawVerts )
		|| length < sizeof( devogiHeader_t ) + outBytes + (size_t)h.numVertexColors * 4 )
		return false;

	const byte *p = data + sizeof( devogiHeader_t );
	const byte *sections = p + outBytes + (size_t)h.numVertexColors * 4;
	if ( !gi_read_sections( sections, data + length, result ) )
		return false;
	for ( const dgLightmapMove &m : result.lightmapMoves )
		if ( m.surf < 0 || m.surf >= numSurfs || m.page < bspPages || m.page >= numPages || m.scale < 1 || m.scale > 8 )
			return false;	// damaged
	if ( result.lightmapMoves.empty() && numPages != bspPages )
		return false;
	result.pageSize = pageSize;
	result.lightmaps.assign( p, p + outBytes );
	result.vertexColors.assign( p + outBytes, sections );
	return true;
}

/*
Smooth shading for the renderer: the normals gi_smooth_normals gives faceted
round walls in the bake, written into a BSP loaded in memory, so per-pixel
light drawn live (the sun, lamps) runs smoothly across them too, instead of
showing every flat face of the wall as its own stripe.
*/
void DG_SmoothMapNormals( byte *bsp )
{
	const dheader_t *header = (const dheader_t *)bsp;
	giScene *scene = new giScene();
	const lump_t &l = header->lumps[LUMP_SHADERS];
	const dshader_t *sh = (const dshader_t *)( bsp + l.fileofs );
	const int count = l.filelen / (int)sizeof( dshader_t );
	scene->shaders.resize( Q_max( 1, count ) );
	for ( int i = 0; i < count; i++ ) {	// only which surfaces are sky or invisible matters here
		scene->shaders[i].surfaceFlags = LittleLong( sh[i].surfaceFlags );
		scene->shaders[i].contentFlags = LittleLong( sh[i].contentFlags );
	}
	gi_build_geometry( *scene, bsp, header );

	const lump_t &lv = header->lumps[LUMP_DRAWVERTS];
	mapVert_t *dv = (mapVert_t *)( bsp + lv.fileofs );
	const int numVerts = lv.filelen / (int)sizeof( mapVert_t );
	for ( const giVert &gv : scene->verts ) {
		if ( gv.bspVert < 0 || gv.bspVert >= numVerts )
			continue;
		dv[gv.bspVert].normal[0] = LittleFloat( gv.normal.x );
		dv[gv.bspVert].normal[1] = LittleFloat( gv.normal.y );
		dv[gv.bspVert].normal[2] = LittleFloat( gv.normal.z );
	}
	delete scene;
}

int DG_NumPages( const dgResult &result )
{
	const size_t pageBytes = (size_t)result.pageSize * result.pageSize * 3;
	return pageBytes ? (int)( result.lightmaps.size() / pageBytes ) : 0;
}

// moves each surface's lightmap coordinates (style slot 0) to its new spot and points it at its new page
void DG_ApplyLightmapMoves( byte *bsp, const std::vector<dgLightmapMove> &moves )
{
	const dheader_t *header = (const dheader_t *)bsp;
	const lump_t &ls = header->lumps[LUMP_SURFACES];
	const lump_t &lv = header->lumps[LUMP_DRAWVERTS];
	dsurface_t *surfs = (dsurface_t *)( bsp + ls.fileofs );
	mapVert_t *verts = (mapVert_t *)( bsp + lv.fileofs );
	const int numSurfs = ls.filelen / (int)sizeof( dsurface_t );
	const int numVerts = lv.filelen / (int)sizeof( mapVert_t );
	for ( const dgLightmapMove &m : moves ) {
		if ( m.surf < 0 || m.surf >= numSurfs )
			continue;
		dsurface_t &s = surfs[m.surf];
		s.lightmapNum[0] = LittleLong( m.page );
		const int first = LittleLong( s.firstVert ), nv = LittleLong( s.numVerts );
		for ( int v = first; v < first + nv; v++ ) {
			if ( v < 0 || v >= numVerts )
				continue;
			const float scale = (float)Q_max( 1, m.scale );
			verts[v].lightmap[0][0] = LittleFloat( LittleFloat( verts[v].lightmap[0][0] ) * scale + m.dx / (float)DEVOGI_BSP_LM_SIZE );
			verts[v].lightmap[0][1] = LittleFloat( LittleFloat( verts[v].lightmap[0][1] ) * scale + m.dy / (float)DEVOGI_BSP_LM_SIZE );
		}
	}
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
