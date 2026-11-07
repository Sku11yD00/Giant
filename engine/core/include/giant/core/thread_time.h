#pragma once

// The CPU time the calling thread has run, for telling a busy frame from
// one where the thread waited or was preempted (wall time well over CPU
// time).

namespace giant {

// Milliseconds of CPU the calling thread has used since it started (on
// Windows from its cycle count, so close rather than exact). Only
// differences between calls on one thread mean anything.
double thread_cpu_ms();

} // namespace giant
