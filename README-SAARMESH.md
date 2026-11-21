# MeshCore Repeater mit WLAN – SaarMesh-Build

Fork von [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore) auf Basis von **repeater-v1.18.0**.
Branch `saarmesh-wifi`, Build-Ziel **`heltec_v4_repeater_wifi`**.

Der Repeater funkt ganz normal im Mesh. Zusätzlich kann er:

| Funktion | Beschreibung |
|---|---|
| **WLAN (Station)** | Zugangsdaten per CLI, werden in `/wifiext.cfg` gespeichert |
| **TCP-CLI** | Telnet-artiger Zugang (Standard Port 23), Login mit dem Admin-Passwort, alle Befehle wie über USB |
| **MQTT + Home Assistant** | Telemetrie als JSON, automatische Anlage in HA per MQTT-Discovery, Buttons für Advert/Neustart |
| **OTA über WLAN** | ArduinoOTA (Passwort = Admin-Passwort), z. B. `pio run -t upload --upload-port <ip>` |

Fällt das WLAN aus, läuft der Repeater unverändert weiter. Die Telemetrie kann dann wie gewohnt
über LoRa abgefragt werden (z. B. MeshCore-HA-Integration → „Repeater Station“).

> **Strom:** Mit aktivem WLAN schläft der ESP32 nicht (Powersaving wird ignoriert). Für Solar-/Akkubetrieb ungeeignet.
> **Sicherheit:** Die TCP-CLI ist unverschlüsselt – nur im LAN bzw. über VPN (Tailscale) nutzen, niemals per Portfreigabe ins Internet.

## Einrichtung (USB-Seriell, 115200 Baud, oder per Remote-Admin über LoRa)

```
password <neues-admin-passwort>
set name <Name>
set wifi.ssid <SSID>
set wifi.pwd <WLAN-Passwort>
set wifi.enabled on

set mqtt.host 192.168.178.126
set mqtt.user <mqtt-user>
set mqtt.pwd <mqtt-passwort>
set mqtt.enabled on

wifi
```

### Befehle der Erweiterung

| Befehl | Bedeutung | Standard |
|---|---|---|
| `wifi` / `wifi.status` | Status: SSID, IP, Signal, TCP/MQTT/OTA – bei MQTT-Fehlern mit Grund (z. B. „Fehler 5 nicht autorisiert“) | |
| `wifi.config` | alle Einstellungen (ohne Passwörter) | |
| `wifi.reconnect` | WLAN neu verbinden | |
| `mqtt.publish` | Discovery + Status sofort senden | |
| `set/get wifi.enabled` | WLAN an/aus | off |
| `set wifi.ssid` / `set wifi.pwd` | Zugangsdaten | |
| `set wifi.hostname` | Hostname | `meshcore-<id>` |
| `set tcp.enabled` / `set tcp.port` | TCP-CLI | on / 23 |
| `set mqtt.enabled` | MQTT an/aus | off |
| `set mqtt.host` / `mqtt.port` | Broker | – / 1883 |
| `set mqtt.user` / `mqtt.pwd` | Login | |
| `set mqtt.prefix` | Topic-Präfix | `meshcore` |
| `set mqtt.interval` | Sendeintervall in s (min. 10) | 60 |
| `set mqtt.cmd` | Buttons (Advert/Neustart) aus HA erlauben | on |
| `set mqtt.discovery` | HA-Discovery senden | on |
| `set ota.enabled` | ArduinoOTA | on |

Passwörter werden bei `get` nie im Klartext ausgegeben.

### TCP-CLI

```
telnet <ip> 23        # oder: nc <ip> 23
Passwort: ********
> stats-core
> get txdelay
> exit
```

Nur eine Sitzung gleichzeitig, 15 min Leerlauf-Timeout, nach 3 Fehlversuchen wird getrennt.

## MQTT-Topics

| Topic | Inhalt |
|---|---|
| `meshcore/<id>/state` | JSON mit allen Werten (retained) |
| `meshcore/<id>/availability` | `online` / `offline` (LWT, retained) |
| `meshcore/<id>/cmd` | `advert`, `flood_advert`, `reboot`, `publish` |
| `homeassistant/sensor/meshcore_<id>/…/config` | Discovery |

`<id>` = erste 6 Byte des Public Keys (hex, klein).

Werte im JSON: `batt_mv, batt_v, batt_pct, noise_floor, last_rssi, last_snr, duty_1h, neighbours, tx_queue,
pkt_recv, pkt_sent, recv_flood, recv_direct, sent_flood, sent_direct, flood_dups, direct_dups, recv_errors,
air_tx, air_rx, uptime, mcu_temp, wifi_rssi, heap_free, err_flags, ip, firmware`.

`duty_1h` = Anteil der Sendezeit in den letzten 60 Minuten (%).

## Bauen

```
pio run -e heltec_v4_repeater_wifi
```

Erstinstallation: `*-merged.bin` ab Adresse `0x0` (z. B. mit flasher.meshcore.io oder esptool).
Update: die normale `.bin` per OTA oder USB.

Geänderte Dateien gegenüber Upstream: `examples/simple_repeater/WifiExt.{h,cpp}` (neu),
kleine Hooks in `MyMesh.{h,cpp}`, `main.cpp`, `UITask.cpp` (IP-Zeile im Display) und das
Build-Ziel in `variants/heltec_v4/platformio.ini`. Alles hinter `#ifdef WITH_WIFI_EXT`.

## Update per OTA (ArduinoOTA)

```
python3 ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
  -i <ip-des-repeaters> -p 3232 -a '<admin-passwort>' -f heltec_v4_repeater_wifi-<version>.bin
```

oder aus dem Repo: `pio run -e heltec_v4_repeater_wifi -t upload --upload-port <ip> --upload-flags --auth=<admin-passwort>`

## Changelog

* **1.0.1** – `wifi` zeigt bei MQTT-Problemen Fehlercode, Grund und nächsten Versuch
* **1.0.0** – erste Version
