/*
===========================================================================
Copyright (C) 2011 Andrei Drexler, Richard Allen, James Canete

This file is part of Reaction source code.

Reaction source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Reaction source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Reaction source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "tr_local.h"

void RB_ToneMap(FBO_t *hdrFbo, vec4i_t hdrBox, FBO_t *ldrFbo, vec4i_t ldrBox, int autoExposure)
{
	vec4i_t srcBox, dstBox;
	vec4_t color;
	static int lastFrameCount = 0;

	if (autoExposure)
	{
		if (lastFrameCount == 0 || tr.frameCount < lastFrameCount || tr.frameCount - lastFrameCount > 5)
		{
			// determine average log luminance
			FBO_t *srcFbo, *dstFbo, *tmp;
			int size = 256;

			lastFrameCount = tr.frameCount;

			VectorSet4(dstBox, 0, 0, size, size);

			FBO_Blit(hdrFbo, hdrBox, NULL, tr.textureScratchFbo[0], dstBox, &tr.calclevels4xShader[0], NULL, 0);

			srcFbo = tr.textureScratchFbo[0];
			dstFbo = tr.textureScratchFbo[1];

			// downscale to 1x1 texture
			while (size > 1)
			{
				VectorSet4(srcBox, 0, 0, size, size);
				size >>= 1;
				VectorSet4(dstBox, 0, 0, size, size);

				if (size == 1)
					dstFbo = tr.targetLevelsFbo;

				FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, &tr.calclevels4xShader[1], NULL, 0);

				tmp = srcFbo;
				srcFbo = dstFbo;
				dstFbo = tmp;
			}
		}

		// blend with old log luminance for gradual change
		VectorSet4(srcBox, 0, 0, 0, 0);

		color[0] =
		color[1] =
		color[2] = 1.0f;

		color[3] = Com_Clamp(0.0f, 1.0f, 0.001f * backEnd.refdef.frameTime);
		FBO_Blit(tr.targetLevelsFbo, srcBox, NULL, tr.calcLevelsFbo, NULL,  NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}

	// tonemap
	color[0] =
	color[1] =
	color[2] = powf(2.0f, r_cameraExposure->value); //exp2(r_cameraExposure->value);
	color[3] = 1.0f;

	if (autoExposure)
		GL_BindToTMU(tr.calcLevelsImage,  TB_LEVELSMAP);
	else
		GL_BindToTMU(tr.fixedLevelsImage, TB_LEVELSMAP);

	bool srgbTransform = tr.hdrLighting == qtrue;
	shaderProgram_t *shader = srgbTransform ? &tr.tonemapShader[1] : &tr.tonemapShader[0];

	if (r_smaa->integer == 1)
		GL_BindToTMU(tr.smaaBlendImage, 2);

	FBO_Bind(NULL);
	GL_SetViewportAndScissor(ldrBox[0], ldrBox[1], ldrBox[2], ldrBox[3]);
	GL_Cull(CT_TWO_SIDED);
	GL_State(GLS_DEPTHTEST_DISABLE);
	GLSL_BindProgram(shader);
	GL_BindToTMU(hdrFbo->colorImage[0], TB_COLORMAP);
	GLSL_SetUniformVec4(shader, UNIFORM_COLOR, color);
	GLSL_SetUniformVec2(shader, UNIFORM_AUTOEXPOSUREMINMAX, tr.refdef.autoExposureMinMax);
	GLSL_SetUniformVec3(shader, UNIFORM_TONEMINAVGMAXLINEAR, tr.refdef.toneMinAvgMaxLinear);
	RB_InstantTriangle();
}

/*
=============
RB_BokehBlur


Blurs a part of one framebuffer to another.

Framebuffers can be identical.
=============
*/
void RB_BokehBlur(FBO_t *src, vec4i_t srcBox, FBO_t *dst, vec4i_t dstBox, float blur)
{
//	vec4i_t srcBox, dstBox;
	vec4_t color;

	blur *= 10.0f;

	if (blur < 0.004f)
		return;

	// bokeh blur
	if (blur > 0.0f)
	{
		vec4i_t quarterBox;

		quarterBox[0] = 0;
		quarterBox[1] = tr.quarterFbo[0]->height;
		quarterBox[2] = tr.quarterFbo[0]->width;
		quarterBox[3] = -tr.quarterFbo[0]->height;

		// create a quarter texture
		//FBO_Blit(NULL, NULL, NULL, tr.quarterFbo[0], NULL, NULL, NULL, 0);
		FBO_FastBlit(src, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}

#ifndef HQ_BLUR
	if (blur > 1.0f)
	{
		// create a 1/16th texture
		//FBO_Blit(tr.quarterFbo[0], NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, NULL, 0);
		FBO_FastBlit(tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}
#endif

	if (blur > 0.0f && blur <= 1.0f)
	{
		// Crossfade original with quarter texture
		VectorSet4(color, 1, 1, 1, blur);

		FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}
#ifndef HQ_BLUR
	// ok blur, but can see some pixelization
	else if (blur > 1.0f && blur <= 2.0f)
	{
		// crossfade quarter texture with 1/16th texture
		FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, NULL, 0);

		VectorSet4(color, 1, 1, 1, blur - 1.0f);

		FBO_Blit(tr.textureScratchFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}
	else if (blur > 2.0f)
	{
		// blur 1/16th texture then replace
		int i;

		for (i = 0; i < 2; i++)
		{
			vec2_t blurTexScale;
			float subblur;

			subblur = ((blur - 2.0f) / 2.0f) / 3.0f * (float)(i + 1);

			blurTexScale[0] =
			blurTexScale[1] = subblur;

			color[0] =
			color[1] =
			color[2] = 0.5f;
			color[3] = 1.0f;

			if (i != 0)
				FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
			else
				FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, 0);
		}

		FBO_Blit(tr.textureScratchFbo[1], NULL, NULL, dst, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], NULL, 0);
	}
#else // higher quality blur, but slower
	else if (blur > 1.0f)
	{
		// blur quarter texture then replace
		int i;

		src = tr.quarterFbo[0];
		dst = tr.quarterFbo[1];

		VectorSet4(color, 0.5f, 0.5f, 0.5f, 1);

		for (i = 0; i < 2; i++)
		{
			vec2_t blurTexScale;
			float subblur;

			subblur = (blur - 1.0f) / 2.0f * (float)(i + 1);

			blurTexScale[0] =
			blurTexScale[1] = subblur;

			color[0] =
			color[1] =
			color[2] = 1.0f;
			if (i != 0)
				color[3] = 1.0f;
			else
				color[3] = 0.5f;

			FBO_Blit(tr.quarterFbo[0], NULL, blurTexScale, tr.quarterFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}

		FBO_Blit(tr.quarterFbo[1], NULL, NULL, dst, dstBox, &tr.textureColorShader, NULL, 0);
	}
#endif
}


static void RB_RadialBlur(FBO_t *srcFbo, FBO_t *dstFbo, int passes, float stretch, float x, float y, float w, float h, float xcenter, float ycenter, float alpha)
{
	vec4i_t srcBox, dstBox;
	vec4_t color;
	const float inc = 1.f / passes;
	const float mul = powf(stretch, inc);
	float scale;

	{
		vec2_t texScale;

		texScale[0] =
		texScale[1] = 1.0f;

		alpha *= inc;
		VectorSet4(color, alpha, alpha, alpha, 1.0f);

		VectorSet4(srcBox, 0, 0, srcFbo->width, srcFbo->height);
		VectorSet4(dstBox, x, y, w, h);
		FBO_Blit(srcFbo, srcBox, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, 0);

		--passes;
		scale = mul;
		while (passes > 0)
		{
			float iscale = 1.f / scale;
			float s0 = xcenter * (1.f - iscale);
			float t0 = (1.0f - ycenter) * (1.f - iscale);
			float s1 = iscale + s0;
			float t1 = iscale + t0;

			if (srcFbo)
			{
				srcBox[0] = s0 * srcFbo->width;
				srcBox[1] = t0 * srcFbo->height;
				srcBox[2] = (s1 - s0) * srcFbo->width;
				srcBox[3] = (t1 - t0) * srcFbo->height;
			}
			else
			{
				srcBox[0] = s0 * glConfig.vidWidth;
				srcBox[1] = t0 * glConfig.vidHeight;
				srcBox[2] = (s1 - s0) * glConfig.vidWidth;
				srcBox[3] = (t1 - t0) * glConfig.vidHeight;
			}

			FBO_Blit(srcFbo, srcBox, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

			scale *= mul;
			--passes;
		}
	}
}


static qboolean RB_UpdateSunFlareVis(void)
{
	GLuint sampleCount = 0;

	tr.sunFlareQueryIndex ^= 1;
	if (!tr.sunFlareQueryActive[tr.sunFlareQueryIndex])
		return qtrue;

	/* debug code */
	if (0)
	{
		int iter;
		for (iter=0 ; ; ++iter)
		{
			GLint available = 0;
			qglGetQueryObjectiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT_AVAILABLE, &available);
			if (available)
				break;
		}

		ri.Printf(PRINT_DEVELOPER, "Waited %d iterations\n", iter);
	}

	qglGetQueryObjectuiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT, &sampleCount);
	return (qboolean)(sampleCount > 0);
}

void RB_SunRays(FBO_t *srcFbo, vec4i_t srcBox, FBO_t *dstFbo, vec4i_t dstBox)
{
	vec4_t color;
	float dot;
	const float cutoff = 0.25f;
	qboolean colorize = qtrue;

//	float w, h, w2, h2;
	matrix_t mvp;
	vec4_t pos, hpos;

	dot = DotProduct(tr.sunDirection, backEnd.viewParms.ori.axis[0]);
	if (dot < cutoff)
		return;

	if (!RB_UpdateSunFlareVis())
		return;

	// From RB_DrawSun()
	{
		float dist;
		matrix_t trans, model;

		Matrix16Translation( backEnd.viewParms.ori.origin, trans );
		Matrix16Multiply( backEnd.viewParms.world.modelViewMatrix, trans, model );
		Matrix16Multiply(backEnd.viewParms.projectionMatrix, model, mvp);

		dist = backEnd.viewParms.zFar / 1.75;		// div sqrt(3)

		VectorScale( tr.sunDirection, dist, pos );
	}

	// project sun point
	//Matrix16Multiply(backEnd.viewParms.projectionMatrix, backEnd.viewParms.world.modelViewMatrix, mvp);
	Matrix16Transform(mvp, pos, hpos);

	// transform to UV coords
	hpos[3] = 0.5f / hpos[3];

	pos[0] = 0.5f + hpos[0] * hpos[3];
	pos[1] = 0.5f + hpos[1] * hpos[3];

	// initialize quarter buffers
	{
		float mul = 1.f;
		vec2_t texScale;
		vec4i_t rayBox, quarterBox;

		texScale[0] =
		texScale[1] = 1.0f;

		VectorSet4(color, mul, mul, mul, 1);

		if (srcFbo)
		{
			rayBox[0] = srcBox[0] * tr.sunRaysFbo->width  / srcFbo->width;
			rayBox[1] = srcBox[1] * tr.sunRaysFbo->height / srcFbo->height;
			rayBox[2] = srcBox[2] * tr.sunRaysFbo->width  / srcFbo->width;
			rayBox[3] = srcBox[3] * tr.sunRaysFbo->height / srcFbo->height;
		}
		else
		{
			rayBox[0] = srcBox[0] * tr.sunRaysFbo->width  / glConfig.vidWidth;
			rayBox[1] = srcBox[1] * tr.sunRaysFbo->height / glConfig.vidHeight;
			rayBox[2] = srcBox[2] * tr.sunRaysFbo->width  / glConfig.vidWidth;
			rayBox[3] = srcBox[3] * tr.sunRaysFbo->height / glConfig.vidHeight;
		}

		quarterBox[0] = 0;
		quarterBox[1] = tr.quarterFbo[0]->height;
		quarterBox[2] = tr.quarterFbo[0]->width;
		quarterBox[3] = -tr.quarterFbo[0]->height;

		// first, downsample the framebuffer
		if (colorize)
		{
			FBO_FastBlit(srcFbo, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			FBO_Blit(tr.sunRaysFbo, rayBox, NULL, tr.quarterFbo[0], quarterBox, NULL, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO);
		}
		else
		{
			FBO_FastBlit(tr.sunRaysFbo, rayBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
	}

	// radial blur passes, ping-ponging between the two quarter-size buffers
	{
		const float stretch_add = 2.f/3.f;
		float stretch = 1.f + stretch_add;
		int i;
		for (i=0; i<2; ++i)
		{
			RB_RadialBlur(tr.quarterFbo[i&1], tr.quarterFbo[(~i) & 1], 5, stretch, 0.f, 0.f, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height, pos[0], pos[1], 1.125f);
			stretch += stretch_add;
		}
	}

	// add result back on top of the main buffer
	{
		float mul = 1.f;
		vec2_t texScale;

		texScale[0] =
		texScale[1] = 1.0f;

		VectorSet4(color, mul, mul, mul, 1);

		FBO_Blit(tr.quarterFbo[0], NULL, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}
}

static void RB_BlurAxis(FBO_t *srcFbo, FBO_t *dstFbo, float strength, qboolean horizontal)
{
	float dx, dy;
	float xmul, ymul;
	float weights[3] = {
		0.227027027f,
		0.316216216f,
		0.070270270f,
	};
	float offsets[3] = {
		0.f,
		1.3846153846f,
		3.2307692308f,
	};

	xmul = horizontal;
	ymul = 1.f - xmul;

	xmul *= strength;
	ymul *= strength;

	{
		vec4i_t srcBox, dstBox;
		vec4_t color;
		vec2_t texScale;

		texScale[0] =
		texScale[1] = 1.0f;

		VectorSet4(color, weights[0], weights[0], weights[0], 1.0f);
		VectorSet4(srcBox, 0, 0, srcFbo->width, srcFbo->height);
		VectorSet4(dstBox, 0, 0, dstFbo->width, dstFbo->height);
		FBO_Blit(srcFbo, srcBox, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, 0 );

		VectorSet4(color, weights[1], weights[1], weights[1], 1.0f);
		dx = offsets[1] * xmul;
		dy = offsets[1] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

		VectorSet4(color, weights[2], weights[2], weights[2], 1.0f);
		dx = offsets[2] * xmul;
		dy = offsets[2] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, texScale, dstFbo, dstBox, &tr.textureColorShader[TEXCOLORDEF_USE_VERTICES], color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	}
}

void RB_HBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qtrue);
}

void RB_VBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qfalse);
}

void RB_GaussianBlur(FBO_t *srcFbo, FBO_t *intermediateFbo, FBO_t *dstFbo, float spread)
{
	// Blur X
	vec2_t scale;
	VectorSet2 (scale, spread, spread);

	FBO_Blit (srcFbo, NULL, scale, intermediateFbo, NULL, &tr.gaussianBlurShader[0], NULL, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO);

	// Blur Y
	FBO_Blit (intermediateFbo, NULL, scale, dstFbo, NULL, &tr.gaussianBlurShader[1], NULL, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO);
}

void RB_BloomDownscale(image_t *sourceImage, FBO_t *destFBO)
{
	vec2_t invTexRes = { 1.0f / sourceImage->width, 1.0f / sourceImage->height };

	FBO_Bind(destFBO);
	GL_State(GLS_DEPTHTEST_DISABLE);
	GL_SetViewportAndScissor(0, 0, destFBO->width, destFBO->height);
	qglClearBufferfv(GL_COLOR, 0, colorBlack);

	GLSL_BindProgram(&tr.dglowDownsample);
	GLSL_SetUniformVec2(&tr.dglowDownsample, UNIFORM_INVTEXRES, invTexRes);
	GL_BindToTMU(sourceImage, 0);

	// Draw fullscreen triangle
	qglDrawArrays(GL_TRIANGLES, 0, 3);
}

void RB_BloomDownscale(FBO_t *sourceFBO, FBO_t *destFBO)
{
	RB_BloomDownscale(sourceFBO->colorImage[0], destFBO);
}

void RB_BloomUpscale(FBO_t *sourceFBO, FBO_t *destFBO)
{
	image_t *sourceImage = sourceFBO->colorImage[0];
	vec2_t invTexRes = { 1.0f / sourceImage->width, 1.0f / sourceImage->height };

	FBO_Bind(destFBO);
	GL_State(GLS_DEPTHTEST_DISABLE);
	GL_SetViewportAndScissor(0, 0, destFBO->width, destFBO->height);
	qglClearBufferfv(GL_COLOR, 0, colorBlack);

	GLSL_BindProgram(&tr.dglowUpsample);
	GLSL_SetUniformVec2(&tr.dglowUpsample, UNIFORM_INVTEXRES, invTexRes);
	GL_BindToTMU(sourceImage, 0);

	// Draw fullscreen triangle
	qglDrawArrays(GL_TRIANGLES, 0, 3);
}
