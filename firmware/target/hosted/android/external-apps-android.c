#include "config.h"
#include "system.h"
#include "debug.h"
#include "external-apps-android.h"
#include <jni.h>
#include <string.h>
#include <stdio.h>

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)

/* External references to JNI environment and service */
extern JNIEnv *env_ptr;
extern jclass RockboxService_class;
extern jobject RockboxService_instance;

/* Method IDs for Java methods */
static jmethodID android_get_external_apps_count_method = NULL;
static jmethodID android_get_external_app_name_method = NULL;
static jmethodID android_get_external_app_package_name_method = NULL;
static jmethodID android_launch_external_app_method = NULL;

/* Get the number of installed applications */
int android_external_apps_get_count(void)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return 0;
    }

    /* Get method ID if not already cached */
    if (android_get_external_apps_count_method == NULL) {
        android_get_external_apps_count_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                        "getExternalAppsCount", "()I");
        if (android_get_external_apps_count_method == NULL) {
            return 0;
        }
    }

    jint result = (*env_ptr)->CallIntMethod(env_ptr, RockboxService_instance,
                                 android_get_external_apps_count_method);

    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return 0;
    }

    return (int)result;
}

/* Get app name by index */
const char* android_external_apps_get_name(int index)
{
    static char app_name_buffer[256];

    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return NULL;
    }

    /* Get method ID if not already cached */
    if (android_get_external_app_name_method == NULL) {
        android_get_external_app_name_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                        "getExternalAppName", "(I)Ljava/lang/String;");
        if (android_get_external_app_name_method == NULL) {
            return NULL;
        }
    }

    jstring result = (*env_ptr)->CallObjectMethod(env_ptr, RockboxService_instance,
                                 android_get_external_app_name_method, (jint)index);

    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return NULL;
    }

    /* Convert Java string to C string */
    if (result != NULL) {
        const char* temp = (*env_ptr)->GetStringUTFChars(env_ptr, result, NULL);
        if (temp != NULL) {
            strncpy(app_name_buffer, temp, sizeof(app_name_buffer) - 1);
            app_name_buffer[sizeof(app_name_buffer) - 1] = '\0';
            (*env_ptr)->ReleaseStringUTFChars(env_ptr, result, temp);
            return app_name_buffer;
        }
    }

    return NULL;
}

/* Get app package name by index */
const char* android_external_apps_get_package_name(int index)
{
    static char package_name_buffer[256];

    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return NULL;
    }

    /* Get method ID if not already cached */
    if (android_get_external_app_package_name_method == NULL) {
        android_get_external_app_package_name_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                        "getExternalAppPackageName", "(I)Ljava/lang/String;");
        if (android_get_external_app_package_name_method == NULL) {
            return NULL;
        }
    }

    jstring result = (*env_ptr)->CallObjectMethod(env_ptr, RockboxService_instance,
                                 android_get_external_app_package_name_method, (jint)index);

    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return NULL;
    }

    /* Convert Java string to C string */
    if (result != NULL) {
        const char* temp = (*env_ptr)->GetStringUTFChars(env_ptr, result, NULL);
        if (temp != NULL) {
            strncpy(package_name_buffer, temp, sizeof(package_name_buffer) - 1);
            package_name_buffer[sizeof(package_name_buffer) - 1] = '\0';
            (*env_ptr)->ReleaseStringUTFChars(env_ptr, result, temp);
            return package_name_buffer;
        }
    }

    return NULL;
}

/* Launch an application by index */
int android_external_apps_launch(int index)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return 0;
    }

    /* This should make this a bit faster if there are many apps */
    if (android_launch_external_app_method == NULL) {
        android_launch_external_app_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                        "launchExternalApp", "(I)Z");
        if (android_launch_external_app_method == NULL) {
            return 0;
        }
    }

    jboolean result = (*env_ptr)->CallBooleanMethod(env_ptr, RockboxService_instance,
                                 android_launch_external_app_method, (jint)index);

    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return 0;
    }

    return (int)result;
}

#endif