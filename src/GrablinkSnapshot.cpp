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


// GrablinkSnapshot.cpp : Defines the class behaviors for the application.
//

#include "stdafx.h"
#include "GrablinkSnapshot.h"

#include "MainFrm.h"
#include "GrablinkSnapshotDoc.h"
#include "GrablinkSnapshotView.h"

#include "core/ModalScopeCounter.h" // application-modal scope for the About box

#include "multicam.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

namespace {

// True once InitInstance() has opened the MultiCam driver successfully. It is
// read by ExitInstance() so the application closes the driver exactly as many
// times as it opened it: the installed SDK documentation for McCloseDriver
// states that as many McCloseDriver() calls as successful McOpenDriver() calls
// are required, so a close after a failed open would unbalance the driver and
// touch a connection this process never established.
bool g_multiCamDriverOpen = false;

} // namespace

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotApp

BEGIN_MESSAGE_MAP(CGrablinkSnapshotApp, CWinApp)
    //{{AFX_MSG_MAP(CGrablinkSnapshotApp)
    ON_COMMAND(ID_APP_ABOUT, OnAppAbout)
    // NOTE - the ClassWizard will add and remove mapping macros here.
    //    DO NOT EDIT what you see in these blocks of generated code!
    //}}AFX_MSG_MAP
    // Standard file based document commands
    ON_COMMAND(ID_FILE_NEW, CWinApp::OnFileNew)
    ON_COMMAND(ID_FILE_OPEN, CWinApp::OnFileOpen)
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotApp construction

CGrablinkSnapshotApp::CGrablinkSnapshotApp()
{
    // TODO: add construction code here,
    // Place all significant initialization in InitInstance
}

/////////////////////////////////////////////////////////////////////////////
// The one and only CGrablinkSnapshotApp object

CGrablinkSnapshotApp theApp;

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotApp initialization

BOOL CGrablinkSnapshotApp::InitInstance()
{

    // --- MultiCam driver open and error-handling policy --------------------
    //
    // McOpenDriver() returns MCSTATUS and, on Windows, fails with
    // MC_SERVICE_ERROR (-25) when the "MultiCam Service" is not running. The
    // installed SDK documentation (McOpenDriver reference) is explicit:
    // "McOpenDriver will return MC_SERVICE_ERROR if the MultiCam service is not
    // started when called", and software should only access MultiCam while that
    // service is started. That same page suggests retrying McOpenDriver in a
    // loop until MC_OK; this application deliberately does not, because a loop
    // would block startup with no bound and no explanation for the operator. A
    // failed open is reported on this UI thread and aborts startup instead;
    // nothing downstream can work without the driver, and a startup failure is
    // preferable to a window that opens and then refuses every action.
    //
    // The result is remembered in g_multiCamDriverOpen so ExitInstance() closes
    // only a driver this process really opened (see that flag).
    g_multiCamDriverOpen = (McOpenDriver(NULL) == MC_OK);
    if (!g_multiCamDriverOpen)
    {
        AfxMessageBox(
            _T("The MultiCam driver could not be opened, so the application cannot run.\n\n")
            _T("Make sure the MultiCam Service is running (the driver returns\n")
            _T("MC_SERVICE_ERROR when it is not), then start the application again."),
            MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // Error handling is set to NONE, the documented "Return" behavior: "On
    // error, the MultiCam driver returns an error code" and no dialog box is
    // shown (installed SDK documentation, ErrorHandling parameter reference and
    // the "API Errors" page; NONE is also the documented default). It is the
    // only one of the four behaviors that both suppresses the vendor's own
    // message box and keeps every status available to this application:
    //   - MSGBOX displays a dialog box and, if the operator picks IGNORE, forces
    //     the function to return MC_OK, which would make a real failure look
    //     like a success to the checks below;
    //   - EXCEPTION and MSGEXCEPTION issue a Win32 structured exception, which
    //     this C++/MFC build does not catch.
    // The operator-facing errors therefore remain this application's own: every
    // setup and activation call is checked and reported on the UI thread with
    // the failing call's label and status (see CGrablinkSnapshotDoc).
    if (McSetParamInt(MC_CONFIGURATION, MC_ErrorHandling, MC_ErrorHandling_NONE) != MC_OK)
    {
        // Without the Return behavior a driver failure could raise the vendor's
        // dialog box or be turned into MC_OK, either of which makes this
        // application's own error reporting unreliable, so startup is aborted
        // rather than continued on a policy that cannot be trusted.
        AfxMessageBox(
            _T("The MultiCam error-handling policy could not be set to NONE, so driver\n")
            _T("failures could not be reported reliably. The application cannot start\n")
            _T("safely; check the MultiCam installation and try again."),
            MB_OK | MB_ICONERROR);
        McCloseDriver();
        g_multiCamDriverOpen = false;
        return FALSE;
    }

    // Best effort only. The error log is written by the parameter consistency
    // check when it produces MC_INVALID_PARAMETER_SETTING, which is not needed
    // for capture, so a refusal here is reported to the debugger and does not
    // abort startup.
    const MCSTATUS errorLogStatus =
        McSetParamStr(MC_CONFIGURATION, MC_ErrorLog, "error.log");
    if (errorLogStatus != MC_OK)
    {
        CString text;
        text.Format(_T("GrablinkSnapshot: McSetParamStr(MC_ErrorLog) failed (status %d); ")
                    _T("the parameter consistency log stays disabled.\n"),
                    errorLogStatus);
        ::OutputDebugString(text);
    }

    AfxEnableControlContainer();

    // Standard initialization
    // If you are not using these features and wish to reduce the size
    //  of your final executable, you should remove from the following
    //  the specific initialization routines you do not need.
// old MFC
	/*
#ifdef _AFXDLL
    Enable3dControls();         // Call this when using MFC in a shared DLL
#else
    Enable3dControlsStatic();   // Call this when linking to MFC statically
#endif
*/
    // Change the registry key under which our settings are stored.
    // TODO: You should modify this string to be something appropriate
    // such as the name of your company or organization.
    SetRegistryKey(_T("Local AppWizard-Generated Applications"));

    LoadStdProfileSettings();  // Load standard INI file options (including MRU)


    // Register the application's document templates.  Document templates
    //  serve as the connection between documents, frame windows and views.

    CSingleDocTemplate* pDocTemplate;
    pDocTemplate = new CSingleDocTemplate(
        IDR_MAINFRAME,
        RUNTIME_CLASS(CGrablinkSnapshotDoc),
        RUNTIME_CLASS(CMainFrame),       // main SDI frame window
        RUNTIME_CLASS(CGrablinkSnapshotView));
    AddDocTemplate(pDocTemplate);

    // Parse command line for standard shell commands, DDE, file open
    CCommandLineInfo cmdInfo;
    ParseCommandLine(cmdInfo);

    // Dispatch commands specified on the command line
    if (!ProcessShellCommand(cmdInfo))
        return FALSE;

    // The one and only window has been initialized, so show and update it.
    m_pMainWnd->ShowWindow(SW_SHOW);
    m_pMainWnd->UpdateWindow();



    return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotApp exit
int CGrablinkSnapshotApp::ExitInstance()
{
    // Terminate the driver only when this process really opened it: the SDK
    // requires as many McCloseDriver() calls as successful McOpenDriver() calls,
    // and a close after a refused open would touch a connection that was never
    // established.
    if (g_multiCamDriverOpen)
    {
        McCloseDriver();
        g_multiCamDriverOpen = false;
    }

    return CWinApp::ExitInstance();
}

/////////////////////////////////////////////////////////////////////////////
// CAboutDlg dialog used for App About

class CAboutDlg : public CDialog
{
public:
    CAboutDlg();

    // Dialog Data
    //{{AFX_DATA(CAboutDlg)
    enum { IDD = IDD_ABOUTBOX };
    //}}AFX_DATA

    // ClassWizard generated virtual function overrides
    //{{AFX_VIRTUAL(CAboutDlg)
protected:
    virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
    //}}AFX_VIRTUAL

    // Implementation
protected:
    //{{AFX_MSG(CAboutDlg)
    // No message handlers
    //}}AFX_MSG
    DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD)
{
    //{{AFX_DATA_INIT(CAboutDlg)
    //}}AFX_DATA_INIT
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    //{{AFX_DATA_MAP(CAboutDlg)
    //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
    //{{AFX_MSG_MAP(CAboutDlg)
    // No message handlers
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

// App command to run the dialog
void CGrablinkSnapshotApp::OnAppAbout()
{
    // Help > About is a modal call on the UI thread, and it is the same hazard as
    // the folder chooser: while DoModal() runs, its nested message loop
    // dispatches the WM_APP_PREVIEW_REFRESH messages the acquisition callback
    // posts, and that continuous post/Invalidate stream entering the loop leaves
    // the About box hidden behind a disabled owner.
    //
    // The guard below holds one application-modal scope on the document for
    // exactly the span of the modal call. It is a real guard even when there is
    // no document to protect (an empty guard holds nothing and is inert), it
    // nests with a scope the document may already be holding, and it ends the
    // scope in its destructor, so the scope is released on every exit from the
    // call.
    grablinkcore::ModalScope modalScope;

    // The document owns the counter, and the active document is the one whose
    // view is being repainted, so the scope is taken from there. An application
    // without a document - or a frame without an active view - simply has no
    // preview to protect.
    CFrameWnd* pMainFrame = DYNAMIC_DOWNCAST(CFrameWnd, AfxGetMainWnd());
    if (pMainFrame != NULL)
    {
        CGrablinkSnapshotDoc* pDocument =
            DYNAMIC_DOWNCAST(CGrablinkSnapshotDoc, pMainFrame->GetActiveDocument());
        if (pDocument != NULL)
        {
            modalScope = pDocument->BeginModalScope();
        }
    }

    CAboutDlg aboutDlg;
    aboutDlg.DoModal();
}
