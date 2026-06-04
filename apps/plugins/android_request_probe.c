/***************************************************************************
 * Android request probe plugin
 ****************************************************************************/

#include "plugin.h"
#include "lib/simple_viewer.h"

#define RESPONSE_BUF_SIZE 1024
#define ERROR_BUF_SIZE 256
#define TEXT_BUF_SIZE 2048

static const char *safe_text(const char *text)
{
    return text != NULL ? text : "(null)";
}

enum plugin_status plugin_start(const void* parameter)
{
    char response_buf[RESPONSE_BUF_SIZE];
    char error_buf[ERROR_BUF_SIZE];
    char text_buf[TEXT_BUF_SIZE];
    const char *wifi_result;
    int status = 0;
    int rc;

    (void)parameter;

    rb->splash(HZ, "WiFi: connecting...");
    wifi_result = rb->android_connect_wifi();

    rb->splash(HZ, "Request: running...");
    rc = rb->android_request("GET",
                             "https://example.com/rockbox-android-request-probe",
                             "Accept: text/plain\nX-Rockbox-Probe: 1",
                             NULL,
                             response_buf,
                             sizeof(response_buf),
                             &status,
                             error_buf,
                             sizeof(error_buf));

    rb->android_disconnect_wifi();

    rb->snprintf(text_buf, sizeof(text_buf),
                 "WiFi progress:\n"
                 "1. connect requested\n"
                 "2. connect result: %s\n"
                 "3. request bridge rc: %d\n"
                 "4. wifi disconnect requested\n"
                 "\n"
                 "HTTP status: %d\n"
                 "\n"
                 "Response preview:\n%s\n"
                 "\n"
                 "Bridge error:\n%s",
                 safe_text(wifi_result),
                 rc,
                 status,
                 response_buf[0] != '\0' ? response_buf : "(empty)",
                 error_buf[0] != '\0' ? error_buf : "(empty)");

    view_text("Android request probe", text_buf);
    return PLUGIN_OK;
}
