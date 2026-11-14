// What shading gets beside its push constants (gpu::ResolveExtras: the
// far field, sun shadows, the voxel light, the sky, dust, lamps, ...), and
// the air over what a pixel shows (the clear air, fog, clouds, what falls,
// dust). resolve.slang shades with them; water.slang lights and fogs the
// water's surface with the same.
#pragma once
#include "common.slang"
#include "shadow_pages.slang"
#include "lights.slang"
#include "sky_common.slang"

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
    uint texture_feedback;   // write descriptor: u32 per material (feedback_materials 0: off)
    uint feedback_materials;
    uint dfg;                // the split-sum BRDF table (src/brdf.h): dfg_size^2 float2
    uint dfg_size;
    uint ao;                 // ambient occlusion (ao.slang), half size (ao_on 0: off)
    uint ao_on;
    uint contact;            // contact shadows (ao.slang's cs_contact): the sun let through, full size (contact_on 0: off)
    uint contact_on;
    uint terrain_surface;    // terrain.slang's colour and roughness, full size (terrain_on 0: off)
    uint terrain_normal;     // and its shading normal
    uint terrain_on;
    LocalLights local;       // lamps (lights.slang; count 0: none)
    uint gi_lamps;           // the lamp words beside the voxel light pool (gi_lamps_on 0: none)
    uint gi_lamps_on;
    uint occlusion;          // characters in the light (occluders.slang), full size rgba8 (occlusion_on 0: none)
    uint occlusion_on;
};

Extras load_extras(uint descriptor, uint offset) {
    ByteAddressBuffer b = read_buffer(descriptor);
    const uint at = offset;
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
    o.texture_feedback = m.z;
    o.feedback_materials = m.w;
    const uint4 q = b.Load4(at + 160);
    o.dfg = q.x;
    o.dfg_size = q.y;
    o.ao = q.z;
    o.ao_on = q.w;
    const uint4 c2 = b.Load4(at + 176);
    o.contact = c2.x;
    o.contact_on = c2.y;
    o.local.buffer = c2.z;
    o.local.count = c2.w;
    const uint3 t3 = b.Load3(at + 192);
    o.terrain_surface = t3.x;
    o.terrain_normal = t3.y;
    o.terrain_on = t3.z;
    const float4 px = asfloat(b.Load4(at + 208));
    const float4 py = asfloat(b.Load4(at + 224));
    const float4 pz = asfloat(b.Load4(at + 240));
    o.shadows.prev_on = px.w > 0.5 ? 1u : 0u;
    o.shadows.prev_x = px.xyz;
    o.shadows.prev_y = py.xyz;
    o.shadows.prev_z = pz.xyz;
    o.shadows.prev_depth_offset = pz.w;
    const uint4 lo = b.Load4(at + 256);
    o.local.lights = lo.x;
    o.local.clusters = lo.y;
    o.local.entries = lo.z;
    o.local.near_depth = asfloat(lo.w);
    const uint4 ls = b.Load4(at + 272);
    o.local.scale = asfloat(ls.x);
    o.local.shadows = ls.y;
    o.local.atlas = ls.z;
    o.local.atlas_on = ls.w;
    const uint4 gl = b.Load4(at + 288);
    o.gi_lamps = gl.x;
    o.gi_lamps_on = gl.y;
    o.occlusion = gl.z;
    o.occlusion_on = gl.w;
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
    const float phase = (1.0 + 0.5 * mu) / (4.0 * 3.14159265);
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

