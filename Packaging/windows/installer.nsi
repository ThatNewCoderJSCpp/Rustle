Unicode true
!include "MUI2.nsh"

!define APP_NAME "Rustle"
!define APP_EXE "rustle.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Rustle"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUTFILE}"
!if "${ARCH}" == "x86"
InstallDir "$PROGRAMFILES\${APP_NAME}"
!else
InstallDir "$PROGRAMFILES64\${APP_NAME}"
!endif
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APP_NAME} ${VERSION}"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} ${VERSION} installer"
VIAddVersionKey "CompanyName" "ThatNewCoderJSCpp"
VIAddVersionKey "LegalCopyright" "ThatNewCoderJSCpp"

Function .onInit
!if "${ARCH}" != "x86"
    SetRegView 64
!endif
    SetShellVarContext all
FunctionEnd

Function un.onInit
!if "${ARCH}" != "x86"
    SetRegView 64
!endif
    SetShellVarContext all
FunctionEnd

Section "Install"
    SetOutPath "$INSTDIR"
    File /r "${SOURCE}/*"
    File "/oname=$INSTDIR\${APP_NAME}.ico" "${ICON}"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_NAME}.ico"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_NAME}.ico"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\${APP_EXE}" "" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\${APP_EXE}" "Path" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "ThatNewCoderJSCpp"
    WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/ThatNewCoderJSCpp/${APP_NAME}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_NAME}.ico"
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    RMDir /r "$SMPROGRAMS\${APP_NAME}"
    !include "${FILELIST}"
    Delete "$INSTDIR\${APP_NAME}.ico"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\${APP_EXE}"
    DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
