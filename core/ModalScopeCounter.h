#pragma once

// ModalScopeCounter.h - "a modal call is running" flag for the UI thread.
//
// The live preview publishes at full capture rate and posts
// WM_APP_PREVIEW_REFRESH for the frames the UI has not seen yet (see
// RefreshCoalescer.h). That is harmless while the UI thread dispatches its own
// message queue, and harmful the moment a modal call starts: every modal call
// the application makes on the UI thread - the shell's folder chooser, an MFC
// dialog such as the About box, any dialog that ends in ::MessageBox - runs a
// nested message loop that dispatches the same posted messages. A continuous
// post/Invalidate stream entering that nested loop leaves the modal window
// hidden behind a disabled owner and takes the application with it.
//
// Suppressing the refresh post for the span of the modal call removes the
// trigger. The span has two properties that a plain bool cannot express:
//
//   * Modal calls nest. An About box can be opened while the folder chooser is
//     up, and a report can be shown from a dialog. The suppression must stay in
//     force until the outermost call has returned, so scopes are counted, not
//     set and cleared.
//   * Every span has to release exactly once, including on the early-return
//     paths a later edit adds. That is what the guard type below is for: the
//     scope ends in a destructor, or explicitly, and never twice.
//
// The producer side is the acquisition callback, on the driver's signal thread:
// it reads IsSuppressed() for every acquired frame. That read therefore takes
// no lock, allocates nothing, and never waits on the UI thread; the depth it
// reads is one std::atomic<unsigned long long>.
//
// Error model: nothing throws and nothing can fail. A release that finds no
// scope of its own (an empty guard, a double release, a guard that outlived a
// Reset()) is refused and counted, never applied: a depth that underflowed to
// its maximum would read as permanently suppressed and would silently disable
// the live preview for the rest of the session.
//
// This header depends on the C++ standard library only - no MFC, no MultiCam -
// and is unit tested without hardware.

#include <atomic>

namespace grablinkcore
{

// Plain-old-data view of the counter's state and its diagnostics counters. Each
// field is read atomically on its own, so a snapshot is a diagnostic and not a
// consistent point in time across fields. Cumulative counters saturate at the
// largest representable value instead of wrapping.
struct ModalScopeCounterSnapshot
{
    // The answer the acquisition callback uses: true while at least one modal
    // scope is held, so a preview refresh post must be suppressed.
    bool suppressed;

    // Modal scopes held right now: 0 when no modal call is running.
    unsigned long long depth;

    // Deepest nesting reached since the last Reset().
    unsigned long long maximumDepth;

    // Scopes begun, and scopes really ended.
    unsigned long long begins;
    unsigned long long ends;

    // Releases that had no scope to end: an empty guard, a double release, or a
    // guard whose scope a Reset() already dropped. A defect in the caller,
    // reported instead of underflowing the depth.
    unsigned long long unbalancedEnds;

    // Reset() calls.
    unsigned long long resets;
};

class ModalScopeCounter;

// RAII obligation for exactly one modal scope.
//
// Constructing it from a counter begins one scope; destroying it, or calling
// Release(), ends that scope. It is movable and not copyable: a copy would make
// two objects responsible for one scope, and the second one to be destroyed
// would end somebody else's. The move transfers the obligation and leaves the
// source empty, so exactly one guard is ever responsible for a scope.
class ModalScope
{
public:
    // Holds nothing. A guard in this state is the "no scope" case - an About
    // box opened when there is no document to protect, for instance - and is
    // what makes Release() safe to call unconditionally.
    ModalScope();

    // Begins one scope on "counter"; the scope ends when this guard is
    // destroyed or released. No lock, no allocation.
    explicit ModalScope(ModalScopeCounter& counter);

    ~ModalScope();

    // The obligation moves to the new guard and the source holds nothing, so
    // the source's destructor is inert.
    ModalScope(ModalScope&& other);

    // Ends the scope this guard already holds, then takes over the other's. A
    // guard owns at most one scope, so an assignment that silently dropped one
    // would leave the counter suppressing refreshes after every modal call had
    // returned.
    ModalScope& operator=(ModalScope&& other);

    // Ends the held scope now, on the calling thread. Returns true when it
    // ended a scope, false when this guard held none (already released, or
    // moved from) or when the counter no longer has a scope of its own to end.
    // The guard holds nothing afterwards, so a second call is a safe no-op.
    bool Release();

    // True while this guard is responsible for a scope.
    bool IsHeld() const;

private:
    ModalScope(const ModalScope&);
    ModalScope& operator=(const ModalScope&);

    // The counter the held scope belongs to, or null when this guard holds none.
    ModalScopeCounter* m_counter;
};

// The suppression flag itself: a count of the modal scopes currently held on
// the UI thread. Not copyable and not movable: a copy would be a second flag
// suppressing the same posts.
class ModalScopeCounter
{
public:
    // Starts unsuppressed with every counter at zero.
    ModalScopeCounter();

    // Begins one modal scope and returns the depth it produced (1 for the
    // outermost call). Lock-free and allocation-free. Pair every call with
    // exactly one End(), or hold the scope through a ModalScope guard instead.
    unsigned long long Begin();

    // Ends one modal scope. Returns true when a scope really was ended, false
    // when none was held - which is counted and changes nothing, so the depth
    // can never underflow.
    bool End();

    // Begins a scope and returns the guard that holds it. The usual way to use
    // this class: one statement per modal call, no explicit pairing.
    ModalScope BeginScope();

    // The check the acquisition callback makes before it posts
    // WM_APP_PREVIEW_REFRESH. Lock-free and allocation-free.
    bool IsSuppressed() const;

    // Modal scopes held right now.
    unsigned long long Depth() const;

    // Clears the depth and every counter, leaving the counter as freshly
    // constructed. Guards that were held across a Reset() find no scope of
    // their own afterwards; their release is refused and counted.
    void Reset();

    // State and counters as they stand right now.
    ModalScopeCounterSnapshot Snapshot() const;

private:
    ModalScopeCounter(const ModalScopeCounter&);
    ModalScopeCounter& operator=(const ModalScopeCounter&);

    // Modal scopes currently held. The one atomic the acquisition path touches.
    std::atomic<unsigned long long> m_depth;

    std::atomic<unsigned long long> m_maximumDepth;
    std::atomic<unsigned long long> m_begins;
    std::atomic<unsigned long long> m_ends;
    std::atomic<unsigned long long> m_unbalancedEnds;
    std::atomic<unsigned long long> m_resets;
};

} // namespace grablinkcore
