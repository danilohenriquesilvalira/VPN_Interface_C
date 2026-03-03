/* vpn.h - RLS Automacao VPN - Core VPN types and API */
#ifndef VPN_H
#define VPN_H

#include <windows.h>

/* ─── Default configuration ──────────────────────── */
#define NIC_NAME        "RLS_Automacao"
#define SERVICE_NAME    "SevpnClient"
#define ACCOUNT_NAME    "RLS_Automacao"
#define DEFAULT_HOST    "10.201.114.222"
#define DEFAULT_PORT    443
#define DEFAULT_HUB     "DEFAULT"
#define DEFAULT_USER    "luiz"
#define DEFAULT_PASS    "luiz1234"

/* ─── Types ──────────────────────────────────────── */
typedef enum {
    VPN_NOT_INSTALLED = 0,
    VPN_NOT_CONFIGURED,
    VPN_DISCONNECTED,
    VPN_CONNECTING,
    VPN_CONNECTED
} VpnState;

typedef struct {
    char           host[256];
    unsigned short port;
    char           hub[64];
    char           username[64];
    char           password[128];
    char           account_name[64];
} VpnConfig;

typedef struct {
    int      connected;
    int      softether_ready;
    int      connection_ready;
    VpnState state;
    char     local_ip[64];
    char     message[512];
    char     raw_status[1024];
} VpnStatus;

/* ─── Public API ─────────────────────────────────── */
void vpn_default_config(VpnConfig *cfg);
int  vpn_is_installed(void);
int  vpn_install_silent(const char *installer_path);
int  vpn_start_service(void);
int  vpn_setup(const VpnConfig *cfg, char *out, int n);
int  vpn_connect(const VpnConfig *cfg, char *out, int n);
int  vpn_disconnect(const VpnConfig *cfg, char *out, int n);
void vpn_get_status(const VpnConfig *cfg, VpnStatus *s);
int  vpn_reset(const VpnConfig *cfg, char *out, int n);

/* Apply a static IP to the VPN virtual adapter via netsh.
   Searches for the VPN adapter automatically.
   Returns 1 on success, 0 on error (with message in out). */
int  vpn_set_static_ip(const char *ip, const char *mask, char *out, int n);

#endif /* VPN_H */
