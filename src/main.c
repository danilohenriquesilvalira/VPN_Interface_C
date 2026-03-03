/* main.c - RLS Automacao VPN - Pure Win32 C Application
 * Compile: gcc -mwindows -O2 -o rls_vpn.exe main.c vpn.c resource.o -lcomctl32 -lgdi32 -lshcore
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "vpn.h"

/* ─── App metadata ──────────────────────────────────── */
#define APP_TITLE   "RLS Automacao VPN"
#define APP_CLASS   "RLSVPNWnd"
#define APP_VER     "v1.0"

/* ─── Custom messages ────────────────────────────────── */
#define WM_VPN_RESULT  (WM_USER + 100)  /* op finished: wParam=ok, lParam=heap str */
#define WM_VPN_LOG     (WM_USER + 101)  /* just log a message from background thread */

/* ─── Window / Layout constants (logical px @ 96 DPI) ── */
#define WIN_W        480
#define WIN_H        650
#define MG           18
#define CW           (WIN_W - 2 * MG)   /* content width = 444 */

/* Y positions of each section */
#define Y_HDR        0
#define H_HDR        74
#define Y_CARD       82
#define H_CARD       120
#define Y_BTNS       214
#define H_BTN        44
#define Y_INFO       272
#define H_INFO       78
#define Y_STGBTN     360
#define H_STGBTN     30
#define Y_CONTENT    398
#define H_CONTENT    220
#define Y_FOOTER     628

/* ─── Control IDs ───────────────────────────────────── */
enum {
    ID_LBL_STATUS = 200,
    ID_LBL_MSG,
    ID_LBL_IP,
    ID_BTN_SETUP,
    ID_BTN_CONNECT,
    ID_LBL_SERVER,
    ID_LBL_USER,
    ID_LBL_ACCOUNT,
    ID_BTN_SETTINGS,
    ID_LOG,
    /* Settings panel */
    ID_LBL_HOST,  ID_EDT_HOST,
    ID_LBL_PORT,  ID_EDT_PORT,
    ID_LBL_HUB,   ID_EDT_HUB,
    ID_LBL_USER2, ID_EDT_USER,
    ID_LBL_PASS,  ID_EDT_PASS,
    ID_BTN_SAVE,
    ID_BTN_RESET,
};

/* ─── Dark theme colors ─────────────────────────────── */
#define C_BG       RGB(13,  17,  23)
#define C_CARD     RGB(22,  27,  34)
#define C_BORDER   RGB(48,  54,  61)
#define C_TEXT     RGB(230, 237, 243)
#define C_DIM      RGB(139, 148, 158)
#define C_GREEN    RGB(35,  134, 54)
#define C_RED      RGB(218, 54,  51)
#define C_ORANGE   RGB(210, 153, 34)
#define C_GRAY     RGB(110, 118, 129)
#define C_BTN      RGB(33,  38,  45)
#define C_BTN_HL   RGB(48,  54,  61)
#define C_ACCENT   RGB(31,  111, 235)
#define C_EDIT_BG  RGB(13,  17,  23)

/* ─── Global state ──────────────────────────────────── */
static HWND      g_hwnd;
static VpnConfig g_cfg;
static VpnStatus g_status;
static BOOL      g_settings_vis = FALSE;
static BOOL      g_busy         = FALSE;
static VpnState  g_prev_state   = (VpnState)-1;

/* GDI resources */
static HFONT  g_font, g_font_bold, g_font_sm, g_font_mono;
static HBRUSH g_br_bg, g_br_card, g_br_edit;

/* Control handles */
static HWND h_lbl_status, h_lbl_msg, h_lbl_ip;
static HWND h_btn_setup, h_btn_connect;
static HWND h_lbl_server, h_lbl_user, h_lbl_account;
static HWND h_btn_settings, h_log;
static HWND h_lbl_host, h_edt_host;
static HWND h_lbl_port, h_edt_port;
static HWND h_lbl_hub,  h_edt_hub;
static HWND h_lbl_user2,h_edt_user;
static HWND h_lbl_pass, h_edt_pass;
static HWND h_btn_save, h_btn_reset;

/* Forward declarations */
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void CreateControls(HWND hwnd);
static void RefreshUI(HWND hwnd);
static void AddLog(const char *msg);
static void ShowSettingsPanel(BOOL vis);

/* ─── Helpers ───────────────────────────────────────── */

static COLORREF StateColor(VpnState s) {
    switch (s) {
    case VPN_CONNECTED:    return C_GREEN;
    case VPN_CONNECTING:   return C_ORANGE;
    case VPN_DISCONNECTED: return C_RED;
    default:               return C_GRAY;
    }
}

static void AddLog(const char *msg) {
    if (!h_log) return;
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char line[640];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s\r\n",
             lt->tm_hour, lt->tm_min, lt->tm_sec, msg);

    int len = GetWindowTextLengthA(h_log);
    SendMessageA(h_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)line);

    /* Trim to 50 lines max */
    int nlines = (int)SendMessageA(h_log, EM_GETLINECOUNT, 0, 0);
    if (nlines > 50) {
        int start = (int)SendMessageA(h_log, EM_LINEINDEX, 0, 0);
        int end   = (int)SendMessageA(h_log, EM_LINEINDEX, nlines - 50, 0);
        SendMessageA(h_log, EM_SETSEL, (WPARAM)start, (LPARAM)end);
        SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)"");
    }
    SendMessageA(h_log, WM_VSCROLL, SB_BOTTOM, 0);
}

/* Post a log message from a background thread (heap-allocates the string) */
static void PostLog(HWND hwnd, const char *msg) {
    char *heap = (char *)malloc(strlen(msg) + 1);
    if (heap) {
        strcpy(heap, msg);
        PostMessageA(hwnd, WM_VPN_LOG, 0, (LPARAM)heap);
    }
}

static void RefreshUI(HWND hwnd) {
    /* Status label */
    const char *state_str;
    switch (g_status.state) {
    case VPN_CONNECTED:      state_str = "LIGADO";           break;
    case VPN_CONNECTING:     state_str = "A LIGAR...";       break;
    case VPN_DISCONNECTED:   state_str = "DESLIGADO";        break;
    case VPN_NOT_CONFIGURED: state_str = "NAO CONFIGURADO";  break;
    default:                 state_str = "NAO INSTALADO";    break;
    }
    SetWindowTextA(h_lbl_status, state_str);
    SetWindowTextA(h_lbl_msg,    g_status.message);

    /* IP badge */
    if (g_status.local_ip[0]) {
        char iptext[128];
        snprintf(iptext, sizeof(iptext), "  IP: %s", g_status.local_ip);
        SetWindowTextA(h_lbl_ip, iptext);
        ShowWindow(h_lbl_ip, SW_SHOW);
    } else {
        ShowWindow(h_lbl_ip, SW_HIDE);
    }

    /* Info section */
    char info[256];
    snprintf(info, sizeof(info), "  Servidor:    %s:%u", g_cfg.host, g_cfg.port);
    SetWindowTextA(h_lbl_server, info);
    snprintf(info, sizeof(info), "  Utilizador:  %s @ %s", g_cfg.username, g_cfg.hub);
    SetWindowTextA(h_lbl_user, info);
    snprintf(info, sizeof(info), "  Conta VPN:   %s", g_cfg.account_name);
    SetWindowTextA(h_lbl_account, info);

    /* Connect button text */
    SetWindowTextA(h_btn_connect,
        g_status.connected ? "Desligar" : "Ligar");

    /* Enable/disable buttons based on state */
    EnableWindow(h_btn_connect, !g_busy && g_status.connection_ready);
    EnableWindow(h_btn_setup,   !g_busy && g_status.softether_ready);

    /* Force repaint for status card and buttons */
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/* ─── Thread infrastructure ─────────────────────────── */

typedef struct {
    HWND      hwnd;
    VpnConfig cfg;
    char      installer[MAX_PATH];
    int       op; /* 0=install 1=setup 2=connect 3=disconnect 4=reset */
} OpParam;

static DWORD WINAPI WorkerThread(LPVOID param) {
    OpParam *p = (OpParam *)param;
    char msg[512] = {0};
    int  ok  = 0;

    switch (p->op) {
    case 0: /* install */
        ok = vpn_install_silent(p->installer);
        strncpy(msg, ok ? "SoftEther instalado com sucesso!"
                        : "Falha ao instalar SoftEther. Instale manualmente.",
                sizeof(msg) - 1);
        break;
    case 1: /* setup */
        ok = vpn_setup(&p->cfg, msg, sizeof(msg));
        break;
    case 2: /* connect */
        ok = vpn_connect(&p->cfg, msg, sizeof(msg));
        break;
    case 3: /* disconnect */
        ok = vpn_disconnect(&p->cfg, msg, sizeof(msg));
        break;
    case 4: /* reset */
        ok = vpn_reset(&p->cfg, msg, sizeof(msg));
        break;
    }

    /* Send result to main thread (heap string, freed in WndProc) */
    char *heap = (char *)malloc(strlen(msg) + 1);
    if (heap) strcpy(heap, msg);
    PostMessageA(p->hwnd, WM_VPN_RESULT, (WPARAM)ok, (LPARAM)heap);
    free(p);
    return 0;
}

static void StartOp(int op_type, const char *installer) {
    if (g_busy) return;
    g_busy = TRUE;

    OpParam *p = (OpParam *)malloc(sizeof(OpParam));
    if (!p) { g_busy = FALSE; return; }
    p->hwnd = g_hwnd;
    p->cfg  = g_cfg;
    p->op   = op_type;
    p->installer[0] = 0;
    if (installer) strncpy(p->installer, installer, MAX_PATH - 1);

    EnableWindow(h_btn_setup,   FALSE);
    EnableWindow(h_btn_connect, FALSE);

    HANDLE ht = CreateThread(NULL, 0, WorkerThread, p, 0, NULL);
    if (ht) CloseHandle(ht);
    else    { free(p); g_busy = FALSE; }
}

/* ─── Control factory helpers ───────────────────────── */

static HWND MakeLabel(HWND p, int id, const char *txt,
                       int x, int y, int w, int h, HFONT fnt) {
    HWND hw = CreateWindowExA(0, "STATIC", txt,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, p, (HMENU)(INT_PTR)id,
        GetModuleHandleA(NULL), NULL);
    if (fnt) SendMessageA(hw, WM_SETFONT, (WPARAM)fnt, TRUE);
    return hw;
}

static HWND MakeEdit(HWND p, int id, const char *txt,
                      int x, int y, int w, int h, BOOL pw) {
    DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
    if (pw) style |= ES_PASSWORD;
    HWND hw = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", txt,
        style, x, y, w, h, p, (HMENU)(INT_PTR)id,
        GetModuleHandleA(NULL), NULL);
    SendMessageA(hw, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hw;
}

static HWND MakeBtn(HWND p, int id, const char *txt,
                     int x, int y, int w, int h) {
    HWND hw = CreateWindowExA(0, "BUTTON", txt,
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x, y, w, h, p, (HMENU)(INT_PTR)id,
        GetModuleHandleA(NULL), NULL);
    SendMessageA(hw, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hw;
}

/* ─── Create all child controls ────────────────────── */

static void CreateControls(HWND hwnd) {
    HINSTANCE hi = GetModuleHandleA(NULL);

    /* Fonts */
    g_font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_font_bold = CreateFontA(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_font_sm = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_font_mono = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    /* Brushes */
    g_br_bg   = CreateSolidBrush(C_BG);
    g_br_card = CreateSolidBrush(C_CARD);
    g_br_edit = CreateSolidBrush(C_EDIT_BG);

    /* ── Status card labels (positioned inside card area) ── */
    h_lbl_status = MakeLabel(hwnd, ID_LBL_STATUS, "INICIANDO...",
        MG + 28, Y_CARD + 8, CW - 30, 26, g_font_bold);
    h_lbl_msg = MakeLabel(hwnd, ID_LBL_MSG, "",
        MG + 10, Y_CARD + 42, CW - 20, 20, g_font);
    h_lbl_ip = MakeLabel(hwnd, ID_LBL_IP, "",
        MG + 10, Y_CARD + 70, 260, 22, g_font_mono);
    ShowWindow(h_lbl_ip, SW_HIDE);

    /* ── Action buttons ── */
    h_btn_setup = MakeBtn(hwnd, ID_BTN_SETUP, "Configurar",
        MG, Y_BTNS, CW / 2 - 5, H_BTN);
    h_btn_connect = MakeBtn(hwnd, ID_BTN_CONNECT, "Ligar",
        MG + CW / 2 + 5, Y_BTNS, CW / 2 - 5, H_BTN);

    /* ── Info section ── */
    h_lbl_server = MakeLabel(hwnd, ID_LBL_SERVER, "  Servidor:   -",
        MG, Y_INFO + 6, CW, 18, g_font_sm);
    h_lbl_user = MakeLabel(hwnd, ID_LBL_USER, "  Utilizador: -",
        MG, Y_INFO + 30, CW, 18, g_font_sm);
    h_lbl_account = MakeLabel(hwnd, ID_LBL_ACCOUNT, "  Conta VPN:  -",
        MG, Y_INFO + 54, CW, 18, g_font_sm);

    /* ── Settings toggle ── */
    h_btn_settings = MakeBtn(hwnd, ID_BTN_SETTINGS,
        "v  Configuracoes avancadas",
        MG, Y_STGBTN, CW, H_STGBTN);

    /* ── Log area (shown by default) ── */
    h_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        MG, Y_CONTENT, CW, H_CONTENT,
        hwnd, (HMENU)ID_LOG, hi, NULL);
    SendMessageA(h_log, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

    /* ── Settings panel controls (initially hidden) ── */
    int rx = MG + 95;   /* edit field x */
    int rw = CW - 95;   /* edit field width */
    int ry = Y_CONTENT;

    h_lbl_host  = MakeLabel(hwnd, ID_LBL_HOST,  "Servidor:",   MG, ry+7,  90, 18, g_font_sm);
    h_edt_host  = MakeEdit (hwnd, ID_EDT_HOST,  g_cfg.host,    rx, ry,    rw, 26, FALSE);

    h_lbl_port  = MakeLabel(hwnd, ID_LBL_PORT,  "Porta:",      MG, ry+47, 90, 18, g_font_sm);
    h_edt_port  = MakeEdit (hwnd, ID_EDT_PORT,  "443",         rx, ry+40, 80, 26, FALSE);

    h_lbl_hub   = MakeLabel(hwnd, ID_LBL_HUB,   "Hub:",        MG, ry+87, 90, 18, g_font_sm);
    h_edt_hub   = MakeEdit (hwnd, ID_EDT_HUB,   g_cfg.hub,     rx, ry+80, rw, 26, FALSE);

    h_lbl_user2 = MakeLabel(hwnd, ID_LBL_USER2, "Utilizador:", MG, ry+127,90, 18, g_font_sm);
    h_edt_user  = MakeEdit (hwnd, ID_EDT_USER,  g_cfg.username,rx, ry+120,rw, 26, FALSE);

    h_lbl_pass  = MakeLabel(hwnd, ID_LBL_PASS,  "Password:",   MG, ry+167,90, 18, g_font_sm);
    h_edt_pass  = MakeEdit (hwnd, ID_EDT_PASS,  g_cfg.password,rx, ry+160,rw, 26, TRUE);

    h_btn_save  = MakeBtn(hwnd, ID_BTN_SAVE,  "Guardar",
        MG,              ry + 198, CW / 2 - 5, 32);
    h_btn_reset = MakeBtn(hwnd, ID_BTN_RESET, "Limpar instalacao",
        MG + CW / 2 + 5, ry + 198, CW / 2 - 5, 32);

    /* Update port field with actual default */
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", g_cfg.port);
    SetWindowTextA(h_edt_port, portstr);

    /* Hide settings panel initially */
    HWND sc[] = {
        h_lbl_host, h_edt_host, h_lbl_port, h_edt_port,
        h_lbl_hub,  h_edt_hub,  h_lbl_user2,h_edt_user,
        h_lbl_pass, h_edt_pass, h_btn_save,  h_btn_reset, NULL
    };
    for (int i = 0; sc[i]; i++) ShowWindow(sc[i], SW_HIDE);
}

/* ─── Toggle settings panel ─────────────────────────── */

static void ShowSettingsPanel(BOOL vis) {
    g_settings_vis = vis;
    ShowWindow(h_log, vis ? SW_HIDE : SW_SHOW);

    HWND sc[] = {
        h_lbl_host, h_edt_host, h_lbl_port, h_edt_port,
        h_lbl_hub,  h_edt_hub,  h_lbl_user2,h_edt_user,
        h_lbl_pass, h_edt_pass, h_btn_save,  h_btn_reset, NULL
    };
    for (int i = 0; sc[i]; i++) ShowWindow(sc[i], vis ? SW_SHOW : SW_HIDE);

    SetWindowTextA(h_btn_settings,
        vis ? "^  Configuracoes avancadas"
            : "v  Configuracoes avancadas");
}

/* ─── Paint owner-draw button ───────────────────────── */

static void PaintButton(DRAWITEMSTRUCT *dis) {
    HDC   dc  = dis->hDC;
    RECT *rc  = &dis->rcItem;
    BOOL  sel = (dis->itemState & ODS_SELECTED) != 0;
    BOOL  dis_= (dis->itemState & ODS_DISABLED)  != 0;

    char txt[80];
    GetWindowTextA(dis->hwndItem, txt, sizeof(txt));

    COLORREF bg, fg, border;
    if (dis_) {
        bg = C_BTN; fg = C_GRAY; border = C_BORDER;
    } else if (strstr(txt, "Desligar")) {
        bg = sel ? RGB(160, 30, 30) : C_RED;
        fg = C_TEXT; border = bg;
    } else if (strcmp(txt, "Ligar") == 0) {
        bg = sel ? RGB(20, 90, 30) : C_GREEN;
        fg = C_TEXT; border = bg;
    } else if (strstr(txt, "Configurar")) {
        bg = sel ? C_BTN_HL : RGB(38, 44, 52);
        fg = C_TEXT; border = C_BORDER;
    } else {
        bg = sel ? C_BTN_HL : C_BTN;
        fg = C_TEXT; border = C_BORDER;
    }

    /* Fill */
    HBRUSH hbr = CreateSolidBrush(bg);
    FillRect(dc, rc, hbr);
    DeleteObject(hbr);

    /* Border */
    HPEN hp = CreatePen(PS_SOLID, 1, border);
    HPEN op = (HPEN)SelectObject(dc, hp);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RECT br = *rc; br.right--; br.bottom--;
    Rectangle(dc, br.left, br.top, br.right, br.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(hp);

    /* Text */
    HFONT of = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    DrawTextA(dc, txt, -1, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

/* ─── Window procedure ──────────────────────────────── */

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        CreateControls(hwnd);
        /* Status poll every 4 seconds */
        SetTimer(hwnd, 1, 4000, NULL);
        return 0;

    case WM_TIMER:
        if (!g_busy) {
            VpnState prev = g_status.state;
            vpn_get_status(&g_cfg, &g_status);
            if (g_status.state != prev) {
                char logmsg[256];
                snprintf(logmsg, sizeof(logmsg), "Estado: %s", g_status.message);
                AddLog(logmsg);
            }
            RefreshUI(hwnd);
        }
        return 0;

    case WM_VPN_RESULT: {
        char *result_msg = (char *)lp;
        if (result_msg) {
            AddLog(result_msg);
            free(result_msg);
        }
        g_busy = FALSE;
        vpn_get_status(&g_cfg, &g_status);
        RefreshUI(hwnd);
        return 0;
    }

    case WM_VPN_LOG: {
        char *log_msg = (char *)lp;
        if (log_msg) {
            AddLog(log_msg);
            free(log_msg);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case ID_BTN_SETUP:
            AddLog("A configurar placa de rede e conta VPN...");
            StartOp(1, NULL);
            break;

        case ID_BTN_CONNECT:
            if (g_status.connected) {
                AddLog("A desligar VPN...");
                StartOp(3, NULL);
            } else {
                AddLog("A ligar VPN...");
                StartOp(2, NULL);
            }
            break;

        case ID_BTN_SETTINGS:
            ShowSettingsPanel(!g_settings_vis);
            break;

        case ID_BTN_SAVE: {
            GetWindowTextA(h_edt_host, g_cfg.host, sizeof(g_cfg.host));
            char portstr[16];
            GetWindowTextA(h_edt_port, portstr, sizeof(portstr));
            g_cfg.port = (unsigned short)atoi(portstr);
            if (g_cfg.port == 0) g_cfg.port = 443;
            GetWindowTextA(h_edt_hub,  g_cfg.hub,      sizeof(g_cfg.hub));
            GetWindowTextA(h_edt_user, g_cfg.username,  sizeof(g_cfg.username));
            GetWindowTextA(h_edt_pass, g_cfg.password,  sizeof(g_cfg.password));
            AddLog("Configuracoes guardadas.");
            RefreshUI(hwnd);
            ShowSettingsPanel(FALSE);
            break;
        }

        case ID_BTN_RESET:
            if (MessageBoxA(hwnd,
                    "Remover conta VPN e placa de rede virtual?\n"
                    "Sera necessario usar 'Configurar' novamente.",
                    "Confirmar Reset",
                    MB_YESNO | MB_ICONWARNING) == IDYES) {
                AddLog("A fazer reset completo...");
                StartOp(4, NULL);
                ShowSettingsPanel(FALSE);
            }
            break;
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; /* prevent flicker, we fill in WM_PAINT */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);

        /* Full background */
        RECT cli;
        GetClientRect(hwnd, &cli);
        FillRect(dc, &cli, g_br_bg);

        /* ── Header: logo circle + title ── */
        {
            HBRUSH br = CreateSolidBrush(C_ACCENT);
            SelectObject(dc, br);
            SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, MG, 13, MG + 48, 61);
            DeleteObject(br);

            /* "V" letter */
            HFONT fnt = CreateFontA(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            HFONT of = (HFONT)SelectObject(dc, fnt);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, C_TEXT);
            RECT vr = { MG, 13, MG + 48, 61 };
            DrawTextA(dc, "V", -1, &vr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
            DeleteObject(fnt);
        }

        /* App name */
        HFONT of = (HFONT)SelectObject(dc, g_font_bold);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
        TextOutA(dc, MG + 58, 18, APP_TITLE, (int)strlen(APP_TITLE));
        SelectObject(dc, g_font_sm);
        SetTextColor(dc, C_DIM);
        TextOutA(dc, MG + 58, 44, APP_VER, (int)strlen(APP_VER));
        SelectObject(dc, of);

        /* ── Status card background + border ── */
        {
            RECT card = { MG, Y_CARD, MG + CW, Y_CARD + H_CARD };
            FillRect(dc, &card, g_br_card);

            COLORREF bc = StateColor(g_status.state);
            HPEN hp = CreatePen(PS_SOLID, 1, bc);
            HPEN op = (HPEN)SelectObject(dc, hp);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, card.left, card.top, card.right, card.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(hp);

            /* Status dot */
            HBRUSH bd = CreateSolidBrush(bc);
            SelectObject(dc, bd);
            SelectObject(dc, GetStockObject(NULL_PEN));
            int dx = MG + 14, dy = Y_CARD + 21, dr = 7;
            Ellipse(dc, dx - dr, dy - dr, dx + dr, dy + dr);
            DeleteObject(bd);
        }

        /* ── Info section border ── */
        {
            HPEN hp = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN op = (HPEN)SelectObject(dc, hp);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            RECT ir = { MG, Y_INFO - 4, MG + CW, Y_INFO + H_INFO };
            Rectangle(dc, ir.left, ir.top, ir.right, ir.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(hp);
        }

        /* ── Separator above settings toggle ── */
        {
            HPEN hp = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN op = (HPEN)SelectObject(dc, hp);
            MoveToEx(dc, MG, Y_STGBTN - 6, NULL);
            LineTo(dc, MG + CW, Y_STGBTN - 6);
            SelectObject(dc, op);
            DeleteObject(hp);
        }

        /* ── Footer ── */
        {
            HFONT fsm = (HFONT)SelectObject(dc, g_font_sm);
            SetTextColor(dc, C_DIM);
            SetBkMode(dc, TRANSPARENT);
            const char *footer = "(c) 2024 RLS Automacao - Todos os direitos reservados";
            RECT fr = { MG, Y_FOOTER, MG + CW, Y_FOOTER + 20 };
            DrawTextA(dc, footer, -1, &fr, DT_CENTER | DT_SINGLELINE);
            SelectObject(dc, fsm);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC  dc   = (HDC)wp;
        HWND ctrl = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);

        /* Status label: color matches VPN state */
        if (ctrl == h_lbl_status) {
            SetTextColor(dc, StateColor(g_status.state));
            return (LRESULT)g_br_card;
        }
        /* IP: green */
        if (ctrl == h_lbl_ip) {
            SetTextColor(dc, C_GREEN);
            return (LRESULT)g_br_card;
        }
        /* Message: on card */
        if (ctrl == h_lbl_msg) {
            SetTextColor(dc, C_TEXT);
            return (LRESULT)g_br_card;
        }
        /* Info & settings labels: dimmed */
        if (ctrl == h_lbl_server || ctrl == h_lbl_user || ctrl == h_lbl_account ||
            ctrl == h_lbl_host  || ctrl == h_lbl_port || ctrl == h_lbl_hub  ||
            ctrl == h_lbl_user2 || ctrl == h_lbl_pass) {
            SetTextColor(dc, C_DIM);
            return (LRESULT)g_br_bg;
        }

        SetTextColor(dc, C_TEXT);
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, C_EDIT_BG);
        SetTextColor(dc, C_TEXT);
        return (LRESULT)g_br_edit;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (dis->CtlType == ODT_BUTTON) {
            PaintButton(dis);
            return TRUE;
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_font)      DeleteObject(g_font);
        if (g_font_bold) DeleteObject(g_font_bold);
        if (g_font_sm)   DeleteObject(g_font_sm);
        if (g_font_mono) DeleteObject(g_font_mono);
        if (g_br_bg)     DeleteObject(g_br_bg);
        if (g_br_card)   DeleteObject(g_br_card);
        if (g_br_edit)   DeleteObject(g_br_edit);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ─── Entry point ───────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmdline, int show) {
    (void)hprev; (void)cmdline; (void)show;

    /* Enable DPI awareness (Windows 8.1+) */
    HMODULE hshcore = LoadLibraryA("shcore.dll");
    if (hshcore) {
        typedef HRESULT (WINAPI *SPDA)(int);
        SPDA fn = (SPDA)GetProcAddress(hshcore, "SetProcessDpiAwareness");
        if (fn) fn(1); /* PROCESS_SYSTEM_DPI_AWARE */
        FreeLibrary(hshcore);
    }

    /* Init common controls */
    INITCOMMONCONTROLSEX icx = { sizeof(icx), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icx);

    /* Init VPN defaults */
    vpn_default_config(&g_cfg);
    memset(&g_status, 0, sizeof(g_status));

    /* Register window class */
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = APP_CLASS;
    wc.hIcon         = LoadIconA(hi, MAKEINTRESOURCEA(1));
    wc.hIconSm       = LoadIconA(hi, MAKEINTRESOURCEA(1));
    RegisterClassExA(&wc);

    /* Center window on screen */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    RECT wr = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int ww = wr.right  - wr.left;
    int wh = wr.bottom - wr.top;

    g_hwnd = CreateWindowExA(0, APP_CLASS, APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        NULL, NULL, hi, NULL);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    /* Initial status check */
    AddLog("RLS Automacao VPN iniciado.");
    vpn_get_status(&g_cfg, &g_status);
    RefreshUI(g_hwnd);

    if (!g_status.softether_ready) {
        AddLog("SoftEther VPN Client nao esta instalado.");
        AddLog("Por favor instale usando o ficheiro rls_vpn.msi");
    } else if (!g_status.connection_ready) {
        AddLog("A configurar conta VPN automaticamente...");
        StartOp(1, NULL);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Sistema VPN pronto. %s", g_status.message);
        AddLog(buf);
    }

    /* Message loop */
    MSG message;
    while (GetMessageA(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return (int)message.wParam;
}
