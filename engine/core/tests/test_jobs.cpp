#include "giant/core/jobs.h"

#include "giant_test.h"

#include <atomic>
#include <chrono>
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

namespace {
// Until `done` (or five seconds: a job never run fails the check after, rather than hang).
template <typename Done> void until(const Done& done) {
    const auto start = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace

// submit() pushes onto a list no lock guards: what one thread submits is still run in the
// order it submitted it (one worker, and nobody helping).
GIANT_TEST("jobs: one thread's jobs run in the order it submitted them") {
    WaitGroup group;
    std::vector<int> order; // (the one worker's)
    std::atomic<int> ran{0};
    JobSystem jobs(1); // (last: destroyed first, whatever a check below finds, so no job outlives what it writes)
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < 100; ++i) {
            jobs.submit(group, [&order, &ran, n = round * 100 + i] {
                order.push_back(n);
                ran.fetch_add(1);
            });
        }
        if (round % 4 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // (the worker asleep for some, busy for others)
        }
    }
    until([&ran] { return ran.load() == 2000; });
    GIANT_CHECK(ran.load() == 2000);
    until([&group] { return group.done(); });
    GIANT_CHECK(group.done());
    bool in_order = order.size() == 2000;
    for (usize i = 0; in_order && i < order.size(); ++i) {
        in_order = order[i] == static_cast<int>(i);
    }
    GIANT_CHECK(in_order);
}

// A worker with nothing to do sleeps; a job submitted then wakes one (nobody calls wait).
GIANT_TEST("jobs: a sleeping worker is woken by a job") {
    WaitGroup group;
    std::atomic<int> ran{0};
    JobSystem jobs(2); // (last: destroyed first)
    for (int round = 0; round < 50; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 3)); // (asleep, or about to be)
        jobs.submit(group, [&ran] { ran.fetch_add(1); });
        until([&ran, round] { return ran.load() == round + 1; });
        GIANT_CHECK(ran.load() == round + 1);
    }
}

GIANT_TEST("jobs: jobs submitted from many threads at once all run, each once") {
    JobSystem jobs(3);
    constexpr int k_threads = 6;
    constexpr int k_each = 3000;
    std::vector<std::atomic<int>> hits(k_threads * k_each);
    WaitGroup group;
    std::vector<std::thread> submitters;
    for (int t = 0; t < k_threads; ++t) {
        submitters.emplace_back([&jobs, &hits, &group, t] {
            for (int i = 0; i < k_each; ++i) {
                jobs.submit(group, [&hits, n = t * k_each + i] { hits[static_cast<usize>(n)].fetch_add(1); });
            }
        });
    }
    for (std::thread& t : submitters) {
        t.join();
    }
    jobs.wait(group);
    bool all_once = true;
    for (const std::atomic<int>& h : hits) {
        all_once = all_once && h.load() == 1;
    }
    GIANT_CHECK(all_once);
}

// Destroying the system: jobs that submit more as they run are run too, and theirs.
GIANT_TEST("jobs: destroying the system runs what its last jobs submit") {
    std::atomic<int> ran{0};
    WaitGroup group;
    {
        JobSystem jobs(2);
        for (int i = 0; i < 50; ++i) {
            jobs.submit(group, [&jobs, &group, &ran] {
                ran.fetch_add(1);
                jobs.submit(group, [&jobs, &group, &ran] {
                    ran.fetch_add(1);
                    jobs.submit(group, [&ran] { ran.fetch_add(1); });
                });
            });
        }
    }
    GIANT_CHECK(ran.load() == 150);
    GIANT_CHECK(group.done());
}

// A thread waiting on its group sleeps on the same signal as the workers: woken for a job
// (which it then runs or finds gone) or for another group, it still sees its own finish.
GIANT_TEST("jobs: waiters on different groups each see their own finish") {
    std::atomic<int> ran{0};
    JobSystem jobs(1); // (last: destroyed first)
    std::vector<std::thread> waiters;
    for (int t = 0; t < 4; ++t) {
        waiters.emplace_back([&jobs, &ran, t] {
            for (int round = 0; round < 200; ++round) {
                WaitGroup group;
                for (int i = 0; i <= (round + t) % 3; ++i) {
                    jobs.submit(group, [&ran] { ran.fetch_add(1); });
                }
                jobs.wait(group);
            }
        });
    }
    for (std::thread& t : waiters) {
        t.join();
    }
    int expected = 0;
    for (int t = 0; t < 4; ++t) {
        for (int round = 0; round < 200; ++round) {
            expected += (round + t) % 3 + 1;
        }
    }
    GIANT_CHECK(ran.load() == expected);
}
