#include <jni.h>
#include <stddef.h>
#include "config.h"

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
extern JNIEnv *env_ptr;
extern jclass RockboxService_class;
extern jobject RockboxService_instance;

int android_update(void)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return -1;
    }
    static jmethodID shutdown_method = NULL;
    if (shutdown_method == NULL) {
        shutdown_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "updateRockbox", "()V");
        if (shutdown_method == NULL) {
            return -1;
        }
    }
    (*env_ptr)->CallVoidMethod(env_ptr, RockboxService_instance, shutdown_method);
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return -1;
    }
    return 0;
}
#endif 