// Distant mountains with snow caps.
shader_type spatial;

varying vec3 world_pos;
varying vec3 tint;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	tint = COLOR.rgb;
}

void fragment() {
	vec3 n = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	float snow = smoothstep(2000.0, 2400.0, world_pos.y + 300.0 * sin(world_pos.x * 0.002) * cos(world_pos.z * 0.0017)) * smoothstep(0.2, 0.5, n.y);
	ALBEDO = mix(tint, vec3(0.92, 0.94, 0.97), snow);
	ROUGHNESS = 0.9;
}
