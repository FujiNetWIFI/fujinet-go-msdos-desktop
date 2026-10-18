; NSIS installer for the Windows build.
;
; Deliberately a per-user install: it lands in %LOCALAPPDATA%\Programs, needs
; no administrator rights and raises no UAC prompt. The build is unsigned, so
; users already have one SmartScreen warning to click through; asking for
; elevation on top of that is a worse first run for no benefit. It also keeps
; the install directory writable, which matches how the app provisions its
; FujiNet runtime tree.
;
; Built in the same MSYS2 job that builds the app (mingw-w64-*-nsis), from the
; staged folder that also becomes the portable zip -- so the two artifacts are
; the same bits, packaged twice. Ported from fujinet-go-atari800-desktop's own
; installer.nsi, renamed for MS-DOS. Nothing copyrighted is in the staged
; folder: no BIOS, video, disk-controller or MIDI ROM ships (they are imported
; at run time); the FujiNet boot floppy (FreeDOS, GPL, with FUJINET.SYS) is
; built into the exe.
;
; The staged folder is cmake/FujiNetRuntime.cmake's Windows install layout:
;   fujinet-go-msdos-windows.exe
;   fujinet.dll                       (where the session looks first)
;   fujinet\fnconfig.ini, fujinet\data\, fujinet\SD\   (the pristine runtime)
;
; makensis -DVERSION=x.y.z -DSRCDIR=<staged folder> -DOUTFILE=<exe> installer.nsi

Unicode true
; The architecture makensis's plugins were built for (release.yml reads it
; off the nsDialogs DLL); x86-unicode when not given.
!ifdef NSIS_TARGET
  Target ${NSIS_TARGET}
!endif

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "dist\FujiNet-Go-MS-DOS"
!endif
!ifndef OUTFILE
  !define OUTFILE "FujiNet-Go-MS-DOS-Setup.exe"
!endif
; CI passes this absolute; the default assumes makensis runs from the repo root.
!ifndef APPICON
  !define APPICON "frontends\windows\app.ico"
!endif

!define APPNAME    "FujiNet Go MS-DOS"
!define PUBLISHER  "Thomas Cherryhomes"
!define EXENAME    "fujinet-go-msdos-windows.exe"
!define HOMEPAGE   "https://fujinet.online/"
!define REGAPP     "Software\FujiNetGoMSDOS"
!define REGUNINST  "Software\Microsoft\Windows\CurrentVersion\Uninstall\FujiNetGoMSDOS"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
InstallDirRegKey HKCU "${REGAPP}" "InstallDir"
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "GPL-3.0-or-later"

!include "MUI2.nsh"

; The application's own icon, so the installer and the Add/Remove Programs
; entry look like the thing being installed.
!define MUI_ICON   "${APPICON}"
!define MUI_UNICON "${APPICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APPNAME}"

; The welcome and finish pages are nsDialogs pages; a makensis without the
; plugin (-DNO_NSDIALOGS, see release.yml) builds the rest.
!ifndef NO_NSDIALOGS
!insertmacro MUI_PAGE_WELCOME
!endif
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!ifndef NO_NSDIALOGS
!insertmacro MUI_PAGE_FINISH
!endif

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  ; The whole staged folder: the exe, fujinet.dll beside it (where the session
  ; looks first) and the pristine FujiNet runtime tree it provisions from.
  File /r "${SRCDIR}\*"

  CreateShortCut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" \
                 "$INSTDIR\${EXENAME}" 0

  WriteRegStr HKCU "${REGAPP}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${REGAPP}" "Version"    "${VERSION}"

  ; Add/Remove Programs.
  WriteRegStr   HKCU "${REGUNINST}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKCU "${REGUNINST}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKCU "${REGUNINST}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKCU "${REGUNINST}" "DisplayIcon"     "$INSTDIR\${EXENAME}"
  WriteRegStr   HKCU "${REGUNINST}" "URLInfoAbout"    "${HOMEPAGE}"
  WriteRegStr   HKCU "${REGUNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${REGUNINST}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegDWORD HKCU "${REGUNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${REGUNINST}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\${EXENAME}"
  ; fujinet.dll and the libraries 86Box links (freetype, libpng, libsndfile
  ; and theirs), all shipped beside the exe
  Delete "$INSTDIR\*.dll"
  RMDir /r "$INSTDIR\fujinet"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "${REGUNINST}"
  DeleteRegKey HKCU "${REGAPP}"

  ; Settings, the provisioned FujiNet tree, the imported BIOS ROMs, the hard
  ; disk images and the SD folder live under %APPDATA%\fujinet-go-msdos and
  ; %LOCALAPPDATA%\fujinet-go-msdos and are deliberately left behind: an
  ; uninstall should not throw away someone's configuration, the ROMs they
  ; imported or the disks they made, and a reinstall picks it straight back up.
SectionEnd
