#pragma once

#include "project_manifest.h"

#define WEB_AP_SSID_PREFIX    PROJECT_DISPLAY_NAME
#define WEB_AP_SSID_SEPARATOR " "
#define WEB_AP_PASSWORD       "sdreader123"

#define WEB_AP_CHANNEL     1
#define WEB_AP_MAX_CLIENTS 4

#define WEB_AP_IP_ADDR  "192.168.4.1"
#define WEB_AP_GATEWAY  "192.168.4.1"
#define WEB_AP_NETMASK  "255.255.255.0"

#define WEB_HTTP_PORT        80
#define WEB_HTML_BUFFER_SIZE 20000
#define WEB_JSON_BUFFER_SIZE 4000
