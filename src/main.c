/*
 * main.c - RLS Automacao VPN Client v1.5.4
 * Interface estilo SoftEther: menu bar, ListViews, status bar, toolbar.
 * Polling de estado em thread dedicada - UI thread NUNCA bloqueia.
 *
 * v1.5.1: fontes maiores, NM_CUSTOMDRAW colors, toolbar maior
 * v1.5.2: remove strip panel, state label simples colorido,
 *         pre-popular ListView no arranque, config dialog maior
 * v1.5.4: fix NicCreate Error 32 - remove adaptador TAP orfao do Windows
 *         via PowerShell + restart servico antes de retentar NicCreate
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vpn.h"

/* ---- Menu IDs ----------------------------------------------------------- */
#define IDM_CONNECT    1001
#define IDM_DISCONNECT 1002
#define IDM_SETUP      1003
#define IDM_INSTALL_SE 1004
#define IDM_EXIT       1005
#define IDM_RESET      1006
#define IDM_SAVE_CFG   1008
#define IDM_SHOW_LOG   1009
#define IDM_ABOUT      1010
#define IDM_APPLYIP    1011
#define IDM_TOGGLE     1012   /* single Ligar/Desligar toggle button */
#define IDM_SCAN       1013   /* scan de rede VPN */

/* ---- Control IDs -------------------------------------------------------- */
#define IDC_LIST_CONNS    200
#define IDC_LIST_ADAPTERS 201
#define IDC_LOG           202
#define IDC_STATUSBAR     203
#define IDC_TOOLBAR       204

/* ---- Scan window control IDs -------------------------------------------- */
#define IDC_SCAN_LIST   600
#define IDC_SCAN_PROG   601
#define IDC_SCAN_START  602
#define IDC_SCAN_STOP   603
#define IDC_SCAN_CLOSE  604
#define IDC_SCAN_BASE   605
#define IDC_SCAN_LABEL  606

/* ---- Scan window messages ------------------------------------------------ */
#define WM_SCAN_RESULT   (WM_APP+4)   /* lp = ScanResult* (heap, caller frees) */
#define WM_SCAN_DONE     (WM_APP+5)   /* scan concluido */
#define WM_SCAN_PROGRESS (WM_APP+6)   /* wp = 1..254 */
/* ---- Layout constants --------------------------------------------------- */
#define TB_H   48   /* toolbar height px */
#define LOG_H  150  /* log panel height px */
#define BTN_W  118  /* toolbar button width */
#define BTN_H  36   /* toolbar button height */
#define BTN_TOGGLE_W 140 /* toggle button wider than regular buttons */

/* ---- Config dialog IDs -------------------------------------------------- */
#define IDC_CFG_HOST 500
#define IDC_CFG_PORT 501
#define IDC_CFG_HUB  502
#define IDC_CFG_USER 503
#define IDC_CFG_PASS 504
#define IDC_CFG_IPST 505
#define IDC_CFG_MASK 506

/* ---- App messages ------------------------------------------------------- */
#define WM_VPN_LOG    (WM_APP+1)
#define WM_VPN_RESULT (WM_APP+2)
#define WM_VPN_STATUS (WM_APP+3)

/* ---- Globals ------------------------------------------------------------ */
static HWND     g_hwnd       = NULL;
static HWND     g_list_conns = NULL;
static HWND     g_list_adaps = NULL;
static HWND     g_log        = NULL;
static HWND     g_statusbar  = NULL;
static BOOL     g_busy       = FALSE;
static VpnConfig g_cfg;
static VpnState  g_state     = VPN_NOT_INSTALLED;
static HFONT    g_font_ui    = NULL;
static HFONT    g_font_bold  = NULL;
static HFONT    g_font_mono  = NULL;
static HFONT    g_font_tb    = NULL;
static HWND     g_btn_toggle   = NULL;
static BOOL     g_connected    = FALSE;
static int      g_last_op      = 0;
static char     g_vpn_ip[64]   = {0};
static BOOL     g_se_installed = FALSE;
static HWND     g_scan_wnd     = NULL;

/* ---- Network scan types ------------------------------------------------- */
typedef struct {
    char ip[20];
    char mac[24];
    char hostname[128];
    DWORD latency_ms;
} ScanResult;
typedef struct { HWND hw; DWORD ip; int idx; } PingArg;

/* ---- Forward declarations ----------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
static LRESULT CALLBACK ToolbarWndProc(HWND,UINT,WPARAM,LPARAM);
static void LayoutChildren(HWND);

/* ---- Toolbar button definitions -----------------------------------------
   Toggle (IDM_TOGGLE): owner-drawn, green=LIGAR / red=DESLIGAR.
   Remaining buttons: regular BS_PUSHBUTTON.                                */
static const struct { int id; const char *label; } g_tbBtns[] = {
    {IDM_TOGGLE,   "LIGAR"},            /* owner-drawn toggle */
    {IDM_SAVE_CFG, "Configurar VPN"},   /* abre dialogo de config */
    {IDM_RESET,    "Reset"},
    {IDM_SCAN,     "Scan Rede"},        /* varredura de IP na rede VPN */
};
#define TB_BTN_CNT ((int)(sizeof g_tbBtns/sizeof g_tbBtns[0]))
static HWND g_toolbar = NULL;

/* ======================================================================
   Helpers
   ====================================================================== */
static HFONT MakeFont(int sz, BOOL bold, BOOL mono)
{
    return CreateFontA(sz,0,0,0, bold?FW_BOLD:FW_NORMAL,
        0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,
        mono?FIXED_PITCH:DEFAULT_PITCH,
        mono?"Consolas":"Segoe UI");
}

static void log_append(const char *msg)
{
    if (!g_log || !msg) return;
    if (GetWindowTextLengthA(g_log) > 38000) {
        SendMessageA(g_log, EM_SETSEL, 0, 6000);
        SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)"");
    }
    int len = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, len, len);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)msg);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
}

static void sb_set(int part, const char *txt)
{
    if (g_statusbar)
        SendMessageA(g_statusbar, SB_SETTEXTA, (WPARAM)part, (LPARAM)txt);
}

static const char *StateStr(VpnState s)
{
    switch(s){
        case VPN_CONNECTED:      return "Ligado";
        case VPN_CONNECTING:     return "A ligar...";
        case VPN_DISCONNECTED:   return "Desligado";
        case VPN_NOT_CONFIGURED: return "Nao configurado";
        default:                 return "Nao instalado";
    }
}

/* ======================================================================
   Worker thread (connect / disconnect / setup / install / etc.)
   ====================================================================== */
typedef struct { int op; VpnConfig cfg; char ip[64]; char mask[64]; } WorkData;

static void vpn_log_cb(const char *msg, void *ctx)
{
    (void)ctx;
    if (msg && g_hwnd)
        PostMessageA(g_hwnd, WM_VPN_LOG, 0, (LPARAM)_strdup(msg));
}

/* WorkerThread: todas as operacoes VPN correm aqui (nunca no UI thread).
   op 1 = vpn_setup    -> [1/3] servico  [2/3] placa virtual  [3/3] conta
   op 2 = vpn_connect  -> AccountConnect (apenas liga, NAO cria placa/conta)
   op 3 = vpn_disconnect -> AccountDisconnect
   op 4 = vpn_reset    -> remove conta + placa
   op 5 = instalar SE  -> instala SoftEther + chama vpn_setup
   op 6 = IP estatico  -> aplica IP na placa VPN via netsh                  */
static DWORD WINAPI WorkerThread(LPVOID p)
{
    WorkData *d = (WorkData*)p;
    int ok = 0;
    char msg[512] = {0};

    switch(d->op) {
    /* ── op 1: Criar placa virtual + conta VPN (setup completo) ── */
    case 1:
        ok = vpn_setup(&d->cfg, msg, sizeof msg);
        if (!msg[0]) strncpy(msg,
            ok ? "Conta VPN configurada com sucesso."
               : "Falha na configuracao VPN.", sizeof msg-1);
        break;

    /* ── op 2: Ligar (AccountConnect apenas) ─────────────────── */
    case 2:
        ok = vpn_connect(&d->cfg, msg, sizeof msg);
        if (!msg[0]) strncpy(msg,
            ok ? "Ligacao iniciada."
               : "Falha ao ligar.", sizeof msg-1);
        break;

    /* ── op 3: Desligar (AccountDisconnect) ──────────────────── */
    case 3:
        vpn_disconnect(&d->cfg, msg, sizeof msg);
        ok = 1;
        if (!msg[0]) strncpy(msg, "VPN desligada.", sizeof msg-1);
        break;

    /* ── op 4: Reset (remove conta + placa) ──────────────────── */
    case 4:
        vpn_reset(&d->cfg, msg, sizeof msg);
        ok = 1;
        if (!msg[0]) strncpy(msg, "Reset concluido.", sizeof msg-1);
        break;

    /* ── op 5: Instalar SoftEther + setup completo ────────────── */
    case 5: {
        char se_dir[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, se_dir, MAX_PATH);
        char *sl = strrchr(se_dir, '\\');
        if (sl) *(sl+1) = '\0';
        int r = vpn_install_silent(se_dir);
        if (!r) {
            strncpy(msg, "Falha ao instalar SoftEther VPN Client.", sizeof msg-1);
            ok = 0;
            break;
        }
        vpn_start_service();
        ok = vpn_setup(&d->cfg, msg, sizeof msg);
        if (!msg[0]) strncpy(msg,
            ok ? "SoftEther instalado e conta configurada."
               : "SoftEther instalado mas configuracao falhou.", sizeof msg-1);
        break;
    }

    /* ── op 6: Aplicar IP estatico na placa VPN ──────────────── */
    case 6:
        ok = vpn_set_static_ip(d->ip, d->mask, msg, sizeof msg);
        if (!msg[0]) strncpy(msg,
            ok ? "IP estatico aplicado."
               : "Falha ao aplicar IP.", sizeof msg-1);
        break;

    default:
        strncpy(msg, "Operacao desconhecida.", sizeof msg-1);
        break;
    }

    PostMessageA(g_hwnd, WM_VPN_RESULT, (WPARAM)ok, (LPARAM)_strdup(msg));
    free(d);
    return 0;
}

static void StartOp(int op, const char *ip, const char *mask)
{
    if (g_busy) {
        MessageBoxA(g_hwnd,"Aguardar operacao em curso.","RLS VPN",MB_ICONINFORMATION);
        return;
    }
    g_busy = TRUE;
    g_last_op = op;
    sb_set(1,"A processar...");

    WorkData *d = (WorkData*)calloc(1, sizeof(WorkData));
    if (!d) { g_busy = FALSE; return; }
    d->op  = op;
    d->cfg = g_cfg;
    if (ip)   strncpy(d->ip,   ip,   sizeof d->ip-1);
    if (mask) strncpy(d->mask, mask, sizeof d->mask-1);

    HANDLE ht = CreateThread(NULL,0,WorkerThread,d,0,NULL);
    if (ht) CloseHandle(ht); else { free(d); g_busy=FALSE; }
}

/* ======================================================================
   Status polling thread - NUNCA bloqueia o UI thread
   ====================================================================== */
static DWORD WINAPI PollThread(LPVOID p)
{
    /* Nao precisamos de g_busy guard aqui: run_vpncmd em vpn.c usa
       CRITICAL_SECTION para serializar — nunca dois vpncmd.exe em parallel. */
    BOOL first = (BOOL)(UINT_PTR)p;
    if (!first) Sleep(3000);
    for (;;) {
        if (!g_hwnd) return 0;
        VpnStatus *st = (VpnStatus*)calloc(1, sizeof(VpnStatus));
        if (st) {
            vpn_get_status(&g_cfg, st);
            if (g_hwnd) PostMessageA(g_hwnd, WM_VPN_STATUS, 0, (LPARAM)st);
            else        free(st);
        }
        Sleep(3000);
    }
}


/* ======================================================================
   Populate ListView rows (can be called before first VPN poll)
   ====================================================================== */
static void FillListViews(const VpnStatus *st)
{
    /* ── Tabela de Ligacoes VPN ─────────────────────────────────────────
       Todos os campos vêm do SoftEther (AccountStatusGet + NicList).
       Fallback para g_cfg apenas quando o SoftEther nao responde ainda. */

    /* Col 0: Nome da conta VPN */
    char acct[64];
    strncpy(acct, g_cfg.account_name[0] ? g_cfg.account_name : ACCOUNT_NAME,
            sizeof acct - 1);

    /* Col 1: Estado — "Connected"/"Offline" do AccountList (vpn_user reutilizado)
       ou status detalhado do AccountStatusGet, com fallback localizado. */
    const char *estado_conn;
    if (st && st->vpn_user[0])
        estado_conn = st->vpn_user;        /* "Connected" / "Offline" (AccountList Status) */
    else if (st && st->status_detail[0])
        estado_conn = st->status_detail;   /* "Connection Completed (...)" */
    else if (st)
        estado_conn = StateStr(st->state); /* fallback localizado */
    else
        estado_conn = "Aguardar...";

    /* Col 2: Servidor — host:porta real do SoftEther */
    const char *servidor;
    char srv_fallback[320];
    if (st && st->server_display[0])
        servidor = st->server_display;
    else {
        snprintf(srv_fallback, sizeof srv_fallback, "%s:%d",
                 g_cfg.host[0] ? g_cfg.host : DEFAULT_HOST, g_cfg.port ? g_cfg.port : DEFAULT_PORT);
        servidor = srv_fallback;
    }

    /* Col 3: Hub — real do SoftEther */
    const char *hub;
    if (st && st->hub_display[0])
        hub = st->hub_display;
    else
        hub = g_cfg.hub[0] ? g_cfg.hub : DEFAULT_HUB;

    /* Col 4: Adaptador Virtual — nome real SoftEther (NicList) */
    const char *nic_se = (st && st->nic_name[0]) ? st->nic_name : NIC_NAME;

    /* Garantir exatamente uma linha */
    while (ListView_GetItemCount(g_list_conns) > 1)
        ListView_DeleteItem(g_list_conns, 0);
    if (ListView_GetItemCount(g_list_conns) == 0) {
        LVITEMA lvi = {0};
        lvi.mask    = LVIF_TEXT;
        lvi.pszText = acct;
        ListView_InsertItem(g_list_conns, &lvi);
    }
    ListView_SetItemText(g_list_conns, 0, 0, acct);
    ListView_SetItemText(g_list_conns, 0, 1, (char*)estado_conn);
    ListView_SetItemText(g_list_conns, 0, 2, (char*)servidor);
    ListView_SetItemText(g_list_conns, 0, 3, (char*)hub);
    ListView_SetItemText(g_list_conns, 0, 4, (char*)nic_se);
    ListView_RedrawItems(g_list_conns, 0, 0);

    /* ── Tabela de Adaptadores ──────────────────────────────────────────
       Col 0: Nome Windows do adaptador (find_vpn_adapter via PowerShell)
       Col 1: MAC Address (NicList)
       Col 2: Estado (ligado/desligado)
       Col 3: Endereco IP (Get-NetIPAddress via PowerShell)              */

    const char *nic_win = (st && st->nic_windows[0]) ? st->nic_windows : "---";
    const char *nic_mac = (st && st->nic_mac[0])     ? st->nic_mac     : "---";

    /* Estado do adaptador: usa "Enabled"/"Disabled" real do NicList quando disponivel */
    const char *adap_estado;
    if (!st)
        adap_estado = "Aguardar...";
    else if (st->nic_status[0])
        adap_estado = st->nic_status;  /* "Enabled" / "Disabled" do NicList */
    else if (st->state == VPN_CONNECTED)
        adap_estado = "Enabled";
    else
        adap_estado = "Disabled";

    char ip_buf[64];
    if (st && st->local_ip[0])
        strncpy(ip_buf, st->local_ip, sizeof ip_buf - 1);
    else
        strncpy(ip_buf, "---", sizeof ip_buf - 1);

    /* SoftEther service status row */
    const char *se_state  = g_se_installed ? "Instalado" : "Nao Instalado";
    char se_label[] = "SoftEther VPN Client";
    char se_dash[]  = "---";

    /* Garantir exatamente 2 linhas */
    while (ListView_GetItemCount(g_list_adaps) > 2)
        ListView_DeleteItem(g_list_adaps, 0);
    if (ListView_GetItemCount(g_list_adaps) == 0) {
        LVITEMA lvi2 = {0}; lvi2.mask=LVIF_TEXT; lvi2.pszText=(char*)nic_win;
        ListView_InsertItem(g_list_adaps, &lvi2);
        lvi2.iItem=1; lvi2.pszText=se_label;
        ListView_InsertItem(g_list_adaps, &lvi2);
    } else if (ListView_GetItemCount(g_list_adaps) == 1) {
        LVITEMA lvi2 = {0}; lvi2.mask=LVIF_TEXT; lvi2.iItem=1; lvi2.pszText=se_label;
        ListView_InsertItem(g_list_adaps, &lvi2);
    }
    /* Row 0: NIC VPN */
    ListView_SetItemText(g_list_adaps, 0, 0, (char*)nic_win);
    ListView_SetItemText(g_list_adaps, 0, 1, (char*)nic_mac);
    ListView_SetItemText(g_list_adaps, 0, 2, (char*)adap_estado);
    ListView_SetItemText(g_list_adaps, 0, 3, ip_buf);
    /* Row 1: SoftEther VPN Client service */
    ListView_SetItemText(g_list_adaps, 1, 0, se_label);
    ListView_SetItemText(g_list_adaps, 1, 1, se_dash);
    ListView_SetItemText(g_list_adaps, 1, 2, (char*)se_state);
    ListView_SetItemText(g_list_adaps, 1, 3, se_dash);
    ListView_RedrawItems(g_list_adaps, 0, 1);
}

/* ======================================================================
   Update on VPN poll result (runs on UI thread via WM_VPN_STATUS)
   ====================================================================== */
static void UpdateUI(const VpnStatus *st)
{
    g_state = st->state;
    /* Guardar IP VPN e estado do SoftEther para uso no scan */
    if (st->local_ip[0]) strncpy(g_vpn_ip, st->local_ip, sizeof(g_vpn_ip)-1);
    g_se_installed = (BOOL)st->softether_ready;
    /* Sync g_connected from authoritative poll result.
       EXCEPÇÃO: se a última operação foi um disconnect (op=3), não deixamos
       um poll "ainda ligado" (SoftEther demora 1-2s a actualizar) flipar
       g_connected de volta para TRUE — isso causaria o botão mostrar
       "DESLIGAR" imediatamente após desligar, impedindo religar. */
    if (st->state == VPN_CONNECTED && g_last_op != 3)  g_connected = TRUE;
    else if (st->state == VPN_DISCONNECTED ||
             st->state == VPN_NOT_CONFIGURED)           g_connected = FALSE;
    /* Redraw toggle button with updated state */
    if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
    FillListViews(st);

    /* Status bar */
    char sb0[128];
    if (st->status_detail[0])
        snprintf(sb0, sizeof sb0, "  SoftEther: %s", st->status_detail);
    else
        snprintf(sb0, sizeof sb0, "  Estado: %s", StateStr(st->state));
    sb_set(0, sb0);
    if (!g_busy) sb_set(1, st->message[0] ? st->message : "  Pronto");
    char sb2[128];
    snprintf(sb2, sizeof sb2, "  IP VPN: %s  |  MAC: %s",
        st->local_ip[0]  ? st->local_ip  : "---",
        st->nic_mac[0]   ? st->nic_mac   : "---");
    sb_set(2, sb2);
}

/* ======================================================================
   Dialogo de senha para o botao Reset
   ====================================================================== */
#define IDC_PWD_EDIT  700
#define IDC_PWD_OK    701
#define IDC_PWD_CANCEL 702
static BOOL g_pwd_ok = FALSE;

static LRESULT CALLBACK PwdWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hI = GetModuleHandleA(NULL);
        HFONT f = g_font_ui;
        /* Mensagem */
        HWND lbl = CreateWindowA("STATIC",
            "Introduza a senha de administrador para continuar:",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            14, 14, 310, 32, hw, NULL, hI, NULL);
        SendMessageA(lbl, WM_SETFONT, (WPARAM)f, 0);
        /* Campo senha */
        HWND ed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,
            14, 52, 310, 26, hw, (HMENU)(UINT_PTR)IDC_PWD_EDIT, hI, NULL);
        SendMessageA(ed, WM_SETFONT, (WPARAM)f, 0);
        SetFocus(ed);
        /* Botoes */
        HWND ok = CreateWindowA("BUTTON", "OK",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
            100, 94, 100, 28, hw, (HMENU)(UINT_PTR)IDC_PWD_OK, hI, NULL);
        SendMessageA(ok, WM_SETFONT, (WPARAM)f, 0);
        HWND cn = CreateWindowA("BUTTON", "Cancelar",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
            210, 94, 100, 28, hw, (HMENU)(UINT_PTR)IDC_PWD_CANCEL, hI, NULL);
        SendMessageA(cn, WM_SETFONT, (WPARAM)f, 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_PWD_OK || (LOWORD(wp)==1 && HIWORD(wp)==BN_CLICKED)) {
            char buf[64] = {0};
            GetDlgItemTextA(hw, IDC_PWD_EDIT, buf, sizeof buf);
            if (strcmp(buf, "Rls@2024") == 0) {
                g_pwd_ok = TRUE;
                DestroyWindow(hw);
            } else {
                SetDlgItemTextA(hw, IDC_PWD_EDIT, "");
                MessageBoxA(hw, "Senha incorreta.", "Acesso negado", MB_ICONWARNING|MB_OK);
                SetFocus(GetDlgItem(hw, IDC_PWD_EDIT));
            }
        } else if (id == IDC_PWD_CANCEL) {
            g_pwd_ok = FALSE;
            DestroyWindow(hw);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_pwd_ok = FALSE; DestroyWindow(hw); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

/* Mostra dialogo de senha de forma modal. Devolve TRUE se senha correcta. */
static BOOL AskResetPassword(HWND parent)
{
    static BOOL cls_reg = FALSE;
    HINSTANCE hI = GetModuleHandleA(NULL);
    if (!cls_reg) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = PwdWndProc;
        wc.hInstance     = hI;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.lpszClassName = "RlsPwdDlg";
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        cls_reg = TRUE;
    }
    g_pwd_ok = FALSE;
    /* Centrar na janela pai */
    RECT pr; GetWindowRect(parent, &pr);
    int W = 340, H = 178;
    int x = pr.left + (pr.right - pr.left - W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - H) / 2;
    HWND hw = CreateWindowExA(
        WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,
        "RlsPwdDlg", "Verificacao de Seguranca",
        WS_POPUP|WS_CAPTION|WS_SYSMENU,
        x, y, W, H, parent, NULL, hI, NULL);
    EnableWindow(parent, FALSE);
    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);
    /* Loop modal local */
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0)) {
        if (!IsDialogMessageA(hw, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return g_pwd_ok;
}

/* ======================================================================
   Dialogo de configuracao (janela modal manual)
   ====================================================================== */
static HWND  g_cfg_wnd = NULL;

static LRESULT CALLBACK CfgWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch(msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK || id == IDM_SETUP) {
            char tmp[256];
            GetDlgItemTextA(hw, IDC_CFG_HOST, g_cfg.host, sizeof g_cfg.host);
            GetDlgItemTextA(hw, IDC_CFG_PORT, tmp, sizeof tmp);
            int p = atoi(tmp); if (p>0) g_cfg.port=(unsigned short)p;
            GetDlgItemTextA(hw, IDC_CFG_HUB,  g_cfg.hub,      sizeof g_cfg.hub);
            GetDlgItemTextA(hw, IDC_CFG_USER, g_cfg.username,  sizeof g_cfg.username);
            GetDlgItemTextA(hw, IDC_CFG_PASS, g_cfg.password,  sizeof g_cfg.password);
            log_append("[CONFIG] Configuracao guardada.");
            sb_set(1, "  Configuracao guardada.");
            FillListViews(NULL);
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(hw);
            g_cfg_wnd = NULL;
            if (id == IDM_SETUP) {
                /* Criar NIC + conta VPN no SoftEther com os novos dados */
                StartOp(1, NULL, NULL);
            }
        } else if (id == IDCANCEL) {
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(hw);
            g_cfg_wnd = NULL;
        } else if (id == IDM_APPLYIP) {
            char ip[64]={0}, mask[64]={0};
            GetDlgItemTextA(hw, IDC_CFG_IPST, ip,   sizeof ip);
            GetDlgItemTextA(hw, IDC_CFG_MASK, mask, sizeof mask);
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(hw);
            g_cfg_wnd = NULL;
            StartOp(6, ip, mask);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(hw);
            g_cfg_wnd = NULL;
        }
        return 0;
    case WM_DESTROY:
        g_cfg_wnd = NULL;
        EnableWindow(g_hwnd, TRUE);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void ShowConfigDlg(HWND parent)
{
    if (g_cfg_wnd) { SetForegroundWindow(g_cfg_wnd); return; }

    HINSTANCE hI = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    static BOOL reg = FALSE;
    if (!reg) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = CfgWndProc;
        wc.hInstance     = hI;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.lpszClassName = "RLS_CFG";
        wc.hCursor       = LoadCursorA(NULL,(LPCSTR)IDC_ARROW);
        RegisterClassA(&wc);
        reg = TRUE;
    }

    int W=620, H=500;
    RECT rc; GetWindowRect(parent,&rc);
    int X = rc.left+(rc.right-rc.left-W)/2;
    int Y = rc.top+(rc.bottom-rc.top-H)/2;

    g_cfg_wnd = CreateWindowExA(WS_EX_DLGMODALFRAME,
        "RLS_CFG", "Configuracoes VPN",
        WS_POPUP|WS_CAPTION|WS_SYSMENU,
        X,Y,W,H, parent, NULL, hI, NULL);

    /* Use the same g_font_ui used by the main window (18pt Segoe UI) */
    HFONT f = g_font_ui;
    char ps[8]; snprintf(ps,sizeof ps,"%d",g_cfg.port);

    /* Row height = 32px, label width = 120px, edit height = 28px */
    #define LBL(t,x,y,w,h) do{ HWND _h=CreateWindowA("STATIC",t,WS_CHILD|WS_VISIBLE|SS_RIGHT,x,y,w,h,g_cfg_wnd,NULL,hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)
    #define EDT(id,t,x,y,w,h,ex) do{ HWND _h=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|(ex),x,y,w,h,g_cfg_wnd,(HMENU)(UINT_PTR)(id),hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)
    #define BTN(t,id,x,y,w,h) do{ HWND _h=CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,x,y,w,h,g_cfg_wnd,(HMENU)(UINT_PTR)(id),hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)

    /* --- Ligacao VPN ------------------------------------------------- */
    LBL("Servidor:",    10,  24, 110, 26); EDT(IDC_CFG_HOST,g_cfg.host,    125, 22, 300, 30, 0);
    LBL("Porta:",      432,  24,  50, 26); EDT(IDC_CFG_PORT,ps,            486, 22,  96, 30, ES_NUMBER);
    LBL("Hub Virtual:", 10,  68, 110, 26); EDT(IDC_CFG_HUB, g_cfg.hub,     125, 66, 240, 30, 0);
    LBL("Utilizador:",  10, 112, 110, 26); EDT(IDC_CFG_USER,g_cfg.username, 125,110, 240, 30, 0);
    LBL("Password:",    10, 156, 110, 26); EDT(IDC_CFG_PASS,g_cfg.password, 125,154, 240, 30, ES_PASSWORD);

    /* --- Separador --------------------------------------------------- */
    HWND sep = CreateWindowA("STATIC","",WS_CHILD|WS_VISIBLE|SS_ETCHEDHORZ,
        10,200,580,2, g_cfg_wnd,NULL,hI,NULL); (void)sep;

    /* --- IP Estatico ------------------------------------------------- */
    LBL("IP Estatico:", 10, 212, 110, 26); EDT(IDC_CFG_IPST,"192.168.10.1",  125,210, 160, 30, 0);
    LBL("Mascara:",    294, 212,  80, 26); EDT(IDC_CFG_MASK,"255.255.255.0", 378,210, 180, 30, 0);
    BTN("Aplicar IP Estatico", IDM_APPLYIP, 10, 254, 200, 36);

    /* --- Separador --------------------------------------------------- */
    HWND sep2 = CreateWindowA("STATIC","",WS_CHILD|WS_VISIBLE|SS_ETCHEDHORZ,
        10,304,580,2, g_cfg_wnd,NULL,hI,NULL); (void)sep2;

    /* --- Acoes principais -------------------------------------------- */
    /* Guardar config + fechar */
    BTN("Guardar e Fechar", IDOK,      10, 318, 200, 40);
    /* Guardar config e criar conta VPN no SoftEther imediatamente */
    BTN("Criar Conta VPN",  IDM_SETUP, 220, 318, 190, 40);
    /* Cancelar */
    BTN("Cancelar",         IDCANCEL,  470, 318, 120, 40);

    #undef LBL
    #undef EDT
    #undef BTN

    EnableWindow(parent, FALSE);
    ShowWindow(g_cfg_wnd, SW_SHOW);
    SetFocus(g_cfg_wnd);
}

/* ======================================================================
   Network Scanner - Varredura de rede VPN
   ====================================================================== */

/* Extrai prefixo /24 de um IP: "192.168.222.253" -> "192.168.222." */
static void extract_base_ip(const char *ip, char *base, int base_len) {
    base[0] = 0;
    if (!ip || !ip[0]) return;
    const char *last = strrchr(ip, '.');
    if (!last) return;
    int len = (int)(last - ip) + 1;
    if (len >= base_len) return;
    memcpy(base, ip, len);
    base[len] = 0;
}

/* MAC via ARP */
static void scan_get_mac(DWORD destIP, char *mac_out, int mac_len) {
    ULONG mac[2] = {0};
    ULONG macLen = 6;
    if (SendARP(destIP, 0, mac, &macLen) == NO_ERROR && macLen == 6) {
        BYTE *b = (BYTE*)mac;
        snprintf(mac_out, mac_len, "%02X-%02X-%02X-%02X-%02X-%02X",
            b[0],b[1],b[2],b[3],b[4],b[5]);
    } else {
        strncpy(mac_out, "---", mac_len-1);
    }
}

/* Hostname via DNS reverso (thread-safe) */
static void scan_get_hostname(DWORD ip_n, char *host_out, int host_len) {
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip_n;
    char h[128] = {0};
    if (getnameinfo((struct sockaddr*)&sa, sizeof(sa), h, sizeof(h),
                    NULL, 0, NI_NAMEREQD) == 0 && h[0])
        strncpy(host_out, h, host_len-1);
    else
        strncpy(host_out, "---", host_len-1);
}

/* Thread que pinga 1 IP e posta resultado */
static BOOL g_scan_stop  = FALSE;
static int  g_scan_found = 0;

static DWORD WINAPI PingThread(LPVOID arg) {
    PingArg *a = (PingArg*)arg;
    HWND    hw     = a->hw;
    DWORD   destIP = a->ip;
    int     idx    = a->idx;
    free(a);

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        PostMessageA(hw, WM_SCAN_PROGRESS, (WPARAM)idx, 0);
        return 0;
    }

    char send_buf[8] = "RLSSCAN";
    BYTE recv_buf[256] = {0};
    DWORD ret = IcmpSendEcho(hIcmp, destIP, send_buf, 8,
                             NULL, recv_buf, sizeof(recv_buf), 1200);
    IcmpCloseHandle(hIcmp);

    PostMessageA(hw, WM_SCAN_PROGRESS, (WPARAM)idx, 0);

    if (ret > 0) {
        ICMP_ECHO_REPLY *reply = (ICMP_ECHO_REPLY*)recv_buf;
        ScanResult *r = (ScanResult*)calloc(1, sizeof(ScanResult));
        if (r) {
            struct in_addr in; in.s_addr = destIP;
            strncpy(r->ip, inet_ntoa(in), sizeof(r->ip)-1);
            r->latency_ms = reply->RoundTripTime;
            scan_get_mac(destIP, r->mac, sizeof(r->mac));
            scan_get_hostname(destIP, r->hostname, sizeof(r->hostname));
            PostMessageA(hw, WM_SCAN_RESULT, 0, (LPARAM)r);
        }
    }
    return 0;
}

/* Thread principal: dispara PingThreads em batches de 32 */
#define SCAN_BATCH 32
typedef struct { HWND hw; char base[32]; } ScanMgrArgs;

static DWORD WINAPI ScanMgrThread(LPVOID arg) {
    ScanMgrArgs *sa = (ScanMgrArgs*)arg;
    HWND  hw = sa->hw;
    char  base[32];
    strncpy(base, sa->base, sizeof(base)-1);
    free(sa);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    HANDLE threads[SCAN_BATCH];
    int start = 1;
    while (start <= 254 && !g_scan_stop) {
        int end = start + SCAN_BATCH - 1;
        if (end > 254) end = 254;
        int cnt = 0;

        for (int i = start; i <= end && !g_scan_stop; i++) {
            char ip[24];
            snprintf(ip, sizeof(ip), "%s%d", base, i);
            PingArg *pa = (PingArg*)calloc(1, sizeof(PingArg));
            if (!pa) continue;
            pa->hw  = hw;
            pa->ip  = inet_addr(ip);
            pa->idx = i;
            HANDLE ht = CreateThread(NULL, 0, PingThread, pa, 0, NULL);
            if (ht) threads[cnt++] = ht;
            else    free(pa);
        }

        if (cnt > 0)
            WaitForMultipleObjects(cnt, threads, TRUE, 3500);
        for (int i = 0; i < cnt; i++) CloseHandle(threads[i]);
        start = end + 1;
    }

    WSACleanup();
    PostMessageA(hw, WM_SCAN_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK ScanWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hI = ((CREATESTRUCTA*)lp)->hInstance;
        HFONT f = g_font_ui ? g_font_ui : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT fm = g_font_mono ? g_font_mono : f;
#define SLBL(t,x,y,w,h)  do{ HWND _h=CreateWindowA("STATIC",t,WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,h,hw,NULL,hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0); }while(0)
#define SBTN(t,id,x,y,w,h) do{ HWND _h=CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,x,y,w,h,hw,(HMENU)(UINT_PTR)(id),hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0); }while(0)

        SLBL("Rede VPN:", 10, 15, 88, 24);
        /* Base IP edit */
        char base[32] = {0};
        extract_base_ip(g_vpn_ip, base, sizeof(base));
        if (!base[0]) strncpy(base, "192.168.222.", sizeof(base)-1);
        HWND he = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", base,
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            102, 10, 168, 32, hw,
            (HMENU)(UINT_PTR)IDC_SCAN_BASE, hI, NULL);
        SendMessageA(he, WM_SETFONT, (WPARAM)fm, 0);
        SLBL("x.x.x.1 - 254", 276, 15, 130, 24);
        SBTN("Iniciar Scan", IDC_SCAN_START, 416, 8, 150, 36);
        SBTN("Parar",        IDC_SCAN_STOP,  576, 8,  80, 36);
        SBTN("Fechar",       IDC_SCAN_CLOSE, 666, 8,  90, 36);
        HWND hlbl = CreateWindowA("STATIC", "Pronto. Clica em Iniciar Scan.",
            WS_CHILD|WS_VISIBLE|SS_LEFT, 768, 15, 280, 24,
            hw, (HMENU)(UINT_PTR)IDC_SCAN_LABEL, hI, NULL);
        SendMessageA(hlbl, WM_SETFONT, (WPARAM)f, 0);
        EnableWindow(GetDlgItem(hw, IDC_SCAN_STOP), FALSE);

        /* Progress bar */
        HWND hp = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
            WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            10, 54, 1030, 22, hw,
            (HMENU)(UINT_PTR)IDC_SCAN_PROG, hI, NULL);
        SendMessageA(hp, PBM_SETRANGE, 0, MAKELPARAM(0, 254));

        /* ListView */
        HWND lv = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
            10, 84, 1030, 486, hw,
            (HMENU)(UINT_PTR)IDC_SCAN_LIST, hI, NULL);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        SendMessageA(lv, WM_SETFONT, (WPARAM)f, 0);
        HIMAGELIST hIL = ImageList_Create(1, 26, ILC_COLOR, 1, 1);
        ListView_SetImageList(lv, hIL, LVSIL_SMALL);
        static const char *cols[] = {"#","IP Address","Hostname","MAC Address","ms"};
        static const int   wids[] = {44, 148,         340,        175,          62};
        for (int i = 0; i < 5; i++) {
            LVCOLUMNA c = {0};
            c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
            c.iSubItem=i; c.pszText=(char*)cols[i]; c.cx=wids[i];
            ListView_InsertColumn(lv, i, &c);
        }
#undef SLBL
#undef SBTN
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_SCAN_CLOSE || id == IDCANCEL) {
            g_scan_stop = TRUE;
            g_scan_wnd  = NULL;
            DestroyWindow(hw);
        } else if (id == IDC_SCAN_START) {
            HWND lv = GetDlgItem(hw, IDC_SCAN_LIST);
            ListView_DeleteAllItems(lv);
            g_scan_found = 0;
            g_scan_stop  = FALSE;
            SendMessageA(GetDlgItem(hw, IDC_SCAN_PROG), PBM_SETPOS, 0, 0);
            SetDlgItemTextA(hw, IDC_SCAN_LABEL, "A varrer...");
            EnableWindow(GetDlgItem(hw, IDC_SCAN_START), FALSE);
            EnableWindow(GetDlgItem(hw, IDC_SCAN_STOP),  TRUE);
            char base[32] = {0};
            GetDlgItemTextA(hw, IDC_SCAN_BASE, base, sizeof(base)-2);
            if (!base[0]) strncpy(base, "192.168.222.", sizeof(base)-1);
            int blen = (int)strlen(base);
            if (blen > 0 && base[blen-1] != '.') { base[blen]='.'; base[blen+1]=0; }
            ScanMgrArgs *sma = (ScanMgrArgs*)calloc(1, sizeof(ScanMgrArgs));
            if (sma) {
                sma->hw = hw;
                strncpy(sma->base, base, sizeof(sma->base)-1);
                HANDLE ht = CreateThread(NULL, 0, ScanMgrThread, sma, 0, NULL);
                if (ht) CloseHandle(ht); else free(sma);
            }
        } else if (id == IDC_SCAN_STOP) {
            g_scan_stop = TRUE;
            EnableWindow(GetDlgItem(hw, IDC_SCAN_STOP),  FALSE);
            EnableWindow(GetDlgItem(hw, IDC_SCAN_START), TRUE);
            SetDlgItemTextA(hw, IDC_SCAN_LABEL, "Scan interrompido.");
        }
        return 0;
    }

    case WM_SCAN_RESULT: {
        ScanResult *r = (ScanResult*)lp;
        if (r) {
            g_scan_found++;
            HWND lv = GetDlgItem(hw, IDC_SCAN_LIST);
            char num[8]; snprintf(num, sizeof(num), "%d", g_scan_found);
            LVITEMA item = {0};
            item.mask = LVIF_TEXT; item.iItem = g_scan_found-1; item.pszText = num;
            ListView_InsertItem(lv, &item);
            int row = g_scan_found - 1;
            ListView_SetItemText(lv, row, 1, r->ip);
            ListView_SetItemText(lv, row, 2, r->hostname);
            ListView_SetItemText(lv, row, 3, r->mac);
            char lat[20]; snprintf(lat, sizeof(lat), "%lu ms", r->latency_ms);
            ListView_SetItemText(lv, row, 4, lat);
            ListView_EnsureVisible(lv, row, FALSE);
            free(r);
        }
        return 0;
    }

    case WM_SCAN_PROGRESS: {
        int prog = (int)(UINT_PTR)wp;
        SendMessageA(GetDlgItem(hw, IDC_SCAN_PROG), PBM_SETPOS, (WPARAM)prog, 0);
        char lbl[80];
        snprintf(lbl, sizeof(lbl), "A varrer... %d/254  |  Encontrados: %d",
                 prog, g_scan_found);
        SetDlgItemTextA(hw, IDC_SCAN_LABEL, lbl);
        return 0;
    }

    case WM_SCAN_DONE: {
        EnableWindow(GetDlgItem(hw, IDC_SCAN_START), TRUE);
        EnableWindow(GetDlgItem(hw, IDC_SCAN_STOP),  FALSE);
        SendMessageA(GetDlgItem(hw, IDC_SCAN_PROG), PBM_SETPOS, 254, 0);
        char lbl[80];
        snprintf(lbl, sizeof(lbl), "Scan completo.  %d dispositivo(s) encontrado(s).", g_scan_found);
        SetDlgItemTextA(hw, IDC_SCAN_LABEL, lbl);
        return 0;
    }

    case WM_DESTROY:
        g_scan_stop = TRUE;
        g_scan_wnd  = NULL;
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_scan_stop=TRUE; g_scan_wnd=NULL; DestroyWindow(hw); }
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void ShowScanDlg(HWND parent)
{
    if (g_scan_wnd) { SetForegroundWindow(g_scan_wnd); return; }

    HINSTANCE hI = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    static BOOL reg = FALSE;
    if (!reg) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = ScanWndProc;
        wc.hInstance     = hI;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
        wc.lpszClassName = "RLS_SCAN";
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        RegisterClassA(&wc);
        reg = TRUE;
    }

    int W = 1060, H = 600;
    RECT rc; GetWindowRect(parent, &rc);
    int X = rc.left + (rc.right-rc.left-W)/2;
    int Y = rc.top  + (rc.bottom-rc.top-H)/2;
    if (X < 0) X = 0;
    if (Y < 0) Y = 0;

    g_scan_found = 0;
    g_scan_stop  = FALSE;
    g_scan_wnd   = CreateWindowExA(WS_EX_APPWINDOW,
        "RLS_SCAN", "Scan de Rede VPN - RLS Automacao",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        X, Y, W, H, NULL, NULL, hI, NULL);
    ShowWindow(g_scan_wnd, SW_SHOW);
    UpdateWindow(g_scan_wnd);
}

/* ======================================================================
   ListView helper
   ====================================================================== */
static HWND MakeListView(HWND parent, UINT id,
    const char **cols, const int *widths, int ncols)
{
    HWND lv = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
        WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
        0,0,100,100, parent,(HMENU)(UINT_PTR)id,NULL,NULL);
    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
    SendMessageA(lv, WM_SETFONT,(WPARAM)g_font_ui,FALSE);
    /* Increase row height via a 1px transparent imagelist */
    HIMAGELIST hIL = ImageList_Create(1, 26, ILC_COLOR, 1, 1);
    ListView_SetImageList(lv, hIL, LVSIL_SMALL);
    LVCOLUMNA c={0};
    c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
    for(int i=0;i<ncols;i++){
        c.iSubItem=i; c.pszText=(char*)cols[i]; c.cx=widths[i];
        ListView_InsertColumn(lv,i,&c);
    }
    return lv;
}

/* ======================================================================
   Layout (resize handler)
   ====================================================================== */
static void LayoutChildren(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd,&rc);
    int W=rc.right, H=rc.bottom;

    /* Status bar */
    SendMessageA(g_statusbar, WM_SIZE,0,0);
    RECT sbr; GetClientRect(g_statusbar,&sbr);
    int sbH = sbr.bottom;

    int content_y = TB_H;
    int content_h = H - sbH - TB_H;
    int log_h     = LOG_H;
    int lists_h   = content_h - log_h;
    int top_h     = lists_h * 58 / 100;
    int bot_h     = lists_h - top_h;

    MoveWindow(g_toolbar,    0, 0,         W, TB_H,  TRUE);
    MoveWindow(g_list_conns, 0, content_y, W, top_h, TRUE);
    MoveWindow(g_list_adaps, 0, content_y+top_h, W, bot_h, TRUE);
    ShowWindow(g_log, SW_SHOW);
    MoveWindow(g_log, 0, content_y+lists_h, W, log_h, TRUE);

    int parts[3] = {280, W-200, W};
    SendMessageA(g_statusbar, SB_SETPARTS, 3, (LPARAM)parts);
}

/* ======================================================================
   Toolbar subclass
   ====================================================================== */
static LRESULT CALLBACK ToolbarWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch(msg){
    case WM_CREATE: {
        HINSTANCE hI=((CREATESTRUCTA*)lp)->hInstance;
        int x = 6;
        for(int i=0;i<TB_BTN_CNT;i++){
            int bw = (g_tbBtns[i].id == IDM_TOGGLE) ? BTN_TOGGLE_W : BTN_W;
            DWORD style = WS_CHILD|WS_VISIBLE|WS_TABSTOP;
            style |= (g_tbBtns[i].id == IDM_TOGGLE) ? BS_OWNERDRAW : BS_PUSHBUTTON;
            HWND b=CreateWindowA("BUTTON",g_tbBtns[i].label, style,
                x, 6, bw, BTN_H, hw,
                (HMENU)(UINT_PTR)g_tbBtns[i].id, hI, NULL);
            SendMessageA(b,WM_SETFONT,(WPARAM)g_font_tb,FALSE);
            if (g_tbBtns[i].id == IDM_TOGGLE) g_btn_toggle = b;
            x += bw + 4;
        }
        return 0;
    }
    case WM_COMMAND:
        PostMessageA(GetParent(hw),WM_COMMAND,wp,lp);
        return 0;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT*)lp;
        if (dis->hwndItem != g_btn_toggle) break;
        BOOL connected = g_connected;  /* usa o boolean rastreado, nao o g_state */
        COLORREF bg = connected ? RGB(180,0,0) : RGB(0,150,0);
        const char *lbl = connected ? "DESLIGAR" : "LIGAR";
        /* Fill background */
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(dis->hDC, &dis->rcItem, br);
        DeleteObject(br);
        /* Indent when pressed */
        RECT rc = dis->rcItem;
        if (dis->itemState & ODS_SELECTED) InflateRect(&rc, -2, -2);
        /* Border */
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
        HPEN old = (HPEN)SelectObject(dis->hDC, pen);
        HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH ob = (HBRUSH)SelectObject(dis->hDC, nb);
        Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                             dis->rcItem.right, dis->rcItem.bottom);
        SelectObject(dis->hDC, old); DeleteObject(pen);
        SelectObject(dis->hDC, ob);
        /* Text */
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, RGB(255,255,255));
        SelectObject(dis->hDC, g_font_tb);
        DrawTextA(dis->hDC, lbl, -1, &rc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &rc);
        return TRUE;
    }
    case WM_ERASEBKGND: {
        HDC dc=(HDC)wp; RECT r; GetClientRect(hw,&r);
        FillRect(dc,&r,(HBRUSH)(COLOR_BTNFACE+1));
        return 1;
    }
    }
    return DefWindowProcA(hw,msg,wp,lp);
}

/* ======================================================================
   Main window procedure
   ====================================================================== */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch(msg){

    case WM_CREATE: {
        HINSTANCE hI=((CREATESTRUCTA*)lp)->hInstance;
        g_font_ui    = MakeFont(-18,FALSE,FALSE);
        g_font_bold  = MakeFont(-18,TRUE, FALSE);
        g_font_mono  = MakeFont(-14,FALSE,TRUE);
        g_font_tb    = MakeFont(-15,FALSE,FALSE);

        /* --- Menu bar ------------------------------------------------ */
        HMENU mb=CreateMenu();

        HMENU mConn=CreatePopupMenu();
        AppendMenuA(mConn,MF_STRING,IDM_CONNECT,   "Ligar\tF5");
        AppendMenuA(mConn,MF_STRING,IDM_DISCONNECT,"Desligar\tF6");
        AppendMenuA(mConn,MF_SEPARATOR,0,NULL);
        AppendMenuA(mConn,MF_STRING,IDM_RESET,     "Reset (remove conta + placa)");
        AppendMenuA(mConn,MF_SEPARATOR,0,NULL);
        AppendMenuA(mConn,MF_STRING,IDM_EXIT,      "Sair\tAlt+F4");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mConn,"Ligacao");

        HMENU mEdit=CreatePopupMenu();
        /* Abre dialogo de configuracao - onde tambem se cria a conta VPN */
        AppendMenuA(mEdit,MF_STRING,IDM_SAVE_CFG,"Configurar VPN...\tCtrl+S");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mEdit,"Editar");

        HMENU mView=CreatePopupMenu();
        AppendMenuA(mView,MF_STRING,IDM_SCAN,"Scan de Rede VPN...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mView,"Ver");

        HMENU mAdap=CreatePopupMenu();
        AppendMenuA(mAdap,MF_STRING,IDM_APPLYIP,"Aplicar IP Estatico...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mAdap,"Adaptador VPN");

        HMENU mTools=CreatePopupMenu();
        AppendMenuA(mTools,MF_STRING,IDM_SCAN,"Scan de Rede...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mTools,"Ferramentas");

        HMENU mHelp=CreatePopupMenu();
        AppendMenuA(mHelp,MF_STRING,IDM_ABOUT,"Sobre RLS VPN...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mHelp,"Ajuda");

        SetMenu(hwnd,mb);

        /* --- Toolbar ------------------------------------------------- */
        WNDCLASSA tc={0};
        tc.lpfnWndProc  =ToolbarWndProc;
        tc.hInstance    =hI;
        tc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        tc.lpszClassName="RLS_TB";
        tc.hCursor=LoadCursorA(NULL,(LPCSTR)IDC_ARROW);
        RegisterClassA(&tc);
        g_toolbar=CreateWindowA("RLS_TB","",WS_CHILD|WS_VISIBLE,
            0,0,100,TB_H, hwnd,(HMENU)(UINT_PTR)IDC_TOOLBAR,hI,NULL);

        /* --- ListViews ----------------------------------------------- */
        {
            const char *cols[]={"Nome da Ligacao VPN","Estado","Servidor VPN","Hub Virtual","Adaptador Virtual"};
            const int   wids[]={260,160,300,160,180};
            g_list_conns=MakeListView(hwnd,IDC_LIST_CONNS,cols,wids,5);
        }
        {
            const char *cols[]={"Adaptador Virtual (Windows)","MAC Address","Estado","Endereco IP"};
            const int   wids[]={280,160,120,200};
            g_list_adaps=MakeListView(hwnd,IDC_LIST_ADAPTERS,cols,wids,4);
        }

        /* --- Log ----------------------------------------------------- */
        g_log=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            0,0,100,100, hwnd,(HMENU)(UINT_PTR)IDC_LOG,hI,NULL);
        SendMessageA(g_log,WM_SETFONT,(WPARAM)g_font_mono,FALSE);
        SendMessageA(g_log,EM_SETLIMITTEXT,40960,0);

        /* --- Status bar --------------------------------------------- */
        g_statusbar=CreateWindowExA(0,STATUSCLASSNAMEA,"",
            WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
            0,0,0,0, hwnd,(HMENU)(UINT_PTR)IDC_STATUSBAR,hI,NULL);
        SendMessageA(g_statusbar,WM_SETFONT,(WPARAM)g_font_ui,FALSE);
        int parts[3]={220,800,-1};
        SendMessageA(g_statusbar,SB_SETPARTS,3,(LPARAM)parts);
        sb_set(0,"  Estado: ---");
        sb_set(1,"  Aguardar estado VPN...");
        sb_set(2,"  IP: ---");

        /* --- Init VPN ----------------------------------------------- */
        vpn_default_config(&g_cfg);
        vpn_set_log_fn(vpn_log_cb,NULL);

        LayoutChildren(hwnd);
        /* Pre-populate ListViews immediately from defaults (user sees data at once) */
        FillListViews(NULL);

        log_append("[RLS VPN v1.5.4] Interface iniciada. Aguardar estado VPN...");

        /* Polling thread (TRUE = nao dorme na primeira iteracao) */
        HANDLE ht=CreateThread(NULL,0,PollThread,(LPVOID)(UINT_PTR)TRUE,0,NULL);
        if(ht) CloseHandle(ht);

        return 0;
    }

    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;

    case WM_COMMAND: {
        int id=LOWORD(wp);
        switch(id){
        case IDM_TOGGLE:
            /* Toggle: usa g_connected (atualizado otimisticamente no clique
               e sincronizado pelo poll) — nao depende do g_state que pode
               estar desatualizado no momento do clique.                    */
            if (g_connected) {
                g_connected = FALSE;   /* otimista: mostra LIGAR imediatamente */
                StartOp(3,NULL,NULL);  /* AccountDisconnect */
            } else {
                g_connected = TRUE;    /* otimista: mostra DESLIGAR imediatamente */
                StartOp(2,NULL,NULL);  /* AccountConnect */
            }
            if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
            break;
        case IDM_CONNECT:    StartOp(2,NULL,NULL); break;
        case IDM_DISCONNECT: StartOp(3,NULL,NULL); break;
        case IDM_SETUP:      StartOp(1,NULL,NULL); break;
        case IDM_RESET:
            if (AskResetPassword(hwnd)) StartOp(4,NULL,NULL);
            break;
        case IDM_SCAN:       ShowScanDlg(hwnd);    break;
        case IDM_SAVE_CFG:
        case IDM_APPLYIP:    ShowConfigDlg(hwnd);  break;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                "RLS Automacao VPN Client v1.5.4\n\n"
                "Fluxo de utilizacao:\n"
                "  1. Botao 'Configurar VPN'\n"
                "     -> preencher Servidor / Hub / User / Password\n"
                "     -> clicar 'Criar Conta VPN'\n"
                "     (cria placa virtual + conta no SoftEther)\n\n"
                "  2. Botao 'Ligar' para estabelecer a ligacao\n"
                "  3. Botao 'Desligar' para terminar\n\n"
                "Interface Win32 nativa. Zero WebView2.\n"
                "Todo o output do SoftEther visivel no log.\n\n"
                "(c) 2026 RLS Automacao",
                "Sobre RLS VPN v1.5.4", MB_ICONINFORMATION);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }

    case WM_VPN_LOG: {
        const char *t=(const char*)lp;
        if(t){ log_append(t); free((void*)t); }
        return 0;
    }

    case WM_VPN_RESULT: {
        const char *t=(const char*)lp;
        int ok=(int)(UINT_PTR)wp;
        g_busy=FALSE;
        /* Corrigir g_connected se a operacao falhou */
        if (g_last_op == 2 && !ok) {
            g_connected = FALSE;  /* connect falhou -> reverte para desligado */
            if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
        } else if (g_last_op == 3) {
            g_connected = FALSE;  /* disconnect concluido (sempre assume desligado) */
            if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
        } else if (g_last_op == 2 && ok) {
            g_connected = TRUE;
            if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
        }
        if(t){
            log_append(t);
            sb_set(1,t);
            /* Popup se SoftEther nao instalado */
            if (strstr(t, "nao esta instalado") || strstr(t, "nao encontrado")) {
                MessageBoxA(g_hwnd, t,
                    "SoftEther VPN nao instalado", MB_ICONWARNING|MB_OK);
            }
            free((void*)t);
        }
        /* Apos ligar/configurar: re-poll rapido para mostrar novo estado.
           Apos DESLIGAR (op=3) NAO fazemos re-poll imediato — o SoftEther
           ainda reporta "connected" por 1-2s e isso flipa g_connected de
           volta para TRUE, tornando impossivel voltar a ligar.
           O PollThread normal (3s) actualiza o estado quando o servico
           confirmar o disconnect. */
        if (g_last_op == 1 || g_last_op == 2) {
            HANDLE ht = CreateThread(NULL, 0, PollThread, (LPVOID)(UINT_PTR)TRUE, 0, NULL);
            if (ht) CloseHandle(ht);
        }
        return 0;
    }

    case WM_VPN_STATUS: {
        VpnStatus *st=(VpnStatus*)lp;
        if(st){ UpdateUI(st); free(st); }
        return 0;
    }

    case WM_KEYDOWN:
        if(wp==VK_F5) { StartOp(2,NULL,NULL); return 0; }
        if(wp==VK_F6) { StartOp(3,NULL,NULL); return 0; }
        break;

    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR*)lp;
        if (hdr->code == NM_CUSTOMDRAW &&
            (hdr->hwndFrom==g_list_conns || hdr->hwndFrom==g_list_adaps)) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW*)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                /* Color row based on VPN state */
                switch(g_state) {
                case VPN_CONNECTED:
                    cd->clrTextBk = RGB(0,160,0);
                    cd->clrText   = RGB(255,255,255);
                    break;
                case VPN_CONNECTING:
                    cd->clrTextBk = RGB(220,150,0);
                    cd->clrText   = RGB(255,255,255);
                    break;
                case VPN_DISCONNECTED:
                case VPN_NOT_CONFIGURED:
                    cd->clrTextBk = RGB(200,0,0);
                    cd->clrText   = RGB(255,255,255);
                    break;
                default:
                    cd->clrTextBk = RGB(100,100,110);
                    cd->clrText   = RGB(220,220,220);
                    break;
                }
                return CDRF_NEWFONT;
            }
        }
        return CDRF_DODEFAULT;
    }

    case WM_DESTROY:
        g_hwnd=NULL;
        /* Desligar VPN ao fechar a aplicacao */
        if (g_connected) {
            char out[256]={0};
            vpn_disconnect(&g_cfg, out, sizeof out);
        }
        DeleteObject(g_font_ui);
        DeleteObject(g_font_bold);
        DeleteObject(g_font_mono);
        DeleteObject(g_font_tb);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

/* ======================================================================
   WinMain
   ====================================================================== */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nCmdShow)
{
    (void)hPrev; (void)lpCmd;

    INITCOMMONCONTROLSEX icc={sizeof icc,
        ICC_WIN95_CLASSES|ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXA wc={0};
    wc.cbSize       =sizeof wc;
    wc.style        =CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc  =WndProc;
    wc.hInstance    =hInst;
    wc.hCursor      =LoadCursorA(NULL,(LPCSTR)IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_3DFACE+1);
    wc.lpszClassName="RLS_VPN_MAIN";
    wc.hIcon        =LoadIconA(hInst,MAKEINTRESOURCEA(1));
    wc.hIconSm      =LoadIconA(hInst,MAKEINTRESOURCEA(1));
    RegisterClassExA(&wc);

    g_hwnd=CreateWindowExA(0,
        "RLS_VPN_MAIN",
        "RLS Automacao VPN Client v1.5.4",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1100,720,
        NULL,NULL,hInst,NULL);
    if(!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(g_hwnd);

    MSG m;
    while(GetMessageA(&m,NULL,0,0)>0){
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}