/***************************************************************************
 * Audiobookshelf plugin for Rockbox/Android
 *
 * Reads config from /sdcard/.rockbox/audiobookshelf.cfg, connects WiFi
 * through the Android bridge, validates the Audiobookshelf Bearer token,
 * and reports a success/failure summary.  Tokens are never shown in UI.
 *
 * Config file format (key: value, # comments, blank lines ignored):
 *   server_url:   https://your.abs.server          (required)
 *   token:        your_api_token                   (required)
 *   library_id:   lib_abc123                       (optional)
 *   download_dir: /sdcard/audiobookshelf/downloads (optional)
 *   page_size:    20                               (optional, default 20)
 ****************************************************************************/

#include "plugin.h"
#include "lib/simple_viewer.h"

#define CONFIG_PATH          "/sdcard/.rockbox/audiobookshelf.cfg"
#define DEFAULT_DOWNLOAD_DIR "/sdcard/audiobookshelf/downloads"
#define DEFAULT_PAGE_SIZE    20

#define URL_BUF_SIZE      512
#define TOKEN_BUF_SIZE    256
#define LIB_ID_BUF_SIZE   128
#define DLOAD_DIR_BUF_SIZE 256
#define HEADER_BUF_SIZE   320
#define ENDPOINT_BUF_SIZE 640
#define RESPONSE_BUF_SIZE 512
#define ERROR_BUF_SIZE    256
#define TEXT_BUF_SIZE     2048
#define LINE_BUF_SIZE     512

struct abs_config {
    char server_url[URL_BUF_SIZE];
    char token[TOKEN_BUF_SIZE];
    char library_id[LIB_ID_BUF_SIZE];
    char download_dir[DLOAD_DIR_BUF_SIZE];
    int  page_size;
};

static char *trim_ws(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + rb->strlen(s);
    while (end > s) {
        char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        end--;
    }
    *end = '\0';
    return s;
}

static const char *config_error_hint =
    "Expected entries in " CONFIG_PATH ":\n"
    "  server_url: https://your.abs.server\n"
    "  token: your_api_token\n"
    "  library_id: (optional)\n"
    "  download_dir: (optional)\n"
    "  page_size: (optional, default 20)";

static bool read_config(struct abs_config *cfg,
                        char *error_buf, size_t error_len)
{
    int fd;
    char line[LINE_BUF_SIZE];

    cfg->server_url[0] = '\0';
    cfg->token[0]      = '\0';
    cfg->library_id[0] = '\0';
    cfg->page_size     = DEFAULT_PAGE_SIZE;
    rb->strlcpy(cfg->download_dir, DEFAULT_DOWNLOAD_DIR,
                sizeof(cfg->download_dir));

    fd = rb->open(CONFIG_PATH, O_RDONLY);
    if (fd < 0) {
        rb->snprintf(error_buf, error_len,
                     "Cannot open " CONFIG_PATH "\n\n%s", config_error_hint);
        return false;
    }

    while (rb->read_line(fd, line, sizeof(line)) > 0) {
        char *t = trim_ws(line);
        char *v;

        if (t[0] == '\0' || t[0] == '#')
            continue;

        if (!rb->strncmp(t, "server_url:", 11)) {
            v = trim_ws(t + 11);
            rb->strlcpy(cfg->server_url, v, sizeof(cfg->server_url));
        } else if (!rb->strncmp(t, "token:", 6)) {
            v = trim_ws(t + 6);
            rb->strlcpy(cfg->token, v, sizeof(cfg->token));
        } else if (!rb->strncmp(t, "library_id:", 11)) {
            v = trim_ws(t + 11);
            rb->strlcpy(cfg->library_id, v, sizeof(cfg->library_id));
        } else if (!rb->strncmp(t, "download_dir:", 13)) {
            v = trim_ws(t + 13);
            if (v[0] != '\0')
                rb->strlcpy(cfg->download_dir, v, sizeof(cfg->download_dir));
        } else if (!rb->strncmp(t, "page_size:", 10)) {
            v = trim_ws(t + 10);
            if (v[0] != '\0')
                cfg->page_size = rb->atoi(v);
        }
    }

    rb->close(fd);

    if (cfg->server_url[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Missing 'server_url' in " CONFIG_PATH "\n\n%s",
                     config_error_hint);
        return false;
    }

    if (cfg->token[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Missing 'token' in " CONFIG_PATH "\n\n%s",
                     config_error_hint);
        return false;
    }

    return true;
}

enum plugin_status plugin_start(const void *parameter)
{
    struct abs_config cfg;
    char header_buf[HEADER_BUF_SIZE];
    char endpoint[ENDPOINT_BUF_SIZE];
    char response_buf[RESPONSE_BUF_SIZE];
    char error_buf[ERROR_BUF_SIZE];
    char text_buf[TEXT_BUF_SIZE];
    const char *wifi_result;
    int http_status = 0;
    int bridge_rc;

    (void)parameter;

    if (!read_config(&cfg, error_buf, sizeof(error_buf))) {
        view_text("Audiobookshelf", error_buf);
        return PLUGIN_OK;
    }

    /* Build Authorization header.  Token never appears in text_buf below. */
    rb->snprintf(header_buf, sizeof(header_buf),
                 "Authorization: Bearer %s\nAccept: application/json",
                 cfg.token);

    rb->snprintf(endpoint, sizeof(endpoint), "%s/api/me", cfg.server_url);

    rb->splash(HZ, "WiFi: connecting...");
    wifi_result = rb->android_connect_wifi();

    rb->splash(HZ, "Validating token...");
    response_buf[0] = '\0';
    error_buf[0]    = '\0';
    bridge_rc = rb->android_request(
        "GET", endpoint, header_buf, NULL,
        response_buf, sizeof(response_buf),
        &http_status,
        error_buf, sizeof(error_buf));

    /* Always disconnect before returning */
    rb->android_disconnect_wifi();

    if (bridge_rc == 0 && http_status == 200) {
        rb->snprintf(text_buf, sizeof(text_buf),
                     "Audiobookshelf: OK\n\n"
                     "Server:       %s\n"
                     "Download dir: %s\n"
                     "Library ID:   %s\n"
                     "Page size:    %d\n\n"
                     "WiFi:         %s\n"
                     "HTTP status:  %d\n"
                     "Bridge rc:    %d",
                     cfg.server_url,
                     cfg.download_dir,
                     cfg.library_id[0] ? cfg.library_id : "(auto-select)",
                     cfg.page_size,
                     wifi_result ? wifi_result : "(null)",
                     http_status,
                     bridge_rc);
    } else {
        rb->snprintf(text_buf, sizeof(text_buf),
                     "Audiobookshelf: FAILED\n\n"
                     "Server:      %s\n\n"
                     "WiFi:        %s\n"
                     "HTTP status: %d\n"
                     "Bridge rc:   %d\n\n"
                     "Bridge error:\n%s",
                     cfg.server_url,
                     wifi_result ? wifi_result : "(null)",
                     http_status,
                     bridge_rc,
                     error_buf[0] ? error_buf : "(none)");
    }

    view_text("Audiobookshelf", text_buf);
    return PLUGIN_OK;
}
