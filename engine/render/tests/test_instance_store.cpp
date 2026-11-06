// The instances kept on the GPU by cell: what is uploaded, and the layout
// the shaders read (common.slang load_instance, instance_drawn).
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant_test.h"
#include "gpu_layout.h"
#include "cells.h"
#include "instance_store.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {
struct Store {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<InstanceStore> store;
    Store() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        store = std::make_unique<InstanceStore>(*device);
    }
    bool update(const std::vector<Instance>& in, const std::vector<CellRange>& cells, const DVec3& camera,
                const std::vector<u32>& drawn, u32 materials = 4) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        const bool ok = store->update(*cmd, in, cells, materials, camera, drawn);
        device->end_frame();
        return ok;
    }
    // What a shader's load_instance gives: the camera-relative translation.
    Vec3 translation(u32 index) {
        const std::vector<u8> b = device->read_buffer(store->buffer());
        gpu::Instance g;
        std::memcpy(&g, b.data() + index * sizeof(g), sizeof(g));
        float a[3];
        std::memcpy(a, b.data() + g.anchor, sizeof(a));
        return {g.model[3] + a[0], g.model[7] + a[1], g.model[11] + a[2]};
    }
    bool drawn(u32 index) {
        const std::vector<u8> b = device->read_buffer(store->buffer());
        u32 word;
        std::memcpy(&word, b.data() + store->skip_offset() + (index / 32) * 4, 4);
        return (word & (1u << (index % 32))) == 0;
    }
};

Instance at(double x, double z) {
    Instance i;
    i.position = {x, 0.0, z};
    return i;
}
} // namespace

GIANT_TEST("instance store: only cells that changed are uploaded; anchors follow the camera") {
    Store s;
    std::vector<Instance> in{at(1000.0, 0.0), at(1010.0, 5.0), at(-20000.0, 3.0)};
    std::vector<CellRange> cells{{0, 2, 1}, {2, 1, 2}};
    GIANT_CHECK(s.update(in, cells, {1000.0, 0.0, 0.0}, {0, 1, 2}));
    GIANT_CHECK(s.store->uploaded() == 3);
    GIANT_CHECK(std::fabs(s.translation(1).x - 10.0f) < 1e-4f && std::fabs(s.translation(1).z - 5.0f) < 1e-4f);
    // The camera moves: nothing uploaded, positions still right.
    GIANT_CHECK(s.update(in, cells, {-20000.0, 0.0, 0.0}, {0, 1, 2}));
    GIANT_CHECK(s.store->uploaded() == 0);
    GIANT_CHECK(std::fabs(s.translation(2).z - 3.0f) < 1e-4f && std::fabs(s.translation(2).x) < 1e-4f);
    GIANT_CHECK(std::fabs(s.translation(0).x - 21000.0f) < 1e-2f);
    // One cell changes.
    in[2].position.z = 7.0;
    cells[1].version = 3;
    GIANT_CHECK(s.update(in, cells, {-20000.0, 0.0, 0.0}, {0, 2}));
    GIANT_CHECK(s.store->uploaded() == 1);
    GIANT_CHECK(std::fabs(s.translation(2).z - 7.0f) < 1e-4f);
    GIANT_CHECK(s.drawn(0) && !s.drawn(1) && s.drawn(2));
    // Another layout, or another material table size: everything.
    cells = {{0, 3, 4}};
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2}));
    GIANT_CHECK(s.store->uploaded() == 3);
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2}, 5));
    GIANT_CHECK(s.store->uploaded() == 3);
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2}, 5));
    GIANT_CHECK(s.store->uploaded() == 0);
}

GIANT_TEST("instance store: without cells everything goes up every frame; it grows") {
    Store s;
    std::vector<Instance> in{at(5.0, 0.0), at(6.0, 0.0)};
    GIANT_CHECK(s.update(in, {}, {5.0, 0.0, 0.0}, {0, 1}));
    GIANT_CHECK(s.update(in, {}, {5.0, 0.0, 0.0}, {0, 1}));
    GIANT_CHECK(s.store->uploaded() == 2);
    GIANT_CHECK(std::fabs(s.translation(1).x - 1.0f) < 1e-5f);
    // Cells that do not cover the instances are not used.
    GIANT_CHECK(s.update(in, {{0, 1, 1}}, {5.0, 0.0, 0.0}, {0, 1}));
    GIANT_CHECK(s.store->uploaded() == 2);
    in.assign(5000, at(1.0, 2.0));
    in[4999].position.x = 3.0;
    std::vector<u32> drawn(5000);
    for (u32 i = 0; i < 5000; ++i) drawn[i] = i;
    GIANT_CHECK(s.update(in, {{0, 5000, 9}}, {0.0, 0.0, 0.0}, drawn));
    GIANT_CHECK(s.store->uploaded() == 5000);
    GIANT_CHECK(std::fabs(s.translation(4999).x - 3.0f) < 1e-5f && s.drawn(4999));
}

GIANT_TEST("instance store: a changed cell after an empty one is found by its slot") {
    Store s;
    std::vector<Instance> in{at(1.0, 0.0), at(3.0, 0.0), at(4.0, 0.0)};
    // The middle cell holds nothing (decals only): it shares `first` with the next.
    std::vector<CellRange> cells{{0, 1, 1}, {1, 0, 2}, {1, 2, 3}};
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2}));
    in[1].position.x = 50.0;
    cells[2].version = 4;
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2}));
    GIANT_CHECK(s.store->uploaded() == 2);
    GIANT_CHECK(std::fabs(s.translation(1).x - 50.0f) < 1e-4f);
    // Two changed cells in a row, one of them empty.
    cells[1].version = 5;
    cells[2].version = 6;
    in[2].position.x = 60.0;
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2}));
    GIANT_CHECK(s.store->uploaded() == 2);
    GIANT_CHECK(std::fabs(s.translation(2).x - 60.0f) < 1e-4f);
}

GIANT_TEST("instance store: cells coming, going and moving upload only themselves") {
    Store s;
    std::vector<Instance> in{at(1.0, 0.0), at(2.0, 0.0), at(100.0, 0.0), at(200.0, 0.0)};
    std::vector<CellRange> cells{{0, 2, 1}, {2, 1, 2}, {3, 1, 3}};
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2, 3}));
    // The middle cell goes (a hole of its own), and a cell joins at the end.
    in[2] = Instance{};
    in[2].mesh = k_no_mesh;
    in.push_back(at(300.0, 0.0));
    in.push_back(at(301.0, 0.0));
    cells = {{0, 2, 1}, {2, 1, 7}, {3, 1, 3}, {4, 2, 8}};
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 3, 4, 5}));
    GIANT_CHECK(s.store->uploaded() == 3);
    for (u32 i : {0u, 1u, 3u, 4u, 5u}) {
        GIANT_CHECK(std::fabs(s.translation(i).x - static_cast<float>(in[i].position.x)) < 1e-3f);
    }
    GIANT_CHECK(!s.drawn(2));
    // The camera moves: the kept cells' anchors still place them.
    GIANT_CHECK(s.update(in, cells, {50.0, 0.0, 0.0}, {0, 1, 3, 4, 5}));
    GIANT_CHECK(s.store->uploaded() == 0);
    for (u32 i : {0u, 1u, 3u, 4u, 5u}) {
        GIANT_CHECK(std::fabs(s.translation(i).x - static_cast<float>(in[i].position.x - 50.0)) < 1e-3f);
    }
    // Two cells join into one place (a hole and its neighbour): just it.
    in[2] = at(150.0, 0.0);
    cells = {{0, 2, 1}, {2, 2, 9}, {4, 2, 8}};
    in[3] = at(151.0, 0.0);
    GIANT_CHECK(s.update(in, cells, {0.0, 0.0, 0.0}, {0, 1, 2, 3, 4, 5}));
    GIANT_CHECK(s.store->uploaded() == 2);
    for (u32 i = 0; i < 6; ++i) {
        GIANT_CHECK(std::fabs(s.translation(i).x - static_cast<float>(in[i].position.x)) < 1e-3f);
    }
}

GIANT_TEST("changed cells: lists the cells not exactly as seen") {
    std::vector<CellRange> seen;
    std::vector<CellRange> changed;
    const std::vector<CellRange> a{{0, 2, 1}, {2, 0, 2}, {2, 3, 3}};
    GIANT_CHECK(!changed_cells(a, 5, seen, changed)); // nothing seen yet
    GIANT_CHECK(changed_cells(a, 5, seen, changed) && changed.empty());
    const std::vector<CellRange> b{{0, 2, 1}, {2, 0, 4}, {2, 3, 3}, {5, 2, 5}};
    GIANT_CHECK(changed_cells(b, 7, seen, changed) && changed.size() == 2);
    GIANT_CHECK(changed[0].version == 4 && changed[1].version == 5);
    const std::vector<CellRange> c{{0, 4, 1}, {4, 3, 6}};
    GIANT_CHECK(changed_cells(c, 7, seen, changed) && changed.size() == 2); // same versions, other places
    GIANT_CHECK(!changed_cells(a, 5, seen, changed)); // fewer instances: everything
    GIANT_CHECK(!changed_cells(std::vector<CellRange>{{0, 2, 1}, {3, 2, 2}}, 5, seen, changed)); // a gap
    GIANT_CHECK(seen.empty());
}
