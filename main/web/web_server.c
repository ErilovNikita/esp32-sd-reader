#include "web_server.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "qrcode.h"
#include "sd_card_logic.h"
#include "web_config.h"
#include "web_status.h"

static const char *TAG = "WEB";
static httpd_handle_t server = NULL;

static esp_err_t root_handler(httpd_req_t *req)
{
    web_status_snapshot_t snapshot = {0};
    sd_card_get_status(&snapshot);

    char *html = malloc(WEB_HTML_BUFFER_SIZE);
    if (html == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_ERR_NO_MEM;
    }

    size_t html_len = web_status_render_html(&snapshot, html, WEB_HTML_BUFFER_SIZE);
    if (html_len >= WEB_HTML_BUFFER_SIZE) {
        free(html);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTML buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, html, html_len);
    free(html);

    return ret;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    web_status_snapshot_t snapshot = {0};
    sd_card_get_status(&snapshot);

    char *json = malloc(WEB_JSON_BUFFER_SIZE);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = web_status_render_json(&snapshot, json, WEB_JSON_BUFFER_SIZE);
    if (json_len >= WEB_JSON_BUFFER_SIZE) {
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, json, json_len);
    free(json);

    return ret;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static void build_ap_ssid(char *ssid, size_t ssid_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(
        ssid,
        ssid_size,
        "%s%s%02X%02X%02X",
        WEB_AP_SSID_PREFIX,
        WEB_AP_SSID_SEPARATOR,
        mac[3],
        mac[4],
        mac[5]
    );
}

static esp_err_t configure_ap_ip(esp_netif_t *ap_netif)
{
    esp_netif_ip_info_t ip_info = {0};

    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(WEB_AP_IP_ADDR, &ip_info.ip), TAG, "invalid AP IP");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(WEB_AP_GATEWAY, &ip_info.gw), TAG, "invalid AP gateway");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(WEB_AP_NETMASK, &ip_info.netmask), TAG, "invalid AP netmask");

    esp_err_t ret = esp_netif_dhcps_stop(ap_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "DHCP server stop failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip_info), TAG, "AP IP config failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), TAG, "DHCP server start failed");

    return ESP_OK;
}

static void build_web_url(char *url, size_t url_size)
{
    if (WEB_HTTP_PORT == 80) {
        snprintf(url, url_size, "http://%s/", WEB_AP_IP_ADDR);
    } else {
        snprintf(url, url_size, "http://%s:%d/", WEB_AP_IP_ADDR, WEB_HTTP_PORT);
    }
}

static size_t append_wifi_qr_escaped(char *out, size_t out_size, const char *value)
{
    size_t pos = 0;

    for (size_t i = 0; value[i] != '\0' && pos + 1 < out_size; i++) {
        char c = value[i];

        if ((c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') && pos + 2 < out_size) {
            out[pos++] = '\\';
        }

        if (pos + 1 >= out_size) {
            break;
        }

        out[pos++] = c;
    }

    out[pos] = '\0';
    return pos;
}

static void build_wifi_qr_payload(char *payload, size_t payload_size, const char *ssid)
{
    char escaped_ssid[80] = {0};
    char escaped_password[80] = {0};

    append_wifi_qr_escaped(escaped_ssid, sizeof(escaped_ssid), ssid);
    append_wifi_qr_escaped(escaped_password, sizeof(escaped_password), WEB_AP_PASSWORD);

    if (strlen(WEB_AP_PASSWORD) == 0) {
        snprintf(payload, payload_size, "WIFI:T:nopass;S:%s;;", escaped_ssid);
    } else {
        snprintf(payload, payload_size, "WIFI:T:WPA;S:%s;P:%s;;", escaped_ssid, escaped_password);
    }
}

typedef struct {
    const char *ssid;
    const char *url;
} connection_info_t;

static const char *qr_console_blocks[] = {
    "  ",
    "\u2580 ",
    " \u2580",
    "\u2580\u2580",
    "\u2584 ",
    "\u2588 ",
    "\u2584\u2580",
    "\u2588\u2580",
    " \u2584",
    "\u2580\u2584",
    " \u2588",
    "\u2580\u2588",
    "\u2584\u2584",
    "\u2588\u2584",
    "\u2584\u2588",
    "\u2588\u2588",
};

static void print_connection_panel(esp_qrcode_handle_t qrcode, void *user_data)
{
    const connection_info_t *info = (const connection_info_t *)user_data;
    const int border = 2;
    const int size = esp_qrcode_get_size(qrcode);
    const int row_count = (size + border * 2 + 1) / 2;
    const int side_line_count = 7;
    const int side_start_row = (row_count - side_line_count) / 2;
    char side_lines[7][96] = {0};

    snprintf(side_lines[0], sizeof(side_lines[0]), "+---------------------------------------------+");
    snprintf(side_lines[1], sizeof(side_lines[1]), "| ESP32 SD Reader Wi-Fi                       |");
    snprintf(side_lines[2], sizeof(side_lines[2]), "|                                             |");
    snprintf(side_lines[3], sizeof(side_lines[3]), "| SSID     : %-32.32s |", info->ssid);
    snprintf(side_lines[4], sizeof(side_lines[4]), "| Password : %-32.32s |", strlen(WEB_AP_PASSWORD) > 0 ? WEB_AP_PASSWORD : "(open)");
    snprintf(side_lines[5], sizeof(side_lines[5]), "| Web UI   : %-32.32s |", info->url);
    snprintf(side_lines[6], sizeof(side_lines[6]), "+---------------------------------------------+");

    printf("\n");

    for (int y = -border, row = 0; y < size + border; y += 2, row++) {
        for (int x = -border; x < size + border; x += 2) {
            unsigned char block = 0;

            if (esp_qrcode_get_module(qrcode, x, y)) {
                block |= 1 << 0;
            }
            if (esp_qrcode_get_module(qrcode, x + 1, y)) {
                block |= 1 << 1;
            }
            if (esp_qrcode_get_module(qrcode, x, y + 1)) {
                block |= 1 << 2;
            }
            if (esp_qrcode_get_module(qrcode, x + 1, y + 1)) {
                block |= 1 << 3;
            }

            printf("%s", qr_console_blocks[block]);
        }

        int side_line = row - side_start_row;
        if (side_line >= 0 && side_line < side_line_count) {
            printf("    %s", side_lines[side_line]);
        }

        printf("\n");
    }

    printf("\n");
}

static void print_connection_info(const char *ssid, const char *url)
{
    char wifi_payload[180] = {0};
    connection_info_t info = {
        .ssid = ssid,
        .url = url,
    };

    build_wifi_qr_payload(wifi_payload, sizeof(wifi_payload), ssid);

    esp_qrcode_config_t qr_config = ESP_QRCODE_CONFIG_DEFAULT();
    qr_config.display_func_with_cb = print_connection_panel;
    qr_config.user_data = &info;
    qr_config.max_qrcode_version = 6;
    qr_config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

    if (esp_qrcode_generate(&qr_config, wifi_payload) != ESP_OK) {
        printf("\n");
        printf("SSID     : %s\n", ssid);
        printf("Password : %s\n", strlen(WEB_AP_PASSWORD) > 0 ? WEB_AP_PASSWORD : "(open)");
        printf("Web UI   : %s\n\n", url);
    }
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_HTTP_PORT;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start failed");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };

    const httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
        .user_ctx = NULL,
    };

    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "root handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status), TAG, "status handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &favicon), TAG, "favicon handler failed");

    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    char ssid[33] = {0};
    char url[40] = {0};
    build_ap_ssid(ssid, sizeof(ssid));
    build_web_url(url, sizeof(url));

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(configure_ap_ip(ap_netif), TAG, "AP IP setup failed");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "esp_wifi_init failed");

    wifi_config_t wifi_config = {
        .ap = {
            .channel = WEB_AP_CHANNEL,
            .max_connection = WEB_AP_MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ssid);
    strlcpy((char *)wifi_config.ap.password, WEB_AP_PASSWORD, sizeof(wifi_config.ap.password));

    if (strlen(WEB_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "HTTP server start failed");

    print_connection_info(ssid, url);

    return ESP_OK;
}
