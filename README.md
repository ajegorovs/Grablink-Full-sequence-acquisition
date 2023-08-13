# GrablinkSnapshotMSVC110
modified Euresys Grablink program (Visual Studio project) to continiously save images


this is a remake of old programm version based on old version of euresys example program. 
When i tried to pull from github and open as project, i could not access project properties
So i had to make a new blank project and copy.

You need libraries from Euresys Multicam Studio, they are in install folder.

Multicam libraries are imported in following manner (idk if its correct, but it works for me)
Change in project properties:

C/C++> General> Additional Include Directories> C:\Program Files (x86)\Euresys\MultiCam\Include

Linker> General> Additional Library Directories> C:\Program Files (x86)\Euresys\MultiCam\Lib\amd64

Linker> Input> Additional Dependencies> multicam.lib

or choose 32 bit lib. change run/debug accordingly.
IN "CGrablinkSnapshotDoc::CGrablinkSnapshotDoc()":
_numImages - number of recorded images;
_outputFolder -  output foolder name


In "BOOL CGrablinkSnapshotDoc::OnNewDocument()":

_outputFolder is created; camera settings are sent to device (og code) and ram memory reserved for images.

In "void CGrablinkSnapshotDoc::Callback(PMCSIGNALINFO SigInfo)"
images are retrieved and iteratively sent to reserved memory
after all images are stored in ram, they are dumped on HD/SDD into _outputFolder
