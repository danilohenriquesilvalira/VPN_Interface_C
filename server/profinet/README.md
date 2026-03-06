# PROFINET Gateway — RLS MiniPC

Integração PROFINET entre Raspberry Pi 4 (p-net v1.0.2) e Siemens TIA Portal.
O Raspberry actua como IO Device PROFINET, expondo o estado dos serviços VPN ao PLC.

---

## Arquitectura

```
TIA Portal (PLC/HMI)
    │
    │  PROFINET  (eth0 · 192.168.1.50)
    │
Raspberry Pi 4  ─── wlan0 ──► WiFi (P300 / RLS-5G / iPhone)
                ─── ZeroTier ► VPN mesh b9a18a606f6713e2
                ─── SoftEther► VPN Server
```

---

## Módulos PROFINET

| Módulo | ID | Slot | Direcção | Bytes | Descrição |
|---|---|---|---|---|---|
| RLS Monitor | 0x50 | 1 | IN  (IB) | 4 | Estado dos serviços → PLC |
| RLS Control | 0x51 | 2 | OUT (QB) | 4 | Comandos PLC → Raspberry |

### Monitor IN (IB0..IB3)

| Byte | Bit | Tag FB | Descrição |
|---|---|---|---|
| IB0 | 0 | Status.WiFi_Ligado   | WiFi ligado (wlan0 com IP) |
| IB0 | 1 | Status.ZT_Online     | ZeroTier ONLINE |
| IB0 | 2 | Status.ZT_Rede_OK    | ZeroTier rede autorizada |
| IB0 | 3 | Status.SE_Activo     | SoftEther VPN activo |
| IB0 | 4 | Status.Eth0_Ligado   | Ethernet PROFINET com IP |
| IB0 | 5 | Status.Internet_OK   | Internet acessível |
| IB1 | — | Status.WiFi_Activa   | Rede activa: 0=N/A 1=P300 2=RLS-5G 3=iPhone |
| IB2 | — | Status.SE_Sessoes    | Sessões SoftEther activas |
| IB3 | 0 | Status.P300_Visivel  | P300 no alcance |
| IB3 | 1 | Status.RLS5G_Visivel | RLS-5G no alcance |
| IB3 | 2 | Status.iPhone_Visivel| iPhone de Danilo no alcance |

### Control OUT (QB0..QB1)

| Byte | Bit | Tag FB | Descrição |
|---|---|---|---|
| QB0 | 0 | HMI.Ligar_WiFi    | Pulso: ligar ao WiFi (ver QB1) |
| QB0 | 1 | HMI.Reiniciar_ZT  | Pulso: reiniciar ZeroTier |
| QB0 | 2 | HMI.Reiniciar_SE  | Pulso: reiniciar SoftEther |
| QB1 | — | HMI.WiFi_Indice   | Índice: 1=P300 2=RLS-5G 3=iPhone |

---

## Estrutura de ficheiros

```
profinet/
  systemd/              # Serviços systemd (instalar em /etc/systemd/system/)
    pnet-dev.service    # Servidor PROFINET p-net (Restart=always)
    pnet-status.service # Executa script de estado (oneshot)
    pnet-status.timer   # Timer: actualiza estado a cada 10s
    pnet-cmd.path       # Watcher: detecta /tmp/pnet_cmd
    pnet-cmd.service    # Executa handler de comandos (oneshot)

  scripts/              # Scripts bash (instalar em /usr/local/bin/)
    pnet-status-update.sh   # Lê estado WiFi/ZT/SE → /tmp/pnet_status_full
    pnet-cmd-handler.sh     # Lê /tmp/pnet_cmd → executa comandos

  pn_dev/               # Ficheiros C modificados do p-net sample
    app_data.c          # Lógica de leitura/escrita dados PROFINET
    app_gsdml.c         # Definição de módulos e submódulos
    app_gsdml.h         # Constantes IDs e tamanhos

  gsdml/                # Ficheiros para importar no TIA Portal
    GSDML-V2.43-RLS-MiniPC-20260306.xml
    GSDML-0175-0001-1-RLS.bmp
    GSDML-0175-0001-1-RLS_48.bmp
    GSDML-0175-0001-1-RLS_128.bmp

  tia_portal/           # Código SCL para TIA Portal
    RLS_VPN_FB.txt      # FB RLS_VPN + UDT_RLS_Status + UDT_RLS_Cmd
    RLS_HMI_mockup.svg  # Mockup do ecrã HMI sugerido
```

---

## Instalação no Raspberry Pi

### 1. Compilar p-net

```bash
cd /home/rls/pnet/p-net-1.0.2-samples
mkdir -p build/debug && cd build/debug
cmake ../.. -DCMAKE_BUILD_TYPE=Debug
make -j4 pn_dev
```

### 2. Instalar scripts

```bash
sudo cp scripts/pnet-status-update.sh /usr/local/bin/
sudo cp scripts/pnet-cmd-handler.sh   /usr/local/bin/
sudo chmod +x /usr/local/bin/pnet-*.sh
```

### 3. Instalar serviços systemd

```bash
sudo cp systemd/*.service /etc/systemd/system/
sudo cp systemd/*.timer   /etc/systemd/system/
sudo cp systemd/*.path    /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now pnet-dev.service
sudo systemctl enable --now pnet-status.timer
sudo systemctl enable --now pnet-cmd.path
```

### 4. Verificar estado

```bash
systemctl status pnet-dev
cat /tmp/pnet_status_full   # STATUS_FLAGS WIFI_IDX SE_SESSIONS WIFI_VISIBLE
```

---

## TIA Portal

1. **Importar GSDML**: Options → Manage GSD → Install → seleccionar `gsdml/GSDML-V2.43-RLS-MiniPC-20260306.xml`
2. **Criar UDTs**: PLC data types → `UDT_RLS_Status` e `UDT_RLS_Cmd` (ver `tia_portal/RLS_VPN_FB.txt`)
3. **Criar FB**: Program blocks → `RLS_VPN` em SCL
4. **Instanciar no OB1** com IB0..IB3 e QB0..QB1
5. **Update time**: configurar 128ms no device properties

---

## Parâmetros PROFINET

| Parâmetro | Valor |
|---|---|
| VendorID | 0x0175 |
| DeviceID | 0x0002 |
| DNS name | rls-minipc |
| IP | 192.168.1.50 |
| Stack | p-net v1.0.2 (rt-labs) |
| Update time recomendado | 128ms |
| Watchdog | 5 ciclos |
