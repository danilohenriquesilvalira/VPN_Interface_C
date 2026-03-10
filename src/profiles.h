/* profiles.h - RLS Automacao VPN - Multi-profile manager
 *
 * Cada perfil guarda apenas os dados que variam entre ligacoes:
 *   nome, host, port, hub, username, password, account_name.
 *
 * A placa virtual (NIC "VPN") e UNICA e partilhada por todos os perfis.
 * Antes de ligar um perfil diferente o chamador DEVE desligar o activo.
 *
 * Persistencia: %APPDATA%\RLS_Automacao\vpn_profiles.json
 *   (criado automaticamente na primeira gravacao)
 */
#ifndef PROFILES_H
#define PROFILES_H

#include "vpn.h"   /* VpnConfig */

#define PROFILES_MAX  32

/* ─── Profile record ────────────────────────────────────────────────── */
typedef struct {
    char     name[64];          /* nome amigavel, ex: "Matriz SP"      */
    char     host[256];
    unsigned short port;
    char     hub[64];
    char     username[64];
    char     password[128];
    char     account_name[64];  /* nome da conta no SoftEther          */
} VpnProfile;

/* ─── Profile list ──────────────────────────────────────────────────── */
typedef struct {
    VpnProfile items[PROFILES_MAX];
    int        count;
    int        active_idx;   /* indice do perfil actualmente ligado, -1 = nenhum */
} VpnProfileList;

/* ─── API ────────────────────────────────────────────────────────────── */

/* Inicializa lista vazia e tenta carregar do ficheiro JSON.
   Se o ficheiro nao existe cria um perfil default.              */
void profiles_init(VpnProfileList *pl);

/* Guardar lista no ficheiro JSON.  Retorna 1 ok / 0 erro.      */
int  profiles_save(const VpnProfileList *pl);

/* Adicionar perfil. Retorna indice ou -1 se lista cheia.       */
int  profiles_add(VpnProfileList *pl, const VpnProfile *p);

/* Remover perfil pelo indice. Retorna 1 ok / 0 erro.           */
int  profiles_remove(VpnProfileList *pl, int idx);

/* Preencher VpnConfig a partir de um perfil.                   */
void profiles_to_cfg(const VpnProfile *p, VpnConfig *cfg);

/* Preencher VpnProfile a partir de um VpnConfig + nome amigavel. */
void profiles_from_cfg(VpnProfile *p, const VpnConfig *cfg,
                        const char *friendly_name);

/* Caminho do ficheiro JSON (buffer de pelo menos MAX_PATH bytes). */
void profiles_get_path(char *path_out, int path_len);

#endif /* PROFILES_H */
