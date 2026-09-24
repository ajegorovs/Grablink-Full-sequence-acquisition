// CallbackDrain.cpp - implementation of the callback admission gate.
//
// The correctness argument lives in the header; this file shows how the
// ordering is achieved with a handful of atomics and nothing else. No lock is
// ever taken, nothing is ever allocated, and no call waits on another thread.
//
// Memory ordering, deliberately: the gate state and the in-flight counter are
// sequentially consistent. The safety argument is about the *relative* order of
// three events - the entrant's increment, Disable()'s store, and the drain's
// read of the counter - and a single total order over those operations is what
// makes it hold without further reasoning. The cost is one locked instruction
// per operation on x64, which is nothing next to a frame copy. The diagnostic
// counters are relaxed: they are read by humans and tests, never by the
// argument.

#include "CallbackDrain.h"

#include <chrono>
#include <thread>

namespace grablinkcore
{
namespace
{
// WaitForDrain() yields this many times before it starts sleeping in small
// slices. A drain normally completes in microseconds, because the callback it
// waits for is short, so yielding keeps the latency low without burning a core
// when the wait turns out to be long.
const unsigned int kYieldsBeforeSleeping = 256;

// Length of one sleeping slice, in milliseconds.
const long long kSleepSliceMilliseconds = 1;
}

CallbackDrain::CallbackDrain()
    : m_enabled(false),
      m_everEnabled(false),
      m_inFlight(0),
      m_maxInFlight(0),
      m_admitted(0),
      m_left(0),
      m_unbalancedLeaves(0),
      m_rejected(0),
      m_rejectedAfterDisable(0),
      m_enables(0),
      m_disables(0),
      m_drainAttempts(0),
      m_drainCompleted(0),
      m_drainTimedOut(0),
      m_drainRefusedEnabled(0)
{
}

bool CallbackDrain::Enable()
{
    // Recorded before the state changes, so a refusal that races this call is
    // never mistaken for a start-up refusal.
    m_everEnabled.store(true, std::memory_order_relaxed);

    if (m_enabled.exchange(true, std::memory_order_seq_cst))
    {
        return false;
    }

    m_enables.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool CallbackDrain::Disable()
{
    // Closing the gate is the whole operation: the in-flight counter is left
    // alone, so a concurrent TryEnter() can never be lost by it.
    if (m_enabled.exchange(false, std::memory_order_seq_cst))
    {
        m_disables.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    return false;
}

bool CallbackDrain::IsEnabled() const
{
    return m_enabled.load(std::memory_order_seq_cst);
}

bool CallbackDrain::TryEnter()
{
    if (!m_enabled.load(std::memory_order_seq_cst))
    {
        Reject();
        return false;
    }

    // Publish the admission *before* re-reading the gate state. This is the
    // heart of the primitive: a callback only proceeds if its increment is
    // ordered before the store that closes the gate, so a drain that runs after
    // Disable() can never miss a callback that is about to run.
    m_inFlight.fetch_add(1, std::memory_order_seq_cst);

    if (!m_enabled.load(std::memory_order_seq_cst))
    {
        // Disable() closed the gate between the check above and the increment.
        // Give the increment back and refuse: the callback body must not run,
        // and the drain that may already have seen this increment is waiting for
        // it to be returned, which is exactly what happens here.
        m_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        Reject();
        return false;
    }

    RaiseMaxInFlight();
    m_admitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void CallbackDrain::Leave()
{
    const long before = m_inFlight.fetch_sub(1, std::memory_order_seq_cst);
    if (before > 0)
    {
        m_left.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Nothing was admitted, so this Leave() has nothing to balance. Put the
    // counter back - a negative in-flight count would keep every later drain
    // waiting for a callback that does not exist - and record the defect.
    m_inFlight.fetch_add(1, std::memory_order_seq_cst);
    m_unbalancedLeaves.fetch_add(1, std::memory_order_relaxed);
}

bool CallbackDrain::WaitForDrain(unsigned long timeoutMs)
{
    m_drainAttempts.fetch_add(1, std::memory_order_relaxed);

    // An open gate admits new callbacks at any moment, so an empty counter would
    // prove nothing. Refuse at once instead of spending the caller's budget:
    // teardown closes the gate first, then drains it.
    if (m_enabled.load(std::memory_order_seq_cst))
    {
        m_drainRefusedEnabled.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (m_inFlight.load(std::memory_order_seq_cst) == 0)
    {
        m_drainCompleted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    unsigned int yields = 0;

    for (;;)
    {
        // Re-arming the gate while the drain waits invalidates it for the same
        // reason as above: new work may arrive, so success must not be claimed.
        if (m_enabled.load(std::memory_order_seq_cst))
        {
            m_drainRefusedEnabled.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (m_inFlight.load(std::memory_order_seq_cst) == 0)
        {
            m_drainCompleted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        const unsigned long long elapsedMs = static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        if (elapsedMs >= static_cast<unsigned long long>(timeoutMs))
        {
            break;
        }

        if (yields < kYieldsBeforeSleeping)
        {
            ++yields;
            std::this_thread::yield();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSleepSliceMilliseconds));
        }
    }

    m_drainTimedOut.fetch_add(1, std::memory_order_relaxed);
    return false;
}

CallbackDrainSnapshot CallbackDrain::Snapshot() const
{
    CallbackDrainSnapshot snapshot;
    snapshot.admitted = m_admitted.load(std::memory_order_relaxed);
    snapshot.left = m_left.load(std::memory_order_relaxed);
    snapshot.unbalancedLeaves = m_unbalancedLeaves.load(std::memory_order_relaxed);
    snapshot.rejected = m_rejected.load(std::memory_order_relaxed);
    snapshot.rejectedAfterDisable = m_rejectedAfterDisable.load(std::memory_order_relaxed);
    snapshot.enables = m_enables.load(std::memory_order_relaxed);
    snapshot.disables = m_disables.load(std::memory_order_relaxed);
    snapshot.drainAttempts = m_drainAttempts.load(std::memory_order_relaxed);
    snapshot.drainCompleted = m_drainCompleted.load(std::memory_order_relaxed);
    snapshot.drainTimedOut = m_drainTimedOut.load(std::memory_order_relaxed);
    snapshot.drainRefusedEnabled = m_drainRefusedEnabled.load(std::memory_order_relaxed);
    snapshot.inFlight = static_cast<long long>(m_inFlight.load(std::memory_order_seq_cst));
    snapshot.maxInFlight = static_cast<long long>(m_maxInFlight.load(std::memory_order_relaxed));
    snapshot.enabled = m_enabled.load(std::memory_order_seq_cst);
    return snapshot;
}

void CallbackDrain::Reject()
{
    m_rejected.fetch_add(1, std::memory_order_relaxed);

    // A refusal while the gate has been enabled at least once is a teardown
    // refusal: the only way the gate is closed after Enable() is Disable().
    if (m_everEnabled.load(std::memory_order_relaxed))
    {
        m_rejectedAfterDisable.fetch_add(1, std::memory_order_relaxed);
    }
}

void CallbackDrain::RaiseMaxInFlight()
{
    // Read the counter after this callback's own increment, so the value
    // observed is a real number of admitted callbacks - never the high-water
    // mark plus one for an entry that was refused again.
    const long observed = m_inFlight.load(std::memory_order_seq_cst);
    long peak = m_maxInFlight.load(std::memory_order_relaxed);
    while (peak < observed &&
           !m_maxInFlight.compare_exchange_weak(peak, observed,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed))
    {
        // compare_exchange_weak() refreshes "peak" on failure, so this loop
        // converges as soon as the mark covers the observation.
    }
}

} // namespace grablinkcore
