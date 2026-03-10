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

;--- Funcao: procura em TODOS os registos de desinstalacao pelo DisplayName ---
; Percorre HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\*
; e HKLM\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*
; Quando encontra DisplayName = "RLS Automacao VPN" le o InstallLocation.
; Resultado em $9 (caminho) ou vazio se nao encontrou.
Function FindInstallDir
  Push $0   ; handle enum
  Push $1   ; subkey name
  Push $2   ; display name
  Push $3   ; install location
  Push $4   ; index
  Push $5   ; root key loop

  StrCpy $9 ""   ; resultado global

  ; Testar os dois ramos do registo (64-bit e 32-bit/WOW)
  StrCpy $5 0
  loop_roots:
    IntCmp $5 2 done_roots done_roots 0

    IntCmp $5 0 0 try_wow
      StrCpy $0 "Software\Microsoft\Windows\CurrentVersion\Uninstall"
      Goto do_enum
    try_wow:
      StrCpy $0 "Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"

    do_enum:
      StrCpy $4 0
      enum_loop:
        EnumRegKey $1 HKLM "$0" $4
        StrCmp $1 "" next_root 0
        IntOp $4 $4 + 1

        ReadRegStr $2 HKLM "$0\$1" "DisplayName"
        ; Aceita "RLS Automacao VPN" com ou sem versao no nome
        StrCpy $3 $2 18   ; primeiros 18 chars
        StrCmp $3 "RLS Automacao VPN" found_display enum_loop

      found_display:
        ; Tentar InstallLocation primeiro
        ReadRegStr $3 HKLM "$0\$1" "InstallLocation"
        StrCmp $3 "" try_uninstall_path 0
        ; Verificar que o exe existe nesse caminho
        IfFileExists "$3\rls_vpn.exe" found_it try_uninstall_path

      try_uninstall_path:
        ; InstallLocation vazio ou invalido - extrair do UninstallString
        ; Ex: "MsiExec.exe /X{GUID}" nao tem caminho util, ignorar
        ; Tentar a chave customizada do NSIS
        ReadRegStr $3 HKLM "Software\RLS Automacao VPN" "Install_Dir"
        StrCmp $3 "" next_root 0
        IfFileExists "$3\rls_vpn.exe" found_it next_root

      found_it:
        StrCpy $9 $3
        Goto done_roots

    next_root:
      IntOp $5 $5 + 1
      Goto loop_roots

  done_roots:
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

;--- Seccao unica: substituir so o executavel ---
Section "Actualizar" SecUpgrade

  ; ------------------------------------------------------------------
  ; 1. Localizar onde esta instalado (qualquer disco, qualquer pasta)
  ; ------------------------------------------------------------------
  DetailPrint "A localizar instalacao do RLS Automacao VPN..."

  ; Tentativa 1: chave propria do nosso installer (mais rapido)
  ReadRegStr $R0 HKLM "Software\RLS Automacao VPN" "Install_Dir"
  StrCmp $R0 "" 0 verify_path
    ; Tentativa 2: procura completa em todos os registos de uninstall
    Call FindInstallDir
    StrCpy $R0 $9

  verify_path:
  ; Verificar que o exe existe no caminho encontrado
  StrCmp $R0 "" not_found 0
  IfFileExists "$R0\rls_vpn.exe" found_ok not_found

  not_found:
    ; Ultima hipotese: pedir ao utilizador para indicar a pasta
    MessageBox MB_YESNO|MB_ICONQUESTION \
      "Nao foi possivel localizar automaticamente o RLS Automacao VPN.$\r$\n$\r$\nDeseja indicar manualmente a pasta de instalacao?" \
      IDYES browse_folder IDNO abort_install
    browse_folder:
      nsDialogs::SelectFolderDialog "Seleccione a pasta onde esta instalado o RLS Automacao VPN" "$PROGRAMFILES64"
      Pop $R0
      StrCmp $R0 "error" abort_install 0
      IfFileExists "$R0\rls_vpn.exe" found_ok abort_install
    abort_install:
      MessageBox MB_ICONSTOP|MB_OK \
        "RLS Automacao VPN nao foi encontrado.$\r$\nPara instalar pela primeira vez use o RLS_VPN_Setup.exe."
      Abort

  found_ok:
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
