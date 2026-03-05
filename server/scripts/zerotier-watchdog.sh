#!/bin/bash
# Watchdog: verifica se ZeroTier está ONLINE, reinicia se não estiver
# Executado pelo timer zerotier-watchdog.timer a cada 2 minutos

STATUS=$(zerotier-cli info 2>/dev/null | awk '{print $3}')

if [ "$STATUS" != "ONLINE" ]; then
    logger -t zerotier-watchdog "ZeroTier está $STATUS - a reiniciar..."
    systemctl restart zerotier-one.service
    sleep 5
    NEW_STATUS=$(zerotier-cli info 2>/dev/null | awk '{print $3}')
    logger -t zerotier-watchdog "ZeroTier após reinicio: $NEW_STATUS"
else
    logger -t zerotier-watchdog "ZeroTier OK: $STATUS"
fi
