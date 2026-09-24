#include "RefreshCoalescer.h"

namespace grablinkcore
{

namespace
{

// Bumps a counter by one, saturating at the largest representable value so a
// capture that runs for days cannot wrap a count around to zero. A
// compare-exchange loop keeps this lock-free and allocation-free on the capture
// path; contention is bounded because a claim and its acknowledgement are the
// only two writers of any one counter and they alternate.
void IncrementSaturating(std::atomic<unsigned long long>& counter)
{
    const unsigned long long maximum = static_cast<unsigned long long>(-1);

    unsigned long long current = counter.load(std::memory_order_relaxed);
    for (;;)
    {
        if (current == maximum)
        {
            return;
        }

        if (counter.compare_exchange_weak(current, current + 1,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed))
        {
            return;
        }

        // compare_exchange_weak reloaded "current" on failure; try again.
    }
}

} // namespace

unsigned long long SaturatingAddCount(unsigned long long value, unsigned long long delta)
{
    const unsigned long long maximum = static_cast<unsigned long long>(-1);

    // Checked before the addition, so the sum never wraps.
    if (delta > maximum - value)
    {
        return maximum;
    }

    return value + delta;
}

RefreshCoalescer::RefreshCoalescer()
    : m_pending(false),
      m_claims(0),
      m_coalesced(0),
      m_acknowledgements(0),
      m_unbalancedAcknowledgements(0),
      m_resets(0)
{
}

bool RefreshCoalescer::TryClaimPost()
{
    // The whole claim is one atomic exchange. Of any number of callbacks racing
    // on the same unacknowledged generation, exactly one observes the previous
    // state as "not pending" and returns true; every other observes "pending"
    // and returns false. No lock, no allocation, no waiting on another thread.
    if (m_pending.exchange(true, std::memory_order_acq_rel))
    {
        // A refresh post is already outstanding, so this publish needs none.
        // This is the post the UI queue was spared.
        IncrementSaturating(m_coalesced);
        return false;
    }

    IncrementSaturating(m_claims);
    return true;
}

bool RefreshCoalescer::Acknowledge()
{
    // A single store clears the pending claim. It cannot lose a concurrent
    // claim: a claim that exchanged before this store is the one being cleared,
    // and a claim that exchanges after it belongs to the next generation and
    // legitimately posts again.
    if (m_pending.exchange(false, std::memory_order_acq_rel))
    {
        IncrementSaturating(m_acknowledgements);
        return true;
    }

    // Nothing was outstanding, so this acknowledgement belonged to no frame.
    // Reported instead of clearing a claim that belongs to a later one.
    IncrementSaturating(m_unbalancedAcknowledgements);
    return false;
}

void RefreshCoalescer::Reset()
{
    // Clear the claim first, so a producer that observes the cleared state
    // immediately may post for the new generation while the counts below are
    // still being zeroed. That is harmless: the counts describe the previous
    // generation until this call returns.
    m_pending.store(false, std::memory_order_release);

    m_claims.store(0, std::memory_order_relaxed);
    m_coalesced.store(0, std::memory_order_relaxed);
    m_acknowledgements.store(0, std::memory_order_relaxed);
    m_unbalancedAcknowledgements.store(0, std::memory_order_relaxed);

    IncrementSaturating(m_resets);
}

bool RefreshCoalescer::IsPending() const
{
    return m_pending.load(std::memory_order_acquire);
}

RefreshCoalescerSnapshot RefreshCoalescer::Snapshot() const
{
    // Each field is read atomically on its own, so the result is not a
    // consistent point in time across fields; it is a diagnostic, not a
    // synchronization point.
    RefreshCoalescerSnapshot snapshot;
    snapshot.claims = m_claims.load(std::memory_order_relaxed);
    snapshot.coalesced = m_coalesced.load(std::memory_order_relaxed);
    snapshot.acknowledgements = m_acknowledgements.load(std::memory_order_relaxed);
    snapshot.unbalancedAcknowledgements =
        m_unbalancedAcknowledgements.load(std::memory_order_relaxed);
    snapshot.resets = m_resets.load(std::memory_order_relaxed);
    snapshot.pending = m_pending.load(std::memory_order_relaxed);
    return snapshot;
}

} // namespace grablinkcore
