#pragma once

// CallbackDrain.h - admission gate that makes callback teardown safe.
//
// The MultiCam callback runs on a driver thread while the UI thread tears the
// channel down. The dangerous interleaving is the obvious one: the callback
// reads "capture is live", the UI thread unregisters the callback and frees the
// state the callback touches, and only then does the callback run and touch
// that freed state. Nothing in the driver API closes that window by itself.
//
// CallbackDrain closes it with three operations:
//
//   Enable()   - called once, before the callback is registered with the
//                driver, so a healthy start-up can never present a closed gate
//                to a callback that is already arriving.
//   TryEnter() - called at the top of the callback. Lock-free and
//                allocation-free: no heap, no mutex, no critical section, no
//                waiting on another thread. It returns false when the gate is
//                closed, and true only when the caller is counted in flight.
//   Leave()    - called once on every exit path of the callback, for every
//                successful TryEnter(). CallbackDrain::Entry enforces that.
//
// Teardown is then Disable() followed by WaitForDrain(timeout), which reports
// success only when it has observed - after the gate was closed - that no
// admitted callback is in flight. That moment is exactly when the state the
// callback touches may be released. The gate can be re-armed afterwards with
// Enable(), so one CallbackDrain serves every capture generation.
//
// Why the double check inside TryEnter() is what makes this correct:
//
//   * TryEnter() publishes its own increment of the in-flight counter *before*
//     it re-reads the gate state, so a callback only ever proceeds if its
//     increment happened before Disable() closed the gate.
//   * Disable() only closes the gate and never touches the counter, so it
//     cannot lose a concurrent entry: an entry that incremented before the gate
//     closed is seen by the counter, and an entry that increments afterwards
//     undoes its increment and returns false without running the callback body.
//   * Therefore, once Disable() has run, a WaitForDrain() that observes an
//     empty in-flight counter cannot be observing a callback that is about to
//     run - every callback body that is running has an increment standing for
//     it, and every callback body that never ran has given its increment back.
//
// WaitForDrain() also refuses to report success while the gate is still open,
// because an empty counter proves nothing when new callbacks may still arrive;
// it fails at once rather than spending the caller's timeout. Reopening the
// gate while a drain is waiting makes that drain give up for the same reason.
//
// The counters are a plain-old-data snapshot for diagnostics and tests. Each
// field is read atomically on its own, so a snapshot is not a consistent point
// in time across fields. The class itself holds nothing but atomics: it
// allocates nothing, blocks nobody, and has no teardown work of its own. It
// depends on the C++ standard library only (no MFC, no MultiCam), so it is
// testable without hardware.

#include <atomic>
#include <cstddef>

namespace grablinkcore
{

// Plain-old-data view of the gate's counters. Cumulative counters are never
// reset - not even by Enable() - so a snapshot taken after a teardown still
// describes the whole life of the gate.
struct CallbackDrainSnapshot
{
    // Callbacks admitted by TryEnter() (the ones that were actually counted).
    unsigned long long admitted;

    // Balanced Leave() calls, one per admitted callback.
    unsigned long long left;

    // Leave() calls that had no admission to balance. A defect in the caller,
    // reported instead of underflowing the in-flight counter.
    unsigned long long unbalancedLeaves;

    // TryEnter() calls refused, for any reason.
    unsigned long long rejected;

    // TryEnter() calls refused because the gate was closed. Before the first
    // Enable() a refusal is only counted in "rejected"; from the first Disable()
    // onwards every refusal lands here too, including an entry that lost the
    // race against a concurrent Disable().
    unsigned long long rejectedAfterDisable;

    // Enable() / Disable() calls that actually changed the state.
    unsigned long long enables;
    unsigned long long disables;

    // WaitForDrain() calls, split by outcome. "drainRefusedEnabled" counts the
    // calls refused because the gate was still open (or was reopened while the
    // call was waiting): those are not timeouts.
    unsigned long long drainAttempts;
    unsigned long long drainCompleted;
    unsigned long long drainTimedOut;
    unsigned long long drainRefusedEnabled;

    // Callbacks currently admitted, and the highest number ever admitted at the
    // same time. The high-water mark is never reset.
    long long inFlight;
    long long maxInFlight;

    // True while the gate admits new callbacks.
    bool enabled;
};

// Admission gate for one callback source. Not copyable and not movable: it owns
// atomic state, and a copy would be a second gate counting the same callbacks.
class CallbackDrain
{
public:
    // RAII entry guard: enters in the constructor, leaves in the destructor,
    // and can be released early. It holds one pointer and one flag, so it is as
    // allocation-free as the gate itself. It is movable so that a callback can
    // hand the obligation to leave to another scope without ever leaving twice
    // or forgetting to leave at all.
    class Entry
    {
    public:
        explicit Entry(CallbackDrain& gate)
            : m_gate(&gate), m_entered(gate.TryEnter())
        {
        }

        ~Entry()
        {
            if (m_entered)
            {
                m_gate->Leave();
            }
        }

        Entry(Entry&& other)
            : m_gate(other.m_gate), m_entered(other.m_entered)
        {
            other.m_gate = 0;
            other.m_entered = false;
        }

        Entry& operator=(Entry&& other)
        {
            if (this != &other)
            {
                if (m_entered)
                {
                    m_gate->Leave();
                }
                m_gate = other.m_gate;
                m_entered = other.m_entered;
                other.m_gate = 0;
                other.m_entered = false;
            }
            return *this;
        }

        // True when the gate admitted this entry, so the callback body may run.
        bool Entered() const
        {
            return m_entered;
        }

        // Leaves early and disarms the destructor. Returns false when the entry
        // was never admitted (or has already been released), in which case
        // nothing is left.
        bool Release()
        {
            if (!m_entered)
            {
                return false;
            }
            m_entered = false;
            m_gate->Leave();
            return true;
        }

    private:
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

        CallbackDrain* m_gate;
        bool m_entered;
    };

    // Starts closed. Enable() must be called before the callback is registered.
    CallbackDrain();

    // Opens the gate for new admissions. Returns true when this call was the one
    // that changed the state, so a caller can tell "armed now" from "already
    // armed". Safe to call from any thread, including during a drain (the drain
    // then gives up rather than claim success).
    bool Enable();

    // Closes the gate: no further TryEnter() succeeds. Callbacks already in
    // flight are unaffected and keep being counted. Returns true when this call
    // was the one that changed the state. Idempotent.
    bool Disable();

    // True while the gate admits new callbacks.
    bool IsEnabled() const;

    // Admits the calling callback. Lock-free and allocation-free. Returns true
    // only when the caller is counted in flight and must call Leave() exactly
    // once; false when the gate is closed, in which case the callback body must
    // not run.
    bool TryEnter();

    // Balances one successful TryEnter(). A call with nothing to balance is
    // counted in "unbalancedLeaves" and leaves the counter untouched.
    void Leave();

    // Blocks, for at most "timeoutMs", until no admitted callback is in flight.
    // Returns true only when the gate is closed and an empty in-flight counter
    // has been observed - the caller may then release what the callback touches.
    // Returns false when the gate is still open (or was reopened while waiting),
    // and false when the timeout expires with callbacks still in flight. A zero
    // timeout still performs one check. Callable repeatedly, and after a
    // completed drain the gate can be re-armed with Enable().
    bool WaitForDrain(unsigned long timeoutMs);

    // Counters as they stand right now.
    CallbackDrainSnapshot Snapshot() const;

private:
    // Records a refused TryEnter(), splitting "after Disable()" from "never
    // enabled" so a test can tell a start-up refusal from a teardown refusal.
    void Reject();

    // Raises the in-flight high-water mark to the value observed now.
    void RaiseMaxInFlight();

    std::atomic<bool> m_enabled;
    std::atomic<bool> m_everEnabled;
    std::atomic<long> m_inFlight;
    std::atomic<long> m_maxInFlight;
    std::atomic<unsigned long long> m_admitted;
    std::atomic<unsigned long long> m_left;
    std::atomic<unsigned long long> m_unbalancedLeaves;
    std::atomic<unsigned long long> m_rejected;
    std::atomic<unsigned long long> m_rejectedAfterDisable;
    std::atomic<unsigned long long> m_enables;
    std::atomic<unsigned long long> m_disables;
    std::atomic<unsigned long long> m_drainAttempts;
    std::atomic<unsigned long long> m_drainCompleted;
    std::atomic<unsigned long long> m_drainTimedOut;
    std::atomic<unsigned long long> m_drainRefusedEnabled;
};

} // namespace grablinkcore
