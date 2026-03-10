; installer_upgrade.nsi - RLS Automacao VPN - Actualizacao/Upgrade
; Requires NSIS 3.x
;
; Este installer e APENAS para actualizacao.
; - Verifica que a versao base ja esta instalada
; - Fecha o processo em execucao
; - Substitui APENAS o rls_vpn.exe
; - Nao toca no SoftEther, nao toca nos perfis do utilizador
; - Interface minima: so uma barra de progresso
;
; Para instalacao nova use RLS_VPN_Setup.exe

!include "MUI2.nsh"

;--- Versao (sobrescrita pelo CI: /DAPP_VERSION=x.y.z) ---
!ifndef APP_VERSION
  !define APP_VERSION      "2.0.0"
!endif
!ifndef APP_VERSION_FULL
  !define APP_VERSION_FULL "2.0.0.0"
!endif

;--- Geral ---
Name               "RLS Automacao VPN - Actualizacao ${APP_VERSION}"
OutFile            "RLS_VPN_Upgrade.exe"
RequestExecutionLevel admin
Unicode True
ShowInstDetails    show

;--- Interface minima: sem paginas de wizard ---
!define MUI_ICON "src\icon.ico"

VIProductVersion  "${APP_VERSION_FULL}"
VIAddVersionKey   "ProductName"      "RLS Automacao VPN Upgrade"
VIAddVersionKey   "CompanyName"      "RLS Automacao"
VIAddVersionKey   "FileDescription"  "RLS Automacao VPN - Actualizacao ${APP_VERSION}"
VIAddVersionKey   "FileVersion"      "${APP_VERSION_FULL}"
VIAddVersionKey   "LegalCopyright"   "Copyright 2026 RLS Automacao"

;--- SEM paginas de wizard ---
!insertmacro MUI_PAGE_INSTFILES

!insertmacro MUI_LANGUAGE "PortugueseBR"

;--- Seccao unica: substituir so o executavel ---
Section "Actualizar" SecUpgrade

  ; ------------------------------------------------------------------
  ; 1. Verificar que a versao base esta instalada
  ; ------------------------------------------------------------------
  ReadRegStr $R0 HKLM "Software\RLS Automacao VPN" "Install_Dir"
  StrCmp $R0 "" not_installed installed

  not_installed:
    ; Chave NSIS nao encontrada - tentar chave do MSI (WiX)
    ReadRegStr $R1 HKLM \
      "Software\Microsoft\Windows\CurrentVersion\Uninstall\{A94F5158-83D4-432D-8949-809CA95F55D5}" \
      "InstallLocation"
    StrCmp $R1 "" try_wix2 found_wix
    found_wix:
      StrCpy $INSTDIR $R1
      Goto installed
    try_wix2:
      ; InstallLocation vazio no WiX - procurar pelo caminho padrao conhecido
      IfFileExists "$PROGRAMFILES64\RLS Automacao VPN\rls_vpn.exe" found_default 0
      IfFileExists "$PROGRAMFILES\RLS Automacao VPN\rls_vpn.exe" found_default32 not_found_anywhere
    found_default:
      StrCpy $INSTDIR "$PROGRAMFILES64\RLS Automacao VPN"
      Goto installed
    found_default32:
      StrCpy $INSTDIR "$PROGRAMFILES\RLS Automacao VPN"
      Goto installed
    not_found_anywhere:
      MessageBox MB_ICONSTOP|MB_OK \
        "RLS Automacao VPN nao esta instalado neste computador.$\r$\n$\r$\nPor favor instale primeiro o RLS_VPN_Setup.exe (instalacao nova).$\r$\n$\r$\nDescarregue em: https://github.com/danilohenriquesilvalira/VPN_Interface_C/releases"
      Abort

  installed:
    StrCpy $INSTDIR $R0
    DetailPrint "Instalacao encontrada em: $INSTDIR"

  ; ------------------------------------------------------------------
  ; 2. Verificar que o novo exe existe no pacote
  ; ------------------------------------------------------------------
  IfFileExists "$EXEDIR\rls_vpn.exe" exe_ok no_exe
  no_exe:
    ; O exe pode estar na pasta do proprio installer se foi descompactado
    IfFileExists "rls_vpn.exe" exe_ok2 exe_missing
    exe_ok2:
      Goto exe_found
    exe_missing:
      MessageBox MB_ICONSTOP|MB_OK \
        "Ficheiro rls_vpn.exe nao encontrado.$\r$\nExecute este installer a partir da pasta onde descompactou o pacote de actualizacao."
      Abort
  exe_ok:
    Goto exe_found
  exe_found:

  ; ------------------------------------------------------------------
  ; 3. Fechar o processo em execucao (silenciosamente)
  ; ------------------------------------------------------------------
  DetailPrint "A fechar RLS Automacao VPN se estiver em execucao..."
  FindWindow $0 "" "RLS Automacao VPN"
  IntCmp $0 0 kill_fallback
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 1500
  kill_fallback:
  ExecWait 'taskkill /F /IM rls_vpn.exe' $1
  Sleep 500

  ; ------------------------------------------------------------------
  ; 4. Substituir APENAS o executavel principal
  ; ------------------------------------------------------------------
  SetOutPath "$INSTDIR"
  DetailPrint "A actualizar rls_vpn.exe para v${APP_VERSION}..."
  File "rls_vpn.exe"

  ; ------------------------------------------------------------------
  ; 5. Actualizar versao no registo
  ; ------------------------------------------------------------------
  WriteRegStr HKLM "Software\RLS Automacao VPN" "Version" "${APP_VERSION}"
  WriteRegStr HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLSAutomacaoVPN" \
    "DisplayVersion" "${APP_VERSION}"

  DetailPrint "Actualizacao para v${APP_VERSION} concluida com sucesso!"

  ; ------------------------------------------------------------------
  ; 6. Perguntar se quer iniciar a aplicacao
  ; ------------------------------------------------------------------
  MessageBox MB_YESNO|MB_ICONINFORMATION \
    "RLS Automacao VPN foi actualizado para v${APP_VERSION}.$\r$\n$\r$\nDeseja iniciar agora?" \
    IDYES launch IDNO done
  launch:
    Exec '"$INSTDIR\rls_vpn.exe"'
  done:

SectionEnd
