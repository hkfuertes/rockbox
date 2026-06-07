#include "request-android.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "plugin.h"

extern JNIEnv *env_ptr;
extern jclass RockboxService_class;
extern jobject RockboxService_instance;

static int copy_to_buffer(char *dst, size_t dst_len, const char *src)
{
    size_t src_len;

    if (dst == NULL || dst_len == 0)
        return ANDROID_REQUEST_INVALID_PARAM;

    if (src == NULL)
        src = "";

    src_len = strlen(src);
    if (src_len >= dst_len)
    {
        memcpy(dst, src, dst_len - 1);
        dst[dst_len - 1] = '\0';
        return ANDROID_REQUEST_TRUNCATED;
    }

    memcpy(dst, src, src_len + 1);
    return ANDROID_REQUEST_OK;
}

static int fail_request(int code, char *error_buf, size_t error_len,
                        const char *message)
{
    if (error_buf != NULL && error_len > 0)
        copy_to_buffer(error_buf, error_len, message);

    return code;
}

static int get_array_string(jobjectArray array, jsize index,
                            char **out_copy)
{
    jstring value;
    const char *utf;

    *out_copy = NULL;
    value = (jstring)(*env_ptr)->GetObjectArrayElement(env_ptr, array, index);
    if (value == NULL)
        return ANDROID_REQUEST_OK;

    utf = (*env_ptr)->GetStringUTFChars(env_ptr, value, NULL);
    if (utf == NULL)
    {
        if ((*env_ptr)->ExceptionCheck(env_ptr))
            (*env_ptr)->ExceptionClear(env_ptr);
        (*env_ptr)->DeleteLocalRef(env_ptr, value);
        return ANDROID_REQUEST_JNI_EXCEPTION;
    }

    *out_copy = strdup(utf);
    (*env_ptr)->ReleaseStringUTFChars(env_ptr, value, utf);
    (*env_ptr)->DeleteLocalRef(env_ptr, value);

    return *out_copy != NULL ? ANDROID_REQUEST_OK : ANDROID_REQUEST_JNI_EXCEPTION;
}

static int finalize_request_result(const char *status_text,
                                   const char *response_text,
                                   const char *error_text,
                                   char *response_buf,
                                   size_t response_len,
                                   int *status_out,
                                   char *error_buf,
                                   size_t error_len)
{
    int rc;

    if (status_text != NULL)
        *status_out = atoi(status_text);

    rc = (*status_out == 0 && error_text != NULL && error_text[0] != '\0') ?
        ANDROID_REQUEST_JNI_EXCEPTION : ANDROID_REQUEST_OK;

    if (copy_to_buffer(response_buf, response_len, response_text) == ANDROID_REQUEST_TRUNCATED)
    {
        rc = ANDROID_REQUEST_TRUNCATED;
        if (error_text == NULL || error_text[0] == '\0')
        {
            if (copy_to_buffer(error_buf, error_len,
                               "response truncated to fit caller buffer") == ANDROID_REQUEST_TRUNCATED)
                rc = ANDROID_REQUEST_TRUNCATED;
        }
        else if (copy_to_buffer(error_buf, error_len, error_text) == ANDROID_REQUEST_TRUNCATED)
            rc = ANDROID_REQUEST_TRUNCATED;
    }
    else if (copy_to_buffer(error_buf, error_len, error_text) == ANDROID_REQUEST_TRUNCATED)
        rc = ANDROID_REQUEST_TRUNCATED;

    return rc;
}

static int finalize_download_result(const char *status_text,
                                    const char *error_text,
                                    int *status_out,
                                    char *error_buf,
                                    size_t error_len)
{
    int rc;

    if (status_text != NULL)
        *status_out = atoi(status_text);

    rc = (error_text != NULL && error_text[0] != '\0') ?
        ANDROID_REQUEST_JNI_EXCEPTION : ANDROID_REQUEST_OK;

    if (copy_to_buffer(error_buf, error_len, error_text) == ANDROID_REQUEST_TRUNCATED)
        rc = ANDROID_REQUEST_TRUNCATED;

    return rc;
}

int android_request(const char *method,
                    const char *url,
                    const char *headers,
                    const char *body,
                    char *response_buf,
                    size_t response_len,
                    int *status_out,
                    char *error_buf,
                    size_t error_len)
{
    static jmethodID request_method = NULL;
    jstring method_j = NULL;
    jstring url_j = NULL;
    jstring headers_j = NULL;
    jstring body_j = NULL;
    jobjectArray result_array = NULL;
    char *status_text = NULL;
    char *response_text = NULL;
    char *error_text = NULL;
    int rc = ANDROID_REQUEST_OK;
    jsize result_len;

    if (response_buf != NULL && response_len > 0)
        response_buf[0] = '\0';
    if (error_buf != NULL && error_len > 0)
        error_buf[0] = '\0';
    if (status_out != NULL)
        *status_out = 0;

    if (method == NULL || method[0] == '\0' ||
        url == NULL || url[0] == '\0' ||
        response_buf == NULL || response_len == 0 ||
        status_out == NULL ||
        error_buf == NULL || error_len == 0)
    {
        return fail_request(ANDROID_REQUEST_INVALID_PARAM, error_buf, error_len,
                            "invalid parameters: method, url, response/status/error outputs are required");
    }

    if (env_ptr == NULL || RockboxService_instance == NULL || RockboxService_class == NULL)
        return fail_request(ANDROID_REQUEST_JNI_UNAVAILABLE, error_buf, error_len,
                            "android request bridge unavailable: JNI service not ready");

    if (request_method == NULL)
    {
        request_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
            "performSynchronousRequest",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
        if (request_method == NULL)
        {
            if ((*env_ptr)->ExceptionCheck(env_ptr))
                (*env_ptr)->ExceptionClear(env_ptr);
            return fail_request(ANDROID_REQUEST_JNI_METHOD_MISSING, error_buf, error_len,
                                "android request bridge unavailable: missing JNI method performSynchronousRequest");
        }
    }

    method_j = (*env_ptr)->NewStringUTF(env_ptr, method);
    url_j = (*env_ptr)->NewStringUTF(env_ptr, url);
    headers_j = (*env_ptr)->NewStringUTF(env_ptr, headers != NULL ? headers : "");
    body_j = (*env_ptr)->NewStringUTF(env_ptr, body != NULL ? body : "");
    if (method_j == NULL || url_j == NULL || headers_j == NULL || body_j == NULL)
    {
        if ((*env_ptr)->ExceptionCheck(env_ptr))
            (*env_ptr)->ExceptionClear(env_ptr);
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android request bridge failed: could not allocate JNI strings");
        goto cleanup;
    }

    result_array = (jobjectArray)(*env_ptr)->CallObjectMethod(env_ptr,
        RockboxService_instance, request_method, method_j, url_j, headers_j, body_j);
    if ((*env_ptr)->ExceptionCheck(env_ptr))
    {
        (*env_ptr)->ExceptionClear(env_ptr);
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android request bridge failed: Java exception during request");
        goto cleanup;
    }

    if (result_array == NULL)
    {
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android request bridge failed: Java returned no result");
        goto cleanup;
    }

    result_len = (*env_ptr)->GetArrayLength(env_ptr, result_array);
    if (result_len < 3)
    {
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android request bridge failed: malformed Java result");
        goto cleanup;
    }

    rc = get_array_string(result_array, 0, &status_text);
    if (rc != ANDROID_REQUEST_OK)
    {
        rc = fail_request(rc, error_buf, error_len,
                          "android request bridge failed: could not read status");
        goto cleanup;
    }

    rc = get_array_string(result_array, 1, &response_text);
    if (rc != ANDROID_REQUEST_OK)
    {
        rc = fail_request(rc, error_buf, error_len,
                          "android request bridge failed: could not read response body");
        goto cleanup;
    }

    rc = get_array_string(result_array, 2, &error_text);
    if (rc != ANDROID_REQUEST_OK)
    {
        rc = fail_request(rc, error_buf, error_len,
                          "android request bridge failed: could not read error text");
        goto cleanup;
    }

    rc = finalize_request_result(status_text, response_text, error_text,
                                 response_buf, response_len, status_out,
                                 error_buf, error_len);

cleanup:
    free(status_text);
    free(response_text);
    free(error_text);

    if (result_array != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, result_array);
    if (method_j != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, method_j);
    if (url_j != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, url_j);
    if (headers_j != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, headers_j);
    if (body_j != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, body_j);

    return rc;
}

int android_download(const char *url,
                     const char *headers,
                     const char *destination_path,
                     int *status_out,
                     char *error_buf,
                     size_t error_len)
{
    static jmethodID download_method = NULL;
    jstring url_j = NULL;
    jstring headers_j = NULL;
    jstring destination_j = NULL;
    jobjectArray result_array = NULL;
    char *status_text = NULL;
    char *error_text = NULL;
    int rc = ANDROID_REQUEST_OK;
    jsize result_len;

    if (error_buf != NULL && error_len > 0)
        error_buf[0] = '\0';
    if (status_out != NULL)
        *status_out = 0;

    if (url == NULL || url[0] == '\0' ||
        destination_path == NULL || destination_path[0] == '\0' ||
        status_out == NULL ||
        error_buf == NULL || error_len == 0)
    {
        return fail_request(ANDROID_REQUEST_INVALID_PARAM, error_buf, error_len,
                            "invalid parameters: url, destination, status and error outputs are required");
    }

    if (env_ptr == NULL || RockboxService_instance == NULL || RockboxService_class == NULL)
        return fail_request(ANDROID_REQUEST_JNI_UNAVAILABLE, error_buf, error_len,
                            "android download bridge unavailable: JNI service not ready");

    if (download_method == NULL)
    {
        download_method = (*env_ptr)->GetMethodID(env_ptr, RockboxService_class,
            "performSynchronousDownload",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
        if (download_method == NULL)
        {
            if ((*env_ptr)->ExceptionCheck(env_ptr))
                (*env_ptr)->ExceptionClear(env_ptr);
            return fail_request(ANDROID_REQUEST_JNI_METHOD_MISSING, error_buf, error_len,
                                "android download bridge unavailable: missing JNI method performSynchronousDownload");
        }
    }

    url_j = (*env_ptr)->NewStringUTF(env_ptr, url);
    headers_j = (*env_ptr)->NewStringUTF(env_ptr, headers != NULL ? headers : "");
    destination_j = (*env_ptr)->NewStringUTF(env_ptr, destination_path);
    if (url_j == NULL || headers_j == NULL || destination_j == NULL)
    {
        if ((*env_ptr)->ExceptionCheck(env_ptr))
            (*env_ptr)->ExceptionClear(env_ptr);
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android download bridge failed: could not allocate JNI strings");
        goto cleanup_download;
    }

    result_array = (jobjectArray)(*env_ptr)->CallObjectMethod(env_ptr,
        RockboxService_instance, download_method, url_j, headers_j, destination_j);
    if ((*env_ptr)->ExceptionCheck(env_ptr))
    {
        (*env_ptr)->ExceptionClear(env_ptr);
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android download bridge failed: Java exception during download");
        goto cleanup_download;
    }

    if (result_array == NULL)
    {
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android download bridge failed: Java returned no result");
        goto cleanup_download;
    }

    result_len = (*env_ptr)->GetArrayLength(env_ptr, result_array);
    if (result_len < 2)
    {
        rc = fail_request(ANDROID_REQUEST_JNI_EXCEPTION, error_buf, error_len,
                          "android download bridge failed: malformed Java result");
        goto cleanup_download;
    }

    rc = get_array_string(result_array, 0, &status_text);
    if (rc != ANDROID_REQUEST_OK)
    {
        rc = fail_request(rc, error_buf, error_len,
                          "android download bridge failed: could not read status");
        goto cleanup_download;
    }

    rc = get_array_string(result_array, 1, &error_text);
    if (rc != ANDROID_REQUEST_OK)
    {
        rc = fail_request(rc, error_buf, error_len,
                          "android download bridge failed: could not read error text");
        goto cleanup_download;
    }

    rc = finalize_download_result(status_text, error_text,
                                  status_out, error_buf, error_len);

cleanup_download:
    free(status_text);
    free(error_text);

    if (result_array != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, result_array);
    if (url_j != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, url_j);
    if (headers_j != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, headers_j);
    if (destination_j != NULL)
        (*env_ptr)->DeleteLocalRef(env_ptr, destination_j);

    return rc;
}
