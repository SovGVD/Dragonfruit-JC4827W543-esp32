#include "palm_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "m5_sd.h"

static const char *TAG = "config";

#define CFG_PATH PALM_SD_DIR "/palm.cfg"

config_load_result_t config_read_file(const char *path, char *buf, size_t cap,
                                      size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (cap == 0) return CONFIG_TOO_BIG;
    buf[0] = '\0';

    /* Mount the shared microSD (display SPI bus); m5_sd retries the flaky first
     * transaction after the IT8951 has been driving the bus. */
    sdmmc_card_t *card = NULL;
    esp_err_t err = m5_sd_mount(&card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) reading %s", esp_err_to_name(err), path);
        return CONFIG_NO_SD;
    }

    config_load_result_t result = CONFIG_OK;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) {            /* genuinely absent — the expected case */
            ESP_LOGI(TAG, "%s not present", path);
            result = CONFIG_NO_FILE;
        } else {                          /* transient/other error — NOT "create me":
                                           * callers keep their current config */
            ESP_LOGW(TAG, "%s open failed: %s", path, strerror(errno));
            result = CONFIG_READ_ERR;
        }
        goto cleanup;
    }

    long sz;
    struct stat st;
    if (stat(path, &st) == 0) {
        sz = (long)st.st_size;
    } else {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
    }

    if (sz < 0 || (size_t)sz >= cap) {
        ESP_LOGE(TAG, "%s %ld bytes exceeds %u buffer", path, sz, (unsigned)cap);
        result = CONFIG_TOO_BIG;
        fclose(f);
        goto cleanup;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        ESP_LOGE(TAG, "%s short read: %u of %ld bytes", path, (unsigned)rd, sz);
        result = CONFIG_READ_ERR;
        goto cleanup;
    }
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    ESP_LOGI(TAG, "read %s (%u bytes)", path, (unsigned)rd);

cleanup:
    m5_sd_unmount(card);
    return result;
}

config_load_result_t config_load_from_sd(char *buf, size_t cap, size_t *out_len)
{
    return config_read_file(CFG_PATH, buf, cap, out_len);
}

bool config_get_value(const char *text, const char *key, char *out, size_t cap)
{
    if (!text || !key || !out || cap == 0) return false;
    size_t klen = strlen(key);
    for (const char *p = text; p && *p; ) {
        const char *eol = strchr(p, '\n');
        const char *e = eol ? eol : p + strlen(p);
        const char *s = p;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        if (s < e && *s != '#' && *s != ';') {
            const char *eq = memchr(s, '=', (size_t)(e - s));
            if (eq) {
                const char *ke = eq;
                while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                if ((size_t)(ke - s) == klen && strncmp(s, key, klen) == 0) {
                    const char *vs = eq + 1, *ve = e;
                    while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
                    while (ve > vs && (ve[-1]==' '||ve[-1]=='\t'||ve[-1]=='\r')) ve--;
                    size_t n = (size_t)(ve - vs);
                    if (n >= cap) n = cap - 1;
                    memcpy(out, vs, n); out[n] = '\0';
                    return true;
                }
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return false;
}

config_load_result_t config_write_file(const char *path, const void *buf, size_t len)
{
    sdmmc_card_t *card = NULL;
    esp_err_t err = m5_sd_mount(&card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) writing %s", esp_err_to_name(err), path);
        return CONFIG_NO_SD;
    }

    config_load_result_t result = CONFIG_OK;
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "could not create %s", path);
        result = CONFIG_READ_ERR;
        goto cleanup;
    }

    size_t wr = fwrite(buf, 1, len, f);
    fclose(f);
    if (wr != len) {
        ESP_LOGE(TAG, "%s short write: %u of %u bytes", path, (unsigned)wr, (unsigned)len);
        remove(path);                 /* never leave a truncated file behind */
        result = CONFIG_READ_ERR;
        goto cleanup;
    }
    ESP_LOGI(TAG, "wrote %s (%u bytes)", path, (unsigned)len);

cleanup:
    m5_sd_unmount(card);
    return result;
}
