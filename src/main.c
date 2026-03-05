/*
 * main.c - RLS Automacao VPN Client v1.5.0
 * Interface estilo SoftEther: menu bar, ListViews, status bar, toolbar.
 * Polling de estado em thread dedicada - UI thread NUNCA bloqueia.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
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

/* ---- Control IDs -------------------------------------------------------- */
#define IDC_LIST_CONNS    200
#define IDC_LIST_ADAPTERS 201
#define IDC_LOG           202
#define IDC_STATUSBAR     203
#define IDC_TOOLBAR        204

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
static BOOL     g_log_vis    = TRUE;
static VpnConfig g_cfg;
static VpnState  g_state     = VPN_NOT_INSTALLED;
static HFONT    g_font_ui    = NULL;
static HFONT    g_font_mono  = NULL;

/* ---- Forward declarations ----------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
static LRESULT CALLBACK ToolbarWndProc(HWND,UINT,WPARAM,LPARAM);
static void LayoutChildren(HWND);

/* ---- Toolbar button definitions ----------------------------------------- */
static const struct { int id; const char *label; } g_tbBtns[] = {
    {IDM_CONNECT,    "Ligar"},
    {IDM_DISCONNECT, "Desligar"},
    {IDM_SETUP,      "Config NIC"},
    {IDM_RESET,      "Reset"},
    {IDM_INSTALL_SE, "Instalar SE"},
    {IDM_SAVE_CFG,   "Configuracoes"},
    {IDM_SHOW_LOG,   "Log"},
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

static DWORD WINAPI WorkerThread(LPVOID p)
{
    WorkData *d = (WorkData*)p;
    int ok = 0;
    char msg[512] = {0};

    switch(d->op) {
    case 1:
        ok = (vpn_setup(&d->cfg, msg, sizeof msg) == 0);
        if (!msg[0]) strncpy(msg, ok?"NIC configurada OK.":"Falha config NIC.", sizeof msg-1);
        break;
    case 2:
        ok = (vpn_connect(&d->cfg, msg, sizeof msg) == 0);
        if (!msg[0]) strncpy(msg, ok?"Ligacao estabelecida.":"Falha ao ligar.", sizeof msg-1);
        break;
    case 3:
        vpn_disconnect(&d->cfg, msg, sizeof msg);
        ok = 1;
        if (!msg[0]) strncpy(msg, "VPN desligada.", sizeof msg-1);
        break;
    case 4:
        vpn_reset(&d->cfg, msg, sizeof msg);
        ok = 1;
        if (!msg[0]) strncpy(msg, "Reset concluido.", sizeof msg-1);
        break;
    case 5: {
        char se_dir[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, se_dir, MAX_PATH);
        char *sl = strrchr(se_dir, '\\');
        if (sl) *(sl+1) = '\0';
        int r = vpn_install_silent(se_dir);
        if (r != 0) {
            strncpy(msg, "Falha ao instalar SoftEther.", sizeof msg-1);
            break;
        }
        vpn_start_service();
        ok = (vpn_setup(&d->cfg, msg, sizeof msg) == 0);
        if (!msg[0]) strncpy(msg,
            ok?"SoftEther instalado e configurado.":"Instalado, config falhou.",
            sizeof msg-1);
        break;
    }
    case 6:
        ok = (vpn_set_static_ip(d->ip, d->mask, msg, sizeof msg) == 0);
        if (!msg[0]) strncpy(msg,
            ok?"IP estatico aplicado.":"Falha ao aplicar IP.", sizeof msg-1);
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
   Update ListViews from VpnStatus (runs on UI thread from WM_VPN_STATUS)
   ====================================================================== */
static void UpdateUI(const VpnStatus *st)
{
    g_state = st->state;

    /* -- ListView: ligacoes ------------------------------------------- */
    char srv[320];
    snprintf(srv, sizeof srv, "%s:%d  (TCP/IP Direto)", g_cfg.host, g_cfg.port);

    if (ListView_GetItemCount(g_list_conns) == 0) {
        LVITEMA lvi = {0};
        lvi.mask     = LVIF_TEXT;
        lvi.pszText  = g_cfg.account_name;
        ListView_InsertItem(g_list_conns, &lvi);
    }
    ListView_SetItemText(g_list_conns, 0, 0, g_cfg.account_name);
    ListView_SetItemText(g_list_conns, 0, 1, (char*)StateStr(st->state));
    ListView_SetItemText(g_list_conns, 0, 2, srv);
    ListView_SetItemText(g_list_conns, 0, 3, g_cfg.hub);
    ListView_SetItemText(g_list_conns, 0, 4, NIC_NAME);

    /* -- ListView: adaptadores ---------------------------------------- */
    char ip_buf[64] = "---";
    if (st->local_ip[0]) strncpy(ip_buf, st->local_ip, sizeof ip_buf-1);

    if (ListView_GetItemCount(g_list_adaps) == 0) {
        LVITEMA lvi = {0};
        lvi.mask    = LVIF_TEXT;
        lvi.pszText = NIC_NAME;
        ListView_InsertItem(g_list_adaps, &lvi);
    }
    ListView_SetItemText(g_list_adaps, 0, 0, NIC_NAME);
    ListView_SetItemText(g_list_adaps, 0, 1,
        (char*)(st->state==VPN_CONNECTED?"Ligado":"Desligado"));
    ListView_SetItemText(g_list_adaps, 0, 2, ip_buf);

    /* -- Status bar --------------------------------------------------- */
    char sb0[80];
    snprintf(sb0, sizeof sb0, "  Estado: %s", StateStr(st->state));
    sb_set(0, sb0);
    if (!g_busy) sb_set(1, st->message[0] ? st->message : "  Pronto");
    char sb2[80];
    snprintf(sb2, sizeof sb2, "  IP VPN: %s", ip_buf);
    sb_set(2, sb2);
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
        if (id == IDOK) {
            char tmp[256];
            GetDlgItemTextA(hw, IDC_CFG_HOST, g_cfg.host, sizeof g_cfg.host);
            GetDlgItemTextA(hw, IDC_CFG_PORT, tmp, sizeof tmp);
            int p = atoi(tmp); if (p>0) g_cfg.port=(unsigned short)p;
            GetDlgItemTextA(hw, IDC_CFG_HUB,  g_cfg.hub,      sizeof g_cfg.hub);
            GetDlgItemTextA(hw, IDC_CFG_USER, g_cfg.username,  sizeof g_cfg.username);
            GetDlgItemTextA(hw, IDC_CFG_PASS, g_cfg.password,  sizeof g_cfg.password);
            log_append("[CONFIG] Configuracao guardada.");
            sb_set(1, "  Configuracao guardada.");
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(hw);
            g_cfg_wnd = NULL;
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

    int W=440, H=320;
    RECT rc; GetWindowRect(parent,&rc);
    int X = rc.left+(rc.right-rc.left-W)/2;
    int Y = rc.top+(rc.bottom-rc.top-H)/2;

    g_cfg_wnd = CreateWindowExA(WS_EX_DLGMODALFRAME,
        "RLS_CFG", "Configuracoes VPN",
        WS_POPUP|WS_CAPTION|WS_SYSMENU,
        X,Y,W,H, parent, NULL, hI, NULL);

    HFONT f = MakeFont(15,FALSE,FALSE);
    char ps[8]; snprintf(ps,sizeof ps,"%d",g_cfg.port);

    #define LBL(t,x,y,w,h) do{ HWND _h=CreateWindowA("STATIC",t,WS_CHILD|WS_VISIBLE|SS_RIGHT,x,y,w,h,g_cfg_wnd,NULL,hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)
    #define EDT(id,t,x,y,w,h,ex) do{ HWND _h=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|(ex),x,y,w,h,g_cfg_wnd,(HMENU)(UINT_PTR)(id),hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)
    #define BTN(t,id,x,y,w,h) do{ HWND _h=CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,x,y,w,h,g_cfg_wnd,(HMENU)(UINT_PTR)(id),hI,NULL); SendMessageA(_h,WM_SETFONT,(WPARAM)f,0);}while(0)

    LBL("Servidor:",     10, 16,  90, 20); EDT(IDC_CFG_HOST,g_cfg.host,    105,14,196,22,0);
    LBL("Porta:",       307, 16,  40, 20); EDT(IDC_CFG_PORT,ps,            350,14, 60,22,ES_NUMBER);
    LBL("Hub Virtual:", 10,  50,  90, 20); EDT(IDC_CFG_HUB, g_cfg.hub,     105,48,180,22,0);
    LBL("Utilizador:",  10,  84,  90, 20); EDT(IDC_CFG_USER,g_cfg.username,105,82,180,22,0);
    LBL("Password:",    10, 118,  90, 20); EDT(IDC_CFG_PASS,g_cfg.password,105,116,180,22,ES_PASSWORD);

    LBL("IP Estatico:", 10, 165,  90, 20); EDT(IDC_CFG_IPST,"192.168.10.1",  105,163,130,22,0);
    LBL("Mascara:",    245, 165,  60, 20); EDT(IDC_CFG_MASK,"255.255.255.0", 308,163,100,22,0);
    BTN("Aplicar IP",IDM_APPLYIP,        10,196,120,28);

    BTN("Guardar",   IDOK,               200,262,100,30);
    BTN("Cancelar",  IDCANCEL,           315,262,100,30);

    #undef LBL
    #undef EDT
    #undef BTN

    EnableWindow(parent, FALSE);
    ShowWindow(g_cfg_wnd, SW_SHOW);
    SetFocus(g_cfg_wnd);
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

    /* Status bar first */
    SendMessageA(g_statusbar, WM_SIZE,0,0);
    RECT sbr; GetClientRect(g_statusbar,&sbr);
    int sbH=sbr.bottom;

    int tbH=38;
    int content_y = tbH;
    int content_h = H - sbH - tbH;
    int log_h     = g_log_vis ? 130 : 0;
    int lists_h   = content_h - log_h;
    int top_h     = lists_h * 58 / 100;
    int bot_h     = lists_h - top_h;

    MoveWindow(g_toolbar,    0, 0,            W, tbH,   TRUE);
    MoveWindow(g_list_conns, 0, content_y,    W, top_h, TRUE);
    MoveWindow(g_list_adaps, 0, content_y+top_h, W, bot_h, TRUE);

    if (g_log_vis) {
        ShowWindow(g_log, SW_SHOW);
        MoveWindow(g_log, 0, content_y+lists_h, W, log_h, TRUE);
    } else {
        ShowWindow(g_log, SW_HIDE);
    }

    /* Resize status bar parts */
    int parts[3] = {220, W-180, W};
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
        HFONT f=MakeFont(14,FALSE,FALSE);
        for(int i=0;i<TB_BTN_CNT;i++){
            HWND b=CreateWindowA("BUTTON",g_tbBtns[i].label,
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                6+i*112,5,106,28, hw,
                (HMENU)(UINT_PTR)g_tbBtns[i].id, hI, NULL);
            SendMessageA(b,WM_SETFONT,(WPARAM)f,FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
        PostMessageA(GetParent(hw),WM_COMMAND,wp,lp);
        return 0;
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
        g_font_ui   = MakeFont(15,FALSE,FALSE);
        g_font_mono = MakeFont(13,FALSE,TRUE);

        /* --- Menu bar ------------------------------------------------ */
        HMENU mb=CreateMenu();

        HMENU mConn=CreatePopupMenu();
        AppendMenuA(mConn,MF_STRING,IDM_CONNECT,   "Ligar\tF5");
        AppendMenuA(mConn,MF_STRING,IDM_DISCONNECT,"Desligar\tF6");
        AppendMenuA(mConn,MF_SEPARATOR,0,NULL);
        AppendMenuA(mConn,MF_STRING,IDM_SETUP,     "Configurar NIC");
        AppendMenuA(mConn,MF_STRING,IDM_RESET,     "Reset Configuracao");
        AppendMenuA(mConn,MF_SEPARATOR,0,NULL);
        AppendMenuA(mConn,MF_STRING,IDM_EXIT,      "Sair\tAlt+F4");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mConn,"Ligacao");

        HMENU mEdit=CreatePopupMenu();
        AppendMenuA(mEdit,MF_STRING,IDM_SAVE_CFG,"Configuracoes VPN...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mEdit,"Editar");

        HMENU mView=CreatePopupMenu();
        AppendMenuA(mView,MF_STRING,IDM_SHOW_LOG,"Mostrar/Ocultar Log");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mView,"Ver");

        HMENU mAdap=CreatePopupMenu();
        AppendMenuA(mAdap,MF_STRING,IDM_APPLYIP,"Aplicar IP Estatico...");
        AppendMenuA(mb,MF_POPUP,(UINT_PTR)mAdap,"Adaptador VPN");

        HMENU mTools=CreatePopupMenu();
        AppendMenuA(mTools,MF_STRING,IDM_INSTALL_SE,"Instalar SoftEther VPN...");
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
            0,0,100,38, hwnd,(HMENU)(UINT_PTR)IDC_TOOLBAR,hI,NULL);

        /* --- ListViews ----------------------------------------------- */
        {
            const char *cols[]={"Nome da Ligacao VPN","Estado","Servidor VPN","Hub Virtual","Adaptador Virtual"};
            const int   wids[]={220,130,260,120,150};
            g_list_conns=MakeListView(hwnd,IDC_LIST_CONNS,cols,wids,5);
        }
        {
            const char *cols[]={"Adaptador Virtual","Estado","Endereco IP"};
            const int   wids[]={240,130,200};
            g_list_adaps=MakeListView(hwnd,IDC_LIST_ADAPTERS,cols,wids,3);
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
        sb_set(0,"  Iniciando...");
        sb_set(1,"  Aguardar...");
        sb_set(2,"  IP: ---");

        /* --- Init VPN ----------------------------------------------- */
        vpn_default_config(&g_cfg);
        vpn_set_log_fn(vpn_log_cb,NULL);

        LayoutChildren(hwnd);
        log_append("[RLS VPN v1.5.0] Interface iniciada.");

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
        case IDM_CONNECT:    StartOp(2,NULL,NULL); break;
        case IDM_DISCONNECT: StartOp(3,NULL,NULL); break;
        case IDM_SETUP:      StartOp(1,NULL,NULL); break;
        case IDM_RESET:      StartOp(4,NULL,NULL); break;
        case IDM_INSTALL_SE: StartOp(5,NULL,NULL); break;
        case IDM_SAVE_CFG:
        case IDM_APPLYIP:    ShowConfigDlg(hwnd);  break;
        case IDM_SHOW_LOG:
            g_log_vis=!g_log_vis;
            LayoutChildren(hwnd);
            break;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                "RLS Automacao VPN Client\nVersao 1.5.0\n\n"
                "Interface Win32 nativa - zero WebView2.\n"
                "Polling de estado em thread dedicada.\n\n"
                "(c) 2026 RLS Automacao",
                "Sobre",MB_ICONINFORMATION);
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
        g_busy=FALSE;
        if(t){ log_append(t); sb_set(1,t); free((void*)t); }
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

    case WM_DESTROY:
        g_hwnd=NULL;
        DeleteObject(g_font_ui);
        DeleteObject(g_font_mono);
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
        "RLS Automacao VPN Client v1.5.0",
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