/* The web browser in Cybiko mode (see cybiko_browser.h). */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include "cybiko_browser.h"
#include "cybiko_app.h"
#include "cybiko_link.h"
#include "cyb_net_esp.h"
#include "m5_sd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "cy_browser";

#define DATA_DIR CYBIKO_SD_DIR "/browser"   /* bookmarks and settings */
#define APP_NAME "Browser.app"

static cyb_t *browser;
static volatile bool requested;

void cybiko_browser_install(uint8_t *disk, uint32_t size)
{
    cy_disk_file_t f;
    if (cy_disk_find(disk, size, APP_NAME, &f) && f.size == cyb_launcher_app_size) {
        uint8_t *have = malloc(cyb_launcher_app_size);
        bool same = have && cy_disk_read(disk, size, APP_NAME, have, (uint32_t)cyb_launcher_app_size) ==
                                (int32_t)cyb_launcher_app_size &&
                    !memcmp(have, cyb_launcher_app, cyb_launcher_app_size);
        free(have);
        if (same) return;
    }
    uint32_t date = (uint32_t)((int64_t)time(NULL) + 2208988800LL);   /* 1970 -> 1900 */
    if (cy_disk_write(disk, size, APP_NAME, cyb_launcher_app, (uint32_t)cyb_launcher_app_size, date) == 0)
        ESP_LOGI(TAG, "put %s on the RAM disk", APP_NAME);
    else
        ESP_LOGW(TAG, "no room for %s on the RAM disk", APP_NAME);
}

uint32_t cybiko_browser_app_call(void *user, uint32_t fn, uint32_t arg)
{
    (void)user;
    (void)arg;
    switch (fn) {
    case CY_APP_VERSION: return CY_APP_API;
    case CY_APP_BROWSER: requested = true; return 1;
    default: return 0;
    }
}

bool cybiko_browser_requested(void)
{
    bool r = requested;
    requested = false;
    return r;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* The browser runs in a task of its own: pages are parsed with a deeper stack
 * than the CPU task has. Its stack may be in PSRAM (it never writes flash;
 * the Wi-Fi networks are saved on the CPU task when it closes). */
#define BROWSER_STACK 16384

typedef struct {
    cybiko_t *m;
    bool (*next_key)(int *key, bool *down);
    SemaphoreHandle_t done;
} run_t;

static void browser_task(void *arg)
{
    run_t *r = arg;
    cybiko_t *m = r->m;
    uint8_t *fb = malloc(CYB_FB_SIZE);
    cyb_show(browser);
    for (;;) {
        int key;
        bool down;
        while (r->next_key(&key, &down)) {
            int k = cybiko_browser_key(key);
            if (k) cyb_key(browser, k, down);
            /* releases reach CyOS's keyboard too, so none stays held down */
            if (!down) cybiko_key(m, key, false);
        }
        if (!cyb_step(browser, now_ms())) break;
        if (fb && cyb_render(browser, fb)) cybiko_lcd_show(m, fb);
        vTaskDelay(pdMS_TO_TICKS(cyb_busy(browser) ? 5 : 20));
    }
    free(fb);
    xSemaphoreGive(r->done);
    vTaskSuspend(NULL);           /* deleted by the CPU task */
}

void cybiko_browser_run(cybiko_t *m, const uint8_t *flash, bool (*next_key)(int *key, bool *down))
{
    if (!browser) {
        static cyb_net_t net;
        cyb_net_esp(&net);
        mkdir(DATA_DIR, 0777);
        browser = cyb_create(&net, flash, CY_FLASH_SIZE, DATA_DIR);
        if (!browser) { ESP_LOGE(TAG, "out of memory"); return; }
    }
    ESP_LOGI(TAG, "open; free PSRAM %u, internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    run_t r = { m, next_key, xSemaphoreCreateBinary() };
    uint8_t *saved = malloc(sizeof m->lcd.ram);
    TaskHandle_t task = NULL;
    bool caps = false;
    if (!r.done || !saved) goto out;
    memcpy(saved, m->lcd.ram, sizeof m->lcd.ram);
    if (xTaskCreatePinnedToCore(browser_task, "browser", BROWSER_STACK, &r, 4, &task, 1) != pdPASS) {
        task = NULL;
        caps = xTaskCreatePinnedToCoreWithCaps(browser_task, "browser", BROWSER_STACK, &r, 4, &task, 1,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        if (!caps) task = NULL;
    }
    if (!task) { ESP_LOGE(TAG, "no memory for the browser task"); goto out; }
    xSemaphoreTake(r.done, portMAX_DELAY);
    if (caps) vTaskDeleteWithCaps(task);
    else vTaskDelete(task);
    memcpy(m->lcd.ram, saved, sizeof m->lcd.ram);
    m->lcd.version++;
out:
    free(saved);
    if (r.done) vSemaphoreDelete(r.done);
    cyb_net_esp_stop();
    cybiko_link_resume();
    ESP_LOGI(TAG, "closed");
}
