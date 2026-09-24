#include "ModalScopeCounter.h"

namespace grablinkcore
{

namespace
{

// Bumps a counter by one, saturating at the largest representable value so a
// session that runs for days cannot wrap a count around to zero. A
// compare-exchange loop keeps this lock-free and allocation-free.
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

// Raises a high-water mark to "candidate" when the candidate is deeper than
// what is recorded. Never lowers it, so the mark describes the deepest modal
// nesting of the whole session until a Reset().
void RaiseMaximum(std::atomic<unsigned long long>& maximum, unsigned long long candidate)
{
    unsigned long long current = maximum.load(std::memory_order_relaxed);
    while (candidate > current)
    {
        if (maximum.compare_exchange_weak(current, candidate,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed))
        {
            return;
        }
    }
}

} // namespace

ModalScope::ModalScope() : m_counter(0)
{
}

ModalScope::ModalScope(ModalScopeCounter& counter) : m_counter(&counter)
{
    counter.Begin();
}

ModalScope::~ModalScope()
{
    // The whole point of the guard: every path out of the modal call - a
    // normal return, an early return somebody adds later, or a thrown
    // exception - ends the scope exactly once.
    Release();
}

ModalScope::ModalScope(ModalScope&& other) : m_counter(other.m_counter)
{
    // The obligation moved; the source is left holding nothing so its
    // destructor cannot end a scope that now belongs to this guard.
    other.m_counter = 0;
}

ModalScope& ModalScope::operator=(ModalScope&& other)
{
    if (this != &other)
    {
        // End what this guard already holds before taking on the other's
        // scope: a guard owns at most one, and dropping one silently would
        // leave the counter suppressing refreshes after the modal call it
        // belonged to had returned.
        Release();

        m_counter = other.m_counter;
        other.m_counter = 0;
    }

    return *this;
}

bool ModalScope::Release()
{
    // Cleared before the counter is asked to end the scope, so the guard is
    // empty no matter how the call below returns; a second release can then
    // only be a safe no-op.
    ModalScopeCounter* counter = m_counter;
    m_counter = 0;

    if (counter == 0)
    {
        // This guard held no scope: no counter has anything to end, and there
        // is none to report a defect to either.
        return false;
    }

    return counter->End();
}

bool ModalScope::IsHeld() const
{
    return m_counter != 0;
}

ModalScopeCounter::ModalScopeCounter()
    : m_depth(0),
      m_maximumDepth(0),
      m_begins(0),
      m_ends(0),
      m_unbalancedEnds(0),
      m_resets(0)
{
}

unsigned long long ModalScopeCounter::Begin()
{
    // The depth is the whole suppression together with the load in
    // IsSuppressed(). fetch_add returns the depth before this scope, so the
    // new depth is that value plus one.
    const unsigned long long previous = m_depth.fetch_add(1, std::memory_order_acq_rel);
    const unsigned long long depth = previous + 1;

    IncrementSaturating(m_begins);
    RaiseMaximum(m_maximumDepth, depth);
    return depth;
}

bool ModalScopeCounter::End()
{
    // A compare-exchange loop rather than a fetch_sub, because the refusal is
    // the important case: subtracting from a depth of zero would wrap it to the
    // largest representable value, which reads as "a modal call is running" for
    // ever and would silently disable the live preview. Nothing is decremented
    // unless a scope really is held.
    unsigned long long current = m_depth.load(std::memory_order_acquire);
    for (;;)
    {
        if (current == 0)
        {
            IncrementSaturating(m_unbalancedEnds);
            return false;
        }

        if (m_depth.compare_exchange_weak(current, current - 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        {
            IncrementSaturating(m_ends);
            return true;
        }

        // compare_exchange_weak reloaded "current" on failure; try again.
    }
}

ModalScope ModalScopeCounter::BeginScope()
{
    // The guard begins the scope in its constructor, so the obligation exists
    // from this statement onwards whatever happens to the returned value.
    return ModalScope(*this);
}

bool ModalScopeCounter::IsSuppressed() const
{
    // The one read on the acquisition path: a single atomic load, no lock, no
    // allocation, no waiting on the UI thread.
    return m_depth.load(std::memory_order_acquire) != 0;
}

unsigned long long ModalScopeCounter::Depth() const
{
    return m_depth.load(std::memory_order_acquire);
}

void ModalScopeCounter::Reset()
{
    // Clear the depth first, so the acquisition callback sees the unsuppressed
    // state as early as possible and the counters below describe the previous
    // generation until this call returns.
    m_depth.store(0, std::memory_order_release);

    m_maximumDepth.store(0, std::memory_order_relaxed);
    m_begins.store(0, std::memory_order_relaxed);
    m_ends.store(0, std::memory_order_relaxed);
    m_unbalancedEnds.store(0, std::memory_order_relaxed);

    IncrementSaturating(m_resets);
}

ModalScopeCounterSnapshot ModalScopeCounter::Snapshot() const
{
    // Each field is read atomically on its own, so the result is not a
    // consistent point in time across fields; it is a diagnostic, not a
    // synchronization point.
    ModalScopeCounterSnapshot snapshot;
    snapshot.suppressed = m_depth.load(std::memory_order_relaxed) != 0;
    snapshot.depth = m_depth.load(std::memory_order_relaxed);
    snapshot.maximumDepth = m_maximumDepth.load(std::memory_order_relaxed);
    snapshot.begins = m_begins.load(std::memory_order_relaxed);
    snapshot.ends = m_ends.load(std::memory_order_relaxed);
    snapshot.unbalancedEnds = m_unbalancedEnds.load(std::memory_order_relaxed);
    snapshot.resets = m_resets.load(std::memory_order_relaxed);
    return snapshot;
}

} // namespace grablinkcore
