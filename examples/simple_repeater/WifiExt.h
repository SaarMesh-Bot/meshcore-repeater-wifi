#pragma once
// ---------------------------------------------------------------------------
// SaarMesh WiFi extension for the MeshCore simple_repeater (ESP32 only)
//
//  * WiFi station mode, configured via CLI (set wifi.ssid / set wifi.pwd ...)
//  * TCP CLI (telnet-style) protected by the repeater admin password
//  * MQTT telemetry with Home Assistant MQTT discovery
//  * ArduinoOTA firmware updates over WiFi (admin password)
//
// Everything is compiled only when WITH_WIFI_EXT is defined.
// The LoRa repeater keeps working unchanged if WiFi/MQTT are unavailable.
// ---------------------------------------------------------------------------

#ifdef WITH_WIFI_EXT

#include <Arduino.h>
#include <FS.h>

void wifi_ext_begin(fs::FS* fs);
void wifi_ext_loop();

// Handles wifi./tcp./mqtt./ota. CLI commands. Returns true if handled.
bool wifi_ext_handle_command(uint32_t sender_timestamp, const char* command, char* reply);

// true while WiFi is enabled (prevents the repeater from light-sleeping)
bool wifi_ext_is_active();

// short status line for the OLED, e.g. "IP 192.168.178.42" or "WiFi: verbinde..."
const char* wifi_ext_status_line();

#endif
