#include "survey_data.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fields_outside_quotes(const char *line)
{
    int fields = 1;
    bool quoted = false;
    for (const char *p = line; *p && *p != '\n'; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == ',' && !quoted) fields++;
    }
    return fields;
}

static survey_adv_info_t parse_ok(const uint8_t *data, size_t length)
{
    survey_adv_info_t info;
    assert(survey_parse_adv(data, length, &info) == 0);
    return info;
}

static void test_parse(void)
{
    /* Unassociated ESL: flags + complete 16-bit UUID list with 0x1857. */
    const uint8_t esl[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0x57, 0x18};
    survey_adv_info_t info = parse_ok(esl, sizeof(esl));
    assert(info.esl && info.service16 == SURVEY_ESL_SERVICE_UUID);
    assert(info.company_id == SURVEY_NO_COMPANY && !info.has_name);

    /* ESL after another UUID still wins the notable-service slot. */
    const uint8_t esl_second[] = {0x05, 0x03, 0x0F, 0x18, 0x57, 0x18};
    info = parse_ok(esl_second, sizeof(esl_second));
    assert(info.esl && info.service16 == SURVEY_ESL_SERVICE_UUID);

    /* Plain battery service: not an ESL. */
    const uint8_t battery[] = {0x03, 0x02, 0x0F, 0x18};
    info = parse_ok(battery, sizeof(battery));
    assert(!info.esl && info.service16 == 0x180F);

    /* ESL announced via 16-bit service data. */
    const uint8_t esl_data[] = {0x05, 0x16, 0x57, 0x18, 0xAA, 0xBB};
    info = parse_ok(esl_data, sizeof(esl_data));
    assert(info.esl);

    /* Manufacturer data: Apple company ID, little endian. */
    const uint8_t apple[] = {0x05, 0xFF, 0x4C, 0x00, 0x02, 0x15};
    info = parse_ok(apple, sizeof(apple));
    assert(info.company_id == 0x004C);
    assert(strcmp(survey_company_name(info.company_id), "Apple") == 0);

    /* Complete name beats shortened name, in either order. */
    const uint8_t short_then_full[] = {0x03, 0x08, 'A', 'B', 0x05, 0x09, 'A', 'B', 'C', 'D'};
    info = parse_ok(short_then_full, sizeof(short_then_full));
    assert(info.has_name && strcmp(info.name, "ABCD") == 0);
    const uint8_t full_then_short[] = {0x05, 0x09, 'A', 'B', 'C', 'D', 0x03, 0x08, 'A', 'B'};
    info = parse_ok(full_then_short, sizeof(full_then_short));
    assert(strcmp(info.name, "ABCD") == 0);

    /* Non-printable bytes are sanitized (protects the UI and CSV). */
    const uint8_t dirty[] = {0x06, 0x09, 'A', 0x01, '"', '\n', 'B'};
    info = parse_ok(dirty, sizeof(dirty));
    assert(strcmp(info.name, "A?\"?B") == 0);

    /* Over-long names are truncated, not overflowed. */
    uint8_t long_name[2 + 60];
    long_name[0] = 61;
    long_name[1] = 0x09;
    memset(&long_name[2], 'X', 60);
    info = parse_ok(long_name, sizeof(long_name));
    assert(strlen(info.name) == SURVEY_NAME_LEN - 1);

    /* Odd-length UUID list reads only complete UUIDs. */
    const uint8_t odd[] = {0x04, 0x03, 0x0F, 0x18, 0x57};
    info = parse_ok(odd, sizeof(odd));
    assert(!info.esl && info.service16 == 0x180F);

    /* Malformed: length runs past the payload. */
    const uint8_t overrun[] = {0x05, 0x09, 'A'};
    assert(survey_parse_adv(overrun, sizeof(overrun), &info) == -1);

    /* Zero padding terminates cleanly; empty / NULL inputs. */
    const uint8_t padded[] = {0x02, 0x01, 0x06, 0x00, 0x00};
    info = parse_ok(padded, sizeof(padded));
    assert(survey_parse_adv(NULL, 0, &info) == 0);
    assert(survey_parse_adv(NULL, 3, &info) == -1);
    assert(survey_parse_adv(esl, sizeof(esl), NULL) == -1);
}

static void test_table_and_merge(void)
{
    survey_entry_t rows[2];
    survey_table_t table;
    survey_table_init(&table, rows, 2);

    const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t b[6] = {6, 5, 4, 3, 2, 1};
    const uint8_t c[6] = {9, 9, 9, 9, 9, 9};

    survey_entry_t *ea = survey_table_locate(&table, SURVEY_SRC_BLE, a);
    assert(ea && table.count == 1);
    assert(survey_table_locate(&table, SURVEY_SRC_BLE, a) == ea);
    /* Same address on another radio is a different device. */
    survey_entry_t *wa = survey_table_locate(&table, SURVEY_SRC_WIFI, a);
    assert(wa && wa != ea && table.count == 2);
    assert(survey_table_locate(&table, SURVEY_SRC_BLE, b) == NULL && table.overflowed);
    (void)c;

    survey_table_init(&table, rows, 2);
    assert(table.count == 0 && !table.overflowed);

    uint8_t le[6] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, msb[6];
    survey_addr_from_le(le, msb);
    const uint8_t want[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    assert(memcmp(msb, want, 6) == 0);

    const uint8_t esl_adv[] = {0x03, 0x03, 0x57, 0x18, 0x04, 0x09, 'E', 'S', 'L'};
    survey_adv_info_t info = parse_ok(esl_adv, sizeof(esl_adv));
    survey_merge_ble(&table, msb, 1, -70, &info, 1000);
    survey_entry_t *e = &rows[0];
    assert(e->hits == 1 && e->first_seen == 1000 && e->last_seen == 1000);
    assert(e->rssi_best == -70 && e->rssi_last == -70 && e->addr_type == 1);
    assert((e->flags & SURVEY_F_ESL) && (e->flags & SURVEY_F_NAMED));
    assert(strcmp(e->name, "ESL") == 0);

    /* Later plain advert: ESL flag and first name are sticky, RSSI tracked. */
    const uint8_t plain[] = {0x05, 0xFF, 0x34, 0x12, 0x00, 0x00, 0x04, 0x09, 'X', 'Y', 'Z'};
    info = parse_ok(plain, sizeof(plain));
    survey_merge_ble(&table, msb, 1, -50, &info, 1005);
    assert(e->hits == 2 && e->first_seen == 1000 && e->last_seen == 1005);
    assert(e->rssi_best == -50 && e->rssi_last == -50);
    survey_merge_ble(&table, msb, 1, -90, &info, 1010);
    assert(e->rssi_best == -50 && e->rssi_last == -90 && e->hits == 3);
    assert((e->flags & SURVEY_F_ESL) && e->service16 == SURVEY_ESL_SERVICE_UUID);
    assert(strcmp(e->name, "ESL") == 0 && e->company_id == 0x1234);
    assert(table.count == 1);

    /* Hit counter saturates instead of wrapping. */
    e->hits = UINT16_MAX;
    survey_merge_ble(&table, msb, 1, -60, NULL, 1020);
    assert(e->hits == UINT16_MAX);
}

static void test_wifi_and_profiles(void)
{
    survey_entry_t rows[4];
    survey_table_t table;
    survey_table_init(&table, rows, 4);

    unsigned walmart = 0, costco = 0, sams = 0;
    for (unsigned i = 0; i < survey_profile_count(); i++) {
        if (!strcmp(survey_profile_name(i), "Walmart")) walmart = i;
        if (!strcmp(survey_profile_name(i), "Costco")) costco = i;
        if (!strcmp(survey_profile_name(i), "Sam's Club")) sams = i;
    }
    assert(walmart && costco && sams);
    assert(!strcmp(survey_profile_name(0), "Generic"));
    assert(!strcmp(survey_profile_name(999), "?"));

    assert(survey_profile_ssid_hit(walmart, "WALMART-Guest"));
    assert(!survey_profile_ssid_hit(0, "Walmart"));          /* generic: ESL only */
    assert(!survey_profile_ssid_hit(walmart, ""));
    assert(!survey_profile_ssid_hit(walmart, NULL));
    assert(!survey_profile_ssid_hit(999, "walmart"));
    assert(survey_profile_ssid_hit(sams, "Sams Club WiFi"));
    assert(!survey_profile_ssid_hit(sams, "Samsung Galaxy S25")); /* no false hit */

    const uint8_t ap1[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
    const uint8_t ap2[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x02};
    survey_merge_wifi(&table, ap1, -40, 6, 3, "Walmart WiFi", walmart, 2000);
    survey_merge_wifi(&table, ap2, -60, 11, 0, "", walmart, 2000);
    survey_entry_t *w1 = &rows[0], *w2 = &rows[1];
    assert(w1->addr_type == SURVEY_ADDR_TYPE_WIFI && w1->channel == 6);
    assert((w1->flags & SURVEY_F_PROFILE) && (w1->flags & SURVEY_F_WIFI));
    assert(!(w2->flags & SURVEY_F_PROFILE) && !(w2->flags & SURVEY_F_NAMED));
    assert(!strcmp(survey_vendor_label(w1), "Wi-Fi AP"));

    survey_reapply_profile(&table, costco);
    assert(!(w1->flags & SURVEY_F_PROFILE));
    survey_reapply_profile(&table, walmart);
    assert(w1->flags & SURVEY_F_PROFILE);

    /* Re-seen under a different profile clears the stale highlight. */
    survey_merge_wifi(&table, ap1, -45, 6, 3, "Walmart WiFi", costco, 2010);
    assert(!(w1->flags & SURVEY_F_PROFILE) && w1->hits == 2 && w1->rssi_best == -40);
}

static void test_sort(void)
{
    survey_entry_t rows[3];
    memset(rows, 0, sizeof(rows));
    rows[0].rssi_best = -30;                       /* plain, strong     */
    rows[1].rssi_best = -90; rows[1].flags = SURVEY_F_ESL;     /* ESL, weak */
    rows[2].rssi_best = -20; rows[2].flags = SURVEY_F_PROFILE; /* profile   */
    qsort(rows, 3, sizeof(rows[0]), survey_compare);
    assert(rows[0].flags & SURVEY_F_ESL);
    assert(rows[1].flags & SURVEY_F_PROFILE);
    assert(rows[2].rssi_best == -30);
}

static void test_csv(void)
{
    char line[512];
    int n = survey_csv_header(line, sizeof(line));
    assert(n > 0 && line[n - 1] == '\n');
    assert(fields_outside_quotes(line) == 15);

    survey_entry_t ble;
    memset(&ble, 0, sizeof(ble));
    ble.source = SURVEY_SRC_BLE;
    const uint8_t addr[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    memcpy(ble.addr, addr, 6);
    ble.addr_type = 1;
    ble.rssi_best = -55;
    ble.rssi_last = -60;
    ble.hits = 7;
    ble.flags = SURVEY_F_ESL | SURVEY_F_NAMED;
    ble.company_id = 0x004C;
    ble.service16 = SURVEY_ESL_SERVICE_UUID;
    ble.first_seen = 0;
    ble.last_seen = 86400;
    strcpy(ble.name, "Aisle 7, \"Dairy\"");
    n = survey_csv_row(&ble, line, sizeof(line));
    assert(n > 0 && fields_outside_quotes(line) == 15);
    assert(strstr(line, "1970-01-01T00:00:00Z,1970-01-02T00:00:00Z,BLE,DE:AD:BE:EF:00:01,1,-55,-60,7,EN,,,0x004C,0x1857,ESL tag,") == line);
    assert(strstr(line, "\"Aisle 7, \"\"Dairy\"\"\"\n"));

    survey_entry_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.source = SURVEY_SRC_WIFI;
    memcpy(ap.addr, addr, 6);
    ap.addr_type = SURVEY_ADDR_TYPE_WIFI;
    ap.rssi_best = -40;
    ap.rssi_last = -41;
    ap.hits = 1;
    ap.flags = SURVEY_F_WIFI | SURVEY_F_PROFILE | SURVEY_F_NAMED;
    ap.channel = 11;
    ap.authmode = 3;
    ap.company_id = SURVEY_NO_COMPANY;
    strcpy(ap.name, "Walmart WiFi");
    n = survey_csv_row(&ap, line, sizeof(line));
    assert(n > 0 && fields_outside_quotes(line) == 15);
    assert(strstr(line, ",WIFI,DE:AD:BE:EF:00:01,,-40,-41,1,PWN,11,wpa2,,,Wi-Fi AP,\"Walmart WiFi\"\n"));

    /* Too-small buffers report failure instead of writing a partial row. */
    assert(survey_csv_row(&ap, line, 10) == -1);
    assert(survey_csv_header(line, 5) == -1);
    assert(survey_csv_header(line, 0) == -1);

    char flags[5];
    survey_flags_str(SURVEY_F_ESL | SURVEY_F_PROFILE | SURVEY_F_WIFI | SURVEY_F_NAMED, flags);
    assert(!strcmp(flags, "EPWN"));
    survey_flags_str(0, flags);
    assert(!strcmp(flags, ""));
    assert(!strcmp(survey_auth_name(0), "open") && !strcmp(survey_auth_name(7), "wpa2/wpa3"));
    assert(!strcmp(survey_auth_name(200), "?"));
}

int main(void)
{
    test_parse();
    test_table_and_merge();
    test_wifi_and_profiles();
    test_sort();
    test_csv();
    puts("Survey data: AD parsing, ESL 0x1857, table/merge, profiles, sort, CSV PASS");
    return 0;
}
