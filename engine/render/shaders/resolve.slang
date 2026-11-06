// Shades the visibility buffer: per pixel, finds the triangle the raster
// pass stored, intersects the pixel's view ray with it for perspective-
// correct barycentrics, interpolates the normal and lights it once.
//
// Lighting: one sun (GGX specular, Lambert diffuse) plus sky/ground ambient,
// then exposure and the Khronos PBR Neutral tone mapper, which keeps base
// colours true until highlights roll off. Written to an sRGB target.
//
// The far field: pixels past RenderSettings::far_field_distance (with
// nothing drawn, or a mesh drawn farther) march the voxel world from there
// and light the voxel they meet the same way.
//
// Sun shadows: virtual shadow pages (shadow_pages.slang), for meshes and
// far voxels alike.
//
// Global illumination: the bounce and sky light around each pixel, looked
// up in the lit voxel world (voxel_gi.slang), in place of the flat sky
// and ground colours; it also shuts the sky out of reflections where the
// sky is hidden.
//
// Reflections, on glossy surfaces: sharp ones from the screen, where what
// the mirror direction meets is in view (a ray against the depth buffer,
// the colour taken from last frame's image, where that point was), and a
// cone through the lit voxel world elsewhere and for the rougher ones
// (voxel_gi.slang), the sky past it. The second target keeps this frame's
// radiance, before exposure, for the next frame's reflections.
#include "common.slang"
#include "decals.slang"
#include "voxel_march.slang"
#include "voxel_gi.slang"
#include "shadow_pages.slang"

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
    uint extras;       // read descriptor of the Extras below (push constants hold 128 bytes)
    uint extras_offset;
};
ConstantBuffer<ResolvePush> push : register(b0);

// gpu::ResolveExtras.
struct Extras {
    // The far field: beyond far_start the voxel world stands in for the
    // meshes left out there (voxel_level_count 0: off).
    uint voxel_pool;
    uint voxel_table;
    uint voxel_levels;
    uint voxel_levels_offset;
    uint voxel_level_count;
    uint voxel_bricks_per_axis;
    float far_start;  // metres from the camera
    float far_radius; // instances this small are stand-ins (RenderSettings::far_field_max_radius)
    Shadows shadows;
    uint far_on;      // 1: the far field marches
    uint gi_on;       // 1: global illumination from the voxel light
    uint gi_light;    // write descriptor of the voxel light pool
    uint depth;       // read descriptor of the scene depth
    uint reflect_on;  // bit 0: voxel cone reflections; bit 1: screen-space ones
    uint history;     // read descriptor of last frame's radiance (a: distance, 0: nothing)
    float prev_p00;   // last frame's projection
    float prev_p11;
    float3 prev_rows[3]; // last frame's camera-relative world to view
    float3 prev_delta;   // this camera minus last frame's
};

Extras load_extras() {
    ByteAddressBuffer b = read_buffer(push.extras);
    const uint at = push.extras_offset;
    const uint4 a = b.Load4(at);
    const uint4 c = b.Load4(at + 16);
    const uint4 d = b.Load4(at + 32);
    const uint4 e = b.Load4(at + 48);
    const float4 x = asfloat(b.Load4(at + 64));
    const float4 y = asfloat(b.Load4(at + 80));
    const float4 z = asfloat(b.Load4(at + 96));
    Extras o;
    o.voxel_pool = a.x;
    o.voxel_table = a.y;
    o.voxel_levels = a.z;
    o.voxel_levels_offset = a.w;
    o.voxel_level_count = c.x;
    o.voxel_bricks_per_axis = c.y;
    o.far_start = asfloat(c.z);
    o.far_radius = asfloat(c.w);
    o.shadows.atlas = d.x;
    o.shadows.table = d.y;
    o.shadows.table_offset = d.z;
    o.shadows.levels = d.w;
    o.shadows.levels_offset = e.x;
    o.shadows.level_count = e.y;
    o.shadows.requests = e.z;
    o.shadows.texel = asfloat(e.w);
    o.shadows.x = x.xyz;
    o.shadows.depth_scale = x.w;
    o.shadows.y = y.xyz;
    o.shadows.z = z.xyz;
    o.shadows.depth_offset = z.w;
    const uint4 g = b.Load4(at + 112);
    o.far_on = g.x;
    o.gi_on = g.y;
    o.gi_light = g.z;
    o.depth = g.w;
    const uint4 h = b.Load4(at + 128);
    o.reflect_on = h.x;
    o.history = h.y;
    o.prev_p00 = asfloat(h.z);
    o.prev_p11 = asfloat(h.w);
    o.prev_rows[0] = asfloat(b.Load3(at + 144));
    o.prev_rows[1] = asfloat(b.Load3(at + 160));
    o.prev_rows[2] = asfloat(b.Load3(at + 176));
    o.prev_delta = asfloat(b.Load3(at + 192));
    return o;
}

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
    float3 center; // the instance's bounding sphere, camera-relative
    float radius;
};

Hit no_hit() {
    Hit h;
    h.world = float3(0.0, 0.0, 0.0);
    h.n = float3(0.0, 1.0, 0.0);
    h.instance = 0xFFFFFFFFu;
    h.material = 0;
    h.cluster = 0;
    h.level = 0;
    h.center = float3(0.0, 0.0, 0.0);
    h.radius = 0.0;
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
    h.center = to_world(inst, mesh.bounds.xyz);
    h.radius = mesh.bounds.w * inst.max_scale;
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

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Lights a surface: one sun (GGX specular, Lambert diffuse) plus the
// ambient light, before exposure and tone mapping. `variance`: the
// normal's change across the pixel (specular AA); `sun`: how much of the
// sun reaches it (shadows); `bounce`: global illumination's light there
// (rgb) and how far to trust it (a; 0: the flat sky and ground colours);
// `reflection`: the light traced along the mirror direction (rgb) and how
// much of the ambient reflection it replaces (a).
float3 shade(Frame f, float3 world, float3 n, float3 base, float rough, float metal, float3 emissive, float variance,
             float sun, float4 bounce, float4 reflection) {
    const float3 v = safe_normalize(-world, n);
    const float3 l = f.sun_direction.xyz;
    const float3 h = safe_normalize(v + l, n);
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
    float3 color = (diffuse + specular) * f.sun_light.rgb * (n_dot_l * sun);

    // Hemisphere ambient: sky above, ground below, for diffuse along the
    // normal and for specular along the reflection.
    const float3 r = reflect(-v, n);
    float3 ambient_diffuse = lerp(f.ground_light.rgb, f.sky_light.rgb, n.y * 0.5 + 0.5);
    float3 ambient_specular = lerp(f.ground_light.rgb, f.sky_light.rgb, r.y * 0.5 + 0.5);
    if (bounce.w > 0.0) {
        // The bounce light takes the flat colours' place; reflections of
        // the sky dim as much as the sky is hidden from here (until they
        // trace rays of their own).
        const float3 gi = lerp(ambient_diffuse, bounce.rgb, bounce.w);
        ambient_specular *= saturate(luminance(gi) / max(luminance(ambient_diffuse), 1e-4));
        ambient_diffuse = gi;
    }
    ambient_specular = lerp(ambient_specular, reflection.rgb, reflection.a);
    const float3 env_fresnel = f0 + (max(1.0 - roughness, f0) - f0) * pow(1.0 - n_dot_v, 5.0);
    color += diffuse_color * ambient_diffuse + env_fresnel * ambient_specular * (1.0 - 0.7 * roughness);
    return color + emissive;
}

// Voxels have a colour and a normal only; the rest is a rough dielectric.
static const float k_far_roughness = 0.8;
// The lighting view's surfaces: plain white-grey, the light alone.
static const float3 k_lighting_albedo = float3(0.75, 0.75, 0.75);
static const uint k_view_lighting = 5;

// --- Reflections -----------------------------------------------------------

// Rougher than this, the voxel cone's reflection fades out (the hemisphere
// colours, dimmed by global illumination, stay); the screen's, sharp as a
// mirror, fade out sooner.
static const float k_voxel_reflection_rough = 0.6;
static const float k_screen_reflection_rough = 0.4;
static const float k_reflection_fade = 0.15;
static const uint k_ssr_steps = 32;
static const uint k_ssr_refine = 6;
static const float k_ssr_distance = 200.0; // metres, at most

float2 ndc_to_uv(float2 ndc) { return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); }

// How far a screen position is from leaving the screen, faded over a
// tenth of it (0 at the edge, 1 well inside).
float edge_fade(float2 uv) {
    const float2 e = min(uv, 1.0 - uv);
    return saturate(min(e.x, e.y) * 10.0);
}

// How thick what the depth buffer holds is taken to be (metres at a
// distance): a ray this far behind a surface still hits it.
float ssr_thickness(float z) { return 0.3 + 0.02 * z; }

// A sharp reflection from the screen: the ray from `p` (camera-relative,
// already off the surface) along `r` (unit) against the depth buffer, and
// where it hits, that point's light in last frame's image. rgb: the light,
// a: how much to trust it (0: missed, off screen, or not seen last frame).
float4 screen_reflection(Frame f, Extras e, float3 p, float3 r) {
    Texture2D<float> depth = read_texture_float(e.depth);
    const float3 vo = float3(dot(f.view_rows[0].xyz, p), dot(f.view_rows[1].xyz, p), dot(f.view_rows[2].xyz, p));
    const float3 vr = float3(dot(f.view_rows[0].xyz, r), dot(f.view_rows[1].xyz, r), dot(f.view_rows[2].xyz, r));
    // Rays coming back at the camera meet what is behind it: faded out.
    const float towards = saturate((0.6 - vr.z) * 2.5);
    if (towards <= 0.0 || -vo.z <= f.near_plane) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    float len = k_ssr_distance;
    if (vr.z > 0.0) {
        len = min(len, (-vo.z - f.near_plane * 1.5) / vr.z); // stays in front of the camera
    }
    if (len <= 0.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float2 size = float2(f.width, f.height);
    float before = 0.0;
    float t_hit = -1.0;
    [loop]
    for (uint i = 0; i < k_ssr_steps; ++i) {
        const float k = float(i + 1) / float(k_ssr_steps);
        const float t = len * k * k; // finer near the surface
        const float3 q = vo + vr * t;
        const float z = -q.z;
        const float2 uv = ndc_to_uv(float2(f.p00 * q.x, f.p11 * q.y) / z);
        if (any(uv < 0.0) || any(uv >= 1.0)) {
            break; // left the screen
        }
        const float d = depth.Load(int3(int2(uv * size), 0));
        const float zs = d > 0.0 ? f.near_plane / d : 3.0e38;
        if (z > zs) {
            // Behind what the screen shows: find where it went in.
            float lo = before;
            float hi = t;
            [loop]
            for (uint j = 0; j < k_ssr_refine; ++j) {
                const float mid = 0.5 * (lo + hi);
                const float3 m = vo + vr * mid;
                const float2 muv = ndc_to_uv(float2(f.p00 * m.x, f.p11 * m.y) / -m.z);
                const float md = depth.Load(int3(int2(saturate(muv) * (size - 1.0)), 0));
                const float mzs = md > 0.0 ? f.near_plane / md : 3.0e38;
                if (-m.z > mzs) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            const float3 m = vo + vr * hi;
            const float2 muv = ndc_to_uv(float2(f.p00 * m.x, f.p11 * m.y) / -m.z);
            const float md = depth.Load(int3(int2(saturate(muv) * (size - 1.0)), 0));
            const float mzs = md > 0.0 ? f.near_plane / md : 3.0e38;
            if (-m.z - mzs < ssr_thickness(mzs)) {
                t_hit = hi;
                break;
            }
            // Passed behind something thin: go on.
        }
        before = t;
    }
    if (t_hit < 0.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    // Where the point was last frame, and what the image showed there.
    const float3 hit = p + r * t_hit;
    const float3 was = hit + e.prev_delta;
    const float3 pv = float3(dot(e.prev_rows[0], was), dot(e.prev_rows[1], was), dot(e.prev_rows[2], was));
    if (-pv.z <= f.near_plane) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float2 puv = ndc_to_uv(float2(e.prev_p00 * pv.x, e.prev_p11 * pv.y) / -pv.z);
    if (any(puv < 0.0) || any(puv > 1.0)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float4 seen = read_texture(e.history).SampleLevel(get_sampler(k_sampler_point_clamp), puv, 0.0);
    // Something else was there last frame (it was hidden, or has moved).
    const float dist = length(was);
    if (seen.a <= 0.0 || abs(seen.a - dist) > ssr_thickness(dist)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float3 hv = vo + vr * t_hit;
    const float2 huv = ndc_to_uv(float2(f.p00 * hv.x, f.p11 * hv.y) / -hv.z);
    return float4(seen.rgb, towards * edge_fade(huv) * edge_fade(puv));
}

// The light reflected along the mirror direction at a surface point, and
// how much of the ambient reflection it replaces.
float4 trace_reflections(Frame f, Extras e, VoxelWorld w, float3 world, float3 n, float rough, float pixel_size) {
    const float voxel_weight =
        (e.reflect_on & 1u) != 0u ? saturate((k_voxel_reflection_rough - rough) / k_reflection_fade) : 0.0;
    const float screen_weight =
        (e.reflect_on & 2u) != 0u ? saturate((k_screen_reflection_rough - rough) / k_reflection_fade) : 0.0;
    if (voxel_weight <= 0.0 && screen_weight <= 0.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float3 v = safe_normalize(-world, n);
    const float3 r = reflect(-v, n);
    float3 color = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    if (voxel_weight > 0.0) {
        const float roughness = clamp(rough, 0.045, 1.0);
        const float cone_tan = clamp(roughness * roughness, 0.03, 0.577);
        VoxelWorld all_voxels = w;
        all_voxels.stand_ins = 0u; // everything reflects
        color = trace_reflection(all_voxels, write_buffer(e.gi_light), f, world, n, r, cone_tan).rgb;
        weight = voxel_weight;
    }
    if (screen_weight > 0.0) {
        // Off the surface by a couple of pixels, so it does not hit itself.
        const float4 s = screen_reflection(f, e, world + n * (2.0 * pixel_size), r);
        const float use = s.a * screen_weight;
        if (use > 0.0) {
            // Where the voxels gave nothing, the screen alone.
            color = weight > 0.0 ? lerp(color, s.rgb, use) : s.rgb;
            weight = weight > 0.0 ? lerp(weight, 1.0, use) : use;
        }
    }
    return float4(color, weight);
}

struct Output {
    float4 color : SV_Target0;    // tone mapped, for the screen
    float4 radiance : SV_Target1; // before exposure; a: the distance (0: nothing drawn)
};

Output finish(float3 color, Frame f, float dist) {
    Output o;
    o.color = float4(tone_map(color * f.exposure), 1.0);
    o.radiance = float4(min(color, float3(60000.0, 60000.0, 60000.0)), dist);
    return o;
}

[shader("pixel")]
Output ps_main(float4 position : SV_Position) {
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

    // The far field: past far_start the meshes are left out (the CPU drops
    // instances wholly beyond it) and the voxel world stands in, in front
    // of whatever mesh is drawn there (lit views only).
    const Extras extras = load_extras();
    const bool lighting_view = f.debug_view == k_view_lighting;
    const bool lit_path = f.debug_view == 0 || lighting_view;
    VoxelWorld w;
    w.pool = extras.voxel_pool;
    w.table = extras.voxel_table;
    w.levels = extras.voxel_levels;
    w.levels_offset = extras.voxel_levels_offset;
    w.level_count = extras.voxel_level_count;
    w.bricks_per_axis = extras.voxel_bricks_per_axis;
    w.stand_ins = 1u; // the far field: only what the CPU left out (meshes drawn keep their own look)
    if (lit_path && extras.far_on != 0) {
        const float mesh_t = hit ? length(s.world) : 3.0e38;
        // A stand-in drawn as a mesh (straddling the distance, moving, or
        // not in the voxel world yet): its own voxels must not cover it.
        const float4 skip = hit && s.radius <= extras.far_radius ? float4(s.center, s.radius)
                                                            : float4(0.0, 0.0, 0.0, -1.0);
        if (mesh_t > extras.far_start) {
            const float2 ndc = float2((position.x / f.width) * 2.0 - 1.0, 1.0 - (position.y / f.height) * 2.0);
            const float3 dir = normalize(f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz);
            float t;
            uint packed;
            uint hit_at;
            if (march_voxels(w, dir, extras.far_start, mesh_t, skip, t, packed, hit_at)) {
                const float3 world = dir * t;
                const float3 vn = unpack_normal(packed);
                const float far_pixel = t * 2.0 / max(f.p11 * f.height, 1e-6);
                const float sun = sun_visibility(extras.shadows, world, vn, f.sun_direction.xyz, far_pixel);
                // The voxel's own light word.
                float4 bounce = float4(0.0, 0.0, 0.0, 0.0);
                if (extras.gi_on != 0) {
                    const uint lw = write_buffer(extras.gi_light).Load(hit_at * 4);
                    bounce = light_lit(lw) ? float4(unpack_light(lw), 1.0) : bounce;
                }
                const float3 albedo = lighting_view ? k_lighting_albedo : unpack_color(packed);
                const float3 color = shade(f, world, vn, albedo, k_far_roughness, 0.0, float3(0.0, 0.0, 0.0), 0.0,
                                           sun, bounce, float4(0.0, 0.0, 0.0, 0.0));
                return finish(color, f, t);
            }
        }
    }
    if (!hit) {
        Output o;
        o.color = float4(push.background.rgb, 1.0);
        o.radiance = float4(0.0, 0.0, 0.0, 0.0);
        return o;
    }
    const float3 n = s.n;
    const float3 world = s.world;

    if (!lit_path) {
        // Flat colour shaded just enough to read the shape.
        const float3 col = f.debug_view == 1 ? hash_color(s.cluster)
                         : f.debug_view == 2 ? level_color(s.level)
                                             : hash_color(s.cluster * 131u + vis.y);
        const float shade_amount = 0.35 + 0.65 * saturate(dot(n, f.sun_direction.xyz) * 0.5 + 0.5);
        Output o;
        o.color = float4(col * shade_amount, 1.0);
        o.radiance = float4(0.0, 0.0, 0.0, 0.0);
        return o;
    }

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
    if (lighting_view) {
        base = k_lighting_albedo;
        metal = 0.0;
        emissive = float3(0.0, 0.0, 0.0);
    }
    const float sun = sun_visibility(extras.shadows, world, n, f.sun_direction.xyz, pixel_size);
    float4 bounce = float4(0.0, 0.0, 0.0, 0.0);
    if (extras.gi_on != 0) {
        bounce = voxel_bounce(w, write_buffer(extras.gi_light), world, n);
    }
    const float4 reflection = trace_reflections(f, extras, w, world, n, rough, pixel_size);
    const float3 color = shade(f, world, n, base, rough, metal, emissive, variance, sun, bounce, reflection);
    return finish(color, f, length(world));
}
