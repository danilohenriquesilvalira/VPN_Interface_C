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
VIProductVersion  "1.3.0.0"
VIAddVersionKey   "ProductName"      "RLS Automacao VPN"
VIAddVersionKey   "CompanyName"      "RLS Automacao"
VIAddVersionKey   "FileDescription"  "RLS Automacao VPN Installer"
VIAddVersionKey   "FileVersion"      "1.3.0.0"
VIAddVersionKey   "LegalCopyright"   "Copyright 2026 RLS Automacao"

;--- MUI Settings ---
!define MUI_ABORTWARNING
!define MUI_ICON                     "src\icon.ico"
!define MUI_UNICON                   "src\icon.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP       "src\icon.ico"
!define MUI_HEADERIMAGE_RIGHT

;--- Texts ---
!define MUI_WELCOMEPAGE_TITLE        "Bem-vindo ao RLS Automacao VPN"
!define MUI_WELCOMEPAGE_TEXT         "Este assistente guidara a instalacao do RLS Automacao VPN v1.3.0.$\r$\n$\r$\nO SoftEther VPN Client sera instalado automaticamente durante este processo.$\r$\n$\r$\nClique em Instalar para continuar."
!define MUI_FINISHPAGE_TITLE         "Instalacao concluida"
!define MUI_FINISHPAGE_TEXT          "O RLS Automacao VPN foi instalado com sucesso.$\r$\n$\r$\nClique em Concluir para sair."
!define MUI_FINISHPAGE_RUN           "$INSTDIR\rls_vpn.exe"
!define MUI_FINISHPAGE_RUN_TEXT      "Iniciar RLS Automacao VPN"

;--- Pages ---
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

  ; WebView2 bootstrap DLL (required for WebView2 interface)
  IfFileExists "WebView2Loader.dll" 0 wv2_skip
    File "WebView2Loader.dll"
  wv2_skip:

  ; SoftEther installer — copia e executa silenciosamente
  IfFileExists "se_client.exe" 0 se_skip
    File "se_client.exe"
    DetailPrint "Instalando SoftEther VPN Client..."
    ExecWait '"$INSTDIR\se_client.exe" /SILENT /NORESTART'
    DetailPrint "SoftEther VPN Client instalado."
  se_skip:

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
    "DisplayVersion" "1.3.0"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "Publisher" "RLS Automacao"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "DisplayIcon" "$INSTDIR\rls_vpn.exe"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "URLInfoAbout" "https://github.com/danilohenriquesilvalira/VPN_Interface_C"
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
  CreateShortCut "$SMPROGRAMS\RLS Automacao\RLS Automacao VPN.lnk" \
    "$INSTDIR\rls_vpn.exe" "" "$INSTDIR\rls_vpn.exe" 0
  CreateShortCut "$DESKTOP\RLS Automacao VPN.lnk" \
    "$INSTDIR\rls_vpn.exe" "" "$INSTDIR\rls_vpn.exe" 0
SectionEnd

;--- Uninstall Section ---
Section "Uninstall"
  ; Para servico VPN antes de desinstalar
  ExecWait 'sc.exe stop SevpnClient'

  Delete "$INSTDIR\rls_vpn.exe"
  Delete "$INSTDIR\WebView2Loader.dll"
  Delete "$INSTDIR\se_client.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir  "$INSTDIR"
  RMDir  "$PROGRAMFILES64\RLS Automacao"

  Delete "$SMPROGRAMS\RLS Automacao\RLS Automacao VPN.lnk"
  RMDir  "$SMPROGRAMS\RLS Automacao"
  Delete "$DESKTOP\RLS Automacao VPN.lnk"

  DeleteRegKey HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN"
  DeleteRegKey HKLM "Software\RLS Automacao VPN"
SectionEnd
