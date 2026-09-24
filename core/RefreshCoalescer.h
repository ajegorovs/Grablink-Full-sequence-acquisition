#pragma once

// RefreshCoalescer.h - one-in-flight gate for the preview refresh message.
//
// The preview integration publishes at full capture rate (hundreds of frames
// per second) and, until now, posted WM_APP_PREVIEW_REFRESH to the view for
// every successful publish. A UI that cannot drain the queue at that rate
// simply accumulates messages: the window procedure runs behind the camera,
// each message costs a repaint of an already stale frame, and the queue grows
// without bound. The refresh the UI needs is "some later frame is ready", not
// "this particular frame is ready", so the surplus posts are pure waste.
//
// This class is the gate that removes the waste, without a lock and without
// allocating anything on the hot path:
//
//   TryClaimPost() - called by the acquisition callback after a successful
//                    publish. Lock-free and allocation-free: no heap, no
//                    mutex, no critical section, no waiting on another thread.
//                    Returns true exactly once per unacknowledged generation,
//                    and that is the only case in which the caller may post
//                    WM_APP_PREVIEW_REFRESH. Every further call before the UI
//                    acknowledges returns false, and each refusal is counted
//                    as a coalesced post - the message the UI did not need.
//   Acknowledge()  - called by the UI thread when it has received the refresh
//                    message. It re-arms the gate so the next publish can
//                    claim a new post. Returns true when there was a pending
//                    claim to clear, false when there was none.
//   Reset()        - clears the pending claim and every counter, so one
//                    coalescer serves every capture generation.
//
// Why one atomic flag is enough:
//
//   * TryClaimPost() claims with an atomic exchange of the pending flag. An
//     exchange is a single atomic step, so of any number of callbacks racing
//     on the same unacknowledged generation exactly one observes "false" and
//     returns true; every other observes "true" and returns false.
//   * Acknowledge() only stores "false". It cannot lose a concurrent claim:
//     a claim that exchanged before the store is the pending one the store
//     clears, and a claim that exchanges after the store belongs to the next
//     generation and legitimately posts again.
//   * Therefore at most one refresh message is outstanding at a time. The UI
//     queue can hold one preview message per capture, not one per frame.
//
// The counters are a plain-old-data snapshot for diagnostics and tests. Each
// field is read atomically on its own, so a snapshot is not a consistent point
// in time across fields. Cumulative counters saturate at the largest
// representable value instead of wrapping, so a capture that runs for days
// cannot roll a counter back to zero.
//
// Error model: nothing throws and nothing can fail. The class holds nothing but
// atomics, depends on the C++ standard library only (no MFC, no MultiCam), and
// is therefore unit tested without hardware.

#include <atomic>

namespace grablinkcore
{

// Saturating counter arithmetic: value + delta, or the largest representable
// count when the sum would overflow. Used by every counter below so that a
// long capture saturates a count instead of wrapping it around.
unsigned long long SaturatingAddCount(unsigned long long value, unsigned long long delta);

// Plain-old-data view of the coalescer's counters. A snapshot describes the
// whole life of the coalescer up to the moment it was taken; Reset() is what
// clears the counts.
struct RefreshCoalescerSnapshot
{
    // TryClaimPost() calls that returned true: refresh messages the caller was
    // told to post.
    unsigned long long claims;

    // TryClaimPost() calls that returned false: publishes whose refresh was
    // already covered by an outstanding message. This is the number of posts
    // the UI queue was spared.
    unsigned long long coalesced;

    // Acknowledge() calls that cleared a pending claim.
    unsigned long long acknowledgements;

    // Acknowledge() calls that had nothing to clear. A defect in the caller,
    // reported instead of clearing a claim that belongs to a later frame.
    unsigned long long unbalancedAcknowledgements;

    // Reset() calls.
    unsigned long long resets;

    // True while a claim is outstanding, that is, while TryClaimPost() would
    // refuse.
    bool pending;
};

// Coalesces the preview refresh message so that at most one post is outstanding
// at a time. Not copyable and not movable: it owns atomic state, and a copy
// would be a second gate admitting the same posts.
class RefreshCoalescer
{
public:
    // Starts with no claim outstanding and every counter at zero.
    RefreshCoalescer();

    // Called by the acquisition callback after a successful publish. Lock-free
    // and allocation-free. Returns true when this call claimed the one
    // outstanding refresh post, in which case the caller posts
    // WM_APP_PREVIEW_REFRESH; false when a post is already outstanding, in
    // which case the caller posts nothing and the publish is counted as
    // coalesced.
    bool TryClaimPost();

    // Called by the UI thread once it has received the refresh message. Re-arms
    // the gate so the next publish can claim a post. Returns true when it
    // cleared a pending claim, false when there was none (counted as
    // unbalanced).
    bool Acknowledge();

    // Clears the pending claim and every counter, leaving the coalescer as
    // freshly constructed. Callable from any thread; a capture restart is the
    // usual reason.
    void Reset();

    // True while a claim is outstanding, so TryClaimPost() would refuse.
    bool IsPending() const;

    // Counters as they stand right now.
    RefreshCoalescerSnapshot Snapshot() const;

private:
    RefreshCoalescer(const RefreshCoalescer&);
    RefreshCoalescer& operator=(const RefreshCoalescer&);

    // True while a refresh post has been claimed and not yet acknowledged.
    // The one atomic the hot path touches.
    std::atomic<bool> m_pending;

    std::atomic<unsigned long long> m_claims;
    std::atomic<unsigned long long> m_coalesced;
    std::atomic<unsigned long long> m_acknowledgements;
    std::atomic<unsigned long long> m_unbalancedAcknowledgements;
    std::atomic<unsigned long long> m_resets;
};

} // namespace grablinkcore
