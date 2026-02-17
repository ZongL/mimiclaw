#include "ota_manager.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE     4096
#define OTA_TIMEOUT_MS   120000

/* ── OTA update: direct path ──────────────────────────────────── */

static esp_err_t ota_update_direct(const char *url)
{
    ESP_LOGI(TAG, "Starting direct OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_TIMEOUT_MS,
        .buffer_size = OTA_BUF_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ── URL parsing helper ───────────────────────────────────────── */

static esp_err_t parse_url(const char *url, char *host, size_t host_len,
                           char *path, size_t path_len, int *port)
{
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return ESP_ERR_INVALID_ARG;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = colon - p;
        if (hlen >= host_len) return ESP_ERR_INVALID_SIZE;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= host_len) return ESP_ERR_INVALID_SIZE;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = 443;
    }

    if (slash) {
        snprintf(path, path_len, "%s", slash);
    } else {
        snprintf(path, path_len, "/");
    }
    return ESP_OK;
}

/* ── Read one HTTP line from proxy connection ─────────────────── */

static int proxy_read_line(proxy_conn_t *conn, char *buf, size_t buf_len)
{
    size_t pos = 0;
    while (pos < buf_len - 1) {
        int n = proxy_conn_read(conn, buf + pos, 1, 15000);
        if (n <= 0) return -1;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            if (pos > 0 && buf[pos - 1] == '\r') buf[pos - 1] = '\0';
            return (int)pos;
        }
        pos++;
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ── Read headers, extract Content-Length and Location ─────────── */

static int proxy_read_headers(proxy_conn_t *conn, int *content_length,
                              char *location, size_t loc_len)
{
    char line[512];
    *content_length = -1;
    if (location) location[0] = '\0';

    while (1) {
        int n = proxy_read_line(conn, line, sizeof(line));
        if (n < 0) return -1;
        if (n == 0 || line[0] == '\0') break;

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            *content_length = atoi(line + 15);
        } else if (location && strncasecmp(line, "Location:", 9) == 0) {
            const char *val = line + 9;
            while (*val == ' ') val++;
            snprintf(location, loc_len, "%s", val);
        }
    }
    return 0;
}

/* ── OTA update: proxy path (streaming with redirect) ─────────── */

static esp_err_t ota_update_proxy(const char *url)
{
    ESP_LOGI(TAG, "Starting proxy OTA from: %s", url);

    char host[128], path[512], location[512];
    int port;
    const char *current_url = url;
    char current_url_buf[512];

    /* Follow up to 5 redirects */
    for (int redirects = 0; redirects < 5; redirects++) {
        if (parse_url(current_url, host, sizeof(host), path, sizeof(path),
                      &port) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to parse URL: %s", current_url);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Connecting to %s:%d%s", host, port, path);
        proxy_conn_t *conn = proxy_conn_open(host, port, OTA_TIMEOUT_MS);
        if (!conn) return ESP_FAIL;

        char header[768];
        int hlen = snprintf(header, sizeof(header),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: MimiClaw-OTA/1.0\r\n"
            "Connection: close\r\n\r\n",
            path, host);

        if (proxy_conn_write(conn, header, hlen) < 0) {
            proxy_conn_close(conn);
            return ESP_FAIL;
        }

        /* Read status line */
        char status_line[256];
        if (proxy_read_line(conn, status_line, sizeof(status_line)) < 0) {
            proxy_conn_close(conn);
            return ESP_FAIL;
        }

        int status_code = 0;
        char *sp = strchr(status_line, ' ');
        if (sp) status_code = atoi(sp + 1);

        int content_length;
        location[0] = '\0';
        proxy_read_headers(conn, &content_length, location, sizeof(location));

        /* Handle redirects (GitHub serves binaries via 302 -> CDN) */
        if (status_code >= 300 && status_code < 400 && location[0]) {
            ESP_LOGI(TAG, "Redirect %d -> %s", status_code, location);
            proxy_conn_close(conn);
            snprintf(current_url_buf, sizeof(current_url_buf), "%s", location);
            current_url = current_url_buf;
            continue;
        }

        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP %d from %s", status_code, host);
            proxy_conn_close(conn);
            return ESP_FAIL;
        }

        /* Stream firmware to OTA partition */
        const esp_partition_t *update_part =
            esp_ota_get_next_update_partition(NULL);
        if (!update_part) {
            ESP_LOGE(TAG, "No OTA partition available");
            proxy_conn_close(conn);
            return ESP_FAIL;
        }

        esp_ota_handle_t ota_handle;
        esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES,
                                       &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            proxy_conn_close(conn);
            return err;
        }

        char *chunk_buf = malloc(OTA_BUF_SIZE);
        if (!chunk_buf) {
            esp_ota_abort(ota_handle);
            proxy_conn_close(conn);
            return ESP_ERR_NO_MEM;
        }

        size_t total_read = 0;
        while (1) {
            int n = proxy_conn_read(conn, chunk_buf, OTA_BUF_SIZE,
                                     OTA_TIMEOUT_MS);
            if (n < 0) {
                ESP_LOGE(TAG, "Read error during OTA download");
                free(chunk_buf);
                esp_ota_abort(ota_handle);
                proxy_conn_close(conn);
                return ESP_FAIL;
            }
            if (n == 0) {
                if (content_length > 0 && (int)total_read < content_length)
                    continue;
                break;
            }

            err = esp_ota_write(ota_handle, chunk_buf, n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s",
                         esp_err_to_name(err));
                free(chunk_buf);
                esp_ota_abort(ota_handle);
                proxy_conn_close(conn);
                return err;
            }
            total_read += n;

            if (content_length > 0) {
                int pct = (int)(total_read * 100 / content_length);
                ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d)",
                         pct, (int)total_read, content_length);
            } else {
                ESP_LOGI(TAG, "OTA downloaded: %d bytes", (int)total_read);
            }

            if (content_length > 0 && (int)total_read >= content_length) break;
        }

        free(chunk_buf);
        proxy_conn_close(conn);

        ESP_LOGI(TAG, "Download complete, %d bytes", (int)total_read);

        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_ota_set_boot_partition(update_part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                     esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "OTA successful, restarting...");
        esp_restart();
        return ESP_OK;  /* unreachable */
    }

    ESP_LOGE(TAG, "Too many redirects");
    return ESP_FAIL;
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t ota_update_from_url(const char *url)
{
    if (http_proxy_is_enabled()) {
        return ota_update_proxy(url);
    }
    return ota_update_direct(url);
}
