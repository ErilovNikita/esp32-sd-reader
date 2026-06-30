#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "web_status.h"

void card_detect_init(void);
bool card_present(void);
esp_err_t mount_card(void);
void unmount_card(void);
void sd_card_mark_missing(void);
void sd_card_get_status(web_status_snapshot_t *snapshot);
