/***************************************************************************
 * Android download probe plugin
 ****************************************************************************/

#include "plugin.h"
#include "lib/simple_viewer.h"

#define CONFIG_PATH "/sdcard/.rockbox/android_download_probe.cfg"
#define URL_BUF_SIZE 512
#define HEADERS_BUF_SIZE 1024
#define ERROR_BUF_SIZE 256
#define TEXT_BUF_SIZE 2048

static char *trim_ws(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;

    end = text + rb->strlen(text);
    while (end > text)
    {
        char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        end--;
    }
    *end = '\0';
    return text;
}

static bool append_header_line(char *headers_buf, size_t headers_buf_len,
                               const char *line)
{
    size_t used = rb->strlen(headers_buf);
    size_t line_len = rb->strlen(line);
    size_t needed = used + line_len + (used > 0 ? 1 : 0) + 1;

    if (needed > headers_buf_len)
        return false;

    if (used > 0)
    {
        headers_buf[used++] = '\n';
        headers_buf[used] = '\0';
    }

    rb->strlcpy(headers_buf + used, line, headers_buf_len - used);
    return true;
}

static bool read_probe_config(char *url_buf, size_t url_buf_len,
                              char *headers_buf, size_t headers_buf_len,
                              char *dest_buf, size_t dest_buf_len,
                              char *error_buf, size_t error_buf_len)
{
    int fd;
    char line[MAX_PATH];

    url_buf[0] = '\0';
    headers_buf[0] = '\0';
    dest_buf[0] = '\0';
    error_buf[0] = '\0';

    fd = rb->open(CONFIG_PATH, O_RDONLY);
    if (fd < 0)
    {
        rb->snprintf(error_buf, error_buf_len,
                     "Could not open %s\n\nExpected lines:\nurl: https://...\ndestination: /sdcard/.rockbox/...\nheader: Name: value",
                     CONFIG_PATH);
        return false;
    }

    while (rb->read_line(fd, line, sizeof(line)) > 0)
    {
        char *trimmed = trim_ws(line);
        char *value;

        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        if (!rb->strncmp(trimmed, "url:", 4))
        {
            value = trim_ws(trimmed + 4);
            rb->strlcpy(url_buf, value, url_buf_len);
        }
        else if (!rb->strncmp(trimmed, "destination:", 12))
        {
            value = trim_ws(trimmed + 12);
            rb->strlcpy(dest_buf, value, dest_buf_len);
        }
        else if (!rb->strncmp(trimmed, "header:", 7))
        {
            value = trim_ws(trimmed + 7);
            if (!append_header_line(headers_buf, headers_buf_len, value))
            {
                rb->snprintf(error_buf, error_buf_len,
                             "Header list too long in %s", CONFIG_PATH);
                rb->close(fd);
                return false;
            }
        }
    }

    rb->close(fd);

    if (url_buf[0] == '\0')
    {
        rb->snprintf(error_buf, error_buf_len,
                     "Missing url: entry in %s", CONFIG_PATH);
        return false;
    }

    if (dest_buf[0] == '\0')
    {
        rb->snprintf(error_buf, error_buf_len,
                     "Missing destination: entry in %s", CONFIG_PATH);
        return false;
    }

    return true;
}

static const char *safe_text(const char *text)
{
    return text != NULL && text[0] != '\0' ? text : "(empty)";
}

static const char *android_request_rc_name(int bridge_rc)
{
    switch (bridge_rc)
    {
    case ANDROID_REQUEST_OK:
        return "ok";
    case ANDROID_REQUEST_INVALID_PARAM:
        return "invalid_param";
    case ANDROID_REQUEST_JNI_UNAVAILABLE:
        return "jni_unavailable";
    case ANDROID_REQUEST_JNI_METHOD_MISSING:
        return "jni_method_missing";
    case ANDROID_REQUEST_JNI_EXCEPTION:
        return "jni_exception";
    case ANDROID_REQUEST_TRUNCATED:
        return "truncated";
    case ANDROID_REQUEST_HELPER_FAILURE:
        return "helper_failure";
    default:
        return "unknown";
    }
}

enum plugin_status plugin_start(const void* parameter)
{
    char url_buf[URL_BUF_SIZE];
    char headers_buf[HEADERS_BUF_SIZE];
    char dest_buf[MAX_PATH];
    char error_buf[ERROR_BUF_SIZE];
    char text_buf[TEXT_BUF_SIZE];
    const char *wifi_result;
    int status = 0;
    int rc;

    (void)parameter;

    if (!read_probe_config(url_buf, sizeof(url_buf),
                           headers_buf, sizeof(headers_buf),
                           dest_buf, sizeof(dest_buf),
                           error_buf, sizeof(error_buf)))
    {
        view_text("Android download probe", error_buf);
        return PLUGIN_OK;
    }

    rb->splash(HZ, "WiFi: connecting...");
    wifi_result = rb->android_connect_wifi();

    rb->splash(HZ, "Download: running...");
    rc = rb->android_download(url_buf,
                              headers_buf[0] != '\0' ? headers_buf : NULL,
                              dest_buf,
                              30 * 60,
                              &status,
                              error_buf,
                              sizeof(error_buf));

    rb->android_disconnect_wifi();

    rb->snprintf(text_buf, sizeof(text_buf),
                 "Config: %s\n"
                 "URL: %s\n"
                 "Destination: %s\n"
                 "Headers configured: %s\n"
                 "\n"
                 "WiFi connect result:\n%s\n"
                 "\n"
                 "Bridge rc: %d (%s)\n"
                 "HTTP status: %d\n"
                 "\n"
                 "Bridge/filesystem error:\n%s",
                 CONFIG_PATH,
                 url_buf,
                 dest_buf,
                 headers_buf[0] != '\0' ? "yes" : "no",
                 safe_text(wifi_result),
                 rc,
                 android_request_rc_name(rc),
                 status,
                 safe_text(error_buf));

    view_text("Android download probe", text_buf);
    return PLUGIN_OK;
}
