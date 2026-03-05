/*
 * main.c — RLS Automação VPN Client v1.3.0
 * Win32 host for WebView2 + vpn.c backend.
 * vpn.c / vpn.h are NEVER modified.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vpn.h"
#include "webview_bridge.h"

/* ── Custom messages (posted to g_hwnd) ──────────────────────────── */
#define WM_VPN_LOG      (WM_APP + 1)  /* LPARAM = strdup'd UTF-8, free after use */
#define WM_VPN_RESULT   (WM_APP + 2)  /* WPARAM = ok(1/0), LPARAM = strdup'd msg */
#define WM_VPN_PROGRESS (WM_APP + 3)  /* WPARAM = percent 0-100, LPARAM = strdup'd step */
#define WM_VPN_STATUS   (WM_APP + 4)  /* no params — poll and send status JSON */

/* ── Globals ─────────────────────────────────────────────────────── */
static HWND        g_hwnd     = NULL;
static int         g_busy     = 0;   /* 1 while a worker thread runs */
static VpnConfig   g_config;         /* persisted across reconfig */

/* ── HTML resource loader ────────────────────────────────────────── */
static void load_ui_html(void)
{
    HRSRC   hRes = FindResourceW(NULL, MAKEINTRESOURCEW(102), RT_RCDATA);
    if (!hRes) return;
    HGLOBAL hMem = LoadResource(NULL, hRes);
    if (!hMem) return;
    DWORD   sz   = SizeofResource(NULL, hRes);
    void   *ptr  = LockResource(hMem);
    if (ptr && sz) wv_navigate_html((const char *)ptr, (int)sz);
}

/* ── JSON helpers ────────────────────────────────────────────────── */

/* Escape a string for JSON (replaces \ " \n \r \t).
 * out must be at least 3*strlen(in)+3 bytes. */
static void json_esc(const char *in, char *out, int outsz)
{
    int i = 0;
    out[i++] = '"';
    for (const char *p = in; *p && i < outsz - 4; p++) {
        switch (*p) {
            case '\\': out[i++]='\\'; out[i++]='\\'; break;
            case '"':  out[i++]='\\'; out[i++]='"';  break;
            case '\n': out[i++]='\\'; out[i++]='n';  break;
            case '\r': out[i++]='\\'; out[i++]='r';  break;
            case '\t': out[i++]='\\'; out[i++]='t';  break;
            default:   out[i++]= *p; break;
        }
    }
    out[i++] = '"';
    out[i]   = '\0';
}

static void send_status_json(const VpnStatus *st)
{
    static const char *state_names[] = {
        "NOT_INSTALLED", "NOT_CONFIGURED",
        "DISCONNECTED", "CONNECTING", "CONNECTED"
    };
    const char *state = (st->state >= 0 && st->state <= 4)
                        ? state_names[st->state] : "DISCONNECTED";

    char e_msg[1024], e_ip[192], e_srv[576], e_user[320], e_acct[192];
    json_esc(st->message,           e_msg,  sizeof e_msg);
    json_esc(st->local_ip,          e_ip,   sizeof e_ip);
    json_esc(g_config.host,         e_srv,  sizeof e_srv);
    json_esc(g_config.username,     e_user, sizeof e_user);
    json_esc(g_config.account_name, e_acct, sizeof e_acct);

    char buf[2048];
    snprintf(buf, sizeof buf,
        "{\"type\":\"status\","
        "\"state\":\"%s\","
        "\"busy\":%s,"
        "\"message\":%s,"
        "\"ip\":%s,"
        "\"server\":%s,"
        "\"user\":%s,"
        "\"account\":%s}",
        state,
        g_busy ? "true" : "false",
        e_msg, e_ip, e_srv, e_user, e_acct);

    wv_post_json(buf);
}

static void send_log_json(const char *msg)
{
    char e_msg[2048];
    json_esc(msg, e_msg, sizeof e_msg);
    char buf[2200];
    snprintf(buf, sizeof buf, "{\"type\":\"log\",\"msg\":%s}", e_msg);
    wv_post_json(buf);
}

static void send_progress_json(int pct, const char *step)
{
    char e_step[512];
    json_esc(step ? step : "", e_step, sizeof e_step);
    char buf[640];
    snprintf(buf, sizeof buf,
        "{\"type\":\"progress\",\"value\":%d,\"step\":%s}", pct, e_step);
    wv_post_json(buf);
}

static void send_result_json(int ok, const char *msg)
{
    char e_msg[1024];
    json_esc(msg ? msg : "", e_msg, sizeof e_msg);
    char buf[1100];
    snprintf(buf, sizeof buf,
        "{\"type\":\"result\",\"ok\":%s,\"msg\":%s}",
        ok ? "true" : "false", e_msg);
    wv_post_json(buf);
}

static void send_config_json(void)
{
    char e_host[576], e_hub[192], e_user[320];
    json_esc(g_config.host,     e_host, sizeof e_host);
    json_esc(g_config.hub,      e_hub,  sizeof e_hub);
    json_esc(g_config.username, e_user, sizeof e_user);
    char buf[1024];
    snprintf(buf, sizeof buf,
        "{\"type\":\"config\","
        "\"host\":%s,"
        "\"port\":%d,"
        "\"hub\":%s,"
        "\"user\":%s}",
        e_host, g_config.port, e_hub, e_user);
    wv_post_json(buf);
}

/* ── JSON parser helpers (no external library) ───────────────────── */

/* Extract string value for key from flat JSON object. */
static int json_str(const char *json, const char *key, char *out, int out_len)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static int json_int(const char *json, const char *key, int *out)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
    *out = atoi(p);
    return 1;
}

/* ── vpn_log_callback (called on worker thread) ──────────────────── */
static void vpn_log_callback(const char *msg, void *ctx)
{
    (void)ctx;
    if (!msg || !g_hwnd) return;

    /* Parse [N/M] progress tag */
    int num = 0, den = 0;
    char step[256] = "";
    const char *bracket = strchr(msg, '[');
    if (bracket) {
        sscanf(bracket, "[%d/%d]", &num, &den);
        /* Step description follows the tag */
        const char *after = strchr(bracket, ']');
        if (after) {
            after++;
            while (*after == ' ') after++;
            strncpy(step, after, sizeof(step)-1);
        }
    }

    if (num > 0 && den > 0) {
        int pct = (num * 100) / den;
        char *stepdup = _strdup(step[0] ? step : msg);
        PostMessageA(g_hwnd, WM_VPN_PROGRESS, (WPARAM)pct, (LPARAM)stepdup);
    }

    PostMessageA(g_hwnd, WM_VPN_LOG, 0, (LPARAM)_strdup(msg));
}

/* ── Worker thread ───────────────────────────────────────────────── */
typedef struct {
    int         op;   /* 1=setup 2=connect 3=disconnect 4=reset 5=install+setup 6=applyip */
    VpnConfig   cfg;
    char        se_dir[MAX_PATH];
    char        ip[64];
    char        mask[64];
} ThreadData;

static DWORD WINAPI WorkerThread(LPVOID param)
{
    ThreadData *td = (ThreadData *)param;
    int ok  = 0;
    char msg[512] = {0};

    switch (td->op) {
        case 1: {
            int r = vpn_setup(&td->cfg);
            ok = (r == 0);
            strncpy(msg, ok ? "NIC e conta VPN configurados com sucesso."
                             : "Falha ao configurar a NIC/conta VPN.", sizeof msg - 1);
            break;
        }
        case 2: {
            int r = vpn_connect(&td->cfg);
            ok = (r == 0);
            strncpy(msg, ok ? "Ligacao estabelecida."
                             : "Falha ao ligar a VPN.", sizeof msg - 1);
            break;
        }
        case 3: {
            vpn_disconnect();
            ok = 1;
            strncpy(msg, "VPN desligada.", sizeof msg - 1);
            break;
        }
        case 4: {
            vpn_reset();
            ok = 1;
            strncpy(msg, "Configuracao reposta.", sizeof msg - 1);
            break;
        }
        case 5: {
            int r = vpn_install_silent(td->se_dir);
            if (r != 0) {
                ok = 0;
                strncpy(msg, "Falha ao instalar o SoftEther VPN.", sizeof msg - 1);
                break;
            }
            vpn_start_service();
            r = vpn_setup(&td->cfg);
            ok = (r == 0);
            strncpy(msg, ok ? "SoftEther instalado e configurado."
                             : "Instalacao OK mas a configuracao falhou.", sizeof msg - 1);
            break;
        }
        case 6: {
            int r = vpn_set_static_ip(NIC_NAME, td->ip, td->mask);
            ok = (r == 0);
            strncpy(msg, ok ? "IP estatico aplicado."
                             : "Falha ao aplicar IP estatico.", sizeof msg - 1);
            break;
        }
        default:
            strncpy(msg, "Operacao desconhecida.", sizeof msg - 1);
            break;
    }

    PostMessageA(g_hwnd, WM_VPN_RESULT, (WPARAM)ok, (LPARAM)_strdup(msg));
    free(td);
    return 0;
}

static void StartOp(int op, const char *ip, const char *mask)
{
    if (g_busy) return;
    g_busy = 1;

    /* Immediately acknowledge as busy */
    VpnStatus st = {0};
    vpn_get_status(&st);
    send_status_json(&st);

    ThreadData *td = (ThreadData *)calloc(1, sizeof(ThreadData));
    if (!td) { g_busy = 0; return; }
    td->op  = op;
    td->cfg = g_config;

    if (op == 5) {
        GetModuleFileNameA(NULL, td->se_dir, MAX_PATH);
        char *slash = strrchr(td->se_dir, '\\');
        if (slash) *(slash + 1) = '\0';
    }
    if (ip)   strncpy(td->ip,   ip,   sizeof td->ip   - 1);
    if (mask) strncpy(td->mask, mask, sizeof td->mask - 1);

    HANDLE hThread = CreateThread(NULL, 0, WorkerThread, td, 0, NULL);
    if (hThread) CloseHandle(hThread);
    else { free(td); g_busy = 0; }
}

/* ── JS command router ───────────────────────────────────────────── */
static void js_command(const char *json)
{
    char cmd[64] = "";
    json_str(json, "cmd", cmd, sizeof cmd);

    if      (strcmp(cmd, "connect")    == 0) StartOp(2, NULL, NULL);
    else if (strcmp(cmd, "disconnect") == 0) StartOp(3, NULL, NULL);
    else if (strcmp(cmd, "setup")      == 0) StartOp(1, NULL, NULL);
    else if (strcmp(cmd, "install")    == 0) StartOp(5, NULL, NULL);
    else if (strcmp(cmd, "reset")      == 0) StartOp(4, NULL, NULL);
    else if (strcmp(cmd, "applyIp")    == 0) {
        char ip[64] = "", mask[64] = "";
        json_str(json, "ip",   ip,   sizeof ip);
        json_str(json, "mask", mask, sizeof mask);
        StartOp(6, ip, mask);
    }
    else if (strcmp(cmd, "saveConfig") == 0) {
        char host[256]="", hub[64]="", user[64]="", pass[128]="";
        int  port = DEFAULT_PORT;
        json_str(json, "host", host, sizeof host);
        json_str(json, "hub",  hub,  sizeof hub);
        json_str(json, "user", user, sizeof user);
        json_str(json, "pass", pass, sizeof pass);
        json_int(json, "port", &port);
        if (host[0]) strncpy(g_config.host,         host, sizeof g_config.host - 1);
        if (hub[0])  strncpy(g_config.hub,           hub,  sizeof g_config.hub  - 1);
        if (user[0]) strncpy(g_config.username,      user, sizeof g_config.username - 1);
        if (pass[0]) strncpy(g_config.password,      pass, sizeof g_config.password - 1);
        if (port > 0) g_config.port = port;
        send_result_json(1, "Configuracao guardada.");
    }
    else if (strcmp(cmd, "getConfig")  == 0) {
        send_config_json();
    }
}

/* ── Status poll timer ───────────────────────────────────────────── */
static void CALLBACK StatusTimer(HWND hwnd, UINT m, UINT_PTR id, DWORD t)
{
    (void)m; (void)id; (void)t;
    PostMessageA(hwnd, WM_VPN_STATUS, 0, 0);
}

/* ── Window procedure ────────────────────────────────────────────── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        if (!wv_create(hwnd, js_command)) {
            MessageBoxW(hwnd,
                L"WebView2 nao encontrado.\n"
                L"Certifique-se de que WebView2Loader.dll esta junto ao executavel\n"
                L"e que o Microsoft Edge WebView2 Runtime esta instalado.",
                L"RLS VPN \u2014 Erro de inicializacao",
                MB_ICONERROR | MB_OK);
        }
        vpn_set_log_fn(vpn_log_callback, hwnd);
        vpn_default_config(&g_config);
        load_ui_html();
        SetTimer(hwnd, 1, 3000, StatusTimer);
        PostMessageA(hwnd, WM_VPN_STATUS, 0, 0);
        if (!vpn_is_installed()) {
            PostMessageA(hwnd, WM_VPN_LOG, 0,
                (LPARAM)_strdup("SoftEther nao instalado. Clique em INSTALAR SOFTETHER."));
        }
        return 0;
    }

    case WM_SIZE:
        wv_resize((int)LOWORD(lp), (int)HIWORD(lp));
        return 0;

    case WM_VPN_LOG: {
        const char *txt = (const char *)lp;
        if (txt) { send_log_json(txt); free((void *)txt); }
        return 0;
    }

    case WM_VPN_PROGRESS: {
        const char *step = (const char *)lp;
        send_progress_json((int)wp, step ? step : "");
        if (step) free((void *)step);
        return 0;
    }

    case WM_VPN_RESULT: {
        int ok = (int)wp;
        const char *txt = (const char *)lp;
        g_busy = 0;
        if (txt) { send_result_json(ok, txt); free((void *)txt); }
        PostMessageA(hwnd, WM_VPN_STATUS, 0, 0);
        return 0;
    }

    case WM_VPN_STATUS: {
        VpnStatus st = {0};
        vpn_get_status(&st);
        send_status_json(&st);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        wv_destroy();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ── WinMain ─────────────────────────────────────────────────────── */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nCmdShow)
{
    (void)hPrev; (void)lpCmd;

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc   = {0};
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"RLS_VPN_WV2";
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0,
        L"RLS_VPN_WV2",
        L"RLS Automa\u00e7\u00e3o VPN Client v1.3.0",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        760, 740,
        NULL, NULL, hInst, NULL);

    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
