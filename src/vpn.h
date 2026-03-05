/* vpn.h - RLS Automacao VPN - Core VPN types and API */
#ifndef VPN_H
#define VPN_H

#include <windows.h>

/* ─── Default configuration ──────────────────────── */
#define NIC_NAME        "VPN"     /* nome padrao para criar nova NIC */
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
    /* Informacao real da placa virtual (populada a cada poll) */
    char     nic_name[64];      /* nome SoftEther, ex: "VPN"              */
    char     nic_windows[256];  /* nome Windows, ex: "VPN - VPN Client"   */
    char     nic_mac[32];       /* MAC address, ex: "00-AC-BB-CC-DD-EE"   */
    char     nic_status[32];    /* estado adaptador: "Enabled"/"Disabled"  */
    /* Informacao real da ligacao (parsed do AccountStatusGet) */
    char     server_display[272]; /* "host:port" real do SoftEther        */
    char     hub_display[64];     /* hub real do SoftEther                */
    char     status_detail[128];  /* status string bruto do SoftEther     */
    char     vpn_user[64];        /* utilizador confirmado pelo SoftEther  */
} VpnStatus;

/* ─── Log callback ──────────────────────────────── */
/* Register a function to receive real-time log messages from background
   operations (setup, connect, disconnect).  Called on worker threads. */
typedef void (*VpnLogFn)(const char *msg, void *ctx);
void vpn_set_log_fn(VpnLogFn fn, void *ctx);

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

/* Returns the current IPv4 address of the VPN virtual adapter.
   Writes "Placa sem IP" when the adapter has no valid address.
   ip_out must be at least 64 bytes. */
void vpn_get_nic_ip(char *ip_out, int ip_len);

/* Apply a static IP to the VPN virtual adapter via netsh.
   Searches for the VPN adapter automatically.
   Returns 1 on success, 0 on error (with message in out). */
int  vpn_set_static_ip(const char *ip, const char *mask, char *out, int n);

#endif /* VPN_H */
