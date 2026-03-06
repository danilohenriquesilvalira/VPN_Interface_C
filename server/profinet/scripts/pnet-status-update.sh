#!/bin/bash
# Estado do sistema RLS MiniPC para PROFINET
# Formato: STATUS_FLAGS WIFI_IDX SE_SESSIONS WIFI_VISIBLE

WIFI_1="P300"
WIFI_2="R L S-5G"
WIFI_3="iPhone de Danilo"

STATUS_FLAGS=0
WIFI_IDX=0
SE_SESSIONS=0
WIFI_VISIBLE=0

# Bit 0: WiFi ligado + indice da rede activa
CURRENT_WIFI=$(iwgetid wlan0 --raw 2>/dev/null)
if [ -n "$CURRENT_WIFI" ]; then
    STATUS_FLAGS=$((STATUS_FLAGS | 0x01))
    [ "$CURRENT_WIFI" = "$WIFI_1" ] && WIFI_IDX=1
    [ "$CURRENT_WIFI" = "$WIFI_2" ] && WIFI_IDX=2
    [ "$CURRENT_WIFI" = "$WIFI_3" ] && WIFI_IDX=3
fi

# Bit 1: ZeroTier ONLINE
zerotier-cli info 2>/dev/null | awk '{print $5}' | grep -q 'ONLINE' && STATUS_FLAGS=$((STATUS_FLAGS | 0x02))

# Bit 2: ZeroTier rede OK
zerotier-cli listnetworks 2>/dev/null | awk '$3=="b9a18a606f6713e2" {print $6}' | grep -q '^OK$' && STATUS_FLAGS=$((STATUS_FLAGS | 0x04))

# Bit 3: SoftEther activo
systemctl is-active --quiet vpnserver.service 2>/dev/null && STATUS_FLAGS=$((STATUS_FLAGS | 0x08))

# Bit 4: eth0 ligado com IP
ip addr show eth0 2>/dev/null | grep -q 'inet ' && STATUS_FLAGS=$((STATUS_FLAGS | 0x10))

# Bit 5: Internet OK
ping -c 1 -W 2 8.8.8.8 &>/dev/null 2>&1 && STATUS_FLAGS=$((STATUS_FLAGS | 0x20))

# Byte 2: SoftEther sessoes (processos activos)
SE_SESSIONS=$(pgrep -c vpnserver 2>/dev/null || echo 0)

# Byte 3: Redes WiFi visiveis
SCAN=$(iw dev wlan0 scan dump 2>/dev/null | grep "SSID:")
echo "$SCAN" | grep -qF "$WIFI_1" && WIFI_VISIBLE=$((WIFI_VISIBLE | 0x01))
echo "$SCAN" | grep -qF "$WIFI_2" && WIFI_VISIBLE=$((WIFI_VISIBLE | 0x02))
echo "$SCAN" | grep -qF "$WIFI_3" && WIFI_VISIBLE=$((WIFI_VISIBLE | 0x04))

printf "%d %d %d %d\n" "$STATUS_FLAGS" "$WIFI_IDX" "$SE_SESSIONS" "$WIFI_VISIBLE" > /tmp/pnet_status_full
