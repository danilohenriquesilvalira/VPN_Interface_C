/*
 * main.c - RLS Automacao VPN Client v2.0.0
 * Interface estilo SoftEther: menu bar, ListViews, status bar, toolbar.
 * Polling de estado em thread dedicada - UI thread NUNCA bloqueia.
 *
 * v1.5.1: fontes maiores, NM_CUSTOMDRAW colors, toolbar maior
 * v1.5.2: remove strip panel, state label simples colorido,
 *         pre-popular ListView no arranque, config dialog maior
 * v1.5.4: fix NicCreate Error 32 - remove adaptador TAP orfao do Windows
 *         via PowerShell + restart servico antes de retentar NicCreate
 * v2.0.0: Multi-profile manager - lista de conexoes independentes.
 *         Uma unica NIC VPN partilhada; disconnect automatico antes de
 *         ligar um perfil diferente; persistencia JSON local.
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
#include "profiles.h"

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
#define IDM_PINGTEST   1014   /* ping ao servidor VPN */
#define IDM_ADD_CONN   1015   /* adicionar nova conexao */
#define IDM_DEL_CONN   1016   /* remover conexao selecionada */

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
#define WM_PING_RESULT   (WM_APP+7)   /* lp = char* resultado (heap, receiver frees) */
#define WM_SRV_PING      (WM_APP+8)   /* wp = 1 (online) / 0 (offline) — auto check */
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
/* Multi-profile globals */
static VpnProfileList g_profiles;       /* lista completa de perfis         */
static VpnConfig      g_cfg;            /* config activa (do perfil activo)  */
static int            g_sel_idx = 0;    /* indice seleccionado na ListView   */
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
static volatile int g_srv_reachable = -1;  /* -1=desconhecido 0=offline 1=online */

/* ---- Network scan types ------------------------------------------------- */
typedef struct {
    char ip[20];
    char mac[24];
    char hostname[128];
    DWORD latency_ms;
} ScanResult;
typedef struct { HWND hw; DWORD ip; int idx; } PingArg;
typedef struct { HWND hw; char host[256]; int port; } SrvPingArg;
typedef struct {
    int   success;        /* 0..4 pings ok */
    DWORD latency[4];     /* ms por ping, 0 = timeout */
    char  host[256];
    char  ip_str[32];
    char  error[256];     /* preenchido se erro antes dos pings */
} SrvPingResult;

/* ---- Forward declarations ----------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
static LRESULT CALLBACK ToolbarWndProc(HWND,UINT,WPARAM,LPARAM);
static void LayoutChildren(HWND);

/* ---- Toolbar button definitions -----------------------------------------
   Toggle (IDM_TOGGLE): owner-drawn, green=LIGAR / red=DESLIGAR.
   Remaining buttons: regular BS_PUSHBUTTON.                                */
static const struct { int id; const char *label; } g_tbBtns[] = {
    {IDM_TOGGLE,   "LIGAR"},            /* owner-drawn toggle */
    {IDM_ADD_CONN, "+ Conexao"},        /* adicionar novo perfil */
    {IDM_SAVE_CFG, "Configurar VPN"},   /* editar perfil selecionado */
    {IDM_DEL_CONN, "Remover"},          /* remover perfil selecionado */
    {IDM_RESET,    "Reset"},
    {IDM_SCAN,     "Scan Rede"},        /* varredura de IP na rede VPN */
    {IDM_PINGTEST, "Teste Servidor"},   /* ping ao servidor configurado */
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
/* op 7 = disconnect_then_connect: desliga o perfil activo e liga o novo.
   O campo cfg contem o perfil a LIGAR; prev_account_name o perfil a DESLIGAR. */
typedef struct {
    int      op;
    VpnConfig cfg;
    char     ip[64];
    char     mask[64];
    char     prev_account_name[64];  /* para op 7: conta a desligar antes */
} WorkData;

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

    /* ── op 7: Desligar perfil activo e ligar perfil novo ────── */
    case 7: {
        /* Passo 1: desligar conta anteriormente activa */
        if (d->prev_account_name[0]) {
            VpnConfig prev_cfg = d->cfg;
            strncpy(prev_cfg.account_name, d->prev_account_name, sizeof prev_cfg.account_name - 1);
            char dis_msg[256] = {0};
            vpn_disconnect(&prev_cfg, dis_msg, sizeof dis_msg);
            if (dis_msg[0] && g_hwnd)
                PostMessageA(g_hwnd, WM_VPN_LOG, 0, (LPARAM)_strdup(dis_msg));
            /* Curta pausa para o SoftEther processar o disconnect */
            Sleep(1200);
        }
        /* Passo 2: ligar novo perfil */
        ok = vpn_connect(&d->cfg, msg, sizeof msg);
        if (!msg[0]) strncpy(msg,
            ok ? "Nova ligacao iniciada."
               : "Falha ao ligar novo perfil.", sizeof msg - 1);
        break;
    }

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

/* StartConnect: ligar o perfil de indice idx.
   Se outro perfil estiver activo (g_profiles.active_idx != -1 e != idx),
   usa op 7 para desligar primeiro e so depois ligar.
   Se o mesmo perfil estiver activo ja nao faz nada.                      */
static void StartConnect(int idx)
{
    if (idx < 0 || idx >= g_profiles.count) return;
    if (g_busy) {
        MessageBoxA(g_hwnd,"Aguardar operacao em curso.","RLS VPN",MB_ICONINFORMATION);
        return;
    }

    /* Actualizar g_cfg para o perfil seleccionado */
    profiles_to_cfg(&g_profiles.items[idx], &g_cfg);

    int active = g_profiles.active_idx;

    if (active == idx && g_connected) {
        /* ja ligado ao mesmo perfil: nada a fazer */
        log_append("[INFO] Perfil ja esta ligado.");
        return;
    }

    g_busy    = TRUE;
    g_last_op = (active >= 0 && active != idx) ? 7 : 2;
    sb_set(1, "A processar...");

    WorkData *d = (WorkData*)calloc(1, sizeof(WorkData));
    if (!d) { g_busy = FALSE; return; }
    d->op  = g_last_op;
    d->cfg = g_cfg;

    /* op 7: indicar qual conta desligar */
    if (g_last_op == 7 && active >= 0 && active < g_profiles.count) {
        strncpy(d->prev_account_name,
                g_profiles.items[active].account_name,
                sizeof d->prev_account_name - 1);
    }

    HANDLE ht = CreateThread(NULL, 0, WorkerThread, d, 0, NULL);
    if (ht) CloseHandle(ht); else { free(d); g_busy = FALSE; }
}

/* ======================================================================
   Server reachability check thread - ping automatico ao servidor VPN
   Corre a cada 8 segundos em segundo plano, nunca bloqueia o UI.
   ====================================================================== */
static DWORD WINAPI SrvCheckThread(LPVOID p)
{
    (void)p;
    Sleep(2500); /* aguardar inicializacao da winsock */
    for (;;) {
        if (!g_hwnd) return 0;
        /* Copiar host da config global (leitura rapida, race benigno) */
        char host[256] = {0};
        strncpy(host, g_cfg.host[0] ? g_cfg.host : DEFAULT_HOST, sizeof(host)-1);

        /* Resolver endereco */
        DWORD destIP = inet_addr(host);
        if (destIP == INADDR_NONE) {
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
                destIP = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(res);
            }
        }

        int reachable = 0;
        if (destIP != INADDR_NONE) {
            HANDLE hIcmp = IcmpCreateFile();
            if (hIcmp != INVALID_HANDLE_VALUE) {
                char send_buf[8] = "RLSCHK";
                BYTE recv_buf[256] = {0};
                DWORD ret = IcmpSendEcho(hIcmp, destIP, send_buf, 8,
                                         NULL, recv_buf, sizeof(recv_buf), 1500);
                reachable = (ret > 0) ? 1 : 0;
                IcmpCloseHandle(hIcmp);
            }
        }

        if (g_hwnd) PostMessageA(g_hwnd, WM_SRV_PING, (WPARAM)reachable, 0);
        Sleep(8000);
    }
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
/* Auxiliar: actualiza uma celula ListView SEM piscar.
   So chama SetItemText se o conteudo realmente mudou.           */
static void lv_set_if_changed(HWND lv, int row, int col, const char *newval)
{
    char cur[512] = {0};
    ListView_GetItemText(lv, row, col, cur, (int)sizeof(cur) - 1);
    if (strcmp(cur, newval) != 0)
        ListView_SetItemText(lv, row, col, (char*)newval);
}

static void FillListViews(const VpnStatus *st)
{
    /* ── Tabela de Ligacoes VPN ─────────────────────────────────────────
       Uma linha por perfil.  A coluna Estado so e preenchida com dados
       reais para o perfil activo (active_idx); os outros ficam "Offline".
       WM_SETREDRAW FALSE/TRUE elimina o piscar causado pelo poll a cada 3s. */

    int nProfiles = g_profiles.count;
    int active    = g_profiles.active_idx;

    /* Suspender redesenho da lista de conexoes */
    SendMessageA(g_list_conns, WM_SETREDRAW, FALSE, 0);

    /* Ajustar numero de linhas apenas se necessario */
    int cur_rows = ListView_GetItemCount(g_list_conns);
    while (cur_rows < nProfiles) {
        LVITEMA lvi = {0};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = cur_rows;
        lvi.pszText = (char*)"";
        ListView_InsertItem(g_list_conns, &lvi);
        cur_rows++;
    }
    while (cur_rows > nProfiles) {
        ListView_DeleteItem(g_list_conns, cur_rows - 1);
        cur_rows--;
    }

    /* Preencher cada linha — so actualiza celulas cujo texto mudou */
    for (int i = 0; i < nProfiles; i++) {
        VpnProfile *pr = &g_profiles.items[i];

        /* Col 0: Nome amigavel */
        lv_set_if_changed(g_list_conns, i, 0, pr->name[0] ? pr->name : "(sem nome)");

        /* Col 1: Estado */
        char estado[128] = {0};
        if (i == active) {
            if (st && st->vpn_user[0])
                strncpy(estado, st->vpn_user, sizeof estado - 1);
            else if (st && st->status_detail[0])
                strncpy(estado, st->status_detail, sizeof estado - 1);
            else if (st)
                strncpy(estado, StateStr(st->state), sizeof estado - 1);
            else
                strncpy(estado, "Aguardar...", sizeof estado - 1);
        } else {
            strncpy(estado, "Offline", sizeof estado - 1);
        }
        lv_set_if_changed(g_list_conns, i, 1, estado);

        /* Col 2: Servidor */
        char srv[320] = {0};
        if (i == active && st && st->server_display[0])
            strncpy(srv, st->server_display, sizeof srv - 1);
        else
            snprintf(srv, sizeof srv, "%s:%d", pr->host, (int)pr->port);
        lv_set_if_changed(g_list_conns, i, 2, srv);

        /* Col 3: Hub */
        char hub[64] = {0};
        if (i == active && st && st->hub_display[0])
            strncpy(hub, st->hub_display, sizeof hub - 1);
        else
            strncpy(hub, pr->hub[0] ? pr->hub : DEFAULT_HUB, sizeof hub - 1);
        lv_set_if_changed(g_list_conns, i, 3, hub);

        /* Col 4: Adaptador Virtual — sempre a mesma NIC */
        const char *nic_se = (i == active && st && st->nic_name[0])
                             ? st->nic_name : NIC_NAME;
        lv_set_if_changed(g_list_conns, i, 4, nic_se);

        /* Col 5: Servidor Online/Offline */
        const char *reach;
        if (i == active) {
            if      (g_srv_reachable == 1)  reach = "Online";
            else if (g_srv_reachable == 0)  reach = "Offline";
            else                            reach = "A verificar...";
        } else {
            reach = "---";
        }
        lv_set_if_changed(g_list_conns, i, 5, reach);
    }

    /* Retomar redesenho e forcar uma unica passagem de pintura */
    SendMessageA(g_list_conns, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list_conns, NULL, FALSE);

    /* ── Tabela de Adaptadores ──────────────────────────────────────────
       Uma unica NIC virtual partilhada por todos os perfis.              */
    SendMessageA(g_list_adaps, WM_SETREDRAW, FALSE, 0);

    const char *nic_win = (st && st->nic_windows[0]) ? st->nic_windows : "---";
    const char *nic_mac = (st && st->nic_mac[0])     ? st->nic_mac     : "---";

    const char *adap_estado;
    if (!st)
        adap_estado = "Aguardar...";
    else if (st->nic_status[0])
        adap_estado = st->nic_status;
    else if (st->state == VPN_CONNECTED)
        adap_estado = "Enabled";
    else
        adap_estado = "Disabled";

    char ip_buf[64] = {0};
    if (st && st->local_ip[0])
        strncpy(ip_buf, st->local_ip, sizeof ip_buf - 1);
    else
        strncpy(ip_buf, "---", sizeof ip_buf - 1);

    const char *se_state  = g_se_installed ? "Instalado" : "Nao Instalado";
    char se_label[] = "SoftEther VPN Client";
    char se_dash[]  = "---";

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
    lv_set_if_changed(g_list_adaps, 0, 0, nic_win);
    lv_set_if_changed(g_list_adaps, 0, 1, nic_mac);
    lv_set_if_changed(g_list_adaps, 0, 2, adap_estado);
    lv_set_if_changed(g_list_adaps, 0, 3, ip_buf);
    lv_set_if_changed(g_list_adaps, 1, 0, se_label);
    lv_set_if_changed(g_list_adaps, 1, 1, se_dash);
    lv_set_if_changed(g_list_adaps, 1, 2, se_state);
    lv_set_if_changed(g_list_adaps, 1, 3, se_dash);

    SendMessageA(g_list_adaps, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list_adaps, NULL, FALSE);
}

/* ======================================================================
   Update on VPN poll result (runs on UI thread via WM_VPN_STATUS)
   ====================================================================== */
static void UpdateUI(const VpnStatus *st)
{
    g_state = st->state;
    if (st->local_ip[0]) strncpy(g_vpn_ip, st->local_ip, sizeof(g_vpn_ip)-1);
    g_se_installed = (BOOL)st->softether_ready;

    if (st->state == VPN_CONNECTED && g_last_op != 3) {
        g_connected = TRUE;
        /* Marcar perfil activo como o que esta seleccionado */
        if (g_profiles.active_idx < 0)
            g_profiles.active_idx = g_sel_idx;
    } else if (st->state == VPN_DISCONNECTED ||
               st->state == VPN_NOT_CONFIGURED) {
        g_connected = FALSE;
        g_profiles.active_idx = -1;
    }
    if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
    FillListViews(st);

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
   Modo: g_cfg_is_new == TRUE  -> criar novo perfil (campos em branco)
         g_cfg_is_new == FALSE -> editar perfil seleccionado (g_sel_idx)
   ====================================================================== */
static HWND  g_cfg_wnd    = NULL;
static BOOL  g_cfg_is_new = FALSE;  /* TRUE quando aberto por "Adicionar Conexao" */

/* IDC extra para o campo Nome amigavel */
#define IDC_CFG_NAME 507

static LRESULT CALLBACK CfgWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch(msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK || id == IDM_SETUP) {
            /* Ler campos */
            char name[64]={0}, tmp[256];
            GetDlgItemTextA(hw, IDC_CFG_NAME, name,     sizeof name);
            GetDlgItemTextA(hw, IDC_CFG_HOST, g_cfg.host, sizeof g_cfg.host);
            GetDlgItemTextA(hw, IDC_CFG_PORT, tmp, sizeof tmp);
            int p = atoi(tmp); if (p>0) g_cfg.port=(unsigned short)p;
            GetDlgItemTextA(hw, IDC_CFG_HUB,  g_cfg.hub,      sizeof g_cfg.hub);
            GetDlgItemTextA(hw, IDC_CFG_USER, g_cfg.username,  sizeof g_cfg.username);
            GetDlgItemTextA(hw, IDC_CFG_PASS, g_cfg.password,  sizeof g_cfg.password);

            if (!g_cfg.host[0]) {
                MessageBoxA(hw, "Preencha o campo Servidor.", "Configuracao VPN", MB_ICONWARNING);
                return 0;
            }

            if (g_cfg_is_new) {
                /* Criar novo perfil */
                if (!name[0]) strncpy(name, g_cfg.host, sizeof name - 1);
                /* Gerar account_name unico baseado no nome */
                char acct[64] = {0};
                int j = 0;
                for (int i = 0; name[i] && j < 12; i++) {
                    char c = name[i];
                    if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9'))
                        acct[j++] = c;
                }
                if (!acct[0]) snprintf(acct, sizeof acct, "P%d", g_profiles.count);
                snprintf(g_cfg.account_name, sizeof g_cfg.account_name, "RLS_%s", acct);

                VpnProfile np = {0};
                profiles_from_cfg(&np, &g_cfg, name);
                int ni = profiles_add(&g_profiles, &np);
                if (ni >= 0) {
                    g_sel_idx = ni;
                    profiles_save(&g_profiles);
                    log_append("[CONFIG] Nova conexao adicionada e guardada.");
                } else {
                    MessageBoxA(hw, "Limite de perfis atingido (32).", "RLS VPN", MB_ICONWARNING);
                }
            } else {
                /* Editar perfil existente */
                int idx = g_sel_idx;
                if (idx >= 0 && idx < g_profiles.count) {
                    if (!name[0]) strncpy(name, g_profiles.items[idx].name, sizeof name - 1);
                    profiles_from_cfg(&g_profiles.items[idx], &g_cfg, name);
                    profiles_save(&g_profiles);
                    log_append("[CONFIG] Configuracao do perfil guardada.");
                }
            }

            sb_set(1, "  Configuracao guardada.");
            FillListViews(NULL);
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(hw);
            g_cfg_wnd = NULL;

            if (id == IDM_SETUP) {
                /* Criar NIC + conta VPN no SoftEther com os novos dados */
                if (g_sel_idx >= 0 && g_sel_idx < g_profiles.count)
                    profiles_to_cfg(&g_profiles.items[g_sel_idx], &g_cfg);
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

/* OpenConfigDlg: abre o dialogo de configuracao.
   is_new == TRUE   -> formulario vazio para nova conexao
   is_new == FALSE  -> editar perfil g_sel_idx                */
static void OpenConfigDlg(HWND parent, BOOL is_new)
{
    if (g_cfg_wnd) { SetForegroundWindow(g_cfg_wnd); return; }
    g_cfg_is_new = is_new;

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

    /* Preparar valores iniciais */
    char init_name[64]  = {0};
    char init_host[256] = {0};
    char init_port[8]   = "443";
    char init_hub[64]   = {0};
    char init_user[64]  = {0};
    char init_pass[128] = {0};

    if (!is_new && g_sel_idx >= 0 && g_sel_idx < g_profiles.count) {
        VpnProfile *pr = &g_profiles.items[g_sel_idx];
        strncpy(init_name, pr->name,     sizeof init_name - 1);
        strncpy(init_host, pr->host,     sizeof init_host - 1);
        snprintf(init_port, sizeof init_port, "%d", (int)pr->port);
        strncpy(init_hub,  pr->hub,     sizeof init_hub - 1);
        strncpy(init_user, pr->username, sizeof init_user - 1);
        strncpy(init_pass, pr->password, sizeof init_pass - 1);
    }

    int W=640, H=540;
    RECT rc; GetWindowRect(parent,&rc);
    int X = rc.left+(rc.right-rc.left-W)/2;
    int Y = rc.top+(rc.bottom-rc.top-H)/2;

    g_cfg_wnd = CreateWindowExA(WS_EX_DLGMODALFRAME,
        "RLS_CFG",
        is_new ? "Nova Conexao VPN" : "Editar Conexao VPN",
        WS_POPUP|WS_CAPTION|WS_SYSMENU,
        X,Y,W,H, parent, NULL, hI, NULL);

    HFONT f = g_font_ui;

    #define LBL(t,x,y,w,h) do{ HWND _h=CreateWindowA("STATIC",t,WS_CHILD|WS_VISIBLE|SS_RIGHT,x,y,w,h,g_cfg_wnd,NULL,hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)
    #define EDT(id,t,x,y,w,h,ex) do{ HWND _h=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|(ex),x,y,w,h,g_cfg_wnd,(HMENU)(UINT_PTR)(id),hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)
    #define BTN(t,id,x,y,w,h) do{ HWND _h=CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,x,y,w,h,g_cfg_wnd,(HMENU)(UINT_PTR)(id),hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)

    /* --- Identificacao ------------------------------------------------ */
    LBL("Nome:",       10,  18, 110, 26); EDT(IDC_CFG_NAME, init_name,  125,  16, 470, 30, 0);

    /* --- Ligacao VPN ------------------------------------------------- */
    LBL("Servidor:",   10,  62, 110, 26); EDT(IDC_CFG_HOST, init_host,  125,  60, 300, 30, 0);
    LBL("Porta:",     440,  62,  50, 26); EDT(IDC_CFG_PORT, init_port,  494,  60,  96, 30, ES_NUMBER);
    LBL("Hub Virtual:",10, 106, 110, 26); EDT(IDC_CFG_HUB,  init_hub,   125, 104, 240, 30, 0);
    LBL("Utilizador:", 10, 150, 110, 26); EDT(IDC_CFG_USER, init_user,  125, 148, 240, 30, 0);
    LBL("Password:",   10, 194, 110, 26); EDT(IDC_CFG_PASS, init_pass,  125, 192, 240, 30, ES_PASSWORD);

    /* --- Separador --------------------------------------------------- */
    { HWND sep = CreateWindowA("STATIC","",WS_CHILD|WS_VISIBLE|SS_ETCHEDHORZ,
        10,238,600,2, g_cfg_wnd,NULL,hI,NULL); (void)sep; }

    /* --- IP Estatico ------------------------------------------------- */
    const char *cur_ip = (g_vpn_ip[0] && strcmp(g_vpn_ip,"Placa sem IP")!=0)
                         ? g_vpn_ip : "";
    LBL("IP Estatico:",10, 250, 110, 26); EDT(IDC_CFG_IPST, cur_ip,      125,248, 160, 30, 0);
    LBL("Mascara:",   294, 250,  80, 26); EDT(IDC_CFG_MASK,"255.255.255.0",378,248,180, 30, 0);
    BTN("Aplicar IP Estatico", IDM_APPLYIP, 10, 292, 210, 36);

    /* --- Separador --------------------------------------------------- */
    { HWND sep2= CreateWindowA("STATIC","",WS_CHILD|WS_VISIBLE|SS_ETCHEDHORZ,
        10,342,600,2, g_cfg_wnd,NULL,hI,NULL); (void)sep2; }

    /* --- Acoes principais -------------------------------------------- */
    BTN("Guardar e Fechar", IDOK,      10, 356, 200, 40);
    BTN("Criar Conta VPN",  IDM_SETUP, 220, 356, 200, 40);
    BTN("Cancelar",         IDCANCEL,  490, 356, 120, 40);

    #undef LBL
    #undef EDT
    #undef BTN

    EnableWindow(parent, FALSE);
    ShowWindow(g_cfg_wnd, SW_SHOW);
    SetFocus(g_cfg_wnd);
}

/* Manter compat com restante codigo que chama ShowConfigDlg */
static void ShowConfigDlg(HWND parent)
{
    OpenConfigDlg(parent, FALSE);
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

static void ShowScanDlg(HWND parent);
static void ShowPingTestDlg(HWND parent);

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
   Teste de Servidor — ping ao g_cfg.host
   ====================================================================== */
#define IDC_PING_BANNER  700   /* banner verde/vermelho */
#define IDC_PING_HOST    701   /* servidor: X [IP: Y] */
#define IDC_PING_L1      702   /* ping linha 1 */
#define IDC_PING_L2      703
#define IDC_PING_L3      704
#define IDC_PING_L4      705
#define IDC_PING_SUMMARY 706   /* media + stats */
#define IDC_PING_CLOSE   707
/* ping window state (GWLP_USERDATA) */
#define PING_ST_LOADING  0
#define PING_ST_OK       1
#define PING_ST_FAIL     2
#define PING_ST_ERROR    3

static HWND g_ping_wnd = NULL;

static DWORD WINAPI SrvPingThread(LPVOID arg)
{
    SrvPingArg *a = (SrvPingArg*)arg;
    HWND hw    = a->hw;
    char host[256]; strncpy(host, a->host, sizeof(host)-1);
    free(a);

    SrvPingResult *r = (SrvPingResult*)calloc(1, sizeof(SrvPingResult));
    if (!r) return 0;
    strncpy(r->host, host, sizeof(r->host)-1);

    /* Resolve: tenta inet_addr primeiro (IP direto), depois DNS */
    DWORD destIP = inet_addr(host);
    if (destIP != INADDR_NONE) {
        strncpy(r->ip_str, host, sizeof(r->ip_str)-1);
    } else {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            snprintf(r->error, sizeof(r->error),
                "Nao foi possivel resolver o endereco:\n\"%s\"\n\nVerifique o servidor nas configuracoes.",
                host);
            PostMessageA(hw, WM_PING_RESULT, 0, (LPARAM)r);
            return 0;
        }
        destIP = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
        strncpy(r->ip_str, inet_ntoa(((struct sockaddr_in*)res->ai_addr)->sin_addr), sizeof(r->ip_str)-1);
        freeaddrinfo(res);
    }

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        snprintf(r->error, sizeof(r->error), "Erro ao criar handle ICMP.");
        PostMessageA(hw, WM_PING_RESULT, 0, (LPARAM)r);
        return 0;
    }

    char send_buf[8] = "RLSPING";
    BYTE recv_buf[256];
    for (int i = 0; i < 4; i++) {
        memset(recv_buf, 0, sizeof(recv_buf));
        DWORD ret = IcmpSendEcho(hIcmp, destIP, send_buf, 8,
                                 NULL, recv_buf, sizeof(recv_buf), 2000);
        if (ret > 0) {
            ICMP_ECHO_REPLY *rep = (ICMP_ECHO_REPLY*)recv_buf;
            r->latency[i] = rep->RoundTripTime;
            r->success++;
        } else {
            r->latency[i] = 0;  /* 0 = timeout */
        }
        Sleep(200);
    }
    IcmpCloseHandle(hIcmp);

    PostMessageA(hw, WM_PING_RESULT, 0, (LPARAM)r);
    return 0;
}

static HBRUSH s_br_green  = NULL;   /* banner ACESSIVEL */
static HBRUSH s_br_red    = NULL;   /* banner INACESSIVEL */
static HBRUSH s_br_lgreen = NULL;   /* fundo claro linhas OK */
static HBRUSH s_br_lred   = NULL;   /* fundo claro linhas Timeout */
static HBRUSH s_br_face   = NULL;   /* COLOR_BTNFACE */

static void PingEnsureBrushes(void) {
    if (!s_br_green)  s_br_green  = CreateSolidBrush(RGB(39,174,96));
    if (!s_br_red)    s_br_red    = CreateSolidBrush(RGB(192,57,43));
    if (!s_br_lgreen) s_br_lgreen = CreateSolidBrush(RGB(232,245,233));
    if (!s_br_lred)   s_br_lred   = CreateSolidBrush(RGB(253,232,230));
    if (!s_br_face)   s_br_face   = GetSysColorBrush(COLOR_BTNFACE);
}

static LRESULT CALLBACK PingTestWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        PingEnsureBrushes();
        HINSTANCE hI = ((CREATESTRUCTA*)lp)->hInstance;
        HFONT f  = g_font_ui;
        HFONT fb = MakeFont(20, TRUE, FALSE);  /* bold para banner */

        /* Banner — fundo colorido com texto estado */
        HWND hbn = CreateWindowExA(0, "STATIC", "  A testar servidor...",
            WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
            0, 0, 484, 56,
            hw, (HMENU)(UINT_PTR)IDC_PING_BANNER, hI, NULL);
        SendMessageA(hbn, WM_SETFONT, (WPARAM)fb, 0);

        /* Servidor info */
        HWND hho = CreateWindowExA(0, "STATIC", "Aguardando...",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            16, 66, 450, 22,
            hw, (HMENU)(UINT_PTR)IDC_PING_HOST, hI, NULL);
        SendMessageA(hho, WM_SETFONT, (WPARAM)f, 0);

        /* 4 linhas de ping */
        int ids[] = {IDC_PING_L1, IDC_PING_L2, IDC_PING_L3, IDC_PING_L4};
        for (int i = 0; i < 4; i++) {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "  Ping %d: ---", i+1);
            HWND hl = CreateWindowExA(WS_EX_CLIENTEDGE, "STATIC", tmp,
                WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
                16, 98 + i*32, 210, 26,
                hw, (HMENU)(UINT_PTR)ids[i], hI, NULL);
            SendMessageA(hl, WM_SETFONT, (WPARAM)f, 0);
        }

        /* Summary */
        HWND hsu = CreateWindowExA(0, "STATIC", "",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            240, 98, 226, 128,
            hw, (HMENU)(UINT_PTR)IDC_PING_SUMMARY, hI, NULL);
        SendMessageA(hsu, WM_SETFONT, (WPARAM)f, 0);

        /* Fechar (desabilitado ate resultado) */
        HWND hcl = CreateWindowA("BUTTON", "Fechar",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
            172, 272, 140, 36,
            hw, (HMENU)(UINT_PTR)IDC_PING_CLOSE, hI, NULL);
        SendMessageA(hcl, WM_SETFONT, (WPARAM)f, 0);
        EnableWindow(hcl, FALSE);
        return 0;
    }
    case WM_PING_RESULT: {
        SrvPingResult *r = (SrvPingResult*)lp;
        if (!r) break;

        int state;
        if (r->error[0]) {
            /* erro de resolucao/ICMP */
            SetDlgItemTextA(hw, IDC_PING_BANNER,  "  ERRO  -  Servidor Inacessivel");
            SetDlgItemTextA(hw, IDC_PING_HOST,    r->error);
            SetDlgItemTextA(hw, IDC_PING_SUMMARY, "");
            state = PING_ST_ERROR;
        } else {
            /* atualiza host */
            char hinfo[300];
            if (strcmp(r->host, r->ip_str) == 0)
                snprintf(hinfo, sizeof(hinfo), "Servidor: %s", r->host);
            else
                snprintf(hinfo, sizeof(hinfo), "Servidor: %s   [IP: %s]", r->host, r->ip_str);
            SetDlgItemTextA(hw, IDC_PING_HOST, hinfo);

            /* atualiza linhas de ping */
            int lineIds[] = {IDC_PING_L1, IDC_PING_L2, IDC_PING_L3, IDC_PING_L4};
            for (int i = 0; i < 4; i++) {
                HWND hl = GetDlgItem(hw, lineIds[i]);
                char tmp[48];
                if (r->latency[i] > 0) {
                    snprintf(tmp, sizeof(tmp), "  Ping %d:  %lu ms", i+1, (unsigned long)r->latency[i]);
                    SetWindowLongPtrA(hl, GWLP_USERDATA, 1);  /* OK - verde */
                } else {
                    snprintf(tmp, sizeof(tmp), "  Ping %d:  Timeout", i+1);
                    SetWindowLongPtrA(hl, GWLP_USERDATA, 2);  /* fail - vermelho */
                }
                SetWindowTextA(hl, tmp);
                InvalidateRect(hl, NULL, TRUE);
            }

            /* summary */
            char sumtxt[256];
            if (r->success > 0) {
                DWORD sum = 0;
                for (int i = 0; i < 4; i++) sum += r->latency[i];
                snprintf(sumtxt, sizeof(sumtxt),
                    "Enviados:    4\nRecebidos:  %d\nPerdidos:    %d\n\nMedia: %lu ms",
                    r->success, 4 - r->success,
                    (unsigned long)(sum / r->success));
            } else {
                snprintf(sumtxt, sizeof(sumtxt),
                    "Enviados:    4\nRecebidos:  0\nPerdidos:    4\n\nServidor nao\nresponde");
            }
            SetDlgItemTextA(hw, IDC_PING_SUMMARY, sumtxt);

            /* banner texto + estado */
            if (r->success == 4)
                SetDlgItemTextA(hw, IDC_PING_BANNER, "   ACESSIVEL   -   Todos os pings OK");
            else if (r->success > 0)
                SetDlgItemTextA(hw, IDC_PING_BANNER, "   ACESSIVEL   -   Com perdas de pacotes");
            else
                SetDlgItemTextA(hw, IDC_PING_BANNER, "   INACESSIVEL   -   Servidor nao responde");

            state = (r->success > 0) ? PING_ST_OK : PING_ST_FAIL;
        }
        free(r);

        SetWindowLongPtrA(hw, GWLP_USERDATA, (LONG_PTR)state);
        InvalidateRect(hw, NULL, TRUE);
        HWND hcl = GetDlgItem(hw, IDC_PING_CLOSE);
        if (hcl) EnableWindow(hcl, TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc  = (HDC)wp;
        HWND hct = (HWND)lp;
        int cid  = GetDlgCtrlID(hct);
        int wstate = (int)GetWindowLongPtrA(hw, GWLP_USERDATA);

        if (cid == IDC_PING_BANNER) {
            BOOL ok = (wstate == PING_ST_OK);
            BOOL loading = (wstate == PING_ST_LOADING);
            SetTextColor(hdc, loading ? RGB(80,80,80) : RGB(255,255,255));
            SetBkMode(hdc, OPAQUE);
            if (loading) {
                SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
                return (LRESULT)s_br_face;
            }
            SetBkColor(hdc, ok ? RGB(39,174,96) : RGB(192,57,43));
            return (LRESULT)(ok ? s_br_green : s_br_red);
        }
        /* ping lines */
        int lineState = (int)GetWindowLongPtrA(hct, GWLP_USERDATA);
        if (cid >= IDC_PING_L1 && cid <= IDC_PING_L4) {
            if (lineState == 1) {
                SetTextColor(hdc, RGB(27,153,66));
                SetBkColor(hdc, RGB(232,245,233));
                SetBkMode(hdc, OPAQUE);
                return (LRESULT)s_br_lgreen;
            } else if (lineState == 2) {
                SetTextColor(hdc, RGB(160,40,30));
                SetBkColor(hdc, RGB(253,232,230));
                SetBkMode(hdc, OPAQUE);
                return (LRESULT)s_br_lred;
            }
        }
        /* summary com cor do estado */
        if (cid == IDC_PING_SUMMARY) {
            if (wstate == PING_ST_OK)   SetTextColor(hdc, RGB(27,153,66));
            if (wstate == PING_ST_FAIL || wstate == PING_ST_ERROR)
                                        SetTextColor(hdc, RGB(160,40,30));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)s_br_face;
        }
        return (LRESULT)NULL;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PING_CLOSE || LOWORD(wp) == IDCANCEL) {
            HWND parent = GetWindow(hw, GW_OWNER);
            if (parent) EnableWindow(parent, TRUE);
            DestroyWindow(hw);
            g_ping_wnd = NULL;
        }
        return 0;
    case WM_CLOSE:
        SendMessageA(hw, WM_COMMAND, IDCANCEL, 0);
        return 0;
    case WM_DESTROY:
        g_ping_wnd = NULL;
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void ShowPingTestDlg(HWND parent)
{
    if (g_ping_wnd) { SetForegroundWindow(g_ping_wnd); return; }
    if (!g_cfg.host[0]) {
        MessageBoxA(parent,
            "Nenhum servidor configurado.\nConfigure o servidor VPN primeiro.",
            "Teste de Servidor", MB_OK|MB_ICONWARNING);
        return;
    }

    HINSTANCE hI = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    static BOOL reg = FALSE;
    if (!reg) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = PingTestWndProc;
        wc.hInstance     = hI;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.lpszClassName = "RLS_PING";
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        RegisterClassA(&wc);
        reg = TRUE;
    }

    int W=484, H=360;
    RECT rc; GetWindowRect(parent, &rc);
    int X = rc.left + (rc.right-rc.left-W)/2;
    int Y = rc.top  + (rc.bottom-rc.top-H)/2;

    g_ping_wnd = CreateWindowExA(WS_EX_DLGMODALFRAME,
        "RLS_PING", "Teste de Servidor",
        WS_POPUP|WS_CAPTION|WS_SYSMENU,
        X, Y, W, H, parent, NULL, hI, NULL);

    EnableWindow(parent, FALSE);
    ShowWindow(g_ping_wnd, SW_SHOW);

    /* Start ping thread */
    SrvPingArg *arg = (SrvPingArg*)calloc(1, sizeof(SrvPingArg));
    if (arg) {
        arg->hw   = g_ping_wnd;
        arg->port = g_cfg.port;
        strncpy(arg->host, g_cfg.host, sizeof(arg->host)-1);
        HANDLE ht = CreateThread(NULL, 0, SrvPingThread, arg, 0, NULL);
        if (ht) CloseHandle(ht);
        else { free(arg); }
    }
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
        AppendMenuA(mConn,MF_STRING,IDM_ADD_CONN,  "Adicionar Conexao...");
        AppendMenuA(mConn,MF_STRING,IDM_DEL_CONN,  "Remover Conexao Selecionada");
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
        AppendMenuA(mView,MF_STRING,IDM_PINGTEST,"Teste de Servidor...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mView,"Ver");

        HMENU mAdap=CreatePopupMenu();
        AppendMenuA(mAdap,MF_STRING,IDM_APPLYIP,"Aplicar IP Estatico...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mAdap,"Adaptador VPN");

        HMENU mTools=CreatePopupMenu();
        AppendMenuA(mTools,MF_STRING,IDM_SCAN,"Scan de Rede...");
        AppendMenuA(mTools,MF_STRING,IDM_PINGTEST,"Teste de Servidor...");
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
            const char *cols[]={"Nome da Ligacao VPN","Estado","Servidor VPN","Hub Virtual","Adaptador Virtual","Servidor"};
            const int   wids[]={240,150,260,140,160,120};
            g_list_conns=MakeListView(hwnd,IDC_LIST_CONNS,cols,wids,6);
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
        profiles_init(&g_profiles);
        g_sel_idx = 0;
        /* Carregar config do primeiro perfil como activo por defeito */
        if (g_profiles.count > 0)
            profiles_to_cfg(&g_profiles.items[0], &g_cfg);
        else
            vpn_default_config(&g_cfg);
        vpn_set_log_fn(vpn_log_cb,NULL);

        LayoutChildren(hwnd);
        /* Pre-populate ListViews immediately from defaults (user sees data at once) */
        FillListViews(NULL);

        log_append("[RLS VPN v2.0.0] Interface iniciada. Aguardar estado VPN...");

        /* Polling thread (TRUE = nao dorme na primeira iteracao) */
        HANDLE ht=CreateThread(NULL,0,PollThread,(LPVOID)(UINT_PTR)TRUE,0,NULL);
        if(ht) CloseHandle(ht);

        /* Server reachability check thread (ping automatico ao servidor) */
        HANDLE hts=CreateThread(NULL,0,SrvCheckThread,NULL,0,NULL);
        if(hts) CloseHandle(hts);

        return 0;
    }

    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;

    case WM_COMMAND: {
        int id=LOWORD(wp);
        switch(id){
        case IDM_TOGGLE:
            /* Actua sobre o perfil seleccionado na lista */
            if (g_connected && g_profiles.active_idx == g_sel_idx) {
                /* Desligar o perfil activo */
                g_connected = FALSE;
                if (g_sel_idx >= 0 && g_sel_idx < g_profiles.count)
                    profiles_to_cfg(&g_profiles.items[g_sel_idx], &g_cfg);
                StartOp(3, NULL, NULL);
            } else {
                /* Ligar o perfil seleccionado (com disconnect automatico se necessario) */
                g_connected = TRUE;
                StartConnect(g_sel_idx);
            }
            if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
            break;
        case IDM_CONNECT:
            StartConnect(g_sel_idx);
            break;
        case IDM_DISCONNECT:
            if (g_sel_idx >= 0 && g_sel_idx < g_profiles.count)
                profiles_to_cfg(&g_profiles.items[g_sel_idx], &g_cfg);
            StartOp(3,NULL,NULL);
            break;
        case IDM_ADD_CONN:
            OpenConfigDlg(hwnd, TRUE);
            break;
        case IDM_DEL_CONN: {
            int idx = g_sel_idx;
            if (idx < 0 || idx >= g_profiles.count) break;
            /* Impedir remocao do perfil activo enquanto ligado */
            if (g_connected && g_profiles.active_idx == idx) {
                MessageBoxA(hwnd,
                    "Nao e possivel remover uma conexao activa.\nDesliga primeiro.",
                    "Remover Conexao", MB_ICONWARNING);
                break;
            }
            char msg[128];
            snprintf(msg, sizeof msg,
                "Remover a conexao \"%s\"?",
                g_profiles.items[idx].name[0] ? g_profiles.items[idx].name : "(sem nome)");
            if (MessageBoxA(hwnd, msg, "Remover Conexao", MB_YESNO|MB_ICONQUESTION) == IDYES) {
                profiles_remove(&g_profiles, idx);
                profiles_save(&g_profiles);
                if (g_sel_idx >= g_profiles.count && g_profiles.count > 0)
                    g_sel_idx = g_profiles.count - 1;
                else if (g_profiles.count == 0)
                    g_sel_idx = 0;
                if (g_profiles.count > 0)
                    profiles_to_cfg(&g_profiles.items[g_sel_idx], &g_cfg);
                FillListViews(NULL);
                log_append("[CONFIG] Conexao removida.");
            }
            break;
        }
        case IDM_SETUP:      StartOp(1,NULL,NULL); break;
        case IDM_RESET:
            if (AskResetPassword(hwnd)) StartOp(4,NULL,NULL);
            break;
        case IDM_SCAN:       ShowScanDlg(hwnd);    break;
        case IDM_PINGTEST:   ShowPingTestDlg(hwnd); break;
        case IDM_SAVE_CFG:
        case IDM_APPLYIP:    ShowConfigDlg(hwnd);  break;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                "RLS Automacao VPN Client v2.0.0\n\n"
                "Multi-Profile Manager:\n"
                "  * Botao '+ Conexao' para adicionar novos servidores VPN.\n"
                "  * Selecciona uma linha na lista e clica 'LIGAR'.\n"
                "  * Se outro perfil estiver activo, e automaticamente\n"
                "    desligado antes de ligar o novo.\n"
                "  * Uma unica NIC virtual e partilhada por todos os perfis.\n"
                "  * Configuracoes gravadas em JSON local (%APPDATA%\\RLS_Automacao).\n\n"
                "Fluxo de utilizacao:\n"
                "  1. '+ Conexao' -> preencher Servidor / Hub / User / Password\n"
                "     -> 'Criar Conta VPN' (regista no SoftEther)\n"
                "  2. Seleccionar a linha -> clicar 'LIGAR'\n"
                "  3. Clicar 'DESLIGAR' para terminar\n\n"
                "Interface Win32 nativa. Zero WebView2.\n"
                "Todo o output do SoftEther visivel no log.\n\n"
                "(c) 2026 RLS Automacao",
                "Sobre RLS VPN v2.0.0", MB_ICONINFORMATION);
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
        /* Actualizar active_idx e g_connected conforme resultado */
        if (g_last_op == 2 || g_last_op == 7) {
            if (ok) {
                g_connected = TRUE;
                g_profiles.active_idx = g_sel_idx;
            } else {
                g_connected = FALSE;
                g_profiles.active_idx = -1;
            }
        } else if (g_last_op == 3) {
            g_connected = FALSE;
            g_profiles.active_idx = -1;
        }
        if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
        if(t){
            log_append(t);
            sb_set(1,t);
            if (strstr(t, "nao esta instalado") || strstr(t, "nao encontrado")) {
                MessageBoxA(g_hwnd, t,
                    "SoftEther VPN nao instalado", MB_ICONWARNING|MB_OK);
            }
            free((void*)t);
        }
        if (g_last_op == 1 || g_last_op == 2 || g_last_op == 7) {
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

        /* ── Seleccao de linha na lista de conexoes ───────────────── */
        if ((hdr->code == NM_CLICK || hdr->code == LVN_ITEMCHANGED)
             && hdr->hwndFrom == g_list_conns) {
            int sel = ListView_GetNextItem(g_list_conns, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < g_profiles.count && sel != g_sel_idx) {
                g_sel_idx = sel;
                /* So actualiza g_cfg se nao estiver ligado a outro perfil */
                if (g_profiles.active_idx < 0 || g_profiles.active_idx == sel)
                    profiles_to_cfg(&g_profiles.items[sel], &g_cfg);
                char info[128];
                snprintf(info, sizeof info,
                    "  Perfil: %s",
                    g_profiles.items[sel].name[0]
                        ? g_profiles.items[sel].name : "(sem nome)");
                sb_set(1, info);
            }
        }

        /* ── Duplo-clique na lista de conexoes: ligar ─────────────── */
        if (hdr->code == NM_DBLCLK && hdr->hwndFrom == g_list_conns) {
            NMITEMACTIVATE *nma = (NMITEMACTIVATE*)lp;
            int row = nma->iItem;
            if (row >= 0 && row < g_profiles.count) {
                g_sel_idx = row;
                profiles_to_cfg(&g_profiles.items[row], &g_cfg);
                /* Se ja esta ligado neste perfil, desliga; senao liga */
                if (g_connected && g_profiles.active_idx == row) {
                    g_connected = FALSE;
                    StartOp(3, NULL, NULL);
                } else {
                    g_connected = TRUE;
                    StartConnect(row);
                }
                if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
            }
            return 0;
        }

        /* ── Botao direito na lista de conexoes: menu contextual ──── */
        if (hdr->code == NM_RCLICK && hdr->hwndFrom == g_list_conns) {
            /* Determinar linha clicada */
            NMITEMACTIVATE *nma = (NMITEMACTIVATE*)lp;
            int row = nma->iItem;
            if (row < 0)
                row = ListView_GetNextItem(g_list_conns, -1, LVNI_SELECTED);

            if (row >= 0 && row < g_profiles.count) {
                /* Actualizar seleccao para a linha clicada */
                g_sel_idx = row;
                ListView_SetItemState(g_list_conns, row,
                    LVIS_SELECTED|LVIS_FOCUSED, LVIS_SELECTED|LVIS_FOCUSED);

                BOOL is_active    = (g_profiles.active_idx == row);
                BOOL conn_this    = (is_active && g_connected);
                BOOL busy         = g_busy;

                HMENU hm = CreatePopupMenu();

                /* Ligar — disponivel se nao estiver ja ligado aqui e nao busy */
                AppendMenuA(hm,
                    (!conn_this && !busy) ? MF_STRING : (MF_STRING|MF_GRAYED),
                    IDM_CONNECT, "Ligar");

                /* Desligar — disponivel apenas se este perfil estiver activo */
                AppendMenuA(hm,
                    (conn_this && !busy) ? MF_STRING : (MF_STRING|MF_GRAYED),
                    IDM_DISCONNECT, "Desligar");

                AppendMenuA(hm, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hm, MF_STRING, IDM_SAVE_CFG, "Editar configuracao...");
                AppendMenuA(hm, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hm,
                    (!conn_this) ? MF_STRING : (MF_STRING|MF_GRAYED),
                    IDM_DEL_CONN, "Remover conexao");

                POINT pt; GetCursorPos(&pt);
                /* TPM_RETURNCMD: devolve o ID escolhido sem disparar WM_COMMAND */
                int cmd = TrackPopupMenu(hm,
                    TPM_RIGHTBUTTON|TPM_RETURNCMD|TPM_NONOTIFY,
                    pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hm);

                if (cmd == IDM_CONNECT) {
                    if (!g_busy) {
                        g_connected = TRUE;
                        profiles_to_cfg(&g_profiles.items[row], &g_cfg);
                        StartConnect(row);
                        if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
                    }
                } else if (cmd == IDM_DISCONNECT) {
                    if (!g_busy) {
                        g_connected = FALSE;
                        profiles_to_cfg(&g_profiles.items[row], &g_cfg);
                        StartOp(3, NULL, NULL);
                        if (g_btn_toggle) { InvalidateRect(g_btn_toggle,NULL,TRUE); UpdateWindow(g_btn_toggle); }
                    }
                } else if (cmd == IDM_SAVE_CFG) {
                    ShowConfigDlg(hwnd);
                } else if (cmd == IDM_DEL_CONN) {
                    PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(IDM_DEL_CONN,0), 0);
                }
            }
            return 0;
        }

        if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == g_list_conns) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW*)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                return CDRF_NOTIFYSUBITEMDRAW;
            if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                /* Col 1: Estado ligacao — "Connected"=verde, "Offline"=vermelho */
                if (cd->iSubItem == 1) {
                    char txt[64] = {0};
                    ListView_GetItemText(g_list_conns,
                                        (int)cd->nmcd.dwItemSpec, 1,
                                        txt, (int)sizeof(txt) - 1);
                    if (strstr(txt, "Connected")) {
                        cd->clrTextBk = RGB(0, 160, 0);
                        cd->clrText   = RGB(255, 255, 255);
                    } else if (strstr(txt, "Offline")) {
                        cd->clrTextBk = RGB(200, 0, 0);
                        cd->clrText   = RGB(255, 255, 255);
                    } else {
                        cd->clrTextBk = CLR_DEFAULT;
                        cd->clrText   = CLR_DEFAULT;
                    }
                    return CDRF_NEWFONT;
                }
                /* Col 5: Servidor — "Online"=verde, "Offline"=vermelho */
                if (cd->iSubItem == 5) {
                    char txt[32] = {0};
                    ListView_GetItemText(g_list_conns,
                                        (int)cd->nmcd.dwItemSpec, 5,
                                        txt, (int)sizeof(txt) - 1);
                    if (strstr(txt, "Online")) {
                        cd->clrTextBk = RGB(0, 160, 0);
                        cd->clrText   = RGB(255, 255, 255);
                    } else if (strstr(txt, "Offline")) {
                        cd->clrTextBk = RGB(200, 0, 0);
                        cd->clrText   = RGB(255, 255, 255);
                    } else {
                        cd->clrTextBk = CLR_DEFAULT;
                        cd->clrText   = CLR_DEFAULT;
                    }
                    return CDRF_NEWFONT;
                }
            }
        }
        return CDRF_DODEFAULT;
    }

    case WM_SRV_PING:
        /* Resultado do ping automatico ao servidor VPN */
        g_srv_reachable = (int)wp;
        {
            int active = g_profiles.active_idx;
            if (active >= 0 && active < ListView_GetItemCount(g_list_conns)) {
                const char *s = (g_srv_reachable == 1) ? "Online" : "Offline";
                ListView_SetItemText(g_list_conns, active, 5, (char*)s);
                ListView_RedrawItems(g_list_conns, active, active);
                UpdateWindow(g_list_conns);
            }
        }
        return 0;

    case WM_DESTROY:
        g_hwnd=NULL;
        /* Desligar VPN ao fechar a aplicacao */
        if (g_connected) {
            char out[256]={0};
            /* Usa a config do perfil activo */
            if (g_profiles.active_idx >= 0 && g_profiles.active_idx < g_profiles.count)
                profiles_to_cfg(&g_profiles.items[g_profiles.active_idx], &g_cfg);
            vpn_disconnect(&g_cfg, out, sizeof out);
        }
        profiles_save(&g_profiles);
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
        "RLS Automacao VPN Client v2.0.0",
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