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

    // Initialize driver and error handling
    McOpenDriver(NULL);

    // Activate message box error handling and generate an error log file
    McSetParamInt (MC_CONFIGURATION, MC_ErrorHandling, MC_ErrorHandling_MSGBOX);
    McSetParamStr (MC_CONFIGURATION, MC_ErrorLog, "error.log");

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
    // Terminate driver
    McCloseDriver ();

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
