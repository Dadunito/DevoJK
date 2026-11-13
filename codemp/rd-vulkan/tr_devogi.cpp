/*
===========================================================================
DevoJK baked global illumination (DevoGI)

While a level loads, the renderer bakes bounced light (global illumination)
into the map's lightmaps and caches the result next to the map as
maps/<name>.devogi. The next time the same BSP is loaded the cache is used
directly; if the BSP changed (its lumps hash differently) or the bake
settings changed, the bake runs again.

Light sources:
  - the BSP lightmaps and vertex colours (all static light q3map2 baked:
    light entities, sun, surface lights), per light style, so switchable
    and flickering styles get their own bounce light
  - "light" entities left in the BSP entity lump (static, style 0), traced
    with shadow rays and merged into the direct light
  - the sky, as a soft ambient term (r_devoGI_sky)

The result overrides the lightmap data before it is uploaded, so the rest
of the renderer (and every shader stage that uses $lightmap) picks it up
with no runtime cost.

Ideas inspired by Berk Emre Saribas' Panko-Renderer (MIT) and Q3Map2's
radiosity; implementation written for the Q3 BSP format.

This file is part of DevoJK, licensed under GPLv2.
===========================================================================
*/

#include "tr_local.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#define DEVOGI_VERSION		8
#define DEVOGI_BSP_LM_SIZE	128			// lightmap page size stored in the BSP
#define DEVOGI_LM_SIZE		gi_lmSize	// page size being baked (BSP size * r_devoGI_scale)

static int gi_lmSize = DEVOGI_BSP_LM_SIZE;

// new style-0 vertex colours (rgba per draw vert) for vertex-lit surfaces, applied by
// R_DevoGI_PatchVertexColors right before the renderer loads the map surfaces
static std::vector<byte> gi_vertexColors;
#define DEVOGI_PATCH_STEPS	16
#define DEVOGI_RAY_TMIN		1.5f	// ignore hits closer than this: tessellated curves would self-occlude
#define DEVOGI_INF			1e30f

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
static cvar_t *r_devoLights;
static cvar_t *r_devoLights_max;
static cvar_t *r_devoLights_intensity;
static cvar_t *r_devoLights_range;

void R_DevoGI_Register( void )
{
	r_devoGI				= ri.Cvar_Get( "r_devoGI",				"1",	CVAR_ARCHIVE | CVAR_LATCH, "Baked global illumination: bounce light is baked into the lightmaps on level load and cached in maps/<name>.devogi\n 0: off\n 1: on" );
	r_devoGI_rays			= ri.Cvar_Get( "r_devoGI_rays",			"128",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI rays per lightmap texel and bounce (8-1024). More is smoother but bakes slower" );
	r_devoGI_bounces		= ri.Cvar_Get( "r_devoGI_bounces",		"4",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI light bounces (1-8)" );
	r_devoGI_intensity		= ri.Cvar_Get( "r_devoGI_intensity",	"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI bounce light strength" );
	r_devoGI_sky			= ri.Cvar_Get( "r_devoGI_sky",			"0.5",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient light from visible sky (0 = none)" );
	r_devoGI_entityLights	= ri.Cvar_Get( "r_devoGI_entityLights",	"1",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI uses light entities kept in the BSP as extra light sources" );
	r_devoGI_threads		= ri.Cvar_Get( "r_devoGI_threads",		"0",	CVAR_ARCHIVE, "DevoGI bake threads (0 = all CPU cores)" );
	r_devoGI_rebuild		= ri.Cvar_Get( "r_devoGI_rebuild",		"0",	CVAR_TEMP, "Set to 1 to force DevoGI to rebake the next map that loads" );
	r_devoGI_ao				= ri.Cvar_Get( "r_devoGI_ao",			"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient occlusion on the bounce light: keeps corners and crevices dark (0 = off, 1 = full, up to 2)" );
	r_devoGI_aoDirect		= ri.Cvar_Get( "r_devoGI_aoDirect",		"0.15",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient occlusion on the original lightmap light, for contact shadows (0 = off, 1 = full)" );
	r_devoGI_aoDistance		= ri.Cvar_Get( "r_devoGI_aoDistance",	"64",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI ambient occlusion range in game units" );
	r_devoGI_relight		= ri.Cvar_Get( "r_devoGI_relight",		"1",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI: recompute the direct light from the map's own light sources at high resolution with soft shadows, replacing the old lightmap (only its brightness is kept)\n 0: keep the old lightmap's direct light" );
	r_devoGI_exposure		= ri.Cvar_Get( "r_devoGI_exposure",		"1.0",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI: overall brightness of relit maps (1 = match the original map's brightness)" );
	r_devoLights			= ri.Cvar_Get( "r_devoLights",			"0",	CVAR_ARCHIVE, "DevoLights: light the map in real time from its own light sources (surface lights, sun, light entities)\n 0: off\n 1: preview through the dynamic light path (no shadows yet)" );
	r_devoLights_max		= ri.Cvar_Get( "r_devoLights_max",		"48",	CVAR_ARCHIVE, "DevoLights: most map lights used per frame (the nearest and brightest visible ones)" );
	r_devoLights_intensity	= ri.Cvar_Get( "r_devoLights_intensity","1.0",	CVAR_ARCHIVE, "DevoLights: brightness of map lights" );
	r_devoLights_range		= ri.Cvar_Get( "r_devoLights_range",	"1.0",	CVAR_ARCHIVE, "DevoLights: reach of map lights" );
	r_devoGI_scale			= ri.Cvar_Get( "r_devoGI_scale",		"4",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI lightmap resolution multiplier (1, 2, 4 or 8). Higher gives sharper shadows and corners but bakes slower and uses more video memory" );
	r_devoGI_saturation		= ri.Cvar_Get( "r_devoGI_saturation",	"1.6",	CVAR_ARCHIVE | CVAR_LATCH, "DevoGI colour bleeding strength: saturation of light reflected off surfaces (1 = texture average colour)" );
}

/*
===========================================================================
Small math helpers
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

// stable per-texel random numbers, so a rebake gives the same result
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

static void gi_basis( const gvec3 &n, gvec3 &t, gvec3 &b ) {
	if ( fabsf( n.z ) < 0.999f )
		t = gnorm( gcross( gvec3( 0, 0, 1 ), n ) );
	else
		t = gnorm( gcross( gvec3( 1, 0, 0 ), n ) );
	b = gcross( n, t );
}

/*
===========================================================================
DevoLights data (filled by R_DevoLights_Extract, used by the bake and the
per-frame dynamic lights)
===========================================================================
*/

struct devoLight_t {
	gvec3	origin;
	gvec3	normal;		// facing of the emitting surface (zero for omni lights)
	gvec3	color;		// normalized, max component 1
	float	photons;
	float	radius;		// where it falls off below ~1/255
	float	size;		// edge length of the emitting patch (0 for point lights)
	int		shader;		// emitting shader, -1 for entity lights
};

static std::vector<devoLight_t>	gi_lights;
static bool						gi_hasSun;
static gvec3					gi_sunDir;		// towards the sun
static gvec3					gi_sunColor;
static float					gi_sunIntensity;
static char						gi_lightsMap[MAX_QPATH];
static uint64_t					gi_lightsHash;	// changes when the light sources change

/*
===========================================================================
Geometry
===========================================================================
*/

struct giVert {
	gvec3	xyz;
	gvec3	normal;
	float	lm[MAXLIGHTMAPS][2];
	float	color[MAXLIGHTMAPS][3];
};

struct giTri {
	gvec3	v0, e1, e2;		// for ray intersection
	gvec3	n;				// geometric normal
	int		surf;
	int		vert[3];		// into giScene::verts
};

struct giSurf {
	int		shader;
	int		lightmapNum[MAXLIGHTMAPS];
	int		lightmapStyle[MAXLIGHTMAPS];
	bool	occluder;
	bool	sky;
};

struct giShader {
	int		surfaceFlags;
	int		contentFlags;
	gvec3	albedo;
};

struct giBVHNode {
	gvec3	mins, maxs;
	int		left;		// child index, or first tri for a leaf
	int		count;		// 0 for inner nodes
};

struct giScene {
	std::vector<giVert>		verts;
	std::vector<giTri>		tris;
	std::vector<giSurf>		surfs;
	std::vector<giShader>	shaders;
	std::vector<giBVHNode>	nodes;
	std::vector<int>		triIndex;
};

// biquadratic patch evaluation for one 3x3 control block
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
		return;	// degenerate
	t.n = gnorm( n );
	// Q3 faces are wound clockwise, so the cross product points into the surface:
	// orient the face normal to agree with the BSP's own vertex normals
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
		gs.occluder = !( sh.surfaceFlags & SURF_NODRAW )
			&& !( sh.contentFlags & ( CONTENTS_WATER | CONTENTS_FOG | CONTENTS_TRANSLUCENT | CONTENTS_LAVA | CONTENTS_SLIME ) );

		const int type = LittleLong( s.surfaceType );
		const int firstVert = LittleLong( s.firstVert );
		const int nv = LittleLong( s.numVerts );
		if ( firstVert < 0 || nv <= 0 || firstVert + nv > numVerts )
			continue;

		if ( type == MST_PLANAR || type == MST_TRIANGLE_SOUP ) {
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
							// orient like the patch normals
							gvec3 n = gcross( scene.verts[v10].xyz - scene.verts[v00].xyz, scene.verts[v01].xyz - scene.verts[v00].xyz );
							if ( gdot( n, scene.verts[v00].normal ) >= 0 ) {
								gi_add_tri( scene, i, v00, v10, v11 );
								gi_add_tri( scene, i, v00, v11, v01 );
							} else {
								gi_add_tri( scene, i, v00, v11, v10 );
								gi_add_tri( scene, i, v00, v01, v11 );
							}
						}
					}
				}
			}
		}
		// flares and bad surfaces have no geometry
	}
}

/*
===========================================================================
BVH ray tracer
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
	float	t;
	int		tri;
	float	u, v;
};

// nearest occluder hit along the ray; anyHit stops at the first one (shadow rays)
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
				// Moller-Trumbore, two sided
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
Shaders: albedo from the diffuse texture
===========================================================================
*/

const char *R_DevoGI_FindShaderText( const char *shaderName );

// first non-lightmap "map"/"clampmap" stage image of a shader, or the shader name itself
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

static gvec3 gi_shader_albedo( const char *shaderName ) {
	char texName[MAX_QPATH];
	gi_shader_texture( shaderName, texName, sizeof( texName ) );
	COM_StripExtension( texName, texName, sizeof( texName ) );

	byte *pic = NULL;
	int w = 0, h = 0;
	R_LoadImage( texName, &pic, &w, &h );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic )
			Z_Free( pic );
		return gvec3( 0.5f, 0.5f, 0.5f );
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
	Z_Free( pic );
	if ( a < 1 )
		return gvec3( 0.5f, 0.5f, 0.5f );

	gvec3 c( (float)( r / a / 255.0 ), (float)( g / a / 255.0 ), (float)( b / a / 255.0 ) );
	// keep energy in check like q3map2 radiosity: albedo never above 0.9
	float m = Q_max( c.x, Q_max( c.y, c.z ) );
	if ( m > 0.9f )
		c = c * ( 0.9f / m );
	return c;
}

// normalized sun colour of a sky shader, from its q3map_sun / q3map_sunExt / sun line
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
===========================================================================
*/

struct giPointLight {
	gvec3	origin;
	gvec3	color;		// already scaled by intensity
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
		// switchable or styled lights only belong to their own light style;
		// they are not added to the always-on (style 0) direct light
		if ( style != 0 || hasTarget || intensity <= 0 )
			continue;

		float m = Q_max( color[0], Q_max( color[1], color[2] ) );
		if ( m > 1.0f ) { color[0] /= m; color[1] /= m; color[2] /= m; }

		giPointLight l;
		l.origin = gvec3( origin );
		l.linear = ( spawnflags & 1 ) != 0;
		// q3map2 point light: photons = intensity * 7500, falloff 1/d^2
		const float photons = intensity * ( l.linear ? 1.0f : 7500.0f );
		l.color = gvec3( color ) * photons;
		l.radius = l.linear ? intensity * 8.0f : sqrtf( photons );	// where it falls under ~1/255
		lights.push_back( l );
	}
}

/*
===========================================================================
Lightmap texels
===========================================================================
*/

struct giTexel {
	gvec3	pos;
	gvec3	normal;		// smoothed shading normal
	gvec3	faceNormal;	// normal of the triangle it lies on (for ray offsets)
	bool	edge;		// centre lies just outside its triangle (lightmap border)
	int		page;
	int		x, y;
	int		style;		// lightmap style slot of the owning surface (0..3)
	int		styleValue;	// light style number in that slot (0 = always on)
	int		surf;
};

struct giBake {
	giScene						scene;
	std::vector<giTexel>		texels;
	std::vector<int>			owner;		// per page pixel: texel index or -1
	std::vector<int>			bspSurf;	// per original 128x128 page pixel: owning surface or -1
												// (-1 also for border texels buried in a wall: q3map2 left them dark)
	std::vector<float>			direct;		// per page pixel rgb
	std::vector<float>			light;		// current total light, per page pixel rgb
	std::vector<float>			indirect;	// per texel rgb + ambient occlusion (4 floats)
	int							numPages;
	gvec3						skyColor;
};

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

	int x0 = Q_max( 0, (int)floorf( Q_min( px[0], Q_min( px[1], px[2] ) ) - 1 ) );
	int y0 = Q_max( 0, (int)floorf( Q_min( py[0], Q_min( py[1], py[2] ) ) - 1 ) );
	int x1 = Q_min( DEVOGI_LM_SIZE - 1, (int)ceilf( Q_max( px[0], Q_max( px[1], px[2] ) ) + 1 ) );
	int y1 = Q_min( DEVOGI_LM_SIZE - 1, (int)ceilf( Q_max( py[0], Q_max( py[1], py[2] ) ) + 1 ) );

	// a little slack (in barycentric units per pixel) so edge texels get covered
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
				continue;	// already covered; only replace with a texel that is truly inside
			w0 = Q_max( 0.0f, w0 ); w1 = Q_max( 0.0f, w1 ); w2 = Q_max( 0.0f, w2 );
			const float ws = w0 + w1 + w2;
			w0 /= ws; w1 /= ws; w2 /= ws;

			giTexel tx;
			tx.pos = v[0]->xyz * w0 + v[1]->xyz * w1 + v[2]->xyz * w2;
			tx.normal = gnorm( v[0]->normal * w0 + v[1]->normal * w1 + v[2]->normal * w2 );
			if ( gdot( tx.normal, t.n ) < 0.2f )
				tx.normal = t.n;	// bad smoothing normals: fall back to the face normal
			tx.faceNormal = t.n;
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
Upsampling the original lightmaps to the bake resolution
===========================================================================
*/

template<typename F>
static void gi_parallel_for( int count, F func );

// plain bilinear sample of an original 128x128 page (clamped at the page edge)
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

// the original 128 owner map: which surface each BSP lightmap texel belongs to
static void gi_build_bsp_owner( giBake &bake ) {
	const int saved = gi_lmSize;
	gi_lmSize = DEVOGI_BSP_LM_SIZE;
	std::vector<giTexel> savedTexels;
	savedTexels.swap( bake.texels );
	gi_build_texels( bake );
	bake.bspSurf.assign( bake.owner.size(), -1 );

	// a border texel whose centre ended up inside neighbouring geometry was lit as
	// black by q3map2 (the dark seams along corners): leave those out of the upscale
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
				buried[i] = 1;	// hit the back of a wall from inside it
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

// direct light at bake resolution: bilinear over the original texels of the same
// surface only, so light never bleeds between neighbouring surfaces in the page
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
				// no good texel of this surface in the 2x2: widen to 5x5 (skips buried seams)
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
				// still nothing: take the nearest original texel
				const int sx = Com_Clampi( 0, N - 1, (int)( fx + 0.5f ) ), sy = Com_Clampi( 0, N - 1, (int)( fy + 0.5f ) );
				const byte *p = src + ( ( page * N + sy ) * N + sx ) * 3;
				out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
			}
		}
	} );
}

/*
===========================================================================
Bounce lighting
===========================================================================
*/

// radiance leaving a hit point, for one light style (0 = always-on light)
static gvec3 gi_hit_radiance( const giBake &bake, const giHit &hit, int styleValue ) {
	const giScene &scene = bake.scene;
	const giTri &t = scene.tris[hit.tri];
	const giSurf &s = scene.surfs[t.surf];
	if ( s.sky )
		return styleValue == 0 ? bake.skyColor : gvec3();

	const giVert &a = scene.verts[t.vert[0]];
	const giVert &b = scene.verts[t.vert[1]];
	const giVert &c = scene.verts[t.vert[2]];
	const float w0 = 1 - hit.u - hit.v, w1 = hit.u, w2 = hit.v;

	// the slot of the hit surface that carries the same light style
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
		const float fx = ( a.lm[slot][0] * w0 + b.lm[slot][0] * w1 + c.lm[slot][0] * w2 ) * DEVOGI_LM_SIZE;
		const float fy = ( a.lm[slot][1] * w0 + b.lm[slot][1] * w1 + c.lm[slot][1] * w2 ) * DEVOGI_LM_SIZE;
		const int x = Com_Clampi( 0, DEVOGI_LM_SIZE - 1, (int)fx );
		const int y = Com_Clampi( 0, DEVOGI_LM_SIZE - 1, (int)fy );
		const float *px = &bake.light[( ( page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x ) * 3];
		L = gvec3( px[0], px[1], px[2] );
	}
	else if ( page == LIGHTMAP_BY_VERTEX ) {
		L = gvec3( a.color[slot][0] * w0 + b.color[slot][0] * w1 + c.color[slot][0] * w2,
				   a.color[slot][1] * w0 + b.color[slot][1] * w1 + c.color[slot][1] * w2,
				   a.color[slot][2] * w0 + b.color[slot][2] * w1 + c.color[slot][2] * w2 );
	}
	return L * scene.shaders[s.shader].albedo;
}

template<typename F>
static void gi_parallel_for( int count, F func ) {
	int threads = r_devoGI_threads->integer;
	if ( threads <= 0 )
		threads = (int)std::thread::hardware_concurrency();
	threads = Com_Clampi( 1, 64, threads );

	std::atomic<int> next( 0 );
	auto worker = [&]() {
		while ( 1 ) {
			const int start = next.fetch_add( 256 );
			if ( start >= count )
				break;
			const int end = Q_min( count, start + 256 );
			for ( int i = start; i < end; i++ )
				func( i );
		}
	};
	std::vector<std::thread> pool;
	for ( int i = 1; i < threads; i++ )
		pool.emplace_back( worker );
	worker();
	for ( std::thread &t : pool )
		t.join();
}

static void gi_entity_direct( giBake &bake, const std::vector<giPointLight> &lights ) {
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
		float *px = &bake.direct[( ( tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
		// the lightmap may already hold these lights: keep the brighter, never add twice
		px[0] = Q_max( px[0], sum.x );
		px[1] = Q_max( px[1], sum.y );
		px[2] = Q_max( px[2], sum.z );
	} );
}

// bounce light and ambient occlusion arriving at one point: out[0..2] rgb, out[3] = 1 open .. 0 enclosed
static void gi_gather_point( const giBake &bake, const gvec3 &pos, const gvec3 &normal, const gvec3 &faceNormal,
	int styleValue, uint32_t seed, int rays, float out[4] ) {
	const giScene &scene = bake.scene;
	const float aoDist = Q_max( 8.0f, r_devoGI_aoDistance->value );
	gvec3 t, b;
	gi_basis( normal, t, b );
	uint32_t rng = gi_hash( seed );
	// start just off the real face plane; the smoothed normal of a tessellated curve
	// may point into neighbouring facets, which made striped occlusion on patches
	const gvec3 o = pos + faceNormal * 1.0f;
	gvec3 sum;
	float occlusion = 0;
	for ( int r = 0; r < rays; r++ ) {
		// cosine weighted hemisphere sample around the shading normal
		const float u1 = gi_rand( rng ), u2 = gi_rand( rng );
		const float rr = sqrtf( u1 ), phi = 6.2831853f * u2;
		const float lx = rr * cosf( phi ), ly = rr * sinf( phi ), lz = sqrtf( Q_max( 0.0f, 1.0f - u1 ) );
		gvec3 d = gnorm( t * lx + b * ly + normal * lz );
		const float below = gdot( d, faceNormal );
		if ( below < 0 )
			d = gnorm( d - faceNormal * ( 2.0f * below ) );	// mirror back above the face
		giHit hit;
		if ( !gi_trace( scene, o, d, 8192.0f, hit, false, DEVOGI_RAY_TMIN ) )
			continue;	// escaped the map
		const giTri &ht = scene.tris[hit.tri];
		const bool sky = scene.surfs[ht.surf].sky;
		// nearby geometry blocks light: ambient occlusion, fading out with distance
		if ( !sky && hit.t < aoDist )
			occlusion += 1.0f - hit.t / aoDist;
		if ( !sky && gdot( ht.n, d ) > 0 )
			continue;	// hit the back of a surface: inside geometry, no light
		sum += gi_hit_radiance( bake, hit, styleValue );
	}
	const float inv = 1.0f / rays;
	out[0] = sum.x * inv;
	out[1] = sum.y * inv;
	out[2] = sum.z * inv;
	out[3] = 1.0f - occlusion * inv;
}

static void gi_gather( giBake &bake, int rays ) {
	bake.indirect.assign( bake.texels.size() * 4, 0.0f );

	// at 4x and above, bounce light (smooth by nature) is gathered on every other texel in
	// each direction and filled in from same-surface neighbours: a quarter of the work
	const bool sparse = DEVOGI_LM_SIZE >= DEVOGI_BSP_LM_SIZE * 4;
	std::vector<char> done( bake.texels.size(), 0 );
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		if ( sparse && ( ( tx.x | tx.y ) & 1 ) )
			return;
		gi_gather_point( bake, tx.pos, tx.normal, tx.faceNormal, tx.styleValue, (uint32_t)i * 9781u + 1u, rays, &bake.indirect[i * 4] );
		done[i] = 1;
	} );
	if ( !sparse )
		return;

	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		if ( done[i] )
			return;
		const giTexel &tx = bake.texels[i];
		float sum[4] = { 0, 0, 0, 0 }, wsum = 0;
		for ( int dy = -1; dy <= 1; dy++ ) {
			for ( int dx = -1; dx <= 1; dx++ ) {
				const int x = tx.x + dx, y = tx.y + dy;
				if ( x < 0 || y < 0 || x >= DEVOGI_LM_SIZE || y >= DEVOGI_LM_SIZE )
					continue;
				const int o = bake.owner[( (size_t)tx.page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x];
				if ( o < 0 || !done[o] )
					continue;
				const giTexel &ot = bake.texels[o];
				if ( ot.styleValue != tx.styleValue || ( ot.surf != tx.surf && gdot( ot.normal, tx.normal ) < 0.9f ) )
					continue;
				for ( int k = 0; k < 4; k++ )
					sum[k] += bake.indirect[o * 4 + k];
				wsum += 1.0f;
			}
		}
		if ( wsum > 0 ) {
			for ( int k = 0; k < 4; k++ )
				bake.indirect[i * 4 + k] = sum[k] / wsum;
		} else {
			// isolated texel (thin strip): gather it directly
			gi_gather_point( bake, tx.pos, tx.normal, tx.faceNormal, tx.styleValue, (uint32_t)i * 9781u + 1u, rays, &bake.indirect[i * 4] );
		}
	} );
}

// smooth the noisy bounce light: 5x5 filter over texels of the same surface with similar normals
static void gi_denoise( giBake &bake ) {
	std::vector<float> out( bake.indirect.size() );
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		gvec3 sum;
		float aoSum = 0;
		float wsum = 0;
		for ( int dy = -2; dy <= 2; dy++ ) {
			for ( int dx = -2; dx <= 2; dx++ ) {
				const int x = tx.x + dx, y = tx.y + dy;
				if ( x < 0 || y < 0 || x >= DEVOGI_LM_SIZE || y >= DEVOGI_LM_SIZE )
					continue;
				const int o = bake.owner[( tx.page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x];
				if ( o < 0 )
					continue;
				const giTexel &ot = bake.texels[o];
				if ( ot.surf != tx.surf && gdot( ot.normal, tx.normal ) < 0.9f )
					continue;
				const float w = ( 1.0f / ( 1.0f + dx * dx + dy * dy ) ) * Q_max( 0.0f, gdot( ot.normal, tx.normal ) );
				sum += gvec3( &bake.indirect[o * 4] ) * w;
				aoSum += bake.indirect[o * 4 + 3] * w;
				wsum += w;
			}
		}
		if ( wsum > 0 ) {
			sum = sum * ( 1.0f / wsum );
			aoSum /= wsum;
		} else {
			aoSum = bake.indirect[i * 4 + 3];
		}
		out[i * 4 + 0] = sum.x;
		out[i * 4 + 1] = sum.y;
		out[i * 4 + 2] = sum.z;
		out[i * 4 + 3] = aoSum;
	} );
	bake.indirect.swap( out );
}

static bool gi_relit;	// direct light was traced with real shadows: no fake contact darkening

static void gi_compose( giBake &bake, float intensity ) {
	const float aoIndirect = Com_Clamp( 0.0f, 2.0f, r_devoGI_ao->value );
	const float aoDirect = gi_relit ? 0.0f : Com_Clamp( 0.0f, 1.0f, r_devoGI_aoDirect->value );
	bake.light = bake.direct;
	for ( size_t i = 0; i < bake.texels.size(); i++ ) {
		const giTexel &tx = bake.texels[i];
		float *px = &bake.light[( ( tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
		const float ao = Com_Clamp( 0.0f, 1.0f, bake.indirect[i * 4 + 3] );
		// bounce light is darkened by occlusion (keeps corners black), the original
		// lightmap only a little, for contact shadows
		const float bounceScale = intensity * powf( ao, aoIndirect * 2.0f );
		const float directScale = 1.0f - aoDirect * ( 1.0f - ao );
		px[0] = px[0] * directScale + bake.indirect[i * 4 + 0] * bounceScale;
		px[1] = px[1] * directScale + bake.indirect[i * 4 + 1] * bounceScale;
		px[2] = px[2] * directScale + bake.indirect[i * 4 + 2] * bounceScale;
	}
}

// spread lit texels 2px into empty neighbours so bilinear filtering does not pull in black
static void gi_dilate( giBake &bake, std::vector<float> &img ) {
	std::vector<char> filled( bake.owner.size() );
	for ( size_t i = 0; i < bake.owner.size(); i++ )
		filled[i] = bake.owner[i] >= 0;
	for ( int pass = 0; pass < 2; pass++ ) {
		std::vector<char> next = filled;
		for ( int p = 0; p < bake.numPages; p++ ) {
			for ( int y = 0; y < DEVOGI_LM_SIZE; y++ ) {
				for ( int x = 0; x < DEVOGI_LM_SIZE; x++ ) {
					const int pix = ( p * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x;
					if ( filled[pix] )
						continue;
					float s[3] = { 0, 0, 0 };
					int n = 0;
					for ( int dy = -1; dy <= 1; dy++ ) {
						for ( int dx = -1; dx <= 1; dx++ ) {
							const int xx = x + dx, yy = y + dy;
							if ( xx < 0 || yy < 0 || xx >= DEVOGI_LM_SIZE || yy >= DEVOGI_LM_SIZE )
								continue;
							const int q = ( p * DEVOGI_LM_SIZE + yy ) * DEVOGI_LM_SIZE + xx;
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
}

/*
===========================================================================
Relighting: direct light recomputed from the map's own light sources

The old lightmaps are low resolution with blocky shadows. When the map's
light sources are known (DevoLights), the direct light is traced again at
bake resolution with soft shadows, and the old lightmap is only used to
calibrate the overall brightness.
===========================================================================
*/

struct giLightGrid {
	float						cell;
	gvec3						origin;
	int							nx, ny, nz;
	std::vector<std::vector<int>>	cells;
};

static void gi_build_light_grid( giLightGrid &g ) {
	g.cell = 256.0f;
	g.cells.clear();
	if ( gi_lights.empty() ) {
		g.nx = g.ny = g.nz = 0;
		return;
	}
	gvec3 mins( DEVOGI_INF, DEVOGI_INF, DEVOGI_INF ), maxs( -DEVOGI_INF, -DEVOGI_INF, -DEVOGI_INF );
	for ( const devoLight_t &lt : gi_lights ) {
		const gvec3 r( lt.radius, lt.radius, lt.radius );
		mins = gmin( mins, lt.origin - r );
		maxs = gmax( maxs, lt.origin + r );
	}
	g.origin = mins;
	g.nx = Q_max( 1, (int)ceilf( ( maxs.x - mins.x ) / g.cell ) );
	g.ny = Q_max( 1, (int)ceilf( ( maxs.y - mins.y ) / g.cell ) );
	g.nz = Q_max( 1, (int)ceilf( ( maxs.z - mins.z ) / g.cell ) );
	// keep the grid sane on huge maps
	while ( (size_t)g.nx * g.ny * g.nz > 262144 ) {
		g.cell *= 2.0f;
		g.nx = Q_max( 1, (int)ceilf( ( maxs.x - mins.x ) / g.cell ) );
		g.ny = Q_max( 1, (int)ceilf( ( maxs.y - mins.y ) / g.cell ) );
		g.nz = Q_max( 1, (int)ceilf( ( maxs.z - mins.z ) / g.cell ) );
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

// direct light at a point, uncalibrated: area/point lights and sun returned separately
static void gi_direct_point( const giBake &bake, const giLightGrid &grid, const gvec3 &pos, const gvec3 &normal,
	const gvec3 &faceNormal, uint32_t seed, gvec3 &lightsOut, gvec3 &sunOut ) {
	const giScene &scene = bake.scene;
	uint32_t rng = gi_hash( seed ^ 0xA5A5A5A5u );
	const gvec3 o = pos + faceNormal * 1.0f;
	lightsOut = gvec3();
	sunOut = gvec3();

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
				const float cosR = gdot( normal, L );
				if ( cosR <= 0 )
					continue;
				float cosE = 1.0f;
				if ( lt.size > 0 ) {
					cosE = -gdot( lt.normal, L );
					if ( cosE <= 0 )
						continue;	// behind the emitting surface
				}
				// distance never below the patch size: no singular hot spots right at the lamp
				const float dd2 = Q_max( d2, lt.size * lt.size * 0.25f + 16.0f );
				const float unshadowed = lt.photons * cosR * cosE / dd2;
				if ( unshadowed < 0.25f )
					continue;

				// soft shadows: sample across the emitting patch, more samples when close
				int samples = 2;
				if ( lt.size > 0 ) {
					if ( d < lt.size * 3.0f ) samples = 12;
					else if ( d < lt.size * 8.0f ) samples = 6;
					else samples = 3;
				}
				gvec3 et, eb;
				if ( lt.size > 0 )
					gi_basis( lt.normal, et, eb );
				int visible = 0;
				const int strata = (int)ceilf( sqrtf( (float)samples ) );
				for ( int s = 0; s < samples; s++ ) {
					gvec3 target = lt.origin;
					if ( lt.size > 0 && samples > 1 ) {
						// stratified across the patch
						const float jx = ( ( s % strata ) + gi_rand( rng ) ) / strata - 0.5f;
						const float jy = ( ( ( s / strata ) % strata ) + gi_rand( rng ) ) / strata - 0.5f;
						target += et * ( jx * lt.size ) + eb * ( jy * lt.size );
					}
					gvec3 Ls = target - o;
					const float ds = sqrtf( gdot( Ls, Ls ) );
					if ( ds < 1.0f ) { visible++; continue; }
					Ls = Ls * ( 1.0f / ds );
					giHit hit;
					if ( !gi_trace( scene, o, Ls, ds - 2.0f, hit, true, DEVOGI_RAY_TMIN ) )
						visible++;
				}
				lightsOut += lt.color * ( unshadowed * visible / (float)samples );
			}
		}
	}

	if ( gi_hasSun ) {
		const float cosS = gdot( normal, gi_sunDir );
		if ( cosS > 0 ) {
			// soft sun: jitter within ~1.5 degrees
			gvec3 st, sb;
			gi_basis( gi_sunDir, st, sb );
			// stratified 4x4 over the sun disc: smooth penumbra instead of speckle
			const int grid = 4, samples = grid * grid;
			int visible = 0;
			for ( int s = 0; s < samples; s++ ) {
				const float jx = ( ( s % grid ) + gi_rand( rng ) ) / grid - 0.5f;
				const float jy = ( ( s / grid ) + gi_rand( rng ) ) / grid - 0.5f;
				const gvec3 dir = gnorm( gi_sunDir + st * ( jx * 0.05f ) + sb * ( jy * 0.05f ) );
				giHit hit;
				if ( !gi_trace( scene, o, dir, 16384.0f, hit, false, DEVOGI_RAY_TMIN ) || scene.surfs[scene.tris[hit.tri].surf].sky )
					visible++;	// reached the sky
			}
			sunOut = gi_sunColor * ( gi_sunIntensity * cosS * visible / (float)samples );
		}
	}
}

static inline float gi_luma( const gvec3 &c ) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// 3x3 tent filter over neighbouring texels of the same surface (or the same plane with a
// similar normal): anti-aliases stair-stepped shadow edges and sample noise without
// letting light bleed around corners
static void gi_smooth_texels( const giBake &bake, std::vector<gvec3> &v ) {
	std::vector<gvec3> out( v.size() );
	gi_parallel_for( (int)bake.texels.size(), [&]( int i ) {
		const giTexel &tx = bake.texels[i];
		gvec3 sum;
		float wsum = 0;
		for ( int dy = -1; dy <= 1; dy++ ) {
			for ( int dx = -1; dx <= 1; dx++ ) {
				const int x = tx.x + dx, y = tx.y + dy;
				if ( x < 0 || y < 0 || x >= DEVOGI_LM_SIZE || y >= DEVOGI_LM_SIZE )
					continue;
				const int o = bake.owner[( (size_t)tx.page * DEVOGI_LM_SIZE + y ) * DEVOGI_LM_SIZE + x];
				if ( o < 0 )
					continue;
				const giTexel &ot = bake.texels[o];
				if ( ot.styleValue != tx.styleValue )
					continue;
				if ( ot.surf != tx.surf ) {
					if ( gdot( ot.normal, tx.normal ) < 0.95f || fabsf( gdot( ot.pos - tx.pos, tx.faceNormal ) ) > 2.0f )
						continue;
				}
				const float w = ( dx ? 0.5f : 1.0f ) * ( dy ? 0.5f : 1.0f );
				sum += v[o] * w;
				wsum += w;
			}
		}
		out[i] = wsum > 0 ? sum * ( 1.0f / wsum ) : v[i];
	} );
	v.swap( out );
}

/*
===========================================================================
Cache file
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
	int32_t		numVertexColors;// rgba entries after the lightmaps (one per draw vert, or 0)
};
#pragma pack(pop)

static uint64_t gi_fnv( uint64_t h, const void *data, size_t len ) {
	const byte *p = (const byte *)data;
	for ( size_t i = 0; i < len; i++ ) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

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
	char buf[256];
	Com_sprintf( buf, sizeof( buf ), "v%d ex%g r%d b%d i%g s%g e%d ao%g aod%g aor%g sat%g x%d rl%d lh%llx", DEVOGI_VERSION, r_devoGI_exposure->value,
		Com_Clampi( 8, 1024, r_devoGI_rays->integer ), Com_Clampi( 1, 8, r_devoGI_bounces->integer ),
		r_devoGI_intensity->value, r_devoGI_sky->value, r_devoGI_entityLights->integer,
		r_devoGI_ao->value, r_devoGI_aoDirect->value, r_devoGI_aoDistance->value, r_devoGI_saturation->value, scale,
		r_devoGI_relight->integer, (unsigned long long)( r_devoGI_relight->integer ? gi_lightsHash : 0 ) );
	return gi_fnv( 1469598103934665603ULL, buf, strlen( buf ) );
}

static void gi_cache_name( const char *mapName, char *out, int outSize ) {
	char base[MAX_QPATH];
	COM_StripExtension( mapName, base, sizeof( base ) );
	Com_sprintf( out, outSize, "%s.devogi", base );
}

/*
===========================================================================
R_DevoGI_Process

Returns a Z_Malloc'd lightmap lump with bounce light added, in the BSP's
page layout but with pages of *outPageSize (128 * scale) pixels, or NULL to
keep the original lightmaps. maxScale is how far pages may grow before the
renderer's lightmap atlas would exceed the GPU's texture size limit.
===========================================================================
*/

byte *R_DevoGI_Process( const char *mapName, const byte *fileBase, const dheader_t *header, int maxScale, int *outPageSize )
{
	*outPageSize = DEVOGI_BSP_LM_SIZE;
	gi_vertexColors.clear();
	if ( !r_devoGI || !r_devoGI->integer )
		return NULL;

	const int numDrawVerts = header->lumps[LUMP_DRAWVERTS].filelen / (int)sizeof( mapVert_t );

	const lump_t &ll = header->lumps[LUMP_LIGHTMAPS];
	const int srcPageBytes = DEVOGI_BSP_LM_SIZE * DEVOGI_BSP_LM_SIZE * 3;
	if ( ll.filelen < srcPageBytes )
		return NULL;	// external or no lightmaps: not supported yet
	const int numPages = ll.filelen / srcPageBytes;
	const byte *srcLightmaps = fileBase + ll.fileofs;

	// bake resolution: r_devoGI_scale rounded down to 1, 2 or 4, within the GPU limit
	int scale = 1;
	while ( scale * 2 <= Com_Clampi( 1, 8, r_devoGI_scale->integer ) && scale * 2 <= maxScale )
		scale *= 2;
	const int pageSize = DEVOGI_BSP_LM_SIZE * scale;
	const int pageBytes = pageSize * pageSize * 3;
	const int outBytes = numPages * pageBytes;

	char cacheName[MAX_QPATH];
	gi_cache_name( mapName, cacheName, sizeof( cacheName ) );
	const uint64_t bspHash = gi_bsp_hash( fileBase, header );
	const uint64_t settingsHash = gi_settings_hash( scale );

	// 1. cached bake for this exact BSP and these settings?
	if ( !r_devoGI_rebuild->integer ) {
		void *cache = NULL;
		const int len = ri.FS_ReadFile( cacheName, &cache );
		if ( cache ) {
			const devogiHeader_t *h = (const devogiHeader_t *)cache;
			if ( len >= (int)sizeof( devogiHeader_t )
				&& !memcmp( h->magic, "DVGI", 4 ) && h->version == DEVOGI_VERSION
				&& h->bspHash == bspHash && h->settingsHash == settingsHash
				&& h->lightmapBytes == outBytes && h->pageSize == pageSize
				&& ( h->numVertexColors == 0 || h->numVertexColors == numDrawVerts )
				&& len == (int)sizeof( devogiHeader_t ) + outBytes + h->numVertexColors * 4 ) {
				byte *out = (byte *)Z_Malloc( outBytes, TAG_BSP, qfalse );
				memcpy( out, (const byte *)cache + sizeof( devogiHeader_t ), outBytes );
				if ( h->numVertexColors ) {
					const byte *vc = (const byte *)cache + sizeof( devogiHeader_t ) + outBytes;
					gi_vertexColors.assign( vc, vc + h->numVertexColors * 4 );
				}
				ri.FS_FreeFile( cache );
				ri.Printf( PRINT_ALL, "DevoGI: using baked lighting from %s (%dx%d lightmaps)\n", cacheName, pageSize, pageSize );
				*outPageSize = pageSize;
				return out;
			}
			ri.FS_FreeFile( cache );
			ri.Printf( PRINT_ALL, "DevoGI: %s is out of date (map or settings changed), rebaking\n", cacheName );
		}
	}
	ri.Cvar_Set( "r_devoGI_rebuild", "0" );

	// 2. bake
	const int startTime = ri.Milliseconds();
	const int rays = Com_Clampi( 8, 1024, r_devoGI_rays->integer );
	const int bounces = Com_Clampi( 1, 8, r_devoGI_bounces->integer );
	const float intensity = Q_max( 0.0f, r_devoGI_intensity->value );

	giBake *bakePtr = new giBake();
	giBake &bake = *bakePtr;
	bake.numPages = numPages;

	// shaders
	{
		const lump_t &l = header->lumps[LUMP_SHADERS];
		const dshader_t *in = (const dshader_t *)( fileBase + l.fileofs );
		const int count = l.filelen / sizeof( dshader_t );
		bake.scene.shaders.resize( Q_max( 1, count ) );
		for ( int i = 0; i < count; i++ ) {
			giShader &s = bake.scene.shaders[i];
			s.surfaceFlags = LittleLong( in[i].surfaceFlags );
			s.contentFlags = LittleLong( in[i].contentFlags );
			s.albedo = ( s.surfaceFlags & SURF_SKY ) ? gvec3() : gi_shader_albedo( in[i].shader );

			// colour bleeding: averaged texture colours are muddy, so push the saturation
			// of reflected light (keeps brightness, still capped at 0.9 like q3map2)
			const float sat = Com_Clamp( 0.0f, 4.0f, r_devoGI_saturation->value );
			const float lum = 0.2126f * s.albedo.x + 0.7152f * s.albedo.y + 0.0722f * s.albedo.z;
			gvec3 c( lum + ( s.albedo.x - lum ) * sat, lum + ( s.albedo.y - lum ) * sat, lum + ( s.albedo.z - lum ) * sat );
			c = gmax( c, gvec3( 0, 0, 0 ) );
			const float m = Q_max( c.x, Q_max( c.y, c.z ) );
			if ( m > 0.9f )
				c = c * ( 0.9f / m );
			s.albedo = c;
		}
	}

	// sky: tinted by the sun colour of the sky shader (q3map_sun / q3map_sunExt), if any
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
		bake.skyColor = tint * ( 64.0f * Q_max( 0.0f, r_devoGI_sky->value ) );
	}

	gi_build_geometry( bake.scene, fileBase, header );
	gi_build_bvh( bake.scene );
	gi_build_bsp_owner( bake );		// at the BSP's 128 resolution
	gi_lmSize = pageSize;
	gi_build_texels( bake );		// at bake resolution

	ri.Printf( PRINT_ALL, "DevoGI: baking %s: %d lightmap pages at %dx%d, %d texels, %d triangles, %d rays x %d bounces...\n",
		mapName, numPages, pageSize, pageSize, (int)bake.texels.size(), (int)bake.scene.tris.size(), rays, bounces );

	// direct light = the original lightmaps, smoothly upsampled per surface
	gi_upsample_direct( bake, srcLightmaps );

	// relight: replace the old direct light with light traced from the map's own sources
	const bool relight = r_devoGI_relight->integer && ( !gi_lights.empty() || gi_hasSun );
	gi_relit = relight;
	giLightGrid lightGrid;
	float relightScaleLights = 1.0f, relightScaleSun = 1.0f;
	std::vector<gvec3> lightPart, sunPart;	// traced direct light per texel (kept for the exposure match)
	std::vector<float> origLuma;			// brightness of the original lightmap per texel
	float exposureLights = 1.0f, exposureSun = 1.0f;	// exposure match, also applied to vertex colours
	if ( relight ) {
		gi_build_light_grid( lightGrid );
		const size_t n = bake.texels.size();
		lightPart.assign( n, gvec3() );
		sunPart.assign( n, gvec3() );
		origLuma.assign( n, 0.0f );
		for ( size_t i = 0; i < n; i++ ) {
			const giTexel &tx = bake.texels[i];
			const float *px = &bake.direct[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			origLuma[i] = gi_luma( gvec3( px[0], px[1], px[2] ) );
		}
		gi_parallel_for( (int)n, [&]( int i ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 )
				return;	// styled (switchable) light keeps its original lightmap
			gi_direct_point( bake, lightGrid, tx.pos, tx.normal, tx.faceNormal, (uint32_t)i * 2654435761u, lightPart[i], sunPart[i] );
		} );
		gi_smooth_texels( bake, lightPart );
		gi_smooth_texels( bake, sunPart );

		// calibrate against the old lightmap: least squares fit of old = a * lights + b * sun
		double sAA = 0, sSS = 0, sAS = 0, sOA = 0, sOS = 0;
		for ( size_t i = 0; i < n; i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 || tx.edge )
				continue;
			const float *px = &bake.direct[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			const double O = gi_luma( gvec3( px[0], px[1], px[2] ) );
			const double A = gi_luma( lightPart[i] ), S = gi_luma( sunPart[i] );
			if ( O < 2.0 && A + S < 2.0 )
				continue;
			sAA += A * A; sSS += S * S; sAS += A * S; sOA += O * A; sOS += O * S;
		}
		const double det = sAA * sSS - sAS * sAS;
		if ( sSS > 0 && sAA > 0 && fabs( det ) > 1e-9 * sAA * sSS ) {
			relightScaleLights = (float)( ( sOA * sSS - sOS * sAS ) / det );
			relightScaleSun = (float)( ( sOS * sAA - sOA * sAS ) / det );
		} else if ( sAA > 0 ) {
			relightScaleLights = (float)( sOA / sAA );
		} else if ( sSS > 0 ) {
			relightScaleSun = (float)( sOS / sSS );
		}
		// a bad fit (lights and sun overlapping a lot) can go negative: fall back to separate ratios
		if ( relightScaleLights <= 0 && sAA > 0 ) relightScaleLights = (float)( sOA / sAA );
		if ( relightScaleSun <= 0 && sSS > 0 ) relightScaleSun = (float)( sOS / sSS );
		relightScaleLights = Com_Clamp( 0.001f, 1000.0f, relightScaleLights );
		relightScaleSun = Com_Clamp( 0.001f, 1000.0f, relightScaleSun );

		for ( size_t i = 0; i < n; i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 )
				continue;
			const gvec3 c = lightPart[i] * relightScaleLights + sunPart[i] * relightScaleSun;
			float *px = &bake.direct[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			px[0] = c.x; px[1] = c.y; px[2] = c.z;
		}
		ri.Printf( PRINT_ALL, "DevoGI: relit from %d lights%s (brightness match: lights x%.3f, sun x%.3f, %.1fs)\n",
			(int)gi_lights.size(), gi_hasSun ? " and the sun" : "", relightScaleLights, relightScaleSun,
			( ri.Milliseconds() - startTime ) / 1000.0f );
	}

	if ( !relight && r_devoGI_entityLights->integer ) {
		std::vector<giPointLight> lights;
		const lump_t &le = header->lumps[LUMP_ENTITIES];
		gi_parse_lights( (const char *)( fileBase + le.fileofs ), le.filelen, lights );
		if ( !lights.empty() ) {
			ri.Printf( PRINT_ALL, "DevoGI: adding %d light entities from the BSP\n", (int)lights.size() );
			gi_entity_direct( bake, lights );
		}
	}

	bake.light = bake.direct;
	for ( int b = 0; b < bounces; b++ ) {
		gi_gather( bake, rays );
		gi_denoise( bake );
		gi_compose( bake, intensity );
		ri.Printf( PRINT_ALL, "DevoGI: bounce %d/%d done (%.1fs)\n", b + 1, bounces, ( ri.Milliseconds() - startTime ) / 1000.0f );
	}
	// exposure match: the old lightmaps also held q3map2's own bounce and ambient light, so
	// after our bounces, lamp-lit and sunlit areas are each scaled to the original's
	// brightness (interiors were coming out too dark), then r_devoGI_exposure on top
	if ( relight ) {
		double oL = 0, fL = 0, oS = 0, fS = 0;
		for ( size_t i = 0; i < bake.texels.size(); i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 || tx.edge || origLuma[i] < 4.0f )
				continue;
			const float *px = &bake.light[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			const float fin = gi_luma( gvec3( px[0], px[1], px[2] ) );
			const float a = gi_luma( lightPart[i] ) * relightScaleLights, s = gi_luma( sunPart[i] ) * relightScaleSun;
			if ( s > a * 2.0f ) { oS += origLuma[i]; fS += fin; }
			else { oL += origLuma[i]; fL += fin; }
		}
		const float rL = fL > 0 ? Com_Clamp( 0.25f, 8.0f, (float)( oL / fL ) ) : 1.0f;
		const float rS = fS > 0 ? Com_Clamp( 0.25f, 8.0f, (float)( oS / fS ) ) : 1.0f;
		const float exposure = Q_max( 0.0f, r_devoGI_exposure->value );
		exposureLights = rL * exposure;
		exposureSun = rS * exposure;
		for ( size_t i = 0; i < bake.texels.size(); i++ ) {
			const giTexel &tx = bake.texels[i];
			if ( tx.styleValue != 0 )
				continue;
			const float a = gi_luma( lightPart[i] ) * relightScaleLights, s = gi_luma( sunPart[i] ) * relightScaleSun;
			const float wS = ( a + s ) > 1e-3f ? s / ( a + s ) : 0.0f;	// unlit by either: an interior
			const float f = ( wS * rS + ( 1.0f - wS ) * rL ) * exposure;
			float *px = &bake.light[( ( (size_t)tx.page * DEVOGI_LM_SIZE + tx.y ) * DEVOGI_LM_SIZE + tx.x ) * 3];
			px[0] *= f; px[1] *= f; px[2] *= f;
		}
		ri.Printf( PRINT_ALL, "DevoGI: exposure match: interiors x%.2f, sunlit x%.2f, r_devoGI_exposure %.2f\n", rL, rS, exposure );
	}

	gi_dilate( bake, bake.light );

	// vertex-lit surfaces (mostly models baked into the map) get the same bounce
	// light and occlusion per vertex, written into their style 0 vertex colours
	{
		const lump_t &lv = header->lumps[LUMP_DRAWVERTS];
		const lump_t &ls = header->lumps[LUMP_SURFACES];
		const mapVert_t *dv = (const mapVert_t *)( fileBase + lv.fileofs );
		const dsurface_t *ds = (const dsurface_t *)( fileBase + ls.fileofs );
		const int numSurfs = ls.filelen / sizeof( dsurface_t );

		std::vector<int> list;					// vertices of vertex-lit surfaces: traced
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
			// lightmapped surfaces also carry q3map2 vertex colours, used by shader stages
			// with rgbGen vertex / exactVertex: those must follow the new lightmap too
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

			// lightmapped surfaces: the new lightmap at the vertex (bilinear)
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
				const float m = Q_max( c[0], Q_max( c[1], c[2] ) );
				const float s = m > 255.0f ? 255.0f / m : 1.0f;
				for ( int k = 0; k < 3; k++ )
					gi_vertexColors[(size_t)v * 4 + k] = (byte)Com_Clampi( 0, 255, (int)( c[k] * s + 0.5f ) );
			} );

			const float aoIndirect = Com_Clamp( 0.0f, 2.0f, r_devoGI_ao->value );
			const float aoDirect = Com_Clamp( 0.0f, 1.0f, r_devoGI_aoDirect->value );
			gi_parallel_for( (int)list.size(), [&]( int i ) {
				const int v = list[i];
				giVert gv;
				gi_load_vert( gv, dv[v] );
				float g[4];
				// twice the rays: a vertex has no neighbours to denoise with
				gi_gather_point( bake, gv.xyz, gv.normal, gv.normal, 0, (uint32_t)v * 7919u + 13u, rays * 2, g );
				const float ao = Com_Clamp( 0.0f, 1.0f, g[3] );
				const float bounceScale = intensity * powf( ao, aoIndirect * 2.0f );
				float direct[3] = { (float)dv[v].color[0][0], (float)dv[v].color[0][1], (float)dv[v].color[0][2] };
				float directScale = 1.0f - aoDirect * ( 1.0f - ao );
				gvec3 lp, sp;
				if ( relight ) {
					// same traced light as the lightmaps, real shadows instead of the fake contact darkening
					gi_direct_point( bake, lightGrid, gv.xyz, gv.normal, gv.normal, (uint32_t)v * 40503u + 7u, lp, sp );
					const gvec3 c = lp * relightScaleLights + sp * relightScaleSun;
					direct[0] = c.x; direct[1] = c.y; direct[2] = c.z;
					directScale = 1.0f;
				}
				float c[3];
				for ( int k = 0; k < 3; k++ )
					c[k] = direct[k] * directScale + g[k] * bounceScale;
				if ( relight ) {
					// same exposure match as the lightmaps, weighted by how sunlit the vertex is
					const float a = gi_luma( lp ) * relightScaleLights, sl = gi_luma( sp ) * relightScaleSun;
					const float wS = ( a + sl ) > 1e-3f ? sl / ( a + sl ) : 0.0f;
					const float f = wS * exposureSun + ( 1.0f - wS ) * exposureLights;
					for ( int k = 0; k < 3; k++ )
						c[k] *= f;
				}
				const float m = Q_max( c[0], Q_max( c[1], c[2] ) );
				const float s = m > 255.0f ? 255.0f / m : 1.0f;
				for ( int k = 0; k < 3; k++ )
					gi_vertexColors[(size_t)v * 4 + k] = (byte)Com_Clampi( 0, 255, (int)( c[k] * s + 0.5f ) );
			} );
			ri.Printf( PRINT_ALL, "DevoGI: lit %d vertices of vertex-lit models, %d vertex colours of lightmapped surfaces\n",
				(int)list.size(), (int)lmList.size() );
		}
	}

	// 3. back to lightmap bytes; colour-normalize instead of clipping to white
	byte *out = (byte *)Z_Malloc( outBytes, TAG_BSP, qfalse );
	for ( size_t pix = 0; pix < bake.owner.size(); pix++ ) {
		float r = bake.light[pix * 3], g = bake.light[pix * 3 + 1], bl = bake.light[pix * 3 + 2];
		const float m = Q_max( r, Q_max( g, bl ) );
		if ( m > 255.0f ) { const float s = 255.0f / m; r *= s; g *= s; bl *= s; }
		out[pix * 3 + 0] = (byte)Com_Clampi( 0, 255, (int)( r + 0.5f ) );
		out[pix * 3 + 1] = (byte)Com_Clampi( 0, 255, (int)( g + 0.5f ) );
		out[pix * 3 + 2] = (byte)Com_Clampi( 0, 255, (int)( bl + 0.5f ) );
	}

	// deluxemap pages are never referenced by surfaces: just upsample them
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

	// 4. save the cache next to the map (in the home path)
	{
		const int numVC = (int)( gi_vertexColors.size() / 4 );
		const int fileLen = (int)sizeof( devogiHeader_t ) + outBytes + numVC * 4;
		byte *file = (byte *)Z_Malloc( fileLen, TAG_TEMP_WORKSPACE, qfalse );
		devogiHeader_t *h = (devogiHeader_t *)file;
		memcpy( h->magic, "DVGI", 4 );
		h->version = DEVOGI_VERSION;
		h->bspHash = bspHash;
		h->settingsHash = settingsHash;
		h->lightmapBytes = outBytes;
		h->pageSize = pageSize;
		h->numVertexColors = numVC;
		memcpy( file + sizeof( devogiHeader_t ), out, outBytes );
		if ( numVC )
			memcpy( file + sizeof( devogiHeader_t ) + outBytes, gi_vertexColors.data(), numVC * 4 );
		ri.FS_WriteFile( cacheName, file, fileLen );
		Z_Free( file );
	}

	ri.Printf( PRINT_ALL, "DevoGI: baked %s in %.1fs, saved %s\n", mapName, ( ri.Milliseconds() - startTime ) / 1000.0f, cacheName );
	*outPageSize = pageSize;
	return out;
}

/*
===========================================================================
R_DevoGI_PatchVertexColors

Writes the baked style 0 vertex colours of vertex-lit surfaces into the
loaded BSP's draw verts, so R_LoadSurfaces picks them up.
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
DevoLights: the map's own light sources, found at load time

q3map2 bakes surface lights (shaders with q3map_surfacelight), the sun
(q3map_sun / q3map_sunExt on the sky shader) and light entities into the
lightmaps, then throws the sources away. They are rebuilt here the same
way q3map2 builds them, so the map can be lit in real time:
  - surface lights: every surface on an emitting shader is cut into
    patches of q3map_lightsubdivide units, each becoming one light with
    photons = q3map_surfacelight * area * 0.25 (q3map2's area scale)
  - the sun: direction, colour and intensity from the sky shader
  - light entities still present in the BSP
===========================================================================
*/

struct giLightShader {
	float	value;		// q3map_surfacelight
	float	subdivide;	// q3map_lightsubdivide
	gvec3	color;
};

// normalized average colour of an image (max component 1); false if it can't be loaded
static bool gi_image_color( const char *name, gvec3 &out ) {
	char texName[MAX_QPATH];
	Q_strncpyz( texName, name, sizeof( texName ) );
	COM_StripExtension( texName, texName, sizeof( texName ) );
	byte *pic = NULL;
	int w = 0, h = 0;
	R_LoadImage( texName, &pic, &w, &h );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic )
			Z_Free( pic );
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
	Z_Free( pic );
	const float m = (float)Q_max( r, Q_max( g, b ) );
	if ( m <= 0 )
		return false;
	out = gvec3( (float)( r / m ), (float)( g / m ), (float)( b / m ) );
	return true;
}

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
	while ( 1 ) {
		const char *token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		if ( token[0] == '{' ) { depth++; continue; }
		if ( token[0] == '}' ) { if ( --depth <= 0 ) break; continue; }
		if ( depth == 1 ) {
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
		else if ( depth >= 2 && !firstMap[0] && ( !Q_stricmp( token, "map" ) || !Q_stricmp( token, "clampmap" ) ) ) {
			token = COM_ParseExt( &p, qfalse );
			if ( token[0] && token[0] != '$' && token[0] != '*' )
				Q_strncpyz( firstMap, token, sizeof( firstMap ) );
		}
	}
	if ( ls.value <= 0 )
		return false;
	if ( ls.subdivide < 8.0f )
		ls.subdivide = 8.0f;

	// colour the way q3map2 picks it: lightRGB, else lightImage, editor image, first stage, shader name
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

// sun of a sky shader: q3map_sun / q3map_sunExt / sun  r g b intensity azimuth elevation
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

void R_DevoLights_Extract( const char *mapName, const byte *fileBase, const dheader_t *header )
{
	gi_lights.clear();
	gi_hasSun = false;
	Q_strncpyz( gi_lightsMap, mapName, sizeof( gi_lightsMap ) );
	const int startTime = ri.Milliseconds();

	// shaders: which emit light, and the sun
	const lump_t &l = header->lumps[LUMP_SHADERS];
	const dshader_t *in = (const dshader_t *)( fileBase + l.fileofs );
	const int numShaders = l.filelen / sizeof( dshader_t );
	std::vector<giLightShader> lightShaders( Q_max( 1, numShaders ) );
	std::vector<char> emits( Q_max( 1, numShaders ), 0 );
	int numEmitting = 0;

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

	// surface lights: cut each emitting surface into subdivide-sized patches
	if ( numEmitting ) {
		gi_build_geometry( scene, fileBase, header );

		struct accum_t { gvec3 c, n; float area; int shader; };
		std::vector<accum_t> patches;
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
			// open-addressing lookup
			size_t slot = key & ( table.size() - 1 );
			int idx = -1;
			while ( table[slot] >= 0 ) {
				if ( tableKeys[slot] == key ) { idx = table[slot]; break; }
				slot = ( slot + 1 ) & ( table.size() - 1 );
			}
			if ( idx < 0 ) {
				if ( patches.size() * 2 >= table.size() ) {
					// grow and rehash
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
			gi_lights.push_back( lt );
		}
	}

	gi_lightsHash = 1469598103934665603ULL;
	for ( const devoLight_t &lt : gi_lights )
		gi_lightsHash = gi_fnv( gi_lightsHash, &lt, sizeof( lt ) );
	if ( gi_hasSun ) {
		gi_lightsHash = gi_fnv( gi_lightsHash, &gi_sunDir, sizeof( gi_sunDir ) );
		gi_lightsHash = gi_fnv( gi_lightsHash, &gi_sunColor, sizeof( gi_sunColor ) );
		gi_lightsHash = gi_fnv( gi_lightsHash, &gi_sunIntensity, sizeof( gi_sunIntensity ) );
	}

	ri.Printf( PRINT_ALL, "DevoLights: %s: %d lights from %d emitting shaders, %s (%dms)\n",
		mapName, (int)gi_lights.size(), numEmitting, gi_hasSun ? "sun found" : "no sun",
		ri.Milliseconds() - startTime );
}

// the nearest and brightest map lights visible from the view, as dynamic lights for this scene
void R_DevoLights_AddToScene( const vec3_t viewOrg, int freeSlots )
{
	if ( !r_devoLights || !r_devoLights->integer || gi_lights.empty() || freeSlots <= 0 )
		return;
	const int maxLights = Q_min( freeSlots, Com_Clampi( 0, 64, r_devoLights_max->integer ) );
	if ( maxLights <= 0 )
		return;

	const float rangeScale = Q_max( 0.05f, r_devoLights_range->value );
	const gvec3 view( viewOrg );
	std::vector<std::pair<float, int>> cand;
	cand.reserve( 256 );
	for ( int i = 0; i < (int)gi_lights.size(); i++ ) {
		const devoLight_t &lt = gi_lights[i];
		const gvec3 d = lt.origin - view;
		const float dist2 = gdot( d, d );
		const float reach = lt.radius * rangeScale * 1.5f;
		if ( dist2 > reach * reach )
			continue;
		vec3_t o = { lt.origin.x, lt.origin.y, lt.origin.z };
		if ( !R_inPVS( viewOrg, o, NULL ) )
			continue;
		cand.push_back( std::make_pair( lt.photons / ( dist2 + 128.0f * 128.0f ), i ) );
	}
	if ( (int)cand.size() > maxLights ) {
		std::nth_element( cand.begin(), cand.begin() + maxLights, cand.end(),
			[]( const std::pair<float, int> &a, const std::pair<float, int> &b ) { return a.first > b.first; } );
		cand.resize( maxLights );
	}

	const float intensity = Q_max( 0.0f, r_devoLights_intensity->value );
	for ( const auto &c : cand ) {
		const devoLight_t &lt = gi_lights[c.second];
		vec3_t o = { lt.origin.x, lt.origin.y, lt.origin.z };
		RE_AddLightToScene( o, lt.radius * rangeScale, lt.color.x * intensity, lt.color.y * intensity, lt.color.z * intensity );
	}
}

// console: devolights
void R_DevoLights_List_f( void )
{
	ri.Printf( PRINT_ALL, "DevoLights for %s: %d lights\n", gi_lightsMap[0] ? gi_lightsMap : "(no map)", (int)gi_lights.size() );
	if ( gi_hasSun )
		ri.Printf( PRINT_ALL, "  sun: dir ( %.2f %.2f %.2f ) colour ( %.2f %.2f %.2f ) intensity %.0f\n",
			gi_sunDir.x, gi_sunDir.y, gi_sunDir.z, gi_sunColor.x, gi_sunColor.y, gi_sunColor.z, gi_sunIntensity );
	else
		ri.Printf( PRINT_ALL, "  sun: none\n" );

	// strongest first
	std::vector<int> order( gi_lights.size() );
	for ( size_t i = 0; i < order.size(); i++ )
		order[i] = (int)i;
	std::sort( order.begin(), order.end(), []( int a, int b ) { return gi_lights[a].photons > gi_lights[b].photons; } );
	const int show = Q_min( 30, (int)order.size() );
	for ( int k = 0; k < show; k++ ) {
		const devoLight_t &lt = gi_lights[order[k]];
		ri.Printf( PRINT_ALL, "  %3d: ( %6.0f %6.0f %6.0f ) radius %5.0f colour ( %.2f %.2f %.2f )\n",
			order[k], lt.origin.x, lt.origin.y, lt.origin.z, lt.radius, lt.color.x, lt.color.y, lt.color.z );
	}
	if ( (int)order.size() > show )
		ri.Printf( PRINT_ALL, "  ... %d more\n", (int)order.size() - show );
}