// Grass and earth that stay readable from 1 m to 50 km away.
// Each layer of detail fades out once it gets smaller than a pixel, so the
// ground never shimmers, with or without TAA.
shader_type spatial;
render_mode cull_disabled;

uniform vec3 grass : source_color = vec3(0.27, 0.36, 0.16);
uniform vec3 dry : source_color = vec3(0.42, 0.4, 0.24);
uniform vec3 earth : source_color = vec3(0.33, 0.27, 0.19);

varying vec3 world_pos;

// Integer hash, so the pattern holds up kilometres from the origin.
float hash(vec2 cell) {
	uvec2 q = uvec2(ivec2(cell) + ivec2(1 << 20));
	uint h = q.x * 1597334677u ^ q.y * 3812015801u;
	h = h * 747796405u + 2891336453u;
	h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
	return float((h >> 22u) ^ h) * (1.0 / 4294967295.0);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Noise with features of size p_size metres, faded out when they get under a pixel.
float layer(vec2 xz, float p_size, float footprint) {
	float fade = 1.0 - smoothstep(p_size * 0.25, p_size * 0.6, footprint);
	return (value_noise(xz / p_size) - 0.5) * fade;
}

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec2 xz = world_pos.xz;
	float footprint = length(fwidth(xz));
	float fine = layer(xz, 0.35, footprint) + 0.7 * layer(xz, 1.5, footprint) + 0.6 * layer(xz, 6.0, footprint);
	float mid = layer(xz, 25.0, footprint) + 0.8 * layer(xz, 110.0, footprint);
	float broad = layer(xz, 600.0, footprint) + 0.8 * layer(xz, 3000.0, footprint) + 0.6 * layer(xz, 12000.0, footprint);
	vec3 color = mix(grass, dry, clamp(0.5 + broad * 1.3 + mid * 0.5, 0.0, 1.0));
	color = mix(color, earth, smoothstep(0.32, 0.5, mid + fine * 0.25));
	color *= 1.0 + fine * 0.35;
	ALBEDO = color;
	ROUGHNESS = 0.95;
	SPECULAR = 0.3;
}
