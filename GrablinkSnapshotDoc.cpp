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


#include <stdlib.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc

IMPLEMENT_DYNCREATE(CGrablinkSnapshotDoc, CDocument)

BEGIN_MESSAGE_MAP(CGrablinkSnapshotDoc, CDocument)
    //{{AFX_MSG_MAP(CGrablinkSnapshotDoc)
    ON_COMMAND(ID_GO, OnGo)
    ON_COMMAND(ID_STOP, OnStop)
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc construction/destruction

CGrablinkSnapshotDoc::CGrablinkSnapshotDoc()
{
    m_Channel = 0;
    m_pCurrent = NULL;
    m_SizeX = 0;
    m_SizeY = 0;
    m_BufferPitch = m_SizeX;
    m_bScreenRefreshCompleted = TRUE;

    //ours
    _numImages = 3;                     
    _numImagesCounter = 0;              
    _strFilename = "Image";             
    _bStopped = true;                   
    BmpHelper::Init8bppHeaders();
}

CGrablinkSnapshotDoc::~CGrablinkSnapshotDoc()
{
    // Set the channel to IDLE before deleting it
    McSetParamInt(m_Channel, MC_ChannelState, MC_ChannelState_IDLE);

    // Delete the channel
    McDelete(m_Channel);
}

BOOL CGrablinkSnapshotDoc::OnNewDocument()
{
    if (!CDocument::OnNewDocument())
        return FALSE;

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

    // The memory allocation for the images is automatically done by MultiCam when activating the channel.
    // We only set the number of surfaces to be created by MultiCam.
    McSetParamInt(m_Channel, MC_SurfaceCount, EURESYS_SURFACE_COUNT);

    // Enable MultiCam signals
    McSetParamInt(m_Channel, MC_SignalEnable + MC_SIG_SURFACE_PROCESSING, MC_SignalEnable_ON);
    McSetParamInt(m_Channel, MC_SignalEnable + MC_SIG_ACQUISITION_FAILURE, MC_SignalEnable_ON);

    // Register the callback function
    McRegisterCallback(m_Channel, GlobalCallback, this);

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

void CGrablinkSnapshotDoc::Callback(PMCSIGNALINFO SigInfo)
{
    // + GrablinkSnapshot Sample Program

    switch(SigInfo->Signal)
    {
        case MC_SIG_SURFACE_PROCESSING: //original code did not have {} for case.
        {
            // Update "current" surface address pointer
            McGetParamPtr(SigInfo->SignalInfo, MC_SurfaceAddr, &m_pCurrent);
            if (_numImagesCounter < _numImages)
            {
                //some basic tests for counter
                /*
                CString str;
                str.Format("xXx%04dxXx", _numImagesCounter);
                MessageBoxA(NULL, str, "testx", MB_OK);
                */

                // create a path string using file name and counter number
                CString str;
                str.Format(".\\SavedImages\\img%04d.bmp", _numImagesCounter);
                // str.Format(".\\SavedImages\\img%04d.txt", _numImagesCounter);

                // Open the file for writing
                FILE* f = fopen(str, "wb");

                if (!f)
                {
                    AfxMessageBox((LPCTSTR)"BMP file writing: Unable to write bitmap image data\n", MB_OK | MB_ICONSTOP);
                }

                if (f != NULL)
                {
                    // Save the image as bmp file
                    BmpHelper::SaveTo8bppBmpFile(f, (LONG)m_SizeX, (LONG)m_SizeY, (unsigned char*)m_pCurrent);
                    fclose(f);
                }
                /*
                 if (f != NULL)
                {
                    fputs("fopen example", f);
                    fclose(f);
                }
                */
               
                
            }
            _numImagesCounter++;
            //----------------------------------------
            //
            // Insert the eVision code here.
            //
            //----------------------------------------

            // Post screen refresh message only if previous refresh completed
            if (m_bScreenRefreshCompleted)
            {
                m_bScreenRefreshCompleted = FALSE;
                POSITION pos = GetFirstViewPosition();
                CGrablinkSnapshotView* pView = (CGrablinkSnapshotView*)GetNextView(pos);
                if (pView == NULL) return;

                HWND hWindow = pView->GetSafeHwnd();
                if (hWindow == NULL) return;

                RECT recpict;
                recpict.left = 0;
                recpict.top = 0;
                recpict.right = m_SizeX - 1;
                recpict.bottom = m_SizeY - 1;
                InvalidateRect(hWindow, &recpict, FALSE);
            }
            break;
        }
        case MC_SIG_END_ACQUISITION_SEQUENCE:
            //MessageBox(NULL, "Images acquired.\n", "GrablinkSnapshot", MB_OK);
            _bStopped = true;
            break;

        case MC_SIG_ACQUISITION_FAILURE:
            MessageBox(NULL, "Acquisition Failure !", "GrablinkSnapshot", MB_OK);
            break;
        default:
            break;

    }
    // - GrablinkSnapshot Sample Program
}



/////////////////////////////////////////////////////////////////////////////
// CGrablinkSnapshotDoc commands

void CGrablinkSnapshotDoc::OnGo()
{
    // + GrablinkSnapshot Sample Program
    

    // Start an acquisition sequence by activating the channel
    McSetParamInt(m_Channel, MC_ChannelState, MC_ChannelState_ACTIVE);
    _bStopped = false;
    _numImagesCounter = 0;
    
    // - GrablinkSnapshot Sample Program
}

void CGrablinkSnapshotDoc::OnStop()
{
    // + GrablinkSnapshot Sample Program
    
    // Stop an acquisition sequence by deactivating the channel
    McSetParamInt(m_Channel, MC_ChannelState, MC_ChannelState_IDLE);
    
    // - GrablinkSnapshot Sample Program
    
    UpdateAllViews(NULL);
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
