#!/bin/bash
# =============================================================================
# Script: setup-zerotier-router.sh
# Descrição: Configura o Raspberry Pi como router NAT para ZeroTier
#            Permite que utilizadores ZeroTier acedam à rede local (eth0)
# Rede local: 192.168.222.0/24
# IP ZeroTier do Raspberry: 10.201.114.222
# =============================================================================

set -e

ZEROTIER_NETWORK="b9a18a606f6713e2"
ETH_IFACE="eth0"
ZT_IFACE="zt3hho244n"   # identificar com: ip link show | grep zt

echo "=== 1. Instalar ZeroTier ==="
curl -s https://install.zerotier.com | bash

echo "=== 2. Entrar na rede ZeroTier ==="
zerotier-cli join $ZEROTIER_NETWORK
echo "IMPORTANTE: Autoriza este dispositivo em my.zerotier.com -> rede $ZEROTIER_NETWORK"
echo "Aguarda atribuição de IP e pressiona Enter para continuar..."
read

echo "=== 3. Ativar IP forwarding ==="
sysctl -w net.ipv4.ip_forward=1
echo "net.ipv4.ip_forward=1" > /etc/sysctl.d/99-zerotier-router.conf

echo "=== 4. Configurar iptables (NAT + forwarding) ==="
iptables -A FORWARD -i $ZT_IFACE -o $ETH_IFACE -j ACCEPT
iptables -A FORWARD -i $ETH_IFACE -o $ZT_IFACE -m state --state RELATED,ESTABLISHED -j ACCEPT
iptables -t nat -A POSTROUTING -o $ETH_IFACE -j MASQUERADE

echo "=== 5. Guardar regras iptables ==="
mkdir -p /etc/iptables
iptables-save > /etc/iptables/rules.v4

echo "=== Concluído! ==="
echo "IP ZeroTier do Raspberry: $(zerotier-cli listnetworks | grep $ZEROTIER_NETWORK | awk '{print $NF}')"
echo ""
echo "Clientes ZeroTier devem adicionar rota:"
echo "  Windows: route add 192.168.222.0 mask 255.255.255.0 <IP_ZEROTIER_RASPBERRY>"
echo "  Linux:   sudo ip route add 192.168.222.0/24 via <IP_ZEROTIER_RASPBERRY>"
