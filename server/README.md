# Server — Raspberry Pi Gateway VPN

Raspberry Pi configurado como **router/gateway** para acesso remoto à rede local industrial (PLCs Siemens) via **ZeroTier + SoftEther VPN**.

## Arquitectura

```
[ PC Remoto ]
      │
      │  ZeroTier (túnel encriptado P2P)
      ▼
[ Raspberry Pi — Gateway ]
  ZeroTier IP : 10.201.114.222  (interface: zt3hho244n)
  Ethernet    : 192.168.222.253 (interface: eth0)
  Internet    : WiFi wlan0
      │
      │  NAT (Layer 3) + Bridge (Layer 2)
      ▼
[ Rede Local 192.168.222.0/24 ]
  PLCs Siemens, Switches, etc.
```

## Instalação num Pi novo

```bash
sudo bash server/install.sh
```

O script instala e configura tudo automaticamente. No final, todos os serviços arrancam sozinhos a cada boot — sem intervenção humana.

## Serviços e arranque automático

| Serviço | Descrição | Auto-start |
|---------|-----------|-----------|
| `zerotier-one` | Túnel P2P encriptado | ✓ systemd |
| `vpnserver` | SoftEther bridge Layer 2 | ✓ systemd |
| `zerotier-watchdog.timer` | Reinicia ZeroTier se OFFLINE | ✓ systemd timer |
| `netfilter-persistent` | Regras iptables NAT | ✓ systemd |

### Ordem de arranque

```
boot
 └─ network-online.target
     └─ zerotier-one  (+10s delay — WiFi precisa de tempo)
         └─ vpnserver (SoftEther)
 └─ netfilter-persistent (iptables NAT)
 └─ zerotier-watchdog.timer (verifica a cada 2min)
```

## Modos de Acesso

### Layer 3 — Acesso IP (via ZeroTier)
Para ping, SSH, SCADA, OPC-UA, etc.

**Windows:**
```
route add 192.168.222.0 mask 255.255.255.0 10.201.114.222
```
**Linux/Mac:**
```bash
sudo ip route add 192.168.222.0/24 via 10.201.114.222
```

### Layer 2 — Acesso Ethernet real (via SoftEther)
Para ferramentas que exigem Layer 2: **Siemens PRONETA**, DCP/LLDP.

| Campo | Valor |
|-------|-------|
| Host | 10.201.114.222 |
| Porta | 443 |
| Hub | DEFAULT |
| User | luiz |
| Password | luiz1234 |

## Estrutura de ficheiros

```
server/
├── install.sh                        ← instala tudo do zero
├── scripts/
│   ├── setup-zerotier-router.sh      ← configuração manual ZeroTier + NAT
│   ├── setup-softether-bridge.sh     ← configuração manual SoftEther
│   └── zerotier-watchdog.sh          ← script do watchdog
├── systemd/
│   ├── vpnserver.service             ← SoftEther como serviço
│   ├── zerotier-watchdog.service     ← watchdog (oneshot)
│   ├── zerotier-watchdog.timer       ← timer do watchdog (2min)
│   └── zerotier-one.override.conf    ← fix timing boot WiFi
└── configs/
    ├── 99-zerotier-router.conf       ← sysctl ip_forward
    └── iptables-rules.v4             ← regras NAT
```

## Porquê Layer 2 para PRONETA?

O Siemens PRONETA usa **DCP** e **LLDP** — protocolos Layer 2 baseados em MAC Address que não atravessam routers. O SoftEther em modo bridge cria uma extensão Layer 2 real da rede local.

| Ferramenta | Layer 3 (ZeroTier) | Layer 2 (SoftEther) |
|------------|--------------------|---------------------|
| Ping / SSH / Web | ✓ | ✓ |
| SCADA / OPC-UA | ✓ | ✓ |
| Siemens PRONETA | ✗ | ✓ |
| DCP / LLDP | ✗ | ✓ |
