#ifndef BRIGHTNESS_PICKER_H
#define BRIGHTNESS_PICKER_H

#include "config.h"

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)

#include "screen_access.h"

/* Function to set brightness using a slider interface */
bool set_brightness(struct screen *display, char *title, int *brightness);

#endif /* CONFIG_PLATFORM & PLATFORM_ANDROID */

#endif /* BRIGHTNESS_PICKER_H */ 
