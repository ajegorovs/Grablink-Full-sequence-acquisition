#include "PreviewPublisher.h"

#include <cstring>

namespace grablinkcore
{

// --- PreviewLayout ---------------------------------------------------------

PreviewLayout::PreviewLayout()
    : width(0),
      height(0),
      pitch(0),
      bytes(0)
{
}

bool ComputePreviewLayout(int width, int height, PreviewLayout& layout)
{
    layout.width = 0;
    layout.height = 0;
    layout.pitch = 0;
    layout.bytes = 0;

    if (width <= 0 || height <= 0)
    {
        return false;
    }

    // Checked arithmetic in std::size_t. Both the DWORD alignment of a row and
    // the size of the frame have to be representable, so a geometry that would
    // wrap is refused instead of producing a plausible but wrong layout.
    const std::size_t rowBytes = static_cast<std::size_t>(width);
    if (rowBytes > static_cast<std::size_t>(-1) - 3)
    {
        return false;
    }

    const std::size_t pitch = (rowBytes + 3) & ~static_cast<std::size_t>(3);
    const std::size_t rows = static_cast<std::size_t>(height);
    if (rows > static_cast<std::size_t>(-1) / pitch)
    {
        return false;
    }

    layout.width = width;
    layout.height = height;
    layout.pitch = pitch;
    layout.bytes = pitch * rows;
    return true;
}

std::size_t SaturatingIncrement(std::size_t value)
{
    return value == static_cast<std::size_t>(-1) ? value : value + 1;
}

// --- PreviewPublisher ------------------------------------------------------

PreviewPublisher::Counters::Counters()
    : published(0),
      replaced(0),
      dropped(0),
      refused(0),
      leased(0)
{
}

PreviewPublisher::Slot::Slot()
    : state(StateFree),
      sequence(0)
{
}

PreviewPublisher::PreviewPublisher()
    : m_mutex(),
      m_storage(),
      m_layout(),
      m_slotBytes(0),
      m_configured(false),
      m_nextSequence(0),
      m_slots(),
      m_counters(),
      m_error()
{
}

PreviewPublisher::~PreviewPublisher()
{
    // The pool is released by PixelStore's own destructor; nothing here holds a
    // slot lease, so there is nothing else to give up.
}

bool PreviewPublisher::Fail(const std::string& message)
{
    m_error = message;
    return false;
}

void PreviewPublisher::ReleasePool()
{
    // Called with m_mutex held. Every slot is a part of the one allocation, so
    // releasing the storage is what releases all three; the bookkeeping is
    // cleared afterwards so no slot can look ready without storage behind it.
    m_storage.Release();

    m_layout = PreviewLayout();
    m_slotBytes = 0;
    m_configured = false;
    m_nextSequence = 0;

    for (int index = 0; index < kSlotCount; ++index)
    {
        m_slots[index] = Slot();
    }

    m_counters = Counters();
}

int PreviewPublisher::ChooseSlotForWrite() const
{
    int oldestReady = -1;

    for (int index = 0; index < kSlotCount; ++index)
    {
        if (m_slots[index].state == Slot::StateFree)
        {
            return index;
        }

        // A ready slot is the fallback: replacing the oldest unpublished frame
        // is what keeps the preview live when the UI is slower than the camera.
        // A displayed slot and a writing slot are never candidates, which is
        // what makes a displayed frame immune to a later publish.
        if (m_slots[index].state == Slot::StateReady &&
            (oldestReady < 0 || m_slots[index].sequence < m_slots[oldestReady].sequence))
        {
            oldestReady = index;
        }
    }

    return oldestReady;
}

unsigned char* PreviewPublisher::SlotBase(std::size_t index)
{
    if (!m_configured || index >= static_cast<std::size_t>(kSlotCount))
    {
        return 0;
    }

    return m_storage.Data() + index * m_slotBytes;
}

const unsigned char* PreviewPublisher::SlotBase(std::size_t index) const
{
    if (!m_configured || index >= static_cast<std::size_t>(kSlotCount))
    {
        return 0;
    }

    return m_storage.Data() + index * m_slotBytes;
}

bool PreviewPublisher::Configure(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // A configuration always starts from nothing: the previous pool is released
    // first, so a failed reconfigure leaves the publisher unconfigured rather
    // than half replaced.
    ReleasePool();

    PreviewLayout layout;
    if (!ComputePreviewLayout(width, height, layout))
    {
        return Fail("PreviewPublisher: width and height must be positive and the preview must be representable");
    }

    // Checked arithmetic for the pool as a whole, on top of the per-frame check.
    if (layout.bytes > static_cast<std::size_t>(-1) / static_cast<std::size_t>(kSlotCount))
    {
        return Fail("PreviewPublisher: the preview pool exceeds std::size_t");
    }

    const std::size_t totalBytes = layout.bytes * static_cast<std::size_t>(kSlotCount);

    // The whole pool is reserved and committed once, here, and never again: the
    // capture path must not allocate. Failure is reported, never thrown.
    if (!m_storage.Allocate(totalBytes))
    {
        return Fail("PreviewPublisher: allocation of the preview pool failed");
    }

    m_layout = layout;
    m_slotBytes = layout.bytes;
    m_configured = true;
    m_error.clear();
    return true;
}

void PreviewPublisher::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    ReleasePool();
    m_error.clear();
}

bool PreviewPublisher::IsConfigured() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_configured;
}

int PreviewPublisher::Width() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_layout.width;
}

int PreviewPublisher::Height() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_layout.height;
}

std::size_t PreviewPublisher::Pitch() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_layout.pitch;
}

std::size_t PreviewPublisher::SlotBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slotBytes;
}

std::size_t PreviewPublisher::ReadyCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::size_t ready = 0;
    for (int index = 0; index < kSlotCount; ++index)
    {
        if (m_slots[index].state == Slot::StateReady)
        {
            ++ready;
        }
    }
    return ready;
}

std::size_t PreviewPublisher::LeaseCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::size_t leased = 0;
    for (int index = 0; index < kSlotCount; ++index)
    {
        if (m_slots[index].state == Slot::StateDisplayed)
        {
            ++leased;
        }
    }
    return leased;
}

bool PreviewPublisher::Publish(const unsigned char* source, int sourcePitch)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_configured)
    {
        m_counters.refused = SaturatingIncrement(m_counters.refused);
        return Fail("PreviewPublisher: Publish called before a successful Configure");
    }

    if (source == 0)
    {
        m_counters.refused = SaturatingIncrement(m_counters.refused);
        return Fail("PreviewPublisher: Publish received a null source pointer");
    }

    if (sourcePitch < m_layout.width)
    {
        m_counters.refused = SaturatingIncrement(m_counters.refused);
        return Fail("PreviewPublisher: source pitch is narrower than the preview width");
    }

    const int index = ChooseSlotForWrite();
    if (index < 0)
    {
        m_counters.dropped = SaturatingIncrement(m_counters.dropped);
        return Fail("PreviewPublisher: every preview slot is displayed or being written");
    }

    Slot& slot = m_slots[index];
    if (slot.state == Slot::StateReady)
    {
        // The oldest unpublished frame is being replaced. The display holds a
        // later frame whenever it holds one at all, so nothing visible is lost.
        m_counters.replaced = SaturatingIncrement(m_counters.replaced);
    }

    slot.state = Slot::StateWriting;
    m_nextSequence = SaturatingIncrement(m_nextSequence);
    slot.sequence = m_nextSequence;

    unsigned char* destination = SlotBase(static_cast<std::size_t>(index));
    for (int row = 0; row < m_layout.height; ++row)
    {
        // Destination rows are one DWORD-aligned pitch apart, source rows are
        // "sourcePitch" apart, and only the useful width of a row is copied: the
        // padding of a slot row is never written. The offsets are computed in
        // std::size_t so a large pitch cannot overflow an int.
        const std::size_t destinationOffset =
            static_cast<std::size_t>(row) * m_layout.pitch;
        const std::size_t sourceOffset =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(sourcePitch);

        std::memcpy(destination + destinationOffset, source + sourceOffset,
                    static_cast<std::size_t>(m_layout.width));
    }

    // Only now is the frame complete, so a display that took the slot would
    // never have seen a half written one; and the lock, which the display side
    // also takes, is what keeps a displayed slot out of the loop above.
    slot.state = Slot::StateReady;
    m_counters.published = SaturatingIncrement(m_counters.published);
    m_error.clear();
    return true;
}

const unsigned char* PreviewPublisher::TakeForDisplay()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_configured)
    {
        m_counters.refused = SaturatingIncrement(m_counters.refused);
        m_error = "PreviewPublisher: TakeForDisplay called before a successful Configure";
        return 0;
    }

    int newest = -1;
    for (int index = 0; index < kSlotCount; ++index)
    {
        if (m_slots[index].state == Slot::StateReady &&
            (newest < 0 || m_slots[index].sequence > m_slots[newest].sequence))
        {
            newest = index;
        }
    }

    if (newest < 0)
    {
        m_counters.refused = SaturatingIncrement(m_counters.refused);
        m_error = "PreviewPublisher: no published preview frame is waiting";
        return 0;
    }

    m_slots[newest].state = Slot::StateDisplayed;
    m_counters.leased = SaturatingIncrement(m_counters.leased);
    m_error.clear();
    return SlotBase(static_cast<std::size_t>(newest));
}

void PreviewPublisher::ReleaseDisplay()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (int index = 0; index < kSlotCount; ++index)
    {
        if (m_slots[index].state == Slot::StateDisplayed)
        {
            m_slots[index].state = Slot::StateFree;
        }
    }

    m_error.clear();
}

const unsigned char* PreviewPublisher::SlotData(std::size_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return SlotBase(index);
}

PreviewPublisher::Counters PreviewPublisher::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_counters;
}

std::string PreviewPublisher::LastError() const
{
    // Copied out under the lock on purpose: a reference to a member that the
    // acquisition thread may be rewriting is a race in the capture path.
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

} // namespace grablinkcore
