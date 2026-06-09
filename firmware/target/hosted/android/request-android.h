#ifndef REQUEST_ANDROID_H
#define REQUEST_ANDROID_H

#include <stddef.h>

int android_request(const char *method,
                    const char *url,
                    const char *headers,
                    const char *body,
                    char *response_buf,
                    size_t response_len,
                    int *status_out,
                    char *error_buf,
                    size_t error_len);
int android_download(const char *url,
                     const char *headers,
                     const char *destination_path,
                     int timeout_seconds,
                     int *status_out,
                     char *error_buf,
                     size_t error_len);

#endif /* REQUEST_ANDROID_H */
