#include "giant/core/jobs.h"

#include "giant_test.h"

#include <atomic>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

using namespace giant;

GIANT_TEST("jobs: every submitted job runs before wait returns") {
    JobSystem jobs(4);
    WaitGroup group;
    std::atomic<int> ran{0};
    for (int i = 0; i < 1000; ++i) {
        jobs.submit(group, [&ran] { ran.fetch_add(1); });
    }
    jobs.wait(group);
    GIANT_CHECK(ran.load() == 1000);
    GIANT_CHECK(group.done());
}

GIANT_TEST("jobs: parallel_for covers every index exactly once") {
    JobSystem jobs(3);
    std::vector<int> hits(10'007, 0);
    jobs.parallel_for(hits.size(), 64, [&hits](usize begin, usize end) {
        for (usize i = begin; i < end; ++i) {
            hits[i] += 1;
        }
    });
    GIANT_CHECK(std::accumulate(hits.begin(), hits.end(), 0) == 10'007);
    bool all_once = true;
    for (int h : hits) {
        all_once = all_once && h == 1;
    }
    GIANT_CHECK(all_once);
}

GIANT_TEST("jobs: parallel_for with zero items does nothing") {
    JobSystem jobs(1);
    bool called = false;
    jobs.parallel_for(0, 8, [&called](usize, usize) { called = true; });
    GIANT_CHECK(!called);
}

// Streaming a cell spawns jobs for its contents; a job that waits on its own
// children must not deadlock even with a single worker.
GIANT_TEST("jobs: waiting inside a job does not deadlock") {
    JobSystem jobs(1);
    WaitGroup outer;
    std::atomic<int> children{0};
    jobs.submit(outer, [&jobs, &children] {
        WaitGroup inner;
        for (int i = 0; i < 50; ++i) {
            jobs.submit(inner, [&children] { children.fetch_add(1); });
        }
        jobs.wait(inner);
    });
    jobs.wait(outer);
    GIANT_CHECK(children.load() == 50);
}

namespace {
struct DestroyFlag {
    std::atomic<bool>* destroyed;
    ~DestroyFlag() { destroyed->store(true); }
};
} // namespace

// Review finding: captures used to be destroyed after wait() returned, racing
// with the caller freeing what they point at.
GIANT_TEST("jobs: a job's captures are destroyed before wait returns") {
    JobSystem jobs(2);
    for (int round = 0; round < 200; ++round) {
        std::atomic<bool> destroyed{false};
        WaitGroup group;
        {
            auto flag = std::make_shared<DestroyFlag>(DestroyFlag{&destroyed});
            jobs.submit(group, [flag] { (void)flag; });
        }
        jobs.wait(group);
        GIANT_CHECK(destroyed.load());
    }
}

// Review finding: destroying the system used to drop queued jobs, leaving
// their WaitGroups pending forever.
GIANT_TEST("jobs: destroying the system runs every queued job") {
    std::atomic<int> ran{0};
    WaitGroup group;
    {
        JobSystem jobs(1);
        for (int i = 0; i < 100; ++i) {
            jobs.submit(group, [&ran] { ran.fetch_add(1); });
        }
    }
    GIANT_CHECK(ran.load() == 100);
    GIANT_CHECK(group.done());
}

GIANT_TEST("jobs: several threads submit and wait at once") {
    JobSystem jobs(3);
    std::atomic<int> ran{0};
    std::vector<std::thread> submitters;
    for (int t = 0; t < 4; ++t) {
        submitters.emplace_back([&jobs, &ran] {
            WaitGroup group;
            for (int i = 0; i < 500; ++i) {
                jobs.submit(group, [&ran] { ran.fetch_add(1); });
            }
            jobs.wait(group);
        });
    }
    for (std::thread& t : submitters) {
        t.join();
    }
    GIANT_CHECK(ran.load() == 2000);
}

GIANT_TEST("jobs: parallel_for inside a job") {
    JobSystem jobs(2);
    WaitGroup group;
    std::atomic<int> sum{0};
    jobs.submit(group, [&jobs, &sum] {
        jobs.parallel_for(1000, 10, [&sum](usize begin, usize end) {
            sum.fetch_add(static_cast<int>(end - begin));
        });
    });
    jobs.wait(group);
    GIANT_CHECK(sum.load() == 1000);
}
