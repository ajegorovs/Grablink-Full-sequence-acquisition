# SCOUT: a side pane ("sidebar") for the existing controls

Research note. No code was changed and no build was run; every statement below is either measured on this
machine or quoted from the installed MFC sources/headers, with the path and line number given.

Question: which of the application's current UI controls could be moved into a side pane that does not exist
yet, and what would hosting them there require?

**Answer in one paragraph.** Yes for most of them, and the cheapest route is a classic `CDialogBar` (no frame
migration, documented command and update routing). All seven commands can be hosted unchanged, because a
control bar forwards its control notifications to the frame and the frame routes to the view, document and
application exactly as the menu does today - the document's handlers, its thread discipline and its
`BeginModalScope()` discipline are reused without touching the capture path. Two things should not move: the
diagnostics report (26-36 lines, up to 73 columns wide, and two lines that are unbounded) and the
edit-then-commit semantics of `Capture Settings...`, whose value is applied only when `DoModal()` returns
IDOK. A resizable, dockable, auto-hiding, layout-persisting pane is also possible, but it requires migrating
the frame from `CFrameWnd` to `CFrameWndEx` and is the one option that a Qt port (AGENDA Phase 7, still
open) would throw away.

## 1. The surface today

| Where | Control | Owner | Update handler | Refused when |
|---|---|---|---|---|
| Menu | `Go!` (32771) | doc `OnGo` - `src/GrablinkSnapshotDoc.cpp:1847` | none | capturing, save running, pending snapshot |
| Menu | `Stop!` (32772) | doc `OnStop` - `:1970` | none | never |
| Menu | `Stop & Save` (32773) | doc `OnStopSave` - `:2003` | none | save running, nothing captured, destination refused |
| Menu | `Set Output Folder...` (32774) | doc `OnSetFolder` - `:2123` | none | capturing, save running |
| Menu | `Capture Settings...` (32777) | doc `OnCaptureSettings` - `:2218` | none | capturing, save running, pending snapshot |
| Menu | `View > Fit to Window` (32776) | doc `OnViewFitWindow` - `:2336` | **yes** - `:2356` (`SetCheck`) | never |
| Menu | `View > Capture Diagnostics...` (32778) | doc `OnCaptureDiagnostics` - `:2396` | none | never (pure reader) |
| Menu | `Help > About` | app `OnAppAbout` - `src/GrablinkSnapshot.cpp:272` | none | never |
| Status bar | one pane, `ID_INDICATOR_CHANNEL`, min 400 px - `src/MainFrm.cpp:46-51,80` | written only by `CGrablinkSnapshotView::OnDraw` - `src/GrablinkSnapshotView.cpp:422` | `Enable()` only - `MainFrm.cpp:113` | - |
| Dialog | `IDD_CAPTURE_SETTINGS_DIALOG` (102): frames edit `1000` + memory label `1001` | `CCaptureSettingsDialog` - `:1621-1770` | n/a (modal) | see `:2230-2260` |
| Dialog | `IDD_CAPTURE_DIAGNOSTICS_DIALOG` (103): read-only report edit `1002` + Close | `CCaptureDiagnosticsDialog` - `:1786-1840` | n/a (modal, static snapshot) | - |
| Dialog | `IDD_ABOUTBOX` (100) | `CAboutDlg` | default | - |
| - | `IDD_FOLDER_DIALOG` / `IDC_FOLDER_EDIT` | **dead resource**: no class, no `DoModal`; the folder is chosen with `SHBrowseForFolder` - `:2200-2215` | - | - |

Status readout fields (all assembled in `OnDraw`, throttled by elapsed time to ~2 Hz - `View.cpp:364-370`):
driver `Channel State` (`McGetParamInt`, `:395`) and `Frame Rate` (`McGetParamFloat`, `:397`), plus save
progress / captured count / pending frames from the document's accessors. Exactly two driver calls per
throttle window, skipped when `m_Channel == 0` (`:388-389`).

Only `ID_VIEW_FIT_WINDOW` has a UI-update handler. The other commands are deliberately runtime refusals
instead of greyed-out items, and the reason is in the code: *"MFC runs ON_UPDATE_COMMAND_UI handlers only for
items inside a popup, so an update handler here would never be called and the item could not be disabled"*
(`Doc.cpp:2226-2229`). That constraint is what a pane changes - see §3.4.

Persisted today: `Capture\FrameCount` (default 200) via `WriteProfileInt` (`Doc.cpp:2307`) under
`HKCU\Software\Local AppWizard-Generated Applications\GrablinkSnapshot` (`GrablinkSnapshot.cpp:175`). The
output folder, the file name and `_bResizeImage` are **not** persisted (`Doc.cpp:306-311`).

## 2. What fits a side pane and what does not

**Fits:** the seven commands as buttons; the status readout (channel state, frame rate, progress, pending
frames); the configured capacity and its committed-memory estimate (currently only visible inside a modal
dialog); the current output folder as a path plus a browse button.

**Does not fit:**

- **The diagnostics report.** Measured from the builder at `Doc.cpp:2429-2571`: 26 lines minimum, 36 maximum,
  widest fixed line 73 columns, and two lines - `Last error: %s` and `Last save start was refused: %s` - carry
  free text with no bound. It is a one-time snapshot with no timer, on purpose (`:1782-1785`). A narrow pane
  can show a compact live subset (state, stored/capacity, FPS, save progress); the full report belongs in the
  dialog that was sized for it.
- **`Capture Settings...`'s edit.** The number is read only in `OnOK`/`ShowRequiredMemory`
  (`:1735-1743`), `_numImages` is assigned only after `DoModal()` returned IDOK (`:2279-2301`), and a refused
  OK keeps the dialog open with the text re-selected (`:1763-1769`). Modeless, that becomes an explicit commit
  policy (Apply button, or commit on focus loss) that the operator can get wrong; the capacity is also the
  setting that decides a multi-gigabyte commit, so its confirmation path should not become ambient.
- **`Help > About`** and `IDD_FOLDER_DIALOG` (dead).

## 3. Hosting mechanisms available to this project

### 3.1 `CDialogBar` - classic control bar, no migration, cheapest

- Available now: `afxext.h:490` `class CDialogBar : public CControlBar`; already reachable through
  `src/StdAfx.h`'s `#include <afxext.h>`. Not feature-pack, not ribbon, no OLE requirement.
- Alignment decides the geometry, documented: with `CBRS_LEFT`/`CBRS_RIGHT` the bar's **height is the frame's
  and its width is the dialog template's** - i.e. a fixed-width sidebar. `CDialogBar::Create` takes the
  template id, and `CalcFixedLayout` returns the template size (`bardlg.cpp:91-98`).
- Command routing: the header comment states it directly - *"It is a modeless dialog that delegates all
  control notifications to the parent window of the control bar [the grandparent of the control]"*
  (`afxext.h:485-488`). Microsoft documents the same for `CDialogBar` (BN_CLICKED / EN_CHANGE reach the main
  window).
- Update-command-UI routing for its controls is both documented (TN031: *"At idle time, the dialog bar will
  call the ON_UPDATE_COMMAND_UI handler with the command ID of all the buttons that have a ID >= 0x8000"*;
  the `CCmdUI` table lists "Normal button in CDialogBar" and "Normal control in CDialogBar") and implemented:
  `CDialogBar::OnUpdateCmdUI` -> `UpdateDialogControls` (`bardlg.cpp:100-103`).
- Template constraint, enforced in debug builds: the template must exist and be an **invisible child**
  (`bardlg.cpp:37` -> `_AfxCheckDialogTemplate`, which rejects `WS_VISIBLE` and a non-child template), so this
  needs a new dialog resource, not one of the existing ones.
- Cost: fixed width only (no user drag), and it is the classic `CFrameWnd::EnableDocking` +
  `DockControlBar` path, i.e. a different mechanism from the feature-pack panes if we ever want those.

### 3.2 `CDockablePane` + `CFrameWndEx` - resizable, dockable, auto-hide, persisted layout

- The classes exist in this toolchain (all in
  `...\MSVC\14.35.32215\atlmfc\include`): `afxdockablepane.h`, `afxdockingmanager.h`, `afxpane.h`,
  `afxframewndex.h`, `afxtoolbar.h`, `afxcontrolbars.h`, `afxsplitterwndex.h`.
- **A plain `CFrameWnd` cannot dock one.** The blocker is not compilation - every `Create` takes a plain
  `CWnd*` and no header mentions `CFrameWndEx` - but docking-manager discovery:
  `afxglobalutils.cpp:283-287` returns a docking manager only for `CFrameWndEx` / `CMDIFrameWndEx` /
  `COleIPFrameWndEx` / `COleDocIPFrameWndEx` / `CMDIChildWndEx`; a plain `CFrameWnd` yields `NULL`, and
  `CBasePane::CreateEx` (`afxbasepane.cpp:119-122`) goes through that lookup. So this option means migrating
  `CMainFrame` (`src/MainFrm.h:31`) to `CFrameWndEx`, which per Microsoft's own migration walkthrough also
  means `CWinAppEx`, `SetRegistryBase`, `afxcontrolbars.h` in the precompiled header, `CMFCStatusBar`, and
  `DockPane()` instead of `DockControlBar()`.
- Static MFC is fine: the x64 Unicode static-release archive contains the feature-pack objects - verified
  here with `lib.exe /LIST` on `atlmfc\lib\x64\uafxcw.lib` (34 matching members, incl. `afxdockablepane.obj`,
  `afxdockingmanager.obj`, `afxtoolbar.obj`). This matters because **`Debug|x64` and `Debug|Win32` are
  `UseOfMfc=Static`** while Release is `Dynamic` (`src/GrablinkSnapshotMSVC110.vcxproj:36-51`).
- The known static-link trap is resources: `afxres.rc` contains no `AFXBARRES` resources at all; the feature
  pack's bitmaps and strings live in `afxribbon.rc`, which this project's `.rc` does not include
  (`src/GrablinkSnapshot.rc:393` includes only `afxres.rc`). Missing it produces the classic
  "Can't load bitmap ... ENSURE(str.LoadString(IDS_AFXBARRES_...))" asserts under static MFC.
- Layout persistence comes with it: `CWinAppEx::LoadState/SaveState`, `CDockingManager::LoadState/SaveState`,
  `CDockablePane::LoadState/SaveState`, `CFrameWndEx::SetDockState` / `EnableLoadDockState` /
  `EnablePaneMenu`. Note the registry APIs are on `CWinAppEx` (`afxwinappex.h:112-116`), not `CWinApp`; a
  non-Ex app would persist manually under the existing profile key.

### 3.3 `CSplitterWnd` (static 1x2) - a fixed side region without the feature pack

Create a static splitter in `CFrameWnd::OnCreateClient` (not currently overridden) with two panes, all of
them created before that function returns. A pane can be any `CWnd` with `DECLARE_DYNCREATE`. No feature pack,
no migration, but also no docking, no auto-hide, no collapse, and the pane is a full client-area pane rather
than a bar - worthwhile only if the sidebar should be a view (a form or a grid), not a strip of controls.

### 3.4 Commands and update handlers from a pane - what actually happens (verified in the sources)

- **Commands reach the document.** `CBasePane::WindowProc` forwards unhandled `WM_COMMAND`, `WM_NOTIFY`,
  `WM_DRAWITEM`, `WM_MEASUREITEM`, `WM_COMPAREITEM`, `WM_VKEYTOITEM`, `WM_CHARTOITEM` to its owner -
  *"send these messages to the owner if not handled"* (`afxbasepane.cpp:963-984`,
  `lResult = GetOwner()->SendMessage(message, wParam, lParam)` at `:984`). A pane's owner is the frame, and
  `CFrameWnd::OnCmdMsg` pumps view first, then frame, then app (`winfrm.cpp:977-996`), with the view reaching
  the document (`viewcore.cpp:160`). Result: a `Go!` button in a pane runs `CGrablinkSnapshotDoc::OnGo` with
  no handler change. Toolbar-typed buttons instead send the command to their owner themselves
  (`afxtoolbar.cpp:2482,7294`).
- **Update handlers reach the controls.** At idle the framework broadcasts `WM_IDLEUPDATECMDUI` to the main
  window's descendants (`thrdcore.cpp:669-677`); `CBasePane::OnIdleUpdateCmdUI` (`afxbasepane.cpp:745-760`)
  calls the pane's `OnUpdateCmdUI`, which for `CDockablePane` and `CDialogBar` is `UpdateDialogControls`
  (`afxdockablepane.cpp:2765-2767`, `bardlg.cpp:100-103`), which walks the pane's children and routes
  `CN_UPDATE_COMMAND_UI` for each control id, applying `Enable()`/`SetCheck()`. Two caveats from the same
  sources: it runs only while the pane is `WS_VISIBLE` and its dock bar is visible (`:749-750`), and the
  framework's auto-disable is limited to `DLGC_BUTTON` controls (`wincore.cpp:4569-4590`) - a disabled
  `Go!`/`Stop!` state must come from our own handlers.
- **Consequence for this repo's design choice:** always-visible buttons *should* carry
  `ON_UPDATE_COMMAND_UI` (unlike the top-level menu items, where they cannot fire - `Doc.cpp:2226-2229`), so
  the sidebar shows why a command is unavailable instead of popping a message box after a click. The state is
  already available through thread-safe accessors (`IsCapturing`, `CaptureFrameCount`,
  `GetSaveProgress`, `PendingSaveFrameCount`).
- **Timing caveat:** idle updates run from `CWinApp::OnIdle`, which is called only when the message queue is
  empty, and the acquisition callback posts a preview refresh per frame (coalesced to at most one
  outstanding). Idle updates therefore interleave with a ~350 Hz post stream and their timing is not ours.
  Sidebar control state should be pushed on the transitions we own (capture start/stop, save start/finish,
  pending change), not left to be polled by the framework.

## 4. Invariants the pane must respect

1. **No new nested-message-loop hole.** The modal-scope discipline (`core/ModalScopeCounter.h`) covers the
   modal calls this application makes. A pane adds UI whose loops are MFC's:
   - Verified **not** a nested loop: the pane divider drag. `CPaneDivider::OnLButtonDown` sets capture and
     creates a floating `CPaneTrackingWnd`; `OnMouseMove` only moves that window; the relayout happens in
     `StopTracking(TRUE)` on button-up (`afxpanedivider.cpp:281-306`, `StopTracking` at `:397`). No `GetMessage` loop anywhere in
     `afxpanedivider.cpp`. The live preview is not resized during the drag.
   - Verified **nested loops** exist on narrower feature-pack paths: `CPaneFrameWnd::StartTearOff`
     (`afxpaneframewnd.cpp:2989-3011`, a `GetMessage` loop that dispatches every other message, so it pumps
     our refresh posts) and `CMFCPopupMenu::StartResize` (`afxpopupmenu.cpp:3845`). Pane caption menus,
     auto-hide and the framework's popup menus all route through the popup-menu code.
   - Already-existing, unrelated to panes but in the same class: a frame border drag runs the Windows
     sizing modal loop (`WM_ENTERSIZEMOVE`), and the AGENDA already lists window resize/maximize as an
     untested reproduction case.
   - Recommendation: take a modal scope around `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE` on the frame, and cover
     any pane popup we enable, rather than discovering the class again on a live run.
2. **One owner for the status readout, throttled by elapsed time.** The ~2 Hz gate and the two driver reads
   live inside `OnDraw` (`View.cpp:364-419`). A pane must not add a second timer or a second set of driver
   reads; publish one snapshot (document or frame) that the status bar and the pane both render, and keep the
   "unavailable" degradation on a failed read.
3. **The pane must not become a second thread or a second capture-state reader.** It reads only the existing
   lock/worker-guarded accessors; nothing in a pane belongs on the callback path, and a failed save start
   must still leave frames retryable (`Stop & Save`).
4. **A modeless pane loses the transitive modal scope.** `CCaptureSettingsDialog::RefuseInput`'s
   `AfxMessageBox` (`Doc.cpp:1767`) is currently covered only because the whole dialog sits inside
   `BeginModalScope()` at `:2275`. The same refusal box from a pane would fire at scope depth 0 and let the
   refresh stream into a nested loop - the exact defect `core/ModalScopeCounter.h` exists to prevent. Every
   message box and browse call a pane triggers must take its own scope.
5. **Preview geometry changes.** Fit-to-window scales to the client rect, so a side pane narrows the
   destination and moves the near-1:1 reproduction case that produced the grey-grid artifact; at 2040 px
   image width plus a pane, 1:1 mode clips sooner. Operator-visible, and part of the live check.
6. **DPI and themed controls are not currently available.** Measured from the shipped x64 Release binary's
   manifest (`mt.exe -inputresource:...\x64\Release\GrablinkSnapshot.exe;#1`): it declares `requestedExecutionLevel`
   and `dpiAware=true` only - **no `Microsoft.Windows.Common-Controls` dependency**, so the app runs on
   Common Controls v5 and any new list view/edit will look classic; and system-DPI-awareness means a
   pixel- or DLU-fixed pane layout is bitmap-scaled at >100% scaling. Also note nothing in the toolchain
   injects the manifest dependency (no `manifestdependency` pragma exists anywhere under the MSVC tree);
   it is the application's job.
7. **A hidden pane must stay reachable.** If the pane can be collapsed or closed, it needs a visible checked
   command that brings it back, and no persisted layout should be able to restore it off-screen or
   zero-sized. Note the documented off-screen rescue exists only for the *classic* `CDockState` path
   ("if the bar is not visible with the current screen settings, CDockState scales the bar's position");
   Microsoft documents no equivalent for the `CFrameWndEx` layout. `EnableLoadDockState(FALSE)` is the
   documented way to refuse a stored layout.

## 5. Recommendation

- **If the sidebar is wanted for validation/instrumentation now:** host it as a `CDialogBar` (§3.1) on the
  existing `CFrameWnd`. It is the only option that adds no framework migration, its command and
  update-command-UI routing is documented *and* implemented, and it reuses every existing handler. Contents:
  the seven commands as buttons with new update handlers, the capacity + committed-memory estimate, the
  output folder, and a live status block fed by the existing 2 Hz readout. Leave the diagnostics dialog as
  the place for the full report and `Capture Settings...` as a modal commit.
- **If the sidebar is the start of the real operator UI:** decide AGENDA Phase 7 (toolkit) first. A
  `CFrameWndEx` + `CWinAppEx` + feature-pack-resources + registry-layout migration is real work in the one
  part of this codebase that has no tests, and it is the part a Qt port replaces wholesale - while a
  `CDialogBar` is a few hundred lines that a port would discard without regret.
- **Either way:** put the enable/disable rule in `core/` as a small tested unit that answers "which commands
  are available given (capturing, save running, pending snapshot, destination known, frames captured)", so
  the sidebar's state logic is hardware-free testable per the repo's convention, instead of duplicating the
  refusal conditions that are now spread over `OnGo`/`OnStopSave`/`OnCaptureSettings`/`OnSetFolder`.

## 6. Not established / must be measured before any code

1. Whether a hand-rolled `CDockingManager` on a plain `CFrameWnd` merely never docks or asserts - only
   community reports exist; a 30-line spike outside this repo settles it.
2. Whether any *enabled* pane popup or auto-hide path starves under a live 351 fps preview. The divider drag
   is verified loop-free; the popup/tear-off paths are not covered by the current modal-scope discipline.
3. On-screen behaviour at non-100% display scaling (the app is system-DPI-aware, the pane layout would be
   the first DPI-sensitive layout in the app).
4. Whether the 1:1 preview remains usable with a fixed side pane at the operator's screen width.

## Evidence

- Repo: `src/GrablinkSnapshotDoc.cpp`, `src/GrablinkSnapshotView.cpp`, `src/MainFrm.{h,cpp}`,
  `src/GrablinkSnapshot.rc`, `src/resource.h`, `src/GrablinkSnapshot.cpp`,
  `src/GrablinkSnapshotMSVC110.vcxproj` (lines quoted inline).
- Installed MFC, `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.35.32215\atlmfc`:
  `include\afxext.h`, `afxbasepane.h`, `afxdockablepane.h`, `afxdockingmanager.h`, `afxframewndex.h`,
  `afxcontrolbars.h`, `afxpaneframewnd.h`, `afxtoolbar.h`, `afxstatusbar.h`, `afxsplitterwndex.h`,
  `afxwinappex.h`, `afxres.h`; `src\mfc\afxbasepane.cpp`, `afxdockablepane.cpp`, `afxpanedivider.cpp`,
  `afxpaneframewnd.cpp`, `afxpopupmenu.cpp`, `afxglobalutils.cpp`, `bardlg.cpp`, `winfrm.cpp`, `viewcore.cpp`,
  `wincore.cpp`, `thrdcore.cpp`, `cmdtarg.cpp`; `lib\x64\uafxcw.lib` (member list).
- Measured on this machine: the manifest of `x64\Release\GrablinkSnapshot.exe` (`mt.exe`), 75-character
  longest literal in the diagnostics builder, 46 text-emitting statements in that region.
- Microsoft documentation consulted for the documented half: `CDialogBar class`, `Dialog Bars`, `TN031`,
  `CCmdUI class`, `CFrameWndEx class`, `CDockablePane class`, `CDockingManager class`, `CPane class`,
  `CSplitterWnd class`, `TN029`, `CWinAppEx class`, `CDockState class`, `CWinApp::OnIdle`,
  `Build Requirements for Windows Vista Common Controls`, the Scribble feature-pack migration walkthrough.
