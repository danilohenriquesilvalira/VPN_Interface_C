/* profiles.c - RLS Automacao VPN - Multi-profile persistence (JSON minimal)
 *
 * JSON gravado manualmente (sem dependencias externas).
 * Formato:
 * {
 *   "active_idx": -1,
 *   "profiles": [
 *     { "name":"...", "host":"...", "port":443,
 *       "hub":"...", "username":"...", "password":"...",
 *       "account_name":"..." },
 *     ...
 *   ]
 * }
 *
 * SEGURANCA: a password e guardada em texto simples no perfil JSON local.
 * O ficheiro fica em %APPDATA%\RLS_Automacao\ que e privado ao utilizador.
 * Nao enviar o ficheiro pela rede nem o incluir em repositorios.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "profiles.h"
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ─── Internal helpers ─────────────────────────────────────────────── */

/* Escape uma string para JSON: substitui \  "  e caracteres de controlo */
static void json_escape(const char *src, char *dst, int dst_len) {
    int d = 0;
    for (int i = 0; src[i] && d < dst_len - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (d < dst_len - 3) { dst[d++] = '\\'; dst[d++] = (char)c; }
        } else if (c < 0x20) {
            /* substituir controlo por espaco */
            if (d < dst_len - 2) dst[d++] = ' ';
        } else {
            dst[d++] = (char)c;
        }
    }
    dst[d] = '\0';
}

/* Procura a primeira ocorrencia de "key": "value" ou "key": number
   no buffer JSON e copia o valor para out (max out_len-1).
   Retorna 1 se encontrou, 0 se nao.                                  */
static int json_get_str(const char *buf, const char *key,
                         char *out, int out_len) {
    if (!buf || !key || !out || out_len < 1) return 0;
    out[0] = '\0';

    /* Procura  "key"  */
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return 0;
    p += strlen(needle);
    /* skip espacos e ':' */
    while (*p == ' ' || *p == '\t' || *p == ':') p++;

    if (*p == '"') {
        /* valor string */
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_len - 1) {
            if (*p == '\\' && *(p+1)) {
                p++;
                switch (*p) {
                    case '"': case '\\': case '/': out[i++] = *p; break;
                    case 'n': out[i++] = '\n'; break;
                    case 'r': out[i++] = '\r'; break;
                    case 't': out[i++] = '\t'; break;
                    default:  out[i++] = *p;   break;
                }
            } else {
                out[i++] = *p;
            }
            p++;
        }
        out[i] = '\0';
        return 1;
    } else if (*p == '-' || isdigit((unsigned char)*p)) {
        /* valor numerico */
        int i = 0;
        while ((*p == '-' || isdigit((unsigned char)*p)) && i < out_len - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return 1;
    }
    return 0;
}

/* Avanca o ponteiro para o proximo objecto '{' dentro do array "profiles" */
static const char *next_profile_obj(const char *p) {
    if (!p) return NULL;
    /* procura { mas ignora o { raiz do documento (ja passamos dele) */
    while (*p && *p != '{') p++;
    if (!*p) return NULL;
    return p;  /* aponta para '{' do objecto perfil */
}

/* Avanca para alem do objecto em p (encontra o '}' do nivel certo) */
static const char *skip_object(const char *p) {
    if (!p || *p != '{') return p;
    int depth = 0;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) return p + 1; }
        else if (*p == '"') { p++; while (*p && (*p != '"' || *(p-1) == '\\')) p++; }
        p++;
    }
    return p;
}

/* ─── Path helper ───────────────────────────────────────────────────── */
void profiles_get_path(char *path_out, int path_len) {
    char base[MAX_PATH] = {0};
    /* SHGetFolderPathA: %APPDATA% */
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, base))) {
        snprintf(path_out, path_len, "%s\\RLS_Automacao\\vpn_profiles.json", base);
    } else {
        /* fallback: mesmo directorio do .exe */
        GetModuleFileNameA(NULL, base, MAX_PATH);
        char *sl = strrchr(base, '\\');
        if (sl) { *(sl+1) = '\0'; }
        snprintf(path_out, path_len, "%svpn_profiles.json", base);
    }
}

/* ─── Load ──────────────────────────────────────────────────────────── */
static int profiles_load(VpnProfileList *pl) {
    char path[MAX_PATH];
    profiles_get_path(path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    /* Ler ficheiro inteiro */
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 1024 * 1024) { fclose(f); return 0; }

    char *buf = (char*)malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, (size_t)fsz, f);
    buf[fsz] = '\0';
    fclose(f);

    /* active_idx */
    char tmp[32];
    if (json_get_str(buf, "active_idx", tmp, sizeof(tmp)))
        pl->active_idx = atoi(tmp);

    /* array "profiles" */
    const char *arr = strstr(buf, "\"profiles\"");
    if (!arr) { free(buf); return 0; }
    arr = strchr(arr, '[');
    if (!arr) { free(buf); return 0; }
    arr++;  /* passa o '[' */

    pl->count = 0;
    const char *p = arr;
    while (pl->count < PROFILES_MAX) {
        /* avanca ate proximo '{' ou ']' */
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;

        /* encontra fim do objecto */
        const char *obj_end = skip_object(p);
        /* copia o objecto para buffer local para parse isolado */
        int obj_len = (int)(obj_end - p);
        if (obj_len <= 0 || obj_len > 4096) { p = obj_end; continue; }
        char obj[4096];
        int copy_len = obj_len < (int)sizeof(obj) - 1 ? obj_len : (int)sizeof(obj) - 1;
        memcpy(obj, p, copy_len);
        obj[copy_len] = '\0';

        VpnProfile *pr = &pl->items[pl->count];
        memset(pr, 0, sizeof(*pr));

        json_get_str(obj, "name",         pr->name,         sizeof(pr->name));
        json_get_str(obj, "host",         pr->host,         sizeof(pr->host));
        json_get_str(obj, "hub",          pr->hub,          sizeof(pr->hub));
        json_get_str(obj, "username",     pr->username,     sizeof(pr->username));
        json_get_str(obj, "password",     pr->password,     sizeof(pr->password));
        json_get_str(obj, "account_name", pr->account_name, sizeof(pr->account_name));
        if (json_get_str(obj, "port", tmp, sizeof(tmp)))
            pr->port = (unsigned short)atoi(tmp);

        /* valida campo minimo */
        if (pr->host[0] && pr->port > 0)
            pl->count++;

        p = obj_end;
    }

    free(buf);
    return (pl->count > 0);
}

/* ─── Public API ────────────────────────────────────────────────────── */

void profiles_init(VpnProfileList *pl) {
    memset(pl, 0, sizeof(*pl));
    pl->active_idx = -1;

    if (profiles_load(pl) && pl->count > 0)
        return;  /* carregado com sucesso */

    /* Primeira execucao: criar perfil default */
    VpnProfile def = {0};
    strncpy(def.name,         "RLS Automacao (default)", sizeof(def.name) - 1);
    strncpy(def.host,         DEFAULT_HOST,   sizeof(def.host) - 1);
    def.port = DEFAULT_PORT;
    strncpy(def.hub,          DEFAULT_HUB,    sizeof(def.hub) - 1);
    strncpy(def.username,     DEFAULT_USER,   sizeof(def.username) - 1);
    strncpy(def.password,     DEFAULT_PASS,   sizeof(def.password) - 1);
    strncpy(def.account_name, ACCOUNT_NAME,   sizeof(def.account_name) - 1);
    profiles_add(pl, &def);
    profiles_save(pl);
}

int profiles_save(const VpnProfileList *pl) {
    char path[MAX_PATH];
    profiles_get_path(path, sizeof(path));

    /* Garantir que o directorio existe */
    char dir[MAX_PATH];
    strncpy(dir, path, sizeof(dir) - 1);
    char *sl = strrchr(dir, '\\');
    if (sl) { *sl = '\0'; CreateDirectoryA(dir, NULL); }

    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    fprintf(f, "{\n  \"active_idx\": %d,\n  \"profiles\": [\n", pl->active_idx);

    for (int i = 0; i < pl->count; i++) {
        const VpnProfile *p = &pl->items[i];
        char ename[64], ehost[256], ehub[64], euser[64], epass[128], eacct[64];
        json_escape(p->name,         ename, sizeof(ename));
        json_escape(p->host,         ehost, sizeof(ehost));
        json_escape(p->hub,          ehub,  sizeof(ehub));
        json_escape(p->username,     euser, sizeof(euser));
        json_escape(p->password,     epass, sizeof(epass));
        json_escape(p->account_name, eacct, sizeof(eacct));
        fprintf(f,
            "    {\n"
            "      \"name\": \"%s\",\n"
            "      \"host\": \"%s\",\n"
            "      \"port\": %u,\n"
            "      \"hub\": \"%s\",\n"
            "      \"username\": \"%s\",\n"
            "      \"password\": \"%s\",\n"
            "      \"account_name\": \"%s\"\n"
            "    }%s\n",
            ename, ehost, (unsigned)p->port,
            ehub, euser, epass, eacct,
            (i < pl->count - 1) ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 1;
}

int profiles_add(VpnProfileList *pl, const VpnProfile *p) {
    if (pl->count >= PROFILES_MAX) return -1;
    pl->items[pl->count] = *p;
    /* garantir account_name preenchido */
    if (!pl->items[pl->count].account_name[0]) {
        /* gerar nome unico: "RLS_" + primeiros 8 chars do name */
        char base[16] = {0};
        int j = 0;
        for (int i = 0; p->name[i] && j < 8; i++) {
            char c = p->name[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')) {
                base[j++] = c;
            }
        }
        if (!base[0]) snprintf(base, sizeof(base), "P%d", pl->count);
        snprintf(pl->items[pl->count].account_name,
                 sizeof(pl->items[pl->count].account_name),
                 "RLS_%s", base);
    }
    return pl->count++;
}

int profiles_remove(VpnProfileList *pl, int idx) {
    if (idx < 0 || idx >= pl->count) return 0;
    for (int i = idx; i < pl->count - 1; i++)
        pl->items[i] = pl->items[i + 1];
    pl->count--;
    if (pl->active_idx == idx)      pl->active_idx = -1;
    else if (pl->active_idx > idx)  pl->active_idx--;
    return 1;
}

void profiles_to_cfg(const VpnProfile *p, VpnConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host,         p->host,         sizeof(cfg->host) - 1);
    cfg->port = p->port ? p->port : DEFAULT_PORT;
    strncpy(cfg->hub,          p->hub,          sizeof(cfg->hub) - 1);
    strncpy(cfg->username,     p->username,     sizeof(cfg->username) - 1);
    strncpy(cfg->password,     p->password,     sizeof(cfg->password) - 1);
    strncpy(cfg->account_name, p->account_name, sizeof(cfg->account_name) - 1);
}

void profiles_from_cfg(VpnProfile *p, const VpnConfig *cfg,
                        const char *friendly_name) {
    strncpy(p->host,         cfg->host,         sizeof(p->host) - 1);
    p->port = cfg->port;
    strncpy(p->hub,          cfg->hub,          sizeof(p->hub) - 1);
    strncpy(p->username,     cfg->username,     sizeof(p->username) - 1);
    strncpy(p->password,     cfg->password,     sizeof(p->password) - 1);
    strncpy(p->account_name, cfg->account_name, sizeof(p->account_name) - 1);
    if (friendly_name && friendly_name[0])
        strncpy(p->name,     friendly_name,     sizeof(p->name) - 1);
}
