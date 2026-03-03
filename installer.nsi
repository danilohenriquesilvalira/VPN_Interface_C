; installer.nsi - RLS Automacao VPN - NSIS Installer Script
; Requires NSIS 3.x (https://nsis.sourceforge.io)
; Build: makensis installer.nsi

!include "MUI2.nsh"

;--- General ---
Name               "RLS Automacao VPN"
OutFile            "RLS_VPN_Setup.exe"
InstallDir         "$PROGRAMFILES64\RLS Automacao\VPN"
InstallDirRegKey   HKLM "Software\RLS Automacao VPN" "Install_Dir"
RequestExecutionLevel admin
Unicode True

;--- Version info ---
VIProductVersion  "1.0.0.0"
VIAddVersionKey   "ProductName"      "RLS Automacao VPN"
VIAddVersionKey   "CompanyName"      "RLS Automacao"
VIAddVersionKey   "FileDescription"  "RLS Automacao VPN Installer"
VIAddVersionKey   "FileVersion"      "1.0.0.0"
VIAddVersionKey   "LegalCopyright"   "Copyright 2024 RLS Automacao"

;--- MUI Pages ---
!define MUI_ABORTWARNING
!define MUI_ICON   "src\icon.ico"
!define MUI_UNICON "src\icon.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "PortugueseBR"

;--- Install Section ---
Section "Principal" SecMain
  SetOutPath "$INSTDIR"

  ; Main executable
  File "rls_vpn.exe"

  ; SoftEther installer (if present in build directory)
  IfFileExists "se_client.exe" 0 +2
    File "se_client.exe"

  ; Registry entries
  WriteRegStr HKLM "Software\RLS Automacao VPN" "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "DisplayName" "RLS Automacao VPN"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "DisplayVersion" "1.0"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "Publisher" "RLS Automacao"
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "NoModify" 1
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "NoRepair" 1

  ; Uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Shortcuts
  CreateDirectory "$SMPROGRAMS\RLS Automacao"
  CreateShortcut "$SMPROGRAMS\RLS Automacao\VPN.lnk"    "$INSTDIR\rls_vpn.exe"
  CreateShortcut "$DESKTOP\RLS Automacao VPN.lnk"       "$INSTDIR\rls_vpn.exe"
SectionEnd

;--- Uninstall Section ---
Section "Uninstall"
  ; Stop VPN service before uninstalling
  ExecWait 'sc.exe stop SevpnClient'

  Delete "$INSTDIR\rls_vpn.exe"
  Delete "$INSTDIR\se_client.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir  "$INSTDIR"

  Delete "$SMPROGRAMS\RLS Automacao\VPN.lnk"
  RMDir  "$SMPROGRAMS\RLS Automacao"
  Delete "$DESKTOP\RLS Automacao VPN.lnk"

  DeleteRegKey HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN"
  DeleteRegKey HKLM "Software\RLS Automacao VPN"
SectionEnd
