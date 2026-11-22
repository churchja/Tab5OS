#include "survey_data.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ----- store profiles ---------------------------------------------------
 * Highlighting only - every signal is logged regardless of profile. ESL
 * tags (0x1857) are vendor-neutral and always highlighted. SSID hints are
 * lowercase substrings and are field-refinable starting points: add what
 * you actually see in each chain. Keep hints specific ("sams" would also
 * match "Samsung ..." SSIDs). */
typedef struct {
    const char *name;
    const char *hints[4];
} survey_profile_t;

static const survey_profile_t PROFILES[] = {
    {"Generic", {NULL}},
    {"Walmart", {"walmart", NULL}},
    {"Sam's Club", {"samsclub", "sams club", NULL}},
    {"Costco", {"costco", NULL}},
    {"Home Depot", {"homedepot", "home depot", "thd", NULL}},
    {"Lowe's", {"lowes", "lowe's", NULL}},
    {"Target", {"target", NULL}},
    {"Meijer", {"meijer", NULL}},
};
#define PROFILE_COUNT ((unsigned)(sizeof(PROFILES) / sizeof(PROFILES[0])))

/* Only stable, well-known company IDs, so consumer gear (phones, earbuds,
 * watches) is easy to tell apart from store infrastructure. The ESL vendor's
 * own ID is deliberately not guessed: it is logged for every device, so read
 * it off a real 0x1857 hit and add it here. */
typedef struct {
    uint16_t id;
    const char *name;
} survey_company_t;

static const survey_company_t COMPANIES[] = {
    {0x004C, "Apple"},
    {0x0006, "Microsoft"},
    {0x00E0, "Google"},
    {0x0075, "Samsung"},
};
#define COMPANY_COUNT (sizeof(COMPANIES) / sizeof(COMPANIES[0]))

/* ----- helpers ----------------------------------------------------------- */
static void sanitize_copy(char *out, size_t out_size, const uint8_t *in, size_t in_len)
{
    size_t n = in_len < out_size - 1 ? in_len : out_size - 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        out[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    out[n] = '\0';
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void note_service(survey_adv_info_t *out, uint16_t uuid)
{
    if (uuid == SURVEY_ESL_SERVICE_UUID) {
        out->esl = true;
        out->service16 = SURVEY_ESL_SERVICE_UUID;
    } else if (!out->service16) {
        out->service16 = uuid;
    }
}

/* ----- advertisement parsing -------------------------------------------- */
int survey_parse_adv(const uint8_t *data, size_t length, survey_adv_info_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->company_id = SURVEY_NO_COMPANY;
    if (!data) return length ? -1 : 0;

    bool name_complete = false;
    size_t i = 0;
    while (i < length) {
        size_t len = data[i];
        if (len == 0) break; /* zero padding terminates the payload */
        if (len > length - i - 1) return -1;
        uint8_t type = data[i + 1];
        const uint8_t *payload = &data[i + 2];
        size_t payload_len = len - 1;

        switch (type) {
        case 0x02: /* incomplete list of 16-bit service UUIDs */
        case 0x03: /* complete list of 16-bit service UUIDs */
            for (size_t k = 0; k + 1 < payload_len; k += 2) note_service(out, le16(&payload[k]));
            break;
        case 0x16: /* service data, 16-bit UUID */
            if (payload_len >= 2) note_service(out, le16(payload));
            break;
        case 0x08: /* shortened local name */
            if (!name_complete && payload_len) {
                sanitize_copy(out->name, sizeof(out->name), payload, payload_len);
                out->has_name = true;
            }
            break;
        case 0x09: /* complete local name */
            if (payload_len) {
                sanitize_copy(out->name, sizeof(out->name), payload, payload_len);
                out->has_name = true;
                name_complete = true;
            }
            break;
        case 0xFF: /* manufacturer specific data */
            if (payload_len >= 2) out->company_id = le16(payload);
            break;
        default:
            break;
        }
        i += len + 1;
    }
    return 0;
}

/* ----- table ------------------------------------------------------------- */
void survey_table_init(survey_table_t *table, survey_entry_t *rows, uint16_t capacity)
{
    table->rows = rows;
    table->capacity = capacity;
    survey_table_clear(table);
}

void survey_table_clear(survey_table_t *table)
{
    table->count = 0;
    table->overflowed = false;
}

survey_entry_t *survey_table_locate(survey_table_t *table, uint8_t source, const uint8_t addr[6])
{
    for (uint16_t i = 0; i < table->count; i++) {
        survey_entry_t *e = &table->rows[i];
        if (e->source == source && memcmp(e->addr, addr, 6) == 0) return e;
    }
    if (table->count >= table->capacity) {
        table->overflowed = true;
        return NULL;
    }
    survey_entry_t *e = &table->rows[table->count++];
    memset(e, 0, sizeof(*e));
    e->source = source;
    memcpy(e->addr, addr, 6);
    e->rssi_best = INT8_MIN;
    e->company_id = SURVEY_NO_COMPANY;
    return e;
}

void survey_addr_from_le(const uint8_t le[6], uint8_t out_msb_first[6])
{
    for (int i = 0; i < 6; i++) out_msb_first[i] = le[5 - i];
}

static void touch(survey_entry_t *e, int8_t rssi, int64_t now)
{
    e->rssi_last = rssi;
    if (rssi > e->rssi_best) e->rssi_best = rssi;
    if (e->hits == 0) e->first_seen = now;
    e->last_seen = now;
    if (e->hits < UINT16_MAX) e->hits++;
}

void survey_merge_ble(survey_table_t *table, const uint8_t addr_msb_first[6], uint8_t addr_type,
                      int8_t rssi, const survey_adv_info_t *info, int64_t now)
{
    survey_entry_t *e = survey_table_locate(table, SURVEY_SRC_BLE, addr_msb_first);
    if (!e) return;
    e->addr_type = addr_type;
    if (info) {
        if (info->esl) {
            e->flags |= SURVEY_F_ESL;
            e->service16 = SURVEY_ESL_SERVICE_UUID;
        } else if (info->service16 && !e->service16) {
            e->service16 = info->service16;
        }
        if (info->company_id != SURVEY_NO_COMPANY) e->company_id = info->company_id;
        if (info->has_name && !e->name[0]) {
            memcpy(e->name, info->name, sizeof(e->name));
            e->flags |= SURVEY_F_NAMED;
        }
    }
    touch(e, rssi, now);
}

void survey_merge_wifi(survey_table_t *table, const uint8_t bssid[6], int8_t rssi,
                       uint8_t channel, uint8_t authmode, const char *ssid,
                       unsigned profile, int64_t now)
{
    survey_entry_t *e = survey_table_locate(table, SURVEY_SRC_WIFI, bssid);
    if (!e) return;
    e->addr_type = SURVEY_ADDR_TYPE_WIFI;
    e->channel = channel;
    e->authmode = authmode;
    e->flags |= SURVEY_F_WIFI;
    if (ssid && ssid[0]) {
        sanitize_copy(e->name, sizeof(e->name), (const uint8_t *)ssid, strlen(ssid));
        e->flags |= SURVEY_F_NAMED;
    }
    if (survey_profile_ssid_hit(profile, e->name)) e->flags |= SURVEY_F_PROFILE;
    else e->flags &= (uint8_t)~SURVEY_F_PROFILE;
    touch(e, rssi, now);
}

/* ----- profiles ---------------------------------------------------------- */
unsigned survey_profile_count(void)
{
    return PROFILE_COUNT;
}

const char *survey_profile_name(unsigned profile)
{
    return profile < PROFILE_COUNT ? PROFILES[profile].name : "?";
}

bool survey_profile_ssid_hit(unsigned profile, const char *ssid)
{
    if (profile == 0 || profile >= PROFILE_COUNT || !ssid || !ssid[0]) return false;
    char low[SURVEY_NAME_LEN];
    size_t i = 0;
    for (; ssid[i] && i < sizeof(low) - 1; i++) {
        char c = ssid[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[i] = '\0';
    for (size_t h = 0; h < 4 && PROFILES[profile].hints[h]; h++)
        if (strstr(low, PROFILES[profile].hints[h])) return true;
    return false;
}

void survey_reapply_profile(survey_table_t *table, unsigned profile)
{
    for (uint16_t i = 0; i < table->count; i++) {
        survey_entry_t *e = &table->rows[i];
        bool hit = e->source == SURVEY_SRC_WIFI && survey_profile_ssid_hit(profile, e->name);
        if (hit) e->flags |= SURVEY_F_PROFILE;
        else e->flags &= (uint8_t)~SURVEY_F_PROFILE;
    }
}

/* ----- labels ------------------------------------------------------------ */
const char *survey_company_name(uint16_t company_id)
{
    for (size_t i = 0; i < COMPANY_COUNT; i++)
        if (COMPANIES[i].id == company_id) return COMPANIES[i].name;
    return NULL;
}

const char *survey_auth_name(uint8_t authmode)
{
    /* esp_wifi wifi_auth_mode_t numbering (verified against ESP-IDF 5.4.2). */
    static const char *const names[] = {
        "open", "wep", "wpa", "wpa2", "wpa/wpa2", "eap", "wpa3", "wpa2/wpa3",
        "wapi", "owe", "wpa3-192", "wpa3", "wpa3", "dpp", "wpa3-eap", "wpa2/3-eap",
    };
    return authmode < sizeof(names) / sizeof(names[0]) ? names[authmode] : "?";
}

const char *survey_vendor_label(const survey_entry_t *entry)
{
    if (entry->flags & SURVEY_F_ESL) return "ESL tag";
    if (entry->source == SURVEY_SRC_WIFI) return "Wi-Fi AP";
    const char *company = survey_company_name(entry->company_id);
    return company ? company : "";
}

void survey_flags_str(uint8_t flags, char out[5])
{
    int i = 0;
    if (flags & SURVEY_F_ESL) out[i++] = 'E';
    if (flags & SURVEY_F_PROFILE) out[i++] = 'P';
    if (flags & SURVEY_F_WIFI) out[i++] = 'W';
    if (flags & SURVEY_F_NAMED) out[i++] = 'N';
    out[i] = '\0';
}

static int rank(const survey_entry_t *e)
{
    if (e->flags & SURVEY_F_ESL) return 2;
    if (e->flags & SURVEY_F_PROFILE) return 1;
    return 0;
}

int survey_compare(const void *a, const void *b)
{
    const survey_entry_t *x = a;
    const survey_entry_t *y = b;
    int rx = rank(x), ry = rank(y);
    if (rx != ry) return ry - rx;
    if (x->rssi_best != y->rssi_best) return (int)y->rssi_best - (int)x->rssi_best;
    if (x->source != y->source) return (int)x->source - (int)y->source;
    return memcmp(x->addr, y->addr, 6);
}

/* ----- CSV --------------------------------------------------------------- */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} appender_t;

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void put(appender_t *a, const char *fmt, ...)
{
    if (a->overflow) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(a->buf + a->len, a->cap - a->len, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= a->cap - a->len) {
        a->overflow = true;
        return;
    }
    a->len += (size_t)n;
}

static void put_time(appender_t *a, int64_t seconds)
{
    time_t t = (time_t)seconds;
    struct tm *tm = gmtime(&t);
    char stamp[24];
    if (!tm || strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", tm) == 0) stamp[0] = '\0';
    put(a, "%s", stamp);
}

int survey_csv_header(char *buffer, size_t capacity)
{
    appender_t a = {buffer, capacity, 0, capacity == 0};
    put(&a, "first_utc,last_utc,source,address,addr_type,rssi_best,rssi_last,hits,"
            "flags,channel,auth,company_id,service16,vendor,name\n");
    return a.overflow ? -1 : (int)a.len;
}

int survey_csv_row(const survey_entry_t *e, char *buffer, size_t capacity)
{
    appender_t a = {buffer, capacity, 0, capacity == 0};
    bool wifi = e->source == SURVEY_SRC_WIFI;
    char flags[5];
    survey_flags_str(e->flags, flags);

    put_time(&a, e->first_seen);
    put(&a, ",");
    put_time(&a, e->last_seen);
    put(&a, ",%s,%02X:%02X:%02X:%02X:%02X:%02X,", wifi ? "WIFI" : "BLE",
        e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
    if (!wifi) put(&a, "%u", (unsigned)e->addr_type);
    put(&a, ",%d,%d,%u,%s,", (int)e->rssi_best, (int)e->rssi_last, (unsigned)e->hits, flags);
    if (wifi) put(&a, "%u,%s", (unsigned)e->channel, survey_auth_name(e->authmode));
    else put(&a, ",");
    put(&a, ",");
    if (!wifi && e->company_id != SURVEY_NO_COMPANY) put(&a, "0x%04X", (unsigned)e->company_id);
    put(&a, ",");
    if (e->service16) put(&a, "0x%04X", (unsigned)e->service16);
    put(&a, ",%s,\"", survey_vendor_label(e));
    for (const char *p = e->name; *p; p++) {
        if (*p == '"') put(&a, "\"\"");
        else put(&a, "%c", *p);
    }
    put(&a, "\"\n");
    return a.overflow ? -1 : (int)a.len;
}
