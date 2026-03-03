/* vpn.c - RLS Automacao VPN - SoftEther vpncmd wrapper */
#define _CRT_SECURE_NO_WARNINGS
#include "vpn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static char s_vpncmd[MAX_PATH] = {0};

/* ─── Find vpncmd.exe ──────────────────────────────── */
static const char *find_vpncmd(void) {
    if (s_vpncmd[0]) return s_vpncmd;
    static const char *paths[] = {
        "C:\\Program Files\\SoftEther VPN Client\\vpncmd.exe",
        "C:\\Program Files (x86)\\SoftEther VPN Client\\vpncmd.exe",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        if (GetFileAttributesA(paths[i]) != INVALID_FILE_ATTRIBUTES) {
            strncpy(s_vpncmd, paths[i], MAX_PATH - 1);
            return s_vpncmd;
        }
    }
    return NULL;
}

/* ─── Execute command, capture stdout+stderr ───────── */
static int exec_capture(const char *cmdline, char *out, int out_len) {
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return 0;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.hStdInput   = INVALID_HANDLE_VALUE;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    /* CreateProcess needs a mutable buffer */
    char cmd[8192];
    strncpy(cmd, cmdline, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = 0;

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        if (out && out_len > 0) strncpy(out, "exec failed", out_len - 1);
        return 0;
    }

    /* Read all output */
    int pos = 0;
    DWORD nr;
    char buf[1024];
    while (ReadFile(hRead, buf, sizeof(buf), &nr, NULL) && nr > 0) {
        if (out && pos + (int)nr < out_len - 1) {
            memcpy(out + pos, buf, nr);
            pos += nr;
        }
    }
    if (out && out_len > 0) out[pos] = '\0';

    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return 1;
}

/* ─── Run vpncmd.exe with given arg string ─────────── */
/* args example: "localhost /CLIENT /CMD NicList"        */
static int run_vpncmd(const char *args, char *out, int out_len) {
    const char *vpncmd = find_vpncmd();
    if (!vpncmd) {
        if (out && out_len > 0) strncpy(out, "vpncmd.exe nao encontrado", out_len - 1);
        return 0;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", vpncmd, args);
    return exec_capture(cmd, out, out_len);
}

/* ─── Run sc.exe for service control ───────────────── */
static void sc_cmd(const char *args) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sc.exe %s", args);
    exec_capture(cmd, NULL, 0);
}

/* ─── Wait for SevpnClient to respond ─────────────── */
static int wait_for_service(int max_secs) {
    char out[4096];
    for (int i = 0; i < max_secs; i++) {
        out[0] = 0;
        run_vpncmd("localhost /CLIENT /CMD NicList", out, sizeof(out));
        if (strstr(out, "The command completed")) return 1;
        Sleep(1000);
    }
    return 0;
}

/* ─── Public API ────────────────────────────────────── */

void vpn_default_config(VpnConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host,         DEFAULT_HOST,  sizeof(cfg->host) - 1);
    cfg->port = DEFAULT_PORT;
    strncpy(cfg->hub,          DEFAULT_HUB,   sizeof(cfg->hub) - 1);
    strncpy(cfg->username,     DEFAULT_USER,  sizeof(cfg->username) - 1);
    strncpy(cfg->password,     DEFAULT_PASS,  sizeof(cfg->password) - 1);
    strncpy(cfg->account_name, ACCOUNT_NAME,  sizeof(cfg->account_name) - 1);
}

int vpn_is_installed(void) {
    s_vpncmd[0] = 0; /* force re-check path */
    return find_vpncmd() != NULL;
}

int vpn_start_service(void) {
    sc_cmd("config SevpnClient start= auto");
    sc_cmd("start SevpnClient");
    return 1;
}

int vpn_install_silent(const char *installer_path) {
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd), "\"%s\" /SILENT /NORESTART", installer_path);
    exec_capture(cmd, NULL, 0);
    Sleep(4000);
    vpn_start_service();
    Sleep(3000);
    return vpn_is_installed();
}

int vpn_setup(const VpnConfig *cfg, char *out, int n) {
    char buf[4096];

    vpn_start_service();
    if (!wait_for_service(30)) {
        snprintf(out, n, "Servico VPN nao iniciou. Reinicia o Windows.");
        return 0;
    }

    /* Check if NIC already exists */
    buf[0] = 0;
    run_vpncmd("localhost /CLIENT /CMD NicList", buf, sizeof(buf));
    int nic_exists = strstr(buf, NIC_NAME) != NULL;

    if (!nic_exists) {
        /* Remove legacy NIC names */
        run_vpncmd("localhost /CLIENT /CMD NicDelete VPN",                       buf, sizeof(buf));
        run_vpncmd("localhost /CLIENT /CMD NicDelete \"VPN Client\"",            buf, sizeof(buf));
        run_vpncmd("localhost /CLIENT /CMD NicDelete \"VPN RLS Automacao\"",     buf, sizeof(buf));
        run_vpncmd("localhost /CLIENT /CMD NicDelete \"RLS Automacao\"",         buf, sizeof(buf));

        /* Create the NIC */
        buf[0] = 0;
        run_vpncmd("localhost /CLIENT /CMD NicCreate " NIC_NAME, buf, sizeof(buf));
        if (strstr(buf, "Cannot connect")) {
            snprintf(out, n, "Falha ao criar adaptador de rede: %.150s", buf);
            return 0;
        }

        /* SevpnClient may restart after NicCreate - wait again */
        Sleep(2000);
        if (!wait_for_service(30)) {
            snprintf(out, n, "Servico VPN nao retomou apos criar adaptador.");
            return 0;
        }

        run_vpncmd("localhost /CLIENT /CMD NicEnable " NIC_NAME, buf, sizeof(buf));
    }

    /* Create or update VPN account */
    char args[1024];
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountCreate %s"
        " /SERVER:%s:%u /HUB:%s /USERNAME:%s /NICNAME:%s",
        cfg->account_name, cfg->host, cfg->port,
        cfg->hub, cfg->username, NIC_NAME);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    if (strstr(buf, "Cannot connect to VPN Client")) {
        snprintf(out, n, "Servico VPN nao responde ao criar conta.");
        return 0;
    }

    /* Set password */
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountPasswordSet %s /PASSWORD:%s /TYPE:standard",
        cfg->account_name, cfg->password);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    if (strstr(buf, "Cannot connect to VPN Client")) {
        snprintf(out, n, "Servico VPN nao responde ao definir password.");
        return 0;
    }

    snprintf(out, n, "Placa de rede e conta VPN configuradas com sucesso.");
    return 1;
}

int vpn_connect(const VpnConfig *cfg, char *out, int n) {
    char buf[4096];

    sc_cmd("start SevpnClient");
    if (!wait_for_service(15)) {
        snprintf(out, n, "Servico VPN nao responde. Aguarda e tenta de novo.");
        return 0;
    }

    char args[256];
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountConnect %s", cfg->account_name);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));

    if (strstr(buf, "The command completed successfully")) {
        snprintf(out, n, "Ligacao iniciada com sucesso.");
        return 1;
    } else if (strstr(buf, "already") || strstr(buf, "Already")) {
        snprintf(out, n, "Ja esta ligado a rede RLS Automacao.");
        return 1;
    } else if (strstr(buf, "Cannot connect to VPN Client")) {
        snprintf(out, n, "Servico VPN nao responde. Aguarda e tenta novamente.");
        return 0;
    }

    snprintf(out, n, "Falha ao ligar: %.200s", buf);
    return 0;
}

int vpn_disconnect(const VpnConfig *cfg, char *out, int n) {
    char buf[4096], args[256];
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountDisconnect %s", cfg->account_name);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    snprintf(out, n, "Desligado com sucesso.");
    return 1;
}

/* Get VPN adapter IP via PowerShell */
static void get_vpn_ip(char *ip_out, int ip_len) {
    char script[2048];
    snprintf(script, sizeof(script),
        "powershell.exe -NonInteractive -WindowStyle Hidden -Command \""
        "$names = @('%s', 'VPN Client Adapter - %s', 'VPN - %s'); "
        "foreach ($n in $names) { "
        "  $ip = Get-NetIPAddress -InterfaceAlias $n -AddressFamily IPv4 "
        "        -ErrorAction SilentlyContinue | "
        "        Where-Object { $_.IPAddress -notlike '169.254.*' } | "
        "        Select-Object -ExpandProperty IPAddress -First 1; "
        "  if ($ip) { Write-Output $ip; exit } } "
        "Get-NetAdapter -ErrorAction SilentlyContinue | "
        "  Where-Object { $_.InterfaceDescription -like '*SoftEther*' -or "
        "                 $_.InterfaceDescription -like '*TAP-Windows*' } | "
        "  ForEach-Object { "
        "    $ip = $_ | Get-NetIPAddress -AddressFamily IPv4 "
        "               -ErrorAction SilentlyContinue | "
        "               Where-Object { $_.IPAddress -notlike '169.254.*' } | "
        "               Select-Object -ExpandProperty IPAddress -First 1; "
        "    if ($ip) { Write-Output $ip; exit } }\"",
        NIC_NAME, NIC_NAME, NIC_NAME);

    char out[512] = {0};
    exec_capture(script, out, sizeof(out));

    /* Trim whitespace */
    char *p = out;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    char *end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\n' || end[-1] == '\t'))
        *--end = 0;

    if (*p && strlen(p) <= 15 && !strstr(p, "169.254"))
        strncpy(ip_out, p, ip_len - 1);
}

void vpn_get_status(const VpnConfig *cfg, VpnStatus *s) {
    memset(s, 0, sizeof(*s));

    if (!vpn_is_installed()) {
        s->state = VPN_NOT_INSTALLED;
        strncpy(s->message, "A instalar componentes VPN...", sizeof(s->message) - 1);
        return;
    }
    s->softether_ready = 1;

    char args[256];
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountStatusGet %s", cfg->account_name);
    char out[4096] = {0};
    run_vpncmd(args, out, sizeof(out));

    /* Extract useful part of output for diagnostics */
    {
        const char *start = strstr(out, "AccountStatusGet");
        if (!start) start = strstr(out, "Session Status");
        if (!start) {
            size_t len = strlen(out);
            start = out + (len > 500 ? len - 500 : 0);
        }
        strncpy(s->raw_status, start, sizeof(s->raw_status) - 1);
    }

    /* Service not responding */
    if (strstr(out, "Cannot connect to VPN Client")) {
        s->state = VPN_DISCONNECTED;
        strncpy(s->message, "Servico VPN nao responde", sizeof(s->message) - 1);
        return;
    }

    /* Build lowercase copy for pattern matching */
    char lower[4096];
    int i;
    for (i = 0; out[i] && i < (int)sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)out[i]);
    lower[i] = 0;

    /* Account not found -> needs configuration */
    int account_ok = !strstr(lower, "not found") && out[0] != 0;
    s->connection_ready = account_ok;
    if (!account_ok) {
        s->state = VPN_NOT_CONFIGURED;
        strncpy(s->message, "Conta VPN nao configurada", sizeof(s->message) - 1);
        return;
    }

    /* Determine connection state */
    int session_up = strstr(lower, "connection established") != NULL ||
                     (strstr(lower, "session status") != NULL &&
                      strstr(lower, "not connected") == NULL &&
                      strstr(lower, "connect") != NULL);

    get_vpn_ip(s->local_ip, sizeof(s->local_ip));
    int has_ip = s->local_ip[0] != 0;

    s->connected = session_up || has_ip;
    if (s->connected) {
        s->state = VPN_CONNECTED;
        strncpy(s->message, "Ligado a rede RLS Automacao", sizeof(s->message) - 1);
    } else if (strstr(lower, "connecting")) {
        s->state = VPN_CONNECTING;
        strncpy(s->message, "A estabelecer ligacao...", sizeof(s->message) - 1);
    } else {
        s->state = VPN_DISCONNECTED;
        strncpy(s->message, "Pronto para ligar", sizeof(s->message) - 1);
    }
}

int vpn_reset(const VpnConfig *cfg, char *out, int n) {
    char buf[4096], args[256];

    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountDisconnect %s", cfg->account_name);
    run_vpncmd(args, buf, sizeof(buf));

    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountDelete %s", cfg->account_name);
    run_vpncmd(args, buf, sizeof(buf));

    run_vpncmd("localhost /CLIENT /CMD NicDelete " NIC_NAME, buf, sizeof(buf));

    snprintf(out, n, "Reset completo. Adaptador e conta removidos.");
    return 1;
}
