#pragma once

// The CPU time the calling thread has run, for telling a busy frame from
// one where the thread waited or was preempted (wall time well over CPU
// time).

namespace giant {

// Milliseconds of CPU the calling thread has used since it started (on
// Windows from its cycle count, so close rather than exact). Only
// differences between calls on one thread mean anything.
double thread_cpu_ms();

// The calling thread is put one step ahead of the engine's other threads from here on: for
// the thread that makes the frames, which wakes workers many times a frame and, with every
// core busy, may lose its own core to one of them. False where the system has no such thing
// to give without privileges (Linux), or refused.
bool prefer_this_thread();

// The calling thread is no longer boosted for a moment each time it wakes from a wait
// (Windows does that; a boosted thread can take the core of the thread that woke it): for
// worker threads. False where there is no such boost to turn off, or refused.
bool no_wake_boost_for_this_thread();

} // namespace giant
