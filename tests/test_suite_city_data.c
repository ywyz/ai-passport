// Host test: province/city data table integrity and lookup logic.
// The picker UI assumes: 34 provinces, every province has 1..21 cities,
// names are unique per province and valid UTF-8 without whitespace, and
// coordinates fall inside China's bounding box. Guards the static row
// arrays in suite_city_picker.c and garbage-coordinate typos.
#include "suite_city_data.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                 \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

// Minimal UTF-8 acceptance: reject overlong/surrogate/out-of-range forms so
// mojibake or a truncated name cannot ship inside a screen label.
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        int extra = 0;
        unsigned int min_cp = 0, max_cp = 0;
        if (c < 0x80) {
            if (c < 0x20 && c != '\0') return false;  // control chars
            p++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            min_cp = 0x80;
            max_cp = 0x7FF;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            min_cp = 0x800;
            max_cp = 0xFFFF;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            min_cp = 0x10000;
            max_cp = 0x10FFFF;
        } else {
            return false;
        }
        unsigned int cp = 0;
        switch (extra) {
        case 1: cp = c & 0x1F; break;
        case 2: cp = c & 0x0F; break;
        case 3: cp = c & 0x07; break;
        }
        for (int i = 0; i < extra; i++) {
            if ((p[1 + i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[1 + i] & 0x3F);
        }
        if (cp < min_cp || cp > max_cp) return false;       // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;     // surrogate
        p += 1 + extra;
    }
    return true;
}

int main(void)
{
    // Province count: 34 province-level divisions.
    const int count = suite_city_province_count();
    CHECK(count == 34, "province count must be 34");

    // Bounds covering all of China incl. 南海诸岛 (三沙 ~16.8N).
    const int32_t lat_min = 150000, lat_max = 550000;
    const int32_t lon_min = 730000, lon_max = 1360000;

    int total_cities = 0;
    int max_cities = 0;
    for (int p = 0; p < count; p++) {
        const suite_province_def_t *prov = suite_city_province_at(p);
        CHECK(prov != NULL, "province_at in range must not be NULL");
        CHECK(prov->name != NULL && prov->name[0] != '\0',
              "province name must be non-empty");
        CHECK(utf8_valid(prov->name), "province name must be valid UTF-8");
        CHECK(prov->cities != NULL, "province must have a city array");
        CHECK(prov->city_count >= 1, "province must have at least one city");
        CHECK(prov->city_count <= 21,
              "province city count must fit CITY_ROWS_MAX (21)");
        if (prov->city_count > max_cities) max_cities = prov->city_count;
        total_cities += prov->city_count;

        for (int c = 0; c < prov->city_count; c++) {
            const suite_city_def_t *city = &prov->cities[c];
            CHECK(city->name != NULL && city->name[0] != '\0',
                  "city name must be non-empty");
            CHECK(utf8_valid(city->name), "city name must be valid UTF-8");
            CHECK(strchr(city->name, ' ') == NULL,
                  "city name must not contain spaces");
            CHECK(city->lat_x1e4 >= lat_min && city->lat_x1e4 <= lat_max,
                  "latitude out of China bounds");
            CHECK(city->lon_x1e4 >= lon_min && city->lon_x1e4 <= lon_max,
                  "longitude out of China bounds");

            // Uniqueness within the province.
            for (int d = c + 1; d < prov->city_count; d++) {
                CHECK(strcmp(city->name, prov->cities[d].name) != 0,
                      "duplicate city name within province");
            }
        }
    }
    CHECK(total_cities >= 320, "must cover at least 320 prefecture divisions");
    CHECK(max_cities == 21, "Guangdong must be the 21-city maximum");

    // Out-of-range access returns NULL.
    CHECK(suite_city_province_at(-1) == NULL, "negative index rejected");
    CHECK(suite_city_province_at(count) == NULL, "upper index rejected");

    // Lookup: known city, ambiguity, and miss.
    int p = -1, c = -1;
    CHECK(suite_city_find("北京", &p, &c), "北京 must be found");
    CHECK(p == 0 && c == 0, "北京 must resolve to province 0 city 0");
    CHECK(suite_city_find("苏州", &p, &c), "苏州 must be found");
    CHECK(strcmp(suite_city_province_at(p)->name, "江苏") == 0,
          "苏州 must belong to 江苏");
    CHECK(suite_city_find("西双版纳", &p, &c), "西双版纳 must be found");
    CHECK(strcmp(suite_city_province_at(p)->name, "云南") == 0,
          "西双版纳 must belong to 云南");
    CHECK(suite_city_find("不存在的城市", NULL, NULL) == false,
          "unknown city must not be found");
    CHECK(suite_city_find("", NULL, NULL) == false,
          "empty name must not be found");
    CHECK(suite_city_find(NULL, NULL, NULL) == false,
          "NULL name must not be found");

    if (g_failures == 0) {
        printf("test_suite_city_data: PASS (%d provinces, %d cities)\n",
               count, total_cities);
        return 0;
    }
    printf("test_suite_city_data: %d failure(s)\n", g_failures);
    return 1;
}
