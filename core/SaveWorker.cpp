#include "SaveWorker.h"

#include <iomanip>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{

// Messages of the rejected Start() calls, kept in one place so the tests and
// the application can rely on them.
const char* const kAlreadyRunning = "SaveWorker: a save job is already running";
const char* const kInvalidSnapshot = "SaveWorker: the frame snapshot is empty or invalid";
const char* const kEmptyFolder = "SaveWorker: the output folder is empty";
const char* const kThreadCreation = "SaveWorker: the worker thread could not be created";
const char* const kMissingFrame = "SaveWorker: the snapshot frame is missing";

const wchar_t* const kPathSeparator = L"\\";

// Message of a save job that was aborted by an exception. It names the frame
// the exception was thrown for, so the reason stays useful even when the
// exception itself carries no text (an unknown exception has no what()).
std::string ExceptionFailureMessage(std::size_t frameIndex, std::size_t totalFrames,
                                    const char* what)
{
    std::ostringstream stream;
    stream << "SaveWorker: the save job was aborted by an exception at frame "
           << frameIndex;
    if (totalFrames != 0)
    {
        stream << " of " << totalFrames;
    }
    if (what != 0 && what[0] != 0)
    {
        stream << ": " << what;
    }
    return stream.str();
}

// Number of decimal digits in "value", at least one.
std::size_t DigitCount(std::size_t value)
{
    std::size_t digits = 1;
    while (value >= 10)
    {
        value /= 10;
        ++digits;
    }
    return digits;
}

// Whole seconds between two points of a monotonic clock. The steady clock is
// used on purpose: a save job must not report a shorter (or negative) duration
// because the wall clock was adjusted while it ran.
double ElapsedSeconds(std::chrono::steady_clock::time_point start,
                      std::chrono::steady_clock::time_point end)
{
    const std::chrono::duration<double> elapsed = end - start;
    return elapsed.count();
}

// Throughput of a job that wrote "bytes" in "seconds". Zero rather than
// infinity when nothing was written or no measurable time has passed, so a
// caller never has to special case an infinite rate.
double BytesPerSecond(unsigned long long bytes, double seconds)
{
    if (seconds <= 0.0)
    {
        return 0.0;
    }
    return static_cast<double>(bytes) / seconds;
}

} // namespace

namespace grablinkcore
{

// --- FrameSnapshot ---------------------------------------------------------

FrameSnapshot::FrameSnapshot()
    : m_width(0),
      m_height(0),
      m_bytesPerFrame(0),
      m_frameCount(0),
      m_error("FrameSnapshot: no frames have been set"),
      m_pixels()
{
}

FrameSnapshot::FrameSnapshot(FrameSnapshot&& other)
    : m_width(other.m_width),
      m_height(other.m_height),
      m_bytesPerFrame(other.m_bytesPerFrame),
      m_frameCount(other.m_frameCount),
      m_error(std::move(other.m_error)),
      m_pixels(std::move(other.m_pixels))
{
    // The source must not keep pointing at frames it no longer owns.
    other.m_width = 0;
    other.m_height = 0;
    other.m_bytesPerFrame = 0;
    other.m_frameCount = 0;
    other.m_error = "FrameSnapshot: the frames have been moved out";
}

FrameSnapshot& FrameSnapshot::operator=(FrameSnapshot&& other)
{
    if (this != &other)
    {
        m_width = other.m_width;
        m_height = other.m_height;
        m_bytesPerFrame = other.m_bytesPerFrame;
        m_frameCount = other.m_frameCount;
        m_error = std::move(other.m_error);
        m_pixels = std::move(other.m_pixels);

        other.m_width = 0;
        other.m_height = 0;
        other.m_bytesPerFrame = 0;
        other.m_frameCount = 0;
        other.m_error = "FrameSnapshot: the frames have been moved out";
    }
    return *this;
}

bool FrameSnapshot::Validate(int width, int height, std::size_t frameCount,
                             std::size_t pixelBytes, std::string& error)
{
    error.clear();

    if (width <= 0)
    {
        error = "FrameSnapshot: width must be positive";
        return false;
    }

    if (height <= 0)
    {
        error = "FrameSnapshot: height must be positive";
        return false;
    }

    if (frameCount == 0)
    {
        error = "FrameSnapshot: at least one frame is required";
        return false;
    }

    // Checked size arithmetic, exactly as in FrameBuffer: a product that does
    // not divide back wrapped, so the request is refused instead of being
    // silently truncated into a smaller copy.
    const std::size_t frameBytes = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height);
    if (frameBytes / static_cast<std::size_t>(width) != static_cast<std::size_t>(height))
    {
        error = "FrameSnapshot: the frame size overflows std::size_t";
        return false;
    }

    if (frameCount > static_cast<std::size_t>(-1) / frameBytes)
    {
        error = "FrameSnapshot: the frame storage size overflows std::size_t";
        return false;
    }

    if (pixelBytes != frameBytes * frameCount)
    {
        error = "FrameSnapshot: the pixel byte count does not match width * height * frames";
        return false;
    }

    return true;
}

FrameSnapshot::FrameSnapshot(int width, int height, std::size_t frameCount,
                             const std::vector<unsigned char>& pixels)
    : m_width(0),
      m_height(0),
      m_bytesPerFrame(0),
      m_frameCount(0),
      m_error(),
      m_pixels()
{
    if (!Validate(width, height, frameCount, pixels.size(), m_error))
    {
        return;
    }

    // The copy is what makes the snapshot immutable: the caller keeps owning
    // its buffer and may write to it again at once.
    if (!m_pixels.CopyFrom(pixels.empty() ? 0 : &pixels[0], pixels.size()))
    {
        m_error = "FrameSnapshot: copying the frames failed (out of memory)";
        return;
    }

    m_width = width;
    m_height = height;
    m_bytesPerFrame = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    m_frameCount = frameCount;
}

FrameSnapshot::FrameSnapshot(int width, int height, std::size_t frameCount,
                             std::vector<unsigned char>&& pixels)
    : m_width(0),
      m_height(0),
      m_bytesPerFrame(0),
      m_frameCount(0),
      m_error(),
      m_pixels()
{
    if (!Validate(width, height, frameCount, pixels.size(), m_error))
    {
        // The caller's vector is deliberately left untouched, so a rejected
        // snapshot never silently empties a buffer the caller still needs.
        return;
    }

    m_width = width;
    m_height = height;
    m_bytesPerFrame = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    m_frameCount = frameCount;
    // Validate() has already established that the vector holds frames, so the
    // hand over cannot fail - but it is still checked, because a refused
    // snapshot must never leave the caller without its pixels.
    if (!m_pixels.Adopt(pixels))
    {
        m_width = 0;
        m_height = 0;
        m_bytesPerFrame = 0;
        m_frameCount = 0;
        m_error = "FrameSnapshot: taking over the frames failed";
    }
}

bool FrameSnapshot::IsValid() const
{
    return m_width > 0 && m_height > 0 && m_frameCount > 0 &&
           m_pixels.Bytes() == m_bytesPerFrame * m_frameCount;
}

int FrameSnapshot::Width() const
{
    return m_width;
}

int FrameSnapshot::Height() const
{
    return m_height;
}

std::size_t FrameSnapshot::BytesPerFrame() const
{
    return m_bytesPerFrame;
}

std::size_t FrameSnapshot::FrameCount() const
{
    return m_frameCount;
}

const unsigned char* FrameSnapshot::Frame(std::size_t index) const
{
    if (!IsValid() || index >= m_frameCount)
    {
        return 0;
    }
    return m_pixels.Data() + index * m_bytesPerFrame;
}

const std::string& FrameSnapshot::LastError() const
{
    return m_error;
}

bool FrameSnapshot::Capture(const FrameBuffer& buffer, std::size_t frameCount,
                            FrameSnapshot& snapshot)
{
    snapshot = FrameSnapshot();

    if (!buffer.IsInitialized() || buffer.Data() == 0)
    {
        snapshot.m_error = "FrameSnapshot: the capture buffer is not initialised";
        return false;
    }

    if (frameCount == 0)
    {
        snapshot.m_error = "FrameSnapshot: at least one frame is required";
        return false;
    }

    if (frameCount > buffer.Count())
    {
        snapshot.m_error = "FrameSnapshot: the buffer holds fewer frames than were requested";
        return false;
    }

    // frameCount <= Count() <= Capacity(), and Capacity() was checked against
    // overflow by FrameBuffer::Init(), so this product cannot wrap.
    const std::size_t pixelBytes = frameCount * buffer.BytesPerFrame();
    const unsigned char* source = buffer.Data();

    if (!snapshot.m_pixels.CopyFrom(source, pixelBytes))
    {
        snapshot = FrameSnapshot();
        snapshot.m_error = "FrameSnapshot: copying the frames failed (out of memory)";
        return false;
    }

    snapshot.m_width = buffer.Width();
    snapshot.m_height = buffer.Height();
    snapshot.m_bytesPerFrame = buffer.BytesPerFrame();
    snapshot.m_frameCount = frameCount;
    snapshot.m_error.clear();
    return true;
}

bool FrameSnapshot::TakeFrom(FrameBuffer& buffer, std::size_t frameCount,
                             FrameSnapshot& snapshot)
{
    // Whatever the target held before is given up first, so a refused transfer
    // never leaves a half filled snapshot behind.
    snapshot = FrameSnapshot();

    if (!buffer.IsInitialized() || buffer.Data() == 0)
    {
        snapshot.m_error = "FrameSnapshot: the capture buffer is not initialised";
        return false;
    }

    if (frameCount == 0)
    {
        snapshot.m_error = "FrameSnapshot: at least one frame is required";
        return false;
    }

    if (frameCount > buffer.Count())
    {
        snapshot.m_error = "FrameSnapshot: the buffer holds fewer frames than were requested";
        return false;
    }

    // Every rejection is behind us: from here on the frames change owner. The
    // whole allocation moves, not just the frames asked for, so the snapshot may
    // own more bytes than it holds - and not one byte is copied, which is what
    // makes a forty gigabyte capture affordable.
    if (!snapshot.m_pixels.Adopt(buffer.m_storage))
    {
        snapshot = FrameSnapshot();
        snapshot.m_error = "FrameSnapshot: the capture buffer has no storage to hand over";
        return false;
    }

    // frameCount <= Count() <= Capacity(), so the frames are a prefix of the
    // block that was just taken over.
    snapshot.m_pixels.SetBytes(frameCount * buffer.BytesPerFrame());

    snapshot.m_width = buffer.Width();
    snapshot.m_height = buffer.Height();
    snapshot.m_bytesPerFrame = buffer.BytesPerFrame();
    snapshot.m_frameCount = frameCount;
    snapshot.m_error.clear();

    // The buffer owns nothing now: no geometry, no frames, and it cannot be
    // appended to until Init() gives it a new allocation.
    buffer.ResetState();
    buffer.m_error.clear();
    return true;
}

// --- SaveProgress ----------------------------------------------------------

SaveProgress::SaveProgress()
    : total(0),
      completed(0),
      failed(0),
      running(false),
      cancelled(false),
      lastError(),
      bytesWritten(0),
      elapsedSeconds(0.0),
      bytesPerSecond(0.0)
{
}

// --- SaveSink --------------------------------------------------------------

SaveSink::~SaveSink()
{
}

BmpWriteResult BmpFileSink::Write(const std::wstring& path,
                                  const unsigned char* pixels,
                                  int width, int height, int pitch)
{
    return BmpWriter::WriteGrayscale8(path, pixels, width, height, pitch);
}

// --- SaveWorker ------------------------------------------------------------

SaveWorker::SaveWorker()
    : m_mutex(),
      m_jobFinished(),
      m_thread(),
      m_defaultSink(),
      m_jobSnapshot(),
      m_jobFolder(),
      m_jobPrefix(),
      m_failNextThreadStart(false),
      m_running(false),
      m_cancelled(false),
      m_total(0),
      m_completed(0),
      m_failed(0),
      m_lastError(),
      m_bytesWritten(0),
      m_startedAt(),
      m_elapsedSeconds(0.0)
{
}

SaveWorker::~SaveWorker()
{
    // Order matters: ask the job to stop, then wait for the thread. A worker is
    // never destroyed with a running thread.
    Cancel();
    Wait();
}

bool SaveWorker::Start(FrameSnapshot& snapshot, const std::wstring& folder,
                       const std::wstring& prefix, SaveSink* sink)
{
    return Begin(snapshot, folder, prefix, sink);
}

bool SaveWorker::Begin(FrameSnapshot& snapshot, const std::wstring& folder,
                       const std::wstring& prefix, SaveSink* sink)
{
    // Everything the previous job left behind, released outside the lock: the
    // finished thread and the frames it was writing with.
    std::thread finished;
    FrameSnapshot retired;
    bool started = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_running)
        {
            m_lastError = kAlreadyRunning;
            return false;
        }

        if (!snapshot.IsValid())
        {
            m_lastError = kInvalidSnapshot;
            return false;
        }

        if (folder.empty())
        {
            m_lastError = kEmptyFolder;
            return false;
        }

        // The previous job has finished (a running one was rejected above) but
        // its thread may not have been joined yet. It is joined outside the
        // lock so that Start() can never deadlock against a worker that still
        // needs the lock to finish.
        if (m_thread.joinable())
        {
            finished.swap(m_thread);
        }

        // Run() has finished with the previous job's frames, so they are given
        // up here (destroyed after the lock is released).
        retired = std::move(m_jobSnapshot);

        // The frames change owner only now, with every rejection above behind
        // us - and even from here they stay recoverable: the very same frame set
        // is moved straight back to the caller when the thread cannot be
        // created.
        m_jobSnapshot = std::move(snapshot);
        m_jobFolder = folder;
        m_jobPrefix = prefix;

        m_total = m_jobSnapshot.FrameCount();
        m_completed = 0;
        m_failed = 0;
        m_cancelled = false;
        m_lastError.clear();

        // The metrics of the new job start from zero. The clock starts here,
        // before the thread exists, so the reported duration covers the whole
        // job as the caller experienced it - including spawning the thread.
        m_bytesWritten = 0;
        m_elapsedSeconds = 0.0;
        m_startedAt = std::chrono::steady_clock::now();

        m_running = true;

        try
        {
            if (m_failNextThreadStart)
            {
                // One-shot seam: the caller has asked for a deterministic
                // thread-creation failure. It is cleared before the throw so
                // the worker stays usable.
                m_failNextThreadStart = false;
                throw std::runtime_error(kThreadCreation);
            }

            m_thread = std::thread(&SaveWorker::Run, this, sink);
            started = true;
        }
        catch (...)
        {
            // No job is running after all, so the worker stays restartable and
            // the frames are handed straight back to the caller - same object,
            // same pointers, still valid. A rejected Start() must never swallow
            // a capture.
            m_running = false;
            m_total = 0;
            m_lastError = kThreadCreation;
            // No job ran after all, so there is no progress to report for it
            // either - a refused Start() must never look like a started job.
            m_bytesWritten = 0;
            m_elapsedSeconds = 0.0;
            snapshot = std::move(m_jobSnapshot);
            m_jobFolder.clear();
            m_jobPrefix.clear();
        }
    }

    if (finished.joinable())
    {
        finished.join();
    }

    return started;
}

SaveProgress SaveWorker::Progress() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    SaveProgress progress;
    progress.total = m_total;
    progress.completed = m_completed;
    progress.failed = m_failed;
    progress.running = m_running;
    progress.cancelled = m_cancelled;
    progress.lastError = m_lastError;

    // A running job reports how long it has been running so far; a finished one
    // reports the duration it took. Both are read under the lock, so the copy
    // is consistent with the counters above.
    progress.elapsedSeconds = m_running
        ? ElapsedSeconds(m_startedAt, std::chrono::steady_clock::now())
        : m_elapsedSeconds;
    progress.bytesWritten = m_bytesWritten;
    progress.bytesPerSecond = BytesPerSecond(m_bytesWritten, progress.elapsedSeconds);
    return progress;
}

void SaveWorker::Cancel()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running)
    {
        m_cancelled = true;
    }
}

void SaveWorker::Wait()
{
    std::thread finished;
    FrameSnapshot retired;

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_thread.joinable())
        {
            return;
        }

        while (m_running)
        {
            m_jobFinished.wait(lock);
        }

        finished.swap(m_thread);

        // The job is over, so Run() no longer reads the frames it was given:
        // they are released below, outside the lock.
        retired = std::move(m_jobSnapshot);
    }

    if (finished.joinable())
    {
        finished.join();
    }
}

bool SaveWorker::ReapFinished()
{
    std::thread finished;
    FrameSnapshot retired;
    bool reaped = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // A running job is never waited for: joining it is not what this call
        // is for, and blocking here is exactly what it exists to avoid.
        if (m_thread.joinable() && !m_running)
        {
            finished.swap(m_thread);
            retired = std::move(m_jobSnapshot);
            reaped = true;
        }
    }

    if (finished.joinable())
    {
        finished.join();
    }

    return reaped;
}

void SaveWorker::FailNextThreadStartForTest()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failNextThreadStart = true;
}

void SaveWorker::Run(SaveSink* sink)
{
    // The worker owns its inputs for the whole job. They are only written while
    // no job is running, so reading them here needs no lock.
    const std::size_t total = m_jobSnapshot.FrameCount();
    const int width = m_jobSnapshot.Width();
    const int height = m_jobSnapshot.Height();
    const std::wstring& folder = m_jobFolder;
    const std::wstring& prefix = m_jobPrefix;
    SaveSink* destination = (sink != 0) ? sink : &m_defaultSink;

    // The frame the job is working on, and the reason it was cut short. Both
    // are written inside the guarded block and read right after it, on this
    // thread only, so no lock is needed for them.
    std::size_t index = 0;
    bool aborted = false;
    std::string abortReason;

    try
    {
        for (index = 0; index < total; ++index)
        {
            // Cancellation is only ever honoured between frames: a frame that
            // is already being written is finished and counted, never
            // abandoned half way through, which keeps the set of files
            // consistent.
            if (IsCancelled())
            {
                break;
            }

            // Path construction, FileNameForIndex() and the write itself are
            // all inside this block: every one of them can fail in a way that
            // is not a return value - an allocation failure while building a
            // path, a sink that throws - and none of it may leave the thread.
            const std::wstring path = folder + kPathSeparator +
                                      FileNameForIndex(prefix, index, total);
            const unsigned char* pixels = m_jobSnapshot.Frame(index);

            BmpWriteResult result;
            if (pixels == 0)
            {
                result.error = kMissingFrame;
            }
            else
            {
                // A packed snapshot frame: the row pitch is the frame width.
                result = destination->Write(path, pixels, width, height, width);
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (result.success)
            {
                ++m_completed;
                // Only the bytes the sink really reported as written are
                // counted. A frame that failed contributes nothing, not even
                // the part of it that may have reached the disk before the
                // failure was noticed.
                m_bytesWritten += result.bytesWritten;
            }
            else
            {
                ++m_failed;
                if (!result.error.empty())
                {
                    m_lastError = result.error;
                }
            }
        }
    }
    catch (const std::exception& exception)
    {
        // Nothing outside the worker can protect it from the sink: an
        // exception leaving Run() calls std::terminate() and would destroy the
        // process with the capture still unsaved. It is turned into the same
        // kind of state a reported write failure produces - a failure count and
        // a reason - and the job ends.
        aborted = true;
        abortReason = ExceptionFailureMessage(index, total, exception.what());
    }
    catch (...)
    {
        aborted = true;
        abortReason = ExceptionFailureMessage(index, total, 0);
    }

    // Reached on every path - a completed loop, a cancellation, or an
    // exception caught above - so a waiter never observes a half updated job.
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (aborted)
        {
            // The frame the job died on was not written, so it is the single
            // failure that ended the job. It is only counted when the job has
            // not already accounted for every frame, so completed + failed can
            // never exceed total.
            if (m_completed + m_failed < m_total)
            {
                ++m_failed;
            }
            m_lastError = abortReason;
        }

        // The job is over: its duration is frozen here, so every later read of
        // Progress() reports the same number and the throughput it implies.
        m_elapsedSeconds = ElapsedSeconds(m_startedAt, std::chrono::steady_clock::now());

        m_running = false;
    }

    // Wakes every Wait(); the state it needs is already visible because
    // m_running was cleared under the lock.
    m_jobFinished.notify_all();
}

bool SaveWorker::IsCancelled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancelled;
}

std::wstring SaveWorker::FileNameForIndex(const std::wstring& prefix,
                                          std::size_t index,
                                          std::size_t totalFrames)
{
    // Five digits cover the 0..9999 frames of a normal capture; a longer job
    // widens the whole field at once, so the names of one job stay unique and
    // their lexicographic order stays the order of the frames.
    std::size_t field = 5;
    const std::size_t highest = (totalFrames == 0) ? 0 : totalFrames - 1;
    const std::size_t digits = DigitCount(highest);
    if (digits > field)
    {
        field = digits;
    }

    std::wostringstream stream;
    stream << prefix << L'_'
           << std::setw(static_cast<int>(field)) << std::setfill(L'0')
           << index << L".bmp";
    return stream.str();
}

} // namespace grablinkcore
