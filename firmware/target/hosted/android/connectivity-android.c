#include <android/log.h>
#include <jni.h>
#include "plugin.h"

/* global fields for use with various JNI calls */
JNIEnv *env_ptr;
jobject RockboxService_instance;
jclass  RockboxService_class;

extern bool upload_scrobble(const char *artist, const char *track, const char *album, int timestamp, long length);

bool upload_scrobble(const char *artist, const char *track, const char *album, int timestamp, long length)
{
    if (env_ptr == NULL || RockboxService_instance == NULL) {
        return false;
    }
    static jmethodID scrobbler_method = NULL;
    if (scrobbler_method == NULL) {
        scrobbler_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class, 
                                            "lastfmScrobbler", 
                                            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IJ)Z");
        if (scrobbler_method == NULL) {
            return false;
        }
    }

    jstring artist_jstring = (*env_ptr)->NewStringUTF(env_ptr, artist);
    jstring track_jstring = (*env_ptr)->NewStringUTF(env_ptr, track);
    jstring album_jstring = (*env_ptr)->NewStringUTF(env_ptr, album);

    (*env_ptr)->CallBooleanMethod(env_ptr, RockboxService_instance, scrobbler_method, 
                                    artist_jstring, 
                                    track_jstring, 
                                    album_jstring, 
                                    (jint) timestamp,
                                    (jlong) length);
    if ((*env_ptr)->ExceptionCheck(env_ptr)) {
        (*env_ptr)->ExceptionClear(env_ptr);
        return false;
    }
    return true;
}