/*
+-------------------------------- DISCLAIMER ---------------------------------+
|                                                                             |
| This application program is provided to you free of charge as an example.   |
| Despite the considerable efforts of Euresys personnel to create a usable    |
| example, you should not assume that this program is error-free or suitable  |
| for any purpose whatsoever.                                                 |
|                                                                             |
| EURESYS does not give any representation, warranty or undertaking that this |
| program is free of any defect or error or suitable for any purpose. EURESYS |
| shall not be liable, in contract, in torts or otherwise, for any damages,   |
| loss, costs, expenses or other claims for compensation, including those     |
| asserted by third parties, arising out of or in connection with the use of  |
| this program.                                                               |
|                                                                             |
+-----------------------------------------------------------------------------+
*/


// GrablinkSnapshotDoc.h : interface of the CGrablinkSnapshotDoc class
//
/////////////////////////////////////////////////////////////////////////////

#if !defined(AFX_PROJECT1DOC_H__A2C9A054_8AFA_4C3E_A5AA_2E1A44CD3916__INCLUDED_)
#define AFX_PROJECT1DOC_H__A2C9A054_8AFA_4C3E_A5AA_2E1A44CD3916__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#include "GrablinkSnapshot.h"
#include "multicam.h"
#include "BmpHelper.h"      //ours
#include "core/FrameBuffer.h"   // hardware-free, virtual-alloc backed frame storage
#include "core/SaveWorker.h"    // hardware-free asynchronous BMP save job
#include "core/CallbackDrain.h" // hardware-free admission gate for the driver callback
#include "core/PreviewPublisher.h" // application-owned pool of live preview frames
#include "core/RefreshCoalescer.h" // one-outstanding-message preview refresh gate
#include "core/ModalScopeCounter.h" // application-modal suppression of the preview refresh
#include "core/CaptureStats.h" // callback-safe aggregate capture diagnostics

#include <atomic>
#include <cstddef>
#include <string>

//---------------------------------------------------------------------------
// Window messages the acquisition callback posts to the view.
//
// They live in the WM_APP range on purpose: WM_USER is owned by the window
// classes themselves and MFC uses it for its own control-notification
// plumbing, so posting there from the driver's signal thread both risks a
// collision and makes it impossible to tell application messages from control
// notifications. The callback only ever posts these to the window the document
// has cached; it never walks the MFC view list (PostViewMessage below).
#define WM_APP_PREVIEW_REFRESH   (WM_APP + 1)  // redraw the live preview
#define WM_APP_CAPTURE_COMPLETE  (WM_APP + 2)  // buffer full: the UI thread owes a save
#define WM_APP_ACQUISITION_ERROR (WM_APP + 3)  // frame store or acquisition failure

//---------------------------------------------------------------------------
// Callback function declaration
void WINAPI GlobalCallback (PMCSIGNALINFO SigInfo);

//---------------------------------------------------------------------------
// RAII holder for the capture critical section. Every callback path that takes
// the lock gives it back on the way out, including the early returns, so the
// acquisition thread can never be left holding it.
class CDocumentCaptureLock
{
public:
    explicit CDocumentCaptureLock(CRITICAL_SECTION& section) : m_section(section)
    {
        ::EnterCriticalSection(&m_section);
    }

    ~CDocumentCaptureLock()
    {
        ::LeaveCriticalSection(&m_section);
    }

private:
    CDocumentCaptureLock(const CDocumentCaptureLock&);
    CDocumentCaptureLock& operator=(const CDocumentCaptureLock&);

    CRITICAL_SECTION& m_section;
};

class CGrablinkSnapshotDoc : public CDocument
{

// Application specific data
public:
    MCHANDLE m_Channel;
    void Callback (PMCSIGNALINFO SigInfo);
    int m_SizeX;
    int m_SizeY;
    int m_BufferPitch;
    //ours
    // Frames one capture can hold. Operator-configurable through
    // "Capture Settings..." and persisted in the application profile; see
    // CGrablinkSnapshotDoc::OnCaptureSettings().
    int _numImages;
    CString _strFilename;
    CString _outputFolder;
    bool _bResizeImage;  // scale image to fit viewport

    // Race free copies of the capture and save state. The acquisition callback
    // mutates them on the driver's signal thread, so the view must never read
    // the fields directly; these accessors take the capture lock instead.
    std::size_t CaptureFrameCount() const;
    std::size_t CaptureFrameCapacity() const;
    bool IsCapturing() const;
    grablinkcore::SaveProgress GetSaveProgress() const;

    // Race free copy of the acquisition diagnostics the driver callback
    // accumulates - surfaces received, frames stored, frames rejected,
    // acquisition failures, preview published and dropped, per-callback cost and
    // the capture window. Safe to call from any thread at any time, including
    // while a capture is running: the counters are atomics read one by one, so
    // the copy is not transactional while the callback is active. Capture-window
    // values stop changing when a run ends, but preview and surface counters
    // continue to advance until the channel is drained.
    //
    // It is read by the on-demand "Capture Diagnostics..." panel, which shows
    // these counters beside the frame-buffer geometry and the SaveWorker's own
    // progress without touching the capture path.
    grablinkcore::CaptureStatsSnapshot CaptureSnapshot() const;

    // Frames that were taken out of the capture buffer but whose save could not
    // be started. They are kept here (UI thread only) so a failed save never
    // costs the user the capture: "Stop & Save" retries them.
    std::size_t PendingSaveFrameCount() const;

    // Registers/clears the window the acquisition callback posts to. Both are
    // called from the UI thread - the view registers itself in OnInitialUpdate
    // and clears itself while it is being destroyed - so the driver's signal
    // thread never has to walk the MFC view list.
    void RegisterViewWindow(HWND hWnd);
    void UnregisterViewWindow(HWND hWnd);

    // UI-thread handlers for the messages the callback posted. They run on the
    // view's window thread and are where all the work the callback must not do
    // happens: validating the destination, copying the path state, taking the
    // buffer over and starting the save.
    void OnCaptureCompleteMessage();
    void OnAcquisitionErrorMessage();

    // Begins one application-modal scope and returns the guard that holds it.
    // The UI thread holds the guard for the whole span of a modal call it makes
    // - while SHBrowseForFolder runs in OnSetFolder(), or while the About
    // dialog's DoModal() runs, which is the path the application hands the guard
    // out for - and the acquisition callback stops posting
    // WM_APP_PREVIEW_REFRESH for exactly that span. Preview pixels keep being
    // published throughout, and the capture-complete and acquisition-error posts
    // are untouched.
    //
    // Scopes nest, so a modal call opened from inside another keeps the
    // suppression up until the outermost one has returned. The returned guard is
    // movable and not copyable and ends its scope in its destructor, so every
    // exit from the modal call - including an early return somebody adds later -
    // releases the scope exactly once.
    grablinkcore::ModalScope BeginModalScope();

    // Live preview, application owned. The acquisition callback publishes each
    // acquired frame into the document's own pool (m_preview) and the view draws
    // from that copy, never from the grabber-owned surface the driver may
    // recycle the moment the callback returns.
    //
    // These five are the whole door into the publisher: the view takes one frame
    // from the UI thread, draws it while the lease is held, and gives it back,
    // and it reads the geometry of the frame it just took rather than inferring
    // anything from the source pitch. The pool itself stays private so no other
    // code can reach into it.
    const unsigned char* TakePreviewForDisplay();
    void ReleasePreviewDisplay();
    void AcknowledgePreviewRefresh();
    int PreviewWidth() const;
    int PreviewHeight() const;
    std::size_t PreviewPitch() const;

protected: // create from serialization only
    CGrablinkSnapshotDoc();
    DECLARE_DYNCREATE(CGrablinkSnapshotDoc)

// Operations
public:

// Overrides
    // ClassWizard generated virtual function overrides
    //{{AFX_VIRTUAL(CGrablinkSnapshotDoc)
public:
    virtual BOOL OnNewDocument();
    virtual void Serialize(CArchive& ar);
    //}}AFX_VIRTUAL

// Implementation
public:
    virtual ~CGrablinkSnapshotDoc();
#ifdef _DEBUG
    virtual void AssertValid() const;
    virtual void Dump(CDumpContext& dc) const;
#endif

protected:
    // Tears down everything a previous run left behind - the callback
    // registration, the channel, the capture state, the live preview pool and
    // any save - so that OnNewDocument() is idempotent (an SDI Ctrl+N calls it
    // again on this very document). Shared with the destructor.
    void ResetCaptureState();

    // Helpers shared by the UI-thread handlers.
    //
    // Makes sure the configured base output folder exists before anything
    // consumes a capture buffer; records the reason in m_strLastError when it
    // cannot.
    bool PrepareOutputFolder();

    // Claims one run-unique subfolder under the configured base. A retry reuses
    // the claim so frames whose worker start failed stay bound to one location.
    bool PrepareRunFolder();

    // UI thread only: copies the path state out of the CStrings and hands
    // "snapshot" to the worker. A refused start leaves the frames with the
    // caller, which keeps them in m_pendingSnapshot for a retry.
    bool TryStartSave(grablinkcore::FrameSnapshot& snapshot);

    // Posts "message" to the cached view window, if there is one. Returns true
    // only when a message really was posted, which is what the callback uses to
    // tell a refused post from a queued one.
    bool PostViewMessage(UINT message) const;

    // Stops the active capture run, if one is live, and closes the capture
    // window in the statistics. Must be called with m_captureLock held, which
    // is where _bCapturing is guarded.
    //
    // The true to false edge of _bCapturing is the whole guard: the callback and
    // the UI command handlers all end a run through here, so EndCapture() is
    // called once per run and never for a run that was already stopped. It
    // returns true only when this call was the one that ended the run.
    bool EndCaptureRunUnderLock() noexcept;

    // MBCS (LPCSTR) to UTF-16 conversion for the paths handed to the worker.
    // UI thread only: it reads CStrings.
    static std::wstring ToWideString(const CString& text);

private:
    // Shows "text" in a message box with "caption" and "flags" and returns
    // what ::MessageBox returned. The box is shown while one application-modal
    // scope from BeginModalScope() is held, for exactly the span of the call, so
    // the acquisition callback stops posting preview refreshes while the box is up
    // - the same discipline the folder chooser and the About box already follow,
    // because ::MessageBox also runs a nested message loop on this thread.
    //
    // The scope is owned by a local guard inside the helper, so it is released on
    // every path out of the call and can never be stranded by a caller.
    //
    // UI thread only: the scope is ended on the thread that began it and the depth
    // it maintains describes this thread's modal calls, so the helper asserts
    // against the thread the document was created on. It is private because it
    // exists for this document's own command and posted-message handlers only.
    int ShowModalMessageBox(LPCTSTR text, LPCTSTR caption, UINT flags);

    // Aborts a setup that failed in OnNewDocument(): reports "message" on the UI
    // thread and releases whatever the partial setup had created. A refused
    // MultiCam call leaves a handle that must not be driven further, so the
    // setup stops there and this tears the generation down through
    // ResetCaptureState() - the same drain-safe path Ctrl+N and the destructor
    // use, and safe on a partial generation because it deletes the channel only
    // when one was actually created and waits out any admitted callback first.
    // This is what keeps a failed open from leaking the channel until the
    // document is destroyed. Must be called with m_captureLock NOT held, because
    // it shows a message box and ResetCaptureState() takes the lock itself.
    BOOL AbortChannelSetup(LPCTSTR message);

protected:
    // Capture state. Everything below is guarded by m_captureLock, except the
    // worker (which carries its own synchronisation) and m_pendingSnapshot
    // (which is only ever touched from the UI thread).
    mutable CRITICAL_SECTION m_captureLock;
    grablinkcore::FrameBuffer m_frameBuffer;
    grablinkcore::SaveWorker m_saveWorker;

    // Aggregate acquisition diagnostics, fed from the callback and read through
    // CaptureSnapshot(). Every Record*() is a relaxed atomic operation, so it is
    // safe on the driver's signal thread; it is a plain member rather than a
    // static or a global so nothing outlives the document that owns it.
    //
    // It is cleared in ResetCaptureState(), and only there - that is the one
    // point at which the admission gate has reported that no callback is in
    // flight, so a reset cannot race a recording callback.
    //
    // SaveWorker owns save progress and diagnostics; this member only tracks
    // acquisition signals and capture windows.
    grablinkcore::CaptureStats m_captureStats;

    // QueryPerformanceCounter ticks per second, cached at construction because
    // the value is fixed for the system and the callback must not query it per
    // frame. Zero when the performance counter is unusable, in which case no
    // callback duration is recorded at all (a stream of fake zero-cost callbacks
    // would be worse than a missing measurement). Written once, on the UI
    // thread, before OnNewDocument() registers the callback that reads it.
    unsigned long long m_captureTicksPerSecond;

    // Admission gate for the acquisition callback, and the only thing that
    // makes the callback safe to unregister: ResetCaptureState() closes it and
    // then waits until no admitted callback is still running before it deletes
    // the channel or releases the buffer, and OnNewDocument() re-arms it only
    // after the previous generation has drained. It is deliberately not guarded
    // by m_captureLock - the callback takes an entry before it takes the lock,
    // so the gate can never be closed by a thread that a callback is waiting on.
    grablinkcore::CallbackDrain m_callbackDrain;

    // The live preview pool. Configure()d once per document generation, right
    // after the driver's image geometry has been read and before the callback is
    // registered, so the driver's signal thread can never publish into an
    // unconfigured pool; Reset() only once the drain above has reported that no
    // admitted callback is running, so no publish can race the release of the
    // pool. The callback reaches it through Publish() and holds no lock of the
    // document while doing so: the publisher is guarded by its own mutex, which
    // is why it must stay out of m_captureLock.
    grablinkcore::PreviewPublisher m_preview;

    // At most one WM_APP_PREVIEW_REFRESH may wait in the UI queue. New frames
    // still replace older unpublished preview pixels while that message is
    // pending; the view acknowledges it when dispatch begins.
    grablinkcore::RefreshCoalescer m_previewRefresh;

    // Counts the application-modal scopes the UI thread is inside right now:
    // the folder chooser, the About box, and every later modal call that adopts
    // BeginModalScope(). Any such call runs a nested message loop that dispatches
    // messages for every window of the thread, so a preview refresh the
    // acquisition callback posts during that time is handled inside the modal
    // loop - which is the behaviour this counter exists to suppress.
    //
    // It replaces the folder-dialog-only flag rather than adding to it: counting
    // the scopes is what lets one modal call be opened from inside another and
    // keeps the suppression up until the outermost one has returned, and the
    // movable guard in core releases a scope exactly once on every path out of
    // the call.
    //
    // It holds atomics rather than a lock on purpose. The driver's signal thread
    // reads IsSuppressed() on every acquired frame, and taking m_captureLock on
    // that path would make the callback wait behind a UI thread that is sitting
    // in a modal loop. It is a member of the document rather than a static or a
    // global so nothing outlives the document that owns it, and it is
    // constructed unsuppressed.
    grablinkcore::ModalScopeCounter m_modalScopes;

    // The thread this document was created on, which is the UI thread. Every
    // message box the document shows goes through ShowModalMessageBox(), and that
    // helper asserts against this value: the modal scope it holds describes this
    // thread's modal calls, so showing a box from anywhere else would corrupt the
    // count that suppresses the preview refresh.
    DWORD m_uiThreadId;

    CString m_strLastError;
    bool _bCapturing;  // true when actively capturing to buffer

    // Cached message target, guarded by m_captureLock: the callback reads it
    // under the lock and posts to it, nothing more.
    HWND m_viewWindow;

    // POD flags the callback sets under the lock; the UI-thread handlers
    // consume them. They carry no text and allocate nothing.
    BOOL m_captureCompletePending;    // the buffer filled up
    BOOL m_captureErrorPending;       // a frame could not be stored
    BOOL m_acquisitionFailurePending; // the driver reported an acquisition failure

    // Surface-address read failure. McGetParamPtr(MC_SurfaceAddr) returns a
    // MCSTATUS and the callback checks it; a refusal means the driver could not
    // hand the callback the address of the surface for that signal, so nothing
    // can be stored or published from it. The callback keeps only POD values:
    // the raw status and the two flags below. No text, no allocation and no
    // driver description lookup happen on the driver's signal thread - the
    // UI-thread handler builds the operator-facing message from m_surfaceAddrStatus.
    // All three are guarded by m_captureLock.
    MCSTATUS m_surfaceAddrStatus;     // MCSTATUS returned by the refused read
    BOOL m_surfaceAddrErrorPending;   // a refused read is not reported to the operator yet
    BOOL m_surfaceAddrErrorNotified;  // a refusal has already been posted for this generation

    // Frames whose save could not be started, kept for a retry. UI thread only.
    grablinkcore::FrameSnapshot m_pendingSnapshot;

    // UI-thread-owned path claimed for the current capture. It survives a
    // failed worker start so Stop & Save retries the same run directory, and is
    // cleared only when a new capture begins or the document is reset.
    std::wstring m_runFolderPath;

protected:
    // Generated message map functions


protected:
    //{{AFX_MSG(CGrablinkSnapshotDoc)
    afx_msg void OnGo();
    afx_msg void OnStop();
    afx_msg void OnStopSave();
    afx_msg void OnSetFolder();
    afx_msg void OnCaptureSettings();
    afx_msg void OnViewFitWindow();
    afx_msg void OnUpdateViewFitWindow(CCmdUI* pCmdUI);
    afx_msg void OnCaptureDiagnostics();
    //}}AFX_MSG
    DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_PROJECT1DOC_H__A2C9A054_8AFA_4C3E_A5AA_2E1A44CD3916__INCLUDED_)
