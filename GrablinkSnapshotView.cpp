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

    m_ChannelState = MC_ChannelState_ORPHAN;

}

CGrablinkSnapshotView::~CGrablinkSnapshotView()
{
    // Delete bitmap info
    if (m_pBitmapInfo) delete m_pBitmapInfo;
}

BOOL CGrablinkSnapshotView::PreCreateWindow(CREATESTRUCT& cs)
{
    // TODO: Modify the Window class or styles here by modifying
    //  the CREATESTRUCT cs
    return CView::PreCreateWindow(cs);
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotView drawing

void CGrablinkSnapshotView::OnDraw(CDC* pDC)
{
    CGrablinkSnapshotDoc* pDoc = GetDocument();
    ASSERT_VALID(pDoc);

    // Protection
    if (pDoc->m_pCurrent==NULL) return;

    // Retrieve image size in the document
    int SizeX = pDoc->m_SizeX;
    int SizeY = pDoc->m_SizeY;
    int BufferPitch = pDoc->m_BufferPitch;

    // Configure the bitmap info according to the image size
    m_pBitmapInfo->bmiHeader.biWidth = BufferPitch / ( m_pBitmapInfo->bmiHeader.biBitCount / 8);  // Width = Pitch(bytes) divided by the number of bytes per pixel
    m_pBitmapInfo->bmiHeader.biHeight = -SizeY ;

    // Display
    SetDIBitsToDevice (pDC->GetSafeHdc(), 0, 0, SizeX, SizeY,
        0, 0, 0, SizeY,
        pDoc->m_pCurrent, m_pBitmapInfo, DIB_RGB_COLORS);

    // Advice the callback the screen refresh is terminated
    pDoc->m_bScreenRefreshCompleted = true;

    // Display channel info on status bar
    // Retrieve the channel state
    McGetParamInt (pDoc->m_Channel, MC_ChannelState, &m_ChannelState);

    // Retrieve the frame rate
    double frameRate_Hz;
    McGetParamFloat(pDoc->m_Channel, MC_PerSecond_Fr, &frameRate_Hz);
    // Display frame rate and channel state
    m_strChannelStatus.Format("Frame Rate: %.2f, Channel State: %s", frameRate_Hz,
        (m_ChannelState == MC_ChannelState_ACTIVE? "ACTIVE" : "IDLE"));

    // Write
    if(m_pMainFrame) m_pMainFrame->WriteStatusBar(m_strChannelStatus);
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
}
