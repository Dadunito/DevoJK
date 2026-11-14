/*
===========================================================================
DevoGI GPU ray tracer - interface

The bake (devogi_core.cpp) spends almost all of its time tracing rays. When a
Vulkan GPU is available, it can hand whole batches of rays to this module,
which traces them in a compute shader (devogi_trace.comp) through the same
BVH and with the same triangle test, and hands the hits back. Everything
else - making the rays and turning hits into light - stays in the core, so
the GPU and the CPU produce the same lighting (up to tiny float differences).

No ray-tracing hardware is needed: any Vulkan 1.0 GPU works, integrated ones
included. The module brings its own Vulkan device, so it works the same in
the game (next to the renderer's device) and in the DevoQ3Map2 compiler.
Without Vulkan, DG_GpuBegin simply returns false and the CPU does the work.

Like devogi_core.{h,cpp}, these files are shared with DevoQ3Map2 (src/).

Part of DevoJK by Devoted, licensed under GPLv2.
===========================================================================
*/

#pragma once

#include <cstddef>
#include <cstdint>

// one ray, 32 bytes: matches the shader's rays buffer
struct dgGpuRay {
	float	origin[3];
	float	tmin;		// hits closer than this are ignored
	float	dir[3];		// unit length
	float	tmax;		// hits further than this are ignored
};

// one result, 16 bytes: matches the shader's hits buffer
struct dgGpuHit {
	float	t;			// distance along the ray (tmax when nothing was hit)
	float	u, v;		// where on the triangle (barycentric)
	int32_t	tri;		// triangle index, -1 when nothing was hit
};

/*
Uploads the scene and starts the GPU tracer; false when no usable Vulkan GPU is
found (the bake then stays on the CPU).
  nodes: 8 floats per BVH node: mins.xyz, first child / first triangle (int bits),
         maxs.xyz, triangle count (int bits, 0 = inner node)
  tris:  12 floats per triangle in BVH leaf order: v0.xyz, original index (int bits),
         e1.xyz, blocks rays (1/0), e2.xyz, unused
print receives one line of text for the log (may be NULL).
*/
bool	DG_GpuBegin( const float *nodes, size_t numNodes, const float *tris, size_t numTris,
			void ( *print )( const char *text ) );

// traces count rays (any number: big batches are split); false on a GPU error
bool	DG_GpuTrace( const dgGpuRay *rays, dgGpuHit *hits, size_t count, bool anyHit );

// frees the GPU objects (safe to call when DG_GpuBegin failed)
void	DG_GpuEnd( void );

// the GPU's name, for messages ("" when not running)
const char *DG_GpuName( void );
