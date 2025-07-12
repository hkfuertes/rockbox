#include "config.h"
#include "system.h"
#include "debug.h"
#include "brightness-android.h"
#include <jni.h>

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)

/* External references to JNI environment and service */
extern JNIEnv *env_ptr;
extern jclass RockboxService_class;
extern jobject RockboxService_instance;

/* Method IDs for Java methods */
static jmethodID android_toggle_brightness_method = NULL;
static jmethodID android_get_brightness_method = NULL;
static jmethodID android_set_brightness_method = NULL;
static jmethodID android_get_brightness_percent_method = NULL;

/* Toggle Android brightness between 50% and 100% */
int android_brightness_toggle(void)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return 1; /* Default to 100% if not ready */
    }
    
    /* Get method ID if not already cached */
    if (android_toggle_brightness_method == NULL) {
        android_toggle_brightness_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                        "toggleAndroidBrightness", "()I");
        if (android_toggle_brightness_method == NULL) {
            return 1; /* Default to 100% on error */
        }
    }
    
    /* Call the Java method */
    jint result = (*env_ptr)->CallIntMethod(env_ptr, RockboxService_instance, 
                                 android_toggle_brightness_method);
    
    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return 1; /* Default to 100% on error */
    }
    
    return (int)result;
}

/* Get current Android brightness mode (0=50%, 1=100%) */
int android_brightness_get_mode(void)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return 1; /* Default to 100% if not ready */
    }
    
    /* Get method ID if not already cached */
    if (android_get_brightness_method == NULL) {
        android_get_brightness_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                     "getAndroidBrightness", "()I");
        if (android_get_brightness_method == NULL) {
            return 1; /* Default to 100% on error */
        }
    }
    
    /* Call the Java method */
    jint result = (*env_ptr)->CallIntMethod(env_ptr, RockboxService_instance, 
                                 android_get_brightness_method);
    
    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return 1; /* Default to 100% on error */
    }
    
    return (int)result;
}

/* Set Android brightness to a specific percentage (0-100) */
int android_brightness_set_percent(int percent)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return -1; /* Error if not ready */
    }
    
    /* Validate input */
    if (percent < 0 || percent > 100) {
        return -1; /* Invalid input */
    }
    
    /* Get method ID if not already cached */
    if (android_set_brightness_method == NULL) {
        android_set_brightness_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                     "setAndroidBrightnessPercent", "(I)I");
        if (android_set_brightness_method == NULL) {
            return -1; /* Error on method not found */
        }
    }
    
    /* Call the Java method */
    jint result = (*env_ptr)->CallIntMethod(env_ptr, RockboxService_instance, 
                                 android_set_brightness_method, (jint)percent);
    
    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return -1; /* Error on exception */
    }
    
    return (int)result;
}

/* Get current Android brightness as percentage (0-100) */
int android_brightness_get_percent(void)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return 100; /* Default to 100% if not ready */
    }
    
    /* Get method ID if not already cached */
    if (android_get_brightness_percent_method == NULL) {
        android_get_brightness_percent_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                         "getAndroidBrightnessPercent", "()I");
        if (android_get_brightness_percent_method == NULL) {
            return 100; /* Default to 100% on error */
        }
    }
    
    /* Call the Java method */
    jint result = (*env_ptr)->CallIntMethod(env_ptr, RockboxService_instance, 
                                 android_get_brightness_percent_method);
    
    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return 100; /* Default to 100% on error */
    }
    
    return (int)result;
}

#endif /* CONFIG_PLATFORM & PLATFORM_ANDROID */ 