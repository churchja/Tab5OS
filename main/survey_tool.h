#pragma once

/*
 * Survey - passive multi-radio INFRASTRUCTURE survey for Tab5OS.
 *
 * Logs what a store broadcasts: BLE advertisements (Electronic Shelf Labels
 * flagged by Bluetooth SIG service UUID 0x1857, beacons, gateways) and Wi-Fi
 * access points (passive beacon scan: SSID / BSSID / channel / auth).
 *
 * Deliberately NOT a people tracker: no Wi-Fi probe-request capture, no
 * client/station identifiers, and it never connects or writes to any
 * device. The point is auditing retail surveillance infrastructure, not
 * surveilling shoppers.
 *
 * Pure logic lives in survey_data.c (host unit-tested); this file is the
 * LVGL / NimBLE / esp_wifi glue.
 */

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_hs.h"
#include "lvgl.h"

typedef void (*survey_scan_refresh_cb_t)(void);

/* Build the screen. scan_refresh re-evaluates main.c's shared BLE scanner. */
void survey_tool_show(lv_obj_t *parent, bool ble_available, bool wifi_available,
                      bool sd_available, survey_scan_refresh_cb_t scan_refresh);

/* Registry leave callback: writes a final CSV checkpoint and releases the scanner. */
void survey_tool_stop(void);

/* True while the survey wants the shared NimBLE discovery running. */
bool survey_tool_scan_requested(void);

/* Called for every advertisement from main.c's GAP discovery callback.
 * Runs in the NimBLE host task: never touches LVGL or files. */
void survey_tool_observe_ble(const ble_addr_t *address, int8_t rssi,
                             const uint8_t *adv_data, uint8_t adv_len);
