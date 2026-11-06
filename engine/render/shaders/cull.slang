// GPU cluster culling: the LOD cut of geometry::select_clusters, per
// instance, on the GPU, with frustum and two-pass occlusion culling.
//
// Pass 0 tests against the HZB of the last frame; whatever it hides is kept
// aside (whole instances, or single clusters) and tested again in pass 1
// against the HZB of what pass 0 drew, so nothing visible is lost even when
// the camera or objects moved. Work flows through queues of
// (instance, node) items, one tree level per dispatch; dispatch sizes come
// from counters on the GPU (cs_prepare writes the indirect arguments).
//
// Counter slots and layouts match engine/render/src/gpu_layout.h.
#include "common.slang"

static const uint k_slot_zero = 0;
static const uint k_slot_visible = 1;
static const uint k_slot_visible_pass0 = 2;
static const uint k_slot_occluded_instances = 3;
static const uint k_slot_occluded_clusters = 4;
static const uint k_slot_overflow = 5;
static const uint k_slot_triangles = 6;
static const uint k_slot_instances = 7;
static const uint k_slot_range = 60; // base and count of the clusters a raster pass draws
static const uint k_slot_requests = 62; // streaming requests this frame
static const uint k_counter_slots = 64;

static const uint k_overflow_queue = 1;
static const uint k_overflow_visible = 2;
static const uint k_overflow_occluded_clusters = 4;
static const uint k_overflow_occluded_instances = 8;

static const uint k_entry_bytes = 8; // (instance, node) or (instance, cluster)

struct CullPush {
    uint frame;
    uint frame_offset;
    uint instances;
    uint instance_offset;
    uint meshes;
    uint mesh_offset;
    uint counters;           // write descriptors from here on, unless noted
    uint queue_in;
    uint queue_out;
    uint visible;
    uint occluded_instances;
    uint occluded_clusters;
    uint hzb;                // read descriptor
    uint args;
    uint pass;               // 0 or 1
    uint slot_in;            // counter of queue_in's items (cs_prepare: of what to cover)
    uint slot_out;           // counter of queue_out's items
    uint queue_capacity;     // items per queue (cs_prepare: capacity of what it covers)
    uint visible_capacity;
    uint occluded_capacity;  // clusters
    uint instance_capacity;  // occluded instances
    uint mode;               // cs_prepare: 0 dispatch arguments, 1 raster arguments
    uint arg_offset;         // cs_prepare: where the arguments go, bytes
    uint requests;           // streaming: (mesh, group) pairs wanted but not loaded
    uint request_capacity;
    uint usage;              // streaming: per global group, the last frame that used it
    uint pad0;
    uint pad1;
};
ConstantBuffer<CullPush> push : register(b0);

RWByteAddressBuffer counters() { return write_buffer(push.counters); }

uint counter(uint slot) { return counters().Load(slot * 4); }

void flag_overflow(uint bit) {
    uint ignored;
    counters().InterlockedOr(k_slot_overflow * 4, bit, ignored);
}

// Reserves `count` entries of a list; returns the first, or ~0 when the
// list is full (and flags the overflow).
uint reserve(uint slot, uint count, uint capacity, uint overflow_bit) {
    uint first;
    counters().InterlockedAdd(slot * 4, count, first);
    if (first + count > capacity) {
        flag_overflow(overflow_bit);
    }
    return first;
}

Instance instance_at(uint index) {
    return load_instance(read_buffer(push.instances), push.instance_offset + index * k_instance_bytes);
}

MeshEntry mesh_of(Instance i) {
    return load_mesh(read_buffer(push.meshes), push.mesh_offset + i.mesh * k_mesh_bytes);
}

void append_visible(uint instance, uint cluster, uint triangles) {
    const uint at = reserve(k_slot_visible, 1, push.visible_capacity, k_overflow_visible);
    if (at < push.visible_capacity) {
        write_buffer(push.visible).Store2(at * k_entry_bytes, uint2(instance, cluster));
        uint ignored;
        counters().InterlockedAdd(k_slot_triangles * 4, triangles, ignored);
    }
}

void emit_roots(uint instance, MeshEntry mesh) {
    const uint first = reserve(push.slot_out, mesh.levels, push.queue_capacity, k_overflow_queue);
    RWByteAddressBuffer queue = write_buffer(push.queue_out);
    for (uint r = 0; r < mesh.levels; ++r) {
        if (first + r < push.queue_capacity) {
            queue.Store2((first + r) * k_entry_bytes, uint2(instance, r));
        }
    }
}

// Zeroes every counter at the start of the frame.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_reset(uint3 id : SV_DispatchThreadID) {
    if (id.x < k_counter_slots) {
        counters().Store(id.x * 4, 0u);
    }
}

// Writes indirect arguments from counters (one thread).
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_prepare(uint3 id : SV_DispatchThreadID) {
    RWByteAddressBuffer args = write_buffer(push.args);
    if (push.mode == 0) {
        const uint n = min(counter(push.slot_in), push.queue_capacity);
        args.Store3(push.arg_offset, uint3((n + 63) / 64, 1, 1));
        return;
    }
    // Raster: pass 0 draws everything found so far; pass 1 what came after.
    const uint total = min(counter(k_slot_visible), push.visible_capacity);
    if (push.pass == 0) {
        counters().Store(k_slot_visible_pass0 * 4, total);
    }
    const uint base = push.pass == 0 ? 0 : counter(k_slot_visible_pass0);
    const uint count = total > base ? total - base : 0;
    counters().Store2(k_slot_range * 4, uint2(base, count));
    // Mesh groups: at most 65535 per dimension.
    const uint x = min(count, 65535u);
    const uint y = count == 0 ? 1 : (count + 65534) / 65535;
    args.Store3(push.arg_offset, uint3(x, y, 1));
    args.Store4(push.arg_offset + 16, uint4(128 * 3, count, 0, 0)); // vertex fallback
}

// Pass 0: every instance. Pass 1: those pass 0 found hidden.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_instances(uint3 id : SV_DispatchThreadID) {
    const Frame f = load_frame(push.frame, push.frame_offset);
    uint index = id.x;
    if (push.pass == 0) {
        if (index >= f.instance_count || !instance_drawn(f, read_buffer(push.instances), index)) {
            return;
        }
    } else {
        if (index >= min(counter(k_slot_occluded_instances), push.instance_capacity)) {
            return;
        }
        index = write_buffer(push.occluded_instances).Load(index * 4);
    }
    const Instance inst = instance_at(index);
    const MeshEntry mesh = mesh_of(inst);
    if (mesh.levels == 0) {
        return;
    }
    const float3 c = to_world(inst, mesh.bounds.xyz);
    const float r = mesh.bounds.w * inst.max_scale;
    if (push.pass == 0 && !sphere_in_frustum(f, c, r)) {
        return;
    }
    if (occluded(f, read_buffer(push.hzb), c, r, push.pass == 0)) {
        if (push.pass == 1) {
            return;
        }
        const uint at = reserve(k_slot_occluded_instances, 1, push.instance_capacity,
                                k_overflow_occluded_instances);
        if (at < push.instance_capacity) {
            write_buffer(push.occluded_instances).Store(at * 4, index);
            return;
        }
        // No room to retest it in pass 1: walk it now, as if visible.
    }
    uint ignored;
    counters().InterlockedAdd(k_slot_instances * 4, 1, ignored);
    emit_roots(index, mesh);
}

// Streaming: asks for a group's geometry (read back by the CPU, which
// loads it within a budget; until then coarser clusters stand in).
void request_group(uint mesh, uint group) {
    uint at;
    counters().InterlockedAdd(k_slot_requests * 4, 1, at);
    if (at < push.request_capacity) {
        write_buffer(push.requests).Store2(at * k_entry_bytes, uint2(mesh, group));
    }
}

// One cluster of a group whose replacement is too coarse: drawn if its
// own detail is enough (or cannot get finer, or the finer version is not
// loaded yet), in view and not hidden.
void test_cluster(Frame f, uint instance, Instance inst, MeshEntry mesh, uint index) {
    const Cluster c = load_cluster(read_buffer_nonuniform(mesh.clusters), index);
    if (c.refined != k_no_group && group_offset(mesh, c.refined) != k_not_resident) {
        const Group finer = load_group(read_buffer_nonuniform(mesh.groups), c.refined);
        if (projected_error(f, inst, finer.bounds, finer.error) > f.threshold) {
            return; // a finer version is drawn instead
        }
    }
    const float3 centre = to_world(inst, c.bounds.xyz);
    const float radius = c.bounds.w * inst.max_scale;
    if (!sphere_in_frustum(f, centre, radius)) {
        return;
    }
    if (occluded(f, read_buffer(push.hzb), centre, radius, push.pass == 0)) {
        if (push.pass == 1) {
            return;
        }
        const uint at = reserve(k_slot_occluded_clusters, 1, push.occluded_capacity,
                                k_overflow_occluded_clusters);
        if (at < push.occluded_capacity) {
            write_buffer(push.occluded_clusters).Store2(at * k_entry_bytes, uint2(instance, index));
            return;
        }
        // No room to retest it in pass 1: draw it now (costs time, not detail).
    }
    append_visible(instance, index, c.triangle_count);
}

// One level of the LOD trees: each item is an (instance, node).
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_traverse(uint3 id : SV_DispatchThreadID) {
    if (id.x >= min(counter(push.slot_in), push.queue_capacity)) {
        return;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint2 item = write_buffer(push.queue_in).Load2(id.x * k_entry_bytes);
    const Instance inst = instance_at(item.x);
    const MeshEntry mesh = mesh_of(inst);
    const Node node = load_node(read_buffer_nonuniform(mesh.nodes), item.y);
    // Precise enough for this view: everything below is drawn coarser.
    if (projected_error(f, inst, node.bounds, node.error) <= f.threshold) {
        return;
    }
    if (node.group == k_no_group) {
        const uint first = reserve(push.slot_out, node.child_count, push.queue_capacity, k_overflow_queue);
        RWByteAddressBuffer queue = write_buffer(push.queue_out);
        for (uint i = 0; i < node.child_count; ++i) {
            if (first + i < push.queue_capacity) {
                queue.Store2((first + i) * k_entry_bytes, uint2(item.x, node.child_offset + i));
            }
        }
        return;
    }
    const Group group = load_group(read_buffer_nonuniform(mesh.groups), node.group);
    if (projected_error(f, inst, group.bounds, group.error) <= f.threshold) {
        return;
    }
    // Needed here. Not loaded: ask for it; the coarser clusters made from
    // it are drawn meanwhile. Loaded: mark it used, so it stays.
    if (group_offset(mesh, node.group) == k_not_resident) {
        // Once per group and frame, however many instances need it: the
        // usage word of a group that is not loaded marks the request.
        uint previous;
        write_buffer(push.usage).InterlockedExchange((mesh.group_base + node.group) * 4, f.frame_number, previous);
        if (previous != f.frame_number) {
            request_group(inst.mesh, node.group);
        }
        return;
    }
    write_buffer(push.usage).Store((mesh.group_base + node.group) * 4, f.frame_number);
    for (uint i = 0; i < group.cluster_count; ++i) {
        test_cluster(f, item.x, inst, mesh, group.first_cluster + i);
    }
}

// Pass 1: clusters pass 0 found hidden, against the new HZB.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_retest(uint3 id : SV_DispatchThreadID) {
    if (id.x >= min(counter(k_slot_occluded_clusters), push.occluded_capacity)) {
        return;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint2 entry = write_buffer(push.occluded_clusters).Load2(id.x * k_entry_bytes);
    const Instance inst = instance_at(entry.x);
    const MeshEntry mesh = mesh_of(inst);
    const Cluster c = load_cluster(read_buffer_nonuniform(mesh.clusters), entry.y);
    const float3 centre = to_world(inst, c.bounds.xyz);
    if (!occluded(f, read_buffer(push.hzb), centre, c.bounds.w * inst.max_scale, false)) {
        append_visible(entry.x, entry.y, c.triangle_count);
    }
}
