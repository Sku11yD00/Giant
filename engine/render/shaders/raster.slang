// Draws chosen clusters into the visibility buffer: per pixel, which
// cluster (its index in the visible list, plus one; 0 is nothing) and which
// of its triangles. Materials are shaded afterwards (resolve.slang).
//
// The same geometry stages draw sun shadow pages (ps_shadow): seen from the
// sun, over one level's window of pages, into the page atlas.
//
// Mesh shader path: one group per cluster, a thread per vertex and per
// triangle. Fallback for GPUs without mesh shaders: a draw of 384 vertices
// per cluster (instance i is cluster base + i); unused triangles collapse
// to a point, which the rasterizer drops.
//
// Cut-out materials (leaves on cards) draw with their own entry points
// (ms_cut, vs_cut, ps_cut, ps_shadow_cut): the vertices carry uvs, and
// pixels where the base colour's alpha is under the material's cutoff are
// dropped. Opaque materials keep pixel shaders without discard (early depth).
#include "common.slang"

struct RasterPush {
    uint frame;
    uint frame_offset;
    uint instances;
    uint instance_offset;
    uint meshes;
    uint mesh_offset;
    uint visible;        // read descriptor of (instance, cluster) entries
    uint visible_offset;
    uint range;          // read descriptor of (first entry, count) to draw
    uint range_offset;
    // Shadow pages (ps_shadow) only.
    uint shadow_atlas;        // write descriptor: u32 depths, page after page of 128 x 128
    uint shadow_table;        // read descriptor of the level's page table
    uint shadow_table_offset; // its first entry
    int shadow_page_x;        // the level window's minimum page
    int shadow_page_y;
    // Which clusters draw, by their material's kind (bit k for kind k:
    // 0 opaque one-sided, 1 opaque two-sided, 2 cut-out one-sided, 3 cut-out
    // two-sided; gpu::Material flags in the frame's table); 0: all.
    uint kinds;
    uint materials;
    uint material_offset;
    uint material_count;
    uint pad;
};
ConstantBuffer<RasterPush> push : register(b0);

static const uint k_material_bytes = 64; // gpu::Material; textures at +32, flags at +48, cutoff at +56
static const uint k_material_two_sided = 2u;
static const uint k_material_cut_out = 4u;

uint material_at(Instance inst) {
    return push.material_offset + min(inst.material, push.material_count) * k_material_bytes;
}

// Whether this pass draws an instance's clusters (push.kinds).
bool drawn_kind(Instance inst) {
    if (push.kinds == 0u) {
        return true;
    }
    const uint flags = read_buffer(push.materials).Load(material_at(inst) + 48);
    const uint kind = ((flags & k_material_two_sided) != 0u ? 1u : 0u) + ((flags & k_material_cut_out) != 0u ? 2u : 0u);
    return ((push.kinds >> kind) & 1u) != 0u;
}

// A cut-out material's base colour (read descriptor) and alpha cutoff.
uint2 cut_out_of(Instance inst) {
    const uint at = material_at(inst);
    ByteAddressBuffer materials = read_buffer(push.materials);
    return uint2(materials.Load(at + 32), materials.Load(at + 56));
}

struct Drawn {
    Instance inst;
    MeshEntry mesh;
    Cluster cluster;
    uint base;  // its group's geometry in the pool
    uint entry; // into the visible list
    bool valid; // false past the end of the pass's range
};

// The id-th cluster of this pass, for mesh shaders.
// Branch-free: groups past the end (the last row of a 2D dispatch) load the
// last cluster and draw nothing. Mesh shaders need this: DXIL allows one
// SetMeshOutputCounts call, and the optimizer would copy it into both arms
// of a branch on validity.
Drawn drawn_cluster_flat(uint id) {
    Drawn d = {};
    const uint2 range = read_buffer(push.range).Load2(push.range_offset);
    d.valid = id < range.y;
    d.entry = range.x + min(id, max(range.y, 1u) - 1);
    const uint2 e = read_buffer(push.visible).Load2(push.visible_offset + d.entry * 8);
    d.inst = load_instance(read_buffer(push.instances), push.instance_offset + e.x * k_instance_bytes);
    d.mesh = load_mesh(read_buffer(push.meshes), push.mesh_offset + d.inst.mesh * k_mesh_bytes);
    d.cluster = load_cluster(read_buffer_nonuniform(d.mesh.clusters), e.y);
    d.base = group_offset(d.mesh, d.cluster.group);
    // Never not resident (culling checks it); combined without a branch.
    d.valid = (uint(d.valid) & uint(d.base != k_not_resident) & uint(drawn_kind(d.inst))) != 0;
    return d;
}

// The id-th cluster of this pass.
Drawn drawn_cluster(uint id) {
    Drawn d = {};
    const uint2 range = read_buffer(push.range).Load2(push.range_offset);
    d.entry = range.x + id;
    d.valid = id < range.y;
    if (!d.valid) {
        return d;
    }
    const uint2 e = read_buffer(push.visible).Load2(push.visible_offset + d.entry * 8);
    d.inst = load_instance(read_buffer(push.instances), push.instance_offset + e.x * k_instance_bytes);
    d.mesh = load_mesh(read_buffer(push.meshes), push.mesh_offset + d.inst.mesh * k_mesh_bytes);
    d.cluster = load_cluster(read_buffer_nonuniform(d.mesh.clusters), e.y);
    d.base = group_offset(d.mesh, d.cluster.group);
    d.valid = d.base != k_not_resident && drawn_kind(d.inst); // resident: culling checks it
    return d;
}

// Where a vertex is drawn: swayed by the wind (resolve.slang moves it the
// same way, so the triangle it finds is the one drawn).
float4 clip_position(Frame f, Drawn d, Swayer w, uint local_vertex) {
    const float3 p = to_world(d.inst, cluster_position(read_buffer(f.pool), d.base, d.cluster, local_vertex));
    const float3 moved = p + wind_offset(f, w, p);
    return transform(f.view_proj[0], f.view_proj[1], f.view_proj[2], f.view_proj[3], float4(moved, 1.0));
}

struct MsVertex {
    float4 position : SV_Position;
};
struct MsPrimitive {
    uint2 vis : VIS_ID;
};
struct MsCutVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
struct MsCutPrimitive {
    uint2 vis : VIS_ID;
    uint2 cut : CUT_OUT; // base colour descriptor, asuint(cutoff)
};

[shader("mesh")]
[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID,
             OutputVertices<MsVertex, 128> verts, OutputIndices<uint3, 128> tris,
             OutputPrimitives<MsPrimitive, 128> prims) {
    const Drawn d = drawn_cluster_flat(gid.y * 65535 + gid.x);
    const uint vertex_count = d.cluster.vertex_count * uint(d.valid);
    const uint triangle_count = d.cluster.triangle_count * uint(d.valid);
    SetMeshOutputCounts(vertex_count, triangle_count);
    const Frame f = load_frame(push.frame, push.frame_offset);
    if (tid < vertex_count) {
        MsVertex v;
        v.position = clip_position(f, d, swayer_of(f, d.inst, d.mesh), tid);
        verts[tid] = v;
    }
    if (tid < triangle_count) {
        tris[tid] = cluster_triangle(read_buffer(f.pool), d.base, d.cluster, tid);
        MsPrimitive p;
        p.vis = uint2(d.entry + 1, tid);
        prims[tid] = p;
    }
}

// The mesh shader for cut-out materials: the same, with uvs.
[shader("mesh")]
[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void ms_cut(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID,
            OutputVertices<MsCutVertex, 128> verts, OutputIndices<uint3, 128> tris,
            OutputPrimitives<MsCutPrimitive, 128> prims) {
    const Drawn d = drawn_cluster_flat(gid.y * 65535 + gid.x);
    const uint vertex_count = d.cluster.vertex_count * uint(d.valid);
    const uint triangle_count = d.cluster.triangle_count * uint(d.valid);
    SetMeshOutputCounts(vertex_count, triangle_count);
    const Frame f = load_frame(push.frame, push.frame_offset);
    if (tid < vertex_count) {
        MsCutVertex v;
        v.position = clip_position(f, d, swayer_of(f, d.inst, d.mesh), tid);
        v.uv = cluster_uv(read_buffer(f.pool), d.base, d.cluster, tid);
        verts[tid] = v;
    }
    if (tid < triangle_count) {
        tris[tid] = cluster_triangle(read_buffer(f.pool), d.base, d.cluster, tid);
        MsCutPrimitive p;
        p.vis = uint2(d.entry + 1, tid);
        p.cut = cut_out_of(d.inst);
        prims[tid] = p;
    }
}

struct VsOut {
    float4 position : SV_Position;
    nointerpolation uint2 vis : VIS_ID;
};
struct VsCutOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint2 vis : VIS_ID;
    nointerpolation uint2 cut : CUT_OUT;
};

[shader("vertex")]
VsOut vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    VsOut o;
    o.position = float4(0.0, 0.0, 0.0, 1.0);
    o.vis = uint2(0, 0);
    const Drawn d = drawn_cluster(instance_id);
    const uint triangle = vertex_id / 3;
    if (!d.valid || triangle >= d.cluster.triangle_count) {
        return o; // all three corners at one point: nothing drawn
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint3 corners = cluster_triangle(read_buffer(f.pool), d.base, d.cluster, triangle);
    const uint corner = vertex_id % 3;
    const uint local = corner == 0 ? corners.x : corner == 1 ? corners.y : corners.z;
    o.position = clip_position(f, d, swayer_of(f, d.inst, d.mesh), local);
    o.vis = uint2(d.entry + 1, triangle);
    return o;
}

// The vertex shader for cut-out materials: the same, with uvs.
[shader("vertex")]
VsCutOut vs_cut(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    VsCutOut o;
    o.position = float4(0.0, 0.0, 0.0, 1.0);
    o.uv = float2(0.0, 0.0);
    o.vis = uint2(0, 0);
    o.cut = uint2(0xFFFFFFFFu, 0u);
    const Drawn d = drawn_cluster(instance_id);
    const uint triangle = vertex_id / 3;
    if (!d.valid || triangle >= d.cluster.triangle_count) {
        return o; // all three corners at one point: nothing drawn
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint3 corners = cluster_triangle(read_buffer(f.pool), d.base, d.cluster, triangle);
    const uint corner = vertex_id % 3;
    const uint local = corner == 0 ? corners.x : corner == 1 ? corners.y : corners.z;
    o.position = clip_position(f, d, swayer_of(f, d.inst, d.mesh), local);
    o.uv = cluster_uv(read_buffer(f.pool), d.base, d.cluster, local);
    o.vis = uint2(d.entry + 1, triangle);
    o.cut = cut_out_of(d.inst);
    return o;
}

struct PsIn {
    float4 position : SV_Position;
    nointerpolation uint2 vis : VIS_ID;
};
struct PsCutIn {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint2 vis : VIS_ID;
    nointerpolation uint2 cut : CUT_OUT;
};

[shader("pixel")]
uint2 ps_main(PsIn i) : SV_Target {
    return i.vis;
}

// Whether a cut-out pixel is cut away: its base colour's alpha under the
// cutoff (no texture: kept).
bool cut_away(PsCutIn i) {
    if (i.cut.x == 0xFFFFFFFFu) {
        return false;
    }
    const float alpha = read_texture_nonuniform(i.cut.x).Sample(get_sampler(k_sampler_anisotropic_wrap), i.uv).a;
    return alpha < asfloat(i.cut.y);
}

[shader("pixel")]
uint2 ps_cut(PsCutIn i) : SV_Target {
    if (cut_away(i)) {
        discard;
    }
    return i.vis;
}

// Sun shadow pages: the pass covers one level's window (16 x 16 pages of
// 128 x 128 texels; no targets). Each texel keeps the depth nearest the sun
// (larger is nearer), in the physical page its virtual page has, if that
// page is drawn this frame; the others keep what they hold.
static const uint k_shadow_page_texels = 128;
static const uint k_shadow_no_page = 0xFFFFFFFFu;
static const uint k_shadow_draw_bit = 1u << 29;

int shadow_wrap(int v) {
    const int m = v % 16;
    return m < 0 ? m + 16 : m;
}

void shadow_texel(float4 position) {
    const uint2 texel = uint2(position.xy);
    const int2 page = int2(push.shadow_page_x, push.shadow_page_y) + int2(texel / k_shadow_page_texels);
    const uint slot = uint(shadow_wrap(page.x) + 16 * shadow_wrap(page.y));
    const uint entry = read_buffer(push.shadow_table).Load(push.shadow_table_offset + slot * 4);
    if (entry == k_shadow_no_page || (entry & k_shadow_draw_bit) == 0u) {
        return;
    }
    const uint2 local = texel % k_shadow_page_texels;
    const uint index = (entry & 0xFFFFu) * (k_shadow_page_texels * k_shadow_page_texels) +
                       local.y * k_shadow_page_texels + local.x;
    uint ignored;
    write_buffer(push.shadow_atlas).InterlockedMax(index * 4, asuint(saturate(position.z)), ignored);
}

[shader("pixel")]
void ps_shadow(PsIn i) {
    if (i.vis.x == 0u) {
        return; // never (drawn triangles carry their entry plus one); keeps the input linked
    }
    shadow_texel(i.position);
}

// Cut-out materials' shadows: only where they are not cut away.
[shader("pixel")]
void ps_shadow_cut(PsCutIn i) {
    if (i.vis.x == 0u || cut_away(i)) {
        return;
    }
    shadow_texel(i.position);
}
