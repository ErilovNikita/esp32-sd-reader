#include "sd_card_logic.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "led_control.h"
#include "sdmmc_cmd.h"

#define PIN_SD_CLK      12
#define PIN_SD_CMD      11
#define PIN_SD_D0       10
#define PIN_CARD_DETECT 9
#define MOUNT_POINT     "/sdcard"

static sdmmc_card_t *mounted_card = NULL;
static bool is_mounted = false;

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

static double bytes_to_gb(uint64_t bytes)
{
    return bytes / 1024.0 / 1024.0 / 1024.0;
}

static void print_usage_bar(int percent)
{
    const int width = 30;
    int filled = (percent * width) / 100;

    printf("Usage           : %3d%% |", percent);

    for (int i = 0; i < width; i++) {
        putchar(i < filled ? '=' : '-');
    }

    printf("|\n");
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

static void print_storage_info(void)
{
    FATFS *fs = NULL;
    DWORD free_clusters = 0;

    FRESULT fr = f_getfree(MOUNT_POINT, &free_clusters, &fs);

    if (fr != FR_OK || fs == NULL) {
        printf("Filesystem      : mounted, f_getfree failed: %d\n", fr);
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

    printf("Filesystem      : FAT/exFAT\n");
    printf("Capacity        : %.2f GB\n", bytes_to_gb(total));
    printf("Used            : %.2f GB\n", bytes_to_gb(used));
    printf("Free            : %.2f GB\n", bytes_to_gb(free));
    print_usage_bar(percent);
}

void card_detect_init(void)
{
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
        printf("\n[SD] Card not inserted\n");
        led_blink_missing_card();
        return ESP_ERR_NOT_FOUND;
    }

    printf("[SD] Card detected, mounting...\n");
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
        printf("[SD] Mount failed: %s\n", esp_err_to_name(ret));
        led_set(255, 0, 0);
        mounted_card = NULL;
        is_mounted = false;
        return ret;
    }

    is_mounted = true;

    char cid_full[33] = {0};
    build_cid_string(mounted_card, cid_full, sizeof(cid_full));

    int mfg_month = mounted_card->cid.date & 0x0F;
    int mfg_year  = 2000 + ((mounted_card->cid.date >> 4) & 0xFF);

    printf("\n");
    printf("═══════════════════════════════════════════════\n");
    printf(" ESP32 SD Reader v2.1 by @minitwiks\n");
    printf("═══════════════════════════════════════════════\n\n");

    printf("Model           : %s\n", mounted_card->cid.name);
    printf("Manufacturer ID : 0x%02X\n", mounted_card->cid.mfg_id);
    printf("Manufacturer    : %s\n", manufacturer_name(mounted_card->cid.mfg_id));
    printf("OEM ID          : 0x%04X\n", mounted_card->cid.oem_id);
    printf("Revision        : %d\n", mounted_card->cid.revision);
    printf("Serial          : 0x%08X\n", (unsigned int)mounted_card->cid.serial);
    printf("Manufactured    : %02d/%04d\n", mfg_month, mfg_year);
    printf("Full CID        : %s\n\n", cid_full);

    print_storage_info();

    printf("\n");
    printf("Bus             : SDMMC 1-bit\n");
    printf("Mount point     : %s\n", MOUNT_POINT);
    printf("\n═══════════════════════════════════════════════\n");

    led_set(0, 255, 0);

    return ESP_OK;
}

void unmount_card(void)
{
    if (!is_mounted) return;

    printf("\n[SD] Card removed, unmounting...\n");

    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, mounted_card);

    mounted_card = NULL;
    is_mounted = false;

    printf("[SD] Unmounted\n");
    led_blink_missing_card();
}
