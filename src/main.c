/*
 * main.c — RLS Automacao VPN Client v1.4.0
 * Pure Win32 GDI UI — zero WebView2, zero C++ dependencies.
 * Fast startup, native Windows, dark theme.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vpn.h"

/* ── Control IDs ─────────────────────────────────────────────────── */
#define IDC_LBL_STATUS    110
#define IDC_LBL_IP        111
#define IDC_LBL_INFO      112
#define IDC_LBL_MSG       113
#define IDC_PROGRESS      120
#define IDC_LOG           130
#define IDC_BTN_CONNECT   200
#define IDC_BTN_DISCONNECT 201
#define IDC_BTN_SETUP     202
#define IDC_BTN_RESET     203
#define IDC_BTN_INSTALL   204
#define IDC_BTN_APPLYIP   205
#define IDC_BTN_SAVE      206
#define IDC_EDIT_HOST     300
#define IDC_EDIT_PORT     301
#define IDC_EDIT_HUB      302
#define IDC_EDIT_USER     303
#define IDC_EDIT_PASS     304
#define IDC_EDIT_IPST     310
#define IDC_EDIT_MASK     311

/* ── Colors ──────────────────────────────────────────────────────── */
#define CLR_BG       RGB(22,  27,  34)
#define CLR_HEADER   RGB(13,  17,  23)
#define CLR_PANEL    RGB(30,  38,  48)
#define CLR_INPUT_BG RGB(13,  17,  23)
#define CLR_TEXT     RGB(230, 237, 243)
#define CLR_DIM      RGB(139, 148, 158)
#define CLR_ACCENT   RGB(31,  111, 235)
#define CLR_GREEN    RGB(35,  197,  94)
#define CLR_RED      RGB(248,  81,  73)
#define CLR_YELLOW   RGB(210, 153,  34)
#define CLR_BORDER   RGB(48,  54,   61)

/* ── Window size ─────────────────────────────────────────────────── */
#define WIN_W  720
#define WIN_H  590
#define HDR_H   64

/* ── Custom messages ─────────────────────────────────────────────── */
#define WM_VPN_LOG      (WM_APP + 1)
#define WM_VPN_RESULT   (WM_APP + 2)
#define WM_VPN_PROGRESS (WM_APP + 3)
#define WM_VPN_STATUS   (WM_APP + 4)

/* ── Globals ─────────────────────────────────────────────────────── */
static HWND g_hwnd = NULL;
static int  g_busy = 0;
static VpnConfig g_config;
static VpnState  g_last_state = VPN_NOT_INSTALLED;

/* Child controls */
static HWND h_lbl_status, h_lbl_ip, h_lbl_info, h_lbl_msg;
static HWND h_progress, h_log;
static HWND h_btn_connect, h_btn_disconnect, h_btn_setup,
            h_btn_reset, h_btn_install, h_btn_applyip, h_btn_save;
static HWND h_edit_host, h_edit_port, h_edit_hub, h_edit_user, h_edit_pass;
static HWND h_edit_ipst, h_edit_mask;

/* GDI resources */
static HBRUSH g_br_bg, g_br_header, g_br_panel, g_br_input;
static HFONT  g_font_ui, g_font_bold, g_font_status, g_font_mono, g_font_small;

/* ── Helpers ─────────────────────────────────────────────────────── */
static HFONT MakeFont(int size, int bold, int mono)
{
    return CreateFontA(size, 0, 0, 0,
        bold ? FW_BOLD : FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
        mono ? FIXED_PITCH : DEFAULT_PITCH,
        mono ? "Consolas" : "Segoe UI");
}

static HWND MkStatic(HWND p, UINT id, const char *t, int x, int y, int w, int h, HFONT f)
{
    HWND hw = CreateWindowA("STATIC", t, WS_CHILD|WS_VISIBLE|SS_LEFT,
                            x, y, w, h, p, (HMENU)(UINT_PTR)id, NULL, NULL);
    SendMessageA(hw, WM_SETFONT, (WPARAM)f, FALSE);
    return hw;
}

static HWND MkBtn(HWND p, UINT id, const char *t, int x, int y, int w, int h, HFONT f)
{
    HWND hw = CreateWindowA("BUTTON", t, WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                            x, y, w, h, p, (HMENU)(UINT_PTR)id, NULL, NULL);
    SendMessageA(hw, WM_SETFONT, (WPARAM)f, FALSE);
    return hw;
}

static HWND MkEdit(HWND p, UINT id, const char *t, int x, int y, int w, int h,
                   DWORD extra, HFONT f)
{
    HWND hw = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", t,
                              WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|extra,
                              x, y, w, h, p, (HMENU)(UINT_PTR)id, NULL, NULL);
    SendMessageA(hw, WM_SETFONT, (WPARAM)f, FALSE);
    return hw;
}

/* ── Log append ──────────────────────────────────────────────────── */
static void log_append(const char *msg)
{
    if (!h_log || !msg) return;
    int tot = GetWindowTextLengthA(h_log);
    if (tot > 36000) {
        SendMessageA(h_log, EM_SETSEL, 0, 8000);
        SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)"");
    }
    int len = GetWindowTextLengthA(h_log);
    SendMessageA(h_log, EM_SETSEL, len, len);
    SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)msg);
    SendMessageA(h_log, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
    SendMessageA(h_log, EM_SCROLLCARET, 0, 0);
}

/* (json helpers removed — no longer needed without WebView2) */



/* ── vpn_log_callback (called on worker thread) ──────────────────── */
static void vpn_log_callback(const char *msg, void *ctx)
{
    (void)ctx;
    if (!msg || !g_hwnd) return;
    int num = 0, den = 0;
    const char *bracket = strchr(msg, '[');
    if (bracket) {
        sscanf(bracket, "[%d/%d]", &num, &den);
        if (num > 0 && den > 0)
            PostMessageA(g_hwnd, WM_VPN_PROGRESS, (WPARAM)((num*100)/den), 0);
    }
    PostMessageA(g_hwnd, WM_VPN_LOG, 0, (LPARAM)_strdup(msg));
}

/* ── Worker thread ───────────────────────────────────────────────── */
typedef struct {
    int       op;
    VpnConfig cfg;
    char      se_dir[MAX_PATH];
    char      ip[64];
    char      mask[64];
} ThreadData;

static DWORD WINAPI WorkerThread(LPVOID param)
{
    ThreadData *td = (ThreadData *)param;
    int ok = 0; char msg[512] = {0};
    switch (td->op) {
        case 1:
            ok=(vpn_setup(&td->cfg,msg,sizeof msg)==0);
            if(!msg[0]) strncpy(msg,ok?"NIC/conta configurada com sucesso.":"Falha ao configurar.",sizeof msg-1);
            break;
        case 2:
            ok=(vpn_connect(&td->cfg,msg,sizeof msg)==0);
            if(!msg[0]) strncpy(msg,ok?"Ligacao estabelecida.":"Falha ao ligar.",sizeof msg-1);
            break;
        case 3:
            vpn_disconnect(&td->cfg,msg,sizeof msg); ok=1;
            if(!msg[0]) strncpy(msg,"VPN desligada.",sizeof msg-1);
            break;
        case 4:
            vpn_reset(&td->cfg,msg,sizeof msg); ok=1;
            if(!msg[0]) strncpy(msg,"Configuracao reposta.",sizeof msg-1);
            break;
        case 5: {
            int r=vpn_install_silent(td->se_dir);
            if(r!=0){ strncpy(msg,"Falha ao instalar SoftEther.",sizeof msg-1); break; }
            vpn_start_service();
            ok=(vpn_setup(&td->cfg,msg,sizeof msg)==0);
            if(!msg[0]) strncpy(msg,ok?"SoftEther instalado e configurado.":"Instalado mas configuracao falhou.",sizeof msg-1);
            break;
        }
        case 6:
            ok=(vpn_set_static_ip(td->ip,td->mask,msg,sizeof msg)==0);
            if(!msg[0]) strncpy(msg,ok?"IP estatico aplicado.":"Falha ao aplicar IP.",sizeof msg-1);
            break;
        default:
            strncpy(msg,"Operacao desconhecida.",sizeof msg-1);
            break;
    }
    PostMessageA(g_hwnd, WM_VPN_RESULT, (WPARAM)ok, (LPARAM)_strdup(msg));
    free(td); return 0;
}

static void EnableAllButtons(void)
{
    EnableWindow(h_btn_connect,    TRUE); EnableWindow(h_btn_disconnect, TRUE);
    EnableWindow(h_btn_setup,      TRUE); EnableWindow(h_btn_reset,      TRUE);
    EnableWindow(h_btn_install,    TRUE); EnableWindow(h_btn_applyip,    TRUE);
    EnableWindow(h_btn_save,       TRUE);
}

static void StartOp(int op, const char *ip, const char *mask)
{
    if (g_busy) return;
    g_busy = 1;
    SendMessageA(h_progress, PBM_SETPOS, 0, 0);
    ShowWindow(h_progress, SW_SHOW);
    EnableWindow(h_btn_connect,    FALSE); EnableWindow(h_btn_disconnect, FALSE);
    EnableWindow(h_btn_setup,      FALSE); EnableWindow(h_btn_reset,      FALSE);
    EnableWindow(h_btn_install,    FALSE); EnableWindow(h_btn_applyip,    FALSE);
    EnableWindow(h_btn_save,       FALSE);

    ThreadData *td = (ThreadData *)calloc(1, sizeof(ThreadData));
    if (!td) { g_busy = 0; return; }
    td->op  = op;
    td->cfg = g_config;
    if (op == 5) {
        GetModuleFileNameA(NULL, td->se_dir, MAX_PATH);
        char *sl = strrchr(td->se_dir, '\\'); if (sl) *(sl+1)='\0';
    }
    if (ip)   strncpy(td->ip,   ip,   sizeof td->ip   - 1);
    if (mask) strncpy(td->mask, mask, sizeof td->mask - 1);
    HANDLE ht = CreateThread(NULL, 0, WorkerThread, td, 0, NULL);
    if (ht) CloseHandle(ht); else { free(td); g_busy=0; }
}

static void SaveConfig(void)
{
    char host[256]="",port_s[16]="",hub[64]="",user[64]="",pass[128]="";
    GetWindowTextA(h_edit_host,host,  sizeof host);
    GetWindowTextA(h_edit_port,port_s,sizeof port_s);
    GetWindowTextA(h_edit_hub, hub,   sizeof hub);
    GetWindowTextA(h_edit_user,user,  sizeof user);
    GetWindowTextA(h_edit_pass,pass,  sizeof pass);
    if(host[0]) strncpy(g_config.host,    host,sizeof g_config.host-1);
    if(hub[0])  strncpy(g_config.hub,     hub, sizeof g_config.hub-1);
    if(user[0]) strncpy(g_config.username,user,sizeof g_config.username-1);
    if(pass[0]) strncpy(g_config.password,pass,sizeof g_config.password-1);
    int p=atoi(port_s); if(p>0) g_config.port=(unsigned short)p;
    SetWindowTextA(h_lbl_msg,"Configuracao guardada.");
    log_append("[INFO] Configuracao atualizada.");
}

static void RefreshStatus(void)
{
    VpnStatus st={0};
    vpn_get_status(&g_config,&st);
    g_last_state=st.state;
    const char *stxt;
    switch(st.state){
        case VPN_CONNECTED:      stxt="CONECTADO";       break;
        case VPN_CONNECTING:     stxt="A LIGAR...";      break;
        case VPN_DISCONNECTED:   stxt="DESLIGADO";       break;
        case VPN_NOT_CONFIGURED: stxt="NAO CONFIGURADO"; break;
        default:                 stxt="NAO INSTALADO";   break;
    }
    SetWindowTextA(h_lbl_status,stxt);
    InvalidateRect(h_lbl_status,NULL,TRUE);
    char ip_txt[80];
    snprintf(ip_txt,sizeof ip_txt,"IP VPN: %s",st.local_ip[0]?st.local_ip:"---");
    SetWindowTextA(h_lbl_ip,ip_txt);
    char info[256];
    snprintf(info,sizeof info,"Servidor: %s:%d   Hub: %s   User: %s",
             g_config.host,g_config.port,g_config.hub,g_config.username);
    SetWindowTextA(h_lbl_info,info);
}

static void PaintHeader(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc=BeginPaint(hwnd,&ps);
    RECT rcH={0,0,WIN_W,HDR_H};
    HBRUSH bH=CreateSolidBrush(CLR_HEADER); FillRect(dc,&rcH,bH); DeleteObject(bH);
    HPEN pen=CreatePen(PS_SOLID,2,CLR_ACCENT);
    HPEN op=(HPEN)SelectObject(dc,pen);
    MoveToEx(dc,0,HDR_H-1,NULL); LineTo(dc,WIN_W,HDR_H-1);
    SelectObject(dc,op); DeleteObject(pen);
    HBRUSH ba=CreateSolidBrush(CLR_ACCENT);
    HBRUSH ob=(HBRUSH)SelectObject(dc,ba);
    HPEN pn=(HPEN)SelectObject(dc,GetStockObject(NULL_PEN));
    Ellipse(dc,16,12,56,52);
    SelectObject(dc,ob); SelectObject(dc,pn); DeleteObject(ba);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
    HFONT fR=MakeFont(22,1,0),oF=(HFONT)SelectObject(dc,fR);
    RECT rR={16,12,56,52}; DrawTextA(dc,"R",1,&rR,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,oF); DeleteObject(fR);
    HFONT fT=MakeFont(20,1,0); oF=(HFONT)SelectObject(dc,fT);
    SetTextColor(dc,CLR_TEXT);
    RECT rT={68,10,WIN_W-10,40}; DrawTextA(dc,"RLS Automacao VPN",-1,&rT,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,oF); DeleteObject(fT);
    HFONT fS=MakeFont(12,0,0); oF=(HFONT)SelectObject(dc,fS);
    SetTextColor(dc,CLR_DIM);
    RECT rS={70,38,WIN_W-10,60}; DrawTextA(dc,"Sistema VPN Industrial  |  v1.4.0",-1,&rS,DT_LEFT|DT_SINGLELINE);
    SelectObject(dc,oF); DeleteObject(fS);
    EndPaint(hwnd,&ps);
}

static void OnCreate(HWND hwnd)
{
    g_font_ui    =MakeFont(15,0,0); g_font_bold  =MakeFont(15,1,0);
    g_font_status=MakeFont(28,1,0); g_font_mono  =MakeFont(12,0,1);
    g_font_small =MakeFont(13,0,0);
    g_br_bg    =CreateSolidBrush(CLR_BG);    g_br_header=CreateSolidBrush(CLR_HEADER);
    g_br_panel =CreateSolidBrush(CLR_PANEL); g_br_input =CreateSolidBrush(CLR_INPUT_BG);

    h_lbl_status=MkStatic(hwnd,IDC_LBL_STATUS,"...",10,HDR_H+8,700,36,g_font_status);
    LONG s=GetWindowLongA(h_lbl_status,GWL_STYLE);
    SetWindowLongA(h_lbl_status,GWL_STYLE,(s&~SS_LEFT)|SS_CENTER);

    h_lbl_ip=MkStatic(hwnd,IDC_LBL_IP,"IP VPN: ---",10,HDR_H+48,700,18,g_font_bold);
    s=GetWindowLongA(h_lbl_ip,GWL_STYLE);
    SetWindowLongA(h_lbl_ip,GWL_STYLE,(s&~SS_LEFT)|SS_CENTER);

    h_lbl_info=MkStatic(hwnd,IDC_LBL_INFO,"",10,HDR_H+68,700,16,g_font_small);
    s=GetWindowLongA(h_lbl_info,GWL_STYLE);
    SetWindowLongA(h_lbl_info,GWL_STYLE,(s&~SS_LEFT)|SS_CENTER);

    h_progress=CreateWindowExA(0,PROGRESS_CLASSA,NULL,WS_CHILD|PBS_SMOOTH,
        10,HDR_H+90,700,6,hwnd,(HMENU)(UINT_PTR)IDC_PROGRESS,NULL,NULL);
    SendMessageA(h_progress,PBM_SETRANGE,0,MAKELPARAM(0,100));
    SendMessageA(h_progress,PBM_SETBARCOLOR,0,(LPARAM)CLR_ACCENT);
    SendMessageA(h_progress,PBM_SETBKCOLOR, 0,(LPARAM)CLR_PANEL);
    ShowWindow(h_progress,SW_HIDE);

    int by1=HDR_H+102;
    h_btn_connect   =MkBtn(hwnd,IDC_BTN_CONNECT,   "CONECTAR",   10, by1,166,34,g_font_bold);
    h_btn_disconnect=MkBtn(hwnd,IDC_BTN_DISCONNECT,"DESLIGAR",  182, by1,166,34,g_font_bold);
    h_btn_setup     =MkBtn(hwnd,IDC_BTN_SETUP,     "CONFIG NIC",354, by1,166,34,g_font_bold);
    h_btn_reset     =MkBtn(hwnd,IDC_BTN_RESET,     "RESET",     526, by1,184,34,g_font_ui);

    int by2=by1+44;
    h_btn_install=MkBtn(hwnd,IDC_BTN_INSTALL,"INSTALAR SOFTETHER",10,by2,196,28,g_font_small);
    MkStatic(hwnd,0,"IP:",214,by2+4,24,18,g_font_small);
    h_edit_ipst=MkEdit(hwnd,IDC_EDIT_IPST,"192.168.10.1",240,by2+2,124,22,0,g_font_small);
    MkStatic(hwnd,0,"Mask:",370,by2+4,40,18,g_font_small);
    h_edit_mask=MkEdit(hwnd,IDC_EDIT_MASK,"255.255.255.0",414,by2+2,118,22,0,g_font_small);
    h_btn_applyip=MkBtn(hwnd,IDC_BTN_APPLYIP,"APLICAR IP",538,by2,172,28,g_font_small);

    int gy=by2+40;
    CreateWindowA("BUTTON"," Ligacao VPN",WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
        10,gy,700,90,hwnd,NULL,NULL,NULL);
    char port_s[8]; snprintf(port_s,sizeof port_s,"%d",g_config.port);
    MkStatic(hwnd,0,"Servidor:", 18,gy+18,62,18,g_font_small);
    h_edit_host=MkEdit(hwnd,IDC_EDIT_HOST,g_config.host,      82,gy+16,186,22,0,g_font_small);
    MkStatic(hwnd,0,"Porta:",   274,gy+18,40,18,g_font_small);
    h_edit_port=MkEdit(hwnd,IDC_EDIT_PORT,port_s,            318,gy+16, 58,22,ES_NUMBER,g_font_small);
    MkStatic(hwnd,0,"Hub:",     382,gy+18,30,18,g_font_small);
    h_edit_hub =MkEdit(hwnd,IDC_EDIT_HUB, g_config.hub,      416,gy+16,100,22,0,g_font_small);
    MkStatic(hwnd,0,"User:",    522,gy+18,34,18,g_font_small);
    h_edit_user=MkEdit(hwnd,IDC_EDIT_USER,g_config.username,  560,gy+16,144,22,0,g_font_small);
    MkStatic(hwnd,0,"Password:", 18,gy+48,62,18,g_font_small);
    h_edit_pass=MkEdit(hwnd,IDC_EDIT_PASS,g_config.password,   82,gy+46,186,22,ES_PASSWORD,g_font_small);
    h_btn_save=MkBtn(hwnd,IDC_BTN_SAVE,"Guardar Config",278,gy+44,140,26,g_font_small);
    h_lbl_msg=MkStatic(hwnd,IDC_LBL_MSG,"",428,gy+48,276,18,g_font_small);

    int ly=gy+100;
    MkStatic(hwnd,0,"Log:",10,ly,40,16,g_font_small);
    h_log=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        10,ly+18,700,WIN_H-ly-28,hwnd,(HMENU)(UINT_PTR)IDC_LOG,NULL,NULL);
    SendMessageA(h_log,WM_SETFONT,(WPARAM)g_font_mono,FALSE);
    SendMessageA(h_log,EM_SETLIMITTEXT,40960,0);
}



/* ── Status poll timer ───────────────────────────────────────────── */
static void CALLBACK StatusTimer(HWND hwnd, UINT m, UINT_PTR id, DWORD t)
{ (void)m;(void)id;(void)t; PostMessageA(hwnd,WM_VPN_STATUS,0,0); }

/* ── Window procedure ────────────────────────────────────────────── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
        vpn_default_config(&g_config);
        OnCreate(hwnd);
        vpn_set_log_fn(vpn_log_callback, NULL);
        SetTimer(hwnd, 1, 3000, StatusTimer);
        PostMessageA(hwnd, WM_VPN_STATUS, 0, 0);
        if (!vpn_is_installed())
            PostMessageA(hwnd, WM_VPN_LOG, 0,
                (LPARAM)_strdup("[AVISO] SoftEther nao instalado - use INSTALAR SOFTETHER."));
        return 0;

    case WM_PAINT:
        PaintHeader(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        HDC dc=(HDC)wp; RECT rc; GetClientRect(hwnd,&rc);
        FillRect(dc,&rc,g_br_bg); return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp; HWND ctrl=(HWND)lp;
        SetBkMode(dc,TRANSPARENT);
        if(ctrl==h_lbl_status){
            switch(g_last_state){
                case VPN_CONNECTED:  SetTextColor(dc,CLR_GREEN);  break;
                case VPN_CONNECTING: SetTextColor(dc,CLR_YELLOW); break;
                default:             SetTextColor(dc,CLR_RED);    break;
            }
        } else if(ctrl==h_lbl_ip){
            SetTextColor(dc,CLR_ACCENT);
        } else if(ctrl==h_lbl_msg){
            SetTextColor(dc,CLR_GREEN);
        } else {
            SetTextColor(dc,CLR_DIM);
        }
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc=(HDC)wp;
        SetBkColor(dc,CLR_INPUT_BG); SetTextColor(dc,CLR_TEXT);
        return (LRESULT)g_br_input;
    }

    case WM_COMMAND: {
        int id=LOWORD(wp);
        if(HIWORD(wp)==BN_CLICKED){
            switch(id){
                case IDC_BTN_CONNECT:    StartOp(2,NULL,NULL); break;
                case IDC_BTN_DISCONNECT: StartOp(3,NULL,NULL); break;
                case IDC_BTN_SETUP:      StartOp(1,NULL,NULL); break;
                case IDC_BTN_RESET:      StartOp(4,NULL,NULL); break;
                case IDC_BTN_INSTALL:    StartOp(5,NULL,NULL); break;
                case IDC_BTN_APPLYIP: {
                    char ip[64]="",mask[64]="";
                    GetWindowTextA(h_edit_ipst,ip,  sizeof ip);
                    GetWindowTextA(h_edit_mask,mask, sizeof mask);
                    StartOp(6,ip,mask); break;
                }
                case IDC_BTN_SAVE: SaveConfig(); break;
            }
        }
        return 0;
    }

    case WM_VPN_LOG: {
        const char *txt=(const char *)lp;
        if(txt){log_append(txt);free((void *)txt);}
        return 0;
    }

    case WM_VPN_PROGRESS:
        SendMessageA(h_progress,PBM_SETPOS,(WPARAM)wp,0);
        return 0;

    case WM_VPN_RESULT: {
        const char *txt=(const char *)lp;
        g_busy=0;
        ShowWindow(h_progress,SW_HIDE);
        EnableAllButtons();
        if(txt){log_append(txt);SetWindowTextA(h_lbl_msg,txt);free((void *)txt);}
        PostMessageA(hwnd,WM_VPN_STATUS,0,0);
        return 0;
    }

    case WM_VPN_STATUS:
        RefreshStatus();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm=(MINMAXINFO *)lp;
        mm->ptMinTrackSize.x=WIN_W; mm->ptMinTrackSize.y=WIN_H;
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd,1);
        DeleteObject(g_br_bg); DeleteObject(g_br_header);
        DeleteObject(g_br_panel); DeleteObject(g_br_input);
        DeleteObject(g_font_ui); DeleteObject(g_font_bold);
        DeleteObject(g_font_status); DeleteObject(g_font_mono);
        DeleteObject(g_font_small);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

/* ── WinMain ─────────────────────────────────────────────────────── */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nCmdShow)
{
    (void)hPrev; (void)lpCmd;
    INITCOMMONCONTROLSEX icc={sizeof icc,ICC_WIN95_CLASSES|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc={0};
    wc.cbSize=sizeof wc; wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hCursor=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW);
    wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName=L"RLS_VPN_WIN32";
    wc.hIcon=LoadIconW(hInst,MAKEINTRESOURCEW(1));
    wc.hIconSm=LoadIconW(hInst,MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    RECT rc={0,0,WIN_W,WIN_H};
    AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,FALSE);

    g_hwnd=CreateWindowExW(0,L"RLS_VPN_WIN32",
        L"RLS Automa\u00e7\u00e3o VPN Client v1.4.0",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,
        rc.right-rc.left,rc.bottom-rc.top,
        NULL,NULL,hInst,NULL);
    if(!g_hwnd) return 1;
    ShowWindow(g_hwnd,nCmdShow);
    UpdateWindow(g_hwnd);
    MSG m;
    while(GetMessageW(&m,NULL,0,0)>0){
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
