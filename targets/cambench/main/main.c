/* Target cambench, S01 bench.
 *
 * Brings up the PAJ7025R2 and prints the objects it sees. See cgcam.h for
 * the wiring, the voltage limits and the three unusual things about the bus,
 * and D-015 for where the register map came from.
 *
 * S01-B01 task 5 says to stop and report if the module does not answer the
 * product ID register, with the wiring and the logic level checked. That is
 * what happens: the checklist is printed and the frame loop never starts.
 *
 * Flash method, D-004:
 *   tools\cgflash.ps1 cambench COM6 build flash monitor
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "cgcam.h"
#include "cgoled.h"

static const char *TAG = "cambench";

static struct {
    uint16_t product_id;
    uint32_t frame_period_us;
    uint32_t frames;
    uint32_t frames_with_objects;
    int last_count;
    int max_count;
} g;

static void report_silence(void)
{
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "STOP: the module did not answer with the product id 0x7025.");
    ESP_LOGE(TAG, "It read back 0x%04X instead.", g.product_id);
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
    ESP_LOGE(TAG, "If all seven check out, the bus settings are the next suspect.");
    ESP_LOGE(TAG, "This build uses SPI mode 3, LSB first, and a command byte of");
    ESP_LOGE(TAG, "0x80 before the register for a read. All three have to be");
    ESP_LOGE(TAG, "right together or nothing answers. See D-015.");
    ESP_LOGE(TAG, "");

    if (cgoled_present()) {
        cgoled_clear();
        cgoled_text(0, 0, "CYB3RGUN CAMBENCH");
        cgoled_rule(1);
        cgoled_text(0, 2, "PAJ7025R2 SILENT");
        cgoled_printf(0, 3, "READ 0x%04X", g.product_id);
        cgoled_text(0, 5, "SEE THE SERIAL");
        cgoled_text(0, 6, "LOG FOR THE LIST");
        cgoled_flush();
    }
}

static void bench_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_CGCAM_FPS);
    TickType_t next = xTaskGetTickCount();
    int64_t last_print = 0;
    cgcam_frame_t frame;

    for (;;) {
        vTaskDelayUntil(&next, (period > 0) ? period : 1);

        if (cgcam_read_frame(&frame, CGCAM_FORMAT_1) != ESP_OK) {
            continue;
        }
        g.frames++;
        g.last_count = frame.count;
        if (frame.count > 0) {
            g.frames_with_objects++;
        }
        if (frame.count > g.max_count) {
            g.max_count = frame.count;
        }

        /* The loop runs at the full rate so the timing is real; only the
         * printing is thinned out, or the port becomes the bottleneck. */
        const int64_t now = esp_timer_get_time();
        if (now - last_print < 1000000 / CONFIG_CGCAM_PRINT_HZ) {
            continue;
        }
        last_print = now;

        char line[256];
        int at = snprintf(line, sizeof(line), "objects %d:", frame.count);
        for (int i = 0; i < frame.count && at < (int)sizeof(line) - 24; i++) {
            const cgcam_object_t *o = &frame.object[i];
            at += snprintf(&line[at], sizeof(line) - (size_t)at, " [%u,%u a=%u b=%u]", o->cx,
                           o->cy, o->area, o->max_brightness);
        }
        ESP_LOGI(TAG, "%s", line);

        /* When nothing is in view, say what the sensor actually returned.
         * A report that is entirely one value means nothing reached the
         * sensor; a report with structure in it means the parse above threw
         * away something real, and those are very different faults. */
        if (frame.count == 0) {
            uint8_t raw[256];
            if (cgcam_read_report(raw, CGCAM_FORMAT_1) == ESP_OK) {
                int nonzero = 0;
                uint8_t high = 0;
                for (size_t i = 0; i < sizeof(raw); i++) {
                    if (raw[i] != 0) {
                        nonzero++;
                    }
                    if (raw[i] > high) {
                        high = raw[i];
                    }
                }
                ESP_LOGI(TAG,
                         "  raw: %d of 256 bytes non zero, highest 0x%02X, slot0 %02X %02X "
                         "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                         "%02X",
                         nonzero, high, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                         raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13],
                         raw[14], raw[15]);
            }
        }

        if (cgoled_present()) {
            cgoled_clear();
            cgoled_text(0, 0, "CYB3RGUN CAMBENCH");
            cgoled_rule(1);
            cgoled_printf(0, 2, "OBJECTS %d", frame.count);
            cgoled_printf(0, 3, "MAX SEEN %d", g.max_count);
            if (frame.count > 0) {
                cgoled_printf(0, 4, "X %u Y %u", frame.object[0].cx, frame.object[0].cy);
                cgoled_printf(0, 5, "AREA %u", frame.object[0].area);
            } else {
                cgoled_text(0, 4, "NOTHING IN VIEW");
            }
            cgoled_printf(0, 6, "FRAMES %" PRIu32, g.frames);
            cgoled_printf(0, 7, "ID %04X %" PRIu32 "US", g.product_id, g.frame_period_us);
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
    if (cgoled_present()) {
        cgoled_clear();
        cgoled_text(0, 0, "CYB3RGUN CAMBENCH");
        cgoled_rule(1);
        cgoled_text(0, 2, "STARTING...");
        cgoled_flush();
    }

    cgcam_config_t cam = CGCAM_HELTEC_V2_CONFIG();
    cam.clock_hz = CONFIG_CGCAM_CLOCK_HZ;
    ESP_ERROR_CHECK(cgcam_init(&cam));

    const esp_err_t id_err = cgcam_product_id(&g.product_id);

    /* A silent bus reads 0x0000 for several different faults. An internal
     * pull up on MISO tells two of them apart: if the line is floating,
     * because a wire is off or the module is unpowered, the reading turns
     * to 0xFFFF. If something is actively holding it down it stays at
     * 0x0000. One flash, and it says whether the module is there at all. */
    if (id_err != ESP_OK) {
        uint16_t pulled = 0;
        gpio_set_pull_mode((gpio_num_t)cam.miso_gpio, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        cgcam_product_id(&pulled);
        ESP_LOGW(TAG, "MISO test: 0x%04X without a pull up, 0x%04X with one",
                 g.product_id, pulled);
        if (pulled == 0xFFFF) {
            ESP_LOGW(TAG, "  the line floats: a wire is off, or the module has no power");
        } else if (pulled == 0x0000) {
            ESP_LOGW(TAG, "  the line is held down: the module is powered and driving it,");
            ESP_LOGW(TAG, "  or MISO is shorted to ground");
        } else {
            ESP_LOGW(TAG, "  the line answers something: the module is alive, the bus is not");
        }
        gpio_set_pull_mode((gpio_num_t)cam.miso_gpio, GPIO_FLOATING);
    }

    if (id_err != ESP_OK) {
        /* Before giving up, ask every bank, in case the map moved. */
        uint8_t bank = 0;
        if (cgcam_probe(&bank) > 0) {
            ESP_LOGW(TAG, "the id is in bank %u, not where it was expected", bank);
        } else {
            report_silence();
            /* Stop, as the briefing asks. The board stays up so the log can
             * be read and the display stays on. */
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    ESP_LOGI(TAG, "PAJ7025R2 answered, product id 0x%04X", g.product_id);

    /* What the sensor is actually set to. A report of nothing can mean a
     * dark room or a sensor told to report nothing, and these tell the two
     * apart. Register addresses from D-015. */
    {
        uint8_t area_lo = 0, area_hi = 0, noise = 0, max_objects = 0;
        uint8_t gain1 = 0, gain2 = 0, exp_lo = 0, exp_hi = 0;

        cgcam_read_reg(0x00, 0x0B, &area_lo);
        cgcam_read_reg(0x00, 0x0C, &area_hi);
        cgcam_read_reg(0x00, 0x0F, &noise);
        cgcam_read_reg(0x00, 0x19, &max_objects);
        cgcam_read_reg(0x01, 0x05, &gain1);
        cgcam_read_reg(0x01, 0x06, &gain2);
        cgcam_read_reg(0x01, 0x0E, &exp_lo);
        cgcam_read_reg(0x01, 0x0F, &exp_hi);

        ESP_LOGI(TAG, "settings: max objects %u, area max threshold 0x%04X, noise 0x%02X",
                 max_objects, (unsigned)((area_hi << 8) | area_lo), noise);
        ESP_LOGI(TAG, "settings: gain 0x%02X 0x%02X, exposure 0x%04X", gain1, gain2,
                 (unsigned)((exp_hi << 8) | exp_lo));

        if (max_objects == 0) {
            ESP_LOGE(TAG, "max objects is 0: the sensor is set to report nothing,");
            ESP_LOGE(TAG, "whatever is in front of it. That is the fault, not the room.");
        }
    }

    if (cgcam_frame_period_us(&g.frame_period_us) == ESP_OK) {
        ESP_LOGI(TAG, "sensor frame period %" PRIu32 " us, about %" PRIu32 " fps",
                 g.frame_period_us,
                 (g.frame_period_us > 0) ? (1000000u / g.frame_period_us) : 0u);
    }

    ESP_LOGI(TAG, "reading format 1, 16 objects, at %d fps, printing %d times a second",
             CONFIG_CGCAM_FPS, CONFIG_CGCAM_PRINT_HZ);
    ESP_LOGI(TAG, "coordinates are 0 to 4095 on both axes");

    xTaskCreate(bench_task, "cg_bench", 4096, NULL, 6, NULL);
}
