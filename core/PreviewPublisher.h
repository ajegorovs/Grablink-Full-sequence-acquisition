#pragma once

// PreviewPublisher.h - hardware-independent owner of the live preview frames.
//
// The application used to draw straight from the grabber's own surface
// (m_pCurrent), which the driver may recycle the moment the callback returns:
// the UI could paint a half overwritten image, and nothing could be copied
// cheaply because the display path owned no storage of its own. This class is
// that storage, and it owns it for the whole application lifetime.
//
// Configure() preallocates a fixed pool of three slots once, in the DIB layout
// a Win32 blit expects, and Publish() never allocates again: it copies the
// useful width bytes of every row out of the caller's (possibly padded) source
// surface into the DWORD-aligned rows of a slot. The acquisition callback can
// therefore publish at full frame rate without touching the heap.
//
// Three slots, three roles: the slot the UI is displaying, the slot a publish
// is writing, and one spare to coalesce into. A new frame takes a free slot;
// when none is free the oldest *unpublished* frame is replaced - the display
// always holds a later frame anyway, so nothing visible is lost - and the
// replacement is counted. Only when every slot is pinned by the display is a
// frame dropped, and that is counted too. A slot the UI has taken for display
// is never written, so a displayed frame cannot tear; that was the flaw of the
// earlier design, where one publication buffer was reused by the next frame
// while the UI still read it.
//
// Threading: Publish() runs on the driver's signal thread, TakeForDisplay() and
// ReleaseDisplay() on the UI thread. A single mutex serialises them; Publish
// holds it only for the duration of the row copies and the display side only
// for a pointer swap, so neither can stall the other for long. The counters
// saturate, so a capture that runs for days cannot wrap one around to zero.
//
// Error model: nothing throws. A refused or dropped Publish() returns false and
// explains itself through LastError().
//
// The file depends on nothing but the C++ standard library and PixelStore, so
// it is unit tested without MFC and without the MultiCam driver.

#include <cstddef>
#include <mutex>
#include <string>

#include "FrameBuffer.h"

namespace grablinkcore
{

// --- PreviewLayout ---------------------------------------------------------

// Geometry of one preview frame in the layout a Win32 blit expects: an 8-bpp
// top-down DIB whose rows are DWORD aligned. A row therefore occupies "pitch"
// bytes, of which only the first "width" are image data; the remainder is
// padding that nothing reads and that Publish() deliberately leaves untouched.
struct PreviewLayout
{
    int width;
    int height;
    std::size_t pitch;   // distance between rows, always a multiple of four
    std::size_t bytes;   // pitch * height

    PreviewLayout();
};

// Pure layout arithmetic: no state, no allocation, no I/O. Returns false for
// non-positive dimensions or a size that cannot be represented, and leaves
// "layout" cleared in that case.
bool ComputePreviewLayout(int width, int height, PreviewLayout& layout);

// Pure counter arithmetic: value + 1, or value when it is already the largest
// representable count. Used so that a long capture saturates a counter instead
// of wrapping it.
std::size_t SaturatingIncrement(std::size_t value);

// --- PreviewPublisher ------------------------------------------------------

class PreviewPublisher
{
public:
    // Three slots: one displayed, one being written, one spare. The spare is
    // what lets a new frame replace an older unpublished one instead of being
    // dropped, so the preview keeps up with the camera rather than with the UI.
    enum { kSlotCount = 3 };

    // Saturating counters. Configure() and Reset() clear them all.
    struct Counters
    {
        std::size_t published;   // frames copied into a slot
        std::size_t replaced;    // older unpublished frames coalesced away
        std::size_t dropped;     // frames that had no slot to go to
        std::size_t refused;     // rejected calls: bad arguments, nothing ready
        std::size_t leased;      // frames handed to the display side

        Counters();
    };

    PreviewPublisher();
    ~PreviewPublisher();

    // Preallocates all three slots for a "width" x "height" preview. Rejects
    // non-positive dimensions, size arithmetic that overflows, and allocation
    // failure; in every failing case the publisher is left unconfigured, because
    // a previous pool is released first rather than left half replaced. Returns
    // true on success, after which the pool is fixed and no call allocates.
    bool Configure(int width, int height);

    // Releases the pool and clears the slots and the counters, leaving the
    // publisher unconfigured. A later Configure() may use other dimensions.
    void Reset();

    bool IsConfigured() const;
    int Width() const;
    int Height() const;

    // DWORD-aligned bytes between the rows of a slot.
    std::size_t Pitch() const;

    // Bytes of one slot (Pitch * Height).
    std::size_t SlotBytes() const;

    // Slots currently holding an unpublished frame.
    std::size_t ReadyCount() const;

    // Slots currently pinned by the display side.
    std::size_t LeaseCount() const;

    // Copies one frame out of "source" into a slot. "sourcePitch" is the
    // distance in bytes between the start of two consecutive source rows and
    // must be at least Width(); only Width() bytes per row are read, so a padded
    // grabber surface can be published directly. Width() bytes per row are
    // copied into the DWORD-aligned rows of the slot, and the row padding of the
    // slot is left untouched.
    //
    // Returns false when the publisher is not configured, the source pointer is
    // null, the source pitch is narrower than the preview, or every slot is
    // pinned by the display (a dropped frame, counted as such).
    bool Publish(const unsigned char* source, int sourcePitch);

    // Display side. Returns the newest published frame and pins its slot so that
    // no publish can overwrite it, or 0 when nothing is published (or the
    // publisher is not configured), in which case nothing is pinned and a
    // refusal is counted. A caller that wants to keep an older frame alive may
    // take it as well: every slot taken stays pinned until ReleaseDisplay(), and
    // the pool of three slots is what bounds how many frames can be held.
    const unsigned char* TakeForDisplay();

    // Releases every slot taken by TakeForDisplay(), returning them to the pool.
    // Safe to call when nothing is pinned.
    void ReleaseDisplay();

    // Base address of one slot, or 0 when the index is out of range or no pool
    // is allocated. Exposed for the DIB setup, and for tests that check the
    // storage never moves.
    const unsigned char* SlotData(std::size_t index) const;

    // Consistent copy of the counters.
    Counters Snapshot() const;

    // Text of the most recent refusal or drop, empty after a success. Returned
    // by value because it is read on one thread while another may be replacing
    // it: a reference to the member would be a race.
    std::string LastError() const;

private:
    PreviewPublisher(const PreviewPublisher&);
    PreviewPublisher& operator=(const PreviewPublisher&);

    // One slot of the pool. "Writing" is the state of the slot a Publish() is
    // copying into right now; it is only observed inside the lock, and it is
    // what a second writer (or a future non-blocking publish) must not touch.
    struct Slot
    {
        enum State { StateFree, StateWriting, StateReady, StateDisplayed };

        Slot();

        State state;
        std::size_t sequence;   // publication order; larger is newer
    };

    bool Fail(const std::string& message);

    // The following are called with m_mutex already held.
    void ReleasePool();
    int ChooseSlotForWrite() const;

    unsigned char* SlotBase(std::size_t index);
    const unsigned char* SlotBase(std::size_t index) const;

    mutable std::mutex m_mutex;
    PixelStore m_storage;
    PreviewLayout m_layout;
    std::size_t m_slotBytes;
    bool m_configured;
    std::size_t m_nextSequence;
    Slot m_slots[kSlotCount];
    Counters m_counters;
    std::string m_error;
};

} // namespace grablinkcore
