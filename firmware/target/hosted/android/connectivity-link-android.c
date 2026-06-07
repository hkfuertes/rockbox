#include <errno.h>
#include <jni.h>
#include <unistd.h>

JNIEXPORT jint JNICALL
Java_org_rockbox_Helper_Connectivity_atomicLinkExclusive(JNIEnv *env,
                                                         jclass clazz,
                                                         jstring src,
                                                         jstring dst)
{
    const char *src_utf;
    const char *dst_utf;
    int rc;

    (void)clazz;

    if (src == NULL || dst == NULL)
        return EINVAL;

    src_utf = (*env)->GetStringUTFChars(env, src, NULL);
    if (src_utf == NULL)
    {
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        return ENOMEM;
    }

    dst_utf = (*env)->GetStringUTFChars(env, dst, NULL);
    if (dst_utf == NULL)
    {
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        (*env)->ReleaseStringUTFChars(env, src, src_utf);
        return ENOMEM;
    }

    rc = link(src_utf, dst_utf) == 0 ? 0 : errno;

    (*env)->ReleaseStringUTFChars(env, dst, dst_utf);
    (*env)->ReleaseStringUTFChars(env, src, src_utf);

    return rc;
}
