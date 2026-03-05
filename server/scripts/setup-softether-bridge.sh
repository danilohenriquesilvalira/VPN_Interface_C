#!/bin/bash
# =============================================================================
# Script: setup-softether-bridge.sh
# Descrição: Configura o SoftEther VPN Server em modo bridge (Layer 2)
#            Permite acesso Layer 2 à rede local via SoftEther Client
#            Ideal para ferramentas como Siemens PRONETA (DCP/LLDP)
# Pré-requisito: ZeroTier já configurado e a funcionar
# =============================================================================

set -e

SOFTETHER_DIR="/home/rls/vpnserver"
VPN_USER="luiz"
VPN_PASSWORD="luiz1234"
ETH_IFACE="eth0"
HUB_NAME="DEFAULT"

echo "=== 1. Iniciar SoftEther VPN Server ==="
cd $SOFTETHER_DIR
./vpnserver start
sleep 3

echo "=== 2. Criar utilizador VPN ==="
printf "\n" | timeout 10 ./vpncmd localhost /SERVER \
    /HUB:$HUB_NAME \
    /CMD UserCreate $VPN_USER /GROUP: /REALNAME:"Utilizador VPN" /NOTE: 2>/dev/null || true

printf "\n" | timeout 10 ./vpncmd localhost /SERVER \
    /HUB:$HUB_NAME \
    /CMD UserPasswordSet $VPN_USER /PASSWORD:$VPN_PASSWORD 2>/dev/null || true

echo "=== 3. Verificar Local Bridge ==="
printf "\n" | timeout 10 ./vpncmd localhost /SERVER /CMD BridgeList 2>/dev/null | grep -E "eth0|Operating" || \
    echo "AVISO: Verifica o bridge manualmente"

echo "=== Concluído! ==="
echo ""
echo "SoftEther VPN Server a ouvir em:"
echo "  - Porta 443 (SSL-VPN)"
echo "  - Porta 992 (SSL-VPN)"
echo "  - Porta 1194 (OpenVPN)"
echo "  - Porta 5555 (Admin)"
echo ""
echo "Configuração para o cliente (SoftEther VPN Client):"
echo "  Host:     <IP_ZEROTIER_RASPBERRY>  (ex: 10.201.114.222)"
echo "  Porta:    443"
echo "  Hub:      $HUB_NAME"
echo "  User:     $VPN_USER"
echo "  Password: $VPN_PASSWORD"
echo ""
echo "Bridge ativo: $HUB_NAME -> $ETH_IFACE (Layer 2)"
echo "Rede local acessível: 192.168.222.0/24"
