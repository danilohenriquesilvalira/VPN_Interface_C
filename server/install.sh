#!/bin/bash
# =============================================================================
# install.sh — Configura o Raspberry Pi como Gateway VPN (ZeroTier + SoftEther)
#
# Uso:
#   sudo bash install.sh
#
# O que faz:
#   1. Instala ZeroTier e entra na rede b9a18a606f6713e2
#   2. Ativa IP forwarding + NAT (iptables) para acesso Layer 3
#   3. Instala SoftEther VPN Server como serviço systemd (Layer 2 bridge)
#   4. Instala watchdog que reinicia ZeroTier automaticamente se ficar OFFLINE
#   5. Tudo arranca automaticamente no boot, sem intervenção humana
#
# Pré-requisitos:
#   - Raspberry Pi com Raspberry Pi OS (64-bit)
#   - SoftEther extraído em /home/rls/vpnserver  (já configurado com bridge eth0)
#   - Ethernet: eth0 ligada à rede local (192.168.222.0/24)
#   - Internet: wlan0 (WiFi) ou outro interface com saída para a Internet
# =============================================================================

set -e

ZEROTIER_NETWORK="b9a18a606f6713e2"
SOFTETHER_DIR="/home/rls/vpnserver"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Cores
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

info()    { echo -e "${GREEN}[OK]${NC} $1"; }
warn()    { echo -e "${YELLOW}[AVISO]${NC} $1"; }
error()   { echo -e "${RED}[ERRO]${NC} $1"; exit 1; }

[ "$EUID" -ne 0 ] && error "Executa com sudo: sudo bash install.sh"

echo "=============================================="
echo "  Gateway VPN — Instalação Automática"
echo "=============================================="
echo ""

# --- 1. ZeroTier ---
echo "=== 1. ZeroTier ==="
if ! command -v zerotier-cli &>/dev/null; then
    info "A instalar ZeroTier..."
    curl -s https://install.zerotier.com | bash
else
    info "ZeroTier já instalado."
fi

# Override para corrigir bug de timing no boot (WiFi)
mkdir -p /etc/systemd/system/zerotier-one.service.d
cp "$SCRIPT_DIR/systemd/zerotier-one.override.conf" \
   /etc/systemd/system/zerotier-one.service.d/override.conf
info "Override de timing aplicado."

systemctl enable zerotier-one
systemctl restart zerotier-one
sleep 12  # aguardar o arranque com o delay

# Entrar na rede
zerotier-cli join $ZEROTIER_NETWORK
info "A aguardar atribuição de IP ZeroTier..."
warn "IMPORTANTE: Autoriza este dispositivo em my.zerotier.com -> rede $ZEROTIER_NETWORK"
warn "Pressiona Enter quando autorizado e com IP atribuído..."
read

ZT_IP=$(zerotier-cli listnetworks | grep $ZEROTIER_NETWORK | awk '{print $NF}' | cut -d'/' -f1)
ZT_IFACE=$(zerotier-cli listnetworks | grep $ZEROTIER_NETWORK | awk '{print $NF-1}' 2>/dev/null || \
           ip link | grep zt | awk -F': ' '{print $2}' | head -1)
info "ZeroTier IP: $ZT_IP  Interface: $ZT_IFACE"

# --- 2. IP Forwarding + iptables ---
echo ""
echo "=== 2. IP Forwarding + NAT ==="

cp "$SCRIPT_DIR/configs/99-zerotier-router.conf" /etc/sysctl.d/
sysctl -w net.ipv4.ip_forward=1
info "IP forwarding ativo."

apt-get install -y iptables-persistent 2>/dev/null || true

cp "$SCRIPT_DIR/configs/iptables-rules.v4" /etc/iptables/rules.v4
netfilter-persistent reload
info "Regras iptables aplicadas e guardadas."

# --- 3. SoftEther VPN Server ---
echo ""
echo "=== 3. SoftEther VPN Server ==="

[ ! -f "$SOFTETHER_DIR/vpnserver" ] && error "SoftEther não encontrado em $SOFTETHER_DIR"

cp "$SCRIPT_DIR/systemd/vpnserver.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable vpnserver.service
systemctl restart vpnserver.service
sleep 3

if systemctl is-active --quiet vpnserver.service; then
    info "SoftEther VPN Server a correr."
else
    error "SoftEther falhou ao arrancar. Verifica: journalctl -u vpnserver"
fi

# --- 4. Watchdog ZeroTier ---
echo ""
echo "=== 4. Watchdog ZeroTier ==="

cp "$SCRIPT_DIR/scripts/zerotier-watchdog.sh" /usr/local/bin/
chmod +x /usr/local/bin/zerotier-watchdog.sh

cp "$SCRIPT_DIR/systemd/zerotier-watchdog.service" /etc/systemd/system/
cp "$SCRIPT_DIR/systemd/zerotier-watchdog.timer"   /etc/systemd/system/
systemctl daemon-reload
systemctl enable zerotier-watchdog.timer
systemctl start zerotier-watchdog.timer
info "Watchdog ativo — ZeroTier será reiniciado automaticamente se ficar OFFLINE."

# --- Resumo Final ---
echo ""
echo "=============================================="
echo -e "  ${GREEN}Instalação concluída!${NC}"
echo "=============================================="
echo ""
echo "Estado dos serviços:"
systemctl is-active zerotier-one.service   | xargs printf "  zerotier-one:        %s\n"
systemctl is-active vpnserver.service      | xargs printf "  vpnserver (SE):      %s\n"
systemctl is-active zerotier-watchdog.timer| xargs printf "  watchdog (timer):    %s\n"
systemctl is-active netfilter-persistent   | xargs printf "  iptables:            %s\n"
echo ""
echo "ZeroTier: $(zerotier-cli info 2>/dev/null)"
echo ""
echo "Acesso Layer 3 (clientes ZeroTier):"
echo "  Windows: route add 192.168.222.0 mask 255.255.255.0 $ZT_IP"
echo "  Linux:   sudo ip route add 192.168.222.0/24 via $ZT_IP"
echo ""
echo "Acesso Layer 2 (SoftEther Client):"
echo "  Host: $ZT_IP  Porta: 443  Hub: DEFAULT  User: luiz  Pass: luiz1234"
echo ""
