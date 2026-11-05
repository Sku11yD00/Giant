// Forward-lit meshes: vertex pulling, instanced, camera-relative.
//
// Lighting: one sun (GGX specular, Lambert diffuse) plus sky/ground ambient,
// then exposure and the Khronos PBR Neutral tone mapper, which keeps base
// colours true until highlights roll off. Written to an sRGB target.
#include "giant/bindless.slang"

// Matches render::Renderer (src/renderer.cpp): kept in transient memory.
struct FrameConstants {
    float4 view_proj[4]; // columns
    float4 sun_direction;
    float4 sun_light;
    float4 sky_light;
    float4 ground_light;
    float4 params; // x: exposure
};
static const uint k_frame_bytes = 144;

// 128 bytes per instance: model rows (3x4), normal matrix rows (3x4, w 0),
// base colour + roughness, emissive + metallic.
static const uint k_instance_bytes = 128;
static const uint k_vertex_bytes = 24;

struct MeshPush {
    uint frame;           // descriptor of the frame constants
    uint frame_offset;
    uint instances;       // descriptor of this draw's instances
    uint instance_offset; // byte offset of its first instance
    uint vertices;        // descriptor of the shared vertex buffer
    uint pad0;
    uint pad1;
    uint pad2;
};
ConstantBuffer<MeshPush> push : register(b0);

float4 load4(ByteAddressBuffer b, uint at) { return asfloat(b.Load4(at)); }

FrameConstants load_frame() {
    ByteAddressBuffer b = read_buffer(push.frame);
    const uint at = push.frame_offset;
    FrameConstants f;
    [unroll] for (uint i = 0; i < 4; ++i) {
        f.view_proj[i] = load4(b, at + i * 16);
    }
    f.sun_direction = load4(b, at + 64);
    f.sun_light = load4(b, at + 80);
    f.sky_light = load4(b, at + 96);
    f.ground_light = load4(b, at + 112);
    f.params = load4(b, at + 128);
    return f;
}

struct VsOut {
    float4 position : SV_Position;
    float3 relative : POSITION0; // camera-relative world position
    float3 normal : NORMAL0;
    nointerpolation float4 base_roughness : COLOR0;
    nointerpolation float4 emissive_metallic : COLOR1;
};

[shader("vertex")]
VsOut vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    ByteAddressBuffer vb = read_buffer(push.vertices);
    const float3 p = asfloat(vb.Load3(vertex_id * k_vertex_bytes));
    const float3 n = asfloat(vb.Load3(vertex_id * k_vertex_bytes + 12));

    ByteAddressBuffer ib = read_buffer(push.instances);
    const uint at = push.instance_offset + instance_id * k_instance_bytes;
    const float4 p1 = float4(p, 1.0);
    const float3 world = float3(dot(load4(ib, at), p1), dot(load4(ib, at + 16), p1),
                                dot(load4(ib, at + 32), p1));
    const float3 normal = float3(dot(load4(ib, at + 48).xyz, n), dot(load4(ib, at + 64).xyz, n),
                                 dot(load4(ib, at + 80).xyz, n));

    const FrameConstants f = load_frame();
    VsOut o;
    o.position = transform(f.view_proj[0], f.view_proj[1], f.view_proj[2], f.view_proj[3],
                           float4(world, 1.0));
    o.relative = world;
    o.normal = normal;
    o.base_roughness = load4(ib, at + 96);
    o.emissive_metallic = load4(ib, at + 112);
    return o;
}

static const float k_pi = 3.14159265;

// GGX / Trowbridge-Reitz normal distribution.
float d_ggx(float n_dot_h, float alpha) {
    const float a2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (k_pi * d * d);
}

// Height-correlated Smith visibility (includes the 1 / (4 n.l n.v) term).
float v_smith(float n_dot_v, float n_dot_l, float alpha) {
    const float a2 = alpha * alpha;
    const float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 f_schlick(float3 f0, float v_dot_h) {
    return f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
}

// Khronos PBR Neutral tone mapping.
float3 tone_map(float3 color) {
    const float start = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < start) {
        return color;
    }
    const float d = 1.0 - start;
    const float new_peak = 1.0 - d * d / (peak + d - start);
    color *= new_peak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return lerp(color, float3(new_peak, new_peak, new_peak), g);
}

[shader("pixel")]
float4 ps_main(VsOut i) : SV_Target {
    const FrameConstants f = load_frame();
    const float3 n = normalize(i.normal);
    const float3 v = normalize(-i.relative);
    const float3 l = f.sun_direction.xyz;
    const float3 h = normalize(v + l);

    const float3 base = i.base_roughness.rgb;
    // Clamped so a mirror-smooth surface keeps a visible, stable highlight.
    const float roughness = clamp(i.base_roughness.a, 0.045, 1.0);
    const float alpha = roughness * roughness;
    const float metallic = saturate(i.emissive_metallic.a);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    const float3 diffuse_color = base * (1.0 - metallic);

    const float n_dot_l = saturate(dot(n, l));
    const float n_dot_v = max(dot(n, v), 1e-4);
    const float n_dot_h = saturate(dot(n, h));
    const float v_dot_h = saturate(dot(v, h));

    const float3 fresnel = f_schlick(f0, v_dot_h);
    const float3 specular = d_ggx(n_dot_h, alpha) * v_smith(n_dot_v, n_dot_l, alpha) * fresnel;
    const float3 diffuse = (1.0 - fresnel) * diffuse_color / k_pi;
    float3 color = (diffuse + specular) * f.sun_light.rgb * n_dot_l;

    // Hemisphere ambient: sky above, ground below, for diffuse along the
    // normal and for specular along the reflection.
    const float3 r = reflect(-v, n);
    const float3 ambient_diffuse = lerp(f.ground_light.rgb, f.sky_light.rgb, n.y * 0.5 + 0.5);
    const float3 ambient_specular = lerp(f.ground_light.rgb, f.sky_light.rgb, r.y * 0.5 + 0.5);
    const float3 env_fresnel = f0 + (max(1.0 - roughness, f0) - f0) * pow(1.0 - n_dot_v, 5.0);
    color += diffuse_color * ambient_diffuse + env_fresnel * ambient_specular * (1.0 - 0.7 * roughness);

    color += i.emissive_metallic.rgb;
    return float4(tone_map(color * f.params.x), 1.0);
}
