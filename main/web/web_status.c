#include "web_status.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "project_manifest.h"
#include "sdkconfig.h"

extern const char web_status_template_start[] asm("_binary_web_status_template_html_start");
extern const char web_status_template_end[] asm("_binary_web_status_template_html_end");

typedef struct {
    const char *key;
    const char *value;
} template_value_t;

static const char *state_label(web_status_state_t state)
{
    switch (state) {
        case WEB_STATUS_STARTING: return "Starting";
        case WEB_STATUS_CARD_MISSING: return "Waiting card";
        case WEB_STATUS_MOUNTING: return "Mounting";
        case WEB_STATUS_MOUNTED: return "Mounted";
        case WEB_STATUS_MOUNT_FAILED: return "Mount failed";
        default: return "Unknown";
    }
}

static const char *state_class(web_status_state_t state)
{
    switch (state) {
        case WEB_STATUS_STARTING: return "starting";
        case WEB_STATUS_CARD_MISSING: return "missing blink";
        case WEB_STATUS_MOUNTING: return "mounting";
        case WEB_STATUS_MOUNTED: return "mounted";
        case WEB_STATUS_MOUNT_FAILED: return "failed";
        default: return "failed";
    }
}

static double bytes_to_gb(uint64_t bytes)
{
    return bytes / 1024.0 / 1024.0 / 1024.0;
}

static void format_bytes(uint64_t bytes, char *output, size_t output_size)
{
    if (bytes >= 1024ULL * 1024ULL) {
        snprintf(output, output_size, "%.2f MB", bytes / 1024.0 / 1024.0);
    } else if (bytes >= 1024ULL) {
        snprintf(output, output_size, "%.2f KB", bytes / 1024.0);
    } else {
        snprintf(output, output_size, "%llu B", (unsigned long long)bytes);
    }
}

static void format_device_id(char *output, size_t output_size)
{
    uint8_t mac[6] = {0};

    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        snprintf(output, output_size, "-");
        return;
    }

    snprintf(
        output,
        output_size,
        "%02X%02X%02X%02X%02X%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
}

static const char *fallback_text(const char *value, const char *fallback)
{
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static bool has_card_data(const web_status_snapshot_t *snapshot)
{
    return snapshot->mounted && snapshot->state == WEB_STATUS_MOUNTED;
}

static void html_escape(const char *input, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    if (input == NULL) {
        output[0] = '\0';
        return;
    }

    size_t pos = 0;
    for (const char *p = input; *p != '\0' && pos + 1 < output_size; p++) {
        const char *replacement = NULL;

        switch (*p) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (pos + replacement_len >= output_size) {
                break;
            }

            memcpy(output + pos, replacement, replacement_len);
            pos += replacement_len;
        } else {
            output[pos++] = *p;
        }
    }

    output[pos] = '\0';
}

static void json_escape(const char *input, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    if (input == NULL) {
        output[0] = '\0';
        return;
    }

    size_t pos = 0;
    for (const char *p = input; *p != '\0' && pos + 1 < output_size; p++) {
        const char *replacement = NULL;

        switch (*p) {
            case '\\': replacement = "\\\\"; break;
            case '"': replacement = "\\\""; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default: break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (pos + replacement_len >= output_size) {
                break;
            }

            memcpy(output + pos, replacement, replacement_len);
            pos += replacement_len;
        } else {
            output[pos++] = *p;
        }
    }

    output[pos] = '\0';
}

static bool append_text(char *buffer, size_t buffer_size, size_t *pos, const char *text, size_t len)
{
    if (*pos + len >= buffer_size) {
        return false;
    }

    memcpy(buffer + *pos, text, len);
    *pos += len;
    buffer[*pos] = '\0';

    return true;
}

static bool append_cstr(char *buffer, size_t buffer_size, size_t *pos, const char *text)
{
    return append_text(buffer, buffer_size, pos, text, strlen(text));
}

static const template_value_t *match_placeholder(
    const char *cursor,
    const char *template_end,
    const template_value_t *values,
    size_t value_count
)
{
    for (size_t i = 0; i < value_count; i++) {
        size_t key_len = strlen(values[i].key);
        if (cursor + key_len <= template_end && memcmp(cursor, values[i].key, key_len) == 0) {
            return &values[i];
        }
    }

    return NULL;
}

static size_t render_template(
    char *buffer,
    size_t buffer_size,
    const template_value_t *values,
    size_t value_count
)
{
    const char *cursor = web_status_template_start;
    const char *template_end = web_status_template_end;
    size_t pos = 0;

    buffer[0] = '\0';

    while (cursor < template_end && *cursor != '\0') {
        const template_value_t *value = match_placeholder(cursor, template_end, values, value_count);

        if (value != NULL) {
            if (!append_cstr(buffer, buffer_size, &pos, value->value)) {
                return buffer_size;
            }

            cursor += strlen(value->key);
        } else {
            if (!append_text(buffer, buffer_size, &pos, cursor, 1)) {
                return buffer_size;
            }

            cursor++;
        }
    }

    return pos;
}

size_t web_status_render_html(
    const web_status_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
)
{
    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (snapshot == NULL) {
        buffer[0] = '\0';
        return 0;
    }

    char usage_percent[8] = {0};
    char manufacturer_id[8] = {0};
    char oem_id[8] = {0};
    char capacity_gb[24] = {0};
    char used_gb[24] = {0};
    char free_gb[24] = {0};
    char free_gb_short[32] = {0};
    char storage_used_summary[48] = {0};
    char revision[8] = {0};
    char serial[16] = {0};
    char manufactured[16] = {0};
    char device_id[24] = {0};
    char chip_revision[16] = {0};
    char cpu_cores[8] = {0};
    char internal_ram_free[24] = {0};
    char internal_ram[24] = {0};
    char memory[64] = {0};

    char model[48] = {0};
    char manufacturer[80] = {0};
    char full_cid[96] = {0};
    char bus[48] = {0};
    char last_error[80] = {0};
    char project_name[64] = {0};
    char project_version[16] = {0};
    char project_author[32] = {0};
    char project_repository_name[64] = {0};
    char project_repository_url[96] = {0};
    bool card_data_available = has_card_data(snapshot);

    snprintf(usage_percent, sizeof(usage_percent), "%u", (unsigned int)snapshot->usage_percent);
    snprintf(manufacturer_id, sizeof(manufacturer_id), "0x%02X", snapshot->manufacturer_id);
    snprintf(oem_id, sizeof(oem_id), "0x%04X", snapshot->oem_id);
    snprintf(capacity_gb, sizeof(capacity_gb), "%.2f GB", bytes_to_gb(snapshot->capacity_bytes));
    snprintf(used_gb, sizeof(used_gb), "%.2f GB", bytes_to_gb(snapshot->used_bytes));
    snprintf(free_gb, sizeof(free_gb), "%.2f GB", bytes_to_gb(snapshot->free_bytes));
    snprintf(free_gb_short, sizeof(free_gb_short), "%.2f GB free", bytes_to_gb(snapshot->free_bytes));
    snprintf(
        storage_used_summary,
        sizeof(storage_used_summary),
        "Used %.2fGb of %.2fGb",
        bytes_to_gb(snapshot->used_bytes),
        bytes_to_gb(snapshot->capacity_bytes)
    );
    snprintf(revision, sizeof(revision), "%u", (unsigned int)snapshot->revision);
    snprintf(serial, sizeof(serial), "0x%08X", (unsigned int)snapshot->serial);
    snprintf(
        manufactured,
        sizeof(manufactured),
        "%02u/%04u",
        (unsigned int)snapshot->manufacture_month,
        (unsigned int)snapshot->manufacture_year
    );

    esp_chip_info_t chip_info = {0};
    esp_chip_info(&chip_info);

    format_device_id(device_id, sizeof(device_id));
    snprintf(chip_revision, sizeof(chip_revision), "v%u.%u", chip_info.revision / 100, chip_info.revision % 100);
    snprintf(cpu_cores, sizeof(cpu_cores), "%u", (unsigned int)chip_info.cores);

    format_bytes(heap_caps_get_total_size(MALLOC_CAP_INTERNAL), internal_ram, sizeof(internal_ram));
    format_bytes(heap_caps_get_free_size(MALLOC_CAP_INTERNAL), internal_ram_free, sizeof(internal_ram_free));
    snprintf(
        memory,
        sizeof(memory),
        "%s / %s free",
        internal_ram,
        internal_ram_free
    );

    html_escape(fallback_text(snapshot->model, "-"), model, sizeof(model));
    html_escape(fallback_text(snapshot->manufacturer, "-"), manufacturer, sizeof(manufacturer));
    html_escape(fallback_text(snapshot->full_cid, "-"), full_cid, sizeof(full_cid));
    html_escape(fallback_text(snapshot->bus, "SDMMC 1-bit"), bus, sizeof(bus));
    html_escape(fallback_text(snapshot->last_error, "None"), last_error, sizeof(last_error));
    html_escape(PROJECT_DISPLAY_NAME, project_name, sizeof(project_name));
    html_escape(PROJECT_VERSION, project_version, sizeof(project_version));
    html_escape(PROJECT_AUTHOR, project_author, sizeof(project_author));
    html_escape(PROJECT_REPOSITORY_NAME, project_repository_name, sizeof(project_repository_name));
    html_escape(PROJECT_REPOSITORY_URL, project_repository_url, sizeof(project_repository_url));

    const template_value_t values[] = {
        {"{{PROJECT_NAME}}", project_name},
        {"{{PROJECT_VERSION}}", project_version},
        {"{{PROJECT_AUTHOR}}", project_author},
        {"{{PROJECT_REPOSITORY_NAME}}", project_repository_name},
        {"{{PROJECT_REPOSITORY_URL}}", project_repository_url},
        {"{{CONTENT_CLASS}}", card_data_available ? "" : "device-only"},
        {"{{NO_CARD_HIDDEN}}", card_data_available ? "is-hidden" : ""},
        {"{{CARD_DATA_HIDDEN}}", card_data_available ? "" : "is-hidden"},
        {"{{STATUS_CLASS}}", state_class(snapshot->state)},
        {"{{STATUS_LABEL}}", state_label(snapshot->state)},
        {"{{CARD_INSERTED}}", snapshot->card_present ? "Yes" : "No"},
        {"{{CARD_MOUNTED}}", snapshot->mounted ? "Yes" : "No"},
        {"{{MODEL}}", model},
        {"{{MANUFACTURER}}", manufacturer},
        {"{{MANUFACTURER_ID}}", manufacturer_id},
        {"{{OEM_ID}}", oem_id},
        {"{{USAGE_PERCENT}}", usage_percent},
        {"{{FREE_GB_SHORT}}", free_gb_short},
        {"{{STORAGE_USED_SUMMARY}}", storage_used_summary},
        {"{{CAPACITY_GB}}", capacity_gb},
        {"{{USED_GB}}", used_gb},
        {"{{FREE_GB}}", free_gb},
        {"{{REVISION}}", revision},
        {"{{SERIAL}}", serial},
        {"{{MANUFACTURED}}", manufactured},
        {"{{FULL_CID}}", full_cid},
        {"{{BUS}}", bus},
        {"{{DEVICE_ID}}", device_id},
        {"{{CHIP_MODEL}}", CONFIG_IDF_TARGET},
        {"{{CHIP_REVISION}}", chip_revision},
        {"{{CPU_CORES}}", cpu_cores},
        {"{{MEMORY}}", memory},
        {"{{LAST_ERROR}}", last_error},
    };

    return render_template(buffer, buffer_size, values, sizeof(values) / sizeof(values[0]));
}

size_t web_status_render_json(
    const web_status_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
)
{
    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (snapshot == NULL) {
        buffer[0] = '\0';
        return 0;
    }

    char usage_percent[8] = {0};
    char manufacturer_id[8] = {0};
    char oem_id[8] = {0};
    char capacity_gb[24] = {0};
    char used_gb[24] = {0};
    char free_gb[24] = {0};
    char storage_used_summary[48] = {0};
    char revision[8] = {0};
    char serial[16] = {0};
    char manufactured[16] = {0};
    char device_id[24] = {0};
    char chip_revision[16] = {0};
    char cpu_cores[8] = {0};
    char internal_ram_free[24] = {0};
    char internal_ram[24] = {0};
    char memory[64] = {0};

    char model[64] = {0};
    char manufacturer[96] = {0};
    char full_cid[96] = {0};
    char bus[64] = {0};
    char last_error[96] = {0};
    char status_class[32] = {0};
    char status_label[32] = {0};
    char card_inserted[8] = {0};
    char card_mounted[8] = {0};
    bool card_data_available = has_card_data(snapshot);

    snprintf(usage_percent, sizeof(usage_percent), "%u", (unsigned int)snapshot->usage_percent);
    snprintf(manufacturer_id, sizeof(manufacturer_id), "0x%02X", snapshot->manufacturer_id);
    snprintf(oem_id, sizeof(oem_id), "0x%04X", snapshot->oem_id);
    snprintf(capacity_gb, sizeof(capacity_gb), "%.2f GB", bytes_to_gb(snapshot->capacity_bytes));
    snprintf(used_gb, sizeof(used_gb), "%.2f GB", bytes_to_gb(snapshot->used_bytes));
    snprintf(free_gb, sizeof(free_gb), "%.2f GB", bytes_to_gb(snapshot->free_bytes));
    snprintf(
        storage_used_summary,
        sizeof(storage_used_summary),
        "Used %.2fGb of %.2fGb",
        bytes_to_gb(snapshot->used_bytes),
        bytes_to_gb(snapshot->capacity_bytes)
    );
    snprintf(revision, sizeof(revision), "%u", (unsigned int)snapshot->revision);
    snprintf(serial, sizeof(serial), "0x%08X", (unsigned int)snapshot->serial);
    snprintf(
        manufactured,
        sizeof(manufactured),
        "%02u/%04u",
        (unsigned int)snapshot->manufacture_month,
        (unsigned int)snapshot->manufacture_year
    );

    esp_chip_info_t chip_info = {0};
    esp_chip_info(&chip_info);

    format_device_id(device_id, sizeof(device_id));
    snprintf(chip_revision, sizeof(chip_revision), "v%u.%u", chip_info.revision / 100, chip_info.revision % 100);
    snprintf(cpu_cores, sizeof(cpu_cores), "%u", (unsigned int)chip_info.cores);
    format_bytes(heap_caps_get_total_size(MALLOC_CAP_INTERNAL), internal_ram, sizeof(internal_ram));
    format_bytes(heap_caps_get_free_size(MALLOC_CAP_INTERNAL), internal_ram_free, sizeof(internal_ram_free));
    snprintf(memory, sizeof(memory), "%s / %s free", internal_ram, internal_ram_free);

    json_escape(fallback_text(snapshot->model, "-"), model, sizeof(model));
    json_escape(fallback_text(snapshot->manufacturer, "-"), manufacturer, sizeof(manufacturer));
    json_escape(fallback_text(snapshot->full_cid, "-"), full_cid, sizeof(full_cid));
    json_escape(fallback_text(snapshot->bus, "SDMMC 1-bit"), bus, sizeof(bus));
    json_escape(fallback_text(snapshot->last_error, "None"), last_error, sizeof(last_error));
    json_escape(state_class(snapshot->state), status_class, sizeof(status_class));
    json_escape(state_label(snapshot->state), status_label, sizeof(status_label));
    json_escape(snapshot->card_present ? "Yes" : "No", card_inserted, sizeof(card_inserted));
    json_escape(snapshot->mounted ? "Yes" : "No", card_mounted, sizeof(card_mounted));

    return snprintf(
        buffer,
        buffer_size,
        "{"
        "\"hasCardData\":%s,"
        "\"statusClass\":\"%s\","
        "\"statusLabel\":\"%s\","
        "\"cardInserted\":\"%s\","
        "\"cardMounted\":\"%s\","
        "\"model\":\"%s\","
        "\"manufacturer\":\"%s\","
        "\"manufacturerId\":\"%s\","
        "\"oemId\":\"%s\","
        "\"usagePercent\":\"%s\","
        "\"storageUsedSummary\":\"%s\","
        "\"capacityGb\":\"%s\","
        "\"usedGb\":\"%s\","
        "\"freeGb\":\"%s\","
        "\"revision\":\"%s\","
        "\"serial\":\"%s\","
        "\"manufactured\":\"%s\","
        "\"fullCid\":\"%s\","
        "\"bus\":\"%s\","
        "\"deviceId\":\"%s\","
        "\"chipModel\":\"%s\","
        "\"chipRevision\":\"%s\","
        "\"cpuCores\":\"%s\","
        "\"memory\":\"%s\","
        "\"lastError\":\"%s\""
        "}",
        card_data_available ? "true" : "false",
        status_class,
        status_label,
        card_inserted,
        card_mounted,
        model,
        manufacturer,
        manufacturer_id,
        oem_id,
        usage_percent,
        storage_used_summary,
        capacity_gb,
        used_gb,
        free_gb,
        revision,
        serial,
        manufactured,
        full_cid,
        bus,
        device_id,
        CONFIG_IDF_TARGET,
        chip_revision,
        cpu_cores,
        memory,
        last_error
    );
}
