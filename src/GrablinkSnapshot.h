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


// GrablinkSnapshot.h : main header file for the PROJECT1 application
//

#if !defined(AFX_PROJECT1_H__4C810EDA_2EF9_4AAA_BBD0_2072FCC0F06F__INCLUDED_)
#define AFX_PROJECT1_H__4C810EDA_2EF9_4AAA_BBD0_2072FCC0F06F__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#ifndef __AFXWIN_H__
    #error include 'stdafx.h' before including this file for PCH
#endif

#include "resource.h"       // main symbols


#define EURESYS_SAMPLE_PROGRAM

//---------------------------------------------------------------------------
#define EURESYS_SURFACE_COUNT 3

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotApp:
// See GrablinkSnapshot.cpp for the implementation of this class
//

class CGrablinkSnapshotApp : public CWinApp
{
public:
    CGrablinkSnapshotApp();

// Overrides
    // ClassWizard generated virtual function overrides
    //{{AFX_VIRTUAL(CGrablinkSnapshotApp)
public:
    virtual BOOL InitInstance();
    virtual int ExitInstance();
    //}}AFX_VIRTUAL

// Implementation
    //{{AFX_MSG(CGrablinkSnapshotApp)
    afx_msg void OnAppAbout();
        // NOTE - the ClassWizard will add and remove member functions here.
        //    DO NOT EDIT what you see in these blocks of generated code !
    //}}AFX_MSG
    DECLARE_MESSAGE_MAP()
};


/////////////////////////////////////////////////////////////////////////////

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_PROJECT1_H__4C810EDA_2EF9_4AAA_BBD0_2072FCC0F06F__INCLUDED_)
