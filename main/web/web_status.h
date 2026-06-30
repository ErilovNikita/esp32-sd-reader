#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WEB_STATUS_STARTING = 0,
    WEB_STATUS_CARD_MISSING,
    WEB_STATUS_MOUNTING,
    WEB_STATUS_MOUNTED,
    WEB_STATUS_MOUNT_FAILED,
} web_status_state_t;

typedef struct {
    web_status_state_t state;
    bool card_present;
    bool mounted;

    char model[16];
    char manufacturer[32];
    uint8_t manufacturer_id;
    uint16_t oem_id;
    uint8_t revision;
    uint32_t serial;
    uint8_t manufacture_month;
    uint16_t manufacture_year;
    char full_cid[33];

    uint64_t capacity_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint8_t usage_percent;

    const char *bus;
    const char *last_error;
} web_status_snapshot_t;

size_t web_status_render_html(
    const web_status_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
);

size_t web_status_render_json(
    const web_status_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
);
