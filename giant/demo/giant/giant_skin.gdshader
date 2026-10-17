// Skin and clothing for a growing giant.
//
// The mesh UVs are laid out in body metres. When the giant is s times bigger,
// one UV metre covers s world metres, so an ordinary texture would stretch s
// times. Here the fine detail (pores, fabric weave) is instead tiled at a fixed
// size in world metres: as she grows, finer and finer copies ("octaves") of the
// detail texture fade in, so up close it always looks as sharp as at human size.
// Mipmaps keep the detail clean from far away, so it needs no TAA.
shader_type spatial;

uniform sampler2D detail_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D detail_albedo : hint_default_white, filter_linear_mipmap_anisotropic, repeat_enable;
// World metres covered by one tile of detail.
uniform float detail_size = 0.25;
uniform float detail_normal_strength = 1.0;
uniform float detail_albedo_strength = 0.22;
// Off shows what an ordinary texture does: the detail stretches with the giant.
uniform bool use_octaves = true;

// Set on every mesh under a GiantBody.
instance uniform float giant_scale = 1.0;

varying vec3 tint;

void vertex() {
	// Vertex colours are authored in sRGB; lighting needs linear.
	tint = mix(COLOR.rgb / 12.92, pow((COLOR.rgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, COLOR.rgb));
}

// Detail at a constant world size: blend the two octaves around the giant's size.
vec4 sample_octaves(sampler2D tex, vec2 uv, out vec4 second, out float blend) {
	float level = use_octaves ? log2(max(giant_scale, 0.001)) : 0.0;
	float base = floor(level);
	blend = level - base;
	vec2 tile_uv = uv / detail_size;
	second = texture(tex, tile_uv * exp2(base + 1.0));
	return texture(tex, tile_uv * exp2(base));
}

void fragment() {
	vec4 albedo_hi;
	float blend;
	vec4 albedo_lo = sample_octaves(detail_albedo, UV, albedo_hi, blend);
	// Blending two noise layers flattens contrast halfway; scale it back up.
	float contrast = inversesqrt((1.0 - blend) * (1.0 - blend) + blend * blend);
	float detail = (mix(albedo_lo.r, albedo_hi.r, blend) - 0.5) * contrast;
	ALBEDO = tint * (1.0 + detail * detail_albedo_strength * 2.0);

	vec4 normal_hi;
	vec4 normal_lo = sample_octaves(detail_normal, UV, normal_hi, blend);
	vec3 n = mix(normal_lo.rgb, normal_hi.rgb, blend) * 2.0 - 1.0;
	n.xy *= contrast;
	NORMAL_MAP = normalize(n) * 0.5 + 0.5;
	NORMAL_MAP_DEPTH = detail_normal_strength;

	ROUGHNESS = 0.6 - 0.15 * detail;
	SPECULAR = 0.4;
}
