/* main.c - RLS Automacao VPN v1.2.0 - Pure Win32 C Application
 * Interface profissional com instalacao in-app do SoftEther e barra de progresso
 * Compile: gcc -mwindows -O2 -o rls_vpn.exe main.c vpn.c resource.o
 *          -lcomctl32 -lgdi32 -lshcore -lole32 -lgdiplus
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "vpn.h"

/* App metadata */
#define APP_TITLE   "RLS Automacao VPN"
#define APP_CLASS   "RLSVPNWnd"
#define APP_VER     "v1.2.0"
#define IDR_LOGO    101

/* Custom messages */
#define WM_VPN_RESULT  (WM_USER + 100)
#define WM_VPN_LOG     (WM_USER + 101)

/* Window / Layout */
#define WIN_W    520
#define WIN_H    770
#define MG        20
#define CW       (WIN_W - 2*MG)

#define Y_HDR       0
#define H_HDR      90
#define Y_CARD     98
#define H_CARD    108
#define Y_PROG    216
#define H_PROG     18
#define Y_STEP    238
#define H_STEP     18
#define Y_MBTN    266
#define H_MBTN     52
#define Y_SBTN    328
#define H_SBTN     34
#define Y_INFO    374
#define H_INFO     72
#define Y_IPCONF  456
#define H_IPCONF   70
#define Y_STGBTN  538
#define H_STGBTN   28
#define Y_CONTENT 576
#define H_CONTENT 154
#define Y_FOOTER  740

/* Control IDs */
enum {
    ID_LBL_STATUS = 200, ID_LBL_MSG, ID_LBL_IP,
    ID_BTN_CONNECT, ID_BTN_SETUP,
    ID_LBL_SERVER, ID_LBL_USER, ID_LBL_ACCOUNT,
    ID_BTN_SETTINGS, ID_LOG,
    ID_PROGRESS, ID_LBL_STEP,
    ID_LBL_IPINPUT, ID_EDT_IPINPUT,
    ID_LBL_MASKINPUT, ID_EDT_MASKINPUT,
    ID_BTN_APPLYIP,
    ID_LBL_HOST,  ID_EDT_HOST,
    ID_LBL_PORT,  ID_EDT_PORT,
    ID_LBL_HUB,   ID_EDT_HUB,
    ID_LBL_USER2, ID_EDT_USER,
    ID_LBL_PASS,  ID_EDT_PASS,
    ID_BTN_SAVE, ID_BTN_RESET,
};

/* Dark theme colors */
#define C_BG        RGB(13,  17,  23)
#define C_CARD      RGB(22,  27,  34)
#define C_HDR       RGB(17,  22,  30)
#define C_BORDER    RGB(48,  54,  61)
#define C_TEXT      RGB(230, 237, 243)
#define C_DIM       RGB(139, 148, 158)
#define C_GREEN     RGB(35,  134,  54)
#define C_GREEN_HL  RGB(46,  160,  67)
#define C_RED       RGB(218,  54,  51)
#define C_RED_HL    RGB(248,  81,  73)
#define C_ORANGE    RGB(210, 153,  34)
#define C_GRAY      RGB(110, 118, 129)
#define C_BTN       RGB(33,   38,  45)
#define C_BTN_HL    RGB(48,   54,  61)
#define C_ACCENT    RGB(31,  111, 235)
#define C_ACCENT_HL RGB(56,  139, 253)
#define C_EDIT_BG   RGB(13,   17,  23)
#define C_SECTION   RGB(28,   33,  40)

/* Global state */
static HWND      g_hwnd;
static VpnConfig g_cfg;
static VpnStatus g_status;
static BOOL      g_settings_vis  = FALSE;
static BOOL      g_busy          = FALSE;
static BOOL      g_closing       = FALSE;
static int       g_prev_state    = -1;
static int       g_hot_id        = 0;
static WNDPROC   g_orig_btn_proc = NULL;
static BOOL      g_was_connected = FALSE;

/* GDI resources */
static HFONT  g_font, g_font_bold, g_font_sm, g_font_mono, g_font_status;
static HBRUSH g_br_bg, g_br_card, g_br_hdr, g_br_edit, g_br_section;

/* GDI+ logo (dynamic) */
typedef int  (WINAPI *PFN_GdiplusStartup)(ULONG_PTR*, const void*, void*);
typedef void (WINAPI *PFN_GdiplusShutdown)(ULONG_PTR);
typedef int  (WINAPI *PFN_GdipLoadImageFromStream)(void*, void**);
typedef int  (WINAPI *PFN_GdipCreateFromHDC)(HDC, void**);
typedef int  (WINAPI *PFN_GdipDrawImageRectI)(void*, void*, int, int, int, int);
typedef int  (WINAPI *PFN_GdipDeleteGraphics)(void*);
typedef int  (WINAPI *PFN_GdipDisposeImage)(void*);

static HMODULE   g_gdiplus       = NULL;
static ULONG_PTR g_gditoken      = 0;
static void     *g_logo_img      = NULL;
static PFN_GdiplusStartup          pfn_Startup       = NULL;
static PFN_GdiplusShutdown         pfn_Shutdown      = NULL;
static PFN_GdipLoadImageFromStream pfn_LoadStream    = NULL;
static PFN_GdipCreateFromHDC       pfn_CreateFromHDC = NULL;
static PFN_GdipDrawImageRectI      pfn_DrawRect      = NULL;
static PFN_GdipDeleteGraphics      pfn_DelGraphics   = NULL;
static PFN_GdipDisposeImage        pfn_DisposeImage  = NULL;

/* Control handles */
static HWND h_lbl_status, h_lbl_msg, h_lbl_ip;
static HWND h_btn_connect, h_btn_setup;
static HWND h_lbl_server, h_lbl_user, h_lbl_account;
static HWND h_btn_settings, h_log;
static HWND h_progress, h_lbl_step;
static HWND h_lbl_ipinput, h_edt_ipinput;
static HWND h_lbl_maskinput, h_edt_maskinput;
static HWND h_btn_applyip;
static HWND h_lbl_host,  h_edt_host;
static HWND h_lbl_port,  h_edt_port;
static HWND h_lbl_hub,   h_edt_hub;
static HWND h_lbl_user2, h_edt_user;
static HWND h_lbl_pass,  h_edt_pass;
static HWND h_btn_save, h_btn_reset;

/* Forward declarations */
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK BtnSubclassProc(HWND, UINT, WPARAM, LPARAM);
static void CreateControls(HWND hwnd);
static void RefreshUI(HWND hwnd);
static void AddLog(const char *msg);
static void ShowSettingsPanel(BOOL vis);

/* VPN log callback */
static void vpn_log_callback(const char *msg, void *ctx) {
    HWND hwnd = (HWND)ctx;
    if (!hwnd) return;
    char *heap = (char *)malloc(strlen(msg) + 1);
    if (heap) { strcpy(heap, msg); PostMessageA(hwnd, WM_VPN_LOG, 0, (LPARAM)heap); }
}

/* GDI+ logo */
static void InitGdiPlus(void) {
    g_gdiplus = LoadLibraryA("gdiplus.dll");
    if (!g_gdiplus) return;
    pfn_Startup       = (PFN_GdiplusStartup)         GetProcAddress(g_gdiplus, "GdiplusStartup");
    pfn_Shutdown      = (PFN_GdiplusShutdown)        GetProcAddress(g_gdiplus, "GdiplusShutdown");
    pfn_LoadStream    = (PFN_GdipLoadImageFromStream) GetProcAddress(g_gdiplus, "GdipLoadImageFromStream");
    pfn_CreateFromHDC = (PFN_GdipCreateFromHDC)      GetProcAddress(g_gdiplus, "GdipCreateFromHDC");
    pfn_DrawRect      = (PFN_GdipDrawImageRectI)     GetProcAddress(g_gdiplus, "GdipDrawImageRectI");
    pfn_DelGraphics   = (PFN_GdipDeleteGraphics)     GetProcAddress(g_gdiplus, "GdipDeleteGraphics");
    pfn_DisposeImage  = (PFN_GdipDisposeImage)       GetProcAddress(g_gdiplus, "GdipDisposeImage");
    if (!pfn_Startup) return;
    DWORD input[4] = {1, 0, 0, 0};
    pfn_Startup(&g_gditoken, input, NULL);
}

static void LoadLogoFromResource(void) {
    if (!pfn_LoadStream) return;
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_LOGO), "RCDATA");
    if (!hRes) return;
    HGLOBAL hData = LoadResource(NULL, hRes);
    DWORD   size  = SizeofResource(NULL, hRes);
    void   *data  = LockResource(hData);
    if (!data || size < 16) return;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return;
    void *pMem = GlobalLock(hMem);
    memcpy(pMem, data, size);
    GlobalUnlock(hMem);
    IStream *stream = NULL;
    CreateStreamOnHGlobal(hMem, TRUE, &stream);
    if (!stream) { GlobalFree(hMem); return; }
    pfn_LoadStream(stream, &g_logo_img);
    stream->lpVtbl->Release(stream);
}

static void DrawLogo(HDC dc, int x, int y, int w, int h) {
    if (!g_logo_img || !pfn_CreateFromHDC) return;
    void *gr = NULL;
    pfn_CreateFromHDC(dc, &gr);
    if (!gr) return;
    pfn_DrawRect(gr, g_logo_img, x, y, w, h);
    pfn_DelGraphics(gr);
}

static void ShutdownGdiPlus(void) {
    if (g_logo_img && pfn_DisposeImage) pfn_DisposeImage(g_logo_img);
    if (g_gditoken  && pfn_Shutdown)    pfn_Shutdown(g_gditoken);
    if (g_gdiplus)  FreeLibrary(g_gdiplus);
    g_logo_img = NULL; g_gditoken = 0; g_gdiplus = NULL;
}

/* Helpers */
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

    /* Atualiza step label com mensagem atual */
    if (h_lbl_step && g_busy) {
        const char *body = msg;
        if (body[0] == '[') {
            const char *sp = strchr(body, ']');
            if (sp) { body = sp + 1; while (*body == ' ') body++; }
        }
        char short_msg[96];
        strncpy(short_msg, body, sizeof(short_msg) - 1);
        short_msg[sizeof(short_msg) - 1] = '\0';
        SetWindowTextA(h_lbl_step, short_msg);
    }

    /* Parse [N/M] para barra de progresso */
    if (h_progress) {
        const char *sp = strstr(msg, "[");
        if (sp) {
            int n = 0, m = 0;
            if (sscanf(sp, "[%d/%d]", &n, &m) == 2 && m > 0 && n >= 0 && n <= m)
                SendMessage(h_progress, PBM_SETPOS, (WPARAM)((n * 100) / m), 0);
        }
    }

    /* Timestamp + log */
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char line[640];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s\r\n",
             lt->tm_hour, lt->tm_min, lt->tm_sec, msg);
    int len = GetWindowTextLengthA(h_log);
    SendMessageA(h_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    int nlines = (int)SendMessageA(h_log, EM_GETLINECOUNT, 0, 0);
    if (nlines > 100) {
        int start = (int)SendMessageA(h_log, EM_LINEINDEX, 0, 0);
        int end   = (int)SendMessageA(h_log, EM_LINEINDEX, nlines - 100, 0);
        SendMessageA(h_log, EM_SETSEL, (WPARAM)start, (LPARAM)end);
        SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)"");
    }
    SendMessageA(h_log, WM_VSCROLL, SB_BOTTOM, 0);
}

static void PostLog(HWND hwnd, const char *msg) {
    char *heap = (char *)malloc(strlen(msg) + 1);
    if (heap) { strcpy(heap, msg); PostMessageA(hwnd, WM_VPN_LOG, 0, (LPARAM)heap); }
}

static void RefreshUI(HWND hwnd) {
    const char *state_str;
    switch (g_status.state) {
    case VPN_CONNECTED:      state_str = "LIGADO";           break;
    case VPN_CONNECTING:     state_str = "A LIGAR...";       break;
    case VPN_DISCONNECTED:   state_str = "DESLIGADO";        break;
    case VPN_NOT_CONFIGURED: state_str = "NAO CONFIGURADO";  break;
    default:                 state_str = "A CONFIGURAR...";  break;
    }
    SetWindowTextA(h_lbl_status, state_str);
    SetWindowTextA(h_lbl_msg,    g_status.message);

    if (g_status.state >= VPN_CONNECTING) {
        char iptext[128];
        snprintf(iptext, sizeof(iptext), "IP: %s",
                 g_status.local_ip[0] ? g_status.local_ip : "Aguardando...");
        SetWindowTextA(h_lbl_ip, iptext);
        ShowWindow(h_lbl_ip, SW_SHOW);
    } else {
        ShowWindow(h_lbl_ip, SW_HIDE);
    }

    char info[256];
    snprintf(info, sizeof(info), "  Servidor:    %s:%u", g_cfg.host, g_cfg.port);
    SetWindowTextA(h_lbl_server, info);
    snprintf(info, sizeof(info), "  Utilizador:  %s @ %s", g_cfg.username, g_cfg.hub);
    SetWindowTextA(h_lbl_user, info);
    snprintf(info, sizeof(info), "  Conta VPN:   %s", g_cfg.account_name);
    SetWindowTextA(h_lbl_account, info);

    SetWindowTextA(h_btn_connect, g_status.connected ? "DESLIGAR" : "LIGAR");
    EnableWindow(h_btn_connect, !g_busy && g_status.connection_ready);
    EnableWindow(h_btn_setup,   !g_busy && g_status.softether_ready);
    EnableWindow(h_btn_applyip, !g_busy);

    /* Barra de progresso: visivel so quando ocupado */
    ShowWindow(h_progress, g_busy ? SW_SHOW : SW_HIDE);
    ShowWindow(h_lbl_step, g_busy ? SW_SHOW : SW_HIDE);
    if (!g_busy) {
        SendMessage(h_progress, PBM_SETPOS, 0, 0);
        if (h_lbl_step) SetWindowTextA(h_lbl_step, "");
    }

    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/* Thread infrastructure */
typedef struct {
    HWND      hwnd;
    VpnConfig cfg;
    int       op;
    char      ip[64];
    char      mask[32];
    char      se_dir[MAX_PATH];
} OpParam;

static DWORD WINAPI WorkerThread(LPVOID param) {
    OpParam *p = (OpParam *)param;
    char msg[512] = {0};
    int  ok = 0;

    switch (p->op) {
    case 1: /* setup NIC + conta */
        ok = vpn_setup(&p->cfg, msg, sizeof(msg));
        break;

    case 2: /* conectar */
        ok = vpn_connect(&p->cfg, msg, sizeof(msg));
        if (ok) {
            char ip[64], msk[32];
            strncpy(ip,  p->ip,   sizeof(ip)  - 1);
            strncpy(msk, p->mask, sizeof(msk) - 1);
            if (ip[0]) {
                PostLog(p->hwnd, "A aguardar placa VPN subir...");
                Sleep(3000);
                char ipmsg[256] = {0};
                vpn_set_static_ip(ip, msk, ipmsg, sizeof(ipmsg));
                PostLog(p->hwnd, ipmsg);
            }
        }
        break;

    case 3: /* desligar */
        ok = vpn_disconnect(&p->cfg, msg, sizeof(msg));
        break;

    case 4: /* reset */
        ok = vpn_reset(&p->cfg, msg, sizeof(msg));
        break;

    case 5: /* instalacao completa: SoftEther + setup NIC/conta */
        if (!vpn_is_installed()) {
            PostLog(p->hwnd, "SoftEther nao instalado. A procurar ficheiro de instalacao...");
            char se_path[MAX_PATH + 20] = {0};
            snprintf(se_path, sizeof(se_path), "%s\\se_client.exe", p->se_dir);
            if (GetFileAttributesA(se_path) != INVALID_FILE_ATTRIBUTES) {
                PostLog(p->hwnd, "[1/5] Instalando SoftEther VPN Client...");
                int inst = vpn_install_silent(se_path);
                if (!inst) {
                    snprintf(msg, sizeof(msg),
                        "Falha ao instalar SoftEther. Execute como Administrador.");
                    break;
                }
                PostLog(p->hwnd, "[2/5] SoftEther instalado com sucesso.");
            } else {
                snprintf(msg, sizeof(msg),
                    "Ficheiro se_client.exe nao encontrado em: %s\n"
                    "Use o instalador rls_vpn.msi para instalacao completa.", p->se_dir);
                break;
            }
        } else {
            PostLog(p->hwnd, "[1/5] SoftEther ja instalado.");
        }
        /* Setup NIC + conta VPN (logs [1/5]..[5/5] internamente) */
        ok = vpn_setup(&p->cfg, msg, sizeof(msg));
        break;

    case 6: /* IP estatico */
        ok = vpn_set_static_ip(p->ip, p->mask, msg, sizeof(msg));
        break;
    }

    char *heap = (char *)malloc(strlen(msg) + 1);
    if (heap) strcpy(heap, msg);
    PostMessageA(p->hwnd, WM_VPN_RESULT, (WPARAM)ok, (LPARAM)heap);
    free(p);
    return 0;
}

static void StartOp(int op_type) {
    if (g_busy) return;
    g_busy = TRUE;

    if (h_progress) SendMessage(h_progress, PBM_SETPOS, 0, 0);
    if (h_lbl_step) SetWindowTextA(h_lbl_step, "A iniciar...");
    ShowWindow(h_progress, SW_SHOW);
    ShowWindow(h_lbl_step, SW_SHOW);

    OpParam *p = (OpParam *)malloc(sizeof(OpParam));
    if (!p) { g_busy = FALSE; return; }
    ZeroMemory(p, sizeof(*p));
    p->hwnd = g_hwnd;
    p->cfg  = g_cfg;
    p->op   = op_type;

    if (op_type == 2 || op_type == 6) {
        GetWindowTextA(h_edt_ipinput,   p->ip,   sizeof(p->ip));
        GetWindowTextA(h_edt_maskinput, p->mask, sizeof(p->mask));
    }
    if (op_type == 5) {
        GetModuleFileNameA(NULL, p->se_dir, MAX_PATH - 1);
        char *last = strrchr(p->se_dir, '\\');
        if (last) *last = '\0';
    }

    EnableWindow(h_btn_setup,   FALSE);
    EnableWindow(h_btn_connect, FALSE);
    EnableWindow(h_btn_applyip, FALSE);

    HANDLE ht = CreateThread(NULL, 0, WorkerThread, p, 0, NULL);
    if (ht) CloseHandle(ht);
    else    { free(p); g_busy = FALSE; }
}

/* Control factory helpers */
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
    WNDPROC orig = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(hw, GWLP_WNDPROC,
                                                        (LONG_PTR)BtnSubclassProc);
    if (!g_orig_btn_proc) g_orig_btn_proc = orig;
    return hw;
}

/* Button subclass: hover tracking */
static LRESULT CALLBACK BtnSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int id = GetDlgCtrlID(hwnd);
    switch (msg) {
    case WM_MOUSEMOVE:
        if (g_hot_id != id) {
            g_hot_id = id;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd; tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hot_id == id) {
            g_hot_id = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return g_orig_btn_proc ? CallWindowProcA(g_orig_btn_proc, hwnd, msg, wp, lp)
                           : DefWindowProcA(hwnd, msg, wp, lp);
}

/* Create all child controls */
static void CreateControls(HWND hwnd) {
    HINSTANCE hi = GetModuleHandleA(NULL);

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
    g_font_status = CreateFontA(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    g_br_bg      = CreateSolidBrush(C_BG);
    g_br_card    = CreateSolidBrush(C_CARD);
    g_br_hdr     = CreateSolidBrush(C_HDR);
    g_br_edit    = CreateSolidBrush(C_EDIT_BG);
    g_br_section = CreateSolidBrush(C_SECTION);

    /* Status card labels */
    h_lbl_status = MakeLabel(hwnd, ID_LBL_STATUS, "A INICIAR...",
        MG + 28, Y_CARD + 14, CW - 40, 32, g_font_status);
    h_lbl_msg = MakeLabel(hwnd, ID_LBL_MSG, "",
        MG + 14, Y_CARD + 56, CW - 28, 20, g_font_sm);
    h_lbl_ip = MakeLabel(hwnd, ID_LBL_IP, "",
        MG + 14, Y_CARD + 78, 260, 20, g_font_sm);
    ShowWindow(h_lbl_ip, SW_HIDE);

    /* Progress bar + step label */
    h_progress = CreateWindowExA(0, PROGRESS_CLASS, NULL,
        WS_CHILD | PBS_SMOOTH,
        MG, Y_PROG, CW, H_PROG,
        hwnd, (HMENU)ID_PROGRESS, hi, NULL);
    SendMessage(h_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(h_progress, PBM_SETBARCOLOR, 0, (LPARAM)C_ACCENT);
    SendMessage(h_progress, PBM_SETBKCOLOR,  0, (LPARAM)C_SECTION);
    ShowWindow(h_progress, SW_HIDE);

    h_lbl_step = MakeLabel(hwnd, ID_LBL_STEP, "",
        MG, Y_STEP, CW, H_STEP, g_font_sm);
    ShowWindow(h_lbl_step, SW_HIDE);

    /* Botao principal full-width */
    h_btn_connect = MakeBtn(hwnd, ID_BTN_CONNECT, "LIGAR",
        MG, Y_MBTN, CW, H_MBTN);

    /* Botoes secundarios */
    int sw2 = (CW - 8) / 2;
    h_btn_setup    = MakeBtn(hwnd, ID_BTN_SETUP,    "Configurar",
        MG,           Y_SBTN, sw2, H_SBTN);
    h_btn_settings = MakeBtn(hwnd, ID_BTN_SETTINGS, "v  Configuracoes",
        MG + sw2 + 8, Y_SBTN, sw2, H_SBTN);

    /* Info section */
    h_lbl_server  = MakeLabel(hwnd, ID_LBL_SERVER,  "  Servidor:   -",
        MG, Y_INFO + 8,  CW, 18, g_font_sm);
    h_lbl_user    = MakeLabel(hwnd, ID_LBL_USER,    "  Utilizador: -",
        MG, Y_INFO + 30, CW, 18, g_font_sm);
    h_lbl_account = MakeLabel(hwnd, ID_LBL_ACCOUNT, "  Conta VPN:  -",
        MG, Y_INFO + 52, CW, 18, g_font_sm);

    /* IP config */
    int ip_lbl_w = 68, ip_fld_w = 118;
    int msk_lbl_w = 62, msk_fld_w = 108;
    int apl_w = 88, gap = 6;
    int row_y = Y_IPCONF + 32;

    h_lbl_ipinput = MakeLabel(hwnd, ID_LBL_IPINPUT, "IP Manual:",
        MG, row_y + 4, ip_lbl_w, 18, g_font_sm);
    h_edt_ipinput = MakeEdit(hwnd, ID_EDT_IPINPUT, "",
        MG + ip_lbl_w + 4, row_y, ip_fld_w, 26, FALSE);

    h_lbl_maskinput = MakeLabel(hwnd, ID_LBL_MASKINPUT, "Mascara:",
        MG + ip_lbl_w + 4 + ip_fld_w + gap, row_y + 4, msk_lbl_w, 18, g_font_sm);
    h_edt_maskinput = MakeEdit(hwnd, ID_EDT_MASKINPUT, "255.255.255.0",
        MG + ip_lbl_w + 4 + ip_fld_w + gap + msk_lbl_w + 4, row_y, msk_fld_w, 26, FALSE);

    int apl_x = MG + ip_lbl_w + 4 + ip_fld_w + gap + msk_lbl_w + 4 + msk_fld_w + gap;
    h_btn_applyip = MakeBtn(hwnd, ID_BTN_APPLYIP, "Aplicar",
        apl_x, row_y, apl_w, 26);

    /* Log area */
    h_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        MG, Y_CONTENT, CW, H_CONTENT,
        hwnd, (HMENU)ID_LOG, hi, NULL);
    SendMessageA(h_log, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

    /* Settings panel (hidden) */
    int rx = MG + 100, rw = CW - 100, ry = Y_CONTENT;
    h_lbl_host  = MakeLabel(hwnd, ID_LBL_HOST,  "Servidor:",   MG, ry+6,   96, 18, g_font_sm);
    h_edt_host  = MakeEdit (hwnd, ID_EDT_HOST,  g_cfg.host,    rx, ry,     rw, 26, FALSE);
    h_lbl_port  = MakeLabel(hwnd, ID_LBL_PORT,  "Porta:",      MG, ry+46,  96, 18, g_font_sm);
    h_edt_port  = MakeEdit (hwnd, ID_EDT_PORT,  "443",         rx, ry+40,  80, 26, FALSE);
    h_lbl_hub   = MakeLabel(hwnd, ID_LBL_HUB,   "Hub:",        MG, ry+86,  96, 18, g_font_sm);
    h_edt_hub   = MakeEdit (hwnd, ID_EDT_HUB,   g_cfg.hub,     rx, ry+80,  rw, 26, FALSE);
    h_lbl_user2 = MakeLabel(hwnd, ID_LBL_USER2, "Utilizador:", MG, ry+126, 96, 18, g_font_sm);
    h_edt_user  = MakeEdit (hwnd, ID_EDT_USER,  g_cfg.username,rx, ry+120, rw, 26, FALSE);
    h_lbl_pass  = MakeLabel(hwnd, ID_LBL_PASS,  "Password:",   MG, ry+166, 96, 18, g_font_sm);
    h_edt_pass  = MakeEdit (hwnd, ID_EDT_PASS,  g_cfg.password,rx, ry+160, rw, 26, TRUE);
    h_btn_save  = MakeBtn(hwnd, ID_BTN_SAVE,  "Guardar",
        MG,            ry+196, sw2, 30);
    h_btn_reset = MakeBtn(hwnd, ID_BTN_RESET, "Limpar instalacao",
        MG + sw2 + 8,  ry+196, sw2, 30);

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", g_cfg.port);
    SetWindowTextA(h_edt_port, portstr);

    HWND sc[] = {
        h_lbl_host, h_edt_host, h_lbl_port, h_edt_port,
        h_lbl_hub,  h_edt_hub,  h_lbl_user2, h_edt_user,
        h_lbl_pass, h_edt_pass, h_btn_save,  h_btn_reset, NULL
    };
    for (int i = 0; sc[i]; i++) ShowWindow(sc[i], SW_HIDE);
}

/* Toggle settings panel */
static void ShowSettingsPanel(BOOL vis) {
    g_settings_vis = vis;
    ShowWindow(h_log, vis ? SW_HIDE : SW_SHOW);
    HWND sc[] = {
        h_lbl_host, h_edt_host, h_lbl_port, h_edt_port,
        h_lbl_hub,  h_edt_hub,  h_lbl_user2, h_edt_user,
        h_lbl_pass, h_edt_pass, h_btn_save,  h_btn_reset, NULL
    };
    for (int i = 0; sc[i]; i++) ShowWindow(sc[i], vis ? SW_SHOW : SW_HIDE);
    SetWindowTextA(h_btn_settings,
        vis ? "^  Configuracoes" : "v  Configuracoes");
}

/* Paint owner-draw button */
static void PaintButton(DRAWITEMSTRUCT *dis) {
    HDC   dc  = dis->hDC;
    RECT *rc  = &dis->rcItem;
    BOOL  sel = (dis->itemState & ODS_SELECTED) != 0;
    BOOL  disi= (dis->itemState & ODS_DISABLED) != 0;
    BOOL  hot = (!disi) && (g_hot_id == (int)dis->CtlID);

    char txt[80];
    GetWindowTextA(dis->hwndItem, txt, sizeof(txt));

    COLORREF bg, fg, border;
    if (disi) {
        bg = RGB(28,33,40); fg = C_GRAY; border = C_BORDER;
    } else if (strstr(txt, "DESLIGAR")) {
        bg = sel ? RGB(155,22,22) : hot ? C_RED_HL : C_RED;
        fg = C_TEXT; border = bg;
    } else if (strcmp(txt, "LIGAR") == 0) {
        bg = sel ? RGB(18,85,26) : hot ? C_GREEN_HL : C_GREEN;
        fg = C_TEXT; border = bg;
    } else if (strstr(txt, "Aplicar")) {
        bg = sel ? RGB(18,70,160) : hot ? C_ACCENT_HL : C_ACCENT;
        fg = C_TEXT; border = bg;
    } else if (strstr(txt, "Limpar")) {
        bg = sel ? RGB(90,18,18) : hot ? RGB(130,35,35) : RGB(70,22,22);
        fg = RGB(220,150,150); border = hot ? C_RED : RGB(80,35,35);
    } else {
        bg = sel ? C_BTN_HL : hot ? RGB(40,46,56) : C_BTN;
        fg = hot ? C_TEXT : RGB(190,200,215); border = hot ? C_ACCENT : C_BORDER;
    }

    HBRUSH hbr = CreateSolidBrush(bg);
    FillRect(dc, rc, hbr);
    DeleteObject(hbr);

    HPEN hp = CreatePen(PS_SOLID, 1, border);
    HPEN op = (HPEN)SelectObject(dc, hp);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RECT br = *rc; br.right--; br.bottom--;
    Rectangle(dc, br.left, br.top, br.right, br.bottom);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(hp);

    HFONT fnt = (strcmp(txt, "LIGAR") == 0 || strstr(txt, "DESLIGAR"))
                ? g_font_bold : g_font;
    HFONT of = (HFONT)SelectObject(dc, fnt);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    DrawTextA(dc, txt, -1, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

/* Window procedure */
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        CreateControls(hwnd);
        SetTimer(hwnd, 1, 5000, NULL);
        return 0;

    case WM_CLOSE:
        if (g_status.connected && !g_closing) {
            g_closing = TRUE;
            AddLog("A desligar VPN antes de fechar...");
            char buf[256];
            vpn_disconnect(&g_cfg, buf, sizeof(buf));
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_TIMER:
        if (!g_busy && !g_closing) {
            int prev = (int)g_status.state;
            vpn_get_status(&g_cfg, &g_status);
            if (g_was_connected && !g_status.connected)
                AddLog("Ligacao perdida. Clique 'LIGAR' para restabelecer.");
            g_was_connected = g_status.connected;
            if ((int)g_status.state != prev) {
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
            if (h_progress) SendMessage(h_progress, PBM_SETPOS, 100, 0);
            AddLog(result_msg);
            free(result_msg);
        }
        g_busy = FALSE;
        vpn_get_status(&g_cfg, &g_status);
        g_was_connected = g_status.connected;
        RefreshUI(hwnd);
        return 0;
    }

    case WM_VPN_LOG: {
        char *log_msg = (char *)lp;
        if (log_msg) { AddLog(log_msg); free(log_msg); }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_CONNECT:
            if (g_status.connected) {
                AddLog("A desligar VPN..."); StartOp(3);
            } else {
                AddLog("A ligar VPN..."); StartOp(2);
            }
            break;
        case ID_BTN_SETUP:
            AddLog("A configurar placa de rede e conta VPN...");
            StartOp(1);
            break;
        case ID_BTN_APPLYIP: {
            char ip[64] = {0};
            GetWindowTextA(h_edt_ipinput, ip, sizeof(ip));
            if (!ip[0]) { AddLog("Insira um IP antes de aplicar."); break; }
            AddLog("A aplicar IP estatico...");
            StartOp(6);
            break;
        }
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
                    "Confirmar Reset", MB_YESNO | MB_ICONWARNING) == IDYES) {
                AddLog("A fazer reset completo...");
                StartOp(4);
                ShowSettingsPanel(FALSE);
            }
            break;
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cli;
        GetClientRect(hwnd, &cli);
        FillRect(dc, &cli, g_br_bg);

        /* Header */
        {
            RECT hr = { 0, 0, WIN_W, H_HDR };
            FillRect(dc, &hr, g_br_hdr);

            /* Barra azul no topo (3px) */
            HBRUSH bacc = CreateSolidBrush(C_ACCENT);
            RECT top_bar = { 0, 0, WIN_W, 3 };
            FillRect(dc, &top_bar, bacc);
            DeleteObject(bacc);

            /* Linha separadora inferior */
            HPEN hp = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN op = (HPEN)SelectObject(dc, hp);
            MoveToEx(dc, 0, H_HDR - 1, NULL);
            LineTo(dc, WIN_W, H_HDR - 1);
            SelectObject(dc, op); DeleteObject(hp);

            /* Logo ou circulo fallback */
            if (g_logo_img) {
                DrawLogo(dc, MG, 15, 58, 58);
            } else {
                HBRUSH bc = CreateSolidBrush(C_ACCENT);
                SelectObject(dc, bc);
                SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, MG, 17, MG + 54, 71);
                DeleteObject(bc);
                HFONT bf = CreateFontA(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
                HFONT of2 = (HFONT)SelectObject(dc, bf);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, C_TEXT);
                RECT vr = { MG, 17, MG + 54, 71 };
                DrawTextA(dc, "R", -1, &vr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, of2); DeleteObject(bf);
            }

            /* Titulo */
            HFONT of = (HFONT)SelectObject(dc, g_font_bold);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, C_TEXT);
            RECT tr = { MG + 66, 18, WIN_W - MG, 50 };
            DrawTextA(dc, APP_TITLE, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            /* Subtitulo */
            SelectObject(dc, g_font_sm);
            SetTextColor(dc, C_DIM);
            RECT sr = { MG + 66, 50, WIN_W - MG - 50, 70 };
            DrawTextA(dc, "Sistema VPN Industrial - RLS Automacao", -1, &sr, DT_LEFT | DT_SINGLELINE);

            /* Versao (direita) */
            RECT vr2 = { MG, 54, WIN_W - MG, 70 };
            DrawTextA(dc, APP_VER, -1, &vr2, DT_RIGHT | DT_SINGLELINE);
            SelectObject(dc, of);
        }

        /* Status card */
        {
            RECT card = { MG, Y_CARD, MG + CW, Y_CARD + H_CARD };
            FillRect(dc, &card, g_br_card);

            /* Barra lateral colorida (6px) */
            COLORREF sc = g_busy ? C_ORANGE : StateColor(g_status.state);
            HBRUSH bsc = CreateSolidBrush(sc);
            RECT sbar = { MG, Y_CARD, MG + 6, Y_CARD + H_CARD };
            FillRect(dc, &sbar, bsc);
            DeleteObject(bsc);

            /* Borda do card */
            HPEN hp = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN op = (HPEN)SelectObject(dc, hp);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            RECT br = card; br.right--; br.bottom--;
            Rectangle(dc, br.left, br.top, br.right, br.bottom);
            SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(hp);

            /* Circulo indicador */
            HBRUSH bd = CreateSolidBrush(sc);
            SelectObject(dc, bd);
            SelectObject(dc, GetStockObject(NULL_PEN));
            int cx = MG + 20, cy = Y_CARD + 30, cr = 7;
            Ellipse(dc, cx - cr, cy - cr, cx + cr, cy + cr);
            DeleteObject(bd);
        }

        /* Info section border */
        {
            HPEN hp = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN op = (HPEN)SelectObject(dc, hp);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            RECT ir = { MG, Y_INFO - 2, MG + CW, Y_INFO + H_INFO + 2 };
            Rectangle(dc, ir.left, ir.top, ir.right, ir.bottom);
            HFONT of = (HFONT)SelectObject(dc, g_font_sm);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, C_BG);
            SetTextColor(dc, C_DIM);
            TextOutA(dc, MG + 10, Y_INFO - 9, " Ligacao ", (int)strlen(" Ligacao "));
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, of);
            SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(hp);
        }

        /* IP config section */
        {
            RECT sr = { MG, Y_IPCONF, MG + CW, Y_IPCONF + H_IPCONF };
            FillRect(dc, &sr, g_br_section);
            HPEN hp = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN op = (HPEN)SelectObject(dc, hp);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            RECT br = sr; br.right--; br.bottom--;
            Rectangle(dc, br.left, br.top, br.right, br.bottom);
            SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(hp);
            HFONT of = (HFONT)SelectObject(dc, g_font_sm);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, C_DIM);
            TextOutA(dc, MG + 8, Y_IPCONF + 8, "IP Estatico da Placa VPN",
                     (int)strlen("IP Estatico da Placa VPN"));
            SelectObject(dc, of);
        }

        /* Separador */
        {
            HPEN hp = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN op = (HPEN)SelectObject(dc, hp);
            MoveToEx(dc, MG, Y_STGBTN - 4, NULL);
            LineTo(dc, MG + CW, Y_STGBTN - 4);
            SelectObject(dc, op); DeleteObject(hp);
        }

        /* Footer */
        {
            HFONT of = (HFONT)SelectObject(dc, g_font_sm);
            SetTextColor(dc, C_DIM);
            SetBkMode(dc, TRANSPARENT);
            RECT fr = { MG, Y_FOOTER, MG + CW, Y_FOOTER + 20 };
            DrawTextA(dc, "(c) 2026 RLS Automacao  |  Sistema VPN Industrial",
                      -1, &fr, DT_CENTER | DT_SINGLELINE);
            SelectObject(dc, of);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC  dc   = (HDC)wp;
        HWND ctrl = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (ctrl == h_lbl_status) {
            SetTextColor(dc, g_busy ? C_ORANGE : StateColor(g_status.state));
            return (LRESULT)g_br_card;
        }
        if (ctrl == h_lbl_ip) {
            SetTextColor(dc, C_GREEN_HL);
            return (LRESULT)g_br_card;
        }
        if (ctrl == h_lbl_msg) {
            SetTextColor(dc, C_DIM);
            return (LRESULT)g_br_card;
        }
        if (ctrl == h_lbl_step) {
            SetTextColor(dc, C_ACCENT);
            return (LRESULT)g_br_bg;
        }
        if (ctrl == h_lbl_server || ctrl == h_lbl_user || ctrl == h_lbl_account) {
            SetTextColor(dc, C_DIM);
            return (LRESULT)g_br_bg;
        }
        if (ctrl == h_lbl_host  || ctrl == h_lbl_port || ctrl == h_lbl_hub ||
            ctrl == h_lbl_user2 || ctrl == h_lbl_pass) {
            SetTextColor(dc, C_DIM);
            return (LRESULT)g_br_bg;
        }
        if (ctrl == h_lbl_ipinput || ctrl == h_lbl_maskinput) {
            SetTextColor(dc, C_TEXT);
            return (LRESULT)g_br_section;
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
        if (dis->CtlType == ODT_BUTTON) { PaintButton(dis); return TRUE; }
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        ShutdownGdiPlus();
        if (g_font)        DeleteObject(g_font);
        if (g_font_bold)   DeleteObject(g_font_bold);
        if (g_font_sm)     DeleteObject(g_font_sm);
        if (g_font_mono)   DeleteObject(g_font_mono);
        if (g_font_status) DeleteObject(g_font_status);
        if (g_br_bg)       DeleteObject(g_br_bg);
        if (g_br_card)     DeleteObject(g_br_card);
        if (g_br_hdr)      DeleteObject(g_br_hdr);
        if (g_br_edit)     DeleteObject(g_br_edit);
        if (g_br_section)  DeleteObject(g_br_section);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Entry point */
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmdline, int show) {
    (void)hprev; (void)cmdline; (void)show;

    HMODULE hshcore = LoadLibraryA("shcore.dll");
    if (hshcore) {
        typedef HRESULT (WINAPI *SPDA)(int);
        SPDA fn = (SPDA)GetProcAddress(hshcore, "SetProcessDpiAwareness");
        if (fn) fn(1);
        FreeLibrary(hshcore);
    }

    InitGdiPlus();
    LoadLogoFromResource();

    INITCOMMONCONTROLSEX icx = { sizeof(icx), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icx);

    vpn_default_config(&g_cfg);
    memset(&g_status, 0, sizeof(g_status));

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

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    RECT wr = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;

    g_hwnd = CreateWindowExA(0, APP_CLASS, APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        NULL, NULL, hi, NULL);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    vpn_set_log_fn(vpn_log_callback, g_hwnd);
    AddLog("RLS Automacao VPN " APP_VER " iniciado.");

    vpn_get_status(&g_cfg, &g_status);
    g_was_connected = g_status.connected;
    RefreshUI(g_hwnd);

    if (!g_status.softether_ready) {
        /* SoftEther nao instalado: tenta instalar in-app */
        AddLog("SoftEther VPN Client nao encontrado. A iniciar instalacao...");
        StartOp(5);
    } else if (!g_status.connection_ready) {
        AddLog("A configurar conta VPN automaticamente...");
        StartOp(1);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Sistema VPN pronto. %s", g_status.message);
        AddLog(buf);
    }

    MSG message;
    while (GetMessageA(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return (int)message.wParam;
}
