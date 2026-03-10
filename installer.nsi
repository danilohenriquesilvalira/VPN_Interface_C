; installer.nsi - RLS Automacao VPN - NSIS Installer Script
; Requires NSIS 3.x (https://nsis.sourceforge.io)
; Build: makensis installer.nsi
;
; UPGRADE SUPPORT:
;   - Detecta versao anterior instalada via registo
;   - Fecha o processo em execucao antes de sobrescrever
;   - Nao reinstala o SoftEther se ja estiver presente
;   - Preserva perfis guardados em %APPDATA%

!include "MUI2.nsh"

;--- Versao centralizada (alterada pelo CI) ---
!define APP_VERSION      "2.0.0"
!define APP_VERSION_FULL "2.0.0.0"

;--- General ---
Name               "RLS Automacao VPN"
OutFile            "RLS_VPN_Setup.exe"
InstallDir         "$PROGRAMFILES64\RLS Automacao\VPN"
InstallDirRegKey   HKLM "Software\RLS Automacao VPN" "Install_Dir"
RequestExecutionLevel admin
Unicode True

;--- Version info ---
VIProductVersion  "${APP_VERSION_FULL}"
VIAddVersionKey   "ProductName"      "RLS Automacao VPN"
VIAddVersionKey   "CompanyName"      "RLS Automacao"
VIAddVersionKey   "FileDescription"  "RLS Automacao VPN Installer"
VIAddVersionKey   "FileVersion"      "${APP_VERSION_FULL}"
VIAddVersionKey   "LegalCopyright"   "Copyright 2026 RLS Automacao"

;--- MUI Settings ---
!define MUI_ABORTWARNING
!define MUI_ICON                     "src\icon.ico"
!define MUI_UNICON                   "src\icon.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP       "src\banner.bmp"
!define MUI_HEADERIMAGE_RIGHT

;--- Textos (detecta se e upgrade ou instalacao nova) ---
!define MUI_WELCOMEPAGE_TITLE        "Bem-vindo ao RLS Automacao VPN ${APP_VERSION}"
!define MUI_WELCOMEPAGE_TEXT         "Este assistente instalara (ou actualizara) o RLS Automacao VPN v${APP_VERSION}.$\r$\n$\r$\nSe ja tiver uma versao anterior instalada, ela sera actualizada automaticamente sem perder as suas configuracoes.$\r$\n$\r$\nClique em Instalar para continuar."
!define MUI_FINISHPAGE_TITLE         "Instalacao/Actualizacao concluida"
!define MUI_FINISHPAGE_TEXT          "RLS Automacao VPN v${APP_VERSION} foi instalado com sucesso.$\r$\n$\r$\nClique em Concluir para sair."
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

  ; ----------------------------------------------------------------
  ; UPGRADE: fechar o processo em execucao antes de sobrescrever
  ; ----------------------------------------------------------------
  DetailPrint "A verificar se existe versao anterior em execucao..."
  FindWindow $0 "" "RLS Automacao VPN"
  IntCmp $0 0 no_running_win
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 1200
  no_running_win:
  ; Matar processo residual (seguranca)
  ExecWait 'taskkill /F /IM rls_vpn.exe' $1

  ; ----------------------------------------------------------------
  ; UPGRADE: usar dir de instalacao anterior se existir
  ; ----------------------------------------------------------------
  ReadRegStr $R0 HKLM "Software\RLS Automacao VPN" "Install_Dir"
  StrCmp $R0 "" use_default_dir
    StrCpy $INSTDIR $R0
  use_default_dir:

  SetOutPath "$INSTDIR"

  ; Executavel principal (sempre substituido)
  File "rls_vpn.exe"

  ; WebView2 bootstrap DLL
  IfFileExists "WebView2Loader.dll" 0 wv2_skip
    File "WebView2Loader.dll"
  wv2_skip:

  ; ----------------------------------------------------------------
  ; SoftEther: so instala se NAO estiver ja instalado
  ; ----------------------------------------------------------------
  IfFileExists "se_client.exe" 0 se_skip
    ReadRegStr $R1 HKLM "SOFTWARE\SoftEther VPN Client" "ExeFile"
    StrCmp $R1 "" se_install se_already
    se_install:
      File "se_client.exe"
      DetailPrint "Instalando SoftEther VPN Client..."
      ExecWait '"$INSTDIR\se_client.exe" /SILENT /NORESTART'
      DetailPrint "SoftEther VPN Client instalado."
      Goto se_skip
    se_already:
      DetailPrint "SoftEther VPN Client ja instalado — a ignorar."
  se_skip:

  ; ----------------------------------------------------------------
  ; Registry entries (versao actualizada)
  ; ----------------------------------------------------------------
  WriteRegStr HKLM "Software\RLS Automacao VPN" "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM "Software\RLS Automacao VPN" "Version"     "${APP_VERSION}"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "DisplayName" "RLS Automacao VPN"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "Publisher" "RLS Automacao"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "DisplayIcon" "$INSTDIR\rls_vpn.exe"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "URLInfoAbout" "https://github.com/danilohenriquesilvalira/VPN_Interface_C"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "HelpLink" "https://github.com/danilohenriquesilvalira/VPN_Interface_C/releases"
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "NoModify" 1
  WriteRegDWORD HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "NoRepair" 1

  ; Uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Shortcuts (idempotente — sobrescreve se ja existirem)
  CreateDirectory "$SMPROGRAMS\RLS Automacao"
  CreateShortCut "$SMPROGRAMS\RLS Automacao\RLS Automacao VPN.lnk" \
    "$INSTDIR\rls_vpn.exe" "" "$INSTDIR\rls_vpn.exe" 0
  CreateShortCut "$DESKTOP\RLS Automacao VPN.lnk" \
    "$INSTDIR\rls_vpn.exe" "" "$INSTDIR\rls_vpn.exe" 0

  DetailPrint "RLS Automacao VPN v${APP_VERSION} instalado/actualizado com sucesso."
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
