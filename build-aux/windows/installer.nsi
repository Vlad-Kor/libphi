; Phi Document Viewer installer, uninstaller and updater for Windows.
;
; Build the bundle with bundle.sh first, then:
;
;   makensis -DVERSION=0.1 -DVERSION_QUAD=0.1.0.0 -DBUNDLE_DIR=<bundle> \
;            -DOUTFILE=<setup.exe> [-DWEBVIEW2_BOOTSTRAPPER=<exe>] installer.nsi
;
; Running a newer installer over an existing installation updates it in
; place and keeps its location, install mode and settings. Silent installs
; take /S, /AllUsers or /CurrentUser and /D=<folder>.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!ifndef VERSION | VERSION_QUAD | BUNDLE_DIR | OUTFILE
  !error "Define VERSION, VERSION_QUAD, BUNDLE_DIR and OUTFILE"
!endif

!define APP_NAME "Phi Document Viewer"
!define APP_ID "ai.korsakov.Phi"
!define PUBLISHER "Vlad Korsakov"
!define WEBSITE "https://github.com/Vlad-Kor/libphi"
!define EXE "pdfv.exe"
!define APP_KEY "Software\Phi"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Phi"
!define APP_PATHS_KEY "Software\Microsoft\Windows\CurrentVersion\App Paths\${EXE}"
!define WEBVIEW2_CLIENT "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

Name "${APP_NAME}"
OutFile "${OUTFILE}"
BrandingText "${APP_NAME} ${VERSION}"

VIProductVersion "${VERSION_QUAD}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2026 ${PUBLISHER}"

; Install for the current user (no administrator rights needed) or for all
; users; administrators are asked which.
!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_INSTALLMODE_INSTDIR "Phi"
!define MULTIUSER_USE_PROGRAMFILES64
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "${APP_KEY}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "InstallMode"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "${APP_KEY}"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallLocation"

!include MultiUser.nsh
!include MUI2.nsh
!include LogicLib.nsh
!include Sections.nsh
!include FileFunc.nsh
!include WordFunc.nsh
!include WinMessages.nsh
!include WinVer.nsh
!include x64.nsh

!define MUI_ICON "..\..\data\windows\ai.korsakov.Phi.ico"
!define MUI_UNICON "..\..\data\windows\ai.korsakov.Phi.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION StartApp
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
; The AGPL grants rights; nobody has to accept it to use the program.
!define MUI_LICENSEPAGE_BUTTON "$(^NextBtn)"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "${APP_NAME} is free software: you may use, study, share and change it under the terms of this license."
!insertmacro MUI_PAGE_LICENSE "${BUNDLE_DIR}\share\doc\phi\COPYING"
!define MULTIUSER_PAGE_CUSTOMFUNCTION_PRE SkipDirectoryOnUpdate
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipDirectoryOnUpdate
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Var Updating
Var PathRoot
Var PathKey

; Files and folders ---------------------------------------------------------

; Asks to close Phi while one of its files in the installation is in use,
; since a running program's files cannot be replaced or removed.
!macro CLOSE_RUNNING UN
Function ${UN}CloseRunning
  ${Do}
    ClearErrors
    ${If} ${FileExists} "$INSTDIR\bin\${EXE}"
      FileOpen $0 "$INSTDIR\bin\${EXE}" a
      ${IfNot} ${Errors}
        FileClose $0
        ${Break}
      ${EndIf}
    ${Else}
      ${Break}
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
      "${APP_NAME} is running. Close all of its windows, then choose Retry." \
      /SD IDCANCEL IDRETRY retry
    Abort "${APP_NAME} is still running."
    retry:
  ${Loop}
  ; The session bus GLib starts for single-instance handling may outlive the
  ; windows for a moment; it holds no user data.
  nsExec::Exec 'powershell.exe -NoProfile -NonInteractive -Command \
    "Get-Process gdbus -ErrorAction SilentlyContinue | \
     Where-Object { $$_.Path -like $\'$INSTDIR\bin\*$\' } | Stop-Process -Force"'
  Pop $0
FunctionEnd
!macroend
!insertmacro CLOSE_RUNNING ""
!insertmacro CLOSE_RUNNING "un."

!macro REMOVE_PROGRAM_FILES
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\share"
  RMDir /r "$INSTDIR\etc"
  RMDir /r "$INSTDIR\cmd"
  Delete "$INSTDIR\uninstall.exe"
!macroend

; PATH ------------------------------------------------------------------------

; The command line shim in $INSTDIR\cmd keeps the DLLs in bin out of PATH,
; where other programs could pick them up.
Function SetPathLocation
  ${If} $MultiUser.InstallMode == AllUsers
    StrCpy $PathRoot HKLM
    StrCpy $PathKey "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
  ${Else}
    StrCpy $PathRoot HKCU
    StrCpy $PathKey "Environment"
  ${EndIf}
FunctionEnd

!macro EDIT_PATH UN OPERATION
Function ${UN}EditPath${OPERATION}
  Call ${UN}SetPathLocation
  ${If} $PathRoot == HKLM
    ReadRegStr $0 HKLM $PathKey "Path"
  ${Else}
    ReadRegStr $0 HKCU $PathKey "Path"
  ${EndIf}
  ; Never write back a value that was too long to read completely.
  StrLen $1 $0
  IntOp $2 ${NSIS_MAX_STRLEN} - 512
  ${If} $1 > $2
    DetailPrint "PATH is too long to change; skipping."
    Return
  ${EndIf}
  ${WordAdd} $0 ";" "${OPERATION}$INSTDIR\cmd" $1
  ${If} $1 == $0
    Return
  ${EndIf}
  ${If} $PathRoot == HKLM
    WriteRegExpandStr HKLM $PathKey "Path" $1
  ${Else}
    WriteRegExpandStr HKCU $PathKey "Path" $1
  ${EndIf}
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=1000
FunctionEnd
!macroend
!insertmacro EDIT_PATH "" "+"
!insertmacro EDIT_PATH "un." "-"

Function un.SetPathLocation
  ${If} $MultiUser.InstallMode == AllUsers
    StrCpy $PathRoot HKLM
    StrCpy $PathKey "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
  ${Else}
    StrCpy $PathRoot HKCU
    StrCpy $PathKey "Environment"
  ${EndIf}
FunctionEnd

; File types ------------------------------------------------------------------

!macro REGISTER_TYPE PROGID DESCRIPTION
  WriteRegStr SHCTX "Software\Classes\${PROGID}" "" "${DESCRIPTION}"
  WriteRegStr SHCTX "Software\Classes\${PROGID}" "FriendlyTypeName" "${DESCRIPTION}"
  WriteRegStr SHCTX "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\bin\${EXE},0"
  WriteRegStr SHCTX "Software\Classes\${PROGID}\shell\open" "FriendlyAppName" "${APP_NAME}"
  WriteRegStr SHCTX "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\bin\${EXE}" "%1"'
!macroend

!macro REGISTER_EXTENSION EXTENSION PROGID
  WriteRegStr SHCTX "Software\Classes\${EXTENSION}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr SHCTX "Software\Classes\Applications\${EXE}\SupportedTypes" "${EXTENSION}" ""
  WriteRegStr SHCTX "${APP_KEY}\Capabilities\FileAssociations" "${EXTENSION}" "${PROGID}"
!macroend

; Opens a type with Phi if no other program claimed it, as for Markdown on a
; fresh system. Windows only lets users choose the default program otherwise.
!macro CLAIM_EXTENSION EXTENSION PROGID
  ReadRegStr $0 HKCR "${EXTENSION}" ""
  ${If} $0 == ""
    WriteRegStr SHCTX "Software\Classes\${EXTENSION}" "" "${PROGID}"
  ${EndIf}
!macroend

!macro UNREGISTER_EXTENSION EXTENSION PROGID
  DeleteRegValue SHCTX "Software\Classes\${EXTENSION}\OpenWithProgids" "${PROGID}"
  DeleteRegKey /ifempty SHCTX "Software\Classes\${EXTENSION}\OpenWithProgids"
  ReadRegStr $0 SHCTX "Software\Classes\${EXTENSION}" ""
  ${If} $0 == "${PROGID}"
    DeleteRegValue SHCTX "Software\Classes\${EXTENSION}" ""
  ${EndIf}
  DeleteRegKey /ifempty SHCTX "Software\Classes\${EXTENSION}"
!macroend

; Sections --------------------------------------------------------------------

Section "!${APP_NAME}" SecApp
  SectionIn RO
  Call CloseRunning

  ; An update replaces the previous program files completely, so no file of
  ; an older version stays behind.
  ${If} $Updating == 1
    DetailPrint "Removing the previous version..."
    !insertmacro REMOVE_PROGRAM_FILES
  ${EndIf}

  SetOutPath "$INSTDIR"
  File /r "${BUNDLE_DIR}\*.*"

  CreateDirectory "$INSTDIR\cmd"
  FileOpen $0 "$INSTDIR\cmd\pdfv.cmd" w
  FileWrite $0 '@"%~dp0..\bin\${EXE}" %*$\r$\n'
  FileClose $0

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Remembered for updates and the uninstaller.
  WriteRegStr SHCTX "${APP_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${APP_KEY}" "InstallMode" "$MultiUser.InstallMode"
  WriteRegStr SHCTX "${APP_KEY}" "Version" "${VERSION}"

  ; Start by name from Run and the Start menu search.
  WriteRegStr SHCTX "${APP_PATHS_KEY}" "" "$INSTDIR\bin\${EXE}"
  WriteRegStr SHCTX "${APP_PATHS_KEY}" "Path" "$INSTDIR\bin"

  CreateShortCut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\bin\${EXE}" "" \
    "$INSTDIR\bin\${EXE}" 0 "" "" "View PDFs and edit Markdown notes"

  ; Apps & features.
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\bin\${EXE},0"
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "URLInfoAbout" "${WEBSITE}"
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "UninstallString" \
    '"$INSTDIR\uninstall.exe" /$MultiUser.InstallMode'
  WriteRegStr SHCTX "${UNINSTALL_KEY}" "QuietUninstallString" \
    '"$INSTDIR\uninstall.exe" /$MultiUser.InstallMode /S'
  WriteRegDWORD SHCTX "${UNINSTALL_KEY}" "EstimatedSize" $0
  WriteRegDWORD SHCTX "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD SHCTX "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Open PDF and Markdown files with Phi" SecTypes
  !insertmacro REGISTER_TYPE "Phi.Pdf" "PDF Document"
  !insertmacro REGISTER_TYPE "Phi.Markdown" "Markdown Document"
  !insertmacro REGISTER_EXTENSION ".pdf" "Phi.Pdf"
  !insertmacro REGISTER_EXTENSION ".md" "Phi.Markdown"
  !insertmacro REGISTER_EXTENSION ".markdown" "Phi.Markdown"
  !insertmacro CLAIM_EXTENSION ".md" "Phi.Markdown"
  !insertmacro CLAIM_EXTENSION ".markdown" "Phi.Markdown"
  WriteRegStr SHCTX "Software\Classes\Applications\${EXE}" "FriendlyAppName" "${APP_NAME}"
  WriteRegStr SHCTX "Software\Classes\Applications\${EXE}\DefaultIcon" "" "$INSTDIR\bin\${EXE},0"
  WriteRegStr SHCTX "Software\Classes\Applications\${EXE}\shell\open\command" "" '"$INSTDIR\bin\${EXE}" "%1"'
  ; Offered under Settings > Apps > Default apps.
  WriteRegStr SHCTX "${APP_KEY}\Capabilities" "ApplicationName" "${APP_NAME}"
  WriteRegStr SHCTX "${APP_KEY}\Capabilities" "ApplicationDescription" "View PDFs and edit Markdown notes"
  WriteRegStr SHCTX "${APP_KEY}\Capabilities" "ApplicationIcon" "$INSTDIR\bin\${EXE},0"
  WriteRegStr SHCTX "Software\RegisteredApplications" "${APP_NAME}" "${APP_KEY}\Capabilities"
  WriteRegStr SHCTX "${APP_KEY}" "FileTypes" 1
  ; SHCNE_ASSOCCHANGED
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Add pdfv to the command line (PATH)" SecPath
  Call EditPath+
  WriteRegStr SHCTX "${APP_KEY}" "Path" 1
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\bin\${EXE}" "" \
    "$INSTDIR\bin\${EXE}" 0
  WriteRegStr SHCTX "${APP_KEY}" "DesktopShortcut" 1
SectionEnd

; The Markdown editor needs the Microsoft Edge WebView2 Runtime, which comes
; with Windows 11 and updated Windows 10 systems.
Section "-WebView2"
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${EndIf}
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${EndIf}
  ${If} $0 != ""
  ${AndIf} $0 != "0.0.0.0"
    DetailPrint "Microsoft Edge WebView2 Runtime $0 is installed."
    Return
  ${EndIf}
!ifdef WEBVIEW2_BOOTSTRAPPER
  DetailPrint "Installing the Microsoft Edge WebView2 Runtime..."
  SetOutPath "$PLUGINSDIR"
  File "/oname=MicrosoftEdgeWebview2Setup.exe" "${WEBVIEW2_BOOTSTRAPPER}"
  ExecWait '"$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" /silent /install' $0
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION \
      "The Microsoft Edge WebView2 Runtime could not be installed ($0). \
       Markdown notes need it; install it from Microsoft to use them." /SD IDOK
  ${EndIf}
!else
  MessageBox MB_OK|MB_ICONEXCLAMATION \
    "Markdown notes need the Microsoft Edge WebView2 Runtime. Install it \
     from Microsoft to use them." /SD IDOK
!endif
SectionEnd

LangString DESC_App ${LANG_ENGLISH} "The viewer and everything it needs."
LangString DESC_Types ${LANG_ENGLISH} "Offer Phi for PDF and Markdown files, and open Markdown files with it unless another program already does."
LangString DESC_Path ${LANG_ENGLISH} "Run pdfv from Command Prompt and PowerShell."
LangString DESC_Desktop ${LANG_ENGLISH} "Put a shortcut on the desktop."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} $(DESC_App)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecTypes} $(DESC_Types)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_Path)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_Desktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  ${IfNot} ${RunningX64}
  ${OrIfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} needs 64-bit Windows 10 or newer." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  !insertmacro MULTIUSER_INIT

  ; An existing installation is updated in place, keeping the components
  ; that were chosen for it.
  StrCpy $Updating 0
  ReadRegStr $0 SHCTX "${APP_KEY}" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\bin\${EXE}"
    StrCpy $Updating 1
    StrCpy $INSTDIR $0
    ReadRegStr $0 SHCTX "${APP_KEY}" "FileTypes"
    ${If} $0 != 1
      !insertmacro UnselectSection ${SecTypes}
    ${EndIf}
    ReadRegStr $0 SHCTX "${APP_KEY}" "Path"
    ${If} $0 != 1
      !insertmacro UnselectSection ${SecPath}
    ${EndIf}
    ReadRegStr $0 SHCTX "${APP_KEY}" "DesktopShortcut"
    ${If} $0 == 1
      !insertmacro SelectSection ${SecDesktop}
    ${EndIf}
  ${EndIf}
FunctionEnd

; Starts Phi through Explorer, so it runs as the signed-in user rather than
; with the installer's administrator rights, which would block dropping
; files onto it.
Function StartApp
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\bin\${EXE}"'
FunctionEnd

; An update keeps the install mode and folder of the installation.
Function SkipDirectoryOnUpdate
  ${If} $Updating == 1
    Abort
  ${EndIf}
FunctionEnd

; Uninstaller -----------------------------------------------------------------

Section "un.${APP_NAME}" UnSecApp
  SectionIn RO
  Call un.CloseRunning

  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  ReadRegStr $0 SHCTX "${APP_KEY}" "Path"
  ${If} $0 == 1
    Call un.EditPath-
  ${EndIf}

  !insertmacro UNREGISTER_EXTENSION ".pdf" "Phi.Pdf"
  !insertmacro UNREGISTER_EXTENSION ".md" "Phi.Markdown"
  !insertmacro UNREGISTER_EXTENSION ".markdown" "Phi.Markdown"
  DeleteRegKey SHCTX "Software\Classes\Phi.Pdf"
  DeleteRegKey SHCTX "Software\Classes\Phi.Markdown"
  DeleteRegKey SHCTX "Software\Classes\Applications\${EXE}"
  DeleteRegValue SHCTX "Software\RegisteredApplications" "${APP_NAME}"
  DeleteRegKey SHCTX "${APP_PATHS_KEY}"
  DeleteRegKey SHCTX "${UNINSTALL_KEY}"
  DeleteRegKey SHCTX "${APP_KEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  !insertmacro REMOVE_PROGRAM_FILES
  RMDir "$INSTDIR"
SectionEnd

; Settings, recent documents and the web view's cache, as on Linux in
; ~/.config, ~/.local/state and ~/.local/share. They belong to the user who
; runs the uninstaller.
Section /o "un.Settings and recent documents" UnSecData
  SetShellVarContext current
  RMDir /r "$LOCALAPPDATA\phi-pdf-viewer"
SectionEnd

LangString DESC_UnApp ${LANG_ENGLISH} "Remove ${APP_NAME}."
LangString DESC_UnData ${LANG_ENGLISH} "Also remove your settings, recently opened documents and cached data."

!insertmacro MUI_UNFUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${UnSecApp} $(DESC_UnApp)
  !insertmacro MUI_DESCRIPTION_TEXT ${UnSecData} $(DESC_UnData)
!insertmacro MUI_UNFUNCTION_DESCRIPTION_END

Function un.onInit
  SetRegView 64
  !insertmacro MULTIUSER_UNINIT
FunctionEnd
