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


// GrablinkSnapshotView.cpp : implementation of the CGrablinkSnapshotView class
//

#include "stdafx.h"
#include "GrablinkSnapshot.h"

#include "GrablinkSnapshotDoc.h"
#include "GrablinkSnapshotView.h"


#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotView

IMPLEMENT_DYNCREATE(CGrablinkSnapshotView, CView)

BEGIN_MESSAGE_MAP(CGrablinkSnapshotView, CView)
    //{{AFX_MSG_MAP(CGrablinkSnapshotView)
    // NOTE - the ClassWizard will add and remove mapping macros here.
    //    DO NOT EDIT what you see in these blocks of generated code!
    //}}AFX_MSG_MAP
    ON_WM_DESTROY()
    // Application messages posted by the acquisition callback, in the WM_APP
    // range (WM_USER belongs to the window classes themselves).
    ON_MESSAGE(WM_APP_PREVIEW_REFRESH, OnRefresh)
    ON_MESSAGE(WM_APP_CAPTURE_COMPLETE, OnCaptureComplete)
    ON_MESSAGE(WM_APP_ACQUISITION_ERROR, OnAcquisitionError)
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotView construction/destruction

CGrablinkSnapshotView::CGrablinkSnapshotView()
{
    m_pMainFrame = NULL;

    // Build bitmap info Y8
    m_pBitmapInfo = (BITMAPINFO *) new BYTE[sizeof(BITMAPINFO) + 255*sizeof(RGBQUAD)];
    m_pBitmapInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    m_pBitmapInfo->bmiHeader.biPlanes = 1;
    m_pBitmapInfo->bmiHeader.biBitCount = 8;
    m_pBitmapInfo->bmiHeader.biCompression = BI_RGB;
    m_pBitmapInfo->bmiHeader.biSizeImage = 0;
    m_pBitmapInfo->bmiHeader.biXPelsPerMeter = 0;
    m_pBitmapInfo->bmiHeader.biYPelsPerMeter = 0;
    m_pBitmapInfo->bmiHeader.biClrUsed = 0;
    m_pBitmapInfo->bmiHeader.biClrImportant = 0;
    for (int i = 0 ; i < 256 ; i++)
    {
        m_pBitmapInfo->bmiColors[i].rgbBlue = (BYTE)i;
        m_pBitmapInfo->bmiColors[i].rgbGreen = (BYTE)i;
        m_pBitmapInfo->bmiColors[i].rgbRed = (BYTE)i;
        m_pBitmapInfo->bmiColors[i].rgbReserved = 0;
    }
    m_pBitmapInfo->bmiHeader.biWidth = 0;
    m_pBitmapInfo->bmiHeader.biHeight = 0;


}

CGrablinkSnapshotView::~CGrablinkSnapshotView()
{
    // The bitmap info was allocated as a byte array, so it has to be released
    // with the matching element type: the old "delete" used BITMAPINFO*, which
    // is the wrong element size (and must not release new[] storage).
    if (m_pBitmapInfo) delete[] reinterpret_cast<BYTE*>(m_pBitmapInfo);
    m_pBitmapInfo = NULL;
}

BOOL CGrablinkSnapshotView::PreCreateWindow(CREATESTRUCT& cs)
{
    // TODO: Modify the Window class or styles here by modifying
    //  the CREATESTRUCT cs
    return CView::PreCreateWindow(cs);
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotView drawing

namespace
{
// Holds the frame the view is drawing for exactly as long as OnDraw needs it.
//
// TakeForDisplay() pins the slot the frame lives in, so a publish that arrives
// while GDI is reading those bytes cannot overwrite them, and the slot stays
// pinned until ReleaseDisplay(). That lease has to be given back on every way
// out of the draw, so it is owned by an object rather than tracked by hand: the
// guard below releases on the ordinary end, on an early return, and on an
// exception from anywhere in between, which is what keeps the three-slot pool
// from slowly starving as refreshes come and go.
class CPreviewLease
{
public:
    explicit CPreviewLease(CGrablinkSnapshotDoc* pDoc) : m_pDoc(pDoc)
    {
    }

    ~CPreviewLease()
    {
        // ReleaseDisplay() is safe when nothing was pinned, so no state has to
        // be remembered here: a guard created before a take that turned out to
        // have nothing to give still releases correctly, and one that is
        // created and never followed by a take costs nothing.
        if (m_pDoc != NULL)
        {
            m_pDoc->ReleasePreviewDisplay();
        }
    }

private:
    // One lease per scope; a copy would release the pool twice.
    CPreviewLease(const CPreviewLease&);
    CPreviewLease& operator=(const CPreviewLease&);

    CGrablinkSnapshotDoc* m_pDoc;
};

// Holds the DC's stretch mode, and the brush origin that goes with it, for
// exactly as long as the scaled draw needs it.
//
// Why HALFTONE: the fit-to-window path scales an 8-bpp grey frame with
// StretchDIBits, and a DC's default stretch mode is COLORONCOLOR, which
// resamples by dropping source rows and columns. When the client area is not
// an integer multiple of the frame, whole scan lines of a high-contrast grey
// image disappear, which is exactly the banding and dark-grey isoline look
// that was reported for this mode. HALFTONE is the one stretch mode that
// actually resamples: GDI builds a half-tone pattern from the source pixels,
// so a scaled 8-bpp grey frame keeps a continuous run of greys instead of
// stepping between the few source values nearest-neighbour dropping keeps.
// That is the hypothesis this guard exists to test; whether it removes the
// visible artifacts is an operator judgement against the previous build, not
// something this code can assert.
//
// What was observed, and why it points here: the artifact is a fine regular
// grid of grey cells roughly 10 px apart, it shows up when the window is
// resized close to the frame's native size (2040x1088, so near a 1:1 scale),
// and the cells are taller than they are wide. A periodic grid whose period is
// 1/|1 - scale| is the signature of dropped or duplicated source rows and
// columns, and cells that are taller than they are wide mean the vertical
// scale is closer to 1 than the horizontal one - which is what two independent
// scale factors produce, since the destination is the whole client rect and
// the aspect ratio is not preserved. That is aliasing, and HALFTONE is the mode
// that resamples it instead of dropping rows and columns.
//
// The case to compare against the previous build: fit mode on, resize the
// window to roughly the image size, and look for the grid.
//
// What it costs: HALFTONE is the slowest stretch mode. It computes a half-tone
// brush and blends every destination pixel rather than copying some of them,
// so a fit-to-window repaint costs measurably more than the same repaint under
// COLORONCOLOR - acceptable for a preview that refreshes at ~10 Hz, but it is
// a real cost, not a free improvement. The mode applies to stretch calls only,
// so the 1:1 SetDIBitsToDevice path is a straight copy and is deliberately
// left outside this guard.
//
// HALFTONE aligns its half-tone brush to the DC's brush origin, so the origin
// is moved to the destination origin for the duration of the call; left at
// whatever origin the DC happened to carry, the pattern can shift relative to
// the destination and tint the result. The previous origin is saved and put
// back with the mode.
class CStretchModeGuard
{
public:
    explicit CStretchModeGuard(HDC hdc)
        : m_hdc(hdc), m_previousMode(0), m_previousBrushOrigin(),
          m_brushOriginSaved(false)
    {
        // SetStretchBltMode() returns the previous mode, so the save and the
        // change are the same call. A return of 0 means it failed and there is
        // no mode to put back.
        m_previousMode = SetStretchBltMode(m_hdc, HALFTONE);

        POINT previousOrigin;
        previousOrigin.x = 0;
        previousOrigin.y = 0;
        if (GetBrushOrgEx(m_hdc, &previousOrigin) != 0)
        {
            m_previousBrushOrigin = previousOrigin;
            m_brushOriginSaved = true;
        }
        SetBrushOrgEx(m_hdc, 0, 0, NULL);
    }

    ~CStretchModeGuard()
    {
        // Put the DC back the way it was found, in the reverse order of the
        // constructor, so the caller's DC keeps whatever it had before.
        if (m_brushOriginSaved)
        {
            SetBrushOrgEx(m_hdc, m_previousBrushOrigin.x, m_previousBrushOrigin.y, NULL);
        }
        if (m_previousMode != 0)
        {
            SetStretchBltMode(m_hdc, m_previousMode);
        }
    }

private:
    // One guard per DC per scope; a copy would restore the mode twice.
    CStretchModeGuard(const CStretchModeGuard&);
    CStretchModeGuard& operator=(const CStretchModeGuard&);

    HDC m_hdc;
    int m_previousMode;
    POINT m_previousBrushOrigin;
    bool m_brushOriginSaved;
};
} // namespace

void CGrablinkSnapshotView::OnDraw(CDC* pDC)
{
    CGrablinkSnapshotDoc* pDoc = GetDocument();
    ASSERT_VALID(pDoc);

    // Protection
    if (pDoc == NULL)
    {
        return;
    }

    // Take the newest published frame and hold its slot for as long as this
    // function draws from it. The publisher hands out a pointer, not a lock: its
    // own mutex is held for the pointer swap only, so the acquisition side is
    // never blocked by a repaint.
    CPreviewLease previewLease(pDoc);
    const unsigned char* pPreviewBits = pDoc->TakePreviewForDisplay();

    if (pPreviewBits != NULL)
    {
        // The geometry of the frame that was just taken, taken from the
        // publisher that owns the storage. The width is the image width and
        // nothing else: the grabber's buffer pitch is a padded row stride, so
        // deriving the width from it (as the old code did) would draw the row
        // padding as extra columns and shift the picture.
        const int previewWidth = pDoc->PreviewWidth();
        const int previewHeight = pDoc->PreviewHeight();
        const std::size_t previewPitch = pDoc->PreviewPitch();

        if (previewWidth > 0 && previewHeight > 0 && previewPitch > 0)
        {
            // 8-bit, top-down (negative height) DIB over the publisher's own
            // bytes. Each pool row is DWORD aligned, which is exactly the stride
            // GDI computes for a BI_RGB 8-bpp DIB; if the two ever disagreed, GDI
            // would walk the wrong rows, so the assumption is asserted where it
            // can still be diagnosed instead of being drawn as a skewed image.
            m_pBitmapInfo->bmiHeader.biWidth = previewWidth;
            m_pBitmapInfo->bmiHeader.biHeight = -previewHeight;

            ASSERT(previewPitch ==
                   ((static_cast<std::size_t>(previewWidth) + 3) & ~static_cast<std::size_t>(3)));

            if (pDoc->_bResizeImage) {
                // Get client area and scale image to fit viewport
                CRect rect;
                GetClientRect(&rect);

                // Scaled draw only: the guard owns the DC's stretch mode for
                // exactly this call and puts the previous mode back on the way
                // out, so nothing else drawn through this DC is affected.
                CStretchModeGuard stretchMode(pDC->GetSafeHdc());

                // Display scaled to fit window
                StretchDIBits(pDC->GetSafeHdc(),
                    0, 0, rect.Width(), rect.Height(),  // destination
                    0, 0, previewWidth, previewHeight,  // source
                    pPreviewBits, m_pBitmapInfo, DIB_RGB_COLORS, SRCCOPY);
            } else {
                // Display at original size
                SetDIBitsToDevice(pDC->GetSafeHdc(), 0, 0, previewWidth, previewHeight,
                    0, 0, 0, previewHeight,
                    pPreviewBits, m_pBitmapInfo, DIB_RGB_COLORS);
            }
        }
    }

    // Nothing below reads pPreviewBits; the lease is released when this function
    // returns, on every path.

    // Capture/save state is read through the document's accessors, which take
    // the capture lock: the acquisition callback mutates those fields on the
    // driver's signal thread.
    const grablinkcore::SaveProgress saveProgress = pDoc->GetSaveProgress();

    CString progressStr;
    if (saveProgress.running)
    {
        progressStr.Format(" - Saving %llu/%llu (failed %llu)",
            (unsigned long long)saveProgress.completed,
            (unsigned long long)saveProgress.total,
            (unsigned long long)saveProgress.failed);
    }
    else if (saveProgress.total > 0 && saveProgress.failed > 0)
    {
        // Last finished job, kept visible so a partially failed save is not
        // silently forgotten.
        progressStr.Format(" - Saved %llu/%llu (failed %llu)",
            (unsigned long long)saveProgress.completed,
            (unsigned long long)saveProgress.total,
            (unsigned long long)saveProgress.failed);
    }
    else
    {
        const std::size_t captured = pDoc->CaptureFrameCount();
        if (captured > 0)
        {
            progressStr.Format(" - Captured %llu/%llu",
                (unsigned long long)captured,
                (unsigned long long)pDoc->CaptureFrameCapacity());
        }
    }

    // A capture whose save could not be started stays with the document until
    // "Stop & Save" retries it, so it is shown instead of being silently kept.
    CString pendingStr;
    const std::size_t pending = pDoc->PendingSaveFrameCount();
    if (pending > 0)
    {
        pendingStr.Format(" - %llu frames pending save", (unsigned long long)pending);
        progressStr += pendingStr;
    }

    // Throttle status bar updates to ~2 Hz.
    //
    // The interval is a duration, not a number of repaints. The previous
    // version divided the channel's frame rate by the target rate and wrote the
    // status bar only every that many OnDraw calls, so the visible update rate
    // was a function of how often the view happened to repaint (and thus of how
    // expensive a repaint was) rather than of elapsed time; at 2 Hz it never
    // reached the target it was derived from. A slower stretch mode makes each
    // repaint more expensive, which is exactly the case that showed up as a
    // staler status bar.
    //
    // GetTickCount64() is monotonic and 64-bit, so the comparison below is
    // unaffected by the ~49-day wrap of the 32-bit tick. The stamp is a
    // function-local static in the style of the counter it replaces, and that
    // is safe across a recreated window because it holds an absolute tick
    // rather than accumulated state: a fresh view can lose at most one update
    // to a stamp left by its predecessor, and there is no repaint count to
    // inherit that would still mean anything.
    const int targetHz = 2;
    const ULONGLONG updateIntervalMs = 1000 / targetHz;

    static ULONGLONG lastStatusUpdateTick = 0;
    const ULONGLONG nowTick = GetTickCount64();

    if (nowTick - lastStatusUpdateTick >= updateIntervalMs) {
        lastStatusUpdateTick = nowTick;

        // Channel state and frame rate are MultiCam driver reads, and they are
        // made here, inside the throttle, and nowhere else: the driver call is
        // the expensive part, and the status bar can only show ~2 Hz of it, so
        // running it on every repaint bought nothing but a driver round trip
        // per paint.
        //
        // Both outputs are initialized before their call and only trusted when
        // that call reports MC_OK. A failed MultiCam read leaves the output
        // untouched, so an uninitialized local (which is what the frame rate
        // used to be) would otherwise be formatted as a garbage number; instead
        // the failing field is shown as unavailable.
        //
        // A channel of 0 means the document has not created its MultiCam
        // channel yet, or has already deleted it (it is set to 0 on teardown),
        // and querying handle 0 is not a valid driver call, so it is skipped.
        m_strChannelStatus = "Frame Rate: unavailable, Channel State: unavailable";
        if (pDoc->m_Channel != 0)
        {
            INT32  channelState = MC_ChannelState_ORPHAN;
            FLOAT64 frameRate_Hz = 0.0;

            const MCSTATUS stateStatus =
                McGetParamInt(pDoc->m_Channel, MC_ChannelState, &channelState);
            const MCSTATUS rateStatus =
                McGetParamFloat(pDoc->m_Channel, MC_PerSecond_Fr, &frameRate_Hz);

            CString stateStr;
            if (stateStatus == MC_OK)
            {
                stateStr = (channelState == MC_ChannelState_ACTIVE) ? "ACTIVE" : "IDLE";
            }
            else
            {
                stateStr = "unavailable";
            }

            if (rateStatus == MC_OK)
            {
                m_strChannelStatus.Format("Frame Rate: %.2f, Channel State: %s",
                    frameRate_Hz, (LPCTSTR)stateStr);
            }
            else
            {
                m_strChannelStatus.Format("Frame Rate: unavailable, Channel State: %s",
                    (LPCTSTR)stateStr);
            }
        }

        m_strChannelStatus += progressStr;
        if(m_pMainFrame) m_pMainFrame->WriteStatusBar(m_strChannelStatus);
    }
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotView diagnostics

#ifdef _DEBUG
void CGrablinkSnapshotView::AssertValid() const
{
    CView::AssertValid();
}

void CGrablinkSnapshotView::Dump(CDumpContext& dc) const
{
    CView::Dump(dc);
}

CGrablinkSnapshotDoc* CGrablinkSnapshotView::GetDocument() // non-debug version is inline
{
    ASSERT(m_pDocument->IsKindOf(RUNTIME_CLASS(CGrablinkSnapshotDoc)));
    return (CGrablinkSnapshotDoc*)m_pDocument;
}
#endif //_DEBUG

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotView message handlers

void CGrablinkSnapshotView::OnInitialUpdate()
{
    CView::OnInitialUpdate();

    m_pMainFrame = (CMainFrame*)GetParent();

    // Register this window as the document's message target, from the UI
    // thread: the acquisition callback posts to it directly instead of walking
    // the MFC view list from the driver's signal thread.
    CGrablinkSnapshotDoc* pDoc = GetDocument();
    if (pDoc != NULL)
    {
        pDoc->RegisterViewWindow(GetSafeHwnd());
    }
}

void CGrablinkSnapshotView::OnDestroy()
{
    // Clear the registration while the window still exists: after this the
    // callback posts to nothing rather than to a dying handle.
    CGrablinkSnapshotDoc* pDoc = GetDocument();
    if (pDoc != NULL)
    {
        pDoc->UnregisterViewWindow(GetSafeHwnd());
    }

    CView::OnDestroy();
}

LRESULT CGrablinkSnapshotView::OnRefresh(WPARAM wParam, LPARAM lParam)
{
    CGrablinkSnapshotDoc* pDoc = GetDocument();
    if (pDoc != NULL)
    {
        pDoc->AcknowledgePreviewRefresh();
    }
    Invalidate(FALSE);  // FALSE = don't erase background (reduces flicker)
    return 0;
}

LRESULT CGrablinkSnapshotView::OnCaptureComplete(WPARAM wParam, LPARAM lParam)
{
    // The buffer filled up. The save is started here, on the UI thread, where
    // the folder can be validated and the CStrings read safely.
    CGrablinkSnapshotDoc* pDoc = GetDocument();
    if (pDoc != NULL)
    {
        pDoc->OnCaptureCompleteMessage();
    }
    return 0;
}

LRESULT CGrablinkSnapshotView::OnAcquisitionError(WPARAM wParam, LPARAM lParam)
{
    CGrablinkSnapshotDoc* pDoc = GetDocument();
    if (pDoc != NULL)
    {
        pDoc->OnAcquisitionErrorMessage();
    }
    return 0;
}
