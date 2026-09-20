/* Target cambench, S01 bench.
 *
 * Brings up the PAJ7025R2 on VSPI and reports what it finds. See cgcam.h for
 * the wiring, the voltage limits and the clock.
 *
 * S01-B01 task 5 says to stop and report if the module does not answer the
 * product ID register, with the wiring and the logic level checked. This
 * firmware does that: it sweeps the plausible transaction encodings and the
 * whole register space, and if 0x7025 never comes back it prints the
 * checklist and stops instead of pretending to see objects.
 *
 * Flash method, D-004:
 *   tools\cgflash.ps1 cambench COM8 build flash monitor
 */

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cgcam.h"
#include "cgoled.h"

static const char *TAG = "cambench";

#define PROBE_HITS_MAX 8

static struct {
    bool alive;
    uint16_t product_id;
    cgcam_probe_hit_t hits[PROBE_HITS_MAX];
    int hit_count;

    uint32_t frames;
    uint32_t changed; /* frames whose window differed from the one before */
} g;

static void show(const char *line3, const char *line4)
{
    if (!cgoled_present()) {
        return;
    }
    cgoled_clear();
    cgoled_text(0, 0, "CYB3RGUN CAMBENCH");
    cgoled_rule(1);
    cgoled_printf(0, 2, "PAJ7025R2 %s", g.alive ? "FOUND" : "SILENT");
    cgoled_text(0, 3, line3);
    cgoled_text(0, 4, line4);
    cgoled_flush();
}

static void report_silence(void)
{
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "STOP: the module never answered with the product id 0x7025.");
    ESP_LOGE(TAG, "None of the %d transaction encodings found it in any bank.",
             CGCAM_FMT_COUNT);
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "Check, in this order:");
    ESP_LOGE(TAG, " 1. VDDMA on pin 17 reads 3.3 V against pin 14. The part takes");
    ESP_LOGE(TAG, "    2.0 to 3.6 V and dies above 3.96 V. The 5 V beacon supply");
    ESP_LOGE(TAG, "    must not touch this module or any of its pins.");
    ESP_LOGE(TAG, " 2. Both grounds are wired: pin 14 VSSD and pin 20 VSSD_LED.");
    ESP_LOGE(TAG, "    The datasheet marks both as required, and one alone is a");
    ESP_LOGE(TAG, "    module that looks powered and answers nothing.");
    ESP_LOGE(TAG, " 3. 0.1 uF and 10 uF sit from VDDMA to GND as close to the");
    ESP_LOGE(TAG, "    module pins as the flying wires allow.");
    ESP_LOGE(TAG, " 4. The four signals go to the right pins and are not swapped:");
    ESP_LOGE(TAG, "    GPIO 21 to pin 10 CSB, GPIO 22 to pin 11 SCK,");
    ESP_LOGE(TAG, "    GPIO 17 from pin 12 MISO, GPIO 23 to pin 13 MOSI.");
    ESP_LOGE(TAG, "    MISO is the one that is easy to get backwards.");
    ESP_LOGE(TAG, " 5. Nothing else drives those four. LoRa holds 5, 18, 19, 27");
    ESP_LOGE(TAG, "    and the OLED holds 4, 15, 16, which is why the camera");
    ESP_LOGE(TAG, "    sits on 17, 21, 22 and 23 through the GPIO matrix.");
    ESP_LOGE(TAG, " 6. The wires are short. The pins drive 4 mA into 100 pF;");
    ESP_LOGE(TAG, "    this build clocks at %d Hz for that reason.", CONFIG_CGCAM_CLOCK_HZ);
    ESP_LOGE(TAG, " 7. Logic levels, with a scope on SCK: a high must clear");
    ESP_LOGE(TAG, "    0.7 x VDDMA, which is 2.31 V at 3.3 V. The ESP32 drives");
    ESP_LOGE(TAG, "    3.3 V, so a low reading means a wiring or a supply fault,");
    ESP_LOGE(TAG, "    not a level mismatch. No shifter is needed at 3.3 V.");
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "If all seven check out, the register map is the next suspect:");
    ESP_LOGE(TAG, "the datasheet in THEHARDWARE stops at page 20 and the SPI data");
    ESP_LOGE(TAG, "format is on page 26. See D-012.");
    ESP_LOGE(TAG, "");

    show("SEE THE SERIAL", "LOG FOR THE LIST");
}

static void bench_task(void *arg)
{
    (void)arg;

    uint8_t window[CONFIG_CGCAM_DUMP_LEN];
    uint8_t previous[CONFIG_CGCAM_DUMP_LEN];
    char line[3 * CONFIG_CGCAM_DUMP_LEN + 1];
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_CGCAM_FPS);

    memset(previous, 0, sizeof(previous));
    TickType_t next = xTaskGetTickCount();
    int64_t last_print = 0;

    for (;;) {
        vTaskDelayUntil(&next, (period > 0) ? period : 1);

        if (cgcam_burst_read(CONFIG_CGCAM_DUMP_REG, window, sizeof(window)) != ESP_OK) {
            continue;
        }
        g.frames++;
        if (memcmp(window, previous, sizeof(window)) != 0) {
            g.changed++;
            memcpy(previous, window, sizeof(window));
        }

        /* One line a second, not fifty. The frame loop runs at the full rate
         * so that the timing is real; only the printing is thinned out. */
        const int64_t now = esp_timer_get_time();
        if (now - last_print < 1000000) {
            continue;
        }
        last_print = now;

        int at = 0;
        for (size_t i = 0; i < sizeof(window); i++) {
            at += snprintf(&line[at], sizeof(line) - (size_t)at, "%02X ", window[i]);
        }
        ESP_LOGI(TAG, "frames %" PRIu32 " changed %" PRIu32 " | %s", g.frames, g.changed, line);

        if (cgoled_present()) {
            cgoled_clear();
            cgoled_text(0, 0, "CYB3RGUN CAMBENCH");
            cgoled_rule(1);
            cgoled_printf(0, 2, "ID %04X OK", g.product_id);
            cgoled_printf(0, 3, "FMT %d BANK %d", (int)cgcam_format(), g.hits[0].bank);
            cgoled_printf(0, 4, "REG %02X LEN %d", CONFIG_CGCAM_DUMP_REG,
                          CONFIG_CGCAM_DUMP_LEN);
            cgoled_printf(0, 5, "FRAMES %" PRIu32, g.frames);
            cgoled_printf(0, 6, "CHANGED %" PRIu32, g.changed);
            cgoled_text(0, 7, "OBJECTS: SEE D-012");
            cgoled_flush();
        }
    }
}

void app_main(void)
{
    const cgoled_config_t oled = CGOLED_HELTEC_V2_CONFIG();
    if (cgoled_init(&oled) != ESP_OK) {
        ESP_LOGW(TAG, "no OLED, carrying on without it");
    }
    show("PROBING...", "");

    cgcam_config_t cam = CGCAM_HELTEC_V2_CONFIG();
    cam.clock_hz = CONFIG_CGCAM_CLOCK_HZ;
    cam.spi_mode = CONFIG_CGCAM_SPI_MODE;
    ESP_ERROR_CHECK(cgcam_init(&cam));

    ESP_LOGI(TAG, "sweeping %d encodings over %d bank(s) and 256 registers",
             CGCAM_FMT_COUNT, CONFIG_CGCAM_PROBE_BANKS);
    g.hit_count = cgcam_probe(g.hits, PROBE_HITS_MAX, CONFIG_CGCAM_PROBE_BANKS);

    if (g.hit_count <= 0) {
        g.alive = false;
        report_silence();
        /* Stop, as the briefing asks. The board stays up so the log can be
         * read and the display stays on. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    g.alive = true;
    if (cgcam_product_id(&g.product_id) != ESP_OK) {
        ESP_LOGW(TAG, "the sweep found 0x7025 but the reread did not agree");
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "PAJ7025R2 answered. %d place(s) in the map carry 0x7025:", g.hit_count);
    for (int i = 0; i < g.hit_count && i < PROBE_HITS_MAX; i++) {
        ESP_LOGI(TAG, "  bank %u reg 0x%02X  %s  %s", g.hits[i].bank, g.hits[i].reg_low,
                 cgcam_format_name(g.hits[i].format),
                 g.hits[i].big_endian ? "big endian" : "little endian");
    }
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Put these lines in docs/measurements.md. They pin down the SPI");
    ESP_LOGI(TAG, "data format of datasheet section 6.1.2 and the product id");
    ESP_LOGI(TAG, "register of section 7.1.3, which the truncated copy in");
    ESP_LOGI(TAG, "THEHARDWARE does not contain. See D-012.");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Reading %d bytes from register 0x%02X at %d fps.", CONFIG_CGCAM_DUMP_LEN,
             CONFIG_CGCAM_DUMP_REG, CONFIG_CGCAM_FPS);
    ESP_LOGI(TAG, "Object count and coordinates need the output access map of");
    ESP_LOGI(TAG, "section 7.4, page 42, which is also past the end of the file.");

    xTaskCreate(bench_task, "cg_bench", 4096, NULL, 6, NULL);
}
