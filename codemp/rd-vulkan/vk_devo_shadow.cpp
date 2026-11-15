/*
===========================================================================
DevoJK - DevoGI sun shadows (real-time)

Part of DevoGI's real-time lighting (r_devoGI_live 1). Once per frame the
scene is drawn a second time, from the sun's direction, into a depth-only
image: the "shadow map". It stores, for every spot of a square area around
the player, how far the sunlight travels before it hits something. There are
two "cascades" side by side in one image: a sharp one close to the player
and a coarser one that covers the whole map. When the
lighting shader later lights a pixel with the sun, it looks that pixel up in
the shadow map: if something was in front of it, it is in shadow. Because
the map is redrawn every frame, players, weapons, doors and everything else
that moves cast shadows too.

Below the two cascades the same image holds small square tiles for the
near-lamp shadows: the lights right by the player (lamps, sabers, blaster bolts)
look at whatever moves around them - one 90 degree view per side of a cube,
only the sides with something in them - so players, weapons and doors throw
shadows from them too. The level itself is not drawn there: its lamp shadows
are baked.

This file owns the GPU side: the depth image, its render pass and
framebuffer, and the comparison sampler the lighting shader reads it with.
tr_devogi.cpp decides where the sun's camera goes (R_DevoGI_SunShadowView)
and fills in the matrix the shader needs (R_DevoGI_FillLights).

The image is bound to every shader at binding VK_DESC_UNIFORM_DEVO_SHADOW_BINDING
of the uniform descriptor set (see vk_create_descriptor_layout and
vk_update_uniform_descriptor).

This file is part of DevoJK by Devoted, licensed under GPLv2.
===========================================================================
*/

#include "tr_local.h"

vkDevoShadow_t vk_devoShadow;

static uint32_t vk_devo_shadow_size( void )
{
	// read straight from the cvar: the GPU objects are made before the renderer's
	// own cvar pointers are guaranteed to exist
	int size = ri.Cvar_VariableIntegerValue( "r_devoGI_sunShadowSize" );
	if ( size <= 0 )
		size = 2048;
	size = Com_Clampi( 512, 4096, size );
	return (uint32_t)size;
}

// near-lamp shadow tiles: pixels per side (r_devoGI_lampShadowSize)
static uint32_t vk_devo_lamp_tile_size( void )
{
	int size = ri.Cvar_VariableIntegerValue( "r_devoGI_lampShadowSize" );
	if ( size <= 0 )
		size = 512;
	return (uint32_t)Com_Clampi( 128, 1024, size );
}

// the depth-only render pass: clears the depth, keeps it for the lighting shader to read
static void vk_devo_shadow_create_render_pass( void )
{
	VkAttachmentDescription attachment;
	Com_Memset( &attachment, 0, sizeof( attachment ) );
	attachment.format			= vk.depth_format;
	attachment.samples			= VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp			= VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp			= VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp	= VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.stencilStoreOp	= VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout	= VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout		= VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

	VkAttachmentReference depthRef;
	depthRef.attachment = 0;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass;
	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint		= VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount	= 0;		// depth only: no colour is written
	subpass.pDepthStencilAttachment	= &depthRef;

	// wait for last frame's lighting to finish reading the map before writing it again,
	// and make this frame's lighting wait until the map is written
	VkSubpassDependency deps[2];
	Com_Memset( deps, 0, sizeof( deps ) );
	deps[0].srcSubpass		= VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass		= 0;
	deps[0].srcStageMask	= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[0].dstStageMask	= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[0].srcAccessMask	= VK_ACCESS_SHADER_READ_BIT;
	deps[0].dstAccessMask	= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[1].srcSubpass		= 0;
	deps[1].dstSubpass		= VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask	= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[1].dstStageMask	= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[1].srcAccessMask	= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[1].dstAccessMask	= VK_ACCESS_SHADER_READ_BIT;

	VkRenderPassCreateInfo desc;
	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType				= VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	desc.attachmentCount	= 1;
	desc.pAttachments		= &attachment;
	desc.subpassCount		= 1;
	desc.pSubpasses			= &subpass;
	desc.dependencyCount	= 2;
	desc.pDependencies		= deps;
	VK_CHECK( qvkCreateRenderPass( vk.device, &desc, NULL, &vk_devoShadow.render_pass ) );
	VK_SET_OBJECT_NAME( vk_devoShadow.render_pass, "render pass - devogi sun shadow", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
}

void vk_devo_shadow_create( void )
{
	Com_Memset( &vk_devoShadow, 0, sizeof( vk_devoShadow ) );
	vk_devoShadow.size = vk_devo_shadow_size();
	const uint32_t size = vk_devoShadow.size;
	// the cascades side by side on top, then enough rows of near-lamp tiles for
	// DEVO_LAMP_SHADOW_MAX lights with all six cube sides each
	vk_devoShadow.width = size * 2;
	vk_devoShadow.lampTile = Q_min( vk_devo_lamp_tile_size(), vk_devoShadow.width );
	vk_devoShadow.lampTilesPerRow = vk_devoShadow.width / vk_devoShadow.lampTile;
	const uint32_t tiles = DEVO_LAMP_SHADOW_MAX * 6;
	const uint32_t rows = ( tiles + vk_devoShadow.lampTilesPerRow - 1 ) / vk_devoShadow.lampTilesPerRow;
	vk_devoShadow.height = size + rows * vk_devoShadow.lampTile;

	// ---- the depth image ----
	VkImageCreateInfo image;
	Com_Memset( &image, 0, sizeof( image ) );
	image.sType			= VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType		= VK_IMAGE_TYPE_2D;
	image.format		= vk.depth_format;
	image.extent.width	= vk_devoShadow.width;	// cascade 1 on the left, cascade 2 on the right
	image.extent.height	= vk_devoShadow.height;	// and the near-lamp tiles below them
	image.extent.depth	= 1;
	image.mipLevels		= 1;
	image.arrayLayers	= 1;
	image.samples		= VK_SAMPLE_COUNT_1_BIT;
	image.tiling		= VK_IMAGE_TILING_OPTIMAL;
	image.usage			= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout	= VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK( qvkCreateImage( vk.device, &image, NULL, &vk_devoShadow.image ) );

	VkMemoryRequirements reqs;
	qvkGetImageMemoryRequirements( vk.device, vk_devoShadow.image, &reqs );
	VkMemoryAllocateInfo alloc;
	Com_Memset( &alloc, 0, sizeof( alloc ) );
	alloc.sType				= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize	= reqs.size;
	alloc.memoryTypeIndex	= vk_find_memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CHECK( qvkAllocateMemory( vk.device, &alloc, NULL, &vk_devoShadow.memory ) );
	VK_CHECK( qvkBindImageMemory( vk.device, vk_devoShadow.image, vk_devoShadow.memory, 0 ) );
	VK_SET_OBJECT_NAME( vk_devoShadow.image, "devogi sun shadow map", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );

	// two views: depth + stencil for drawing into, depth only for the shader to read
	const bool hasStencil = glConfig.stencilBits > 0;
	VkImageViewCreateInfo view;
	Com_Memset( &view, 0, sizeof( view ) );
	view.sType							= VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image							= vk_devoShadow.image;
	view.viewType						= VK_IMAGE_VIEW_TYPE_2D;
	view.format							= vk.depth_format;
	view.components.r = view.components.g = view.components.b = view.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.subresourceRange.aspectMask	= VK_IMAGE_ASPECT_DEPTH_BIT | ( hasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0 );
	view.subresourceRange.levelCount	= 1;
	view.subresourceRange.layerCount	= 1;
	VK_CHECK( qvkCreateImageView( vk.device, &view, NULL, &vk_devoShadow.attachment_view ) );
	view.subresourceRange.aspectMask	= VK_IMAGE_ASPECT_DEPTH_BIT;
	VK_CHECK( qvkCreateImageView( vk.device, &view, NULL, &vk_devoShadow.sampler_view ) );

	// ---- render pass and framebuffer ----
	vk_devo_shadow_create_render_pass();

	VkFramebufferCreateInfo fb;
	Com_Memset( &fb, 0, sizeof( fb ) );
	fb.sType			= VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb.renderPass		= vk_devoShadow.render_pass;
	fb.attachmentCount	= 1;
	fb.pAttachments		= &vk_devoShadow.attachment_view;
	fb.width			= vk_devoShadow.width;
	fb.height			= vk_devoShadow.height;
	fb.layers			= 1;
	VK_CHECK( qvkCreateFramebuffer( vk.device, &fb, NULL, &vk_devoShadow.framebuffer ) );

	// ---- the sampler: compares depths for us, with free 2x2 smoothing (hardware PCF) ----
	VkSamplerCreateInfo sampler;
	Com_Memset( &sampler, 0, sizeof( sampler ) );
	sampler.sType			= VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter		= VK_FILTER_LINEAR;
	sampler.minFilter		= VK_FILTER_LINEAR;
	sampler.mipmapMode		= VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU	= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sampler.addressModeV	= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sampler.addressModeW	= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sampler.borderColor		= VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;	// outside the map: lit
	sampler.compareEnable	= VK_TRUE;
	sampler.compareOp		= VK_COMPARE_OP_LESS_OR_EQUAL;			// lit if nothing is nearer to the sun
	sampler.maxLod			= 0.0f;
	VK_CHECK( qvkCreateSampler( vk.device, &sampler, NULL, &vk_devoShadow.sampler ) );

	// the shader may read the map before it was ever drawn: give it a valid, empty-ish layout
	// (a plain barrier: the renderer's layout helper doesn't know the depth read-only layout)
	VkImageMemoryBarrier barrier;
	Com_Memset( &barrier, 0, sizeof( barrier ) );
	barrier.sType							= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask					= 0;
	barrier.dstAccessMask					= VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout						= VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout						= VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	barrier.srcQueueFamilyIndex				= VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex				= VK_QUEUE_FAMILY_IGNORED;
	barrier.image							= vk_devoShadow.image;
	barrier.subresourceRange.aspectMask		= VK_IMAGE_ASPECT_DEPTH_BIT | ( hasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0 );
	barrier.subresourceRange.levelCount		= 1;
	barrier.subresourceRange.layerCount		= 1;

	VkCommandBuffer cmd = vk_begin_command_buffer();
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier );
	vk_end_command_buffer( cmd, __func__ );

	vk_devoShadow.active = qtrue;
	ri.Printf( PRINT_DEVELOPER, "DevoGI: sun shadow map %ux%u, near-lamp tiles %u\n", size, size, vk_devoShadow.lampTile );
}

void vk_devo_shadow_destroy( void )
{
	if ( !vk_devoShadow.active )
		return;
	qvkDestroySampler( vk.device, vk_devoShadow.sampler, NULL );
	qvkDestroyFramebuffer( vk.device, vk_devoShadow.framebuffer, NULL );
	qvkDestroyRenderPass( vk.device, vk_devoShadow.render_pass, NULL );
	qvkDestroyImageView( vk.device, vk_devoShadow.sampler_view, NULL );
	qvkDestroyImageView( vk.device, vk_devoShadow.attachment_view, NULL );
	qvkDestroyImage( vk.device, vk_devoShadow.image, NULL );
	qvkFreeMemory( vk.device, vk_devoShadow.memory, NULL );
	Com_Memset( &vk_devoShadow, 0, sizeof( vk_devoShadow ) );
}

// the shader's view of the map (written into every uniform descriptor set)
void vk_devo_shadow_write_descriptor( VkDescriptorSet set )
{
	if ( !vk_devoShadow.active )
		return;
	VkDescriptorImageInfo info;
	info.sampler		= vk_devoShadow.sampler;
	info.imageView		= vk_devoShadow.sampler_view;
	info.imageLayout	= VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write;
	Com_Memset( &write, 0, sizeof( write ) );
	write.sType				= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet			= set;
	write.dstBinding		= VK_DESC_UNIFORM_DEVO_SHADOW_BINDING;
	write.descriptorCount	= 1;
	write.descriptorType	= VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo		= &info;
	qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
}

// starts drawing the sun's view into the shadow map
void vk_devo_shadow_begin_pass( void )
{
	vk.renderPassIndex = RENDER_PASS_DEVO_SHADOW;
	vk.renderWidth = vk_devoShadow.width;
	vk.renderHeight = vk_devoShadow.height;
	vk.renderScaleX = vk.renderScaleY = 1.0f;

	VkClearValue clear;
	clear.depthStencil.depth = 1.0f;	// far away: nothing in the way yet
	clear.depthStencil.stencil = 0;

	VkRenderPassBeginInfo begin;
	Com_Memset( &begin, 0, sizeof( begin ) );
	begin.sType						= VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass				= vk_devoShadow.render_pass;
	begin.framebuffer				= vk_devoShadow.framebuffer;
	begin.renderArea.extent.width	= vk_devoShadow.width;
	begin.renderArea.extent.height	= vk_devoShadow.height;
	begin.clearValueCount			= 1;
	begin.pClearValues				= &clear;
	qvkCmdBeginRenderPass( vk.cmd->command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE );

	// the viewport and scissor are set again for the new size on the next draw
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
}
