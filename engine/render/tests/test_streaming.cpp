// The cluster streamer on its own (src/streaming.h), on the null device:
// pool bookkeeping, loading order, budgets and eviction.
#include "giant/geometry/clusters.h"
#include "giant/geometry/mesh.h"
#include "giant/rhi/rhi.h"
#include "giant_test.h"
#include "streaming.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

using namespace giant;
using namespace giant::render;

GIANT_TEST("streaming: the pool allocator fits, merges and refuses") {
    PoolAllocator a(100);
    const u64 x = a.allocate(10);
    const u64 y = a.allocate(20);
    const u64 z = a.allocate(30);
    GIANT_CHECK(x == 0 && y == 10 && z == 30 && a.used() == 60);
    GIANT_CHECK(a.allocate(41) == PoolAllocator::k_none); // 40 left
    a.free(y, 20);
    GIANT_CHECK(a.allocate(15) == y); // best fit: the 20-unit hole, not the 40 at the end
    a.free(y, 15);
    a.free(x, 10);
    a.free(z, 30);
    GIANT_CHECK(a.used() == 0 && a.free_ranges() == 1 && a.largest_free() == 100); // all merged back
    GIANT_CHECK(a.allocate(0) == PoolAllocator::k_none);

    // Random churn keeps the books straight.
    PoolAllocator b(1 << 16);
    std::mt19937 rng(7);
    std::vector<std::pair<u64, u64>> live;
    u64 used = 0;
    bool ok = true;
    for (int i = 0; i < 4000; ++i) {
        if (live.empty() || rng() % 3 != 0) {
            const u64 n = 1 + rng() % 300;
            const u64 at = b.allocate(n);
            if (at != PoolAllocator::k_none) {
                for (const auto& [o, s] : live) ok = ok && (at + n <= o || o + s <= at); // no overlap
                live.push_back({at, n});
                used += n;
            }
        } else {
            const usize k = rng() % live.size();
            b.free(live[k].first, live[k].second);
            used -= live[k].second;
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
        }
        ok = ok && b.used() == used;
    }
    GIANT_CHECK(ok);
    for (const auto& [o, s] : live) b.free(o, s);
    GIANT_CHECK(b.used() == 0 && b.free_ranges() == 1);
}

namespace {

struct Bench {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<GeometryStreamer> streamer;
    geometry::ClusterMesh mesh = geometry::build_clusters(geometry::make_sphere(128, 64));
    geometry::GroupLinks links = geometry::link_groups(mesh);
    u32 frame = 0;

    explicit Bench(u64 pool_bytes) {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        streamer = std::make_unique<GeometryStreamer>(*device, pool_bytes);
    }
    bool add(u32 id) {
        std::vector<gpu::Cluster> clusters(mesh.clusters.size());
        return streamer->add(id, mesh, clusters);
    }
    void step() {
        ++frame;
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        streamer->update(*cmd, frame);
        device->end_frame();
    }
    std::vector<u32> all_groups() const {
        std::vector<u32> g(mesh.groups.size());
        std::iota(g.begin(), g.end(), 0u);
        return g;
    }
    u64 bytes_of_all() const {
        // Every group's geometry, as laid out (a lower bound: rounding to units).
        u64 total = 0;
        for (const geometry::Cluster& c : mesh.clusters) total += c.vertex_count * 24ull + c.triangle_count * 4ull;
        return total;
    }
    // Every loaded group's parents are loaded (the cut stays whole).
    bool closed(u32 id) const {
        const std::vector<u8>& r = streamer->resident(id);
        for (u32 g = 0; g < r.size(); ++g) {
            if (r[g] == 0) continue;
            for (const u32 p : links.parents_of(g)) {
                if (r[p] == 0) return false;
            }
        }
        return true;
    }
    u32 loaded(u32 id) const {
        const std::vector<u8>& r = streamer->resident(id);
        return static_cast<u32>(std::count(r.begin(), r.end(), u8{1}));
    }
};

} // namespace

GIANT_TEST("streaming: the coarsest level loads first, then what is asked for") {
    Bench b(64ull << 20);
    GIANT_CHECK(b.streamer->init());
    GIANT_CHECK(b.add(0));
    GIANT_CHECK(!b.streamer->ready(0)); // nothing loaded before the first frame
    b.step();
    GIANT_CHECK(b.streamer->ready(0));
    u32 roots = 0;
    for (const geometry::ClusterGroup& g : b.mesh.groups) roots += geometry::is_root_group(g) ? 1u : 0u;
    GIANT_CHECK(b.loaded(0) == roots && b.streamer->stats().loaded == roots);

    // Ask for the finest groups only: their parents come along, coarse first.
    std::vector<u32> finest;
    for (u32 g = 0; g < b.mesh.groups.size(); ++g) {
        if (b.mesh.groups[g].depth == 0) finest.push_back(g);
    }
    b.streamer->want(0, finest);
    b.step();
    GIANT_CHECK(b.closed(0));
    GIANT_CHECK(b.loaded(0) == b.mesh.groups.size()); // everything is an ancestor of some finest group
    GIANT_CHECK(b.streamer->stats().waiting == 0);
    GIANT_CHECK(b.streamer->stats().used_bytes >= b.bytes_of_all());
    // Asking again loads nothing; bad requests are ignored.
    b.streamer->want(0, finest);
    const u32 bad[] = {1u << 30};
    b.streamer->want(0, bad);
    b.streamer->want(7, bad);
    b.step();
    GIANT_CHECK(b.streamer->stats().loaded == 0);
}

GIANT_TEST("streaming: the per-frame budget spreads loading over frames") {
    Bench b(64ull << 20);
    GIANT_CHECK(b.streamer->init());
    GeometryStreamer::Budget budget;
    budget.groups_per_frame = 3;
    b.streamer->set_budget(budget);
    GIANT_CHECK(b.add(0));
    b.step(); // roots, whatever the budget
    const u32 total = static_cast<u32>(b.mesh.groups.size());
    u32 frames = 0;
    bool within = true;
    bool closed = true;
    while (b.loaded(0) < total && frames < 1000) {
        b.streamer->want(0, b.all_groups());
        b.step();
        within = within && b.streamer->stats().loaded <= 3;
        closed = closed && b.closed(0);
        ++frames;
    }
    GIANT_CHECK(within && closed);
    GIANT_CHECK(b.loaded(0) == total);
    GIANT_CHECK(frames >= (total - 1) / 3);
}

GIANT_TEST("streaming: a small pool evicts what went unused, never the coarsest level") {
    Bench probe(64ull << 20);
    GIANT_CHECK(probe.streamer->init() && probe.add(0));
    probe.step();
    const u64 roots_bytes = probe.streamer->stats().used_bytes;
    probe.streamer->want(0, probe.all_groups());
    probe.step();
    const u64 all_bytes = probe.streamer->stats().used_bytes;
    GIANT_CHECK(all_bytes > roots_bytes);

    // Room for two meshes' coarsest levels and about half of one mesh more.
    Bench b(2 * roots_bytes + (all_bytes - roots_bytes) / 2);
    GIANT_CHECK(b.streamer->init() && b.add(0) && b.add(1));
    b.step();
    GIANT_CHECK(b.streamer->ready(0) && b.streamer->ready(1));
    // Mesh 0 fills the pool; it stays in use, so mesh 1 waits.
    for (int i = 0; i < 4; ++i) {
        b.streamer->want(0, b.all_groups());
        b.streamer->touch(0, b.all_groups(), b.frame + 1);
        b.step();
    }
    const u32 kept = b.loaded(0);
    GIANT_CHECK(kept > 0 && b.closed(0));
    b.streamer->want(1, b.all_groups());
    b.step();
    GIANT_CHECK(b.streamer->stats().evicted == 0 && b.streamer->stats().waiting > 0);
    // Mesh 0 goes unused: once old enough its detail makes room for mesh 1.
    bool closed = true;
    u32 evicted = 0;
    for (u32 i = 0; i < GeometryStreamer::k_keep_frames + 40; ++i) {
        b.streamer->want(1, b.all_groups());
        b.streamer->touch(1, b.all_groups(), b.frame + 1);
        b.step();
        evicted += b.streamer->stats().evicted;
        closed = closed && b.closed(0) && b.closed(1);
        GIANT_CHECK(b.streamer->stats().used_bytes <= b.streamer->stats().pool_bytes);
    }
    GIANT_CHECK(evicted > 0 && closed);
    GIANT_CHECK(b.loaded(1) > b.loaded(0));
    GIANT_CHECK(b.streamer->ready(0)); // its coarsest level stays
    // Removing a mesh frees its share.
    const u64 before = b.streamer->stats().used_bytes;
    b.streamer->remove(1);
    GIANT_CHECK(b.streamer->stats().used_bytes < before);
    GIANT_CHECK(!b.streamer->ready(1) && b.streamer->resident(1).empty());
}

GIANT_TEST("streaming: a pool too small for the coarsest level leaves the mesh undrawn") {
    Bench b(1024);
    GIANT_CHECK(b.streamer->init() && b.add(0));
    b.step();
    GIANT_CHECK(!b.streamer->ready(0));
    GIANT_CHECK(b.closed(0));
}
