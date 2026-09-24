#include "FrameBuffer.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdlib>
#include <cstring>
#include <utility>

namespace grablinkcore
{

// --- PixelStore ------------------------------------------------------------

PixelStore::PixelStore()
    : m_vector(),
      m_block(0),
      m_bytes(0),
      m_owned(0)
{
}

PixelStore::~PixelStore()
{
    Drop();
}

PixelStore::PixelStore(PixelStore&& other)
    : m_vector(std::move(other.m_vector)),
      m_block(other.m_block),
      m_bytes(other.m_bytes),
      m_owned(other.m_owned)
{
    // The moved-from store must own nothing: it is destroyed independently.
    other.m_block = 0;
    other.m_bytes = 0;
    other.m_owned = 0;
}

PixelStore& PixelStore::operator=(PixelStore&& other)
{
    if (this != &other)
    {
        Drop();
        m_vector.swap(other.m_vector);
        m_block = other.m_block;
        m_bytes = other.m_bytes;
        m_owned = other.m_owned;
        other.m_block = 0;
        other.m_bytes = 0;
        other.m_owned = 0;
    }
    return *this;
}

bool PixelStore::Allocate(std::size_t bytes)
{
    // Any previous block is given up first, exactly like a fresh Init(): the
    // store is either empty or holds the new block, never a mixture.
    Drop();

    if (bytes == 0)
    {
        return true;
    }

#if defined(_WIN32)
    // Reserve and commit the range but touch nothing: committed pages are
    // demand zeroed by the system the moment they are first written, so a
    // 40 GiB capture buffer costs no physical memory until it is filled. A CRT
    // heap allocation would be worse than that - the debug heap pre-fills every
    // block with 0xCD - so the virtual allocator is used deliberately.
    void* block = ::VirtualAlloc(0, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (block == 0)
    {
        return false;
    }
#else
    void* block = std::malloc(bytes);
    if (block == 0)
    {
        return false;
    }
#endif

    m_block = static_cast<unsigned char*>(block);
    m_bytes = bytes;
    m_owned = bytes;
    return true;
}

bool PixelStore::CopyFrom(const unsigned char* source, std::size_t bytes)
{
    if (source == 0 || bytes == 0)
    {
        Drop();
        return false;
    }

    if (!Allocate(bytes))
    {
        return false;
    }

    std::memcpy(m_block, source, bytes);
    return true;
}

bool PixelStore::Adopt(std::vector<unsigned char>& source)
{
    Drop();
    m_vector.swap(source);
    m_bytes = m_vector.size();
    m_owned = m_vector.size();
    return !m_vector.empty();
}

bool PixelStore::Adopt(PixelStore& source)
{
    Drop();

    // Both kinds of block change hands the same way: the source is left empty,
    // and not one byte is copied.
    m_vector.swap(source.m_vector);
    m_block = source.m_block;
    m_bytes = source.m_bytes;
    m_owned = source.m_owned;

    source.m_block = 0;
    source.m_bytes = 0;
    source.m_owned = 0;
    return !Empty();
}

bool PixelStore::SetBytes(std::size_t bytes)
{
    if (bytes > m_owned)
    {
        return false;
    }

    m_bytes = bytes;
    return true;
}

void PixelStore::Release()
{
    Drop();
}

void PixelStore::Drop()
{
    if (m_block != 0)
    {
#if defined(_WIN32)
        ::VirtualFree(m_block, 0, MEM_RELEASE);
#else
        std::free(m_block);
#endif
        m_block = 0;
    }

    if (!m_vector.empty())
    {
        // swap with a fresh vector so that the buffer really is handed back,
        // not merely marked unused.
        std::vector<unsigned char>().swap(m_vector);
    }

    m_bytes = 0;
    m_owned = 0;
}

unsigned char* PixelStore::Data()
{
    if (m_block != 0)
    {
        return m_block;
    }
    return m_vector.empty() ? 0 : &m_vector[0];
}

const unsigned char* PixelStore::Data() const
{
    if (m_block != 0)
    {
        return m_block;
    }
    return m_vector.empty() ? 0 : &m_vector[0];
}

bool PixelStore::Empty() const
{
    return m_block == 0 && m_vector.empty();
}

std::size_t PixelStore::Bytes() const
{
    return m_bytes;
}

// --- FrameBuffer -----------------------------------------------------------

FrameBuffer::FrameBuffer()
    : m_width(0),
      m_height(0),
      m_bytesPerFrame(0),
      m_capacity(0),
      m_count(0),
      m_initialized(false),
      m_error(),
      m_storage()
{
}

FrameBuffer::FrameBuffer(int width, int height, std::size_t capacity)
    : m_width(0),
      m_height(0),
      m_bytesPerFrame(0),
      m_capacity(0),
      m_count(0),
      m_initialized(false),
      m_error(),
      m_storage()
{
    Init(width, height, capacity);
}

FrameBuffer::~FrameBuffer()
{
}

bool FrameBuffer::Fail(const std::string& message)
{
    m_error = message;
    return false;
}

void FrameBuffer::ResetState()
{
    m_initialized = false;
    m_count = 0;
    m_width = 0;
    m_height = 0;
    m_bytesPerFrame = 0;
    m_capacity = 0;
}

bool FrameBuffer::Init(int width, int height, std::size_t capacity)
{
    ResetState();
    m_storage.Release();
    m_error.clear();

    if (width <= 0 || height <= 0)
    {
        return Fail("FrameBuffer: width and height must be positive");
    }

    if (capacity == 0)
    {
        return Fail("FrameBuffer: capacity must hold at least one frame");
    }

    // Checked size arithmetic: width and height are positive here, so a
    // frame size that does not divide back is a wrapped product. The capacity
    // is then bounded so that frameBytes * capacity cannot wrap either.
    const std::size_t frameBytes = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height);
    if (frameBytes / static_cast<std::size_t>(width) != static_cast<std::size_t>(height))
    {
        return Fail("FrameBuffer: frame size overflows std::size_t");
    }

    const std::size_t maxCapacity = static_cast<std::size_t>(-1) / frameBytes;
    if (capacity > maxCapacity)
    {
        return Fail("FrameBuffer: requested frame storage exceeds std::size_t");
    }

    const std::size_t totalBytes = frameBytes * capacity;

    // Allocation failure is a reported error, never an escaping exception: the
    // capture path must be able to tell the user instead of crashing. The bytes
    // are deliberately not initialised; the frames arrive through Append().
    if (!m_storage.Allocate(totalBytes))
    {
        return Fail("FrameBuffer: allocation of the frame storage failed");
    }

    m_width = width;
    m_height = height;
    m_bytesPerFrame = frameBytes;
    m_capacity = capacity;
    m_initialized = true;
    return true;
}

void FrameBuffer::Clear()
{
    if (!m_initialized)
    {
        return;
    }

    // Only the frame count is dropped: the allocation (and its payload) is kept
    // so that a new capture can overwrite the frames without reallocating.
    m_count = 0;
    m_error.clear();
}

bool FrameBuffer::Append(const unsigned char* source, int pitch)
{
    if (!m_initialized)
    {
        return Fail("FrameBuffer: Append called before a successful Init");
    }

    if (m_count >= m_capacity)
    {
        return Fail("FrameBuffer: Append would exceed the frame capacity");
    }

    if (source == 0)
    {
        return Fail("FrameBuffer: Append received a null source pointer");
    }

    if (pitch < m_width)
    {
        return Fail("FrameBuffer: source pitch is narrower than the frame width");
    }

    unsigned char* destination = m_storage.Data() + m_count * m_bytesPerFrame;
    for (int row = 0; row < m_height; ++row)
    {
        // Destination rows are packed (stride == width); the source may be a
        // padded surface whose rows are pitch bytes apart.
        const std::size_t destinationOffset = static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(m_width);
        const std::size_t sourceOffset = static_cast<std::size_t>(row) *
                                         static_cast<std::size_t>(pitch);
        std::memcpy(destination + destinationOffset, source + sourceOffset,
                    static_cast<std::size_t>(m_width));
    }

    ++m_count;
    m_error.clear();
    return true;
}

bool FrameBuffer::IsInitialized() const
{
    return m_initialized;
}

int FrameBuffer::Width() const
{
    return m_width;
}

int FrameBuffer::Height() const
{
    return m_height;
}

std::size_t FrameBuffer::BytesPerFrame() const
{
    return m_bytesPerFrame;
}

std::size_t FrameBuffer::Capacity() const
{
    return m_capacity;
}

std::size_t FrameBuffer::Count() const
{
    return m_count;
}

std::size_t FrameBuffer::TotalBytes() const
{
    return m_bytesPerFrame * m_capacity;
}

const unsigned char* FrameBuffer::Data() const
{
    return m_storage.Data();
}

const unsigned char* FrameBuffer::Frame(std::size_t index) const
{
    if (!m_initialized || index >= m_count)
    {
        return 0;
    }
    return m_storage.Data() + index * m_bytesPerFrame;
}

const std::string& FrameBuffer::LastError() const
{
    return m_error;
}

} // namespace grablinkcore
