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
    uint local_size; // local light shadow tiles: their side (0: sun pages); the tile's first word is shadow_table_offset
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
// same way, so the triangle it finds is the one drawn), and grown across
// the screen so that nothing is thinner than a pixel (common.slang's
// grown). Only x and y move, by what the grown point's would be: the depth
// and w stay the vertex's own, so a grown thing is no nearer the eye, and
// a flat face's every point moves by its own depth's half pixel however
// large the face and wherever its corners are (the move is linear in the
// place: clipping and interpolation are exact, also across the eye's
// plane), so a floor does not rise under what stands on it. (In a shadow
// map a face the sun grazes is in effect a little nearer the sun for
// having slid across it: architecture.md, "Growing".)
float4 clip_position(Frame f, Drawn d, Swayer w, uint local_vertex) {
    const float3 p = to_world(d.inst, cluster_position(read_buffer(f.pool), d.base, d.cluster, local_vertex));
    const float3 swayed = p + wind_offset(f, w, p);
    const float4 clip = transform(f.view_proj[0], f.view_proj[1], f.view_proj[2], f.view_proj[3], float4(swayed, 1.0));
    const float3 out_by = grown(d.inst, cluster_grow(read_buffer(f.pool), d.base, d.cluster, local_vertex),
                                grow_by(f, depth_ahead(f, swayed)));
    const float4 moved = transform(f.view_proj[0], f.view_proj[1], f.view_proj[2], f.view_proj[3],
                                   float4(swayed + out_by, 1.0));
    return float4(moved.xy, clip.zw);
}

// A lamp's shadow face (local_size set): whether the cluster is inside the
// face. A light's faces share one culling (the box round its reach), so
// each face drops here what lies in the others (sun pages: culled per
// level already, always).
// Branch-free (as drawn_cluster_flat: a known 0 count could have the
// compiler copy SetMeshOutputCounts into two arms).
uint in_face(Frame f, Drawn d) {
    const float3 centre = to_world(d.inst, d.cluster.bounds.xyz);
    const float radius = d.cluster.bounds.w * d.inst.max_scale + wind_reach(f, swayer_of(f, d.inst, d.mesh));
    float worst = 1.0e30;
    [unroll] for (uint p = 0; p < 5; ++p) {
        worst = min(worst, dot(f.planes[p].xyz, centre) + f.planes[p].w + radius);
    }
    return uint(push.local_size == 0u) | uint(worst >= 0.0);
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
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint drawn = uint(d.valid) & in_face(f, d);
    const uint vertex_count = d.cluster.vertex_count * drawn;
    const uint triangle_count = d.cluster.triangle_count * drawn;
    SetMeshOutputCounts(vertex_count, triangle_count);
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
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint drawn = uint(d.valid) & in_face(f, d);
    const uint vertex_count = d.cluster.vertex_count * drawn;
    const uint triangle_count = d.cluster.triangle_count * drawn;
    SetMeshOutputCounts(vertex_count, triangle_count);
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

// Cut-out materials drawn with four samples a pixel (multisampling): how
// many of the pixel's samples the cut-out keeps, from how far its alpha is
// over the cutoff against how fast it changes across the pixel, so a leaf's
// edge inside its card is as smooth as the card's own edge (cut whole
// pixels at a time, it would crawl as the card moves). Which samples are
// kept for a count goes by the triangle's place in its cluster (the same
// from frame to frame, which its place in the frame's visible list is
// not), so two cards one behind the other mostly do not keep the same ones
// and hide each other wholly. The alpha is read inside the card: a pixel whose middle the
// card does not cover is drawn for the samples it does, and the uv at its
// middle would be past the card's edge (in an atlas, in the next leaf).
[shader("pixel")]
uint2 ps_cut_samples(PsCutIn i, in uint covered : SV_Coverage, out uint coverage : SV_Coverage) : SV_Target {
    coverage = 0xFu;
    if (i.cut.x != 0xFFFFFFFFu) {
        // The middle of the samples the card covers, from the pixel's
        // middle, and the uv there (uv is linear across the screen as far
        // as a pixel goes).
        float2 middle = float2(0.0, 0.0);
        float samples = 0.0;
        [unroll] for (uint s = 0; s < 4; ++s) {
            if ((covered & (1u << s)) != 0u) {
                middle += GetRenderTargetSamplePosition(int(s));
                samples += 1.0;
            }
        }
        middle /= max(samples, 1.0);
        const float2 inside = i.uv + ddx(i.uv) * middle.x + ddy(i.uv) * middle.y;
        const float alpha = read_texture_nonuniform(i.cut.x)
                                .SampleGrad(get_sampler(k_sampler_anisotropic_wrap), inside, ddx(i.uv), ddy(i.uv))
                                .a;
        const float kept = saturate((alpha - asfloat(i.cut.y)) / max(fwidth(alpha), 1.0e-4) + 0.5);
        const uint count = uint(round(kept * 4.0));
        if (count == 0u) {
            discard;
        }
        // 1: one sample; 2: two opposite ones; 3: all but one; 4: all.
        const uint turn = (i.vis.y * 5u + (i.vis.y >> 2u)) & 3u;
        coverage = count == 1u ? 1u << turn : count == 2u ? ((turn & 1u) != 0u ? 0x6u : 0x9u)
                 : count == 3u ? 0xFu ^ (1u << turn) : 0xFu;
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
    if (push.local_size != 0u) {
        // A local light's tile: the pass covers it alone.
        if (texel.x >= push.local_size || texel.y >= push.local_size) {
            return;
        }
        uint kept;
        write_buffer(push.shadow_atlas)
            .InterlockedMax((push.shadow_table_offset + texel.y * push.local_size + texel.x) * 4,
                            asuint(saturate(position.z)), kept);
        return;
    }
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
