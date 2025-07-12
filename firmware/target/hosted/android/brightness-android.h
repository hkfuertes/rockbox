#ifndef BRIGHTNESS_ANDROID_H
#define BRIGHTNESS_ANDROID_H

/* Toggle Android brightness between 50% and 100% */
int android_brightness_toggle(void);

/* Get current Android brightness mode (0=50%, 1=100%) */
int android_brightness_get_mode(void);

/* Set Android brightness to a specific percentage (0-100) */
int android_brightness_set_percent(int percent);

/* Get current Android brightness as percentage (0-100) */
int android_brightness_get_percent(void);

#endif /* BRIGHTNESS_ANDROID_H */ 