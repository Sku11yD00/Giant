// Forward-lit cluster meshes: vertex pulling, camera-relative.
//
// Each draw covers one mesh. Instance i of the draw is entry i of the
// frame's chosen clusters (an instance and a cluster); vertex v of it is
// corner v % 3 of the cluster's triangle v / 3. Clusters have at most 128
// triangles, so draws use 384 vertices and clusters with fewer triangles
// collapse the rest to a point, which the rasterizer drops.
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
    float4 params; // x: exposure, y: debug view (0 lit, 1 clusters, 2 levels, 3 triangles)
};
static const uint k_frame_bytes = 144;

// 128 bytes per instance: model rows (3x4), normal matrix rows (3x4, w 0),
// base colour + roughness, emissive + metallic.
static const uint k_instance_bytes = 128;
static const uint k_vertex_bytes = 24;
static const uint k_entry_bytes = 8;    // instance, cluster
static const uint k_cluster_bytes = 16; // first index, triangle count, level, unused

struct ClusterPush {
    uint frame;           // descriptor of the frame constants
    uint frame_offset;
    uint instances;       // descriptor of the frame's instances
    uint instance_offset; // byte offset of instance 0
    uint entries;         // descriptor of the frame's chosen clusters
    uint entry_offset;    // byte offset of this draw's first entry
    uint vertices;        // this mesh's vertices
    uint indices;         // this mesh's cluster triangles
    uint clusters;        // this mesh's cluster table
    uint pad0;
    uint pad1;
    uint pad2;
};
ConstantBuffer<ClusterPush> push : register(b0);

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
    nointerpolation uint3 debug : TEXCOORD0; // cluster, level, triangle
};

[shader("vertex")]
VsOut vs_main(uint vertex_id : SV_VertexID, uint entry_id : SV_InstanceID) {
    VsOut o;
    const uint2 entry = read_buffer(push.entries).Load2(push.entry_offset + entry_id * k_entry_bytes);
    const uint4 cluster = read_buffer(push.clusters).Load4(entry.y * k_cluster_bytes);
    const uint triangle = vertex_id / 3;
    if (triangle >= cluster.y) {
        // All three corners meet at one point: no area, nothing drawn.
        o.position = float4(0.0, 0.0, 0.0, 1.0);
        o.relative = float3(0.0, 0.0, 0.0);
        o.normal = float3(0.0, 1.0, 0.0);
        o.base_roughness = float4(0.0, 0.0, 0.0, 0.0);
        o.emissive_metallic = float4(0.0, 0.0, 0.0, 0.0);
        o.debug = uint3(0, 0, 0);
        return o;
    }
    const uint index = read_buffer(push.indices).Load((cluster.x + vertex_id) * 4);
    ByteAddressBuffer vb = read_buffer(push.vertices);
    const float3 p = asfloat(vb.Load3(index * k_vertex_bytes));
    const float3 n = asfloat(vb.Load3(index * k_vertex_bytes + 12));

    ByteAddressBuffer ib = read_buffer(push.instances);
    const uint at = push.instance_offset + entry.x * k_instance_bytes;
    const float4 p1 = float4(p, 1.0);
    const float3 world = float3(dot(load4(ib, at), p1), dot(load4(ib, at + 16), p1),
                                dot(load4(ib, at + 32), p1));
    const float3 normal = float3(dot(load4(ib, at + 48).xyz, n), dot(load4(ib, at + 64).xyz, n),
                                 dot(load4(ib, at + 80).xyz, n));

    const FrameConstants f = load_frame();
    o.position = transform(f.view_proj[0], f.view_proj[1], f.view_proj[2], f.view_proj[3],
                           float4(world, 1.0));
    o.relative = world;
    o.normal = normal;
    o.base_roughness = load4(ib, at + 96);
    o.emissive_metallic = load4(ib, at + 112);
    o.debug = uint3(entry.y, cluster.z, triangle);
    return o;
}

static const float k_pi = 3.14159265;

// normalize() that returns `fallback` instead of NaN for a zero vector
// (the camera on a surface, or looking straight into the sun).
float3 safe_normalize(float3 v, float3 fallback) {
    const float len2 = dot(v, v);
    return len2 > 1e-12 ? v * rsqrt(len2) : fallback;
}

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

// A well-mixed hash (PCG) for debug colours.
uint hash(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float3 hash_color(uint v) {
    const uint h = hash(v);
    return float3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0 * 0.8 + 0.2;
}

float3 level_color(uint level) {
    static const float3 k_levels[8] = {
        float3(1.0, 0.15, 0.1), float3(1.0, 0.85, 0.1), float3(0.3, 0.9, 0.2), float3(0.1, 0.85, 0.9),
        float3(0.2, 0.35, 1.0), float3(0.75, 0.3, 1.0), float3(1.0, 0.4, 0.75), float3(0.9, 0.9, 0.9),
    };
    return k_levels[min(level, 7u)];
}

[shader("pixel")]
float4 ps_main(VsOut i) : SV_Target {
    const FrameConstants f = load_frame();
    const float3 n = safe_normalize(i.normal, float3(0.0, 1.0, 0.0));
    const uint debug_view = (uint)f.params.y;
    if (debug_view != 0) {
        // Flat colour shaded just enough to read the shape.
        const float3 c = debug_view == 1 ? hash_color(i.debug.x)
                       : debug_view == 2 ? level_color(i.debug.y)
                                         : hash_color(i.debug.x * 131u + i.debug.z);
        const float shade = 0.35 + 0.65 * saturate(dot(n, f.sun_direction.xyz) * 0.5 + 0.5);
        return float4(c * shade, 1.0);
    }
    const float3 v = safe_normalize(-i.relative, n);
    const float3 l = f.sun_direction.xyz;
    const float3 h = safe_normalize(v + l, n);

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
