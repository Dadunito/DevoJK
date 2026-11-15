#ifndef SHADER_PBR_GLSL
#define SHADER_PBR_GLSL

#if defined(PER_PIXEL_LIGHTING)
	float WrapLambert( in float NL, in float w )
	{
		return clamp((NL + w) / pow(1.0 + w, 2.0), 0.0, 1.0);
	}

	vec3 Diffuse_Lambert(in vec3 DiffuseColor)
	{
		return DiffuseColor * (1.0 / M_PI);
	}

	vec3 CalcNormal( in vec3 vertexNormal, in vec4 vertexTangent, in vec2 frag_tex_coord )
	{
		if ( normal_texture_set > -1 ) {
			vec3 biTangent = vertexTangent.w * cross(vertexNormal, vertexTangent.xyz);
			vec3 n = texture(normal_texture, frag_tex_coord).agb - vec3(0.5);

			n.xy *= u_global.normalScale.xy;
			n.z = sqrt(clamp((0.25 - n.x * n.x) - n.y * n.y, 0.0, 1.0));
			n = n.x * vertexTangent.xyz + n.y * biTangent + n.z * vertexNormal;

			// DevoJK: degenerate geometry (zero-length normal or tangent) must not give a
			// NaN normal, which draws the surface black: fall back to the vertex normal
			if ( dot( n, n ) > 1e-12 )
				return normalize(n);
		}

		if ( dot( vertexNormal, vertexNormal ) > 1e-12 )
			return normalize(vertexNormal);
		return vec3( 0.0, 0.0, 1.0 );
	}

	vec3 CalcDiffuse( in vec3 diffuse, in float NE, in float NL,
		in float LH, in float roughness )
	{
		return Diffuse_Lambert(diffuse);
	}

	vec3 F_Schlick( in vec3 SpecularColor, in float VH )
	{
		float Fc = pow(1 - VH, 5);
		return clamp(50.0 * SpecularColor.g, 0.0, 1.0) * Fc + (1 - Fc) * SpecularColor; //hacky way to decide if reflectivity is too low (< 2%)
	}

	float D_GGX( in float NH, in float a )
	{
		float a2 = a * a;
		float d = (NH * a2 - NH) * NH + 1;
		return a2 / (M_PI * d * d);
	}

	// Appoximation of joint Smith term for GGX
	// [Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"]
	float V_SmithJointApprox( in float a, in float NV, in float NL )
	{
		float Vis_SmithV = NL * (NV * (1 - a) + a);
		float Vis_SmithL = NV * (NL * (1 - a) + a);
		return 0.5 * (1.0 / (Vis_SmithV + Vis_SmithL));
	}

	vec3 CalcSpecular( in vec3 specular, in float NH, in float NL,
		in float NE, float LH, in float VH, in float roughness )
	{
		vec3  F = F_Schlick(specular, VH);
		float D = D_GGX(NH, roughness);
		float V = V_SmithJointApprox(roughness, NE, NL);

		return D * F * V;
	}

	vec3 CalcIBLContribution( in float roughness, in vec3 N, in vec3 E,
		in float NE, in vec3 specular )
	{
		vec3 R = reflect(-E, N);
		R.y *= -1.0f;
	
		vec3 cubeLightColor = textureLod(env_texture, R, roughness * 6).rgb * 1.0;
		vec2 EnvBRDF = texture(brdflut_texture, vec2(NE, 1.0 - roughness)).rg;

		return cubeLightColor * (specular.rgb * EnvBRDF.x + EnvBRDF.y);
	}

	#ifdef VK_DLIGHT_GPU
		float CalcLightAttenuation(float normDist)
		{
			// zero light at 1.0, approximating q3 style
			float attenuation = 0.5 * normDist - 0.5;
			return clamp(attenuation, 0.0, 1.0);
		}

		// DevoGI: the same light a lamp gave this spot in the bake:
		// strength * cos(at the lamp) / distance^2, softened right at the lamp by its
		// area and faded out smoothly before the lamp's reach
		float CalcDevoLampAttenuation( in vkUniformLightEntry_t light, in vec3 L, in float sqrLightDist, in float radiusSqr )
		{
			float cosE = 1.0;
			// origin.w = type + 10 * ( mask channel + 1 ): type 1 is a glowing surface
			if ( mod( light.origin.w, 10.0 ) < 1.5 ) {
				cosE = -dot( light.normal.xyz, L );
				if ( cosE <= 0.0 )
					return 0.0;		// behind the glowing surface
			}
			float fade = 1.0 - ( sqrLightDist * sqrLightDist ) / ( radiusSqr * radiusSqr );
			return cosE * fade * fade / ( sqrLightDist + light.normal.w );
		}

		/*
		DevoGI real-time sun shadow: how much sun reaches this spot, 0..1, from the two
		shadow maps ("cascades") drawn from the sun this frame: a sharp one around the
		player and a coarse one across the map, side by side in one image. The lookup is
		pushed a little off the surface and towards the sun, so a surface never shadows
		itself ("acne"). 16 lookups over 4x4 shadow-map pixels (each already smoothed
		2x2 by the hardware) give soft edges.
		*/

		// one cascade: -1 when the spot is outside it, otherwise the light 0..1, and
		// 'edge' tells how close to the cascade's border it is (0 centre .. 1 border)
		float DevoSunShadowCascade( in mat4 mvp, in float cascade, in vec3 p, out float edge )
		{
			edge = 1.0;
			vec4 clip = mvp * vec4( p, 1.0 );
			vec3 ndc = clip.xyz / clip.w;
			vec2 fromCentre = abs( ndc.xy );
			edge = max( fromCentre.x, fromCentre.y );
			if ( edge >= 0.98 || ndc.z <= 0.0 || ndc.z >= 1.0 )
				return -1.0;

			vec4 params = u_lights.devo_shadow_params;
			// this cascade's half of the top rows of the image (the near-lamp shadow
			// tiles sit below them): the cascades are W/2 pixels tall, params.z = 1 / height
			vec2 uv = vec2( ( ndc.x * 0.5 + 0.5 + cascade ) * 0.5,
				( ndc.y * 0.5 + 0.5 ) * ( 0.5 / params.y ) * params.z );
			float sum = 0.0;
			for ( int y = 0; y < 4; y++ )
				for ( int x = 0; x < 4; x++ )
					sum += texture( devo_shadow_map, vec3( uv + ( vec2( x, y ) - 1.5 ) * params.yz, ndc.z ) );
			return sum / 16.0;
		}

		float CalcDevoSunShadow( in vec3 position, in vec3 vertexNormal, in vec3 sunDir, in float bakedVis, in bool bakedExact )
		{
			vec4 params = u_lights.devo_shadow_params;
			if ( params.x < 0.5 )
				return bakedVis;

			vec3 offsetDir = vertexNormal + sunDir;
			float edgeNear, edgeFar;
			float nearVis = DevoSunShadowCascade( u_lights.devo_shadow_mvp0, 0.0,
				position + offsetDir * u_lights.devo_shadow_offset.x, edgeNear );
			float farVis = DevoSunShadowCascade( u_lights.devo_shadow_mvp1, 1.0,
				position + offsetDir * u_lights.devo_shadow_offset.y, edgeFar );

			// far cascade, fading into the bake right at its own border
			float vis = bakedVis;
			if ( farVis >= 0.0 )
				vis = mix( farVis, bakedVis, clamp( ( edgeFar - params.w ) / ( 0.98 - params.w ), 0.0, 1.0 ) );
			// near cascade on top, fading into the far one at its border
			if ( nearVis >= 0.0 )
				vis = mix( nearVis, vis, clamp( ( edgeNear - params.w ) / ( 0.98 - params.w ), 0.0, 1.0 ) );
			// On the level itself the baked sun shadow is exact, while the shadow maps look a
			// little off the surface and can let a strip of sun in at a wall's foot. So the
			// level keeps its baked shadow and the maps only add what moves. (Models get their
			// baked value from the coarse probes, so they use the maps as they are.)
			return bakedExact ? min( vis, bakedVis ) : vis;
		}

		/*
		DevoGI near-lamp shadows: how much of a light right by the player reaches this
		spot past the things that move (players, weapons, doors), 0..1. The light looked
		at them from the middle of a cube, one 90 degree view per side, each in its own
		tile of the shadow image (only the sides that had something moving in them).
		The level's own shadows are not in here - they are baked.
		*/
		float DevoLampShadow( in int slot, in vec3 position, in vec3 vertexNormal, in vec3 lampOrigin )
		{
			vec4 faces = u_lights.devo_lamp_shadow[slot * 2];
			vec4 more = u_lights.devo_lamp_shadow[slot * 2 + 1];
			vec3 d = position - lampOrigin;
			vec3 ad = abs( d );
			// the side of the cube this spot lies behind
			float tile;
			if ( ad.x >= ad.y && ad.x >= ad.z )
				tile = d.x > 0.0 ? faces.x : faces.y;
			else if ( ad.y >= ad.z )
				tile = d.y > 0.0 ? faces.z : faces.w;
			else
				tile = d.z > 0.0 ? more.x : more.y;
			if ( tile < -0.5 )
				return 1.0;		// nothing moving on that side of the light

			// look a little off the surface and towards the light (no self-shadowing
			// "acne"): one shadow pixel grows with distance from the light
			float dist = max( length( d ), 1.0 );
			float push = more.z * dist + 0.5;
			vec3 p = position + vertexNormal * push - ( d / dist ) * push;
			int t = int( tile + 0.5 );
			vec4 clip = u_lights.devo_lamp_shadow_mvp[t] * vec4( p, 1.0 );
			if ( clip.w <= 0.0 )
				return 1.0;
			vec3 ndc = clip.xyz / clip.w;
			if ( ndc.z <= 0.0 || ndc.z >= 1.0 )
				return 1.0;

			vec4 params = u_lights.devo_lamp_shadow_params;
			vec2 pixel = u_lights.devo_shadow_params.yz;			// one pixel of the image
			// the tiles start right below the sun's cascades (half the image's width tall)
			vec2 tileOrigin = vec2( mod( float( t ), params.w ), floor( float( t ) / params.w ) ) * params.yz
				+ vec2( 0.0, ( 0.5 / pixel.x ) * pixel.y );
			vec2 lo = tileOrigin + pixel * 1.5, hi = tileOrigin + params.yz - pixel * 1.5;
			vec2 uv = tileOrigin + clamp( ndc.xy * 0.5 + 0.5, 0.0, 1.0 ) * params.yz;
			// 3x3 lookups (each smoothed 2x2 by the hardware): soft edges
			float sum = 0.0;
			for ( int y = -1; y <= 1; y++ )
				for ( int x = -1; x <= 1; x++ )
					sum += texture( devo_shadow_map, vec3( clamp( uv + vec2( x, y ) * pixel, lo, hi ), ndc.z ) );
			return sum / 9.0;
		}

		vec3 CalcDynamicLightContribution(
			in float roughness,
			in vec3 N,
			in vec3 E,
			in vec3 viewOrigin,
			in vec3 viewDir,
			in float NE,
			in vec3 diffuse,
			in vec3 specular,
			in vec3 vertexNormal,
			in float devoSunVis,
			in bool devoAllowed,
			in vec4 devoMask,
			in bool devoMaskMode )
		{
			vec3 outColor = vec3(0.0);
			vec3 position = viewOrigin - viewDir;

			for ( int i = 0; i < u_lights.num_lights; i++ )
			{
				#if 0
					if ( ( u_LightMask & ( 1 << i ) ) == 0 ) {
						continue;
					}
				#endif

				vkUniformLightEntry_t light = u_lights.light[i];
				// origin.w = type + 10 * ( mask channel + 1 ) + 100 * ( shadow slot + 1 )
				bool devoLamp = mod( light.origin.w, 10.0 ) > 0.5;
				if ( devoLamp && !devoAllowed ) {
					continue;	// this surface already has the lamps baked in
				}

				vec3  L  = light.origin.xyz - position;
				float sqrLightDist = dot(L, L);

				// sunny, skip if light is out of radius
				float radiusSqr = light.radius * light.radius;
				if (sqrLightDist > radiusSqr) {
					continue;
				}

				float attenuation;
				if ( devoLamp ) {
					attenuation = CalcDevoLampAttenuation( light, L / sqrt(sqrLightDist), sqrLightDist, radiusSqr );
				} else {
					attenuation = CalcLightAttenuation(light.radius * light.radius / sqrLightDist);
				}

				// sunny, skip lights with negligible intensity
				// (DevoGI lamps keep their strength in the colour and their attenuation is
				// plain 1 / distance^2 - below 0.001 from about 32 units on - so for them it
				// is the light actually arriving that must be negligible)
				float arriving = devoLamp ? attenuation * max( light.color.r, max( light.color.g, light.color.b ) ) : attenuation;
				if (arriving < 0.001) {
					continue;
				}

				L /= sqrt(sqrLightDist);

				// DevoGI near-lamp shadows: the shadows of moving things (see DevoLampShadow)
				float moverVis = 1.0;
				float slot = floor( light.origin.w / 100.0 ) - 1.0;
				if ( slot >= 0.0 && slot < u_lights.devo_lamp_shadow_params.x )
					moverVis = DevoLampShadow( int( slot ), position, vertexNormal, light.origin.xyz );

				vec3  H  = normalize(L + E);
				float NL = clamp(dot(N, L), 0.0, 1.0);
				float LH = clamp(dot(L, H), 0.0, 1.0);
				float NH = clamp(dot(N, H), 0.0, 1.0);
				float VH = clamp(dot(E, H), 0.0, 1.0);

				// On a lightmapped surface the lamp's light and soft shadows are already
				// baked, so live it only adds its highlight, as far as the baked mask says
				// this spot sees the lamp. Lamps without a mask channel stay baked only.
				if ( devoLamp && devoMaskMode ) {
					float channel = floor( mod( light.origin.w, 100.0 ) / 10.0 ) - 1.0;
					if ( channel < 0.0 )
						continue;
					float vis = channel < 0.5 ? devoMask.r : ( channel < 1.5 ? devoMask.g : ( channel < 2.5 ? devoMask.b : devoMask.a ) );
					if ( vis <= 0.0 )
						continue;
					vec3 lit = light.color * ( attenuation * NL * vis );
					outColor += lit * CalcSpecular(specular, NH, NL, NE, LH, VH, roughness) * moverVis;
					// where a mover blocks the lamp, take its baked light back out
					// (the same amount, see CalcDevoLampAttenuation): that's the shadow
					outColor -= lit * diffuse * ( 1.0 - moverVis );
					continue;
				}

				vec3 reflectance = diffuse + CalcSpecular(specular, NH, NL, NE, LH, VH, roughness);

				outColor += light.color * reflectance * attenuation * NL * moverVis;
			}

			// DevoGI: the sun, live, with its baked shadow (devoSunVis) until the
			// renderer draws its own sun shadows
			if ( devoAllowed && u_lights.devo_sun_dir.w > 0.5 && dot( vertexNormal, u_lights.devo_sun_dir.xyz ) > -0.2 )
			{
				vec3  L  = u_lights.devo_sun_dir.xyz;
				devoSunVis = CalcDevoSunShadow( position, vertexNormal, L, devoSunVis, devoMaskMode );
				vec3  H  = normalize(L + E);
				float NL = clamp(dot(N, L), 0.0, 1.0);
				float LH = clamp(dot(L, H), 0.0, 1.0);
				float NH = clamp(dot(N, H), 0.0, 1.0);
				float VH = clamp(dot(E, H), 0.0, 1.0);

				vec3 reflectance = diffuse + CalcSpecular(specular, NH, NL, NE, LH, VH, roughness);

				outColor += u_lights.devo_sun_color.rgb * reflectance * NL * devoSunVis;
			}

			return outColor;
		}
	#endif
#endif

#endif