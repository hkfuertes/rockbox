#include "config.h"
#include "system.h"
#include "debug.h"
#include "restart-app-android.h"
#include <jni.h>

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)

/* External references to JNI environment and service */
extern JNIEnv *env_ptr;
extern jclass RockboxService_class;
extern jobject RockboxService_instance;

/* Method IDs for Java methods */
static jmethodID android_restart_app_method = NULL;
static jmethodID android_set_restart_app_method = NULL;

int android_restart_app(int restart)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return -1; /* Error if not ready */
    }
    
    /* Get method ID if not already cached */
    if (android_restart_app_method == NULL) {
        android_restart_app_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                     "restartAndroidApp", "(I)I");
        if (android_restart_app_method == NULL) {
            return -1; /* Error on method not found */
        }
    }
    
    /* Call the Java method */
    jint result = (*env_ptr)->CallIntMethod(env_ptr, RockboxService_instance, 
                                 android_restart_app_method, (jint)restart);
    
    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return -1; /* Error on exception */
    }
    
    return (int)result;
}

int android_set_restart_app(int restart)
{
    /* Check if JNI environment and service are available */
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return -1; /* Error if not ready */
    }
    
    /* Get method ID if not already cached */
    if (android_set_restart_app_method == NULL) {
        android_set_restart_app_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
                                                     "restartAndroidApp", "(I)I");
        if (android_set_restart_app_method == NULL) {
            return -1; /* Error on method not found */
        }
    }
    
    /* Call the Java method */
    jint result = (*env_ptr)->CallIntMethod(env_ptr, RockboxService_instance, 
                                 android_set_restart_app_method, (jint)restart);
    
    /* Check for exceptions */
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return -1; /* Error on exception */
    }
    
    return (int)result;
}

#endif /* CONFIG_PLATFORM & PLATFORM_ANDROID */ 