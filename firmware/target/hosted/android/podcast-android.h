#ifndef PODCAST_ANDROID_H
#define PODCAST_ANDROID_H

#include <jni.h>

int android_podcast_download_episode(int, int);
int android_podcast_delete_episode(int, int);
int android_podcast_get_list_count(char**);
char** android_podcast_get_podcast_names(void);
char** android_podcast_get_episode_list(int);
const char* android_podcast_get_episode_path(int podcast_num, int num)
void free_array(char**);
char** split_string_newline(const char*, int*);
const char* android_podcast_connect_wifi(void);
int android_podcast_disconnect_wifi(void);
#endif // PODCAST_ANDROID_H 