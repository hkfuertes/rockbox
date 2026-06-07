#include "wifi-android.h"

extern const char* android_podcast_connect_wifi(void);
extern int android_podcast_disconnect_wifi(void);

const char* android_connect_wifi(void)
{
    return android_podcast_connect_wifi();
}

int android_disconnect_wifi(void)
{
    return android_podcast_disconnect_wifi();
}
