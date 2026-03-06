#!/bin/bash
# Executa comandos do PLC via PROFINET
# Formato: CMD_FLAGS WIFI_IDX

CMD_FILE="/tmp/pnet_cmd"
[ -f "$CMD_FILE" ] || exit 0

read CMD_FLAGS WIFI_IDX < "$CMD_FILE"
rm -f "$CMD_FILE"

WIFI_1="P300"
WIFI_2="R L S-5G"
WIFI_3="iPhone de Danilo"

logger -t pnet-cmd "Comando: FLAGS=$CMD_FLAGS WIFI_IDX=$WIFI_IDX"

# Bit 0: Ligar WiFi por indice
if [ $(( CMD_FLAGS & 0x01 )) -ne 0 ]; then
    case "$WIFI_IDX" in
        1) SSID="$WIFI_1" ; PASS="12345678" ;;
        2) SSID="$WIFI_2" ; PASS="" ;;
        3) SSID="$WIFI_3" ; PASS="" ;;
        *) SSID="" ; PASS="" ;;
    esac
    if [ -n "$SSID" ]; then
        if [ -n "$PASS" ]; then
            nmcli dev wifi connect "$SSID" password "$PASS" ifname wlan0 2>/dev/null && \
                logger -t pnet-cmd "WiFi ligado: $SSID" || \
                logger -t pnet-cmd "ERRO WiFi: $SSID"
        else
            nmcli dev wifi connect "$SSID" ifname wlan0 2>/dev/null && \
                logger -t pnet-cmd "WiFi ligado: $SSID" || \
                logger -t pnet-cmd "ERRO WiFi: $SSID"
        fi
    fi
fi

# Bit 1: Reiniciar ZeroTier
if [ $(( CMD_FLAGS & 0x02 )) -ne 0 ]; then
    systemctl restart zerotier-one.service
    logger -t pnet-cmd "ZeroTier reiniciado"
fi

# Bit 2: Reiniciar SoftEther
if [ $(( CMD_FLAGS & 0x04 )) -ne 0 ]; then
    systemctl restart vpnserver.service
    logger -t pnet-cmd "SoftEther reiniciado"
fi
