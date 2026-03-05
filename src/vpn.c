/* vpn.c - RLS Automacao VPN - SoftEther vpncmd wrapper */
#define _CRT_SECURE_NO_WARNINGS
#include "vpn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static char s_vpncmd[MAX_PATH] = {0};
static CRITICAL_SECTION s_vpncmd_cs;  /* serializa todas as chamadas vpncmd */
static BOOL             s_vpncmd_cs_ready = FALSE;

/* ─── Log callback (called on worker threads) ───────── */
static VpnLogFn  s_log_fn  = NULL;
static void     *s_log_ctx = NULL;

void vpn_set_log_fn(VpnLogFn fn, void *ctx) {
    s_log_fn  = fn;
    s_log_ctx = ctx;
}

static void vpn_log(const char *msg) {
    if (s_log_fn) s_log_fn(msg, s_log_ctx);
}

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
    /* Serializa: apenas UM vpncmd.exe a correr de cada vez (qualquer thread) */
    if (s_vpncmd_cs_ready) EnterCriticalSection(&s_vpncmd_cs);
    int r = exec_capture(cmd, out, out_len);
    if (s_vpncmd_cs_ready) LeaveCriticalSection(&s_vpncmd_cs);
    return r;
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
    char logbuf[64];
    for (int i = 0; i < max_secs; i++) {
        out[0] = 0;
        run_vpncmd("localhost /CLIENT /CMD NicList", out, sizeof(out));
        if (strstr(out, "The command completed")) return 1;
        snprintf(logbuf, sizeof(logbuf), "Aguardando servico VPN... (%d/%d)", i + 1, max_secs);
        vpn_log(logbuf);
        Sleep(1000);
    }
    return 0;
}

/* ─── Public API ────────────────────────────────────── */

void vpn_default_config(VpnConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    /* Inicializa CS uma única vez (chamado no thread UI antes de qualquer thread) */
    if (!s_vpncmd_cs_ready) {
        InitializeCriticalSection(&s_vpncmd_cs);
        s_vpncmd_cs_ready = TRUE;
    }
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

/* ─── Return last N chars of a string (where the real error usually is) ── */
static const char *tail(const char *s, int n) {
    int len = (int)strlen(s);
    return s + (len > n ? len - n : 0);
}

/* ─── True if vpncmd output indicates the command succeeded ─────────────── */
static int cmd_ok(const char *buf) {
    return strstr(buf, "The command completed successfully") != NULL
        || strstr(buf, "command completed") != NULL;
}

/* ─── Parse a field value from NicList table output ──────────────────────
   Finds lines of the form "Field Name|Value" and returns the value.
   Used for both "VPN Client Adapter Name" and "MAC Address".             */
static int parse_niclist_field(const char *out, const char *field,
                                char *val, int val_len) {
    if (!out || !field || !val || val_len < 2) return 0;
    val[0] = 0;
    const char *p = out;
    while ((p = strstr(p, field)) != NULL) {
        const char *after = p + strlen(field);
        /* Skip spaces/tabs then expect '|' (not letters — avoids partial matches) */
        while (*after == ' ' || *after == '\t') after++;
        if (*after == '|') {
            after++;
            while (*after == ' ' || *after == '\t') after++;
            int i = 0;
            while (*after && *after != '\r' && *after != '\n' && i < val_len - 1)
                val[i++] = *after++;
            val[i] = 0;
            while (i > 0 && (val[i-1]==' '||val[i-1]=='\t')) val[--i] = 0;
            if (val[0]) return 1;
        }
        p++;
    }
    return 0;
}

/* ─── Parse first NIC name from vpncmd NicList output ───────────────── */
/* Real field name confirmed from SoftEther 4.41 output: "Virtual Network Adapter Name" */
static int parse_first_nic_name(const char *out, char *name, int name_len) {
    return parse_niclist_field(out, "Virtual Network Adapter Name", name, name_len);
}

/* ─── Parse NIC MAC address from vpncmd NicList output ──────────────── */
/* NicList has both "MAC Address" and "MAC Address of Virtual Host".
   We want only the physical TAP MAC ("MAC Address" followed directly by |). */
static int parse_nic_mac(const char *out, char *mac, int mac_len) {
    /* Use the generic parser — it finds "MAC Address|value" (stops at |)
       and will not match "MAC Address of Virtual Host|" because that field
       has "of Virtual Host" before the | so it's handled correctly.        */
    return parse_niclist_field(out, "MAC Address", mac, mac_len);
}

/* ─── Forward declaration ────────────────────────────────────────────────── */
static int find_vpn_adapter(char *name_out, int name_len);

/* ─── Skip SoftEther startup banner, return pointer to actual output ──────
   vpncmd always prints a multi-line version/copyright header before the
   actual command response. We skip past the "VPN Client>" prompt line.    */
static const char *skip_banner(const char *s) {
    if (!s) return s;
    /* After the prompt line comes the actual command output */
    const char *p = strstr(s, "VPN Client>");
    if (p) { p = strchr(p, '\n'); if (p) return p + 1; }
    /* Fallback: skip past "Connected to VPN Client" line */
    p = strstr(s, "Connected to VPN Client");
    if (p) { p = strchr(p, '\n'); if (p) return p + 1; }
    return s;
}

/* ─── Log the meaningful part of a vpncmd response ───────────────────────
   Strips the banner, then logs up to 300 chars stopping at the first
   blank line (which separates the command result from trailing noise).    */
static void vpn_log_output(const char *prefix, const char *buf) {
    if (!buf || !buf[0]) return;
    const char *s = skip_banner(buf);
    while (*s == '\r' || *s == '\n') s++;   /* trim leading blank lines */
    if (!*s) return;
    char tmp[400];
    int i = 0;
    while (*s && i < 300) {
        /* Stop at double-newline (end of the relevant output block) */
        if ((s[0]=='\r'&&s[1]=='\n'&&(s[2]=='\r'||s[2]=='\n')) ||
            (s[0]=='\n'&&s[1]=='\n')) break;
        tmp[i++] = *s++;
    }
    tmp[i] = 0;
    while (i > 0 && (tmp[i-1]=='\r'||tmp[i-1]=='\n'||tmp[i-1]==' ')) tmp[--i] = 0;
    if (!tmp[0]) return;
    char logbuf[512];
    if (prefix && prefix[0])
        snprintf(logbuf, sizeof(logbuf), "%s %s", prefix, tmp);
    else
        strncpy(logbuf, tmp, sizeof(logbuf)-1);
    vpn_log(logbuf);
}

int vpn_setup(const VpnConfig *cfg, char *out, int n) {
    char buf[8192];
    char args[1024];

    /* ════════════════════════════════════════════════════════════════════
       FASE 1/3 — Servico SoftEther VPN Client
       ════════════════════════════════════════════════════════════════════ */
    if (!vpn_is_installed()) {
        snprintf(out, n,
            "SoftEther VPN Client nao esta instalado.\n"
            "Usa o botao 'Instalar SE' primeiro.");
        vpn_log(out);
        return 0;
    }

    vpn_log("[1/3] Iniciando servico SoftEther VPN Client...");
    vpn_start_service();
    if (!wait_for_service(20)) {
        snprintf(out, n, "Servico VPN nao respondeu em 20s. Reinicia o Windows e tenta novamente.");
        vpn_log(out);
        return 0;
    }
    vpn_log("[1/3] Servico SoftEther OK.");

    /* ════════════════════════════════════════════════════════════════════
       FASE 2/3 — Placa de rede virtual (NIC)

       Logica:
         1. Consultar NicList — se ja existe alguma NIC, usa-la
         2. Se nao existe, criar com NicCreate VPN
         3. Em caso de Error 32 (nome bloqueado no driver Windows):
              a. Parar servico (liberta handles do driver TAP)
              b. Remover via tapinstall.exe + PowerShell Remove-PnpDevice
              c. Reiniciar servico e retentar
         4. Apos criar/encontrar a NIC, guardar o NOME REAL (do NicList)
            — este nome sera usado no AccountCreate, nao um nome hardcoded
       ════════════════════════════════════════════════════════════════════ */
    char nic_real[64] = {0};  /* nome real da NIC, lido do NicList */

    vpn_log("[2/3] Consultando NicList...");
    buf[0] = 0;
    run_vpncmd("localhost /CLIENT /CMD NicList", buf, sizeof(buf));
    vpn_log_output("[2/3] NicList:", buf);

    if (parse_first_nic_name(buf, nic_real, sizeof(nic_real))) {
        /* NIC ja existe — usar a que esta la */
        char info[128];
        snprintf(info, sizeof(info), "[2/3] Placa virtual existente: '%s'. Reutilizando.", nic_real);
        vpn_log(info);
    } else {
        /* Nao existe nenhuma NIC — criar uma nova */
        vpn_log("[2/3] Nenhuma placa virtual encontrada. Criando...");
        buf[0] = 0;
        run_vpncmd("localhost /CLIENT /CMD NicCreate " NIC_NAME, buf, sizeof(buf));
        vpn_log_output("[2/3] NicCreate:", buf);

        /* ── Error 32: o nome esta bloqueado no driver TAP do Windows mas
              SoftEther nao tem registo interno (estado orfao).
              ORDEM CRITICA: parar servico PRIMEIRO (liberta handles),
              depois remover dispositivo PnP, depois reiniciar.             */
        if (!cmd_ok(buf)) {
            if (strstr(buf, "Error code: 32") || strstr(buf, "(Error code: 32)")) {
                vpn_log("[2/3] Erro 32: nome TAP bloqueado. Parando servico...");
                sc_cmd("stop SevpnClient");
                Sleep(4000);

                /* tapinstall.exe (ferramenta propria do SoftEther) */
                {
                    char tapinst[MAX_PATH] = {0};
                    const char *vp = find_vpncmd();
                    if (vp) {
                        strncpy(tapinst, vp, MAX_PATH - 20);
                        char *sep = strrchr(tapinst, '\\');
                        if (sep) strcpy(sep + 1, "tapinstall.exe");
                    }
                    if (tapinst[0] &&
                        GetFileAttributesA(tapinst) != INVALID_FILE_ATTRIBUTES) {
                        vpn_log("[2/3] tapinstall: remove tap0901...");
                        char tc[MAX_PATH + 32];
                        snprintf(tc, sizeof(tc), "\"%s\" remove tap0901", tapinst);
                        char to[1024] = {0};
                        exec_capture(tc, to, sizeof(to));
                        vpn_log_output("[2/3] tapinstall:", to);
                        Sleep(2000);
                    }
                }

                /* PowerShell: Remove-PnpDevice (remove dispositivo do Device Manager) */
                vpn_log("[2/3] PowerShell: removendo dispositivo PnP...");
                char ps_del[2048];
                snprintf(ps_del, sizeof(ps_del),
                    "powershell.exe -NonInteractive -WindowStyle Hidden "
                    "-ExecutionPolicy Bypass -Command \""
                    "Get-NetAdapter -ErrorAction SilentlyContinue | "
                    "  Where-Object { $_.InterfaceDescription -like '*SoftEther*' } | "
                    "  Remove-NetAdapter -Confirm:$false -ErrorAction SilentlyContinue; "
                    "Get-PnpDevice -Class Net -ErrorAction SilentlyContinue | "
                    "  Where-Object { $_.FriendlyName -like '*SoftEther*' } | "
                    "  ForEach-Object { "
                    "    Remove-PnpDevice -InstanceId $_.InstanceId "
                    "      -Confirm:$false -ErrorAction SilentlyContinue }\"");
                exec_capture(ps_del, NULL, 0);
                Sleep(3000);

                vpn_log("[2/3] Reiniciando servico SoftEther...");
                sc_cmd("start SevpnClient");
                if (!wait_for_service(25)) {
                    snprintf(out, n,
                        "Servico VPN nao reiniciou apos limpeza TAP.\n"
                        "Reinicia o Windows e tenta novamente.");
                    vpn_log(out);
                    return 0;
                }

                vpn_log("[2/3] Recriando placa virtual (2a tentativa)...");
                buf[0] = 0;
                run_vpncmd("localhost /CLIENT /CMD NicCreate " NIC_NAME, buf, sizeof(buf));
                vpn_log_output("[2/3] NicCreate (2a):", buf);
            }

            /* Verificar NicList independentemente de qual caminho tomamos */
            {
                char chk[4096] = {0};
                run_vpncmd("localhost /CLIENT /CMD NicList", chk, sizeof(chk));
                if (!parse_first_nic_name(chk, nic_real, sizeof(nic_real))) {
                    snprintf(out, n,
                        "Falha ao criar placa virtual.\n"
                        "Confirma que o programa corre como Administrador.\n"
                        "Se persistir, reinicia o Windows.\n"
                        "Detalhe: %.400s", tail(buf, 400));
                    vpn_log(out);
                    return 0;
                }
            }
        }

        /* Ler NicList apos criacao para obter o nome real atribuido */
        if (!nic_real[0]) {
            char chk[4096] = {0};
            run_vpncmd("localhost /CLIENT /CMD NicList", chk, sizeof(chk));
            parse_first_nic_name(chk, nic_real, sizeof(nic_real));
        }
        if (!nic_real[0]) strncpy(nic_real, NIC_NAME, sizeof(nic_real) - 1);

        /* Aguardar servico estabilizar apos instalacao do driver TAP */
        vpn_log("[2/3] Aguardando servico estabilizar...");
        Sleep(2000);
        wait_for_service(20);

        {
            char en_args[128];
            snprintf(en_args, sizeof(en_args),
                "localhost /CLIENT /CMD NicEnable %s", nic_real);
            run_vpncmd(en_args, NULL, 0);
        }
    }

    {
        char info[128];
        snprintf(info, sizeof(info), "[2/3] Placa virtual pronta: '%s'", nic_real);
        vpn_log(info);
    }

    /* ════════════════════════════════════════════════════════════════════
       FASE 3/3 — Conta VPN (AccountCreate + AccountPasswordSet)
       USA o nome real da NIC obtido do NicList — nao um nome hardcoded.
       ════════════════════════════════════════════════════════════════════ */
    {
        char info[256];
        vpn_log("[3/3] Configurando conta VPN...");
        snprintf(info, sizeof(info), "[3/3]    Servidor  : %s:%u", cfg->host, cfg->port);
        vpn_log(info);
        snprintf(info, sizeof(info), "[3/3]    Hub       : %s", cfg->hub);
        vpn_log(info);
        snprintf(info, sizeof(info), "[3/3]    Utilizador: %s", cfg->username);
        vpn_log(info);
        snprintf(info, sizeof(info), "[3/3]    Conta VPN : '%s'  NIC: '%s'",
            cfg->account_name, nic_real);
        vpn_log(info);
    }

    /* AccountCreate com o NIC name real (nao hardcoded) */
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountCreate %s"
        " /SERVER:%s:%u /HUB:%s /USERNAME:%s /NICNAME:%s",
        cfg->account_name, cfg->host, cfg->port,
        cfg->hub, cfg->username, nic_real);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    vpn_log_output("[3/3] AccountCreate:", buf);

    /* Se conta ja existe: apagar e recriar com dados atualizados */
    if (!cmd_ok(buf)) {
        vpn_log("[3/3] Conta ja existe, recriando com novos dados...");
        char del_args[256];
        snprintf(del_args, sizeof(del_args),
            "localhost /CLIENT /CMD AccountDelete %s", cfg->account_name);
        run_vpncmd(del_args, NULL, 0);
        buf[0] = 0;
        run_vpncmd(args, buf, sizeof(buf));
        vpn_log_output("[3/3] AccountCreate (2a):", buf);
        if (!cmd_ok(buf)) {
            snprintf(out, n,
                "Falha ao criar conta '%s'.\nDetalhe: %.400s",
                cfg->account_name, tail(buf, 400));
            vpn_log(out);
            return 0;
        }
    }
    vpn_log("[3/3] Conta registada no SoftEther.");

    /* Set authentication method and password */
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountPasswordSet %s /PASSWORD:%s /TYPE:standard",
        cfg->account_name, cfg->password);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    vpn_log_output("[3/3] AccountPasswordSet:", buf);
    if (!cmd_ok(buf)) {
        snprintf(out, n, "Erro ao definir password.\nDetalhe: %.300s", tail(buf, 300));
        vpn_log(out);
        return 0;
    }
    vpn_log("[3/3] Autenticacao configurada (standard password).");

    vpn_log(">>> Configuracao concluida com sucesso! Clica em 'Ligar'.");
    snprintf(out, n, "Conta VPN '%s' configurada. Clica em Ligar.", cfg->account_name);
    return 1;
}

int vpn_connect(const VpnConfig *cfg, char *out, int n) {
    char buf[4096];
    char args[256];

    vpn_log("[Ligar] Verificando servico SoftEther...");
    sc_cmd("start SevpnClient");
    if (!wait_for_service(15)) {
        snprintf(out, n,
            "Servico VPN nao responde em 15s.\n"
            "Verifica se o SoftEther esta instalado.");
        vpn_log(out);
        return 0;
    }

    {
        char info[128];
        snprintf(info, sizeof(info),
            "[Ligar] AccountConnect %s -> %s:%u hub=%s user=%s",
            cfg->account_name, cfg->host, cfg->port, cfg->hub, cfg->username);
        vpn_log(info);
    }

    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountConnect %s", cfg->account_name);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    vpn_log_output("[Ligar] SoftEther:", buf);

    if (strstr(buf, "The command completed successfully")) {
        snprintf(out, n, "Ligacao iniciada. Aguardando atribuicao de IP...");
        vpn_log(out);
        return 1;
    }
    if (strstr(buf, "already") || strstr(buf, "Already")) {
        snprintf(out, n, "Ja esta ligado a '%s'.", cfg->account_name);
        vpn_log(out);
        return 1;
    }
    if (strstr(buf, "Cannot connect to VPN Client")) {
        snprintf(out, n, "Servico VPN nao responde. Tenta novamente.");
        vpn_log(out);
        return 0;
    }
    if (strstr(buf, "The specified account is not found") || strstr(buf, "not found")) {
        snprintf(out, n,
            "Conta '%s' nao encontrada.\n"
            "Usa 'Configurar VPN' -> 'Criar Conta VPN' primeiro.",
            cfg->account_name);
        vpn_log(out);
        return 0;
    }
    /* Any other error: show the raw vpncmd message */
    if (buf[0]) {
        snprintf(out, n, "Erro ao ligar: %.300s", tail(buf, 300));
        vpn_log(out);
        return 0;
    }
    snprintf(out, n, "Sem resposta do vpncmd. Verifica o servico SoftEther.");
    vpn_log(out);
    return 0;
}

int vpn_disconnect(const VpnConfig *cfg, char *out, int n) {
    char buf[4096], args[256];
    vpn_log("[Desligar] Enviando AccountDisconnect...");
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountDisconnect %s", cfg->account_name);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    vpn_log_output("[Desligar] SoftEther:", buf);
    snprintf(out, n, "Conta '%s' desligada.", cfg->account_name);
    return 1;
}

/* ─── Get VPN adapter IP via PowerShell ─────────────── */
/* nic_name: nome real do adaptador SoftEther (ex: "VPN").
   O alias Windows é sempre "[nic_name] - VPN Client".    */
static void get_vpn_ip(const char *nic_name, char *ip_out, int ip_len) {
    if (!nic_name || !nic_name[0]) nic_name = NIC_NAME;

    /* "VPN" -> "VPN - VPN Client" (formato fixo do SoftEther) */
    char alias[128] = {0};
    snprintf(alias, sizeof(alias), "%s - VPN Client", nic_name);

    char script[2048];
    snprintf(script, sizeof(script),
        "powershell.exe -NonInteractive -WindowStyle Hidden -Command \""
        "$aliases = @('%s', '%s'); "
        "foreach ($n in $aliases) { "
        "  $ip = Get-NetIPAddress -InterfaceAlias $n -AddressFamily IPv4 "
        "        -ErrorAction SilentlyContinue | "
        "        Where-Object { $_.IPAddress -notlike '169.254.*' } | "
        "        Select-Object -ExpandProperty IPAddress -First 1; "
        "  if ($ip) { Write-Output $ip; exit } } "
        "Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | "
        "  Where-Object { $_.InterfaceDescription -like '*VPN Client Adapter*' -or "
        "                 $_.InterfaceDescription -like '*SoftEther*' } | "
        "  ForEach-Object { "
        "    $ip = Get-NetIPAddress -InterfaceIndex $_.ifIndex -AddressFamily IPv4 "
        "               -ErrorAction SilentlyContinue | "
        "               Where-Object { $_.IPAddress -notlike '169.254.*' } | "
        "               Select-Object -ExpandProperty IPAddress -First 1; "
        "    if ($ip) { Write-Output $ip; exit } }\"",
        alias, nic_name);

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

/* ─── Public wrapper for VPN adapter IP ───────────────── */
void vpn_get_nic_ip(char *ip_out, int ip_len) {
    if (!ip_out || ip_len < 1) return;
    ip_out[0] = '\0';
    get_vpn_ip(NIC_NAME, ip_out, ip_len);
    if (ip_out[0] == '\0')
        strncpy(ip_out, "Placa sem IP", ip_len - 1);
}

void vpn_get_status(const VpnConfig *cfg, VpnStatus *s) {
    memset(s, 0, sizeof(*s));

    if (!vpn_is_installed()) {
        s->state = VPN_NOT_INSTALLED;
        strncpy(s->message, "A instalar componentes VPN...", sizeof(s->message) - 1);
        return;
    }
    s->softether_ready = 1;

    /* ═══════════════════════════════════════════════════════════════════
       PASSO 1: NicList — feito PRIMEIRO para obter nic_name real.
       Necessário antes de get_vpn_ip para poder construir alias correcto.
       ═══════════════════════════════════════════════════════════════════ */
    {
        char nl[4096] = {0};
        run_vpncmd("localhost /CLIENT /CMD NicList", nl, sizeof(nl));
        parse_first_nic_name(nl, s->nic_name,   sizeof(s->nic_name));
        parse_nic_mac       (nl, s->nic_mac,    sizeof(s->nic_mac));
        parse_niclist_field (nl, "Status", s->nic_status, sizeof(s->nic_status));
        /* Nome Windows: SoftEther cria sempre "[nic_name] - VPN Client" */
        const char *nn = s->nic_name[0] ? s->nic_name : NIC_NAME;
        snprintf(s->nic_windows, sizeof(s->nic_windows), "%s - VPN Client", nn);
    }

    /* ═══════════════════════════════════════════════════════════════════
       PASSO 2: AccountStatusGet — estado detalhado da sessão
       Campos confirmados v4.41:
         "Session Status" -> "Connection Completed (Session Established)"
                          -> "Offline"
                          -> "Connecting..."
         "Server Name"    -> "10.201.114.222"
         "Port Number"    -> "TCP Port 443"
       ═══════════════════════════════════════════════════════════════════ */
    char args[256];
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountStatusGet %s", cfg->account_name);
    char out[4096] = {0};
    run_vpncmd(args, out, sizeof(out));

    /* Diagnostics: guardar parte útil do output */
    {
        const char *start = strstr(out, "Session Status");
        if (!start) start = strstr(out, "AccountStatusGet");
        if (!start) { size_t len = strlen(out); start = out+(len>500?len-500:0); }
        strncpy(s->raw_status, start, sizeof(s->raw_status) - 1);
    }

    /* Serviço não responde */
    if (strstr(out, "Cannot connect to VPN Client")) {
        s->state = VPN_DISCONNECTED;
        strncpy(s->message, "Servico VPN nao responde", sizeof(s->message) - 1);
        return;
    }

    /* Conta não existe → não configurada */
    char lower_out[4096]; int i;
    for (i=0; out[i]&&i<(int)sizeof(lower_out)-1; i++)
        lower_out[i]=(char)tolower((unsigned char)out[i]);
    lower_out[i]=0;

    int account_ok = !strstr(lower_out, "not found") && out[0] != 0;
    s->connection_ready = account_ok;
    if (!account_ok) {
        s->state = VPN_NOT_CONFIGURED;
        strncpy(s->message, "Conta VPN nao configurada", sizeof(s->message) - 1);
        return;
    }

    /* Parsear campo "Session Status" — ÚNICA fonte fiável de estado.
       BUG HISTÓRICO: busca broad em todo o output dava falso positivo
       para "Offline" (contém "session status" mas não "not connected"). */
    char sess_status[128] = {0};
    parse_niclist_field(out, "Session Status", sess_status, sizeof(sess_status));
    char sess_lower[128] = {0};
    for (i=0; sess_status[i]&&i<127; i++)
        sess_lower[i]=(char)tolower((unsigned char)sess_status[i]);

    /* Classificação exacta com base no valor do campo Session Status */
    int session_up      = strstr(sess_lower, "established")       != NULL ||
                          strstr(sess_lower, "connection completed") != NULL;
    int session_connect = strstr(sess_lower, "connecting")         != NULL;

    /* Parse campos de display do AccountStatusGet */
    {
        char host[256]={0}, port_raw[64]={0};
        strncpy(s->status_detail, sess_status, sizeof(s->status_detail)-1);
        parse_niclist_field(out, "Server Name", host, sizeof(host));
        parse_niclist_field(out, "Port Number", port_raw, sizeof(port_raw));
        /* Extrair número de "TCP Port 443" */
        char port_num[16]={0};
        { const char *pp=port_raw; while(*pp&&!(*pp>='0'&&*pp<='9'))pp++;
          int j=0; while(*pp>='0'&&*pp<='9'&&j<15)port_num[j++]=*pp++; port_num[j]=0; }
        if (host[0]&&port_num[0])
            snprintf(s->server_display,sizeof(s->server_display),"%s:%s",host,port_num);
        else if (host[0])
            strncpy(s->server_display,host,sizeof(s->server_display)-1);
    }

    /* ═══════════════════════════════════════════════════════════════════
       PASSO 3: AccountList — campos de exibição na UI
       Campos confirmados v4.41:
         "Status"                       -> "Connected" / "Offline" / "Connecting..."
         "VPN Server Hostname"          -> "10.201.114.222:443 (Direct TCP/IP...)"
         "Virtual Hub"                  -> "DEFAULT"
         "Virtual Network Adapter Name" -> "VPN"
       ═══════════════════════════════════════════════════════════════════ */
    {
        char al[4096] = {0};
        run_vpncmd("localhost /CLIENT /CMD AccountList", al, sizeof(al));

        /* "Status" do AccountList — mais simples que Session Status */
        char al_status[64] = {0};
        parse_niclist_field(al, "Status", al_status, sizeof(al_status));
        if (al_status[0]) {
            /* Guardar para exibição na coluna Estado da ListView */
            strncpy(s->vpn_user, al_status, sizeof(s->vpn_user)-1);
            if (!s->status_detail[0])
                strncpy(s->status_detail, al_status, sizeof(s->status_detail)-1);
            /* Se AccountStatusGet não deu Session Status, usar AccountList Status */
            if (!sess_status[0]) {
                char al_lower[64]={0};
                for (int j=0;al_status[j]&&j<63;j++)
                    al_lower[j]=(char)tolower((unsigned char)al_status[j]);
                /* "Connected" → ligado; "Connecting" → a ligar */
                if (strstr(al_lower,"connected")&&!strstr(al_lower,"not"))
                    session_up=1;
                else if (strstr(al_lower,"connect"))
                    session_connect=1;
            }
        }

        /* "VPN Server Hostname" — preferir AccountList (inclui porta) */
        char al_srv[272]={0};
        parse_niclist_field(al, "VPN Server Hostname", al_srv, sizeof(al_srv));
        if (al_srv[0]) {
            char *paren=strstr(al_srv," ("); if(paren)*paren=0;
            strncpy(s->server_display, al_srv, sizeof(s->server_display)-1);
        }

        /* "Virtual Hub" */
        parse_niclist_field(al, "Virtual Hub", s->hub_display, sizeof(s->hub_display));

        /* "Virtual Network Adapter Name" — confirmar/completar nic_name do NicList */
        if (!s->nic_name[0])
            parse_niclist_field(al, "Virtual Network Adapter Name", s->nic_name, sizeof(s->nic_name));
    }

    /* Fallbacks para campos obrigatorios */
    if (!s->server_display[0])
        snprintf(s->server_display, sizeof(s->server_display), "%s:%u", cfg->host, cfg->port);
    if (!s->hub_display[0])
        strncpy(s->hub_display, cfg->hub[0] ? cfg->hub : DEFAULT_HUB, sizeof(s->hub_display)-1);

    /* ═══════════════════════════════════════════════════════════════════
       PASSO 4: IP do adaptador — usa nic_name REAL obtido no Passo 1
       Quando desligado o adaptador fica hidden → IP vazio (normal).
       ═══════════════════════════════════════════════════════════════════ */
    get_vpn_ip(s->nic_name[0] ? s->nic_name : NIC_NAME, s->local_ip, sizeof(s->local_ip));
    int has_ip = s->local_ip[0] != 0;

    /* ═══════════════════════════════════════════════════════════════════
       ESTADO FINAL: baseado em Session Status (campo explícito) + IP
       ═══════════════════════════════════════════════════════════════════ */
    s->connected = session_up || has_ip;
    if (s->connected) {
        s->state = VPN_CONNECTED;
        strncpy(s->message, "Ligado a rede RLS Automacao", sizeof(s->message) - 1);
        if (!has_ip)
            strncpy(s->local_ip, "Placa sem IP", sizeof(s->local_ip) - 1);
    } else if (session_connect || strstr(lower_out, "connecting")) {
        s->state = VPN_CONNECTING;
        strncpy(s->message, "A estabelecer ligacao...", sizeof(s->message) - 1);
        strncpy(s->local_ip, "Aguardar...", sizeof(s->local_ip) - 1);
    } else {
        s->state = VPN_DISCONNECTED;
        strncpy(s->message, "Pronto para ligar", sizeof(s->message) - 1);
        s->local_ip[0] = '\0';
    }
}

/* ─── Find VPN virtual adapter name via PowerShell ── */
/* Uses the same strategy as get_vpn_ip() - reliable across all Windows versions */
static int find_vpn_adapter(char *name_out, int name_len) {
    /* ── Step 1: ask SoftEther vpncmd for the exact NIC name ──────────
       This is the most reliable source — it's the name we created via
       NicCreate, e.g. "VPN". Fallback to NIC_NAME if vpncmd is down.   */
    char nic_real[128] = {0};
    {
        char nl[4096] = {0};
        run_vpncmd("localhost /CLIENT /CMD NicList", nl, sizeof(nl));
        parse_first_nic_name(nl, nic_real, sizeof(nic_real));
    }
    if (nic_real[0] == '\0')
        strncpy(nic_real, NIC_NAME, sizeof(nic_real) - 1);

    /* ── Step 2: match ONLY SoftEther-specific adapter names ───────────
       SoftEther always creates:
         InterfaceAlias       = "{nic_name} - VPN Client"   (exact)
         InterfaceDescription = "VPN Client Adapter - {nic_name}"
       We deliberately avoid "*TAP-Windows*", "*VPN*" and other generic
       patterns because OpenVPN / WireGuard / other VPN software also
       creates adapters that match those broad terms.                    */
    char ps[1024];
    snprintf(ps, sizeof(ps),
        "powershell.exe -NonInteractive -WindowStyle Hidden -Command \""
        "$n = '%s'; "
        "$a = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { "
        "  $_.InterfaceAlias       -eq ($n + ' - VPN Client') -or "
        "  $_.InterfaceDescription -like ('VPN Client Adapter - ' + $n + '*') "
        "} | Select-Object -First 1; "
        "if ($a) { Write-Output $a.InterfaceAlias } "
        "else    { Write-Output 'NOTFOUND' }\"",
        nic_real);

    char buf[512] = {0};
    exec_capture(ps, buf, sizeof(buf));

    /* Trim whitespace / CRLF */
    char *p = buf;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1]==' '||e[-1]=='\r'||e[-1]=='\n'||e[-1]=='\t')) *--e = 0;

    if (*p && strcmp(p, "NOTFOUND") != 0) {
        strncpy(name_out, p, name_len - 1);
        return 1;
    }
    return 0;
}

/* ─── Apply static IP to VPN virtual adapter ─────── */
int vpn_set_static_ip(const char *ip, const char *mask, char *out, int n) {
    if (!ip || !ip[0]) {
        snprintf(out, n, "Erro: campo IP vazio.");
        return 0;
    }

    const char *m = (mask && mask[0]) ? mask : "255.255.255.0";

    /* Step 1: find adapter */
    char adapter[256] = {0};
    if (!find_vpn_adapter(adapter, sizeof(adapter))) {
        snprintf(out, n,
            "Placa VPN nao encontrada. Verifique 'Alterar adaptadores de rede' "
            "no Windows e confirme que a VPN esta ligada.");
        return 0;
    }

    /* Step 2: apply via netsh
       Correct syntax: netsh interface ip set address "name" static IP MASK
       netsh returns EMPTY stdout on success, error text on failure. */
    char cmd[1024];
    char result[2048] = {0};
    snprintf(cmd, sizeof(cmd),
        "netsh interface ip set address name=\"%s\" static %s %s",
        adapter, ip, m);
    exec_capture(cmd, result, sizeof(result));

    /* Trim result */
    char *r = result;
    while (*r == ' ' || *r == '\r' || *r == '\n' || *r == '\t') r++;
    char *re = r + strlen(r);
    while (re > r && (re[-1]==' '||re[-1]=='\r'||re[-1]=='\n'||re[-1]=='\t')) *--re=0;

    /* SUCCESS = empty output (netsh convention) */
    if (*r == '\0') {
        snprintf(out, n, "IP %s / %s aplicado na placa \"%s\".", ip, m, adapter);
        return 1;
    }

    /* FAILURE = any non-empty output */
    snprintf(out, n, "Falha ao aplicar IP na \"%s\": %s", adapter, r);
    return 0;
}

int vpn_reset(const VpnConfig *cfg, char *out, int n) {
    char buf[4096], args[256];

    vpn_log("[Reset] Desligando sessao VPN...");
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountDisconnect %s", cfg->account_name);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    vpn_log_output("[Reset] AccountDisconnect:", buf);

    vpn_log("[Reset] Removendo conta VPN...");
    snprintf(args, sizeof(args),
        "localhost /CLIENT /CMD AccountDelete %s", cfg->account_name);
    buf[0] = 0;
    run_vpncmd(args, buf, sizeof(buf));
    vpn_log_output("[Reset] AccountDelete:", buf);

    vpn_log("[Reset] Removendo placa virtual " NIC_NAME "...");
    buf[0] = 0;
    run_vpncmd("localhost /CLIENT /CMD NicDelete " NIC_NAME, buf, sizeof(buf));
    vpn_log_output("[Reset] NicDelete:", buf);

    snprintf(out, n, "Reset concluido. Conta e placa virtual removidas.");
    return 1;
}
