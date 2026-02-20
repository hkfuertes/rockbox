#include <jni.h>
#include <stddef.h>
#include "config.h"
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include "plugin.h"

extern JNIEnv *env_ptr;
extern jclass RockboxService_class;
extern jobject RockboxService_instance;

extern int android_podcast_download_episode(int podcast_num, int num);
int android_podcast_download_episode(int podcast_num, int num)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return -1;
    }
    static jmethodID podcast_method = NULL;
    if (podcast_method == NULL) {
        podcast_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "startPodcastDownload", "(II)V");
        if (podcast_method == NULL) {
            return -1;
        }
    }
    (*env_ptr)->CallVoidMethod(env_ptr, RockboxService_instance, podcast_method, podcast_num, num);
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return -1;
    }
    return 0;
}

extern int android_podcast_delete_episode(int podcast_num, int num);
int android_podcast_delete_episode(int podcast_num, int num)
{
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "delete episode start");

    if (env_ptr == NULL || RockboxService_instance == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "service null");
        return -1;

    }
    static jmethodID podcast_method = NULL;
    if (podcast_method == NULL) {
        podcast_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "deleteEpisode", "(II)V");
        if (podcast_method == NULL) {
            __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "failed getting method deleteepisode");
            return -1;
        }
    }
    (*env_ptr)->CallVoidMethod(env_ptr, RockboxService_instance, podcast_method, podcast_num, num);
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "some other error on return");
        return -1;
    }
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "done deleteing");
    return 0;
}

char** split_string_newline(const char* str, int* count) {
    if (!str) {
        *count = 0;
        return NULL;
    }
    
    // count number of lines
    *count = 1;
    const char* ptr = str;
    while (*ptr) {
        if (*ptr == '\n') {
            (*count)++;
        }
        ptr++;
    }
    
    char** result = malloc((*count + 1) * sizeof(char*)); // +1 for terminator
    if (!result) {
        *count = 0;
        return NULL;
    }
    
    // actually split string
    char* copy = strdup(str);
    if (!copy) {
        free(result);
        *count = 0;
        return NULL;
    }
    
    int index = 0;
    char* token = strtok(copy, "\n");
    while (token && index < *count) {
        result[index] = strdup(token);
        if (result[index]) {
            index++;
        }
        token = strtok(NULL, "\n");
    }
    
    result[index] = NULL;
    free(copy);
    
    return result;
}

extern void free_array(char** array);
void free_array(char** array) {
    if (array) {
        int i = 0;
        while (array[i]) {
            free(array[i]);
            i++;
        }
        free(array);
    }
}

extern char** android_podcast_get_podcast_names(void);
char** android_podcast_get_podcast_names(void)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return NULL;
    }
    
    static jmethodID podcast_method = NULL;
    if (podcast_method == NULL) {
        podcast_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "getPodcastNames", "()Ljava/lang/String;");
        if (podcast_method == NULL) {
            __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get getPodcastNames method ID");
            return NULL;
        }
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Calling getPodcastNames method");
    jstring jstr = (*env_ptr)->CallObjectMethod(env_ptr, RockboxService_instance, podcast_method);
    
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Exception occurred in getPodcastNames");
        (*env_ptr)->ExceptionClear(env_ptr);
        return NULL;
    }
    
    if (jstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "getPodcastNames returned NULL string");
        return NULL;
    }
    
    const char* cstr = (*env_ptr)->GetStringUTFChars(env_ptr, jstr, NULL);
    if (cstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get UTF chars");
        (*env_ptr)->DeleteLocalRef(env_ptr, jstr);
        return NULL;
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "getPodcastNames returned string: %s", cstr);
    
    // Split the string into array
    int count;
    char** result = split_string_newline(cstr, &count);
    
    // Clean up
    (*env_ptr)->ReleaseStringUTFChars(env_ptr, jstr, cstr);
    (*env_ptr)->DeleteLocalRef(env_ptr, jstr);
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Split into %d podcasts", count);
    return result;
}

extern char** android_podcast_get_episode_list(int podcast_num);
char** android_podcast_get_episode_list(int podcast_num)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return NULL;
    }
    
    static jmethodID podcast_method = NULL;
    if (podcast_method == NULL) {
        podcast_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "getEpisodeList", "(I)Ljava/lang/String;");
        if (podcast_method == NULL) {
            __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get getEpisodeList method ID");
            return NULL;
        }
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Calling getEpisodeList method");
    jstring jstr = (*env_ptr)->CallObjectMethod(env_ptr, RockboxService_instance, podcast_method, podcast_num);
    
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Exception occurred in getEpisodeList");
        (*env_ptr)->ExceptionClear(env_ptr);
        return NULL;
    }
    
    if (jstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "getEpisodeList returned NULL string");
        return NULL;
    }
    
    const char* cstr = (*env_ptr)->GetStringUTFChars(env_ptr, jstr, NULL);
    if (cstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get UTF chars");
        (*env_ptr)->DeleteLocalRef(env_ptr, jstr);
        return NULL;
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "getEpisodeList returned string: %s", cstr);
    
    // Split the string into array
    int count;
    char** result = split_string_newline(cstr, &count);
    
    // Clean up
    (*env_ptr)->ReleaseStringUTFChars(env_ptr, jstr, cstr);
    (*env_ptr)->DeleteLocalRef(env_ptr, jstr);
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Split into %d episodes", count);
    return result;
}

extern const char* android_podcast_get_episode_path(int podcast_num, int num);
const const char* android_podcast_get_episode_path(int podcast_num, int num)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return NULL;
    }
    
    static jmethodID podcast_method = NULL;
    if (podcast_method == NULL) {
        podcast_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "getEpisodePath", "(II)Ljava/lang/String;");
        if (podcast_method == NULL) {
            __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get getEpisodePath method ID");
            return NULL;
        }
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Calling getEpisodePath method");
    jstring jstr = (*env_ptr)->CallObjectMethod(env_ptr, RockboxService_instance, podcast_method, podcast_num, num);
    
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Exception occurred in getEpisodePath");
        (*env_ptr)->ExceptionClear(env_ptr);
        return NULL;
    }
    
    if (jstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "getEpisodePath returned NULL string");
        return NULL;
    }
    
    const char* cstr = (*env_ptr)->GetStringUTFChars(env_ptr, jstr, NULL);
    if (cstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get UTF chars");
        (*env_ptr)->DeleteLocalRef(env_ptr, jstr);
        return NULL;
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "getEpisodePath returned string: %s", cstr);
    return cstr;
}

extern int android_podcast_get_list_count(char** array);
int android_podcast_get_list_count(char** array) {
    if (!array) return 0;
    int count = 0;
    while (array[count]) {
        count++;
    }
    return count;
}

extern const char* android_podcast_connect_wifi(void);
const char* android_podcast_connect_wifi(void)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return NULL;
    }
    
    static jmethodID podcast_method = NULL;
    if (podcast_method == NULL) {
        podcast_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "connectWifi", "()Ljava/lang/String;");
        if (podcast_method == NULL) {
            __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get connectWifi method ID");
            return NULL;
        }
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Calling connectWifi method");
    jstring jstr = (*env_ptr)->CallObjectMethod(env_ptr, RockboxService_instance, podcast_method);
    
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Exception occurred in connectWifi");
        (*env_ptr)->ExceptionClear(env_ptr);
        return NULL;
    }
    
    if (jstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "connectWifi returned NULL string");
        return NULL;
    }
    
    const char* cstr = (*env_ptr)->GetStringUTFChars(env_ptr, jstr, NULL);
    if (cstr == NULL) {
        __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "Failed to get UTF chars");
        (*env_ptr)->DeleteLocalRef(env_ptr, jstr);
        return NULL;
    }
    
    __android_log_print(ANDROID_LOG_DEBUG, "RockboxService", "connectWifi returned string: %s", cstr);
    return cstr;
}

extern int android_podcast_disconnect_wifi(void);
int android_podcast_disconnect_wifi(void)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return -1;
    }
    static jmethodID podcast_method = NULL;
    if (podcast_method == NULL) {
        podcast_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, "disconnectWifi", "()V");
        if (podcast_method == NULL) {
            return -1;
        }
    }
    (*env_ptr)->CallVoidMethod(env_ptr, RockboxService_instance, podcast_method);
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return -1;
    }
    return 0;
}
