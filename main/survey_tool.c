#include "survey_tool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "storage_io.h"
#include "survey_data.h"

#define SURVEY_MAX_ENTRIES   768
#define SURVEY_WIFI_MAX      48
#define SURVEY_LIST_ROWS     40
#define SURVEY_FMT_CAP       8192
#define SURVEY_UI_PERIOD_MS  750
#define SURVEY_CKPT_MS       10000
#define SURVEY_DIR           "/sdcard/SURVEY"   /* FAT is 8.3-only in this build */
#define SURVEY_PATH_CAP      48

static const char *TAG = "survey";

/* ----- shared state (BLE host task <-> LVGL task <-> Wi-Fi worker) ------- *
 * A mutex, not a portMUX spinlock: the UI copies up to ~77 KB out of PSRAM
 * while holding it, which must not run with interrupts disabled. */
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static survey_entry_t *rows;      /* PSRAM */
static survey_entry_t *snapshot;  /* PSRAM: UI/CSV work outside the lock */
static survey_table_t table;

static wifi_ap_record_t *wifi_records; /* PSRAM, written by the worker task */
static volatile bool wifi_busy;
static volatile bool wifi_result_ready;
static volatile uint16_t wifi_result_count;
static volatile esp_err_t wifi_error;

static volatile bool screen_active;
static volatile bool ble_running;
static volatile unsigned profile;

/* ----- screen ------------------------------------------------------------ */
static lv_obj_t *status_label;
static lv_obj_t *summary_label;
static lv_obj_t *list_label;
static lv_obj_t *ble_button_label;
static lv_timer_t *ui_timer;
static char *fmt_buf;             /* PSRAM */
static char csv_line[512];

static bool ble_ok;
static bool wifi_ok;
static bool sd_ok;
static survey_scan_refresh_cb_t refresh_scan;
static int64_t last_checkpoint_ms;

static bool csv_ready;
static char csv_final[SURVEY_PATH_CAP];
static char csv_temp[SURVEY_PATH_CAP];
static char csv_backup[SURVEY_PATH_CAP];

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool lock_take(TickType_t wait)
{
    return lock && xSemaphoreTake(lock, wait) == pdTRUE;
}

static void lock_give(void)
{
    xSemaphoreGive(lock);
}

static bool ensure_resources(void)
{
    if (!lock) lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (!rows)
        rows = heap_caps_malloc(SURVEY_MAX_ENTRIES * sizeof(*rows), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot)
        snapshot = heap_caps_malloc(SURVEY_MAX_ENTRIES * sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wifi_records)
        wifi_records = heap_caps_malloc(SURVEY_WIFI_MAX * sizeof(*wifi_records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fmt_buf)
        fmt_buf = heap_caps_malloc(SURVEY_FMT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return lock && rows && snapshot && wifi_records && fmt_buf;
}

/* ===================== BLE intake (NimBLE host task) ===================== */
void survey_tool_observe_ble(const ble_addr_t *address, int8_t rssi,
                             const uint8_t *adv_data, uint8_t adv_len)
{
    if (!screen_active || !ble_running || !address || !lock) return;

    survey_adv_info_t info;
    (void)survey_parse_adv(adv_data, adv_len, &info); /* partial parse still useful */
    uint8_t addr[6];
    survey_addr_from_le(address->val, addr);
    int64_t now = (int64_t)time(NULL);

    /* Never stall the BLE host: drop this advert if the UI holds the lock. */
    if (!lock_take(pdMS_TO_TICKS(20))) return;
    survey_merge_ble(&table, addr, address->type, rssi, &info, now);
    lock_give();
}

/* ===================== Wi-Fi AP intake (worker task) ===================== *
 * Passive beacon scan: the radio only listens - no probe requests are sent,
 * and nothing about client devices is captured. Runs off the LVGL task so
 * the ~2 s all-channel sweep doesn't freeze the screen. */
static void wifi_task(void *argument)
{
    (void)argument;
    wifi_scan_config_t config = {0};
    config.show_hidden = true;
    config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
    config.scan_time.passive = 120; /* ms per channel */

    esp_err_t error = esp_wifi_scan_start(&config, true);
    uint16_t count = SURVEY_WIFI_MAX;
    if (error == ESP_OK) error = esp_wifi_scan_get_ap_records(&count, wifi_records);
    wifi_error = error;
    wifi_result_count = error == ESP_OK ? count : 0;
    wifi_result_ready = true;
    wifi_busy = false;
    vTaskDelete(NULL);
}

static void merge_wifi_results(void)
{
    if (!wifi_result_ready) return;
    wifi_result_ready = false;
    if (wifi_error != ESP_OK) {
        lv_label_set_text_fmt(status_label, "Wi-Fi scan failed: %s", esp_err_to_name(wifi_error));
        return;
    }
    uint16_t count = wifi_result_count;
    int64_t now = (int64_t)time(NULL);
    if (!lock_take(portMAX_DELAY)) return;
    for (uint16_t i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &wifi_records[i];
        survey_merge_wifi(&table, ap->bssid, ap->rssi, ap->primary, (uint8_t)ap->authmode,
                          (const char *)ap->ssid, profile, now);
    }
    lock_give();
    lv_label_set_text_fmt(status_label, "Wi-Fi: %u access points in this sweep", (unsigned)count);
    ESP_LOGI(TAG, "merged %u access points", (unsigned)count);
}

/* ===================== CSV (crash-safe, 8.3 names) ======================= */
static bool choose_session_file(void)
{
    if (mkdir(SURVEY_DIR, 0775) != 0 && errno != EEXIST) return false;
    for (unsigned n = 1; n <= 99999; n++) {
        struct stat st;
        snprintf(csv_final, sizeof(csv_final), SURVEY_DIR "/SVY%05u.CSV", n);
        if (stat(csv_final, &st) == 0) continue;
        if (errno != ENOENT) return false;
        snprintf(csv_temp, sizeof(csv_temp), SURVEY_DIR "/SVY%05u.TMP", n);
        snprintf(csv_backup, sizeof(csv_backup), SURVEY_DIR "/SVY%05u.BAK", n);
        return true;
    }
    return false;
}

static bool csv_checkpoint(uint16_t count)
{
    if (!sd_ok || !csv_ready || !count) return false;
    FILE *file = fopen(csv_temp, "w");
    if (!file) return false;
    bool ok = survey_csv_header(csv_line, sizeof(csv_line)) > 0 && fputs(csv_line, file) >= 0;
    for (uint16_t i = 0; ok && i < count; i++) {
        if (survey_csv_row(&snapshot[i], csv_line, sizeof(csv_line)) < 0) continue; /* never partial */
        ok = fputs(csv_line, file) >= 0;
    }
    if (!ok || ferror(file)) {
        fclose(file);
        remove(csv_temp);
        return false;
    }
    /* Closes + fsyncs the temp, keeps the previous checkpoint as .BAK. */
    return storage_commit_replace_file(&file, csv_temp, csv_final, csv_backup) == 0;
}

/* Copy the live table out under the lock; returns row count. */
static uint16_t take_snapshot(bool *overflowed)
{
    uint16_t count = 0;
    if (!lock_take(portMAX_DELAY)) return 0;
    count = table.count;
    if (count) memcpy(snapshot, rows, count * sizeof(*rows));
    if (overflowed) *overflowed = table.overflowed;
    lock_give();
    return count;
}

/* ===================== UI ================================================= */
static void render_list(uint16_t count)
{
    qsort(snapshot, count, sizeof(*snapshot), survey_compare);
    size_t pos = 0;
    fmt_buf[0] = '\0';
    for (uint16_t i = 0; i < count && i < SURVEY_LIST_ROWS; i++) {
        const survey_entry_t *e = &snapshot[i];
        const char *tag = (e->flags & SURVEY_F_ESL) ? "[ESL]" : (e->flags & SURVEY_F_PROFILE) ? "[HIT]"
                        : e->source == SURVEY_SRC_WIFI ? "[AP]" : "[BLE]";
        char detail[48];
        if (e->source == SURVEY_SRC_WIFI) {
            snprintf(detail, sizeof(detail), "ch%u %s", (unsigned)e->channel, survey_auth_name(e->authmode));
        } else if (e->flags & SURVEY_F_ESL) {
            snprintf(detail, sizeof(detail), "shelf label 0x1857");
        } else if (survey_company_name(e->company_id)) {
            snprintf(detail, sizeof(detail), "%s", survey_company_name(e->company_id));
        } else if (e->company_id != SURVEY_NO_COMPANY) {
            snprintf(detail, sizeof(detail), "mfg 0x%04X", (unsigned)e->company_id);
        } else {
            detail[0] = '\0';
        }
        int written = snprintf(fmt_buf + pos, SURVEY_FMT_CAP - pos,
            "%s %02X:%02X:%02X:%02X:%02X:%02X %4d dBm  %s  %s  x%u\n", tag,
            e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5],
            (int)e->rssi_best, e->name[0] ? e->name : (e->source == SURVEY_SRC_WIFI ? "<hidden>" : "-"),
            detail, (unsigned)e->hits);
        if (written < 0 || (size_t)written >= SURVEY_FMT_CAP - pos) break;
        pos += (size_t)written;
    }
    lv_label_set_text(list_label, pos ? fmt_buf : "Nothing yet - start BLE and/or scan Wi-Fi.");
}

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!screen_active) return;
    merge_wifi_results();

    bool overflowed = false;
    uint16_t count = take_snapshot(&overflowed);
    unsigned esl = 0, aps = 0, hits = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (snapshot[i].flags & SURVEY_F_ESL) esl++;
        if (snapshot[i].source == SURVEY_SRC_WIFI) aps++;
        if (snapshot[i].flags & SURVEY_F_PROFILE) hits++;
    }
    lv_label_set_text_fmt(summary_label, "%s | devices %u%s | shelf labels %u | APs %u | profile hits %u",
                          survey_profile_name(profile), (unsigned)count, overflowed ? "+ (table full)" : "",
                          esl, aps, hits);

    if (sd_ok && csv_ready && count && uptime_ms() - last_checkpoint_ms >= SURVEY_CKPT_MS) {
        last_checkpoint_ms = uptime_ms();
        if (!csv_checkpoint(count)) lv_label_set_text(status_label, "SD write failed - check the card");
        else if (!wifi_busy) lv_label_set_text_fmt(status_label, "Saved %u rows to %s", (unsigned)count, csv_final);
    }
    render_list(count); /* sorts snapshot in place - after the checkpoint */
}

static void ble_toggle(lv_event_t *event)
{
    (void)event;
    if (!ble_ok) return;
    ble_running = !ble_running;
    lv_label_set_text(ble_button_label, ble_running ? "Stop BLE" : "Start BLE");
    if (refresh_scan) refresh_scan();
}

static void wifi_scan(lv_event_t *event)
{
    (void)event;
    if (!wifi_ok || wifi_busy) return;
    wifi_busy = true;
    if (xTaskCreate(wifi_task, "survey_wifi", 6144, NULL, 4, NULL) != pdPASS) {
        wifi_busy = false;
        lv_label_set_text(status_label, "Could not start Wi-Fi scan task");
        return;
    }
    lv_label_set_text(status_label, "Wi-Fi: listening on every channel (~2 s)...");
}

static void profile_changed(lv_event_t *event)
{
    unsigned selected = (unsigned)lv_dropdown_get_selected(lv_event_get_target_obj(event));
    profile = selected < survey_profile_count() ? selected : 0;
    if (!lock_take(portMAX_DELAY)) return;
    survey_reapply_profile(&table, profile);
    lock_give();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    if (label_out) *label_out = label;
    return button;
}

/* ===================== lifecycle ========================================== */
bool survey_tool_scan_requested(void)
{
    return screen_active && ble_running;
}

void survey_tool_show(lv_obj_t *parent, bool ble_available, bool wifi_available,
                      bool sd_available, survey_scan_refresh_cb_t scan_refresh)
{
    if (!ensure_resources()) {
        lv_label_set_text(lv_label_create(parent), "Survey: not enough PSRAM");
        return;
    }
    ble_ok = ble_available;
    wifi_ok = wifi_available;
    sd_ok = sd_available;
    refresh_scan = scan_refresh;

    if (lock_take(portMAX_DELAY)) {
        survey_table_init(&table, rows, SURVEY_MAX_ENTRIES);
        lock_give();
    }
    wifi_result_ready = false;
    csv_ready = sd_ok && choose_session_file();
    last_checkpoint_ms = uptime_ms();

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 6, 0);

    status_label = lv_label_create(root);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    if (csv_ready)
        lv_label_set_text_fmt(status_label, "Receive-only infrastructure survey. Logging to %s", csv_final);
    else
        lv_label_set_text(status_label, sd_ok ? "Receive-only survey. Could not create /sdcard/SURVEY - screen only."
                                              : "Receive-only survey. No SD card - screen only, nothing saved.");

    lv_obj_t *controls = lv_obj_create(root);
    lv_obj_set_size(controls, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(controls, 8, 0);

    make_button(controls, ble_ok ? "Start BLE" : "BLE unavailable", ble_toggle, &ble_button_label);
    make_button(controls, wifi_ok ? "Scan Wi-Fi" : "Wi-Fi unavailable", wifi_scan, NULL);

    lv_obj_t *profiles = lv_dropdown_create(controls);
    char options[192];
    size_t used = 0;
    options[0] = '\0';
    for (unsigned i = 0; i < survey_profile_count(); i++) {
        int n = snprintf(options + used, sizeof(options) - used, "%s%s", i ? "\n" : "", survey_profile_name(i));
        if (n < 0 || (size_t)n >= sizeof(options) - used) break;
        used += (size_t)n;
    }
    lv_dropdown_set_options(profiles, options);
    lv_dropdown_set_selected(profiles, profile < survey_profile_count() ? profile : 0);
    lv_obj_add_event_cb(profiles, profile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    summary_label = lv_label_create(root);
    lv_obj_set_width(summary_label, lv_pct(100));
    lv_label_set_long_mode(summary_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(summary_label, "");

    lv_obj_t *panel = lv_obj_create(root);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_flex_grow(panel, 1);
    list_label = lv_label_create(panel);
    lv_obj_set_width(list_label, lv_pct(100));
    lv_label_set_long_mode(list_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(list_label, "Nothing yet - start BLE and/or scan Wi-Fi.");

    ble_running = false;
    screen_active = true;
    ui_timer = lv_timer_create(ui_tick, SURVEY_UI_PERIOD_MS, NULL);
}

void survey_tool_stop(void)
{
    if (!screen_active) return;
    screen_active = false;
    ble_running = false;
    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = NULL;
    }
    if (ble_ok && refresh_scan) refresh_scan(); /* release the shared scanner */

    uint16_t count = take_snapshot(NULL);
    if (count && !csv_checkpoint(count)) ESP_LOGW(TAG, "final checkpoint failed");

    /* clear_content() deletes the widgets right after this leave callback. */
    status_label = summary_label = list_label = ble_button_label = NULL;
}
