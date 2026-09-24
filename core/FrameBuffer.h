#pragma once

// FrameBuffer.h - hardware-independent owner of packed 8-bit frames.
//
// The class stores "capacity" frames of exactly Width() * Height() bytes in one
// contiguous allocation. Frames are packed: there is no row padding inside the
// buffer, so a captured frame always occupies BytesPerFrame() bytes.
//
// Allocation is lazy: Init() reserves and commits the address range without
// reading or writing a single byte of it (no zero fill), so a capture buffer of
// tens of gigabytes costs nothing until frames are actually stored. Clear()
// neither releases nor zeroes the storage.
//
// Error model: Init() and Append() never throw. They report the first failure
// through LastError() and return false, leaving Count() and the already stored
// frames untouched (except after a failed Init(), which leaves the buffer
// uninitialised and releases any previous allocation).
//
// This file depends on nothing but the C++ standard library, so it can be unit
// tested without MFC and without the MultiCam driver; only the .cpp reaches for
// the operating system's virtual allocator.

#include <cstddef>
#include <string>
#include <vector>

namespace grablinkcore
{

class FrameSnapshot;

// --- PixelStore ------------------------------------------------------------

// Internal owner of one contiguous, uninitialised byte block, shared by
// FrameBuffer and FrameSnapshot. It exists so an allocation can be handed from
// one owner to the other without copying a single byte (FrameSnapshot::TakeFrom)
// and so that releasing storage is always RAII, whoever ends up owning it.
//
// The whole block is owned, not just the bytes in use: Bytes() is the logical
// payload, which may be narrowed to fewer bytes than the block holds (a snapshot
// taken from a capture buffer owns the capture buffer's whole allocation). A
// store is never copied; it only ever hands its block over.
class PixelStore
{
public:
    PixelStore();
    ~PixelStore();

    // Takes over "bytes" bytes of uninitialised storage. Returns false when the
    // system could not provide it; the store is then empty.
    bool Allocate(std::size_t bytes);

    // Copies "bytes" bytes out of "source" into a block of this store's own.
    // Returns false (and stays empty) when the block cannot be provided.
    bool CopyFrom(const unsigned char* source, std::size_t bytes);

    // Adopts the buffer of "source", which is left empty: no byte is copied.
    // This is how a caller's vector of frames is taken over instead of copied.
    bool Adopt(std::vector<unsigned char>& source);

    // Adopts the whole block of another store, which is left empty: no byte is
    // copied. This is how a capture buffer hands its allocation over.
    bool Adopt(PixelStore& source);

    // Narrows the logical payload to the first "bytes" of the owned block, which
    // is how a snapshot holds fewer frames than the block it took over. Returns
    // false, leaving the store untouched, when that is more than it owns.
    bool SetBytes(std::size_t bytes);

    // Gives up the block. The store becomes empty; Data() returns 0.
    void Release();

    // Move: the moved-from store is left empty and owns nothing.
    PixelStore(PixelStore&& other);
    PixelStore& operator=(PixelStore&& other);

    // Start of the block, or 0 when the store is empty.
    unsigned char* Data();
    const unsigned char* Data() const;

    // True when no block is owned.
    bool Empty() const;

    // Logical payload size, never larger than the owned block.
    std::size_t Bytes() const;

private:
    PixelStore(const PixelStore&);
    PixelStore& operator=(const PixelStore&);

    // Releases whatever is owned, whichever kind of block it happens to be.
    void Drop();

    // Exactly one of the two is used: m_block for storage this store allocated
    // itself (released with the same allocator), m_vector for storage that
    // arrived as a std::vector (released by the vector).
    std::vector<unsigned char> m_vector;
    unsigned char* m_block;
    std::size_t m_bytes;
    std::size_t m_owned;
};

// --- FrameBuffer -----------------------------------------------------------

class FrameBuffer
{
public:
    // Creates an uninitialised buffer. Call Init() before Append().
    FrameBuffer();

    // Creates a buffer initialised with the given geometry. Check
    // IsInitialized() / LastError() afterwards: construction never throws.
    FrameBuffer(int width, int height, std::size_t capacity);

    ~FrameBuffer();

    // Configures the buffer with Width*Height byte frames and allocates room for
    // "capacity" of them, without initialising (or even touching) the storage.
    // Rejects non-positive dimensions or capacity, rejects size arithmetic that
    // overflows std::size_t and reports allocation failure instead of
    // propagating an exception. Returns true on success.
    bool Init(int width, int height, std::size_t capacity);

    // Drops all stored frames without releasing (or clearing) the allocation.
    // The buffer stays initialised and Append() can be used again immediately.
    void Clear();

    // Copies one packed Width() x Height() frame from "source" into the buffer.
    // "pitch" is the distance in bytes between the start of two consecutive
    // source rows; it must be >= Width() so that padded source buffers (for
    // example a MultiCam surface the caller does not own) can be consumed
    // directly. Appends in place without zeroing the destination first.
    bool Append(const unsigned char* source, int pitch);

    bool IsInitialized() const;
    int Width() const;
    int Height() const;

    // Bytes of one packed frame (Width * Height).
    std::size_t BytesPerFrame() const;

    // Number of frames the buffer can hold.
    std::size_t Capacity() const;

    // Number of frames stored so far.
    std::size_t Count() const;

    // Bytes of the whole allocation (BytesPerFrame * Capacity).
    std::size_t TotalBytes() const;

    // Base address of the storage, or 0 when nothing is allocated.
    const unsigned char* Data() const;

    // Read-only access to one stored frame, or 0 when the index is out of range.
    const unsigned char* Frame(std::size_t index) const;

    // Text of the most recent failed operation, empty after a success.
    const std::string& LastError() const;

private:
    // FrameSnapshot::TakeFrom() moves the whole allocation out of the buffer, so
    // it needs access to the storage and to the state reset that follows.
    friend class FrameSnapshot;

    FrameBuffer(const FrameBuffer&);
    FrameBuffer& operator=(const FrameBuffer&);

    bool Fail(const std::string& message);
    void ResetState();

    int m_width;
    int m_height;
    std::size_t m_bytesPerFrame;
    std::size_t m_capacity;
    std::size_t m_count;
    bool m_initialized;
    std::string m_error;
    PixelStore m_storage;
};

} // namespace grablinkcore
