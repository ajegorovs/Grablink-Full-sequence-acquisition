#pragma once

// SaveWorker.h - hardware-independent, thread-safe asynchronous BMP save job.
//
// The capture side owns a FrameBuffer that it keeps overwriting, so the save
// side must never read that memory while capture can still mutate it. The
// bridge between the two is FrameSnapshot: an owning set of the frames to
// write. Once a snapshot exists the worker only ever reads the snapshot's
// private bytes, so capture is free to Clear() or overwrite its buffer the
// moment Start() returns.
//
// A snapshot can either copy the frames (FrameSnapshot::Capture, the vector
// constructors) or take over the capture buffer's whole allocation without
// copying a byte (FrameSnapshot::TakeFrom). Snapshots are move-only, so the
// expensive frames can never be duplicated by accident: Start() consumes one.
//
// Only one job runs at a time. Start() takes the request over and returns as
// soon as the worker thread exists; the caller observes the job through
// Progress(), stops it early with Cancel() (between frames), joins a *finished*
// job without waiting with ReapFinished(), and joins a running one with Wait().
// The destructor cancels and joins, so a SaveWorker can never outlive its
// thread. After a job has finished and been joined the worker can be started
// again.
//
// Error model: no exception escapes the worker API or thread. A rejected
// Start() returns false and explains itself through Progress().lastError; a
// frame whose BMP write reports failure is counted and the remaining frames are
// attempted (unless cancelled). An unexpected sink/allocation exception aborts
// the job, counts the current frame as failed and records the reason.
//
// The file contains nothing but the C++ standard library: no MFC, no MultiCam
// and no external dependency, so it can be unit tested without hardware.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "BmpWriter.h"
#include "FrameBuffer.h"

namespace grablinkcore
{

// --- FrameSnapshot ---------------------------------------------------------

// Immutable, self-contained set of grayscale frames ready to be written. The
// pixel bytes are owned by the snapshot, so the object the snapshot was built
// from (a capture buffer, a caller's vector) may be modified or destroyed
// immediately afterwards without affecting a running save job.
class FrameSnapshot
{
public:
    // Empty snapshot: IsValid() is false and LastError() explains why.
    FrameSnapshot();

    // Copies "pixels", which must hold exactly width * height * frameCount
    // bytes of packed (padding free) 8-bpp frames. Unlike the && overload this
    // leaves the caller's vector untouched.
    FrameSnapshot(int width, int height, std::size_t frameCount,
                  const std::vector<unsigned char>& pixels);

    // Takes ownership of "pixels" (same size requirement). On failure the
    // caller's vector is left alone, not moved from.
    FrameSnapshot(int width, int height, std::size_t frameCount,
                  std::vector<unsigned char>&& pixels);

    // Move-only on purpose: a snapshot owns the frames of a whole capture, so
    // an implicit copy would silently double the memory of the very transfer
    // this type exists to make cheap. Declaring the move operations is what
    // makes the copy operations unavailable.
    FrameSnapshot(FrameSnapshot&& other);
    FrameSnapshot& operator=(FrameSnapshot&& other);

    bool IsValid() const;
    int Width() const;
    int Height() const;

    // Bytes of one packed frame (Width * Height).
    std::size_t BytesPerFrame() const;

    // Number of frames held.
    std::size_t FrameCount() const;

    // Read-only access to one frame, or 0 when the index is out of range.
    const unsigned char* Frame(std::size_t index) const;

    // Reason the snapshot is invalid, empty once it is valid.
    const std::string& LastError() const;

    // Copies the first "frameCount" frames out of a capture buffer so the job
    // that follows can never observe a frame being overwritten. Returns false
    // (with the reason in snapshot.LastError()) when the buffer is not
    // initialised, holds fewer frames than requested, or the copy fails.
    static bool Capture(const FrameBuffer& buffer, std::size_t frameCount,
                        FrameSnapshot& snapshot);

    // Zero-copy alternative to Capture(): takes over the buffer's whole
    // allocation and the first "frameCount" frames in it. The snapshot may
    // therefore own more bytes than it holds frames. On success "buffer" is
    // left uninitialised and empty, with its storage gone, and must be Init()ed
    // again before Append() can be used; on failure "buffer" is untouched and
    // still usable, because a refused request must never steal a capture.
    static bool TakeFrom(FrameBuffer& buffer, std::size_t frameCount,
                         FrameSnapshot& snapshot);

private:
    // Shared geometry/size validation. On failure "error" carries the reason.
    static bool Validate(int width, int height, std::size_t frameCount,
                         std::size_t pixelBytes, std::string& error);

    int m_width;
    int m_height;
    std::size_t m_bytesPerFrame;
    std::size_t m_frameCount;
    std::string m_error;
    PixelStore m_pixels;
};

// --- SaveProgress ----------------------------------------------------------

// Consistent copy of a worker's state: "completed" counts frames written
// successfully, "failed" those whose write failed, so while a finished,
// uncancelled job always has completed + failed == total.
//
// The counters below describe the *actual* result reported by the sink, not an
// estimate from the frame geometry: a job that wrote nothing (every frame
// failed, or it was cancelled before the first write finished) reports zero
// bytes and zero throughput even when it was busy for a while.
struct SaveProgress
{
    std::size_t total;
    std::size_t completed;
    std::size_t failed;
    bool running;
    bool cancelled;
    std::string lastError;   // most recent failure, empty while all is well

    // Bytes the sink reported as successfully written by this job. Only frames
    // whose write succeeded contribute, and each one contributes what the sink
    // said it really handed to the file - for the default sink that is the
    // exact size of the written BMP (padding included), not width * height.
    unsigned long long bytesWritten;

    // Monotonic elapsed duration of the job, measured from the moment Start() accepted
    // it: how long it has been running while it is still running, how long it
    // took once it has finished. Zero before any job has started and for a
    // refused Start().
    double elapsedSeconds;

    // bytesWritten / elapsedSeconds. Zero while no byte has been written or no
    // measurable time has passed, so "no bytes, no throughput" is unambiguous.
    double bytesPerSecond;

    SaveProgress();
};

// --- SaveSink --------------------------------------------------------------

// Seam between the worker and the file system. The default implementation
// writes real BMP files; a test (or a future encoder) can substitute its own
// without the worker knowing. The worker holds no ownership: an injected sink
// must outlive the job, which Wait() and the destructor both guarantee.
class SaveSink
{
public:
    virtual ~SaveSink();

    virtual BmpWriteResult Write(const std::wstring& path,
                                 const unsigned char* pixels,
                                 int width, int height, int pitch) = 0;
};

// Default sink: an 8-bpp grayscale BMP through BmpWriter.
class BmpFileSink : public SaveSink
{
public:
    virtual BmpWriteResult Write(const std::wstring& path,
                                 const unsigned char* pixels,
                                 int width, int height, int pitch);
};

// --- SaveWorker ------------------------------------------------------------

class SaveWorker
{
public:
    SaveWorker();
    ~SaveWorker();

    // Starts a job writing the snapshot's frames into "folder" as
    // "<prefix>_<index>.bmp". Returns as soon as the worker thread is running.
    //
    // "snapshot" is handed over by reference and never copied: a capture of
    // forty gigabytes must reach the worker with a pointer move. The parameter
    // is an lvalue reference, not an rvalue one, precisely so that a refusal
    // cannot swallow a capture: a caller passing an rvalue would lose its
    // frames the moment the thread cannot be created, because the moved-from
    // temporary dies with the call. Here the frames are moved into the worker
    // only for the duration of the attempt and are moved straight back - same
    // object, same pointers, still valid - when the request is refused for any
    // reason: a job is already running (only one job at a time), the snapshot
    // is invalid, "folder" is empty, or the worker thread could not be created.
    bool Start(FrameSnapshot& snapshot, const std::wstring& folder,
               const std::wstring& prefix, SaveSink* sink = 0);

    // Race free copy of the current state; safe to call at any time, including
    // from another thread while the job runs.
    SaveProgress Progress() const;

    // Asks the running job to stop. Cancellation is honoured between frames: a
    // frame that is already being written is always finished and counted.
    void Cancel();

    // Blocks until the job has finished, then joins the worker thread. Safe to
    // call when no job was ever started and safe to call more than once.
    void Wait();

    // Joins a job whose thread has finished but has not been joined yet, and
    // releases the frames it owned. Never waits for a job that is still running:
    // it returns false at once in that case, and true when it really did reap a
    // finished thread. This is the non-blocking replacement for the
    // check-then-Wait() pattern, which could block the caller through a job that
    // had been started in between the check and the wait.
    bool ReapFinished();

    // Deterministic test seam: makes the next Start() fail exactly as if the
    // worker thread could not be created, without needing the system to refuse
    // a thread. The flag is one-shot, so the worker stays usable afterwards.
    void FailNextThreadStartForTest();

    // Deterministic name of the frame at "index" of a "totalFrames" job:
    // "<prefix>_<zero padded index>.bmp". The field is at least five digits,
    // so names stay unique and lexicographically ordered well past 9999 frames
    // (a wider job simply widens the whole field, never just some names).
    static std::wstring FileNameForIndex(const std::wstring& prefix,
                                         std::size_t index,
                                         std::size_t totalFrames);

private:
    SaveWorker(const SaveWorker&);
    SaveWorker& operator=(const SaveWorker&);

    bool Begin(FrameSnapshot& snapshot, const std::wstring& folder,
               const std::wstring& prefix, SaveSink* sink);
    void Run(SaveSink* sink);
    bool IsCancelled() const;

    mutable std::mutex m_mutex;
    std::condition_variable m_jobFinished;
    std::thread m_thread;
    BmpFileSink m_defaultSink;

    // Inputs of the running job, owned by the worker for as long as the job
    // lasts. Run() reads them without the lock; they are only written while no
    // job is running, and released by the join paths (Wait, ReapFinished, the
    // next Begin and the destructor) once the thread is done with them.
    FrameSnapshot m_jobSnapshot;
    std::wstring m_jobFolder;
    std::wstring m_jobPrefix;

    // One-shot test seam, see FailNextThreadStartForTest().
    bool m_failNextThreadStart;

    // All of the following is guarded by m_mutex.
    bool m_running;
    bool m_cancelled;
    std::size_t m_total;
    std::size_t m_completed;
    std::size_t m_failed;
    std::string m_lastError;

    // Metrics of the current (or last finished) job, see SaveProgress. The
    // start stamp is only meaningful while a job is running; once it has
    // finished the duration is frozen in m_elapsedSeconds.
    unsigned long long m_bytesWritten;
    std::chrono::steady_clock::time_point m_startedAt;
    double m_elapsedSeconds;
};

} // namespace grablinkcore
