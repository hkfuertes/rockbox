#include "podcast-android.h"
#include "wifi-android.h"

const char* android_connect_wifi(void)
{
    return android_podcast_connect_wifi();
}

int android_disconnect_wifi(void)
{
    return android_podcast_disconnect_wifi();
}
