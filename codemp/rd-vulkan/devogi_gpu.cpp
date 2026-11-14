/*
===========================================================================
DevoGI GPU ray tracer - Vulkan compute (see devogi_gpu.h)

A small, self-contained Vulkan program: it opens the Vulkan library itself,
picks a GPU (a dedicated one if there is one), creates its own device with a
compute queue, uploads the BVH and triangles once, and then traces batches of
rays with the compute shader in devogi_trace.comp.

Memory: the scene lives in GPU memory (copied there through a staging
buffer, so dedicated cards read it at full speed); rays go up and hits come
back through host-visible "streaming" buffers, a few million at a time.

Part of DevoJK by Devoted, licensed under GPLv2.
===========================================================================
*/

#include "devogi_gpu.h"

#define VK_NO_PROTOTYPES
#include "vulkan/vulkan_core.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	static HMODULE	dg_vkLib;
	static void *dg_lib_open( void ) { dg_vkLib = LoadLibraryA( "vulkan-1.dll" ); return dg_vkLib; }
	static void *dg_lib_sym( const char *name ) { return (void *)GetProcAddress( dg_vkLib, name ); }
	static void dg_lib_close( void ) { if ( dg_vkLib ) FreeLibrary( dg_vkLib ); dg_vkLib = NULL; }
#else
	#include <dlfcn.h>
	static void		*dg_vkLib;
	static void *dg_lib_open( void ) {
		dg_vkLib = dlopen( "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL );
		if ( !dg_vkLib ) dg_vkLib = dlopen( "libvulkan.so", RTLD_NOW | RTLD_LOCAL );
		return dg_vkLib;
	}
	static void *dg_lib_sym( const char *name ) { return dlsym( dg_vkLib, name ); }
	static void dg_lib_close( void ) { if ( dg_vkLib ) dlclose( dg_vkLib ); dg_vkLib = NULL; }
#endif

#include "devogi_trace_spv.h"	// the compiled compute shader

// ---- the Vulkan functions we use, loaded by hand ----
#define DG_VK_FUNCS \
	F( vkGetPhysicalDeviceProperties ) F( vkGetPhysicalDeviceQueueFamilyProperties ) \
	F( vkGetPhysicalDeviceMemoryProperties ) F( vkCreateDevice ) F( vkDestroyDevice ) \
	F( vkGetDeviceQueue ) F( vkCreateBuffer ) F( vkDestroyBuffer ) F( vkGetBufferMemoryRequirements ) \
	F( vkAllocateMemory ) F( vkFreeMemory ) F( vkBindBufferMemory ) F( vkMapMemory ) F( vkUnmapMemory ) \
	F( vkCreateShaderModule ) F( vkDestroyShaderModule ) F( vkCreateDescriptorSetLayout ) \
	F( vkDestroyDescriptorSetLayout ) F( vkCreatePipelineLayout ) F( vkDestroyPipelineLayout ) \
	F( vkCreateComputePipelines ) F( vkDestroyPipeline ) F( vkCreateDescriptorPool ) \
	F( vkDestroyDescriptorPool ) F( vkAllocateDescriptorSets ) F( vkUpdateDescriptorSets ) \
	F( vkCreateCommandPool ) F( vkDestroyCommandPool ) F( vkAllocateCommandBuffers ) \
	F( vkBeginCommandBuffer ) F( vkEndCommandBuffer ) F( vkResetCommandBuffer ) F( vkCmdBindPipeline ) \
	F( vkCmdBindDescriptorSets ) F( vkCmdPushConstants ) F( vkCmdDispatch ) F( vkCmdCopyBuffer ) \
	F( vkCmdPipelineBarrier ) F( vkQueueSubmit ) F( vkCreateFence ) F( vkDestroyFence ) \
	F( vkWaitForFences ) F( vkResetFences ) F( vkDestroyInstance ) F( vkEnumeratePhysicalDevices )

static PFN_vkGetInstanceProcAddr	dg_vkGetInstanceProcAddr;
static PFN_vkCreateInstance			dg_vkCreateInstance;
#define F( name ) static PFN_##name dg_##name;
DG_VK_FUNCS
#undef F

// ---- state ----
struct dgBuffer {
	VkBuffer		buffer = VK_NULL_HANDLE;
	VkDeviceMemory	memory = VK_NULL_HANDLE;
	VkDeviceSize	size = 0;
	void			*mapped = NULL;
};

static struct {
	bool					running;
	char					name[256];
	VkInstance				instance;
	VkPhysicalDevice		physical;
	VkDevice				device;
	VkQueue					queue;
	uint32_t				queueFamily;
	VkPhysicalDeviceMemoryProperties memProps;
	uint32_t				maxGroupsX;
	dgBuffer				nodes, tris, rays, hits;
	size_t					batch;		// rays per dispatch
	VkShaderModule			shader;
	VkDescriptorSetLayout	setLayout;
	VkPipelineLayout		pipeLayout;
	VkPipeline				pipeline;
	VkDescriptorPool		pool;
	VkDescriptorSet			set;
	VkCommandPool			cmdPool;
	VkCommandBuffer			cmd;
	VkFence					fence;
	// statistics for the log
	double					gpuSeconds, copySeconds;
	uint64_t				rayCount;
} dg;

static double dg_now( void ) {
	return std::chrono::duration<double>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

static void ( *dg_print )( const char *text );

static void dg_log( const char *fmt, const char *arg ) {
	if ( !dg_print )
		return;
	char buf[512];
	snprintf( buf, sizeof( buf ), fmt, arg );
	dg_print( buf );
}

static int dg_memory_type( uint32_t bits, VkMemoryPropertyFlags want ) {
	for ( uint32_t i = 0; i < dg.memProps.memoryTypeCount; i++ )
		if ( ( bits & ( 1u << i ) ) && ( dg.memProps.memoryTypes[i].propertyFlags & want ) == want )
			return (int)i;
	return -1;
}

/*
The bake's tree (8 floats per node, see devogi_gpu.h) in the shader's layout: one
entry per inner node holding both children's boxes, so a single read decides
where a ray goes next (see devogi_trace.comp).
*/
static std::vector<float> dg_gpu_nodes( const float *nodes, size_t numNodes ) {
	auto intAt = []( const float *f ) { int32_t v; memcpy( &v, f, 4 ); return v; };
	auto putInt = []( float *f, int32_t v ) { memcpy( f, &v, 4 ); };
	std::vector<int32_t> gpuIndex( numNodes, -1 );
	std::vector<size_t> order;

	if ( intAt( &nodes[7] ) > 0 ) {
		// the whole scene is one leaf: one node with that leaf on the left, nothing on the right
		std::vector<float> out( 16, 0.0f );
		memcpy( &out[0], &nodes[0], 3 * sizeof( float ) ); putInt( &out[3], intAt( &nodes[3] ) );
		memcpy( &out[4], &nodes[4], 3 * sizeof( float ) ); putInt( &out[7], intAt( &nodes[7] ) );
		out[8] = out[9] = out[10] = 1.0f; out[12] = out[13] = out[14] = -1.0f;	// an empty box
		return out;
	}
	gpuIndex[0] = 0;
	order.push_back( 0 );
	for ( size_t k = 0; k < order.size(); k++ ) {
		const int32_t left = intAt( &nodes[order[k] * 8 + 3] );
		for ( int c = left; c <= left + 1; c++ ) {
			if ( intAt( &nodes[(size_t)c * 8 + 7] ) == 0 && gpuIndex[c] < 0 ) {
				gpuIndex[c] = (int32_t)order.size();
				order.push_back( (size_t)c );
			}
		}
	}
	std::vector<float> out( order.size() * 16 );
	for ( size_t g = 0; g < order.size(); g++ ) {
		const int32_t left = intAt( &nodes[order[g] * 8 + 3] );
		for ( int side = 0; side < 2; side++ ) {
			const size_t c = (size_t)left + side;
			const float *src = &nodes[c * 8];
			float *dst = &out[g * 16 + side * 8];
			const int32_t count = intAt( &src[7] );
			memcpy( &dst[0], &src[0], 3 * sizeof( float ) );
			memcpy( &dst[4], &src[4], 3 * sizeof( float ) );
			putInt( &dst[3], count > 0 ? intAt( &src[3] ) : gpuIndex[c] );	// first triangle, or inner node
			putInt( &dst[7], count );
		}
	}
	return out;
}

static bool dg_create_buffer( dgBuffer &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, bool map ) {
	VkBufferCreateInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size = size;
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if ( dg_vkCreateBuffer( dg.device, &bi, NULL, &b.buffer ) != VK_SUCCESS )
		return false;
	VkMemoryRequirements req;
	dg_vkGetBufferMemoryRequirements( dg.device, b.buffer, &req );
	int type = dg_memory_type( req.memoryTypeBits, want );
	if ( type < 0 && ( want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ) )	// cached memory is a wish, not a must
		type = dg_memory_type( req.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT );
	if ( type < 0 )
		return false;
	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = (uint32_t)type;
	if ( dg_vkAllocateMemory( dg.device, &ai, NULL, &b.memory ) != VK_SUCCESS )
		return false;
	dg_vkBindBufferMemory( dg.device, b.buffer, b.memory, 0 );
	b.size = size;
	if ( map && dg_vkMapMemory( dg.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped ) != VK_SUCCESS )
		return false;
	return true;
}

static void dg_destroy_buffer( dgBuffer &b ) {
	if ( b.mapped ) dg_vkUnmapMemory( dg.device, b.memory );
	if ( b.buffer ) dg_vkDestroyBuffer( dg.device, b.buffer, NULL );
	if ( b.memory ) dg_vkFreeMemory( dg.device, b.memory, NULL );
	b = dgBuffer();
}

// records, submits and waits for one command buffer
template<typename REC>
static bool dg_submit( REC record ) {
	dg_vkResetCommandBuffer( dg.cmd, 0 );
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	dg_vkBeginCommandBuffer( dg.cmd, &bi );
	record( dg.cmd );
	dg_vkEndCommandBuffer( dg.cmd );
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &dg.cmd;
	dg_vkResetFences( dg.device, 1, &dg.fence );
	if ( dg_vkQueueSubmit( dg.queue, 1, &si, dg.fence ) != VK_SUCCESS )
		return false;
	return dg_vkWaitForFences( dg.device, 1, &dg.fence, VK_TRUE, 60ull * 1000000000ull ) == VK_SUCCESS;
}

// copies data into a GPU-only buffer through a temporary staging buffer
static bool dg_upload( dgBuffer &dst, const void *data, VkDeviceSize size ) {
	if ( !dg_create_buffer( dst, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false ) )
		return false;
	dgBuffer staging;
	if ( !dg_create_buffer( staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true ) )
		return false;
	memcpy( staging.mapped, data, (size_t)size );
	const bool ok = dg_submit( [&]( VkCommandBuffer cmd ) {
		VkBufferCopy region = { 0, 0, size };
		dg_vkCmdCopyBuffer( cmd, staging.buffer, dst.buffer, 1, &region );
	} );
	dg_destroy_buffer( staging );
	return ok;
}

static bool dg_init_device( void ) {
	if ( !dg_lib_open() )
		return false;
	dg_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dg_lib_sym( "vkGetInstanceProcAddr" );
	if ( !dg_vkGetInstanceProcAddr )
		return false;
	dg_vkCreateInstance = (PFN_vkCreateInstance)dg_vkGetInstanceProcAddr( NULL, "vkCreateInstance" );
	if ( !dg_vkCreateInstance )
		return false;

	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "DevoGI bake";
	app.apiVersion = VK_API_VERSION_1_0;
	VkInstanceCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	if ( dg_vkCreateInstance( &ici, NULL, &dg.instance ) != VK_SUCCESS )
		return false;

#define F( name ) dg_##name = (PFN_##name)dg_vkGetInstanceProcAddr( dg.instance, #name ); if ( !dg_##name ) return false;
	DG_VK_FUNCS
#undef F

	// pick a GPU: a dedicated card if there is one, otherwise the first one
	uint32_t count = 0;
	dg_vkEnumeratePhysicalDevices( dg.instance, &count, NULL );
	if ( !count )
		return false;
	std::vector<VkPhysicalDevice> devs( count );
	dg_vkEnumeratePhysicalDevices( dg.instance, &count, devs.data() );
	dg.physical = devs[0];
	for ( VkPhysicalDevice d : devs ) {
		VkPhysicalDeviceProperties p;
		dg_vkGetPhysicalDeviceProperties( d, &p );
		if ( p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) { dg.physical = d; break; }
	}
	VkPhysicalDeviceProperties props;
	dg_vkGetPhysicalDeviceProperties( dg.physical, &props );
	snprintf( dg.name, sizeof( dg.name ), "%s", props.deviceName );
	dg.maxGroupsX = props.limits.maxComputeWorkGroupCount[0];
	dg_vkGetPhysicalDeviceMemoryProperties( dg.physical, &dg.memProps );

	// a queue that can run compute shaders
	uint32_t nq = 0;
	dg_vkGetPhysicalDeviceQueueFamilyProperties( dg.physical, &nq, NULL );
	std::vector<VkQueueFamilyProperties> qf( nq );
	dg_vkGetPhysicalDeviceQueueFamilyProperties( dg.physical, &nq, qf.data() );
	dg.queueFamily = UINT32_MAX;
	for ( uint32_t i = 0; i < nq; i++ )
		if ( qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT ) { dg.queueFamily = i; break; }
	if ( dg.queueFamily == UINT32_MAX )
		return false;

	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = dg.queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	if ( dg_vkCreateDevice( dg.physical, &dci, NULL, &dg.device ) != VK_SUCCESS )
		return false;
	dg_vkGetDeviceQueue( dg.device, dg.queueFamily, 0, &dg.queue );

	VkCommandPoolCreateInfo cpi = {};
	cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = dg.queueFamily;
	if ( dg_vkCreateCommandPool( dg.device, &cpi, NULL, &dg.cmdPool ) != VK_SUCCESS )
		return false;
	VkCommandBufferAllocateInfo cai = {};
	cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cai.commandPool = dg.cmdPool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	if ( dg_vkAllocateCommandBuffers( dg.device, &cai, &dg.cmd ) != VK_SUCCESS )
		return false;
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	return dg_vkCreateFence( dg.device, &fci, NULL, &dg.fence ) == VK_SUCCESS;
}

static bool dg_init_pipeline( void ) {
	VkShaderModuleCreateInfo smi = {};
	smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smi.codeSize = sizeof( devogi_trace_spv );
	smi.pCode = devogi_trace_spv;
	if ( dg_vkCreateShaderModule( dg.device, &smi, NULL, &dg.shader ) != VK_SUCCESS )
		return false;

	VkDescriptorSetLayoutBinding binds[4] = {};
	for ( uint32_t i = 0; i < 4; i++ ) {
		binds[i].binding = i;
		binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		binds[i].descriptorCount = 1;
		binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dli = {};
	dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dli.bindingCount = 4;
	dli.pBindings = binds;
	if ( dg_vkCreateDescriptorSetLayout( dg.device, &dli, NULL, &dg.setLayout ) != VK_SUCCESS )
		return false;

	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
	VkPipelineLayoutCreateInfo pli = {};
	pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &dg.setLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if ( dg_vkCreatePipelineLayout( dg.device, &pli, NULL, &dg.pipeLayout ) != VK_SUCCESS )
		return false;

	VkComputePipelineCreateInfo cpi = {};
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = dg.shader;
	cpi.stage.pName = "main";
	cpi.layout = dg.pipeLayout;
	if ( dg_vkCreateComputePipelines( dg.device, VK_NULL_HANDLE, 1, &cpi, NULL, &dg.pipeline ) != VK_SUCCESS )
		return false;

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
	VkDescriptorPoolCreateInfo dpi = {};
	dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpi.maxSets = 1;
	dpi.poolSizeCount = 1;
	dpi.pPoolSizes = &ps;
	if ( dg_vkCreateDescriptorPool( dg.device, &dpi, NULL, &dg.pool ) != VK_SUCCESS )
		return false;
	VkDescriptorSetAllocateInfo dai = {};
	dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dai.descriptorPool = dg.pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &dg.setLayout;
	if ( dg_vkAllocateDescriptorSets( dg.device, &dai, &dg.set ) != VK_SUCCESS )
		return false;

	const dgBuffer *bufs[4] = { &dg.nodes, &dg.tris, &dg.rays, &dg.hits };
	VkDescriptorBufferInfo infos[4];
	VkWriteDescriptorSet writes[4] = {};
	for ( int i = 0; i < 4; i++ ) {
		infos[i] = { bufs[i]->buffer, 0, VK_WHOLE_SIZE };
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = dg.set;
		writes[i].dstBinding = (uint32_t)i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	dg_vkUpdateDescriptorSets( dg.device, 4, writes, 0, NULL );
	return true;
}

bool DG_GpuBegin( const float *nodes, size_t numNodes, const float *tris, size_t numTris, void ( *print )( const char *text ) )
{
	DG_GpuEnd();
	dg_print = print;
	if ( !numNodes || !numTris )
		return false;
	if ( !dg_init_device() ) {
		dg_log( "DevoGI: no Vulkan GPU for the bake%s, using the CPU\n", "" );
		DG_GpuEnd();
		return false;
	}

	// the scene, in GPU memory; rays and hits through memory both sides can see
	dg.batch = 2u * 1024u * 1024u;
	const VkMemoryPropertyFlags stream = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	// the hits are read back by the CPU: cached memory makes that many times faster
	const VkMemoryPropertyFlags readback = stream | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	const std::vector<float> gpuNodes = dg_gpu_nodes( nodes, numNodes );
	if ( !dg_upload( dg.nodes, gpuNodes.data(), gpuNodes.size() * sizeof( float ) )
		|| !dg_upload( dg.tris, tris, numTris * 12 * sizeof( float ) )
		|| !dg_create_buffer( dg.rays, dg.batch * sizeof( dgGpuRay ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, stream, true )
		|| !dg_create_buffer( dg.hits, dg.batch * sizeof( dgGpuHit ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, readback, true )
		|| !dg_init_pipeline() ) {
		dg_log( "DevoGI: the GPU (%s) could not be set up for the bake, using the CPU\n", dg.name );
		DG_GpuEnd();
		return false;
	}
	dg.running = true;
	dg_log( "DevoGI: tracing rays on the GPU: %s\n", dg.name );
	return true;
}

bool DG_GpuTrace( const dgGpuRay *rays, dgGpuHit *hits, size_t count, bool anyHit )
{
	if ( !dg.running )
		return false;
	for ( size_t first = 0; first < count; first += dg.batch ) {
		const uint32_t n = (uint32_t)( count - first < dg.batch ? count - first : dg.batch );
		double t0 = dg_now();
		memcpy( dg.rays.mapped, rays + first, n * sizeof( dgGpuRay ) );
		double t1 = dg_now();

		const uint32_t groups = ( n + 63u ) / 64u;
		const uint32_t gx = groups < 32768u ? groups : 32768u;
		const uint32_t gy = ( groups + gx - 1u ) / gx;
		const uint32_t push[2] = { n, anyHit ? 1u : 0u };
		const bool ok = dg_submit( [&]( VkCommandBuffer cmd ) {
			dg_vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dg.pipeline );
			dg_vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dg.pipeLayout, 0, 1, &dg.set, 0, NULL );
			dg_vkCmdPushConstants( cmd, dg.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), push );
			dg_vkCmdDispatch( cmd, gx, gy, 1 );
			// make the hits visible to the CPU
			VkMemoryBarrier mb = {};
			mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			dg_vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL );
		} );
		if ( !ok ) {
			dg_log( "DevoGI: the GPU stopped responding during the bake (%s), finishing on the CPU\n", dg.name );
			DG_GpuEnd();
			return false;
		}
		double t2 = dg_now();
		memcpy( hits + first, dg.hits.mapped, n * sizeof( dgGpuHit ) );
		double t3 = dg_now();
		dg.gpuSeconds += t2 - t1;
		dg.copySeconds += ( t1 - t0 ) + ( t3 - t2 );
		dg.rayCount += n;
	}
	return true;
}

void DG_GpuEnd( void )
{
	if ( dg.running && dg.rayCount && dg_print ) {
		char buf[256];
		snprintf( buf, sizeof( buf ), "DevoGI: GPU traced %.0f million rays in %.1fs (%.0f million/s), copying took %.1fs\n",
			dg.rayCount / 1e6, dg.gpuSeconds, dg.rayCount / 1e6 / ( dg.gpuSeconds > 0 ? dg.gpuSeconds : 1 ), dg.copySeconds );
		dg_print( buf );
	}
	if ( dg.device ) {
		dg_destroy_buffer( dg.nodes );
		dg_destroy_buffer( dg.tris );
		dg_destroy_buffer( dg.rays );
		dg_destroy_buffer( dg.hits );
		if ( dg.fence ) dg_vkDestroyFence( dg.device, dg.fence, NULL );
		if ( dg.cmdPool ) dg_vkDestroyCommandPool( dg.device, dg.cmdPool, NULL );
		if ( dg.pool ) dg_vkDestroyDescriptorPool( dg.device, dg.pool, NULL );
		if ( dg.pipeline ) dg_vkDestroyPipeline( dg.device, dg.pipeline, NULL );
		if ( dg.pipeLayout ) dg_vkDestroyPipelineLayout( dg.device, dg.pipeLayout, NULL );
		if ( dg.setLayout ) dg_vkDestroyDescriptorSetLayout( dg.device, dg.setLayout, NULL );
		if ( dg.shader ) dg_vkDestroyShaderModule( dg.device, dg.shader, NULL );
		dg_vkDestroyDevice( dg.device, NULL );
	}
	if ( dg.instance && dg_vkDestroyInstance )
		dg_vkDestroyInstance( dg.instance, NULL );
	dg_lib_close();
	memset( &dg, 0, sizeof( dg ) );
}

const char *DG_GpuName( void )
{
	return dg.running ? dg.name : "";
}
