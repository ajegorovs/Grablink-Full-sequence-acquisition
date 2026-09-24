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


// GrablinkSnapshotDoc.cpp : implementation of the CGrablinkSnapshotDoc class
//
#include "stdafx.h"
#include "GrablinkSnapshot.h"

#include "GrablinkSnapshotDoc.h"
#include "GrablinkSnapshotView.h"
#include "core/RunFolder.h"
#include <stdlib.h>
//custom
using namespace std;
#include <cstdlib>
#include <cstring>
#include <string>
#include <cmath>
#include <utility>
#include <limits>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// Capture settings
//
// The capture capacity is operator-configurable ("Capture Settings...") and is
// kept in the application profile, so it survives a restart. The profile is the
// MFC profile API - CGrablinkSnapshotApp::InitInstance() calls SetRegistryKey(),
// so GetProfileInt() and WriteProfileInt() land under
// HKCU\Software\Local AppWizard-Generated Applications\GrablinkSnapshot - and
// the 200 frames this application has always captured stay the default.

namespace {

const TCHAR* const kCaptureSettingsSection = _T("Capture");
const TCHAR* const kCaptureFramesKey = _T("FrameCount");
const int kDefaultCaptureFrames = 200;

} // namespace

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc

IMPLEMENT_DYNCREATE(CGrablinkSnapshotDoc, CDocument)

BEGIN_MESSAGE_MAP(CGrablinkSnapshotDoc, CDocument)
    //{{AFX_MSG_MAP(CGrablinkSnapshotDoc)
    ON_COMMAND(ID_GO, OnGo)
    ON_COMMAND(ID_STOP, OnStop)
    ON_COMMAND(ID_STOP_SAVE, OnStopSave)
    ON_COMMAND(ID_SET_FOLDER, OnSetFolder)
    ON_COMMAND(ID_CAPTURE_SETTINGS, OnCaptureSettings)
    ON_COMMAND(ID_VIEW_FIT_WINDOW, OnViewFitWindow)
    ON_UPDATE_COMMAND_UI(ID_VIEW_FIT_WINDOW, OnUpdateViewFitWindow)
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc construction/destruction

CGrablinkSnapshotDoc::CGrablinkSnapshotDoc()
{
    // The capture lock has to exist before any member that could be reached
    // from the driver thread, and before the document can fail to open.
    ::InitializeCriticalSection(&m_captureLock);

    m_Channel = 0;
    m_SizeX = 0;
    m_SizeY = 0;
    m_BufferPitch = m_SizeX;

    //ours
    // Capture capacity: how many frames one capture can hold. It is
    // operator-configurable through "Capture Settings...", it is persisted in
    // the application profile so it survives a restart, and 200 - the capacity
    // this application has always used - is what a missing profile value means.
    //
    // A stored value is used only when it is positive. The image geometry is not
    // known this early in the document's life (the channel does not exist yet),
    // so that is as much as can be checked here: a value whose size does not fit
    // the geometry the driver reports is refused by the dialog before it is ever
    // stored, and a hand-edited profile value that turns out to be unusable is
    // reported by FrameBuffer::Init() when "Go!" runs rather than silently
    // corrected.
    _numImages = kDefaultCaptureFrames;
    {
        const int storedFrames = AfxGetApp()->GetProfileInt(
            kCaptureSettingsSection, kCaptureFramesKey, kDefaultCaptureFrames);
        if (storedFrames > 0)
        {
            _numImages = storedFrames;
        }
    }
    _strFilename = "Image";
    _bCapturing = false;
    _bResizeImage = false;
    // Keep development captures local and ignored instead of writing into a
    // previous experiment directory. The operator can choose another folder.
    _outputFolder = _T("SavedImages");
    m_strLastError.Empty();
    m_viewWindow = NULL;

    // The document is created on the UI thread, so this is the thread every
    // message box is shown from and the thread whose modal scopes
    // m_modalScopes counts. ShowModalMessageBox() asserts against it instead
    // of trusting the call site.
    m_uiThreadId = ::GetCurrentThreadId();
    m_captureCompletePending = FALSE;
    m_captureErrorPending = FALSE;
    m_acquisitionFailurePending = FALSE;

    // No modal call can be running while the document is being constructed, so
    // m_modalScopes is constructed unsuppressed and there is nothing to do here.
    BmpHelper::Init8bppHeaders();
}

CGrablinkSnapshotDoc::~CGrablinkSnapshotDoc()
{
    // The same teardown a Ctrl+N performs: close the callback gate, stop the
    // signals and the capture path, wait for admitted callbacks to drain, then
    // delete the channel and cancel and join the save. ResetCaptureState() only
    // returns once the drain has completed, so nothing the callback touches is
    // still in use when the lock is destroyed here.
    ResetCaptureState();

    ::DeleteCriticalSection(&m_captureLock);
}

void CGrablinkSnapshotDoc::ResetCaptureState()
{
    // Close the admission gate before anything else is touched. From here on a
    // callback is refused at the very top of Callback(), so nothing new can
    // reach the channel, the frame buffer or the posted-message flags. This is
    // the part the MultiCam calls below cannot do: they stop the driver from
    // *sending* signals, but a callback that is already running is unaffected
    // by any of them.
    m_callbackDrain.Disable();

    if (m_Channel != 0)
    {
        // Stop the driver from delivering the two signals this document ever
        // enabled, park the channel, and drop the callback registration. All
        // three act on future signals; none of them recalls a callback that is
        // already in flight, which is exactly what the drain below waits for.
        McSetParamInt(m_Channel, MC_SignalEnable + MC_SIG_SURFACE_PROCESSING,
                      MC_SignalEnable_OFF);
        McSetParamInt(m_Channel, MC_SignalEnable + MC_SIG_ACQUISITION_FAILURE,
                      MC_SignalEnable_OFF);

        // Set the channel to IDLE before deleting it
        McSetParamInt(m_Channel, MC_ChannelState, MC_ChannelState_IDLE);

        // Unregister the callback function
        McRegisterCallback(m_Channel, NULL, NULL);
    }

    // Wait for every callback that was admitted before the gate closed. The
    // channel, the frame buffer and the flags are all still alive here and are
    // only released below, once the drain has reported that no admitted
    // callback is running - that observation is the one moment at which the
    // state the callback touches may be freed.
    //
    // The first wait is generous but bounded, so the ordinary case (a short
    // callback that is already on its way out) costs nothing and the common
    // path never logs. A timeout is diagnosed, never acted on: continuing
    // towards McDelete() or m_frameBuffer.Clear() with a callback still in
    // flight is precisely the use-after-free this gate exists to prevent, so
    // the wait is repeated in bounded slices until the drain really completes.
    // No dialog is shown from here - this runs in the destructor and on the
    // Ctrl+N path, not on the UI thread's message loop - and none is shown from
    // the callback either.
    {
        const unsigned long kDrainSliceMs = 2000;
        bool drained = m_callbackDrain.WaitForDrain(kDrainSliceMs);

        if (!drained)
        {
            const grablinkcore::CallbackDrainSnapshot snapshot = m_callbackDrain.Snapshot();
            CString diagnostic;
            diagnostic.Format(
                _T("GrablinkSnapshot: callback drain timed out (inFlight=%lld, ")
                _T("admitted=%llu); still waiting for it to complete.\n"),
                snapshot.inFlight, snapshot.admitted);
            ::OutputDebugString(diagnostic);

            while (!drained)
            {
                drained = m_callbackDrain.WaitForDrain(kDrainSliceMs);
            }
        }
    }

    // No admitted callback is running now, so the capture state can be released
    // without racing one. The lock keeps a callback that re-entered (it cannot:
    // the gate is closed and every entry is refused) from touching the buffer
    // while it is emptied.
    //
    // The preview pool is released at this exact point, and for the same reason:
    // the drain that just completed is the moment at which no callback can be
    // inside Publish() any more, so this is the only place where the pool may be
    // released without a publish racing its release. It goes before the channel
    // and the frame buffer are torn down, so nothing is ever left holding a slot
    // of a pool that no longer exists. Reset() clears the slots and the
    // counters, which is what makes each document generation start with an
    // empty, unpainted preview.
    //
    // The publisher carries its own mutex and is deliberately not reached under
    // m_captureLock: Publish() (driver thread) and TakeForDisplay()/
    // ReleaseDisplay() (UI thread) are serialised by the publisher alone.
    m_preview.Reset();

    {
        CDocumentCaptureLock captureLock(m_captureLock);
        _bCapturing = false;
        m_captureCompletePending = FALSE;
        m_captureErrorPending = FALSE;
        m_acquisitionFailurePending = FALSE;

        // Frames of the previous document must not appear in the new one. The
        // storage itself is released and reallocated by the Init() in
        // OnNewDocument, under this same lock.
        m_frameBuffer.Clear();
    }

    if (m_Channel != 0)
    {
        // Delete the channel - the drain above has made sure no callback can be
        // using it any more.
        McDelete(m_Channel);
        m_Channel = 0;
    }

    // Cancel and join the asynchronous save, which also releases whatever the
    // snapshot it owns is still holding.
    m_saveWorker.Cancel();
    m_saveWorker.Wait();

    // Frames whose save never started belong to the old document too.
    m_pendingSnapshot = grablinkcore::FrameSnapshot();
    m_runFolderPath.clear();
}

BOOL CGrablinkSnapshotDoc::OnNewDocument()
{
    if (!CDocument::OnNewDocument())
        return FALSE;

    // In an SDI application Ctrl+N calls OnNewDocument() again on this very
    // document, so everything the previous run left behind - the callback
    // registration, the channel, the capture buffer and any running save - is
    // torn down first. Without this the old channel would leak and the buffer
    // would be reset under a callback that still belonged to it.
    //
    // ResetCaptureState() also closes the callback gate and waits for the
    // previous generation to drain, so the Enable() below re-arms a fresh
    // generation only once the old one can no longer be running.
    ResetCaptureState();

    // ================ CREATING IMAGE DIRECTORY !!!!===================
    if (CreateDirectory(_outputFolder.GetString(), NULL) ||
        ERROR_ALREADY_EXISTS == GetLastError())
    {
        //MessageBox(NULL, "Creating Directory.\n", "Error", MB_OK);
    }
    else
    {
        ShowModalMessageBox("Failed to create directory!\n", "Error", MB_OK);
    }



    // + GrablinkSnapshot Sample Program

    // Show scope of the sample program

    /*
    MessageBox(NULL,
        "This program demonstrates the SNAPSHOT Acquisition Mode on a Grablink Board.\n"
        "\n"
        "The \"Go!\" menu starts an acquisition sequence by activating the channel.\n"
        "By default, this program requires an area-scan camera connected on connector M.",
            "MultiCam sample description", MB_OK);
    */ //original


    // In order to support a 10-tap camera on Grablink Full
    // BoardTopology must be set to MC_BoardTopology_MONO_DECA
    // In all other cases the default value will work properly
    // and the parameter doesn't need to be set

    // Set the board topology to support 10 taps mode (only with a Grablink Full)
    McSetParamInt(MC_BOARD + 0, MC_BoardTopology, MC_BoardTopology_MONO_DECA);

    // Create a channel and associate it with the first connector on the first board
    McCreate(MC_CHANNEL, &m_Channel);
    McSetParamInt(m_Channel, MC_DriverIndex, 0);

    // In order to use single camera on connector A
    // MC_Connector need to be set to A for Grablink DualBase
    // For all the other Grablink boards the parameter has to be set to M

    // For all GrabLink boards but Grablink DualBase
    McSetParamStr(m_Channel, MC_Connector, "M");
    // For Grablink DualBase
    // McSetParamStr(m_Channel, MC_Connector, "A");

    // Choose the CAM file
    //McSetParamStr(m_Channel, MC_CamFile, "1000m_P50RG"); //org
    McSetParamStr(m_Channel, MC_CamFile, "acA2000-340km_P340SC"); //ours
    // Choose the camera expose duration
    McSetParamInt(m_Channel, MC_Expose_us, 20000);
    // Choose the pixel color format
    McSetParamInt(m_Channel, MC_ColorFormat, MC_ColorFormat_Y8);

    // Set the acquisition mode
    McSetParamInt(m_Channel, MC_AcquisitionMode, MC_AcquisitionMode_SNAPSHOT);

    // Choose the way the first acquisition is triggered
    McSetParamInt(m_Channel, MC_TrigMode, MC_TrigMode_IMMEDIATE);
    // Choose the triggering mode for subsequent acquisitions
    McSetParamInt(m_Channel, MC_NextTrigMode, MC_NextTrigMode_REPEAT);
    // Choose the number of images to acquire
    McSetParamInt(m_Channel, MC_SeqLength_Fr, MC_INDETERMINATE);

    // Retrieve image dimensions
    McGetParamInt(m_Channel, MC_ImageSizeX, &m_SizeX);
    McGetParamInt(m_Channel, MC_ImageSizeY, &m_SizeY);
    McGetParamInt(m_Channel, MC_BufferPitch, &m_BufferPitch);

    // Allocate the whole capture buffer before the channel is activated: the
    // driver starts delivering surfaces the moment the channel goes ACTIVE, and
    // a capture that could not be stored would be silently dropped.
    //
    // The range is reserved *and committed* upfront (MEM_RESERVE | MEM_COMMIT by
    // FrameBuffer::Init), so the whole commit capacity is charged against the
    // system commit limit as soon as this returns and the capture cannot fail
    // for lack of memory halfway through. No page is touched or zero filled,
    // however: physical pages are only backed as frames actually arrive, so the
    // buffer costs RAM in proportion to what has been captured.
    //
    // The lock is held so no callback can Append while the storage is replaced.
    bool allocated = false;
    {
        CDocumentCaptureLock captureLock(m_captureLock);
        allocated = m_frameBuffer.Init(m_SizeX, m_SizeY,
                                       static_cast<std::size_t>(_numImages));
    }

    if (!allocated)
    {
        CString message;
        message.Format(_T("Cannot allocate the capture buffer for %d x %d x %d frames.")
                       _T("\n\n%s\n\nThe channel was not started."),
                       m_SizeX, m_SizeY, _numImages, m_frameBuffer.LastError().c_str());
        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // The memory allocation for the images is automatically done by MultiCam when activating the channel.
    // We only set the number of surfaces to be created by MultiCam.
    McSetParamInt(m_Channel, MC_SurfaceCount, EURESYS_SURFACE_COUNT);

    // Prepare the application-owned preview pool, now that the driver has told
    // us the image geometry and before the callback that publishes into it is
    // registered below. The pool is fixed here - three slots of exactly the
    // image size - which is what lets Publish() run on the driver's signal
    // thread without ever allocating: the capture path must not touch the heap,
    // and a preview that allocated per frame would do exactly that.
    //
    // The geometry comes from the driver (MC_ImageSizeX/Y), never from the
    // buffer pitch: the pitch is a padded row stride, so using it as a width
    // would make the preview wider than the image and draw the padding as
    // picture. A failure here is fatal for the same reason a failed capture
    // buffer is: the document would otherwise run with a live acquisition and
    // no way to display it, and the operator would have no idea why the window
    // stayed black.
    if (!m_preview.Configure(m_SizeX, m_SizeY))
    {
        CString message;
        message.Format(_T("Cannot allocate the preview pool for %d x %d.")
                       _T("\n\n%s\n\nThe channel was not started."),
                       m_SizeX, m_SizeY, m_preview.LastError().c_str());
        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // Enable MultiCam signals
    McSetParamInt(m_Channel, MC_SignalEnable + MC_SIG_SURFACE_PROCESSING, MC_SignalEnable_ON);
    McSetParamInt(m_Channel, MC_SignalEnable + MC_SIG_ACQUISITION_FAILURE, MC_SignalEnable_ON);

    // Open the callback gate immediately before the callback is registered with
    // the driver. Arming it any later would present a closed gate to a callback
    // that is already arriving, which is the start-up half of the race this
    // gate exists to close; arming it earlier is not possible either, because
    // until ResetCaptureState() has drained the previous generation a callback
    // could still belong to it. From here Callback() counts itself in flight and
    // teardown will wait for it.
    m_callbackDrain.Enable();

    // Register the callback function
    McRegisterCallback(m_Channel, GlobalCallback, this);

    // Activate channel for live preview (without capturing to buffer)
    McSetParamInt(m_Channel, MC_ChannelState, MC_ChannelState_ACTIVE);

    // - GrablinkSnapshot Sample Program

    return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// GlobalCallback

void WINAPI GlobalCallback(PMCSIGNALINFO SigInfo)
{
    if (SigInfo && SigInfo->Context)
    {
        CGrablinkSnapshotDoc* pDoc = (CGrablinkSnapshotDoc*) SigInfo->Context;
        pDoc->Callback (SigInfo);
    }
}

/////////////////////////////////////////////////////////////////////////////
// Callback

// The callback runs on the driver's signal thread, once per acquired frame, and
// has to return to it promptly. Everything it does below is therefore either a
// POD write under the capture lock, a fixed-size copy into the preview pool, or
// a PostMessage to a window handle it copied under that same lock: no CString,
// no file system, no thread creation, no MessageBox and no MFC view traversal.
// All of the work that needs any of those happens later, on the UI thread, in
// the handlers for the messages posted here.
//
// The first thing it does is take an admission entry from m_callbackDrain, so a
// signal that arrives while the document is tearing the channel down is refused
// before any of the above happens. That guard is the only thing the gate adds
// here: no lock, no allocation and no wait.
void CGrablinkSnapshotDoc::Callback(PMCSIGNALINFO SigInfo)
{
    // Admit this callback before it touches anything. The guard is allocation
    // free - it holds one pointer and one flag - and takes an entry in its
    // constructor, so a callback that arrives while the gate is closed (that is,
    // while ResetCaptureState is tearing the channel down) is refused here and
    // returns without reading the surface pointer, the capture buffer or the
    // posted-message state. Leaving is owed for every admitted entry and is done
    // by the guard's destructor on every exit path below, so no return can
    // forget it.
    grablinkcore::CallbackDrain::Entry entry(m_callbackDrain);
    if (!entry.Entered())
    {
        return;
    }

    // + GrablinkSnapshot Sample Program

    switch(SigInfo->Signal)
    {
        case MC_SIG_SURFACE_PROCESSING:
        {
            // The surface the driver handed us for this very signal. It is a
            // callback-local pointer on purpose: the driver may recycle that
            // memory the moment this function returns, so storing it in a
            // document member (as the code used to) would let the UI thread read
            // a surface that no longer belongs to this frame. It is read here,
            // consumed by the copies below, and never kept.
            PVOID pCurrent = NULL;
            McGetParamPtr(SigInfo->SignalInfo, MC_SurfaceAddr, &pCurrent);

            // Store one frame, and only one, under the capture lock. A full
            // buffer or a refused frame is turned into a POD flag here; the
            // save that follows is owed to the UI thread and is asked for by
            // message, never performed on this thread.
            bool bufferFull = false;
            bool captureFailed = false;
            {
                CDocumentCaptureLock captureLock(m_captureLock);

                if (_bCapturing && m_frameBuffer.IsInitialized())
                {
                    if (m_frameBuffer.Count() < m_frameBuffer.Capacity())
                    {
                        // Exactly one frame per signal, consuming the source's
                        // padded rows through m_BufferPitch. Nothing is allocated
                        // here: the storage was reserved before the channel was
                        // activated. A failed Append leaves the stored frames
                        // untouched.
                        if (!m_frameBuffer.Append(
                                static_cast<const unsigned char*>(pCurrent),
                                m_BufferPitch))
                        {
                            // A refused frame (null surface, or a pitch narrower
                            // than the frame) would repeat on every signal, so
                            // capture is stopped instead of spinning on it. The
                            // reason stays in the buffer and is read by the UI
                            // handler.
                            captureFailed = true;
                            _bCapturing = false;
                            m_captureErrorPending = TRUE;
                        }
                    }

                    if (_bCapturing && m_frameBuffer.Count() >= m_frameBuffer.Capacity())
                    {
                        // Capturing is over. It is stopped atomically here so no
                        // further frame is stored; the buffer is handed to the
                        // save worker on the UI thread, which is what the
                        // posted message asks for.
                        _bCapturing = false;
                        bufferFull = true;
                        m_captureCompletePending = TRUE;
                    }
                }
            }

            if (captureFailed)
            {
                PostViewMessage(WM_APP_ACQUISITION_ERROR);
            }
            else if (bufferFull)
            {
                PostViewMessage(WM_APP_CAPTURE_COMPLETE);
            }

            //----------------------------------------
            //
            // Insert the eVision code here.
            //
            //----------------------------------------

            // Publish the frame into the document's own preview pool - only the
            // width bytes of every row, through the source's padded stride - and
            // ask the UI to redraw only when that really succeeded. The copy is
            // a memcpy into a slot that was allocated before the channel went
            // ACTIVE, so this is safe on the driver's signal thread: no
            // allocation, no CString, no dialog, no file and no MFC traversal.
            //
            // Posting on success rather than on every signal is what keeps a
            // refresh from being queued for a frame that never reached the pool
            // (a null surface, or every slot pinned by a slow repaint). The
            // publish itself coalesces: the pool is three slots, so a frame that
            // arrives while the UI is behind replaces an older unpublished one
            // instead of forcing a redraw per frame.
            //
            // No latch is consulted, and none could be: a volatile "refresh
            // completed" flag written by the UI thread and read here is not a
            // synchronisation primitive. The real handshake is the pool itself -
            // the display side takes a slot and gives it back.
            if (m_preview.Publish(static_cast<const unsigned char*>(pCurrent), m_BufferPitch))
            {
                // The frame is published either way: the pool always holds the
                // newest pixels, so a redraw that happens for any other reason
                // still shows a current image.
                //
                // The refresh post is skipped while any application-modal call
                // is running on the UI thread. Every one of them - the shell's
                // folder chooser, the About box, any dialog that follows them -
                // pumps a nested message loop, and that loop dispatches this
                // message too, so posting here would repaint the view from
                // inside the modal loop. The suppression is read before
                // TryClaimPost, never after: a claim that was taken but not
                // posted would have to be released again, and this path
                // deliberately never touches the coalescer while the suppression
                // is on.
                //
                // Nothing has to re-arm anything on the way out. The coalescer
                // is left exactly as it was, and the next live frame after the
                // modal call returns claims the next post in the ordinary way.
                // If a claim was already outstanding when the modal call began,
                // its message is still queued and the view acknowledges it when
                // the nested loop dispatches it, so the gate re-arms by itself.
                if (!m_modalScopes.IsSuppressed() &&
                    m_previewRefresh.TryClaimPost())
                {
                    // A failed PostMessage must release the claim; otherwise one
                    // transiently missing/destroyed target would suppress every
                    // later refresh for the lifetime of the document.
                    if (!PostViewMessage(WM_APP_PREVIEW_REFRESH))
                    {
                        m_previewRefresh.Acknowledge();
                    }
                }
            }
            break;
        }
        case MC_SIG_END_ACQUISITION_SEQUENCE:
            //MessageBox(NULL, "Images acquired.\n", "GrablinkSnapshot", MB_OK);
            break;

        case MC_SIG_ACQUISITION_FAILURE:
            // The modal report belongs to the UI thread; the callback only
            // flags the failure and posts for it.
            {
                CDocumentCaptureLock captureLock(m_captureLock);
                // The driver has declared the acquisition unusable. Stop storing
                // immediately so later surface notifications cannot make the run
                // appear complete after a failure.
                _bCapturing = false;
                m_captureCompletePending = FALSE;
                m_acquisitionFailurePending = TRUE;
            }
            PostViewMessage(WM_APP_ACQUISITION_ERROR);
            break;
        default:
            break;

    }
    // - GrablinkSnapshot Sample Program
}



/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc helpers

std::wstring CGrablinkSnapshotDoc::ToWideString(const CString& text)
{
    if (text.IsEmpty())
    {
        return std::wstring();
    }

    const int length = text.GetLength();
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, text, length, NULL, 0);
    if (needed <= 0)
    {
        return std::wstring();
    }

    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text, length, &wide[0], needed);
    return wide;
}

bool CGrablinkSnapshotDoc::PrepareOutputFolder()
{
    if (_outputFolder.IsEmpty())
    {
        m_strLastError = _T("No output folder is set. Use \"Set Output Folder...\" first.");
        return false;
    }

    const std::wstring folder = ToWideString(_outputFolder);

    // A directory that was really created is the end of the story.
    if (::CreateDirectoryW(folder.c_str(), NULL) != FALSE)
    {
        return true;
    }

    // ERROR_ALREADY_EXISTS only says that *something* is at that path - and it
    // can be a regular file. Accepting it would let the capture go ahead and
    // then fail every single frame write with "cannot create file", after the
    // buffer had already been consumed. Only a real directory is accepted.
    if (::GetLastError() == ERROR_ALREADY_EXISTS)
    {
        const DWORD attributes = ::GetFileAttributesW(folder.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return true;
        }

        m_strLastError.Format(
            _T("\"%s\" already exists, but it is a file, not a folder.")
            _T("\n\nChoose another output folder and try again."),
            (LPCTSTR)_outputFolder);
        return false;
    }

    m_strLastError.Format(_T("Cannot create or open the output folder:\n%s"),
                          (LPCTSTR)_outputFolder);
    return false;
}

bool CGrablinkSnapshotDoc::PrepareRunFolder()
{
    // A failed worker start leaves the frames and this claim available for an
    // exact retry. Do not create a second empty directory for the same capture.
    if (!m_runFolderPath.empty())
    {
        const DWORD attributes = ::GetFileAttributesW(m_runFolderPath.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return true;
        }

        m_runFolderPath.clear();
        m_strLastError = _T("The prepared run folder is no longer available. Choose another")
                         _T(" output folder and retry; the captured frames are still kept.");
        return false;
    }

    if (!PrepareOutputFolder())
    {
        return false;
    }

    SYSTEMTIME localTime;
    ::GetLocalTime(&localTime);
    const std::wstring token = grablinkcore::RunFolder::FormatTimestampToken(
        localTime.wYear, localTime.wMonth, localTime.wDay, localTime.wHour,
        localTime.wMinute, localTime.wSecond, localTime.wMilliseconds);

    grablinkcore::RunFolder claim;
    if (!claim.Create(ToWideString(_outputFolder), token, 1000))
    {
        m_strLastError.Format(_T("A unique run folder could not be created:\n%s"),
                              claim.LastError().c_str());
        return false;
    }

    m_runFolderPath = claim.Path();
    m_strLastError.Empty();
    return true;
}

bool CGrablinkSnapshotDoc::TryStartSave(grablinkcore::FrameSnapshot& snapshot)
{
    if (!snapshot.IsValid())
    {
        m_strLastError = _T("There is nothing to save.");
        return false;
    }

    if (m_saveWorker.Progress().running)
    {
        m_strLastError = _T("A save is already running; the frames were not started.");
        return false;
    }

    // Pending snapshots can reach this method after destination preflight failed
    // or after the operator selected a different base folder. Re-establish the
    // claim here so every advertised Stop & Save retry has a complete path.
    if (!PrepareRunFolder())
    {
        return false;
    }

    // UI thread only from here on: the prefix is copied out of the CString and
    // the claimed run folder is immutable for this capture, so the worker never
    // touches MFC state.
    const std::wstring folder = m_runFolderPath;
    const std::wstring prefix = ToWideString(_strFilename);

    // Start() takes the frames for the duration of the attempt only and hands
    // them straight back when the worker thread cannot be created, so on every
    // failure path below the caller still owns its capture.
    if (!m_saveWorker.Start(snapshot, folder, prefix))
    {
        const grablinkcore::SaveProgress progress = m_saveWorker.Progress();
        m_strLastError.Format(_T("The save could not be started:\n%s"),
                              progress.lastError.c_str());
        return false;
    }

    m_strLastError.Empty();
    return true;
}

bool CGrablinkSnapshotDoc::PostViewMessage(UINT message) const
{
    // The callback copies the handle under the lock and posts to it; it never
    // walks the MFC view list, which is not thread safe.
    HWND target = NULL;
    {
        CDocumentCaptureLock captureLock(m_captureLock);
        target = m_viewWindow;
    }

    if (target == NULL)
    {
        return false;
    }

    return ::PostMessage(target, message, 0, 0) != FALSE;
}

// Begins one application-modal scope on this document and returns the guard that
// holds it. Every modal call the UI thread makes goes through here, so the
// suppression covers the folder chooser and the About box alike, and the scope
// is released on every path out of the call.
grablinkcore::ModalScope CGrablinkSnapshotDoc::BeginModalScope()
{
    // The guard begins the scope in its constructor and ends it in its
    // destructor, and the counter is a member of the document: nothing here can
    // outlive the counter it points at. The UI thread is the only thread that
    // calls this, and the acquisition callback only ever reads the counter.
    return m_modalScopes.BeginScope();
}

// Shows one message box on the UI thread while holding one application-modal
// scope for exactly the span of the call.
//
// Every message box the document shows is a modal call: ::MessageBox runs a
// nested message loop of its own, so while the box is up that loop dispatches
// the WM_APP_PREVIEW_REFRESH messages the acquisition callback keeps posting,
// and a continuous post/Invalidate stream entering a modal loop is the defect
// the folder chooser and the About box were already fixed for. Holding one
// scope for the span of the call is what makes a box shown during an active
// capture behave like every other modal call in the application: the callback
// stops posting refreshes for exactly as long as the box is up, and the preview
// pool keeps being published throughout.
//
// The guard is a local of this function, so its destructor runs on every path
// out of the call - including paths a later edit adds - and no path can strand
// the scope. Nothing between the two statements can fail, return or throw, and
// the counter is a member of the document, so the guard can never outlive it.
//
// UI thread only, exactly as the call sites it replaces were: the scope is ended
// on the thread that began it and the depth it maintains describes this thread's
// modal calls, so showing a box from the driver's signal thread would corrupt
// that depth. The assertion keeps that a checked invariant rather than a comment
// - the document is created on the UI thread, and every caller is a command
// handler or a posted-message handler, which the framework runs there.
//
// The owner stays NULL, as it was at every call site this replaced. A
// desktop-owned box is not application-modal and is not kept above the
// application's own windows; passing the main frame's handle instead is the
// candidate fix for that, and it is a separate change with its own operator
// consequences, so it is deliberately not made here.
int CGrablinkSnapshotDoc::ShowModalMessageBox(LPCTSTR text, LPCTSTR caption, UINT flags)
{
    ASSERT(::GetCurrentThreadId() == m_uiThreadId);

    grablinkcore::ModalScope modalScope = BeginModalScope();
    return ::MessageBox(NULL, text, caption, flags);
}

void CGrablinkSnapshotDoc::RegisterViewWindow(HWND hWnd)
{
    CDocumentCaptureLock captureLock(m_captureLock);
    m_viewWindow = hWnd;
}

void CGrablinkSnapshotDoc::UnregisterViewWindow(HWND hWnd)
{
    CDocumentCaptureLock captureLock(m_captureLock);

    // Only the window that is actually registered may be cleared, so a view
    // that is being torn down cannot unregister a newer one.
    if (m_viewWindow == hWnd)
    {
        m_viewWindow = NULL;
    }
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc state accessors (race free for the UI thread)

std::size_t CGrablinkSnapshotDoc::CaptureFrameCount() const
{
    CDocumentCaptureLock captureLock(m_captureLock);
    return m_frameBuffer.IsInitialized() ? m_frameBuffer.Count() : 0;
}

std::size_t CGrablinkSnapshotDoc::CaptureFrameCapacity() const
{
    CDocumentCaptureLock captureLock(m_captureLock);
    if (m_frameBuffer.IsInitialized())
    {
        return m_frameBuffer.Capacity();
    }
    return static_cast<std::size_t>(_numImages);
}

bool CGrablinkSnapshotDoc::IsCapturing() const
{
    CDocumentCaptureLock captureLock(m_captureLock);
    return _bCapturing != false;
}

grablinkcore::SaveProgress CGrablinkSnapshotDoc::GetSaveProgress() const
{
    // The worker carries its own synchronisation, so no capture lock is needed.
    return m_saveWorker.Progress();
}

std::size_t CGrablinkSnapshotDoc::PendingSaveFrameCount() const
{
    // UI thread only, like every other reader of m_pendingSnapshot.
    return m_pendingSnapshot.FrameCount();
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc live preview accessors
//
// The view is the only caller of all five, and each of them is the publisher's
// own thread rule: TakeForDisplay()/ReleaseDisplay() on the UI thread, which is
// where OnDraw runs. The pool is private and stays private, so nothing else in
// the application can reach a slot, hold a lease or publish out of turn.

const unsigned char* CGrablinkSnapshotDoc::TakePreviewForDisplay()
{
    // Returns the newest published frame and pins its slot against any later
    // publish, or NULL when there is nothing published yet (before the first
    // frame, or while the pool is unconfigured after a reset). A NULL is not an
    // error: the view simply has nothing new to draw.
    return m_preview.TakeForDisplay();
}

void CGrablinkSnapshotDoc::ReleasePreviewDisplay()
{
    // Gives every slot the display side holds back to the pool. Safe to call
    // when nothing was taken, so the view's RAII guard can call it
    // unconditionally on every exit path.
    m_preview.ReleaseDisplay();
}

void CGrablinkSnapshotDoc::AcknowledgePreviewRefresh()
{
    // Dispatch has begun, so the queued refresh no longer needs to reserve the
    // one outstanding-message slot. A frame published after this point may
    // queue the next redraw while this one is being invalidated/painted.
    m_previewRefresh.Acknowledge();
}

int CGrablinkSnapshotDoc::PreviewWidth() const
{
    return m_preview.Width();
}

int CGrablinkSnapshotDoc::PreviewHeight() const
{
    return m_preview.Height();
}

std::size_t CGrablinkSnapshotDoc::PreviewPitch() const
{
    // The DWORD-aligned stride of a pool row. The view uses it only to check
    // that the DIB it hands to GDI really matches the storage; the image width
    // is PreviewWidth(), never this.
    return m_preview.Pitch();
}

/////////////////////////////////////////////////////////////////////////////
// Capture capacity: validation and the memory cost of one capture
//
// The operator needs a capture that runs long enough to be stopped
// deliberately: at 351 fps a 200 frame capacity is full in about 0.57 s, so
// "Stop" and "Stop & Save" cannot be used as part of a decision. The capacity
// is therefore a setting, and it is shown together with the memory it commits,
// because the whole block is reserved and committed before acquisition starts
// (see AGENTS.md) - that commit is what the setting really spends.
//
// Everything below is file-local on purpose: the dialog is a helper of
// CGrablinkSnapshotDoc::OnCaptureSettings() and earns neither a translation
// unit of its own nor a project entry.

namespace {

// Multiplies in 64 bits and reports whether the product fits. Checked at every
// step: a count the operator types must never wrap into a small positive number
// that looks affordable while the allocation it stands for cannot be described
// at all.
bool MultiplyChecked(unsigned long long left, unsigned long long right,
                     unsigned long long& product)
{
    if (left != 0 && right > (std::numeric_limits<unsigned long long>::max)() / left)
    {
        return false;
    }

    product = left * right;
    return true;
}

// The largest capacity the document can hold: it keeps the count in an int
// (_numImages), so anything above int is not a count it could store.
unsigned long long MaxFrameCount()
{
    return static_cast<unsigned long long>((std::numeric_limits<int>::max)());
}

// Outcome of validating one candidate capacity.
struct CaptureCapacityCheck
{
    bool ok;                  // the count may be used
    bool bytesKnown;          // the geometry was usable, so "bytes" means something
    unsigned long long bytes; // width * height * frames
    CString reason;           // operator-facing explanation, empty when ok
};

// Validates a candidate capture capacity against the driver's image geometry.
//
// A count that is not a positive whole number of frames - empty, zero, or larger
// than an int - is refused, and so is a count whose width * height * frames does
// not fit in 64 bits. Nothing is rounded and nothing is clamped: the operator is
// told which number is acceptable and asked again.
//
// When the geometry is not known (width or height is not positive, which is what
// the document sees when no channel could be created) the count is accepted and
// the size is reported as unknown: the capacity has to be settable before a
// camera is connected, and the allocation check that really matters is the one
// FrameBuffer::Init() performs when "Go!" runs.
CaptureCapacityCheck CheckCaptureCapacity(int width, int height,
                                          unsigned long long frames)
{
    CaptureCapacityCheck result;
    result.ok = false;
    result.bytesKnown = false;
    result.bytes = 0;

    if (frames == 0 || frames > MaxFrameCount())
    {
        result.reason.Format(
            _T("The frame count must be a whole number between 1 and %llu."),
            MaxFrameCount());
        return result;
    }

    result.ok = true;

    if (width <= 0 || height <= 0)
    {
        return result;
    }

    unsigned long long pixels = 0;
    unsigned long long bytes = 0;
    if (!MultiplyChecked(static_cast<unsigned long long>(width),
                         static_cast<unsigned long long>(height), pixels) ||
        !MultiplyChecked(pixels, frames, bytes))
    {
        result.ok = false;
        result.reason.Format(
            _T("%d x %d x %llu frames does not fit in 64 bits, so the committed size of")
            _T(" that capture cannot even be computed."),
            width, height, frames);
        return result;
    }

    result.bytesKnown = true;
    result.bytes = bytes;
    return result;
}

// Human-readable size of the block: MiB below one GiB and GiB from one GiB up,
// with the exact byte count appended so the figure can be compared with what the
// process reports as committed.
CString FormatCommittedBytes(unsigned long long bytes)
{
    const unsigned long long kMiB = 1024ULL * 1024ULL;
    const unsigned long long kGiB = 1024ULL * kMiB;

    CString text;
    if (bytes >= kGiB)
    {
        text.Format(_T("%.2f GiB (%llu bytes)"),
                    static_cast<double>(bytes) / static_cast<double>(kGiB), bytes);
    }
    else
    {
        text.Format(_T("%.1f MiB (%llu bytes)"),
                    static_cast<double>(bytes) / static_cast<double>(kMiB), bytes);
    }

    return text;
}

// Strict decimal parse of a frame count: blanks around the number are tolerated,
// nothing else is. A sign, a decimal point, an exponent or any other character is
// refused rather than converted, so "1e6" or "-200" cannot be interpreted for the
// operator as something they did not type. The accumulation is bounded by the
// acceptable range, so it cannot wrap either.
bool ParseFrameCount(const CString& text, unsigned long long& frames)
{
    int begin = 0;
    int end = text.GetLength();

    while (begin < end && (text[begin] == _T(' ') || text[begin] == _T('\t')))
    {
        ++begin;
    }
    while (end > begin && (text[end - 1] == _T(' ') || text[end - 1] == _T('\t')))
    {
        --end;
    }

    if (begin >= end)
    {
        return false;
    }

    unsigned long long value = 0;
    for (int index = begin; index < end; ++index)
    {
        const TCHAR character = text[index];
        if (character < _T('0') || character > _T('9'))
        {
            return false;
        }

        value = value * 10ULL + static_cast<unsigned long long>(character - _T('0'));
        if (value > MaxFrameCount())
        {
            return false;
        }
    }

    frames = value;
    return true;
}

// The one place the label text is worded, shared by the initial state and by the
// live update, so the two cannot drift apart.
CString DescribeCaptureCapacity(int width, int height, unsigned long long frames)
{
    const CaptureCapacityCheck check = CheckCaptureCapacity(width, height, frames);

    if (!check.ok)
    {
        return check.reason;
    }

    if (!check.bytesKnown)
    {
        return CString(_T("Required committed memory: not known yet - the driver geometry")
                       _T(" is unavailable (no channel). It is checked when \"Go!\" runs."));
    }

    CString text;
    text.Format(_T("Required committed memory: %s"),
                (LPCTSTR)FormatCommittedBytes(check.bytes));
    return text;
}

// The Capture Settings... dialog.
//
// It is a helper of OnCaptureSettings(), which owns the setting: the dialog reads
// nothing from the document and writes nothing back except the count it was asked
// to collect, and it validates that count with the same checked arithmetic the
// document uses. The memory cost is recomputed on every EN_CHANGE, so what the
// operator is about to accept is on screen the whole time they are typing rather
// than only when they press OK.
class CCaptureSettingsDialog : public CDialog
{
public:
    // "width" and "height" are the driver's image geometry, copied on the UI
    // thread before the modal call; "frames" is the capacity in force now.
    CCaptureSettingsDialog(int width, int height, int frames, CWnd* pParent)
        : CDialog(IDD_CAPTURE_SETTINGS_DIALOG, pParent),
          m_width(width),
          m_height(height),
          m_frames(frames)
    {
    }

    // The validated count. Only meaningful when DoModal() returned IDOK: a
    // refused OK leaves the dialog open and this value untouched.
    int FrameCount() const { return m_frames; }

protected:
    // ClassWizard generated virtual function overrides
    //{{AFX_VIRTUAL(CCaptureSettingsDialog)
    virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    //}}AFX_VIRTUAL

    //{{AFX_MSG(CCaptureSettingsDialog)
    afx_msg void OnFramesChanged();
    //}}AFX_MSG
    DECLARE_MESSAGE_MAP()

private:
    // The edit's text, or an empty string when the control is not there yet.
    CString ReadFrameText();

    // Recomputes and shows the committed size for whatever is in the edit. It
    // refuses nothing itself: it is the live cost display, and the refusal
    // happens in OnOK().
    void ShowRequiredMemory();

    // Says why the current input was refused and puts the caret back in the
    // edit, leaving the dialog open with the capacity unchanged.
    void RefuseInput(const CString& reason);

    int m_width;
    int m_height;
    int m_frames;
    CEdit m_framesEdit;
    CStatic m_memoryLabel;
};

void CCaptureSettingsDialog::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    //{{AFX_DATA_MAP(CCaptureSettingsDialog)
    DDX_Control(pDX, IDC_CAPTURE_FRAMES_EDIT, m_framesEdit);
    DDX_Control(pDX, IDC_CAPTURE_MEMORY_LABEL, m_memoryLabel);
    //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CCaptureSettingsDialog, CDialog)
    //{{AFX_MSG_MAP(CCaptureSettingsDialog)
    ON_EN_CHANGE(IDC_CAPTURE_FRAMES_EDIT, OnFramesChanged)
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

BOOL CCaptureSettingsDialog::OnInitDialog()
{
    CDialog::OnInitDialog();

    // The edit starts on the capacity in force, so OK without typing anything
    // leaves the setting as it was instead of resetting it to the default.
    CString text;
    text.Format(_T("%d"), m_frames);
    m_framesEdit.SetWindowText(text);

    // SetWindowText() above already raised EN_CHANGE, but the label is filled
    // here as well so its first content never depends on that notification
    // having arrived in this order.
    ShowRequiredMemory();

    m_framesEdit.SetSel(0, -1);
    m_framesEdit.SetFocus();
    return TRUE;
}

void CCaptureSettingsDialog::OnFramesChanged()
{
    // The edit was changed, so the number - and with it the committed size - is
    // recomputed while the operator types.
    ShowRequiredMemory();
}

void CCaptureSettingsDialog::OnOK()
{
    unsigned long long frames = 0;
    if (!ParseFrameCount(ReadFrameText(), frames))
    {
        // Zero stands for "not a frame count at all", which CheckCaptureCapacity()
        // reports the acceptable range for: the same message then covers an empty
        // box, a zero, a negative number and anything that is not a whole number.
        frames = 0ULL;
    }

    const CaptureCapacityCheck check = CheckCaptureCapacity(m_width, m_height, frames);
    if (!check.ok)
    {
        RefuseInput(check.reason);
        return;
    }

    m_frames = static_cast<int>(frames);
    CDialog::OnOK();
}

CString CCaptureSettingsDialog::ReadFrameText()
{
    CString text;
    if (m_framesEdit.GetSafeHwnd() != NULL)
    {
        m_framesEdit.GetWindowText(text);
    }
    return text;
}

void CCaptureSettingsDialog::ShowRequiredMemory()
{
    // The guard is why an EN_CHANGE that arrives while the dialog is still being
    // initialized cannot touch a window that is not there yet.
    if (m_memoryLabel.GetSafeHwnd() == NULL)
    {
        return;
    }

    unsigned long long frames = 0;
    if (!ParseFrameCount(ReadFrameText(), frames))
    {
        frames = 0ULL;
    }

    m_memoryLabel.SetWindowText(DescribeCaptureCapacity(m_width, m_height, frames));
}

void CCaptureSettingsDialog::RefuseInput(const CString& reason)
{
    // The dialog stays open: a refused number is not a capacity, and the operator
    // can see both the number and its cost while correcting it.
    AfxMessageBox(reason, MB_OK | MB_ICONINFORMATION);
    m_framesEdit.SetFocus();
    m_framesEdit.SetSel(0, -1);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc commands

void CGrablinkSnapshotDoc::OnGo()
{
    // Starting an already-active capture would clear the frame count and begin
    // overwriting the same allocation from slot zero. Reject the command instead
    // so an accidental second click cannot silently truncate the run.
    if (IsCapturing())
    {
        ShowModalMessageBox(
                            _T("A capture is already running."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // A running save owns a snapshot of the previous capture and the worker
    // cannot be started again while it runs.
    if (m_saveWorker.Progress().running)
    {
        ShowModalMessageBox(
                            _T("A save is still running - wait for it to finish before starting a new capture.")
                            _T("\n\nThe status bar shows its progress."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // A capture that could not be handed to the worker earlier is a completed
    // capture whose frames are still owned by m_pendingSnapshot. Starting a new
    // run now would allocate a second full-size capture buffer while the first
    // one is unsaved, so the user is asked to decide which comes first. Nothing
    // is discarded here: the frames wait for "Stop & Save".
    if (m_pendingSnapshot.IsValid())
    {
        ShowModalMessageBox(
                            _T("An earlier capture is still waiting to be saved - its frames are not lost.")
                            _T("\n\nUse \"Stop & Save\" to retry writing it, or Ctrl+N (File > New) to")
                            _T(" discard it, and start the capture again."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Join a job that has finished but was never joined: that releases the
    // snapshot it was writing so the new capture does not compete with it for
    // memory. ReapFinished() never waits for a job that is still running, so
    // the old check-then-Wait() race - a save started in between the check and
    // the wait could block the UI thread through Wait() - cannot happen.
    m_saveWorker.ReapFinished();

    // The previous run's directory remains immutable once claimed. This capture
    // will claim a fresh one only when it is ready to save.
    m_runFolderPath.clear();

    bool armed = false;
    {
        CDocumentCaptureLock captureLock(m_captureLock);

        // This run starts a fresh capture: a completion that was posted for the
        // previous buffer must not start a save for this one.
        m_captureCompletePending = FALSE;

        // The configured capacity is the block this run needs. An initialized
        // buffer of a different size must not be reused: the run would stop at
        // the old capacity instead of the one the operator chose, so the block is
        // replaced. FrameBuffer::Init() releases the old allocation and reserves
        // (and commits) the new one without writing a byte, exactly as the first
        // allocation did, so the "never zero the capture block" invariant is
        // untouched either way.
        if (m_frameBuffer.IsInitialized() &&
            m_frameBuffer.Capacity() == static_cast<std::size_t>(_numImages))
        {
            // Reuse the allocation already in place: only the frame count is
            // dropped. The buffer is deliberately not zeroed - every byte is
            // overwritten as frames arrive, so clearing 40 GiB would be pure
            // wasted work.
            m_frameBuffer.Clear();
            armed = true;
        }
        else
        {
            // The previous capture transferred the allocation to a snapshot, or
            // the configured capacity no longer matches the block in hand, so a
            // fresh one is needed before capture can start again. The lock is
            // held, as it is around every other Init() of this buffer, so no
            // callback can Append into storage that is being replaced.
            armed = m_frameBuffer.Init(m_SizeX, m_SizeY,
                                       static_cast<std::size_t>(_numImages));
            if (!armed)
            {
                m_strLastError.Format(
                    _T("Cannot allocate the capture buffer for %d x %d x %d frames:\n%s"),
                    m_SizeX, m_SizeY, _numImages, m_frameBuffer.LastError().c_str());
            }
        }

        if (armed)
        {
            // State is reset before capture is armed, and arming is the very
            // last thing done here.
            _bCapturing = true;
        }
    }

    if (!armed)
    {
        ShowModalMessageBox(m_strLastError, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
    }

    // The channel stays ACTIVE: live preview and the frame rate keep working
    // whether or not frames are being stored.
    PostViewMessage(WM_APP_PREVIEW_REFRESH);
}

void CGrablinkSnapshotDoc::OnStop()
{
    {
        CDocumentCaptureLock captureLock(m_captureLock);

        // Stop capturing and discard the current run: the frame count is
        // dropped, nothing is written, and the allocation stays in place so the
        // buffer is immediately reusable.
        _bCapturing = false;
        m_captureCompletePending = FALSE;
        if (m_frameBuffer.IsInitialized())
        {
            m_frameBuffer.Clear();
        }
    }

    // A capture whose save could not be started is a *completed* capture, not
    // part of the run being discarded here, so m_pendingSnapshot and its run
    // folder are left alone: "Stop & Save" can still write it out and Ctrl+N
    // (ResetCaptureState) discards both. A current capture has not claimed its
    // folder yet; if a failed transfer did claim one, release that path now.
    if (!m_pendingSnapshot.IsValid())
    {
        m_runFolderPath.clear();
    }

    // Update UI via posted message
    PostViewMessage(WM_APP_PREVIEW_REFRESH);
}

void CGrablinkSnapshotDoc::OnStopSave()
{
    std::size_t captured = 0;
    {
        CDocumentCaptureLock captureLock(m_captureLock);

        // Stop capture atomically so no further frame can arrive while the
        // buffer is being handed over.
        _bCapturing = false;
        m_captureCompletePending = FALSE;
        if (m_frameBuffer.IsInitialized())
        {
            captured = m_frameBuffer.Count();
        }
    }

    if (m_saveWorker.Progress().running)
    {
        ShowModalMessageBox(_T("A save is already running - wait for it to finish."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    // A capture that could not be handed to the worker earlier is retried
    // first, and its frames are never dropped: they stay in m_pendingSnapshot
    // until a save really starts.
    if (m_pendingSnapshot.IsValid())
    {
        if (!TryStartSave(m_pendingSnapshot))
        {
            ShowModalMessageBox(m_strLastError, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        }

        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    if (captured == 0)
    {
        ShowModalMessageBox(_T("No frames have been captured yet - there is nothing to save."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    // Validate the destination before the buffer is consumed: if the folder
    // cannot be created the frames are still in the buffer and the user can fix
    // the path and try again.
    if (!PrepareRunFolder())
    {
        ShowModalMessageBox(m_strLastError, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    grablinkcore::FrameSnapshot snapshot;
    bool transferred = false;
    {
        CDocumentCaptureLock captureLock(m_captureLock);
        if (m_frameBuffer.IsInitialized() && m_frameBuffer.Count() > 0)
        {
            // Zero copy transfer of exactly the frames that were captured.
            transferred = grablinkcore::FrameSnapshot::TakeFrom(
                m_frameBuffer, m_frameBuffer.Count(), snapshot);
        }
    }

    if (!transferred)
    {
        ShowModalMessageBox(_T("The captured frames could not be handed to the save worker."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
    }
    else if (!TryStartSave(snapshot))
    {
        // The refused start left the frames with us, so the capture is kept for
        // a retry instead of being lost.
        m_pendingSnapshot = std::move(snapshot);

        CString message;
        message.Format(_T("%s\n\nThe %llu captured frames are kept - fix the destination and use")
                       _T(" \"Stop & Save\" again."),
                       (LPCTSTR)m_strLastError,
                       (unsigned long long)m_pendingSnapshot.FrameCount());
        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
    }

    // Returns promptly: the worker writes the files in the background.
    PostViewMessage(WM_APP_PREVIEW_REFRESH);
}

namespace {

// The shell sends BFFM_INITIALIZED once the browse dialog exists, and that is
// the only moment at which its initially selected folder can be set. The path
// arrives through lpData, which points at a buffer the caller keeps alive for
// the whole modal call, so it is still valid inside this callback. The ANSI
// message is deliberate: this legacy build is MBCS (no UNICODE/_UNICODE is
// defined for the application project), so the TCHAR path is exactly the ANSI
// string BFFM_SETSELECTIONA expects.
//
// The selection is sent synchronously, which is what the notification means:
// BFFM_INITIALIZED is delivered while the shell is initializing the dialog and
// is waiting for this callback to return, so the dialog window already exists
// and can be reached directly. This is the pre-experiment form; the queued
// (PostMessageA) variant was tried as an A/B experiment and reverted, because
// it failed the same way the synchronous form does.
int CALLBACK BrowseFolderInitCallback(HWND hwnd, UINT message, LPARAM /*lParam*/, LPARAM lpData)
{
    if (message == BFFM_INITIALIZED && lpData != 0)
    {
        // wParam TRUE means lpData is a file-system path instead of a PIDL.
        ::SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, lpData);
    }
    return 0;
}

} // namespace

void CGrablinkSnapshotDoc::OnSetFolder()
{
    // The output folder is read by the UI-thread save paths, so changing it
    // while a capture is filling its buffer or a save is reading it would race.
    // Both are refused instead.
    if (IsCapturing())
    {
        ShowModalMessageBox(_T("Stop the capture before changing the output folder."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (m_saveWorker.Progress().running)
    {
        ShowModalMessageBox(_T("A save is still running - wait for it to finish before changing")
                            _T(" the output folder."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // The shell can only be asked to preselect a folder that is really there.
    // _outputFolder may be relative (the "SavedImages" default), so it is
    // resolved first: a relative path handed to the shell would be resolved
    // against the shell's own notion of the current directory instead of ours.
    // The buffer lives on this frame, so it stays valid for the whole modal
    // call, which is what the BFFM_INITIALIZED callback requires.
    CString initialFolder;
    if (!_outputFolder.IsEmpty())
    {
        const DWORD required = ::GetFullPathName(_outputFolder, 0, NULL, NULL);
        if (required > 0)
        {
            LPTSTR buffer = initialFolder.GetBuffer(static_cast<int>(required) + 1);
            const DWORD length =
                ::GetFullPathName(_outputFolder, required + 1, buffer, NULL);
            initialFolder.ReleaseBuffer(length > 0 ? static_cast<int>(length) : 0);
        }

        const DWORD attributes = initialFolder.IsEmpty()
            ? INVALID_FILE_ATTRIBUTES
            : ::GetFileAttributes(initialFolder);

        // Not an existing directory: let the shell start wherever it normally
        // would rather than pointing the operator at something invisible.
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            initialFolder.Empty();
        }
    }

    // Use folder browse dialog
    BROWSEINFO bi = {0};
    bi.hwndOwner = AfxGetMainWnd()->GetSafeHwnd();
    bi.lpszTitle = "Select output folder";
    if (!initialFolder.IsEmpty())
    {
        bi.lpfn = BrowseFolderInitCallback;
        bi.lParam = reinterpret_cast<LPARAM>(initialFolder.GetString());
    }

    // The modal call runs a nested message loop on this very thread. Everything
    // the acquisition callback posts during that time - a preview refresh in
    // particular - is dispatched inside that loop, so one application-modal
    // scope is held for exactly the span of the call and the callback stops
    // posting refresh messages for it. The scope begins immediately before the
    // call and is ended immediately after it returns, by the guard's
    // destructor, so the window in which a preview post can still land inside
    // the modal loop is the call itself and nothing else. The guard is scoped to
    // this block for that reason: the pidl handling below is ordinary UI-thread
    // work and must not extend the suppression.
    //
    // The driver's signal thread keeps publishing frames while the scope is
    // held, so the pool holds the newest pixels throughout; only the redraw
    // request is held back. Nothing is posted when the call returns - the
    // coalescer stays the only source of refresh posts, and the next live frame
    // after this returns claims the next one.
    LPITEMIDLIST pidl = NULL;
    {
        grablinkcore::ModalScope modalScope = BeginModalScope();
        pidl = SHBrowseForFolder(&bi);
    }

    if (pidl) {
        TCHAR path[MAX_PATH];
        if (SHGetPathFromIDList(pidl, path)) {
            _outputFolder = path;
            // A pending capture may be retried into the newly selected base;
            // its previous empty run directory is never reused or overwritten.
            m_runFolderPath.clear();
        }
        CoTaskMemFree(pidl);
    }
}

void CGrablinkSnapshotDoc::OnCaptureSettings()
{
    // The capacity decides how much the capture buffer commits, and that buffer
    // is allocated, reused and emptied on the capture path and handed to the save
    // path. Changing it under a run that is using the current one would be the
    // same race OnSetFolder() refuses for the output folder, so the same three
    // states are refused here, each with the reason in the message.
    //
    // They are runtime refusals rather than a greyed-out menu item on purpose:
    // the command is a bare top-level menu item, and MFC runs
    // ON_UPDATE_COMMAND_UI handlers only for items inside a popup, so an update
    // handler here would never be called and the item could not be disabled.
    if (IsCapturing())
    {
        ShowModalMessageBox(
                            _T("Stop the capture before changing the capture settings.")
                            _T("\n\nThe frames being stored belong to the capacity in force now."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (m_saveWorker.Progress().running)
    {
        ShowModalMessageBox(
                            _T("A save is still running - wait for it to finish before changing")
                            _T(" the capture settings."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // A pending snapshot is a completed capture whose frames were taken out of
    // the buffer this setting describes. "Stop & Save" must stay able to retry it
    // exactly as it was captured, so the setting is left alone while it waits;
    // Ctrl+N (File > New) discards it if it is not wanted.
    if (m_pendingSnapshot.IsValid())
    {
        ShowModalMessageBox(
                            _T("An earlier capture is still waiting to be saved - its frames are not lost.")
                            _T("\n\nUse \"Stop & Save\" to retry writing it, or Ctrl+N (File > New) to")
                            _T(" discard it, and change the capture settings afterwards."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // The image geometry is read on this thread only: the driver filled it in
    // when the channel was created (OnNewDocument), so it is the size the next
    // capture really allocates. A non-positive dimension means no geometry is
    // known, which the dialog reports as "not known yet" rather than as zero.
    CCaptureSettingsDialog dialog(m_SizeX, m_SizeY, _numImages, AfxGetMainWnd());

    // Capture Settings... is a modal call like the About box: one application
    // modal scope is held for exactly the span of DoModal(), so the acquisition
    // callback stops posting preview refreshes while its nested message loop
    // runs. The guard is a local, so the scope is released on every exit from
    // this call - including an early return somebody adds later.
    INT_PTR result = IDCANCEL;
    {
        grablinkcore::ModalScope modalScope = BeginModalScope();
        result = dialog.DoModal();
    }

    if (result != IDOK)
    {
        return;
    }

    // Re-checked here as well as in the dialog. The capacity is the document's
    // to own, and neither the committed block nor the stored profile value may
    // depend on a helper having validated the number.
    const int requested = dialog.FrameCount();
    const CaptureCapacityCheck check = CheckCaptureCapacity(
        m_SizeX, m_SizeY,
        requested > 0 ? static_cast<unsigned long long>(requested) : 0ULL);
    if (!check.ok)
    {
        ShowModalMessageBox(check.reason, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        return;
    }

    const bool changed = (requested != _numImages);

    // In force from here on: OnGo() compares the block it has against this value
    // and replaces the allocation when the two differ.
    _numImages = requested;

    // Persisted through the profile API so the choice survives a restart. A failed
    // write is reported instead of being silently forgotten - the capacity is
    // applied either way, but the operator has just been told it would be
    // remembered.
    if (!AfxGetApp()->WriteProfileInt(kCaptureSettingsSection, kCaptureFramesKey, requested))
    {
        CString message;
        message.Format(_T("The capture capacity is now %d frames, but it could not be")
                       _T(" written to the application profile: it will not be remembered")
                       _T(" after a restart."),
                       requested);
        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONWARNING);
        return;
    }

    if (changed)
    {
        // The memory cost is repeated here because it is the operator's only
        // warning of what the next "Go!" will commit: FrameBuffer::Init() reserves
        // and commits the whole range before acquisition starts.
        const CString cost = check.bytesKnown
            ? FormatCommittedBytes(check.bytes)
            : CString(_T("not known yet (no driver geometry)"));

        CString message;
        message.Format(_T("The capture capacity is now %d frames.")
                       _T("\n\nCommitted memory for one full capture: %s.")
                       _T("\n\nThe next \"Go!\" reserves that block before acquisition starts."),
                       requested, (LPCTSTR)cost);
        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
    }
}

void CGrablinkSnapshotDoc::OnViewFitWindow()
{
    // The command is a toggle, so it flips the flag rather than setting it: a
    // check mark means "scale the live preview to the viewport" and no check
    // mark means the 1:1 pixel mapping the view otherwise uses.
    //
    // MFC dispatches a command to the document on the UI thread, which is the
    // only thread that reads or writes this flag (the constructor sets it and
    // the view's OnDraw reads it), so no lock is involved and the acquisition
    // callback - which never touches it - is unaffected.
    _bResizeImage = !_bResizeImage;

    // Repaint through the existing posted-message path instead of invalidating
    // the view directly: that is the one route into the view this document owns
    // and it needs no knowledge of the view's MFC objects. The view repaints
    // from the new mode when it handles the message; if it is not registered
    // (no window yet) there is nothing on screen to redraw.
    PostViewMessage(WM_APP_PREVIEW_REFRESH);
}

void CGrablinkSnapshotDoc::OnUpdateViewFitWindow(CCmdUI* pCmdUI)
{
    // The update handler is what makes the check mark reliable: MFC calls it
    // while the View menu is being opened, and SetCheck() is the only way the
    // framework learns the current state. Without it the item would be drawn
    // unchecked no matter what the flag says.
    pCmdUI->SetCheck(_bResizeImage ? 1 : 0);
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc UI-thread message handlers

void CGrablinkSnapshotDoc::OnCaptureCompleteMessage()
{
    {
        CDocumentCaptureLock captureLock(m_captureLock);
        if (!m_captureCompletePending)
        {
            // Already handled, or the buffer was discarded and the capture
            // restarted after the message was posted.
            return;
        }
        m_captureCompletePending = FALSE;
    }

    if (m_saveWorker.Progress().running)
    {
        ShowModalMessageBox(
                            _T("A save is still running, so the captured frames were not taken yet.")
                            _T("\n\nThey are kept in the capture buffer; use \"Stop & Save\" once it finishes."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONINFORMATION);
        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    // An earlier capture whose save could not be started is dealt with first,
    // so this one can never overwrite it. Whether the retry succeeds or fails,
    // this notification is now fully handled: a successful retry has started the
    // worker and must not fall through into a second Start attempt.
    if (m_pendingSnapshot.IsValid())
    {
        if (!TryStartSave(m_pendingSnapshot))
        {
            CString message;
            message.Format(_T("A previous capture is still waiting to be saved and the retry failed:\n%s")
                           _T("\n\nThe frames of this capture are kept in the capture buffer;")
                           _T(" use \"Stop & Save\" to try again."),
                           (LPCTSTR)m_strLastError);
            ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        }

        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    // Preflight, entirely on the UI thread and before the zero-copy hand over:
    // the destination is validated and created, and the paths are copied out of
    // the CStrings, so nothing below reads UI-thread-only state.
    if (!PrepareRunFolder())
    {
        // Protect this completed capture from a later Go! clearing the reusable
        // buffer. Taking ownership is zero-copy; Stop & Save can retry after the
        // destination is corrected.
        grablinkcore::FrameSnapshot pending;
        bool retained = false;
        {
            CDocumentCaptureLock captureLock(m_captureLock);
            if (m_frameBuffer.IsInitialized() && m_frameBuffer.Count() > 0)
            {
                retained = grablinkcore::FrameSnapshot::TakeFrom(
                    m_frameBuffer, m_frameBuffer.Count(), pending);
            }
        }

        CString message = m_strLastError;
        if (retained)
        {
            m_pendingSnapshot = std::move(pending);
            CString retainedMessage;
            retainedMessage.Format(
                _T("\n\nThe %llu captured frames are kept; fix the destination and use")
                _T(" \"Stop & Save\" to try again."),
                (unsigned long long)m_pendingSnapshot.FrameCount());
            message += retainedMessage;
        }
        else
        {
            message += _T("\n\nThe captured frames remain in the capture buffer; use")
                       _T(" \"Stop & Save\" before starting another capture.");
        }

        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    grablinkcore::FrameSnapshot snapshot;
    bool transferred = false;
    {
        CDocumentCaptureLock captureLock(m_captureLock);
        if (m_frameBuffer.IsInitialized() && m_frameBuffer.Count() > 0)
        {
            // Zero copy: the snapshot takes over the buffer's whole allocation,
            // which is why the capture buffer has to be Init()ed again before
            // the next capture.
            transferred = grablinkcore::FrameSnapshot::TakeFrom(
                m_frameBuffer, m_frameBuffer.Count(), snapshot);
        }
    }

    if (!transferred)
    {
        ShowModalMessageBox(
                            _T("The captured frames could not be handed to the save worker."),
                            _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
        PostViewMessage(WM_APP_PREVIEW_REFRESH);
        return;
    }

    if (!TryStartSave(snapshot))
    {
        // The refused start left the frames with us: keep them so the user can
        // retry instead of losing the capture.
        m_pendingSnapshot = std::move(snapshot);

        CString message;
        message.Format(_T("The capture could not be saved:\n%s")
                       _T("\n\nThe %llu captured frames are kept; use \"Stop & Save\" to try again."),
                       (LPCTSTR)m_strLastError,
                       (unsigned long long)m_pendingSnapshot.FrameCount());
        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
    }

    PostViewMessage(WM_APP_PREVIEW_REFRESH);
}

void CGrablinkSnapshotDoc::OnAcquisitionErrorMessage()
{
    bool captureError = false;
    bool acquisitionFailure = false;
    std::string captureReason;

    {
        CDocumentCaptureLock captureLock(m_captureLock);

        captureError = m_captureErrorPending != FALSE;
        acquisitionFailure = m_acquisitionFailurePending != FALSE;
        m_captureErrorPending = FALSE;
        m_acquisitionFailurePending = FALSE;

        if (captureError)
        {
            // The callback only flagged the failure; the buffer's message is
            // read here, on the UI thread, and copied before the lock is given
            // up.
            captureReason = m_frameBuffer.LastError();
        }
    }

    if (captureError)
    {
        CString message;
        message.Format(_T("Storing a captured frame failed and the capture has been stopped.")
                       _T("\n\n%s\n\nThe frames captured so far are kept and can still be saved."),
                       captureReason.c_str());
        ShowModalMessageBox(message, _T("GrablinkSnapshot"), MB_OK | MB_ICONERROR);
    }
    else if (acquisitionFailure)
    {
        ShowModalMessageBox(_T("Acquisition Failure !"), _T("GrablinkSnapshot"),
                            MB_OK | MB_ICONERROR);
    }
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc serialization

void CGrablinkSnapshotDoc::Serialize(CArchive& ar)
{
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc diagnostics

#ifdef _DEBUG
void CGrablinkSnapshotDoc::AssertValid() const
{
    CDocument::AssertValid();
}

void CGrablinkSnapshotDoc::Dump(CDumpContext& dc) const
{
    CDocument::Dump(dc);
}
#endif //_DEBUG
