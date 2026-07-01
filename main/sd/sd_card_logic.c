#include "sd_card_logic.h"

#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_control.h"
#include "sdmmc_cmd.h"

#define PIN_SD_CLK      12
#define PIN_SD_CMD      11
#define PIN_SD_D0       10
#define PIN_CARD_DETECT 9
#define MOUNT_POINT     "/sdcard"

static sdmmc_card_t *mounted_card = NULL;
static bool is_mounted = false;
static SemaphoreHandle_t status_mutex = NULL;
static web_status_snapshot_t current_status = {
    .state = WEB_STATUS_STARTING,
    .bus = "SDMMC 1-bit",
    .last_error = "None",
};

static void status_lock(void)
{
    if (status_mutex != NULL) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (status_mutex != NULL) {
        xSemaphoreGive(status_mutex);
    }
}

static void clear_card_status_fields(web_status_snapshot_t *status)
{
    status->mounted = false;
    status->model[0] = '\0';
    status->manufacturer[0] = '\0';
    status->manufacturer_id = 0;
    status->oem_id = 0;
    status->revision = 0;
    status->serial = 0;
    status->manufacture_month = 0;
    status->manufacture_year = 0;
    status->full_cid[0] = '\0';
    status->capacity_bytes = 0;
    status->used_bytes = 0;
    status->free_bytes = 0;
    status->usage_percent = 0;
}

static void update_status_missing(void)
{
    status_lock();
    current_status.state = WEB_STATUS_CARD_MISSING;
    current_status.card_present = false;
    current_status.last_error = "None";
    clear_card_status_fields(&current_status);
    status_unlock();
}

static void update_status_mounting(void)
{
    status_lock();
    current_status.state = WEB_STATUS_MOUNTING;
    current_status.card_present = true;
    current_status.last_error = "None";
    clear_card_status_fields(&current_status);
    status_unlock();
}

static void update_status_mount_failed(esp_err_t error)
{
    status_lock();
    current_status.state = WEB_STATUS_MOUNT_FAILED;
    current_status.card_present = true;
    current_status.last_error = esp_err_to_name(error);
    clear_card_status_fields(&current_status);
    status_unlock();
}

static const char *manufacturer_name(uint8_t mfg_id)
{
    switch (mfg_id) {
        case 0x01: return "Panasonic";
        case 0x02: return "Toshiba / Kioxia";
        case 0x03: return "SanDisk / Western Digital";
        case 0x1B: return "Samsung";
        case 0x1D: return "ADATA";
        case 0x27: return "Phison";
        case 0x28: return "Lexar";
        case 0x31: return "Silicon Power";
        case 0x41: return "Kingston";
        case 0x74: return "Transcend";
        case 0x82: return "Sony";
        default:   return "Unknown";
    }
}

static uint8_t sd_crc7(const uint8_t *data, int len)
{
    uint8_t crc = 0;

    for (int i = 0; i < len; i++) {
        uint8_t d = data[i];

        for (int b = 0; b < 8; b++) {
            crc <<= 1;

            if (((d ^ crc) & 0x80) != 0) {
                crc ^= 0x09;
            }

            d <<= 1;
        }
    }

    return crc & 0x7F;
}

static void build_cid_string(const sdmmc_card_t *card, char *out, size_t out_size)
{
    uint8_t cid[16] = {0};

    cid[0] = card->cid.mfg_id & 0xFF;

    cid[1] = (card->cid.oem_id >> 8) & 0xFF;
    cid[2] = card->cid.oem_id & 0xFF;

    memcpy(&cid[3], card->cid.name, 5);

    cid[8] = card->cid.revision & 0xFF;

    cid[9]  = (card->cid.serial >> 24) & 0xFF;
    cid[10] = (card->cid.serial >> 16) & 0xFF;
    cid[11] = (card->cid.serial >> 8) & 0xFF;
    cid[12] = card->cid.serial & 0xFF;

    cid[13] = (card->cid.date >> 8) & 0x0F;
    cid[14] = card->cid.date & 0xFF;

    uint8_t crc = sd_crc7(cid, 15);
    cid[15] = (crc << 1) | 0x01;

    size_t pos = 0;
    for (int i = 0; i < 16 && pos + 2 < out_size; i++) {
        pos += snprintf(out + pos, out_size - pos, "%02X", cid[i]);
    }
}

static void read_storage_info(web_status_snapshot_t *status)
{
    FATFS *fs = NULL;
    DWORD free_clusters = 0;

    FRESULT fr = f_getfree(MOUNT_POINT, &free_clusters, &fs);

    if (fr != FR_OK || fs == NULL) {
        status->last_error = "f_getfree failed";
        return;
    }

    uint64_t total_sectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors  = (uint64_t)free_clusters * fs->csize;
    uint64_t used_sectors  = total_sectors - free_sectors;

    uint64_t sector_size = 512;

#if FF_MAX_SS != FF_MIN_SS
    sector_size = fs->ssize;
#endif

    uint64_t total = total_sectors * sector_size;
    uint64_t free  = free_sectors  * sector_size;
    uint64_t used  = used_sectors  * sector_size;

    int percent = total > 0 ? (int)((used * 100) / total) : 0;

    status->capacity_bytes = total;
    status->used_bytes = used;
    status->free_bytes = free;
    status->usage_percent = percent > 100 ? 100 : (uint8_t)percent;
}

void card_detect_init(void)
{
    if (status_mutex == NULL) {
        status_mutex = xSemaphoreCreateMutex();
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_CARD_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);
}

bool card_present(void)
{
    return gpio_get_level(PIN_CARD_DETECT) == 0;
}

esp_err_t mount_card(void)
{
    if (!card_present()) {
        update_status_missing();
        led_blink_missing_card();
        return ESP_ERR_NOT_FOUND;
    }

    update_status_mounting();
    led_set(255, 255, 0);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.width = 1;
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0  = PIN_SD_D0;

    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;

    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &mounted_card
    );

    if (ret != ESP_OK) {
        led_set(255, 0, 0);
        mounted_card = NULL;
        is_mounted = false;
        update_status_mount_failed(ret);
        return ret;
    }

    is_mounted = true;

    char cid_full[33] = {0};
    build_cid_string(mounted_card, cid_full, sizeof(cid_full));

    int mfg_month = mounted_card->cid.date & 0x0F;
    int mfg_year  = 2000 + ((mounted_card->cid.date >> 4) & 0xFF);

    web_status_snapshot_t mounted_status = {
        .state = WEB_STATUS_MOUNTED,
        .card_present = true,
        .mounted = true,
        .manufacturer_id = mounted_card->cid.mfg_id,
        .oem_id = mounted_card->cid.oem_id,
        .revision = mounted_card->cid.revision,
        .serial = mounted_card->cid.serial,
        .manufacture_month = mfg_month,
        .manufacture_year = mfg_year,
        .bus = "SDMMC 1-bit",
        .last_error = "None",
    };

    snprintf(mounted_status.model, sizeof(mounted_status.model), "%s", mounted_card->cid.name);
    snprintf(
        mounted_status.manufacturer,
        sizeof(mounted_status.manufacturer),
        "%s",
        manufacturer_name(mounted_card->cid.mfg_id)
    );
    snprintf(mounted_status.full_cid, sizeof(mounted_status.full_cid), "%s", cid_full);

    read_storage_info(&mounted_status);

    status_lock();
    current_status = mounted_status;
    status_unlock();

    led_set(0, 255, 0);

    return ESP_OK;
}

void unmount_card(void)
{
    if (!is_mounted) {
        update_status_missing();
        return;
    }

    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, mounted_card);

    mounted_card = NULL;
    is_mounted = false;

    update_status_missing();
    led_blink_missing_card();
}

void sd_card_mark_missing(void)
{
    update_status_missing();
}

void sd_card_get_status(web_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    status_lock();
    *snapshot = current_status;
    status_unlock();
}
