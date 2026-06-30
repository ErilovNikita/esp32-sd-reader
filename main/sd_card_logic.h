#pragma once

#include <stdbool.h>

#include "esp_err.h"

void card_detect_init(void);
bool card_present(void);
esp_err_t mount_card(void);
void unmount_card(void);
