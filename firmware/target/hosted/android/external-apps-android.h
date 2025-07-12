#ifndef __EXTERNAL_APPS_ANDROID_H__
#define __EXTERNAL_APPS_ANDROID_H__

#include "config.h"

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)

/**
 * Get the number of installed apps
 */
int android_external_apps_get_count(void);

/**
 * Get app name by index
 */
const char* android_external_apps_get_name(int index);

/**
 * Get app package name by index
 */
const char* android_external_apps_get_package_name(int index);

/**
 * Launch an app by index
 */
int android_external_apps_launch(int index);

#endif

#endif
