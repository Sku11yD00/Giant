// Shades the visibility buffer: per pixel, finds the triangle the raster
// pass stored, intersects the pixel's view ray with it for perspective-
// correct barycentrics, interpolates the normal and lights it once.
//
// Lighting: one sun (GGX specular, Lambert diffuse) plus sky/ground ambient,
// written as the light before exposure (a: 1 to expose and tone map, 0
// shown as it is). With screen reflections on, also what reflect.slang
// needs to add them (ps_main); off, the light alone (ps_direct). Either
// way post.slang then exposes and tone maps it.
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
// Reflections, on glossy surfaces: a cone through the lit voxel world
// along the mirror direction (voxel_gi.slang), the hemisphere colours past
// it. Glossy pixels are marked (their reflection's weight and normal, and
// the reflected light used here) for the screen's sharper reflections,
// which reflect.slang traces once this frame's image is shaded.
#include "common.slang"
#include "decals.slang"
#include "voxel_march.slang"
#include "voxel_gi.slang"
#include "shadow_pages.slang"
#include "tone_map.slang"
#include "sky_common.slang"

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

static const uint k_material_bytes = 48; // gpu::Material; its textures at +32
static const uint k_no_texture = 0xFFFFFFFFu; // rhi::k_no_descriptor


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
    uint reflect_on;  // bit 0: voxel cone reflections; bit 1: mark pixels for the screen's
    uint sky_params;  // the sky's SkyParams (sky_on 0: the flat background)
    uint sky_params_offset;
    uint sky_on;
    uint dust_puffs;  // dust_count puffs of 32 bytes at dust_offset (gpu::GpuDustPuff)
    uint dust_offset;
    uint dust_count;
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
    o.reflect_on = g.w;
    const uint4 k = b.Load4(at + 128);
    o.sky_params = k.x;
    o.sky_params_offset = k.y;
    o.sky_on = k.z;
    o.dust_puffs = k.w;
    const uint4 m = b.Load4(at + 144);
    o.dust_offset = m.x;
    o.dust_count = m.y;
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

struct Surface {
    float3 position; // camera-relative
    float3 normal;   // unit
};

// The ray through pixel `pixel` (camera-relative, from the camera) against
// the triangle's corners: Moller-Trumbore, without back-face rejection.
// Barycentrics are clamped, so pixels whose centre misses the triangle
// slightly (edges: the rasterizer covers by area) stay on it.
// Unclamped: where the ray meets the triangle's plane, even outside it
// (for the uv differentials of neighbouring pixels' rays). w: 1 if it meets
// the plane in front of the camera, 0 if the ray runs along it or meets it
// behind.
float4 plane_barycentrics(float3 dir, float3 a, float3 b, float3 c) {
    const float3 e1 = b - a;
    const float3 e2 = c - a;
    const float3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (abs(det) < 1e-20) {
        return float4(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0, 0.0); // seen edge-on
    }
    const float inv = 1.0 / det;
    const float3 s = -a; // the ray starts at the camera, the origin
    const float u = dot(s, p) * inv;
    const float3 q = cross(s, e1);
    const float v = dot(dir, q) * inv;
    const float t = dot(e2, q) * inv;
    return float4(1.0 - u - v, u, v, t > 0.0 ? 1.0 : 0.0);
}

// Clamped onto the triangle: pixels whose centre misses it slightly
// (edges: the rasterizer covers by area) stay on it.
float3 on_triangle(float3 plane) {
    const float3 w = max(plane, float3(0.0, 0.0, 0.0));
    return w / max(w.x + w.y + w.z, 1e-12);
}

// The uv's change to a neighbouring pixel, from where its ray meets the
// plane: at most the whole texture (grazing views), `fallback` (also the
// whole texture) when it misses the plane.
float2 uv_step(float4 there, float2 uv_here, float2 uv0, float2 uv1, float2 uv2, float2 fallback) {
    if (there.w == 0.0) {
        return fallback;
    }
    const float2 d = uv0 * there.x + uv1 * there.y + uv2 * there.z - uv_here;
    const float len = length(d);
    if (!(len <= 1.0)) {
        return len > 1.0 && len < 3.0e38 ? d / len : fallback; // too far, or not a number
    }
    return d;
}

// What a pixel shows, rebuilt from the visibility buffer.
struct Hit {
    float3 world;  // camera-relative
    float3 n;      // unit
    float2 uv;     // the mesh's texture coordinates here
    float2 uv_dx;  // and their change to the next pixel right and down (ray
    float2 uv_dy;  // differentials: texture filtering's footprint)
    float4 tangent; // world tangent (along +u) and bitangent sign
    uint4 textures; // the material's (gpu::Material::textures)
    float3 shading_n; // n bent by the material's normal map (shade_pixel sets it)
    uint instance; // into the frame's instances
    uint material;
    uint cluster;  // the visible list's cluster index
    uint level;
    float3 center; // the instance's bounding sphere, camera-relative
    float radius;
    bool always_mesh; // never a far-field stand-in (terrain tiles)
};

Hit no_hit() {
    Hit h;
    h.world = float3(0.0, 0.0, 0.0);
    h.n = float3(0.0, 1.0, 0.0);
    h.uv = float2(0.0, 0.0);
    h.uv_dx = float2(0.0, 0.0);
    h.uv_dy = float2(0.0, 0.0);
    h.tangent = float4(1.0, 0.0, 0.0, 1.0);
    h.textures = uint4(k_no_texture, k_no_texture, k_no_texture, k_no_texture);
    h.shading_n = h.n;
    h.instance = 0xFFFFFFFFu;
    h.material = 0;
    h.cluster = 0;
    h.level = 0;
    h.center = float3(0.0, 0.0, 0.0);
    h.radius = 0.0;
    h.always_mesh = false;
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
    float3 a = to_world(inst, cluster_position(pool, geometry, cluster, corners.x));
    float3 b = to_world(inst, cluster_position(pool, geometry, cluster, corners.y));
    float3 c = to_world(inst, cluster_position(pool, geometry, cluster, corners.z));
    // Moved by the wind as raster.slang drew them (the normals are left
    // as they rest: the bend is gentle).
    const Swayer sway = swayer_of(f, inst, mesh);
    if (sway.height > 0.0) {
        a += wind_offset(f, sway, a);
        b += wind_offset(f, sway, b);
        c += wind_offset(f, sway, c);
    }

    const float2 ndc = float2((position.x / f.width) * 2.0 - 1.0, 1.0 - (position.y / f.height) * 2.0);
    const float3 dir = f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz;
    const float4 here = plane_barycentrics(dir, a, b, c);
    const float3 w = on_triangle(here.xyz);
    const float3 object_normal = cluster_normal(pool, geometry, cluster, corners.x) * w.x +
                                 cluster_normal(pool, geometry, cluster, corners.y) * w.y +
                                 cluster_normal(pool, geometry, cluster, corners.z) * w.z;
    Hit h = no_hit();
    h.n = safe_normalize(to_world_normal(inst, object_normal), float3(0.0, 1.0, 0.0));
    h.shading_n = h.n;
    h.world = a * w.x + b * w.y + c * w.z;
    // The material's textures (the CPU keeps the index in range): the
    // texture coordinates and their footprint only for a material with
    // any, the tangent frame only for one with a normal map.
    h.textures = read_buffer(push.materials)
                     .Load4(push.material_offset + min(inst.material, push.material_count) * k_material_bytes + 32);
    if (any(h.textures != k_no_texture)) {
        // How far the uv moves to the neighbouring pixels: their rays met
        // with this triangle's plane (unclamped, so the footprint stays
        // right at its edges).
        const float2 uv0 = cluster_uv(pool, geometry, cluster, corners.x);
        const float2 uv1 = cluster_uv(pool, geometry, cluster, corners.y);
        const float2 uv2 = cluster_uv(pool, geometry, cluster, corners.z);
        h.uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;
        const float2 uv_here = uv0 * here.x + uv1 * here.y + uv2 * here.z;
        h.uv_dx = uv_step(plane_barycentrics(dir + f.ray_x.xyz * (2.0 / f.width), a, b, c), uv_here, uv0, uv1, uv2,
                          float2(1.0, 0.0));
        h.uv_dy = uv_step(plane_barycentrics(dir - f.ray_y.xyz * (2.0 / f.height), a, b, c), uv_here, uv0, uv1, uv2,
                          float2(0.0, 1.0));
    }
    if (h.textures.y != k_no_texture) {
        // The tangent turns with the mesh (not as a normal does); a
        // mirroring instance flips the bitangent.
        const float4 t0 = cluster_tangent(pool, geometry, cluster, corners.x);
        const float3 object_tangent = t0.xyz * w.x + cluster_tangent(pool, geometry, cluster, corners.y).xyz * w.y +
                                      cluster_tangent(pool, geometry, cluster, corners.z).xyz * w.z;
        const float3 world_tangent = float3(dot(inst.model[0].xyz, object_tangent),
                                            dot(inst.model[1].xyz, object_tangent),
                                            dot(inst.model[2].xyz, object_tangent));
        const float det = dot(inst.model[0].xyz, cross(inst.model[1].xyz, inst.model[2].xyz));
        h.tangent = float4(world_tangent, det < 0.0 ? -t0.w : t0.w);
    }
    h.instance = entry.x;
    h.material = inst.material;
    h.cluster = entry.y;
    h.level = cluster.level;
    h.center = to_world(inst, mesh.bounds.xyz);
    h.radius = mesh.bounds.w * inst.max_scale;
    h.always_mesh = inst.always_mesh;
    return h;
}

// --- Material textures ------------------------------------------------------


// A material texture at the pixel's uv, filtered over the pixel's
// footprint (anisotropic, its mip from the ray differentials).
float4 material_sample(uint descriptor, Hit s) {
    return read_texture_nonuniform(descriptor)
        .SampleGrad(get_sampler(k_sampler_anisotropic_wrap), s.uv, s.uv_dx, s.uv_dy);
}

// The shading normal: the surface's bent by the material's normal map
// (x and y in R and G, z rebuilt; in the frame of the tangent, made
// perpendicular to the normal, and the bitangent its sign gives).
float3 bent_normal(Hit s) {
    if (s.textures.y == k_no_texture) {
        return s.n;
    }
    const float2 xy = material_sample(s.textures.y, s).rg * 2.0 - 1.0;
    const float z = sqrt(saturate(1.0 - dot(xy, xy)));
    const float3 t = safe_normalize(s.tangent.xyz - s.n * dot(s.n, s.tangent.xyz), float3(1.0, 0.0, 0.0));
    const float3 b = cross(s.n, t) * s.tangent.w;
    return safe_normalize(t * xy.x + b * xy.y + s.n * z, s.n);
}

// The material's other textures (base colour, roughness and metallic,
// occlusion) over its plain values, as glTF combines them.
void apply_textures(Hit s, inout float3 base, inout float rough, inout float metal, inout float occlusion) {
    if (s.textures.x != k_no_texture) {
        base *= material_sample(s.textures.x, s).rgb; // an sRGB view: linear here
    }
    if (s.textures.z != k_no_texture) {
        const float2 rm = material_sample(s.textures.z, s).rg;
        rough *= rm.x;
        metal *= rm.y;
    }
    if (s.textures.w != k_no_texture) {
        occlusion = material_sample(s.textures.w, s).r;
    }
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
// `cone`: the light a voxel cone along the mirror direction met (rgb) and
// how much it met (a: the rest is the hemisphere), weighted `cone_weight`
// against the hemisphere alone. Out: what the ambient
// reflection is multiplied by (`spec_weight`) and the reflected light it
// used (`spec_light`), for the screen's reflections to replace.
float3 shade(Frame f, float3 world, float3 n, float3 base, float rough, float metal, float3 emissive, float variance,
             float sun, float4 bounce, float4 cone, float cone_weight, float occlusion, out float3 spec_weight,
             out float3 spec_light) {
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

    // Ambient: the sky's light from all round (or the hemisphere of sky
    // above, ground below), for diffuse along the normal and for specular
    // along the reflection.
    const float3 r = reflect(-v, n);
    float3 ambient_diffuse;
    float3 ambient_specular;
    ambient_light2(f, n, r, ambient_diffuse, ambient_specular);
    if (bounce.w > 0.0) {
        // The bounce light takes the flat colours' place; reflections of
        // the sky dim as much as the sky is hidden from here (until they
        // trace rays of their own).
        const float3 gi = lerp(ambient_diffuse, bounce.rgb, bounce.w);
        ambient_specular *= saturate(luminance(gi) / max(luminance(ambient_diffuse), 1e-4));
        ambient_diffuse = gi;
    }
    // The material's occlusion: small creases the voxels are too coarse to
    // see dim the ambient light (and below, everything reflected).
    ambient_diffuse *= occlusion;
    // The voxel cone: what it met, and the dimmed hemisphere past it.
    ambient_specular = lerp(ambient_specular, cone.rgb + (1.0 - cone.a) * ambient_specular, cone_weight);
    const float3 env_fresnel = f0 + (max(1.0 - roughness, f0) - f0) * pow(1.0 - n_dot_v, 5.0);
    spec_weight = env_fresnel * (1.0 - 0.7 * roughness) * occlusion; // the screen's reflections too
    spec_light = ambient_specular;
    color += diffuse_color * ambient_diffuse + spec_weight * ambient_specular;
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

// Octahedral unit vector in 16 + 16 bits (reflect.slang decodes it).
uint pack_octahedral(float3 n) {
    float2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    }
    const uint2 q = uint2(round(saturate(p * 0.5 + 0.5) * 65535.0));
    return q.x | (q.y << 16);
}

struct Output {
    float4 radiance : SV_Target0; // before exposure; a: 1 to tone map, 0 as it is (background, debug views)
    uint2 gloss : SV_Target1;     // x: the screen reflection's weight (10:10:10; 0: none), y: the normal
    float4 reflected : SV_Target2; // rgb: the reflected light shading used, which the screen's replaces
};

Output plain_output(float3 color) {
    Output o;
    o.radiance = float4(color, 0.0);
    o.gloss = uint2(0u, 0u);
    o.reflected = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}

Output lit_output(float3 color) {
    Output o = plain_output(min(color, float3(60000.0, 60000.0, 60000.0)));
    o.radiance.a = 1.0;
    return o;
}

// The clouds in front of what a pixel shows `distance` metres away, then
// the fog over it all (as far as 30 km: the sky's own air does the rest).
float3 clouds_and_fog(Sky s, float3 color, float3 dir, float distance, uint2 pixel, out float through) {
    // The scene fogged over its whole distance, then the clouds over it,
    // themselves fogged only over the distance to the first of them.
    // `through`: how much of the scene's own light comes through.
    // The clear air first (the physical sky's aerial perspective), then the fog.
    const Air air = aerial_at(s, pixel, distance < 1.0e30 ? distance : 0.0);
    color = color * air.through + air.light;
    const float air_through = (air.through.r + air.through.g + air.through.b) * (1.0 / 3.0);
    const float t = fog_transmittance(s, dir, min(distance, 30000.0));
    const float3 fog = fog_color(s, dir);
    color = color * t + fog * (1.0 - t);
    through = air_through * t;
    float first;
    const float4 cloud = clouds_at(s, pixel, distance, first);
    if (cloud.a >= 0.9999) {
        return color;
    }
    through = air_through * t * cloud.a;
    const float near_t = fog_transmittance(s, dir, min(first, 30000.0));
    // The clouds seen through the air before them (the scene's own air
    // already holds that part's light, but only as much as the clouds let through).
    const Air near_air = aerial_at(s, pixel, first);
    const float3 cloud_light = cloud.rgb * near_air.through + near_air.light * (1.0 - cloud.a);
    color = color * cloud.a + cloud_light * near_t + fog * ((1.0 - near_t) * (1.0 - cloud.a));
    return color;
}

// The sky's part over what a pixel shows: clouds, fog, then what falls.
float3 under_sky(Sky s, float3 color, float3 dir, float distance, uint2 pixel, out float through) {
    color = clouds_and_fog(s, color, dir, distance, pixel, through);
    return apply_falling(s, color, dir, distance);
}

// Dust in the air: soft balls (gpu::GpuDustPuff, centres camera-relative)
// whose density falls as (1 - r^2 / R^2)^2 from the centre, taken along the
// ray exactly from the camera to what the pixel shows `scene_t` metres
// away, over `color`, lit by the sun (scattered about evenly, a little
// forward, and again inside the cloud) and by the sky and ground round it.
// `through` (how much of the scene's own light comes through) thins alike.
float dust_integral(float s, float a, float k) {
    // The antiderivative of (a - s^2 k)^2.
    const float s2k = s * s * k;
    return s * (a * a - s2k * (2.0 * a / 3.0 - s2k / 5.0));
}

// The dust's part along a ray: `trans` of what is behind comes through,
// and `scatter` (radiance) is added over it.
void dust_along(Extras e, Frame f, float3 dir, float scene_t, out float trans, out float3 scatter) {
    trans = 1.0;
    scatter = float3(0.0, 0.0, 0.0);
    if (e.dust_count == 0u) {
        return;
    }
    ByteAddressBuffer puffs = read_buffer(e.dust_puffs);
    float depth = 0.0;
    float3 tint = float3(0.0, 0.0, 0.0);
    const uint count = min(e.dust_count, 32u);
    for (uint i = 0u; i < count; ++i) {
        const uint at = e.dust_offset + i * 32u;
        const float4 cr = asfloat(puffs.Load4(at));
        const float4 cd = asfloat(puffs.Load4(at + 16u));
        const float r2 = cr.w * cr.w;
        const float tc = dot(cr.xyz, dir);
        const float3 across = cr.xyz - dir * tc; // (not |c|^2 - tc^2: that cancels far off)
        const float b2 = dot(across, across);
        if (b2 >= r2) {
            continue;
        }
        const float a = 1.0 - b2 / r2;
        const float h = cr.w * sqrt(a);
        const float s0 = max(-h, -tc);
        const float s1 = min(h, scene_t - tc);
        if (s1 <= s0) {
            continue;
        }
        const float k = 1.0 / r2;
        const float d = cd.w * max(dust_integral(s1, a, k) - dust_integral(s0, a, k), 0.0);
        depth += d;
        tint += cd.rgb * d;
    }
    if (!(depth > 1e-5)) {
        return;
    }
    trans = exp(-min(depth, 60.0));
    const float mu = dot(dir, f.sun_direction.xyz);
    const float phase = (1.0 + 0.5 * mu) / (4.0 * k_pi);
    float3 below;
    float3 above;
    ambient_light2(f, float3(0.0, -1.0, 0.0), float3(0.0, 1.0, 0.0), below, above);
    const float3 light = f.sun_light.rgb * (2.0 * phase) + lerp(below, above, 0.6);
    scatter = (tint / depth) * light * (1.0 - trans);
}

float3 apply_dust(Extras e, Frame f, float3 color, float3 dir, float scene_t, inout float through) {
    float trans;
    float3 scatter;
    dust_along(e, f, dir, scene_t, trans, scatter);
    through *= trans;
    return color * trans + scatter;
}

Output shade_pixel(float4 position) {
    const uint2 pixel = uint2(position.xy);
    const uint2 vis = read_texture_uint2(push.vis).Load(int3(int2(pixel), 0));
    const Frame f = load_frame(push.frame, push.frame_offset);
    const bool hit = vis.x != 0;
    Hit s = no_hit();
    if (hit) {
        s = surface_at(f, position, vis);
        s.shading_n = bent_normal(s);
    }
    // Every pixel of the quad reaches this, drawn or not. The shading
    // normal's change: a normal map's bumps widen the highlight too.
    const float variance = normal_variance(s.shading_n, s.instance);

    // The far field: past far_start the meshes are left out (the CPU drops
    // instances wholly beyond it) and the voxel world stands in, in front
    // of whatever mesh is drawn there (lit views only).
    const Extras extras = load_extras();
    const bool lighting_view = f.debug_view == k_view_lighting;
    const bool lit_path = f.debug_view == 0 || lighting_view;
    const bool sky_on = lit_path && extras.sky_on != 0u;
    // (Off, the descriptor is the frame's: read, never used.)
    const Sky sky = load_sky(extras.sky_params, extras.sky_params_offset);
    const float2 pixel_ndc = float2((position.x / f.width) * 2.0 - 1.0, 1.0 - (position.y / f.height) * 2.0);
    const float3 view_dir = normalize(f.ray_x.xyz * pixel_ndc.x + f.ray_y.xyz * pixel_ndc.y + f.ray_z.xyz);
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
        // An always-mesh instance has none, and the stand-ins on it (trees
        // on a terrain tile) must still show.
        const float4 skip = hit && !s.always_mesh && s.radius <= extras.far_radius ? float4(s.center, s.radius)
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
                float sun = sun_visibility(extras.shadows, world, vn, f.sun_direction.xyz, far_pixel);
                if (sky_on) {
                    sun *= cloud_shadow(sky, world);
                }
                // The voxel's own light word.
                float4 bounce = float4(0.0, 0.0, 0.0, 0.0);
                if (extras.gi_on != 0) {
                    const uint lw = write_buffer(extras.gi_light).Load(hit_at * 4);
                    bounce = light_lit(lw) ? float4(unpack_light(lw), 1.0) : bounce;
                }
                float3 albedo = lighting_view ? k_lighting_albedo : unpack_color(packed);
                float far_rough = k_far_roughness;
                float far_metal = 0.0;
                float3 far_n = vn;
                if (sky_on && !lighting_view) {
                    weather_surface(sky, world, albedo, far_rough, far_metal, far_n);
                }
                float3 spec_weight;
                float3 spec_light;
                float3 color = shade(f, world, far_n, albedo, far_rough, far_metal, float3(0.0, 0.0, 0.0), 0.0,
                                     sun, bounce, float4(0.0, 0.0, 0.0, 0.0), 0.0, 1.0, spec_weight, spec_light);
                float far_through = 1.0;
                if (sky_on) {
                    color = under_sky(sky, color, dir, t, pixel, far_through);
                }
                return lit_output(apply_dust(extras, f, color, dir, t, far_through));
            }
        }
    }
    if (!hit) {
        if (sky_on) {
            float through;
            const float3 behind = under_sky(sky, sky_background(sky, view_dir), view_dir, 3.0e38, pixel, through);
            return lit_output(apply_dust(extras, f, behind, view_dir, 3.0e38, through));
        }
        if (lit_path && extras.dust_count != 0u) {
            // The flat background is shown as it is (not exposed): the
            // dust over it exposed and tone mapped alike.
            float trans;
            float3 scatter;
            dust_along(extras, f, view_dir, 3.0e38, trans, scatter);
            const float3 exposed = scatter * f.exposure;
            return plain_output(push.background.rgb * trans +
                                (f.tone_mapper == 1u ? tone_map_agx(exposed) : tone_map(exposed)));
        }
        return plain_output(push.background.rgb);
    }
    // The shading normal (normal map, decals, weather) lights the surface;
    // the surface's own places shadow lookups and light probes, so bumps do
    // not move them.
    float3 n = s.shading_n;
    const float3 surface_n = s.n;
    const float3 world = s.world;

    if (!lit_path) {
        // Flat colour shaded just enough to read the shape.
        const float3 col = f.debug_view == 1 ? hash_color(s.cluster)
                         : f.debug_view == 2 ? level_color(s.level)
                                             : hash_color(s.cluster * 131u + vis.y);
        const float shade_amount = 0.35 + 0.65 * saturate(dot(n, f.sun_direction.xyz) * 0.5 + 0.5);
        return plain_output(col * shade_amount);
    }

    // The instance's material (the CPU keeps the index in range), then
    // the decals over this pixel.
    ByteAddressBuffer materials = read_buffer(push.materials);
    const uint material_at = push.material_offset + min(s.material, push.material_count) * k_material_bytes;
    const float4 base_roughness = load4(materials, material_at);
    const float4 emissive_metallic = load4(materials, material_at + 16);
    float3 base = base_roughness.rgb;
    float rough = base_roughness.a;
    float metal = emissive_metallic.a;
    float3 emissive = emissive_metallic.rgb;
    float occlusion = 1.0;
    apply_textures(s, base, rough, metal, occlusion);
    const float pixel_size = length(world) * 2.0 / max(f.p11 * f.height, 1e-6);
    apply_decals(push.decals, push.decal_offset, push.tile_offset, push.entry_offset, push.tile_columns,
                 push.tile_rows, pixel, world, n, pixel_size, base, rough, metal, emissive);
    // Voxel cones follow the dry surface: rain makes all open ground glossy,
    // and a cone on every wet pixel would cost the frame what a storm must
    // not (the wet gloss shows in the screen's reflections and the sky's).
    const float dry_rough = rough;
    if (sky_on && !lighting_view) {
        weather_surface(sky, world, base, rough, metal, n); // wet, puddles, snow
    }
    if (lighting_view) {
        base = k_lighting_albedo;
        metal = 0.0;
        emissive = float3(0.0, 0.0, 0.0);
    }
    float sun = sun_visibility(extras.shadows, world, surface_n, f.sun_direction.xyz, pixel_size);
    if (sky_on) {
        sun *= cloud_shadow(sky, world);
    }
    float4 bounce = float4(0.0, 0.0, 0.0, 0.0);
    if (extras.gi_on != 0) {
        bounce = voxel_bounce(w, write_buffer(extras.gi_light), world, surface_n);
    }
    // Reflections: the voxel cone along the mirror direction, and the
    // screen's weight for reflect.slang.
    float4 cone = float4(0.0, 0.0, 0.0, 0.0);
    float cone_weight = 0.0;
    if ((extras.reflect_on & 1u) != 0u) {
        cone_weight = saturate((k_voxel_reflection_rough - dry_rough) / k_reflection_fade);
        if (cone_weight > 0.0) {
            const float roughness = clamp(dry_rough, 0.045, 1.0);
            const float3 r = reflect(-safe_normalize(-world, n), n);
            VoxelWorld all_voxels = w;
            all_voxels.stand_ins = 0u; // everything reflects, not only the far field's stand-ins
            cone = trace_reflection(all_voxels, write_buffer(extras.gi_light), f, world, surface_n, r,
                                    clamp(roughness * roughness, 0.03, 0.577));
        }
    }
    float3 spec_weight;
    float3 spec_light;
    float3 color = shade(f, world, n, base, rough, metal, emissive, variance, sun, bounce, cone, cone_weight,
                         occlusion, spec_weight, spec_light);
    float through = 1.0; // of the reflection, which reflect.slang adds over the fog and clouds
    if (sky_on) {
        color = under_sky(sky, color, view_dir, length(world), pixel, through);
    }
    color = apply_dust(extras, f, color, view_dir, length(world), through);
    Output o = lit_output(color);
    const float screen_weight =
        (extras.reflect_on & 2u) != 0u ? saturate((k_screen_reflection_rough - rough) / k_reflection_fade) : 0.0;
    const uint3 q = uint3(round(saturate(spec_weight * (screen_weight * through)) * 1023.0));
    o.gloss = uint2(q.x | (q.y << 10) | (q.z << 20), pack_octahedral(n));
    o.reflected = float4(min(spec_light, float3(60000.0, 60000.0, 60000.0)), 0.0);
    return o;
}

// With screen reflections: the three targets for reflect.slang to finish.
[shader("pixel")]
Output ps_main(float4 position : SV_Position) {
    return shade_pixel(position);
}

// Without: the light alone, for post.slang.
[shader("pixel")]
float4 ps_direct(float4 position : SV_Position) : SV_Target {
    return shade_pixel(position).radiance;
}
