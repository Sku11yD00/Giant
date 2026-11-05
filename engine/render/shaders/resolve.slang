// Shades the visibility buffer: per pixel, finds the triangle the raster
// pass stored, intersects the pixel's view ray with it for perspective-
// correct barycentrics, interpolates the normal and lights it once.
//
// Lighting: one sun (GGX specular, Lambert diffuse) plus sky/ground ambient,
// then exposure and the Khronos PBR Neutral tone mapper, which keeps base
// colours true until highlights roll off. Written to an sRGB target.
#include "common.slang"
#include "decals.slang"

struct ResolvePush {
    float4 background; // linear; where nothing was drawn
    uint frame;
    uint frame_offset;
    uint instances;
    uint instance_offset;
    uint meshes;
    uint mesh_offset;
    uint visible;      // read descriptor of the visible list
    uint visible_offset;
    uint vis;          // read descriptor of the visibility buffer (rg32_uint)
    uint materials;    // the frame's material table, the default last
    uint material_offset;
    uint material_count;
    uint decals;       // decals, tiles and entries (src/decals.h)
    uint decal_offset;
    uint tile_offset;
    uint entry_offset;
    uint tile_columns; // 0: no decals
    uint tile_rows;
    uint specular_aa;  // 1: widen highlights where normals change quickly
    uint pad0;
};
ConstantBuffer<ResolvePush> push : register(b0);

// A triangle covering the screen.
[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
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

struct Surface {
    float3 position; // camera-relative
    float3 normal;   // unit
};

// The ray through pixel `pixel` (camera-relative, from the camera) against
// the triangle's corners: Moller-Trumbore, without back-face rejection.
// Barycentrics are clamped, so pixels whose centre misses the triangle
// slightly (edges: the rasterizer covers by area) stay on it.
float3 barycentrics(float3 dir, float3 a, float3 b, float3 c) {
    const float3 e1 = b - a;
    const float3 e2 = c - a;
    const float3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (abs(det) < 1e-20) {
        return float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0); // seen edge-on
    }
    const float inv = 1.0 / det;
    const float3 s = -a; // the ray starts at the camera, the origin
    const float u = dot(s, p) * inv;
    const float3 q = cross(s, e1);
    const float v = dot(dir, q) * inv;
    const float3 w = max(float3(1.0 - u - v, u, v), float3(0.0, 0.0, 0.0));
    return w / max(w.x + w.y + w.z, 1e-12);
}

// What a pixel shows, rebuilt from the visibility buffer.
struct Hit {
    float3 world;  // camera-relative
    float3 n;      // unit
    uint instance; // into the frame's instances
    uint material;
    uint cluster;  // the visible list's cluster index
    uint level;
};

Hit no_hit() {
    Hit h;
    h.world = float3(0.0, 0.0, 0.0);
    h.n = float3(0.0, 1.0, 0.0);
    h.instance = 0xFFFFFFFFu;
    h.material = 0;
    h.cluster = 0;
    h.level = 0;
    return h;
}

Hit surface_at(Frame f, float4 position, uint2 vis) {
    const uint2 entry = read_buffer(push.visible).Load2(push.visible_offset + (vis.x - 1) * 8);
    const Instance inst = load_instance(read_buffer(push.instances), push.instance_offset + entry.x * k_instance_bytes);
    const MeshEntry mesh = load_mesh(read_buffer(push.meshes), push.mesh_offset + inst.mesh * k_mesh_bytes);
    const Cluster cluster = load_cluster(read_buffer_nonuniform(mesh.clusters), entry.y);
    // Drawn this frame, so its group is loaded (residency only changes
    // between frames).
    ByteAddressBuffer pool = read_buffer(f.pool);
    const uint geometry = group_offset(mesh, cluster.group);
    const uint3 corners = cluster_triangle(pool, geometry, cluster, vis.y);
    const float3 a = to_world(inst, cluster_position(pool, geometry, cluster, corners.x));
    const float3 b = to_world(inst, cluster_position(pool, geometry, cluster, corners.y));
    const float3 c = to_world(inst, cluster_position(pool, geometry, cluster, corners.z));

    const float2 ndc = float2((position.x / f.width) * 2.0 - 1.0, 1.0 - (position.y / f.height) * 2.0);
    const float3 dir = f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz;
    const float3 w = barycentrics(dir, a, b, c);
    const float3 object_normal = cluster_normal(pool, geometry, cluster, corners.x) * w.x +
                                 cluster_normal(pool, geometry, cluster, corners.y) * w.y +
                                 cluster_normal(pool, geometry, cluster, corners.z) * w.z;
    Hit h;
    h.n = safe_normalize(to_world_normal(inst, object_normal), float3(0.0, 1.0, 0.0));
    h.world = a * w.x + b * w.y + c * w.z;
    h.instance = entry.x;
    h.material = inst.material;
    h.cluster = entry.y;
    h.level = cluster.level;
    return h;
}

// Specular anti-aliasing (Tokuyoshi and Kaplanyan, "Improved Geometric
// Specular Antialiasing", 2019): where the normal changes quickly across
// the pixel's footprint, the highlight is widened to what the pixel really
// covers, so fine bumps and far-off curves do not sparkle or crawl. The
// change is measured against the neighbouring pixels of the 2x2 quad on the
// same instance, so silhouettes do not count.
static const float k_specular_aa_variance = 0.25; // the pixel filter's variance
static const float k_specular_aa_limit = 0.18;    // the most roughness^4 it may add

float normal_variance(float3 n, uint instance) {
    const float3 nx = QuadReadAcrossX(n);
    const float3 ny = QuadReadAcrossY(n);
    const uint ix = QuadReadAcrossX(instance);
    const uint iy = QuadReadAcrossY(instance);
    float v = 0.0;
    if (ix == instance) {
        v += dot(nx - n, nx - n);
    }
    if (iy == instance) {
        v += dot(ny - n, ny - n);
    }
    return v;
}

[shader("pixel")]
float4 ps_main(float4 position : SV_Position) : SV_Target {
    const uint2 pixel = uint2(position.xy);
    const uint2 vis = read_texture_uint2(push.vis).Load(int3(int2(pixel), 0));
    const Frame f = load_frame(push.frame, push.frame_offset);
    const bool hit = vis.x != 0;
    Hit s = no_hit();
    if (hit) {
        s = surface_at(f, position, vis);
    }
    // Every pixel of the quad reaches this, drawn or not.
    const float variance = normal_variance(s.n, s.instance);
    if (!hit) {
        return float4(push.background.rgb, 1.0);
    }
    const float3 n = s.n;
    const float3 world = s.world;

    if (f.debug_view != 0) {
        // Flat colour shaded just enough to read the shape.
        const float3 col = f.debug_view == 1 ? hash_color(s.cluster)
                         : f.debug_view == 2 ? level_color(s.level)
                                             : hash_color(s.cluster * 131u + vis.y);
        const float shade = 0.35 + 0.65 * saturate(dot(n, f.sun_direction.xyz) * 0.5 + 0.5);
        return float4(col * shade, 1.0);
    }
    const float3 v = safe_normalize(-world, n);
    const float3 l = f.sun_direction.xyz;
    const float3 h = safe_normalize(v + l, n);

    // The instance's material (the CPU keeps the index in range), then
    // the decals over this pixel.
    ByteAddressBuffer materials = read_buffer(push.materials);
    const uint material_at = push.material_offset + min(s.material, push.material_count) * 32;
    const float4 base_roughness = load4(materials, material_at);
    const float4 emissive_metallic = load4(materials, material_at + 16);
    float3 base = base_roughness.rgb;
    float rough = base_roughness.a;
    float metal = emissive_metallic.a;
    float3 emissive = emissive_metallic.rgb;
    const float pixel_size = length(world) * 2.0 / max(f.p11 * f.height, 1e-6);
    apply_decals(push.decals, push.decal_offset, push.tile_offset, push.entry_offset, push.tile_columns,
                 push.tile_rows, pixel, world, n, pixel_size, base, rough, metal, emissive);
    // Clamped so a mirror-smooth surface keeps a visible, stable highlight.
    const float roughness = clamp(rough, 0.045, 1.0);
    float alpha = roughness * roughness;
    if (push.specular_aa != 0) {
        const float kernel = min(2.0 * k_specular_aa_variance * variance, k_specular_aa_limit);
        alpha = sqrt(saturate(alpha * alpha + kernel));
    }
    const float metallic = saturate(metal);
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

    color += emissive;
    return float4(tone_map(color * f.exposure), 1.0);
}
