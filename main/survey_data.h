#pragma once

/*
 * survey_data - pure C11 logic for the Survey app (no ESP-IDF / LVGL deps,
 * host-testable like the other *_data modules).
 *
 * Scope: store INFRASTRUCTURE only - BLE advertisements and Wi-Fi access
 * points (beacons). There is deliberately no API here for Wi-Fi probe
 * requests or client/station identifiers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SURVEY_SRC_BLE  0u
#define SURVEY_SRC_WIFI 1u

#define SURVEY_ESL_SERVICE_UUID 0x1857u /* Bluetooth SIG Electronic Shelf Label Service */
#define SURVEY_NO_COMPANY       0xFFFFu
#define SURVEY_ADDR_TYPE_WIFI   0xFFu

#define SURVEY_F_ESL     0x01u /* advertises / carries the ESL service UUID */
#define SURVEY_F_PROFILE 0x02u /* SSID matched the selected store profile  */
#define SURVEY_F_NAMED   0x04u
#define SURVEY_F_WIFI    0x08u

#define SURVEY_NAME_LEN 33u

typedef struct {
    uint8_t source;      /* SURVEY_SRC_* */
    uint8_t addr[6];     /* display order, most significant byte first */
    uint8_t addr_type;   /* BLE address type, SURVEY_ADDR_TYPE_WIFI for APs */
    int8_t rssi_last;
    int8_t rssi_best;
    uint8_t flags;       /* SURVEY_F_* */
    uint8_t channel;     /* Wi-Fi primary channel */
    uint8_t authmode;    /* Wi-Fi auth mode (esp_wifi numbering) */
    uint16_t company_id; /* BLE manufacturer company ID or SURVEY_NO_COMPANY */
    uint16_t service16;  /* notable 16-bit service UUID, 0 = none */
    uint16_t hits;
    int64_t first_seen;  /* Unix seconds */
    int64_t last_seen;
    char name[SURVEY_NAME_LEN]; /* BLE local name or Wi-Fi SSID (sanitized) */
} survey_entry_t;

typedef struct {
    survey_entry_t *rows;
    uint16_t count;
    uint16_t capacity;
    bool overflowed;
} survey_table_t;

typedef struct {
    bool esl;
    uint16_t service16;
    uint16_t company_id;
    bool has_name;
    char name[SURVEY_NAME_LEN];
} survey_adv_info_t;

/* Parse raw advertising / scan-response AD structures.
 * Returns 0 on success, -1 if the payload is malformed (out is still
 * filled with whatever parsed cleanly before the error). */
int survey_parse_adv(const uint8_t *data, size_t length, survey_adv_info_t *out);

void survey_table_init(survey_table_t *table, survey_entry_t *rows, uint16_t capacity);
void survey_table_clear(survey_table_t *table);
/* Find or insert by (source, addr). NULL when full (sets overflowed). */
survey_entry_t *survey_table_locate(survey_table_t *table, uint8_t source, const uint8_t addr[6]);

/* BLE addresses arrive least-significant byte first (NimBLE ble_addr_t.val). */
void survey_addr_from_le(const uint8_t le[6], uint8_t out_msb_first[6]);

void survey_merge_ble(survey_table_t *table, const uint8_t addr_msb_first[6], uint8_t addr_type,
                      int8_t rssi, const survey_adv_info_t *info, int64_t now);
void survey_merge_wifi(survey_table_t *table, const uint8_t bssid[6], int8_t rssi,
                       uint8_t channel, uint8_t authmode, const char *ssid,
                       unsigned profile, int64_t now);

unsigned survey_profile_count(void);
const char *survey_profile_name(unsigned profile);
bool survey_profile_ssid_hit(unsigned profile, const char *ssid);
void survey_reapply_profile(survey_table_t *table, unsigned profile);

const char *survey_company_name(uint16_t company_id);
const char *survey_auth_name(uint8_t authmode);
const char *survey_vendor_label(const survey_entry_t *entry);
void survey_flags_str(uint8_t flags, char out[5]);

/* qsort comparator: ESL first, then profile hits, then strongest RSSI. */
int survey_compare(const void *a, const void *b);

/* CSV (UTC timestamps). Return bytes written, or -1 if it would not fit. */
int survey_csv_header(char *buffer, size_t capacity);
int survey_csv_row(const survey_entry_t *entry, char *buffer, size_t capacity);
