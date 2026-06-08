/***************************************************************************
 * Audiobookshelf plugin for Rockbox/Android
 *
 * Reads config from /sdcard/.rockbox/audiobookshelf.cfg, connects WiFi
 * through the Android bridge, validates the Audiobookshelf Bearer token,
 * fetches accessible libraries (when library_id is not configured), shows
 * a Rockbox list picker, and reports the result. Tokens are never shown.
 *
 * Config file format (key: value, # comments, blank lines ignored):
 *   server_url:   https://your.abs.server          (required)
 *   token:        your_api_token                   (required)
 *   library_id:   lib_abc123                       (optional)
 *   download_dir: /sdcard/audiobookshelf/downloads (optional)
 *   page_size:    20                               (optional, default 20)
 *
 * Compile-time toggle:
 *   Define AUDIOBOOKSHELF_FAKE_BACKEND (e.g. -DAUDIOBOOKSHELF_FAKE_BACKEND)
 *   to replace all network calls with static fake data.  All UI code paths
 *   remain live; no real WiFi or server is needed.
 ****************************************************************************/

#include "plugin.h"
#include "lib/simple_viewer.h"
#include "jsmn.h"

/* ---- sizes ---------------------------------------------------------------- */
#define CONFIG_PATH          "/sdcard/.rockbox/audiobookshelf.cfg"
#define DEFAULT_DOWNLOAD_DIR "/sdcard/audiobookshelf/downloads"
#define SAFE_DOWNLOAD_BASE   "/sdcard/audiobookshelf"
#define DEFAULT_PAGE_SIZE    20
#define MAX_PAGE_SIZE        50

#define URL_BUF_SIZE         512
#define TOKEN_BUF_SIZE       256
#define LIB_ID_BUF_SIZE      128
#define DLOAD_DIR_BUF_SIZE   256
#define HEADER_BUF_SIZE      320
#define ENDPOINT_BUF_SIZE    640
#define RESPONSE_BUF_SIZE    512  /* for /api/me — we only check http_status */
#define ERROR_BUF_SIZE       256
#define TEXT_BUF_SIZE        2048
#define LINE_BUF_SIZE        512
#define INDEX_LINE_BUF_SIZE  1024

#define LIBRARIES_RESPONSE_SIZE 4096
#define ITEMS_RESPONSE_SIZE     65536
#define DETAIL_RESPONSE_SIZE    32768
#define JSON_TOKEN_COUNT        4096
#define MAX_LIBRARIES           32
#define LIBRARY_NAME_SIZE       64
#define LIBRARY_ID_SIZE         128
#define MAX_BOOKS_PER_PAGE      MAX_PAGE_SIZE
#define BOOK_TITLE_SIZE         160
#define BOOK_ID_SIZE            128
#define BOOK_META_SIZE          64
#define DETAIL_LINE_SIZE        192
#define MAX_DETAIL_LINES        16
#define LIST_TITLE_SIZE         96
#define FILE_EXT_SIZE           16
#define MAX_DETAIL_AUDIO_FILES  64

#define LOCAL_INDEX_PATH        SAFE_DOWNLOAD_BASE "/local-index.tsv"
#define JSON_TMP_PATH           "/sdcard/.rockbox/audiobookshelf-response.tmp"

#define BOOK_PICKER_CANCEL      (-1)
#define BOOK_PICKER_USB         (-2)
#define BOOK_PICKER_PREV_PAGE   (-3)
#define BOOK_PICKER_NEXT_PAGE   (-4)

#define DETAIL_ACTION_BACK          0
#define DETAIL_ACTION_DOWNLOAD      1
#define DETAIL_ACTION_PLAY_LOCAL    2
#define DETAIL_ACTION_DELETE_LOCAL  3
#define DETAIL_ACTION_SYNC_PROGRESS 4
#define DETAIL_ACTION_USB          -2

/* ---- config --------------------------------------------------------------- */
struct abs_config {
    char server_url[URL_BUF_SIZE];
    char token[TOKEN_BUF_SIZE];
    char library_id[LIB_ID_BUF_SIZE];
    char download_dir[DLOAD_DIR_BUF_SIZE];
    int  page_size;
};

/* ---- static globals ------------------------------------------------------- */
static char      g_lib_names[MAX_LIBRARIES][LIBRARY_NAME_SIZE];
static char      g_lib_ids[MAX_LIBRARIES][LIBRARY_ID_SIZE];
static int       g_lib_count;
static char      g_lib_response[LIBRARIES_RESPONSE_SIZE];

static char      g_book_titles[MAX_BOOKS_PER_PAGE][BOOK_TITLE_SIZE];
static char      g_book_ids[MAX_BOOKS_PER_PAGE][BOOK_ID_SIZE];
static int       g_book_count;
static int       g_book_page;
static int       g_book_limit;
static int       g_book_total;
static bool      g_book_has_next;
static char      g_items_response[ITEMS_RESPONSE_SIZE];
static char      g_detail_response[DETAIL_RESPONSE_SIZE];
static char      g_list_title[LIST_TITLE_SIZE];

struct abs_book_detail {
    char item_id[BOOK_ID_SIZE];
    char title[BOOK_TITLE_SIZE];
    char author[BOOK_META_SIZE];
    char series[BOOK_META_SIZE];
    char narrator[BOOK_META_SIZE];
    char published_year[16];
    char single_audio_ino[BOOK_ID_SIZE];
    char single_audio_filename[BOOK_TITLE_SIZE];
    char single_audio_ext[FILE_EXT_SIZE];
    char audio_file_ids[MAX_DETAIL_AUDIO_FILES][BOOK_ID_SIZE];
    char audio_file_exts[MAX_DETAIL_AUDIO_FILES][FILE_EXT_SIZE];
    char local_path[MAX_PATH];
    int duration_seconds;
    int audio_file_count;
    int tracked_audio_file_count;
    int downloadable_audio_file_count;
    int local_audio_file_count;
    int local_index_entry_count;
    int local_stale_index_count;
    bool audio_files_known;
    bool has_audio_files;
    bool has_single_audio_file;
    bool local_file_found;
};

struct abs_progress_state {
    int current_time;
    int duration;
    int percent;
    bool known;
    bool finished;
};

struct abs_progress_snapshot {
    char local_path[MAX_PATH];
    int current_time;
    int duration;
    int percent;
    bool finished;
};

static struct abs_book_detail g_book_detail;
static char      g_detail_lines[MAX_DETAIL_LINES][DETAIL_LINE_SIZE];
static int       g_detail_line_count;
static int       g_detail_action_start;

static jsmntok_t g_tokens[JSON_TOKEN_COUNT];

static int count_local_indexed_detail_files(const struct abs_config *cfg,
                                            const char *library_id,
                                            char *first_path,
                                            size_t first_path_len);
static int count_local_detail_index_entries(const struct abs_config *cfg,
                                            const char *library_id,
                                            bool existing_only);

/* ---- helpers -------------------------------------------------------------- */

static char *trim_ws(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + rb->strlen(s);
    while (end > s) {
        char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        end--;
    }
    *end = '\0';
    return s;
}

static int sanitize_page_size(int page_size)
{
    if (page_size <= 0)
        return DEFAULT_PAGE_SIZE;
    if (page_size > MAX_PAGE_SIZE)
        return MAX_PAGE_SIZE;
    return page_size;
}

/* Compare a jsmn STRING token against a C string literal. */
static bool tok_eq(const char *js, const jsmntok_t *tok, const char *s)
{
    int len = tok->end - tok->start;
    return tok->type == JSMN_STRING &&
           len == (int)rb->strlen(s) &&
           rb->memcmp(js + tok->start, s, (size_t)len) == 0;
}

/* Copy a jsmn token's text into buf (NUL-terminated, truncated if needed). */
static void tok_copy(const char *js, const jsmntok_t *tok,
                     char *buf, size_t buf_len)
{
    size_t len = (size_t)(tok->end - tok->start);
    if (buf_len == 0)
        return;
    if (len >= buf_len)
        len = buf_len - 1;
    rb->memcpy(buf, js + tok->start, len);
    buf[len] = '\0';
}

static int tok_to_int(const char *js, const jsmntok_t *tok)
{
    char buf[16];

    if (tok->type != JSMN_PRIMITIVE && tok->type != JSMN_STRING)
        return 0;

    tok_copy(js, tok, buf, sizeof(buf));
    return rb->atoi(buf);
}

static bool tok_to_bool(const char *js, const jsmntok_t *tok, bool *value)
{
    char buf[8];

    if (!value || (tok->type != JSMN_PRIMITIVE && tok->type != JSMN_STRING))
        return false;

    tok_copy(js, tok, buf, sizeof(buf));
    if (!rb->strcmp(buf, "true") || !rb->strcmp(buf, "1")) {
        *value = true;
        return true;
    }
    if (!rb->strcmp(buf, "false") || !rb->strcmp(buf, "0")) {
        *value = false;
        return true;
    }

    return false;
}

static int tok_to_percent(const char *js, const jsmntok_t *tok)
{
    char buf[24];
    char *dot;
    int whole;
    int frac = 0;

    if (tok->type != JSMN_PRIMITIVE && tok->type != JSMN_STRING)
        return -1;

    tok_copy(js, tok, buf, sizeof(buf));
    whole = rb->atoi(buf);
    dot = rb->strchr(buf, '.');

    if (whole > 1)
        return whole > 100 ? 100 : whole;
    if (!dot)
        return whole == 1 ? 100 : whole;
    if (whole >= 1)
        return 100;

    if (dot[1] >= '0' && dot[1] <= '9')
        frac += (dot[1] - '0') * 10;
    if (dot[2] >= '0' && dot[2] <= '9')
        frac += (dot[2] - '0');

    return frac;
}

/*
 * Advance token index past the token at [i] and all its descendants.
 * Uses character-position containment: all tokens whose start < tokens[i].end
 * are children of token i (jsmn stores tokens in parsing order).
 */
static int skip_value(int i, int r, const jsmntok_t *tokens)
{
    int end = tokens[i].end;
    i++;
    while (i < r && tokens[i].start < end)
        i++;
    return i;
}

static int find_object_value(const char *js, const jsmntok_t *tokens,
                             int r, int obj_idx, const char *key)
{
    int i;
    int obj_end;

    if (obj_idx < 0 || obj_idx >= r || tokens[obj_idx].type != JSMN_OBJECT)
        return -1;

    obj_end = tokens[obj_idx].end;
    i = obj_idx + 1;

    while (i + 1 < r && tokens[i].start < obj_end) {
        if (tokens[i].type == JSMN_STRING && tok_eq(js, &tokens[i], key))
            return i + 1;
        i++;
        if (i < r)
            i = skip_value(i, r, tokens);
    }

    return -1;
}

static void build_book_title(char *dest, size_t dest_len,
                             const char *title,
                             const char *author,
                             const char *series)
{
    const char *base = title[0] != '\0' ? title : "(untitled)";

    if (author[0] != '\0' && series[0] != '\0') {
        rb->snprintf(dest, dest_len, "%s - %s / %s", base, author, series);
    } else if (author[0] != '\0') {
        rb->snprintf(dest, dest_len, "%s - %s", base, author);
    } else if (series[0] != '\0') {
        rb->snprintf(dest, dest_len, "%s - %s", base, series);
    } else {
        rb->strlcpy(dest, base, dest_len);
    }
}

static void build_book_entry(char *dest, size_t dest_len,
                             const char *item_id,
                             const char *title,
                             const char *author,
                             const char *series,
                             const char *download_state,
                             const char *progress_state)
{
    char base[BOOK_TITLE_SIZE];

    build_book_title(base, sizeof(base), title, author, series);
    rb->snprintf(dest, dest_len, "[%s] %s | DL:%s | P:%s",
                 item_id && item_id[0] ? item_id : "?",
                 base,
                 download_state && download_state[0] ? download_state : "--",
                 progress_state && progress_state[0] ? progress_state : "--");
}

static void format_duration(int seconds, char *buf, size_t buf_len)
{
    int hours;
    int minutes;

    if (seconds <= 0) {
        buf[0] = '\0';
        return;
    }

    hours = seconds / 3600;
    minutes = (seconds % 3600) / 60;
    seconds %= 60;

    if (hours > 0) {
        rb->snprintf(buf, buf_len, "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        rb->snprintf(buf, buf_len, "%d:%02d", minutes, seconds);
    }
}

static void add_detail_line(const char *label, const char *value)
{
    if (!value || value[0] == '\0' || g_detail_line_count >= MAX_DETAIL_LINES)
        return;

    rb->snprintf(g_detail_lines[g_detail_line_count],
                 sizeof(g_detail_lines[g_detail_line_count]),
                 "%s: %s", label, value);
    g_detail_line_count++;
}

static const char *safe_text(const char *text)
{
    return text != NULL && text[0] != '\0' ? text : "(none)";
}

static bool path_has_traversal(const char *path)
{
    const char *p = path;

    if (path == NULL || path[0] == '\0')
        return true;

    while (*p) {
        if ((p == path || p[-1] == '/') &&
            p[0] == '.' &&
            ((p[1] == '.' && (p[2] == '/' || p[2] == '\0')) ||
             (p[1] == '/' || p[1] == '\0')))
            return true;
        p++;
    }

    return false;
}

static bool path_is_under_base(const char *path, const char *base)
{
    size_t base_len;

    if (path == NULL || base == NULL)
        return false;

    base_len = rb->strlen(base);
    if (rb->strncmp(path, base, base_len) != 0)
        return false;

    return path[base_len] == '\0' || path[base_len] == '/';
}

static void sanitize_path_component(const char *src, char *dest, size_t dest_len)
{
    size_t di = 0;
    bool last_was_sep = false;
    char c;

    if (dest_len == 0)
        return;

    if (src == NULL || src[0] == '\0')
        src = "untitled";

    while ((c = *src++) != '\0' && di + 1 < dest_len) {
        bool keep = (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9');

        if (keep || c == '-' || c == '_') {
            dest[di++] = c;
            last_was_sep = false;
        } else if (c == ' ' || c == '.' || c == ',') {
            if (!last_was_sep && di > 0) {
                dest[di++] = '_';
                last_was_sep = true;
            }
        } else {
            if (!last_was_sep && di > 0) {
                dest[di++] = '_';
                last_was_sep = true;
            }
        }
    }

    while (di > 0 && dest[di - 1] == '_')
        di--;

    if (di == 0) {
        rb->strlcpy(dest, "untitled", dest_len);
        return;
    }

    dest[di] = '\0';
}

static bool normalize_file_extension(const char *filename,
                                     const char *ext_hint,
                                     char *ext_buf,
                                     size_t ext_len)
{
    char tmp[FILE_EXT_SIZE];
    const char *src = NULL;
    size_t i;
    size_t len;

    if (ext_len == 0)
        return false;

    ext_buf[0] = '\0';
    if (ext_hint != NULL && ext_hint[0] == '.')
        src = ext_hint;
    else if (filename != NULL)
        src = rb->strrchr(filename, '.');

    if (src == NULL || src[0] != '.')
        return false;

    len = rb->strlen(src);
    if (len < 2 || len >= sizeof(tmp))
        return false;

    tmp[0] = '.';
    for (i = 1; i < len; i++) {
        char c = src[i];

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9')))
            return false;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        tmp[i] = c;
    }
    tmp[len] = '\0';

    rb->strlcpy(ext_buf, tmp, ext_len);
    return true;
}

static bool ensure_directory_tree(const char *path)
{
    char buf[MAX_PATH];
    char *p;

    if (path == NULL || path[0] != '/' || rb->strlen(path) >= sizeof(buf))
        return false;

    rb->strlcpy(buf, path, sizeof(buf));
    for (p = buf + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (!rb->file_exists(buf) && rb->mkdir(buf) < 0) {
            *p = '/';
            return false;
        }
        *p = '/';
    }

    return rb->file_exists(buf) || rb->mkdir(buf) >= 0;
}

static bool build_temp_download_path(const char *final_path,
                                     char *tmp_buf,
                                     size_t tmp_len)
{
    size_t final_len;
    const char *suffix = ".part";
    size_t suffix_len = rb->strlen(suffix);

    if (final_path == NULL || tmp_buf == NULL || tmp_len == 0)
        return false;

    final_len = rb->strlen(final_path);
    if (final_len + suffix_len >= tmp_len)
        return false;

    rb->snprintf(tmp_buf, tmp_len, "%s%s", final_path, suffix);
    return true;
}

static bool promote_temp_file_exclusive(const char *tmp_path,
                                        const char *dest_path,
                                        char *error_buf,
                                        size_t error_len)
{
    int in_fd;
    int out_fd;
    char buf[4096];
    ssize_t nread;
    bool ok = true;

    in_fd = rb->open(tmp_path, O_RDONLY);
    if (in_fd < 0) {
        rb->snprintf(error_buf, error_len,
                     "Cannot open downloaded temp file");
        return false;
    }

    out_fd = rb->open(dest_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out_fd < 0) {
        rb->close(in_fd);
        rb->snprintf(error_buf, error_len,
                     rb->file_exists(dest_path) ?
                     "Destination already exists; not overwriting" :
                     "Cannot create final download file");
        return false;
    }

    while ((nread = rb->read(in_fd, buf, sizeof(buf))) > 0) {
        if (rb->write(out_fd, buf, (size_t)nread) != nread) {
            ok = false;
            rb->snprintf(error_buf, error_len,
                         "Cannot write final download file");
            break;
        }
    }
    if (nread < 0) {
        ok = false;
        rb->snprintf(error_buf, error_len,
                     "Cannot read downloaded temp file");
    }
    if (rb->close(out_fd) < 0 && ok) {
        ok = false;
        rb->snprintf(error_buf, error_len,
                     "Cannot flush final download file");
    }
    rb->close(in_fd);

    if (!ok) {
        rb->remove(dest_path);
        return false;
    }

    rb->remove(tmp_path);
    return true;
}

static bool local_index_path_is_safe(const struct abs_config *cfg,
                                     const char *path)
{
    char root_buf[DLOAD_DIR_BUF_SIZE];
    size_t len;

    if (cfg == NULL || path == NULL || path[0] == '\0' ||
        path_has_traversal(path) || !path_is_under_base(path, SAFE_DOWNLOAD_BASE))
        return false;

    rb->strlcpy(root_buf, cfg->download_dir, sizeof(root_buf));
    len = rb->strlen(root_buf);
    while (len > 1 && root_buf[len - 1] == '/')
        root_buf[--len] = '\0';

    return path_is_under_base(path, root_buf);
}

static bool read_local_index_path(const struct abs_config *cfg,
                                  const char *library_id,
                                  const char *item_id,
                                  const char *media_file_id,
                                  char *path_buf,
                                  size_t path_len)
{
    int fd;
    char line[INDEX_LINE_BUF_SIZE];

    if (path_len == 0)
        return false;
    path_buf[0] = '\0';

    fd = rb->open(LOCAL_INDEX_PATH, O_RDONLY);
    if (fd < 0)
        return false;

    while (rb->read_line(fd, line, sizeof(line)) > 0) {
        char *server;
        char *library;
        char *item;
        char *media;
        char *path;
        char *tab;

        server = line;
        tab = rb->strchr(server, '\t');
        if (!tab)
            continue;
        *tab++ = '\0';
        library = tab;
        tab = rb->strchr(library, '\t');
        if (!tab)
            continue;
        *tab++ = '\0';
        item = tab;
        tab = rb->strchr(item, '\t');
        if (!tab)
            continue;
        *tab++ = '\0';
        media = tab;
        tab = rb->strchr(media, '\t');
        if (!tab)
            continue;
        *tab++ = '\0';
        path = trim_ws(tab);

        if (!rb->strcmp(server, cfg->server_url) &&
            !rb->strcmp(library, library_id) &&
            !rb->strcmp(item, item_id) &&
            !rb->strcmp(media, media_file_id) &&
            local_index_path_is_safe(cfg, path) &&
            rb->file_exists(path)) {
            rb->strlcpy(path_buf, path, path_len);
            rb->close(fd);
            return true;
        }
    }

    rb->close(fd);
    return false;
}

static bool write_local_index_path(const struct abs_config *cfg,
                                   const char *library_id,
                                   const char *item_id,
                                   const char *media_file_id,
                                   const char *path,
                                   char *error_buf,
                                   size_t error_len)
{
    int in_fd;
    int out_fd;
    char line[INDEX_LINE_BUF_SIZE];
    char tmp_path[MAX_PATH];
    char backup_path[MAX_PATH];
    bool have_old_index = false;
    bool backed_up_old_index = false;
    int backup_idx;

    if (!ensure_directory_tree(SAFE_DOWNLOAD_BASE)) {
        rb->snprintf(error_buf, error_len,
                     "Cannot create %s", SAFE_DOWNLOAD_BASE);
        return false;
    }

    rb->snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", LOCAL_INDEX_PATH);
    if (rb->strlen(tmp_path) >= sizeof(tmp_path) - 1) {
        rb->snprintf(error_buf, error_len,
                     "Local index temp path is too long");
        return false;
    }

    rb->remove(tmp_path);
    out_fd = rb->open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        rb->snprintf(error_buf, error_len,
                     "Cannot write local index temp file");
        return false;
    }

    in_fd = rb->open(LOCAL_INDEX_PATH, O_RDONLY);
    if (in_fd >= 0) {
        bool write_failed = false;
        while (rb->read_line(in_fd, line, sizeof(line)) > 0) {
            char parse_buf[INDEX_LINE_BUF_SIZE];
            char *server;
            char *library;
            char *item;
            char *media;
            char *tab;
            bool replace = false;

            rb->strlcpy(parse_buf, line, sizeof(parse_buf));
            server = parse_buf;
            tab = rb->strchr(server, '\t');
            if (tab) {
                *tab++ = '\0';
                library = tab;
                tab = rb->strchr(library, '\t');
                if (tab) {
                    *tab++ = '\0';
                    item = tab;
                    tab = rb->strchr(item, '\t');
                    if (tab) {
                        *tab++ = '\0';
                        media = tab;
                        tab = rb->strchr(media, '\t');
                        if (tab) {
                            *tab = '\0';
                            replace = !rb->strcmp(server, cfg->server_url) &&
                                      !rb->strcmp(library, library_id) &&
                                      !rb->strcmp(item, item_id) &&
                                      !rb->strcmp(media, media_file_id);
                        }
                    }
                }
            }

            if (!replace && rb->fdprintf(out_fd, "%s\n", trim_ws(line)) < 0)
                write_failed = true;
        }
        rb->close(in_fd);
        if (write_failed) {
            rb->close(out_fd);
            rb->remove(tmp_path);
            rb->snprintf(error_buf, error_len,
                         "Cannot write complete local index");
            return false;
        }
    }

    if (rb->fdprintf(out_fd, "%s\t%s\t%s\t%s\t%s\n",
                     cfg->server_url, library_id, item_id, media_file_id, path) < 0) {
        rb->close(out_fd);
        rb->remove(tmp_path);
        rb->snprintf(error_buf, error_len,
                     "Cannot write local index entry");
        return false;
    }
    if (rb->close(out_fd) < 0) {
        rb->remove(tmp_path);
        rb->snprintf(error_buf, error_len,
                     "Cannot flush local index");
        return false;
    }

    have_old_index = rb->file_exists(LOCAL_INDEX_PATH);
    if (have_old_index) {
        backup_path[0] = '\0';
        for (backup_idx = 0; backup_idx < 10; backup_idx++) {
            rb->snprintf(backup_path, sizeof(backup_path), "%s.bak%d",
                         LOCAL_INDEX_PATH, backup_idx);
            if (!rb->file_exists(backup_path))
                break;
        }
        if (backup_idx >= 10 || backup_path[0] == '\0') {
            rb->remove(tmp_path);
            rb->snprintf(error_buf, error_len,
                         "Cannot find safe local index backup path");
            return false;
        }
        if (rb->rename(LOCAL_INDEX_PATH, backup_path) < 0) {
            rb->remove(tmp_path);
            rb->snprintf(error_buf, error_len,
                         "Cannot preserve local index before replace");
            return false;
        }
        backed_up_old_index = true;
    }

    if (rb->rename(tmp_path, LOCAL_INDEX_PATH) < 0) {
        rb->remove(tmp_path);
        if (backed_up_old_index)
            rb->rename(backup_path, LOCAL_INDEX_PATH);
        rb->snprintf(error_buf, error_len,
                     "Cannot finalize local index");
        return false;
    }

    if (backed_up_old_index)
        rb->remove(backup_path);

    return true;
}

static bool parse_local_index_line_for_item(char *line,
                                            const struct abs_config *cfg,
                                            const char *library_id,
                                            const char *item_id,
                                            char *path_buf,
                                            size_t path_len)
{
    char *server;
    char *library;
    char *item;
    char *media;
    char *path;
    char *tab;

    if (path_len > 0)
        path_buf[0] = '\0';
    if (cfg == NULL || library_id == NULL || item_id == NULL)
        return false;

    server = line;
    tab = rb->strchr(server, '\t');
    if (!tab)
        return false;
    *tab++ = '\0';
    library = tab;
    tab = rb->strchr(library, '\t');
    if (!tab)
        return false;
    *tab++ = '\0';
    item = tab;
    tab = rb->strchr(item, '\t');
    if (!tab)
        return false;
    *tab++ = '\0';
    media = tab;
    tab = rb->strchr(media, '\t');
    if (!tab)
        return false;
    *tab++ = '\0';
    path = trim_ws(tab);
    (void)media;

    if (path_len > 0)
        rb->strlcpy(path_buf, path, path_len);

    return !rb->strcmp(server, cfg->server_url) &&
           !rb->strcmp(library, library_id) &&
           !rb->strcmp(item, item_id);
}

static int count_local_detail_index_entries(const struct abs_config *cfg,
                                            const char *library_id,
                                            bool existing_only)
{
    int fd;
    int count = 0;
    char line[INDEX_LINE_BUF_SIZE];

    if (cfg == NULL || library_id == NULL || library_id[0] == '\0')
        return 0;

    fd = rb->open(LOCAL_INDEX_PATH, O_RDONLY);
    if (fd < 0)
        return 0;

    while (rb->read_line(fd, line, sizeof(line)) > 0) {
        char parse_buf[INDEX_LINE_BUF_SIZE];
        char path_buf[MAX_PATH];

        rb->strlcpy(parse_buf, line, sizeof(parse_buf));
        if (!parse_local_index_line_for_item(parse_buf, cfg, library_id,
                                             g_book_detail.item_id,
                                             path_buf, sizeof(path_buf)))
            continue;
        if (existing_only &&
            (!local_index_path_is_safe(cfg, path_buf) || !rb->file_exists(path_buf)))
            continue;
        count++;
    }

    rb->close(fd);
    return count;
}

static void refresh_local_detail_state(const struct abs_config *cfg,
                                       const char *library_id)
{
    g_book_detail.local_file_found = false;
    g_book_detail.local_path[0] = '\0';
    g_book_detail.local_audio_file_count = 0;
    g_book_detail.local_index_entry_count = 0;
    g_book_detail.local_stale_index_count = 0;

    if (cfg == NULL || library_id == NULL || library_id[0] == '\0')
        return;

    g_book_detail.local_audio_file_count = count_local_indexed_detail_files(
        cfg, library_id, g_book_detail.local_path,
        sizeof(g_book_detail.local_path));
    g_book_detail.local_index_entry_count = count_local_detail_index_entries(
        cfg, library_id, false);
    g_book_detail.local_stale_index_count =
        g_book_detail.local_index_entry_count - g_book_detail.local_audio_file_count;
    if (g_book_detail.local_stale_index_count < 0)
        g_book_detail.local_stale_index_count = 0;

    g_book_detail.local_file_found = g_book_detail.local_audio_file_count > 0;
    if (g_book_detail.has_single_audio_file &&
        g_book_detail.single_audio_ino[0] != '\0' &&
        read_local_index_path(cfg, library_id,
                              g_book_detail.item_id,
                              g_book_detail.single_audio_ino,
                              g_book_detail.local_path,
                              sizeof(g_book_detail.local_path)))
        g_book_detail.local_file_found = true;
}

static bool play_local_detail_file(const struct abs_config *cfg,
                                   const char *library_id,
                                   char *error_buf,
                                   size_t error_len)
{
    int i;
    int queued = 0;

    if (cfg == NULL || library_id == NULL || library_id[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Cannot play local files without library context.");
        return false;
    }
    if (g_book_detail.local_audio_file_count <= 0) {
        rb->snprintf(error_buf, error_len,
                     g_book_detail.local_index_entry_count > 0 ?
                     "Indexed local files are missing or stale.\nUse Delete Local to repair the index or download again." :
                     "No local files indexed for this book.");
        return false;
    }

    if (!(rb->playlist_remove_all_tracks(NULL) == 0 &&
          rb->playlist_create(NULL, NULL) == 0)) {
        rb->snprintf(error_buf, error_len,
                     "Cannot create playback playlist.");
        return false;
    }

    for (i = 0; i < g_book_detail.tracked_audio_file_count; i++) {
        char path_buf[MAX_PATH];

        if (g_book_detail.audio_file_ids[i][0] == '\0' ||
            !read_local_index_path(cfg, library_id,
                                   g_book_detail.item_id,
                                   g_book_detail.audio_file_ids[i],
                                   path_buf, sizeof(path_buf)))
            continue;
        if (rb->playlist_insert_track(NULL, path_buf,
                                      queued, false, true) < 0) {
            rb->snprintf(error_buf, error_len,
                         "Cannot queue local file:\n%s",
                         path_buf);
            return false;
        }
        queued++;
    }

    if (queued <= 0) {
        rb->snprintf(error_buf, error_len,
                     "No playable local files remain.\nUse Delete Local to clear stale entries or download again.");
        return false;
    }

    rb->playlist_start(0, 0, 0);
    return true;
}

static bool delete_local_detail(const struct abs_config *cfg,
                                const char *library_id,
                                char *text_buf,
                                size_t text_len)
{
    int in_fd;
    int out_fd;
    int matched_entries = 0;
    int kept_entries = 0;
    int deleted_files = 0;
    int missing_files = 0;
    int delete_failures = 0;
    char line[INDEX_LINE_BUF_SIZE];
    char tmp_path[MAX_PATH];
    char backup_path[MAX_PATH];
    bool backed_up_old_index = false;
    int backup_idx;

    if (cfg == NULL || library_id == NULL || library_id[0] == '\0') {
        rb->snprintf(text_buf, text_len,
                     "Delete Local skipped\n\nMissing library context.");
        return false;
    }
    if (g_book_detail.local_index_entry_count <= 0) {
        rb->snprintf(text_buf, text_len,
                     "Delete Local skipped\n\nNo local index entries were found for this book.");
        return false;
    }
    if (!rb->yesno_pop("Delete local files for this book?")) {
        rb->snprintf(text_buf, text_len,
                     "Delete Local cancelled\n\nBook: %s",
                     g_book_detail.title);
        return false;
    }
    if (!ensure_directory_tree(SAFE_DOWNLOAD_BASE)) {
        rb->snprintf(text_buf, text_len,
                     "Delete Local failed\n\nCannot create %s",
                     SAFE_DOWNLOAD_BASE);
        return false;
    }

    in_fd = rb->open(LOCAL_INDEX_PATH, O_RDONLY);
    if (in_fd < 0) {
        rb->snprintf(text_buf, text_len,
                     "Delete Local skipped\n\nLocal index file was not found.");
        return false;
    }

    rb->snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", LOCAL_INDEX_PATH);
    rb->remove(tmp_path);
    out_fd = rb->open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        rb->close(in_fd);
        rb->snprintf(text_buf, text_len,
                     "Delete Local failed\n\nCannot write local index temp file.");
        return false;
    }

    while (rb->read_line(in_fd, line, sizeof(line)) > 0) {
        char parse_buf[INDEX_LINE_BUF_SIZE];
        char path_buf[MAX_PATH];
        bool keep_line = true;

        rb->strlcpy(parse_buf, line, sizeof(parse_buf));
        if (parse_local_index_line_for_item(parse_buf, cfg, library_id,
                                            g_book_detail.item_id,
                                            path_buf, sizeof(path_buf))) {
            matched_entries++;
            if (!local_index_path_is_safe(cfg, path_buf) || !rb->file_exists(path_buf)) {
                missing_files++;
                keep_line = false;
            } else if (rb->remove(path_buf) >= 0) {
                deleted_files++;
                keep_line = false;
            } else {
                delete_failures++;
            }
        }

        if (keep_line) {
            if (rb->fdprintf(out_fd, "%s\n", trim_ws(line)) < 0) {
                rb->close(in_fd);
                rb->close(out_fd);
                rb->remove(tmp_path);
                rb->snprintf(text_buf, text_len,
                             "Delete Local failed\n\nCannot rewrite local index.");
                return false;
            }
            kept_entries++;
        }
    }

    rb->close(in_fd);
    if (rb->close(out_fd) < 0) {
        rb->remove(tmp_path);
        rb->snprintf(text_buf, text_len,
                     "Delete Local failed\n\nCannot flush local index.");
        return false;
    }

    backup_path[0] = '\0';
    for (backup_idx = 0; backup_idx < 10; backup_idx++) {
        rb->snprintf(backup_path, sizeof(backup_path), "%s.bak%d",
                     LOCAL_INDEX_PATH, backup_idx);
        if (!rb->file_exists(backup_path))
            break;
    }
    if (backup_idx >= 10 || backup_path[0] == '\0') {
        rb->remove(tmp_path);
        rb->snprintf(text_buf, text_len,
                     "Delete Local failed\n\nCannot find safe local index backup path.");
        return false;
    }
    if (rb->rename(LOCAL_INDEX_PATH, backup_path) < 0) {
        rb->remove(tmp_path);
        rb->snprintf(text_buf, text_len,
                     "Delete Local failed\n\nCannot preserve local index before replace.");
        return false;
    }
    backed_up_old_index = true;

    if (matched_entries <= 0) {
        rb->remove(tmp_path);
        if (backed_up_old_index)
            rb->rename(backup_path, LOCAL_INDEX_PATH);
        rb->snprintf(text_buf, text_len,
                     "Delete Local skipped\n\nNo matching local index entries were found for this book.");
        return false;
    }

    if (rb->rename(tmp_path, LOCAL_INDEX_PATH) < 0) {
        rb->remove(tmp_path);
        if (backed_up_old_index)
            rb->rename(backup_path, LOCAL_INDEX_PATH);
        rb->snprintf(text_buf, text_len,
                     "Delete Local failed\n\nCannot finalize local index.");
        return false;
    }

    if (backed_up_old_index)
        rb->remove(backup_path);
    if (kept_entries == 0)
        rb->remove(LOCAL_INDEX_PATH);

    rb->snprintf(text_buf, text_len,
                 delete_failures > 0 ?
                 "Delete Local incomplete\n\nBook: %s\nDeleted files: %d\nCleared stale entries: %d\nDelete failures: %d\nRemaining index entries: %d" :
                 "Delete Local finished\n\nBook: %s\nDeleted files: %d\nCleared stale entries: %d\nRemaining index entries: %d",
                 g_book_detail.title,
                 deleted_files,
                 missing_files,
                 delete_failures,
                 kept_entries);

    return delete_failures == 0;
}

static void redact_token_text(const char *src,
                              const char *token,
                              char *dest,
                              size_t dest_len)
{
    const char *p = src;
    size_t token_len;
    size_t used = 0;
    const char *marker = "[redacted]";
    size_t marker_len = rb->strlen(marker);

    if (dest_len == 0)
        return;

    dest[0] = '\0';
    if (src == NULL || src[0] == '\0')
        return;

    token_len = token != NULL ? rb->strlen(token) : 0;
    while (*p != '\0' && used + 1 < dest_len) {
        if (token_len > 0 && rb->strncmp(p, token, token_len) == 0) {
            size_t copy = marker_len;

            if (copy > dest_len - used - 1)
                copy = dest_len - used - 1;
            rb->memcpy(dest + used, marker, copy);
            used += copy;
            dest[used] = '\0';
            p += token_len;
            continue;
        }
        dest[used++] = *p++;
        dest[used] = '\0';
    }
}

static const char *android_request_rc_name(int bridge_rc)
{
    switch (bridge_rc) {
    case ANDROID_REQUEST_OK:
        return "ok";
    case ANDROID_REQUEST_INVALID_PARAM:
        return "invalid_param";
    case ANDROID_REQUEST_JNI_UNAVAILABLE:
        return "jni_unavailable";
    case ANDROID_REQUEST_JNI_METHOD_MISSING:
        return "jni_method_missing";
    case ANDROID_REQUEST_JNI_EXCEPTION:
        return "jni_exception";
    case ANDROID_REQUEST_TRUNCATED:
        return "truncated";
    default:
        return "unknown";
    }
}

static bool is_auth_error_status(int http_status)
{
    return http_status == 401 || http_status == 403;
}

static bool http_status_is_success(int http_status)
{
    return http_status >= 200 && http_status < 300;
}

static bool android_request_status_ok(int http_status, int bridge_rc)
{
    return http_status_is_success(http_status) &&
           (bridge_rc >= 0 || bridge_rc == ANDROID_REQUEST_TRUNCATED);
}

static bool detail_has_manual_sync_mapping(void)
{
    return g_book_detail.item_id[0] != '\0' &&
           g_book_detail.duration_seconds > 0 &&
           g_book_detail.has_single_audio_file &&
           g_book_detail.local_file_found &&
           g_book_detail.local_path[0] != '\0';
}

static void init_progress_state(struct abs_progress_state *state)
{
    if (state == NULL)
        return;

    state->current_time = -1;
    state->duration = -1;
    state->percent = -1;
    state->known = false;
    state->finished = false;
}

static void parse_progress_state_object(const char *js,
                                        const jsmntok_t *tokens,
                                        int r,
                                        int obj_idx,
                                        struct abs_progress_state *state)
{
    int progress_idx;
    int current_time_idx;
    int duration_idx;
    int finished_idx;

    if (state == NULL || obj_idx < 0 || obj_idx >= r ||
        tokens[obj_idx].type != JSMN_OBJECT)
        return;

    progress_idx = find_object_value(js, tokens, r, obj_idx, "progress");
    if (progress_idx < 0)
        progress_idx = find_object_value(js, tokens, r, obj_idx, "percentage");
    if (progress_idx < 0)
        progress_idx = find_object_value(js, tokens, r, obj_idx, "percentComplete");
    current_time_idx = find_object_value(js, tokens, r, obj_idx, "currentTime");
    duration_idx = find_object_value(js, tokens, r, obj_idx, "duration");
    finished_idx = find_object_value(js, tokens, r, obj_idx, "isFinished");

    if (progress_idx >= 0)
        state->percent = tok_to_percent(js, &tokens[progress_idx]);
    if (current_time_idx >= 0)
        state->current_time = tok_to_int(js, &tokens[current_time_idx]);
    if (duration_idx >= 0)
        state->duration = tok_to_int(js, &tokens[duration_idx]);
    if (finished_idx >= 0)
        (void)tok_to_bool(js, &tokens[finished_idx], &state->finished);

    state->known = state->finished || state->percent >= 0 || state->current_time >= 0;
}

static bool finalize_remote_progress_state(struct abs_progress_state *state)
{
    if (state == NULL)
        return false;

    if (state->duration <= 0)
        state->duration = g_book_detail.duration_seconds;
    if (state->finished) {
        state->known = true;
        if (state->duration > 0)
            state->current_time = state->duration;
        state->percent = 100;
    } else if (state->percent < 0 && state->current_time >= 0 && state->duration > 0) {
        state->percent = (state->current_time * 100) / state->duration;
    }

    return state->known;
}

static bool extract_progress_state_from_json(const char *json,
                                             struct abs_progress_state *state,
                                             char *error_buf,
                                             size_t error_len)
{
    jsmn_parser parser;
    int r;
    int media_idx;
    int progress_idx;

    init_progress_state(state);
    if (json == NULL || json[0] == '\0') {
        if (error_buf != NULL && error_len > 0)
            rb->snprintf(error_buf, error_len, "empty JSON response");
        return false;
    }

    jsmn_init(&parser);
    r = jsmn_parse(&parser, json, rb->strlen(json), g_tokens, JSON_TOKEN_COUNT);
    if (r <= 0) {
        if (error_buf != NULL && error_len > 0)
            rb->snprintf(error_buf, error_len, "JSON parse failed (%d)", r);
        return false;
    }
    if (g_tokens[0].type != JSMN_OBJECT) {
        if (error_buf != NULL && error_len > 0)
            rb->snprintf(error_buf, error_len, "expected JSON object");
        return false;
    }

    progress_idx = find_object_value(json, g_tokens, r, 0, "mediaProgress");
    if (progress_idx < 0)
        progress_idx = find_object_value(json, g_tokens, r, 0, "userMediaProgress");

    media_idx = find_object_value(json, g_tokens, r, 0, "media");
    if (progress_idx < 0 && media_idx >= 0 && g_tokens[media_idx].type == JSMN_OBJECT) {
        progress_idx = find_object_value(json, g_tokens, r, media_idx,
                                         "mediaProgress");
        if (progress_idx < 0)
            progress_idx = find_object_value(json, g_tokens, r, media_idx,
                                             "userMediaProgress");
    }

    if (progress_idx >= 0) {
        parse_progress_state_object(json, g_tokens, r, progress_idx, state);
    } else {
        parse_progress_state_object(json, g_tokens, r, 0, state);
    }

    if (!finalize_remote_progress_state(state)) {
        if (error_buf != NULL && error_len > 0)
            rb->snprintf(error_buf, error_len,
                         "response did not include usable progress fields");
        return false;
    }

    return true;
}

static bool extract_detail_remote_progress(struct abs_progress_state *state)
{
    return extract_progress_state_from_json(g_detail_response, state, NULL, 0);
}

static bool build_local_progress_snapshot(struct abs_progress_snapshot *snapshot,
                                          char *error_buf,
                                          size_t error_len)
{
    struct mp3entry *current;
    int duration_ms;
    int elapsed_ms;

    if (snapshot == NULL) {
        rb->snprintf(error_buf, error_len, "Manual sync snapshot is unavailable.");
        return false;
    }

    rb->memset(snapshot, 0, sizeof(*snapshot));
    if (!detail_has_manual_sync_mapping()) {
        rb->snprintf(error_buf, error_len,
                     "Manual sync needs one downloaded single-file book with a local Audiobookshelf mapping.");
        return false;
    }

    current = rb->audio_current_track();
    if (current == NULL || current->path[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Manual sync spike only supports the selected book while it is currently playing.");
        return false;
    }
    if (rb->strcmp(current->path, g_book_detail.local_path) != 0) {
        rb->snprintf(error_buf, error_len,
                     "Current track does not match this downloaded book.\n\nExpected:\n%s\n\nPlaying:\n%s",
                     g_book_detail.local_path,
                     current->path);
        return false;
    }

    duration_ms = (int)current->length;
    elapsed_ms = (int)current->elapsed;
    if (duration_ms <= 0 && g_book_detail.duration_seconds > 0)
        duration_ms = g_book_detail.duration_seconds * 1000;
    if (duration_ms <= 0) {
        rb->snprintf(error_buf, error_len,
                     "Cannot determine playback duration for the selected local file.");
        return false;
    }
    if (elapsed_ms < 0)
        elapsed_ms = 0;
    if (elapsed_ms > duration_ms)
        elapsed_ms = duration_ms;

    rb->strlcpy(snapshot->local_path, g_book_detail.local_path,
                sizeof(snapshot->local_path));
    snapshot->current_time = elapsed_ms / 1000;
    snapshot->duration = duration_ms / 1000;
    if (snapshot->duration <= 0)
        snapshot->duration = g_book_detail.duration_seconds;
    if (snapshot->duration <= 0)
        snapshot->duration = 1;
    if (snapshot->current_time > snapshot->duration)
        snapshot->current_time = snapshot->duration;
    snapshot->percent = (snapshot->current_time * 100) / snapshot->duration;
    snapshot->finished = snapshot->current_time >= snapshot->duration;
    return true;
}

static void format_request_failure(const char *title,
                                   const struct abs_config *cfg,
                                   const char *wifi_result,
                                   int http_status,
                                   int bridge_rc,
                                   const char *error_text,
                                   char *text_buf,
                                   size_t text_len)
{
    char redacted_error[ERROR_BUF_SIZE];
    const char *hint;
    const char *summary;

    redact_token_text(error_text, cfg->token,
                      redacted_error, sizeof(redacted_error));

    if (is_auth_error_status(http_status)) {
        summary = "auth_error";
        hint = "Hint: token rejected; verify token in " CONFIG_PATH;
    } else if (bridge_rc < 0 || http_status == 0) {
        summary = "connection_error";
        hint = "Hint: no valid HTTP response; check WiFi, server URL, and bridge error";
    } else {
        summary = "server_error";
        hint = "Hint: server responded with an error; check server health/logs";
    }

    rb->snprintf(text_buf, text_len,
                 "%s\n\n"
                 "%s\n\n"
                 "Server:      %s\n"
                 "WiFi:        %s\n"
                 "HTTP status: %d\n"
                 "Bridge rc:   %d (%s)\n"
                 "%s\n\n"
                 "Error:\n%s",
                 title,
                 summary,
                 cfg->server_url,
                 wifi_result ? wifi_result : "(null)",
                 http_status,
                 bridge_rc,
                 android_request_rc_name(bridge_rc),
                 hint,
                 redacted_error[0] ? redacted_error : "(none)");
}

static const char *config_error_hint =
    "Expected entries in " CONFIG_PATH ":\n"
    "  server_url: https://your.abs.server\n"
    "  token: your_api_token\n"
    "  library_id: (optional)\n"
    "  download_dir: (optional)\n"
    "  page_size: (optional, default 20)";

static bool normalize_server_url(char *server_url,
                                 char *error_buf,
                                 size_t error_len)
{
    size_t len;

    if (server_url[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Missing 'server_url' in " CONFIG_PATH "\n\n%s",
                     config_error_hint);
        return false;
    }

    len = rb->strlen(server_url);
    while (len > 0 && server_url[len - 1] == '/')
        server_url[--len] = '\0';

    if (!rb->strncmp(server_url, "http://", 7)) {
        rb->snprintf(error_buf, error_len,
                     "Plain HTTP is not allowed.\n\n"
                     "Update " CONFIG_PATH ":\n"
                     "  server_url: https://your.abs.server\n\n"
                     "HTTP sends your API token in the clear.");
        server_url[0] = '\0';
        return false;
    }

    if (rb->strncmp(server_url, "https://", 8)) {
        rb->snprintf(error_buf, error_len,
                     "server_url must start with https://\n\n"
                     "Found: %s\n\n%s",
                     server_url, config_error_hint);
        server_url[0] = '\0';
        return false;
    }

    return true;
}

static bool apply_config_value(struct abs_config *cfg,
                               const char *key,
                               char *value,
                               char *error_buf,
                               size_t error_len)
{
    if (!rb->strcmp(key, "server_url")) {
        rb->strlcpy(cfg->server_url, value, sizeof(cfg->server_url));
    } else if (!rb->strcmp(key, "token")) {
        rb->strlcpy(cfg->token, value, sizeof(cfg->token));
    } else if (!rb->strcmp(key, "library_id")) {
        rb->strlcpy(cfg->library_id, value, sizeof(cfg->library_id));
    } else if (!rb->strcmp(key, "download_dir")) {
        if (value[0] != '\0')
            rb->strlcpy(cfg->download_dir, value, sizeof(cfg->download_dir));
    } else if (!rb->strcmp(key, "page_size")) {
        if (value[0] != '\0')
            cfg->page_size = rb->atoi(value);
    } else {
        /* Forward-compatible: ignore unknown keys, but malformed lines still fail. */
        (void)error_buf;
        (void)error_len;
    }

    return true;
}

static bool parse_config_line(struct abs_config *cfg,
                              char *line,
                              char *error_buf,
                              size_t error_len)
{
    char *t = trim_ws(line);
    char *sep;
    char *key;
    char *value;

    if (t[0] == '\0' || t[0] == '#')
        return true;

    sep = rb->strchr(t, ':');
    if (sep == NULL) {
        rb->snprintf(error_buf, error_len,
                     "Malformed config line in " CONFIG_PATH ":\n%s",
                     t);
        return false;
    }

    *sep = '\0';
    key = trim_ws(t);
    value = trim_ws(sep + 1);

    if (key[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Malformed config key in " CONFIG_PATH ":\n%s",
                     t);
        return false;
    }

    return apply_config_value(cfg, key, value, error_buf, error_len);
}

static void init_config_defaults(struct abs_config *cfg)
{
    cfg->server_url[0] = '\0';
    cfg->token[0]      = '\0';
    cfg->library_id[0] = '\0';
    cfg->page_size     = DEFAULT_PAGE_SIZE;
    rb->strlcpy(cfg->download_dir, DEFAULT_DOWNLOAD_DIR,
                sizeof(cfg->download_dir));
}

static bool validate_config(struct abs_config *cfg,
                            char *error_buf,
                            size_t error_len)
{
    cfg->page_size = sanitize_page_size(cfg->page_size);

    if (!normalize_server_url(cfg->server_url, error_buf, error_len))
        return false;

    if (cfg->token[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Missing 'token' in " CONFIG_PATH "\n\n%s",
                     config_error_hint);
        return false;
    }

    return true;
}

static bool parse_config_text(struct abs_config *cfg,
                              char *text,
                              char *error_buf,
                              size_t error_len)
{
    char *line = text;

    init_config_defaults(cfg);

    while (line != NULL && line[0] != '\0') {
        char *next = rb->strchr(line, '\n');

        if (next != NULL)
            *next++ = '\0';

        if (!parse_config_line(cfg, line, error_buf, error_len))
            return false;

        line = next;
    }

    return validate_config(cfg, error_buf, error_len);
}

static bool validate_download_root(const struct abs_config *cfg,
                                   char *root_buf,
                                   size_t root_len,
                                   char *error_buf,
                                   size_t error_len)
{
    size_t len;

    if (cfg->download_dir[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Download root is empty in " CONFIG_PATH);
        return false;
    }
    if (cfg->download_dir[0] != '/') {
        rb->snprintf(error_buf, error_len,
                     "Download root must be an absolute path:\n%s",
                     cfg->download_dir);
        return false;
    }
    if (path_has_traversal(cfg->download_dir)) {
        rb->snprintf(error_buf, error_len,
                     "Download root rejected before bridge: path traversal is not allowed:\n%s",
                     cfg->download_dir);
        return false;
    }
    if (!path_is_under_base(cfg->download_dir, SAFE_DOWNLOAD_BASE)) {
        rb->snprintf(error_buf, error_len,
                     "Download root must stay under %s:\n%s",
                     SAFE_DOWNLOAD_BASE, cfg->download_dir);
        return false;
    }
    if (path_is_under_base(cfg->download_dir, "/sdcard/.rockbox")) {
        rb->snprintf(error_buf, error_len,
                     "Download root may not be inside /sdcard/.rockbox:\n%s",
                     cfg->download_dir);
        return false;
    }

    rb->strlcpy(root_buf, cfg->download_dir, root_len);
    len = rb->strlen(root_buf);
    while (len > 1 && root_buf[len - 1] == '/')
        root_buf[--len] = '\0';

    return true;
}

static bool build_download_destination(const char *root,
                                       const char *title,
                                       const char *item_id,
                                       const char *media_file_id,
                                       const char *file_ext,
                                       int file_number,
                                       int total_files,
                                       char *dest_buf,
                                       size_t dest_len,
                                       char *error_buf,
                                       size_t error_len)
{
    char folder[BOOK_TITLE_SIZE];
    char file_title[BOOK_TITLE_SIZE];
    char file_id[BOOK_ID_SIZE];
    char media_id[BOOK_ID_SIZE];

    sanitize_path_component(title, folder, sizeof(folder));
    sanitize_path_component(title, file_title, sizeof(file_title));
    sanitize_path_component(item_id, file_id, sizeof(file_id));
    sanitize_path_component(media_file_id, media_id, sizeof(media_id));

    if (path_has_traversal(folder) || path_has_traversal(file_title) ||
        path_has_traversal(file_id) || path_has_traversal(media_id)) {
        rb->snprintf(error_buf, error_len,
                     "Download destination rejected before bridge: sanitized title is unsafe");
        return false;
    }
    if (file_ext == NULL || file_ext[0] != '.') {
        rb->snprintf(error_buf, error_len,
                     "Download destination requires a usable file extension");
        return false;
    }

    if (total_files > 1) {
        rb->snprintf(dest_buf, dest_len, "%s/%s/%03d-%s-%s-%s%s",
                     root,
                     folder,
                     file_number + 1,
                     file_title,
                     file_id,
                     media_id,
                     file_ext);
    } else {
        rb->snprintf(dest_buf, dest_len, "%s/%s/%s-%s-%s%s",
                     root,
                     folder,
                     file_title,
                     file_id,
                     media_id,
                     file_ext);
    }

    if (dest_buf[0] == '\0' || rb->strlen(dest_buf) >= dest_len - 1) {
        rb->snprintf(error_buf, error_len,
                     "Download destination is too long");
        return false;
    }
    if (path_has_traversal(dest_buf) || !path_is_under_base(dest_buf, root)) {
        rb->snprintf(error_buf, error_len,
                     "Download destination rejected before bridge:\n%s",
                     dest_buf);
        return false;
    }

    return true;
}

static int count_local_indexed_detail_files(const struct abs_config *cfg,
                                            const char *library_id,
                                            char *first_path,
                                            size_t first_path_len)
{
    int i;
    int found = 0;
    char path_buf[MAX_PATH];

    if (first_path != NULL && first_path_len > 0)
        first_path[0] = '\0';
    if (cfg == NULL || library_id == NULL || library_id[0] == '\0')
        return 0;

    for (i = 0; i < g_book_detail.tracked_audio_file_count; i++) {
        if (g_book_detail.audio_file_ids[i][0] == '\0' ||
            g_book_detail.audio_file_exts[i][0] == '\0')
            continue;
        if (!read_local_index_path(cfg, library_id,
                                   g_book_detail.item_id,
                                   g_book_detail.audio_file_ids[i],
                                   path_buf, sizeof(path_buf)))
            continue;
        if (found == 0 && first_path != NULL && first_path_len > 0)
            rb->strlcpy(first_path, path_buf, first_path_len);
        found++;
    }

    return found;
}

static bool download_book(const struct abs_config *cfg,
                          const char *header_buf,
                          const char *library_name,
                          const char *library_id,
                          const char *item_id,
                          char *text_buf,
                          size_t text_len)
{
    char endpoint[ENDPOINT_BUF_SIZE];
    char root_buf[DLOAD_DIR_BUF_SIZE];
    char book_dir[MAX_PATH];
    char dest_buf[MAX_PATH];
    char tmp_dest_buf[MAX_PATH];
    char error_buf[ERROR_BUF_SIZE];
    char redacted_error[ERROR_BUF_SIZE];
    char last_media_id[BOOK_ID_SIZE];
    const char *wifi_result = "not needed";
    const char *book_title;
    bool overall_success;
    bool wifi_connected = false;
    int http_status = 0;
    int bridge_rc = 0;
    int downloaded_now = 0;
    int already_downloaded = 0;
    int failed = 0;
    int unsupported_files = 0;
    int total_files;
    int i;

    book_title = g_book_detail.title[0] != '\0' ? g_book_detail.title : "(untitled)";
    error_buf[0] = '\0';
    redacted_error[0] = '\0';
    last_media_id[0] = '\0';

    if (g_book_detail.audio_files_known && !g_book_detail.has_audio_files) {
        rb->snprintf(text_buf, text_len,
                     "Download skipped\n\n"
                     "Book: %s\n"
                     "Book ID: %s\n\n"
                     "Reason: item detail explicitly reports zero audio files.",
                     book_title,
                     item_id);
        return false;
    }

    total_files = g_book_detail.audio_files_known ?
        g_book_detail.audio_file_count : g_book_detail.downloadable_audio_file_count;
    unsupported_files = total_files - g_book_detail.downloadable_audio_file_count;
    if (unsupported_files < 0)
        unsupported_files = 0;
    failed = unsupported_files;
    if (unsupported_files > 0) {
        rb->snprintf(error_buf, sizeof(error_buf),
                     "%d audio file(s) lack usable ids/extensions or exceed the tracked limit",
                     unsupported_files);
    }
    if (total_files <= 0 || g_book_detail.downloadable_audio_file_count <= 0) {
        rb->snprintf(text_buf, text_len,
                     "Download skipped\n\n"
                     "Book: %s\n"
                     "Book ID: %s\n\n"
                     "Reason: no downloadable audio files with usable ids/extensions were parsed.",
                     book_title,
                     item_id);
        return false;
    }

    if (!validate_download_root(cfg, root_buf, sizeof(root_buf),
                                error_buf, sizeof(error_buf))) {
        rb->snprintf(text_buf, text_len,
                     "Download blocked\n\n"
                     "Book: %s\n"
                     "Book ID: %s\n\n"
                     "%s",
                     book_title,
                     item_id,
                     error_buf);
        return false;
    }

    for (i = 0; i < g_book_detail.tracked_audio_file_count; i++) {
        char indexed_path[MAX_PATH];
        bool file_success = false;
        bool finalized_file = false;

        if (g_book_detail.audio_file_ids[i][0] == '\0' ||
            g_book_detail.audio_file_exts[i][0] == '\0')
            continue;

        rb->strlcpy(last_media_id, g_book_detail.audio_file_ids[i],
                    sizeof(last_media_id));
        error_buf[0] = '\0';
        redacted_error[0] = '\0';
        http_status = 0;
        bridge_rc = 0;

        if (!build_download_destination(root_buf, book_title, item_id,
                                        g_book_detail.audio_file_ids[i],
                                        g_book_detail.audio_file_exts[i],
                                        i, total_files,
                                        dest_buf, sizeof(dest_buf),
                                        error_buf, sizeof(error_buf))) {
            failed++;
            redact_token_text(error_buf, cfg->token,
                              redacted_error, sizeof(redacted_error));
            break;
        }

        rb->strlcpy(book_dir, dest_buf, sizeof(book_dir));
        {
            char *slash = rb->strrchr(book_dir, '/');
            if (slash == NULL) {
                rb->snprintf(error_buf, sizeof(error_buf),
                             "Cannot derive destination folder.");
                failed++;
                break;
            }
            *slash = '\0';
        }
        if (!ensure_directory_tree(book_dir)) {
            rb->snprintf(error_buf, sizeof(error_buf),
                         "Cannot create destination folder:\n%s",
                         book_dir);
            failed++;
            break;
        }

        if (!build_temp_download_path(dest_buf, tmp_dest_buf, sizeof(tmp_dest_buf))) {
            rb->snprintf(error_buf, sizeof(error_buf),
                         "Temporary path is too long for:\n%s",
                         dest_buf);
            failed++;
            break;
        }

        if (rb->file_exists(dest_buf)) {
            if (read_local_index_path(cfg, library_id, item_id,
                                      g_book_detail.audio_file_ids[i],
                                      indexed_path, sizeof(indexed_path)) &&
                !rb->strcmp(indexed_path, dest_buf)) {
                already_downloaded++;
                continue;
            }

            rb->snprintf(error_buf, sizeof(error_buf),
                         "Destination already exists but is not indexed; not overwriting:\n%s",
                         dest_buf);
            failed++;
            redact_token_text(error_buf, cfg->token,
                              redacted_error, sizeof(redacted_error));
            continue;
        }

        rb->remove(tmp_dest_buf);

        if (rb->snprintf(endpoint, sizeof(endpoint),
                         "%s/api/items/%s/file/%s/download",
                         cfg->server_url, item_id,
                         g_book_detail.audio_file_ids[i]) >= (int)sizeof(endpoint)) {
            rb->snprintf(error_buf, sizeof(error_buf),
                         "Download endpoint is too long.");
            failed++;
            break;
        }

        if (!wifi_connected) {
            rb->splash(HZ, "WiFi: connecting...");
            wifi_result = rb->android_connect_wifi();
            wifi_connected = true;
        }

        rb->splash_progress(i + 1, total_files, "Downloading audio files...");
        bridge_rc = rb->android_download(endpoint,
                                         header_buf,
                                         tmp_dest_buf,
                                         &http_status,
                                         error_buf,
                                         sizeof(error_buf));

        file_success = bridge_rc >= 0 && http_status >= 200 && http_status < 300 &&
                       rb->file_exists(tmp_dest_buf);

        if (file_success && !promote_temp_file_exclusive(tmp_dest_buf, dest_buf,
                                                         error_buf, sizeof(error_buf))) {
            file_success = false;
        } else if (file_success) {
            finalized_file = true;
        }
        if (file_success && !write_local_index_path(cfg, library_id, item_id,
                                                    g_book_detail.audio_file_ids[i],
                                                    dest_buf,
                                                    error_buf, sizeof(error_buf))) {
            file_success = false;
            if (finalized_file)
                rb->remove(dest_buf);
        }

        if (!file_success) {
            failed++;
            rb->remove(tmp_dest_buf);
            redact_token_text(error_buf, cfg->token,
                              redacted_error, sizeof(redacted_error));
            continue;
        }

        downloaded_now++;
    }

    if (wifi_connected)
        rb->android_disconnect_wifi();

    overall_success = failed == 0 && (downloaded_now > 0 || already_downloaded > 0);

    rb->snprintf(text_buf, text_len,
                 "%s\n\n"
                 "Server: %s\n"
                 "Library: %s\n"
                 "Library ID: %s\n"
                 "Book: %s\n"
                 "Book ID: %s\n"
                 "Downloaded: %d/%d\n"
                 "Downloaded now: %d\n"
                 "Already complete: %d\n"
                 "Failures: %d\n"
                 "Last audio file ID: %s\n"
                 "Last destination: %s\n"
                 "WiFi: %s\n"
                 "HTTP status: %d\n"
                 "Bridge rc: %d\n"
                 "Bridge error (redacted): %s",
                 overall_success ? (already_downloaded == total_files ? "Already downloaded" : "Download finished") : "Download incomplete",
                 cfg->server_url,
                 library_name && library_name[0] ? library_name : "(unknown)",
                 library_id && library_id[0] ? library_id : "(unknown)",
                 book_title,
                 item_id && item_id[0] ? item_id : "(unknown)",
                 downloaded_now + already_downloaded,
                 total_files,
                 downloaded_now,
                 already_downloaded,
                 failed,
                 last_media_id[0] ? last_media_id : "(unknown)",
                 dest_buf,
                 safe_text(wifi_result),
                 http_status,
                 bridge_rc,
                 safe_text(redacted_error));

    if (downloaded_now > 0 || already_downloaded > 0) {
        g_book_detail.local_audio_file_count = downloaded_now + already_downloaded;
        if (g_book_detail.has_single_audio_file)
            g_book_detail.local_file_found = true;
    }

    return overall_success;
}

/* ---- config parsing ------------------------------------------------------- */

static bool read_config(struct abs_config *cfg,
                        char *error_buf, size_t error_len)
{
    int fd;
    char line[LINE_BUF_SIZE];

    init_config_defaults(cfg);

    fd = rb->open(CONFIG_PATH, O_RDONLY);
    if (fd < 0) {
        rb->snprintf(error_buf, error_len,
                     "Cannot open " CONFIG_PATH "\n\n%s", config_error_hint);
        return false;
    }

    while (rb->read_line(fd, line, sizeof(line)) > 0) {
        if (!parse_config_line(cfg, line, error_buf, error_len)) {
            rb->close(fd);
            return false;
        }
    }

    rb->close(fd);
    return validate_config(cfg, error_buf, error_len);
}

/* ---- JSON library parsing ------------------------------------------------- */

/*
 * Parse the Audiobookshelf /api/libraries response stored in g_lib_response
 * into g_lib_names[] and g_lib_ids[]. Sets g_lib_count on success.
 *
 * Expected shape: {"libraries": [{"id": "...", "name": "...", ...}, ...]}
 *
 * Returns the number of libraries parsed (>= 0) or -1 on parse error.
 * On error, error_buf contains a diagnostic that does not include credentials.
 */
static int parse_libraries(int json_len,
                           char *error_buf, size_t error_len)
{
    jsmn_parser parser;
    int r, i;

    g_lib_count = 0;
    jsmn_init(&parser);
    r = jsmn_parse(&parser, g_lib_response, (size_t)json_len,
                   g_tokens, JSON_TOKEN_COUNT);

    if (r == JSMN_ERROR_NOMEM) {
        rb->snprintf(error_buf, error_len,
                     "Library JSON has more than %d tokens; "
                     "set library_id in " CONFIG_PATH " to skip selection.",
                     JSON_TOKEN_COUNT);
        return -1;
    }
    if (r == JSMN_ERROR_PART) {
        rb->snprintf(error_buf, error_len,
                     "Library response appears truncated (incomplete JSON). "
                     "Set library_id in " CONFIG_PATH " to skip selection.");
        return -1;
    }
    if (r < 0) {
        rb->snprintf(error_buf, error_len,
                     "Malformed library JSON (parse error %d). "
                     "Response starts: %.40s",
                     r, g_lib_response[0] ? g_lib_response : "(empty)");
        return -1;
    }
    if (r == 0 || g_tokens[0].type != JSMN_OBJECT) {
        rb->snprintf(error_buf, error_len,
                     "Unexpected library response: expected JSON object, "
                     "got %d token(s). Starts: %.40s",
                     r, g_lib_response[0] ? g_lib_response : "(empty)");
        return -1;
    }

    /* Locate the "libraries" array in the root object. */
    i = find_object_value(g_lib_response, g_tokens, r, 0, "libraries");
    if (i < 0 || g_tokens[i].type != JSMN_ARRAY) {
        rb->snprintf(error_buf, error_len,
                     "Library response has no 'libraries' array (%d tokens). "
                     "Starts: %.40s",
                     r, g_lib_response);
        return -1;
    }

    /* Walk each library object inside the array. */
    {
        int arr_end = g_tokens[i].end;
        int arr_idx = i + 1;

        while (arr_idx < r && g_tokens[arr_idx].start < arr_end &&
               g_lib_count < MAX_LIBRARIES) {
            int id_idx;
            int name_idx;

            if (g_tokens[arr_idx].type != JSMN_OBJECT) {
                arr_idx = skip_value(arr_idx, r, g_tokens);
                continue;
            }

            id_idx = find_object_value(g_lib_response, g_tokens, r,
                                       arr_idx, "id");
            name_idx = find_object_value(g_lib_response, g_tokens, r,
                                         arr_idx, "name");

            if (id_idx >= 0 && g_tokens[id_idx].type == JSMN_STRING) {
                tok_copy(g_lib_response, &g_tokens[id_idx],
                         g_lib_ids[g_lib_count], sizeof(g_lib_ids[g_lib_count]));

                if (name_idx >= 0 && g_tokens[name_idx].type == JSMN_STRING) {
                    tok_copy(g_lib_response, &g_tokens[name_idx],
                             g_lib_names[g_lib_count],
                             sizeof(g_lib_names[g_lib_count]));
                } else {
                    rb->strlcpy(g_lib_names[g_lib_count], g_lib_ids[g_lib_count],
                                sizeof(g_lib_names[g_lib_count]));
                }

                g_lib_count++;
            }

            arr_idx = skip_value(arr_idx, r, g_tokens);
        }
    }

    return g_lib_count;
}

/* ---- JSON item parsing ---------------------------------------------------- */

/*
 * Expected common shape:
 * {
 *   "results": [
 *     {
 *       "id": "...",
 *       "media": {
 *         "metadata": {
 *           "title": "...",
 *           "authorName": "...",
 *           "seriesName": "..."
 *         }
 *       }
 *     }
 *   ],
 *   "page": 0,
 *   "limit": 20,
 *   "total": 123
 * }
 */
static int parse_books(int json_len, int requested_page, int requested_limit,
                       char *error_buf, size_t error_len)
{
    jsmn_parser parser;
    int r;
    int results_idx;
    int total_idx;
    int page_idx;
    int limit_idx;
    int i;

    g_book_count    = 0;
    g_book_page     = requested_page;
    g_book_limit    = requested_limit;
    g_book_total    = -1;
    g_book_has_next = false;

    jsmn_init(&parser);
    r = jsmn_parse(&parser, g_items_response, (size_t)json_len,
                   g_tokens, JSON_TOKEN_COUNT);

    if (r == JSMN_ERROR_NOMEM) {
        rb->snprintf(error_buf, error_len,
                     "Book list JSON has more than %d tokens. "
                     "Reduce page_size in " CONFIG_PATH ".",
                     JSON_TOKEN_COUNT);
        return -1;
    }
    if (r == JSMN_ERROR_PART) {
        rb->snprintf(error_buf, error_len,
                     "Book list response appears truncated (incomplete JSON). "
                     "Reduce page_size in " CONFIG_PATH ".");
        return -1;
    }
    if (r < 0) {
        rb->snprintf(error_buf, error_len,
                     "Malformed book list JSON (parse error %d). "
                     "Response starts: %.40s",
                     r, g_items_response[0] ? g_items_response : "(empty)");
        return -1;
    }
    if (r == 0 || g_tokens[0].type != JSMN_OBJECT) {
        rb->snprintf(error_buf, error_len,
                     "Unexpected book list response: expected JSON object, "
                     "got %d token(s). Starts: %.40s",
                     r, g_items_response[0] ? g_items_response : "(empty)");
        return -1;
    }

    results_idx = find_object_value(g_items_response, g_tokens, r, 0, "results");
    if (results_idx < 0)
        results_idx = find_object_value(g_items_response, g_tokens, r, 0,
                                        "libraryItems");
    if (results_idx < 0 || g_tokens[results_idx].type != JSMN_ARRAY) {
        rb->snprintf(error_buf, error_len,
                     "Book list response has no 'results' array (%d tokens). "
                     "Starts: %.40s",
                     r, g_items_response[0] ? g_items_response : "(empty)");
        return -1;
    }

    total_idx = find_object_value(g_items_response, g_tokens, r, 0, "total");
    page_idx = find_object_value(g_items_response, g_tokens, r, 0, "page");
    limit_idx = find_object_value(g_items_response, g_tokens, r, 0, "limit");

    if (total_idx >= 0)
        g_book_total = tok_to_int(g_items_response, &g_tokens[total_idx]);
    if (page_idx >= 0)
        g_book_page = tok_to_int(g_items_response, &g_tokens[page_idx]);
    if (limit_idx >= 0)
        g_book_limit = tok_to_int(g_items_response, &g_tokens[limit_idx]);

    if (g_book_limit <= 0)
        g_book_limit = MAX_BOOKS_PER_PAGE;

    i = results_idx + 1;
    while (i < r && g_tokens[i].start < g_tokens[results_idx].end &&
           g_book_count < MAX_BOOKS_PER_PAGE) {
        int id_idx;
        int title_idx;
        int media_idx;
        int metadata_idx;
        int author_idx = -1;
        int series_idx = -1;
        int downloaded_idx = -1;
        int progress_obj_idx = -1;
        int progress_idx = -1;
        int current_time_idx = -1;
        int duration_idx = -1;
        int finished_idx = -1;
        int progress_percent = -1;
        int current_time = -1;
        int duration = -1;
        bool downloaded = false;
        bool downloaded_known = false;
        bool finished = false;
        char id[BOOK_ID_SIZE];
        char title[BOOK_TITLE_SIZE];
        char author[BOOK_META_SIZE];
        char series[BOOK_META_SIZE];
        char download_state[8];
        char progress_state[16];

        if (g_tokens[i].type != JSMN_OBJECT) {
            i = skip_value(i, r, g_tokens);
            continue;
        }

        id[0] = '\0';
        title[0] = '\0';
        author[0] = '\0';
        series[0] = '\0';
        rb->strlcpy(download_state, "--", sizeof(download_state));
        rb->strlcpy(progress_state, "--", sizeof(progress_state));

        id_idx = find_object_value(g_items_response, g_tokens, r, i, "id");
        title_idx = find_object_value(g_items_response, g_tokens, r, i, "title");
        if (title_idx < 0)
            title_idx = find_object_value(g_items_response, g_tokens, r, i, "name");
        author_idx = find_object_value(g_items_response, g_tokens, r, i,
                                       "authorName");
        series_idx = find_object_value(g_items_response, g_tokens, r, i,
                                       "seriesName");

        media_idx = find_object_value(g_items_response, g_tokens, r, i, "media");
        downloaded_idx = find_object_value(g_items_response, g_tokens, r, i,
                                           "isDownloaded");
        if (downloaded_idx < 0)
            downloaded_idx = find_object_value(g_items_response, g_tokens, r, i,
                                               "downloaded");
        progress_obj_idx = find_object_value(g_items_response, g_tokens, r, i,
                                             "mediaProgress");
        if (progress_obj_idx < 0)
            progress_obj_idx = find_object_value(g_items_response, g_tokens, r, i,
                                                 "userMediaProgress");
        progress_idx = find_object_value(g_items_response, g_tokens, r, i,
                                         "progress");
        if (progress_idx < 0)
            progress_idx = find_object_value(g_items_response, g_tokens, r, i,
                                             "percentComplete");
        current_time_idx = find_object_value(g_items_response, g_tokens, r, i,
                                             "currentTime");
        duration_idx = find_object_value(g_items_response, g_tokens, r, i,
                                         "duration");
        finished_idx = find_object_value(g_items_response, g_tokens, r, i,
                                         "isFinished");

        if (media_idx >= 0 && g_tokens[media_idx].type == JSMN_OBJECT) {
            metadata_idx = find_object_value(g_items_response, g_tokens, r,
                                             media_idx, "metadata");
            if (metadata_idx >= 0 && g_tokens[metadata_idx].type == JSMN_OBJECT) {
                if (title_idx < 0)
                    title_idx = find_object_value(g_items_response, g_tokens, r,
                                                  metadata_idx, "title");
                if (author_idx < 0)
                    author_idx = find_object_value(g_items_response, g_tokens, r,
                                                   metadata_idx, "authorName");
                if (series_idx < 0)
                    series_idx = find_object_value(g_items_response, g_tokens, r,
                                                   metadata_idx, "seriesName");
            }
            if (downloaded_idx < 0)
                downloaded_idx = find_object_value(g_items_response, g_tokens, r,
                                                   media_idx, "isDownloaded");
            if (progress_obj_idx < 0)
                progress_obj_idx = find_object_value(g_items_response, g_tokens, r,
                                                     media_idx, "mediaProgress");
            if (progress_obj_idx < 0)
                progress_obj_idx = find_object_value(g_items_response, g_tokens, r,
                                                     media_idx, "userMediaProgress");
            if (duration_idx < 0)
                duration_idx = find_object_value(g_items_response, g_tokens, r,
                                                 media_idx, "duration");
        }

        if (progress_obj_idx >= 0 && g_tokens[progress_obj_idx].type == JSMN_OBJECT) {
            if (progress_idx < 0)
                progress_idx = find_object_value(g_items_response, g_tokens, r,
                                                 progress_obj_idx, "progress");
            if (progress_idx < 0)
                progress_idx = find_object_value(g_items_response, g_tokens, r,
                                                 progress_obj_idx, "percentage");
            if (progress_idx < 0)
                progress_idx = find_object_value(g_items_response, g_tokens, r,
                                                 progress_obj_idx, "percentComplete");
            if (current_time_idx < 0)
                current_time_idx = find_object_value(g_items_response, g_tokens, r,
                                                     progress_obj_idx, "currentTime");
            if (duration_idx < 0)
                duration_idx = find_object_value(g_items_response, g_tokens, r,
                                                 progress_obj_idx, "duration");
            if (finished_idx < 0)
                finished_idx = find_object_value(g_items_response, g_tokens, r,
                                                 progress_obj_idx, "isFinished");
        }

        if (id_idx >= 0 && g_tokens[id_idx].type == JSMN_STRING)
            tok_copy(g_items_response, &g_tokens[id_idx], id, sizeof(id));
        if (title_idx >= 0 && g_tokens[title_idx].type == JSMN_STRING)
            tok_copy(g_items_response, &g_tokens[title_idx], title, sizeof(title));
        if (author_idx >= 0 && g_tokens[author_idx].type == JSMN_STRING)
            tok_copy(g_items_response, &g_tokens[author_idx], author,
                     sizeof(author));
        if (series_idx >= 0 && g_tokens[series_idx].type == JSMN_STRING)
            tok_copy(g_items_response, &g_tokens[series_idx], series,
                     sizeof(series));
        if (downloaded_idx >= 0)
            downloaded_known = tok_to_bool(g_items_response,
                                           &g_tokens[downloaded_idx],
                                           &downloaded);
        if (finished_idx >= 0)
            (void)tok_to_bool(g_items_response, &g_tokens[finished_idx],
                              &finished);
        if (progress_idx >= 0)
            progress_percent = tok_to_percent(g_items_response,
                                              &g_tokens[progress_idx]);
        if (current_time_idx >= 0)
            current_time = tok_to_int(g_items_response,
                                      &g_tokens[current_time_idx]);
        if (duration_idx >= 0)
            duration = tok_to_int(g_items_response,
                                  &g_tokens[duration_idx]);

        if (downloaded_known)
            rb->strlcpy(download_state, downloaded ? "yes" : "no",
                        sizeof(download_state));
        if (finished) {
            rb->strlcpy(progress_state, "100%", sizeof(progress_state));
        } else if (progress_percent >= 0) {
            rb->snprintf(progress_state, sizeof(progress_state), "%d%%",
                         progress_percent > 100 ? 100 : progress_percent);
        } else if (current_time > 0 && duration > 0) {
            rb->snprintf(progress_state, sizeof(progress_state), "%d%%",
                         (current_time * 100) / duration);
        }

        if (id[0] != '\0') {
            rb->strlcpy(g_book_ids[g_book_count], id,
                        sizeof(g_book_ids[g_book_count]));
            build_book_entry(g_book_titles[g_book_count],
                             sizeof(g_book_titles[g_book_count]),
                             id, title, author, series,
                             download_state, progress_state);
            g_book_count++;
        }

        i = skip_value(i, r, g_tokens);
    }

    if (g_book_total >= 0) {
        g_book_has_next = ((g_book_page + 1) * g_book_limit) < g_book_total;
    } else {
        g_book_has_next = (g_book_count >= g_book_limit && g_book_count > 0);
    }

    return g_book_count;
}

static bool parse_book_detail(int json_len,
                              const char *requested_item_id,
                              char *error_buf, size_t error_len)
{
    jsmn_parser parser;
    int r;
    int title_idx;
    int media_idx;
    int metadata_idx = -1;
    int author_idx = -1;
    int series_idx = -1;
    int narrator_idx = -1;
    int published_idx = -1;
    int duration_idx = -1;
    int audio_files_idx = -1;
    int array_item_idx;
    int tracked_idx = 0;
    int audio_ino_idx = -1;
    int audio_filename_idx = -1;
    int file_metadata_idx = -1;
    int audio_ext_idx = -1;
    char ext_hint[FILE_EXT_SIZE];

    rb->memset(&g_book_detail, 0, sizeof(g_book_detail));
    if (requested_item_id)
        rb->strlcpy(g_book_detail.item_id, requested_item_id,
                    sizeof(g_book_detail.item_id));

    jsmn_init(&parser);
    r = jsmn_parse(&parser, g_detail_response, (size_t)json_len,
                   g_tokens, JSON_TOKEN_COUNT);

    if (r == JSMN_ERROR_NOMEM) {
        rb->snprintf(error_buf, error_len,
                     "Book detail JSON has more than %d tokens.",
                     JSON_TOKEN_COUNT);
        return false;
    }
    if (r == JSMN_ERROR_PART) {
        rb->snprintf(error_buf, error_len,
                     "Book detail response appears truncated (incomplete JSON).");
        return false;
    }
    if (r < 0) {
        rb->snprintf(error_buf, error_len,
                     "Malformed book detail JSON (parse error %d). "
                     "Response starts: %.40s",
                     r, g_detail_response[0] ? g_detail_response : "(empty)");
        return false;
    }
    if (r == 0 || g_tokens[0].type != JSMN_OBJECT) {
        rb->snprintf(error_buf, error_len,
                     "Unexpected book detail response: expected JSON object, "
                     "got %d token(s). Starts: %.40s",
                     r, g_detail_response[0] ? g_detail_response : "(empty)");
        return false;
    }

    title_idx = find_object_value(g_detail_response, g_tokens, r, 0, "title");
    media_idx = find_object_value(g_detail_response, g_tokens, r, 0, "media");
    if (media_idx >= 0 && g_tokens[media_idx].type == JSMN_OBJECT) {
        metadata_idx = find_object_value(g_detail_response, g_tokens, r,
                                         media_idx, "metadata");
        duration_idx = find_object_value(g_detail_response, g_tokens, r,
                                         media_idx, "duration");
        audio_files_idx = find_object_value(g_detail_response, g_tokens, r,
                                            media_idx, "audioFiles");
    }
    if (metadata_idx >= 0 && g_tokens[metadata_idx].type == JSMN_OBJECT) {
        if (title_idx < 0)
            title_idx = find_object_value(g_detail_response, g_tokens, r,
                                          metadata_idx, "title");
        author_idx = find_object_value(g_detail_response, g_tokens, r,
                                       metadata_idx, "authorName");
        series_idx = find_object_value(g_detail_response, g_tokens, r,
                                       metadata_idx, "seriesName");
        narrator_idx = find_object_value(g_detail_response, g_tokens, r,
                                         metadata_idx, "narratorName");
        if (narrator_idx < 0)
            narrator_idx = find_object_value(g_detail_response, g_tokens, r,
                                             metadata_idx, "narrators");
        published_idx = find_object_value(g_detail_response, g_tokens, r,
                                          metadata_idx, "publishedYear");
        if (published_idx < 0)
            published_idx = find_object_value(g_detail_response, g_tokens, r,
                                              metadata_idx, "publishedDate");
    }

    if (title_idx >= 0 && g_tokens[title_idx].type == JSMN_STRING)
        tok_copy(g_detail_response, &g_tokens[title_idx], g_book_detail.title,
                 sizeof(g_book_detail.title));
    if (author_idx >= 0 && g_tokens[author_idx].type == JSMN_STRING)
        tok_copy(g_detail_response, &g_tokens[author_idx], g_book_detail.author,
                 sizeof(g_book_detail.author));
    if (series_idx >= 0 && g_tokens[series_idx].type == JSMN_STRING)
        tok_copy(g_detail_response, &g_tokens[series_idx], g_book_detail.series,
                 sizeof(g_book_detail.series));
    if (narrator_idx >= 0 && g_tokens[narrator_idx].type == JSMN_STRING)
        tok_copy(g_detail_response, &g_tokens[narrator_idx], g_book_detail.narrator,
                 sizeof(g_book_detail.narrator));
    if (published_idx >= 0 &&
        (g_tokens[published_idx].type == JSMN_STRING ||
         g_tokens[published_idx].type == JSMN_PRIMITIVE))
        tok_copy(g_detail_response, &g_tokens[published_idx],
                 g_book_detail.published_year,
                 sizeof(g_book_detail.published_year));
    if (duration_idx >= 0)
        g_book_detail.duration_seconds = tok_to_int(g_detail_response,
                                                    &g_tokens[duration_idx]);
    if (audio_files_idx >= 0 && g_tokens[audio_files_idx].type == JSMN_ARRAY) {
        g_book_detail.audio_files_known = true;
        g_book_detail.audio_file_count = g_tokens[audio_files_idx].size;
        g_book_detail.has_audio_files = g_book_detail.audio_file_count > 0;

        array_item_idx = audio_files_idx + 1;
        while (array_item_idx < r &&
               tracked_idx < MAX_DETAIL_AUDIO_FILES &&
               tracked_idx < g_book_detail.audio_file_count) {
            char filename_buf[BOOK_TITLE_SIZE];

            if (g_tokens[array_item_idx].start >= g_tokens[audio_files_idx].end)
                break;
            if (g_tokens[array_item_idx].type != JSMN_OBJECT) {
                array_item_idx = skip_value(array_item_idx, r, g_tokens);
                continue;
            }

            audio_ino_idx = find_object_value(g_detail_response, g_tokens, r,
                                              array_item_idx, "ino");
            if (audio_ino_idx < 0)
                audio_ino_idx = find_object_value(g_detail_response, g_tokens, r,
                                                  array_item_idx, "id");
            if (audio_ino_idx < 0)
                audio_ino_idx = find_object_value(g_detail_response, g_tokens, r,
                                                  array_item_idx, "fileId");
            audio_filename_idx = find_object_value(g_detail_response, g_tokens, r,
                                                   array_item_idx, "filename");
            file_metadata_idx = find_object_value(g_detail_response, g_tokens, r,
                                                  array_item_idx, "metadata");
            audio_ext_idx = -1;
            filename_buf[0] = '\0';
            ext_hint[0] = '\0';

            if (file_metadata_idx >= 0 &&
                g_tokens[file_metadata_idx].type == JSMN_OBJECT) {
                if (audio_filename_idx < 0)
                    audio_filename_idx = find_object_value(g_detail_response,
                                                           g_tokens, r,
                                                           file_metadata_idx,
                                                           "filename");
                audio_ext_idx = find_object_value(g_detail_response, g_tokens, r,
                                                  file_metadata_idx, "ext");
            }

            if (audio_ino_idx >= 0 && g_tokens[audio_ino_idx].type == JSMN_STRING) {
                tok_copy(g_detail_response, &g_tokens[audio_ino_idx],
                         g_book_detail.audio_file_ids[tracked_idx],
                         sizeof(g_book_detail.audio_file_ids[tracked_idx]));
            }
            if (audio_filename_idx >= 0 &&
                g_tokens[audio_filename_idx].type == JSMN_STRING) {
                tok_copy(g_detail_response, &g_tokens[audio_filename_idx],
                         filename_buf, sizeof(filename_buf));
            }
            if (audio_ext_idx >= 0 && g_tokens[audio_ext_idx].type == JSMN_STRING) {
                tok_copy(g_detail_response, &g_tokens[audio_ext_idx],
                         ext_hint, sizeof(ext_hint));
            }
            (void)normalize_file_extension(
                filename_buf,
                ext_hint[0] ? ext_hint : NULL,
                g_book_detail.audio_file_exts[tracked_idx],
                sizeof(g_book_detail.audio_file_exts[tracked_idx]));

            if (tracked_idx == 0) {
                rb->strlcpy(g_book_detail.single_audio_ino,
                            g_book_detail.audio_file_ids[tracked_idx],
                            sizeof(g_book_detail.single_audio_ino));
                rb->strlcpy(g_book_detail.single_audio_filename,
                            filename_buf,
                            sizeof(g_book_detail.single_audio_filename));
                rb->strlcpy(g_book_detail.single_audio_ext,
                            g_book_detail.audio_file_exts[tracked_idx],
                            sizeof(g_book_detail.single_audio_ext));
            }
            if (g_book_detail.audio_file_ids[tracked_idx][0] != '\0' &&
                g_book_detail.audio_file_exts[tracked_idx][0] != '\0')
                g_book_detail.downloadable_audio_file_count++;

            tracked_idx++;
            array_item_idx = skip_value(array_item_idx, r, g_tokens);
        }

        g_book_detail.tracked_audio_file_count = tracked_idx;
        g_book_detail.has_single_audio_file =
            g_book_detail.audio_file_count == 1 &&
            g_book_detail.single_audio_ino[0] != '\0' &&
            g_book_detail.single_audio_ext[0] != '\0';
    }

    if (g_book_detail.title[0] == '\0')
        rb->strlcpy(g_book_detail.title, "(untitled)",
                    sizeof(g_book_detail.title));

    return true;
}

/* ---- list UIs ------------------------------------------------------------- */

static const char *lib_list_get_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    (void)data;
    if (selected_item < 0 || selected_item >= g_lib_count)
        return "???";
    rb->strlcpy(buffer, g_lib_names[selected_item], buffer_len);
    return buffer;
}

static const char *book_list_get_name(int selected_item, void *data,
                                      char *buffer, size_t buffer_len)
{
    bool has_prev = g_book_page > 0;
    int book_start = has_prev ? 1 : 0;
    int next_idx = book_start + g_book_count;

    (void)data;

    if (has_prev && selected_item == 0) {
        rb->strlcpy(buffer, "[Previous page]", buffer_len);
        return buffer;
    }
    if (selected_item >= book_start && selected_item < next_idx) {
        rb->strlcpy(buffer, g_book_titles[selected_item - book_start], buffer_len);
        return buffer;
    }
    if (g_book_has_next && selected_item == next_idx) {
        rb->strlcpy(buffer, "[Next page]", buffer_len);
        return buffer;
    }

    return "???";
}

static const char *detail_list_get_name(int selected_item, void *data,
                                        char *buffer, size_t buffer_len)
{
    (void)data;
    if (selected_item < 0 || selected_item >= g_detail_line_count)
        return "???";
    rb->strlcpy(buffer, g_detail_lines[selected_item], buffer_len);
    return buffer;
}

/*
 * Shows a Rockbox list of parsed library names.
 * Returns the selected index (>= 0), -1 if cancelled, or -2 on USB connect.
 */
static int show_library_picker(void)
{
    struct gui_synclist list;
    int action;
    bool done = false;
    int result = -1;

    rb->gui_synclist_init(&list, lib_list_get_name, NULL, false, 1, NULL);
    rb->gui_synclist_set_nb_items(&list, g_lib_count);
    rb->gui_synclist_set_title(&list, "Select Library", Icon_Rockbox);
    rb->gui_synclist_draw(&list);

    while (!done) {
        action = rb->get_action(CONTEXT_LIST, HZ / 10);
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        switch (action) {
        case ACTION_STD_OK:
            result = rb->gui_synclist_get_sel_pos(&list);
            done   = true;
            break;
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            result = -1;
            done   = true;
            break;
        default:
            if (rb->default_event_handler(action) == SYS_USB_CONNECTED) {
                result = -2;
                done   = true;
            }
            break;
        }
    }
    return result;
}

static int show_book_picker(const char *library_name)
{
    struct gui_synclist list;
    int action;
    bool done = false;
    int result = BOOK_PICKER_CANCEL;
    int list_count = g_book_count + (g_book_page > 0 ? 1 : 0) +
                     (g_book_has_next ? 1 : 0);
    int sel;
    int book_start = g_book_page > 0 ? 1 : 0;
    int next_idx = book_start + g_book_count;

    if (g_book_total >= 0) {
        rb->snprintf(g_list_title, sizeof(g_list_title),
                     "%s p%d (%d)",
                     library_name && library_name[0] ? library_name : "Books",
                     g_book_page + 1, g_book_total);
    } else {
        rb->snprintf(g_list_title, sizeof(g_list_title),
                     "%s p%d",
                     library_name && library_name[0] ? library_name : "Books",
                     g_book_page + 1);
    }

    rb->gui_synclist_init(&list, book_list_get_name, NULL, false, 1, NULL);
    rb->gui_synclist_set_nb_items(&list, list_count);
    rb->gui_synclist_set_title(&list, g_list_title, Icon_Audio);
    rb->gui_synclist_draw(&list);

    while (!done) {
        action = rb->get_action(CONTEXT_LIST, HZ / 10);
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        switch (action) {
        case ACTION_STD_OK:
            sel = rb->gui_synclist_get_sel_pos(&list);
            if (g_book_page > 0 && sel == 0)
                result = BOOK_PICKER_PREV_PAGE;
            else if (g_book_has_next && sel == next_idx)
                result = BOOK_PICKER_NEXT_PAGE;
            else if (sel >= book_start && sel < next_idx)
                result = sel - book_start;
            done = true;
            break;
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            result = BOOK_PICKER_CANCEL;
            done   = true;
            break;
        default:
            if (rb->default_event_handler(action) == SYS_USB_CONNECTED) {
                result = BOOK_PICKER_USB;
                done   = true;
            }
            break;
        }
    }

    return result;
}

static int show_book_detail_menu(void)
{
    struct gui_synclist list;
    int action;
    bool done = false;
    int result = DETAIL_ACTION_BACK;
    char duration[32];
    char audio_files[32];
    char local_state[32];

    g_detail_line_count = 0;
    add_detail_line("Title", g_book_detail.title);
    add_detail_line("Author", g_book_detail.author);
    add_detail_line("Series", g_book_detail.series);
    add_detail_line("Narrator", g_book_detail.narrator);
    add_detail_line("Published", g_book_detail.published_year);
    format_duration(g_book_detail.duration_seconds, duration, sizeof(duration));
    add_detail_line("Duration", duration);
    if (g_book_detail.audio_files_known) {
        rb->snprintf(audio_files, sizeof(audio_files), "%d",
                     g_book_detail.audio_file_count);
        add_detail_line("Audio files", audio_files);
    }
    if (g_book_detail.downloadable_audio_file_count > 0) {
        if (g_book_detail.local_stale_index_count > 0) {
            rb->snprintf(local_state, sizeof(local_state), "%d/%d +%d stale",
                         g_book_detail.local_audio_file_count,
                         g_book_detail.downloadable_audio_file_count,
                         g_book_detail.local_stale_index_count);
        } else {
            rb->snprintf(local_state, sizeof(local_state), "%d/%d",
                         g_book_detail.local_audio_file_count,
                         g_book_detail.downloadable_audio_file_count);
        }
    } else if (g_book_detail.local_stale_index_count > 0) {
        rb->snprintf(local_state, sizeof(local_state), "%s +%d stale",
                     g_book_detail.local_file_found ? "yes" : "no",
                     g_book_detail.local_stale_index_count);
    } else {
        rb->strlcpy(local_state, g_book_detail.local_file_found ? "yes" : "no",
                    sizeof(local_state));
    }
    add_detail_line("Local", local_state);

    g_detail_action_start = g_detail_line_count;
    if (g_book_detail.local_file_found)
        rb->strlcpy(g_detail_lines[g_detail_line_count++], "[Play Local]",
                    sizeof(g_detail_lines[0]));
    if (g_book_detail.local_index_entry_count > 0)
        rb->strlcpy(g_detail_lines[g_detail_line_count++], "[Delete Local]",
                    sizeof(g_detail_lines[0]));
    if (detail_has_manual_sync_mapping())
        rb->strlcpy(g_detail_lines[g_detail_line_count++], "[Sync Progress]",
                    sizeof(g_detail_lines[0]));
    rb->strlcpy(g_detail_lines[g_detail_line_count++], "[Download]",
                sizeof(g_detail_lines[0]));
    rb->strlcpy(g_detail_lines[g_detail_line_count++], "[Back]",
                sizeof(g_detail_lines[0]));

    rb->gui_synclist_init(&list, detail_list_get_name, NULL, false, 1, NULL);
    rb->gui_synclist_set_nb_items(&list, g_detail_line_count);
    rb->gui_synclist_set_title(&list, g_book_detail.title, Icon_Audio);
    rb->gui_synclist_select_item(&list, g_detail_action_start);
    rb->gui_synclist_draw(&list);

    while (!done) {
        action = rb->get_action(CONTEXT_LIST, HZ / 10);
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        switch (action) {
        case ACTION_STD_OK: {
            int sel = rb->gui_synclist_get_sel_pos(&list);
            int action_pos = g_detail_action_start;

            if (g_book_detail.local_file_found) {
                if (sel == action_pos) {
                    result = DETAIL_ACTION_PLAY_LOCAL;
                    done = true;
                    break;
                }
                action_pos++;
            }
            if (g_book_detail.local_index_entry_count > 0) {
                if (sel == action_pos) {
                    result = DETAIL_ACTION_DELETE_LOCAL;
                    done = true;
                    break;
                }
                action_pos++;
            }
            if (detail_has_manual_sync_mapping()) {
                if (sel == action_pos) {
                    result = DETAIL_ACTION_SYNC_PROGRESS;
                    done = true;
                    break;
                }
                action_pos++;
            }
            if (sel == action_pos) {
                result = DETAIL_ACTION_DOWNLOAD;
                done = true;
            } else if (sel == action_pos + 1) {
                result = DETAIL_ACTION_BACK;
                done = true;
            }
            break;
        }
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            result = DETAIL_ACTION_BACK;
            done   = true;
            break;
        default:
            if (rb->default_event_handler(action) == SYS_USB_CONNECTED) {
                result = DETAIL_ACTION_USB;
                done   = true;
            }
            break;
        }
    }

    return result;
}

/* ---- network -------------------------------------------------------------- */

static bool read_downloaded_json_file(const char *path,
                                      char *buf,
                                      size_t buf_len,
                                      char *error_buf,
                                      size_t error_len)
{
    int fd;
    off_t size;
    ssize_t nread;

    if (buf == NULL || buf_len == 0) {
        rb->snprintf(error_buf, error_len, "JSON buffer is unavailable");
        return false;
    }

    buf[0] = '\0';
    fd = rb->open(path, O_RDONLY);
    if (fd < 0) {
        rb->snprintf(error_buf, error_len,
                     "Could not open downloaded JSON:\n%s", path);
        return false;
    }

    size = rb->filesize(fd);
    if (size < 0) {
        rb->close(fd);
        rb->snprintf(error_buf, error_len,
                     "Could not determine downloaded JSON size:\n%s", path);
        return false;
    }
    if ((size_t)size >= buf_len) {
        rb->close(fd);
        rb->snprintf(error_buf, error_len,
                     "Downloaded JSON is too large (%ld bytes, buffer %lu bytes)",
                     (long)size, (unsigned long)buf_len);
        return false;
    }

    nread = rb->read(fd, buf, (size_t)size);
    rb->close(fd);
    if (nread != size) {
        buf[0] = '\0';
        rb->snprintf(error_buf, error_len,
                     "Could not read complete downloaded JSON (%ld/%ld bytes)",
                     (long)nread, (long)size);
        return false;
    }

    buf[(size_t)size] = '\0';
    return true;
}

static bool fetch_json_to_buffer_via_download(const char *endpoint,
                                              const char *header_buf,
                                              char *response_buf,
                                              size_t response_len,
                                              int *http_status,
                                              int *bridge_rc,
                                              char *error_buf,
                                              size_t error_len)
{
    bool ok;
    int rc;

    rb->remove(JSON_TMP_PATH);
    if (http_status != NULL)
        *http_status = 0;
    rc = rb->android_download(endpoint, header_buf, JSON_TMP_PATH,
                              http_status, error_buf, error_len);
    if (bridge_rc != NULL)
        *bridge_rc = rc;

    if (rc < 0) {
        rb->remove(JSON_TMP_PATH);
        return false;
    }
    if (http_status != NULL && *http_status != 200) {
        rb->remove(JSON_TMP_PATH);
        return false;
    }

    ok = read_downloaded_json_file(JSON_TMP_PATH, response_buf, response_len,
                                   error_buf, error_len);
    rb->remove(JSON_TMP_PATH);
    if (!ok && bridge_rc != NULL)
        *bridge_rc = ANDROID_REQUEST_TRUNCATED;
    return ok;
}

static bool fetch_books_page(const struct abs_config *cfg,
                             const char *header_buf,
                             const char *library_id,
                             int page,
                             int *page_size_io,
                             char *error_buf, size_t error_len,
                             char *text_buf, size_t text_len)
{
    char endpoint[ENDPOINT_BUF_SIZE];
    const char *wifi_result;
    int http_status = 0;
    int bridge_rc;
    int page_size;

    page_size = page_size_io != NULL ? *page_size_io : cfg->page_size;
    if (page_size <= 0)
        page_size = DEFAULT_PAGE_SIZE;

    rb->splashf(HZ, "Loading page %d...", page + 1);
    wifi_result = rb->android_connect_wifi();

    while (page_size >= 1) {
        rb->snprintf(endpoint, sizeof(endpoint),
                     "%s/api/libraries/%s/items?limit=%d&page=%d&sort=media.metadata.title&desc=0&minified=1",
                     cfg->server_url, library_id, page_size, page);

        g_items_response[0] = '\0';
        error_buf[0] = '\0';
        http_status = 0;
        (void)fetch_json_to_buffer_via_download(endpoint, header_buf,
                                                g_items_response,
                                                sizeof(g_items_response),
                                                &http_status, &bridge_rc,
                                                error_buf, error_len);

        if (bridge_rc != ANDROID_REQUEST_TRUNCATED)
            break;
        if (page_size <= 1)
            break;

        page_size = (page_size + 1) / 2;
        if (page_size_io != NULL)
            *page_size_io = page_size;
        rb->splashf(HZ, "Response too large; retrying %d/page", page_size);
    }

    rb->android_disconnect_wifi();

    if (page_size_io != NULL)
        *page_size_io = page_size;

    if (bridge_rc < 0 || http_status != 200) {
        char redacted_error[ERROR_BUF_SIZE];
        redact_token_text(error_buf, cfg->token,
                          redacted_error, sizeof(redacted_error));
        rb->snprintf(text_buf, text_len,
                     "Book list fetch failed\n\n"
                     "Library ID:   %s\n"
                     "Page:         %d\n"
                     "Page size:    %d\n"
                     "WiFi:         %s\n"
                     "HTTP status:  %d\n"
                     "Bridge rc:    %d (%s)\n\n"
                     "Error:\n%s",
                     library_id, page, page_size,
                     wifi_result ? wifi_result : "(null)",
                     http_status, bridge_rc, android_request_rc_name(bridge_rc),
                     redacted_error[0] ? redacted_error : "(none)");
        return false;
    }

    return true;
}

static bool fetch_book_detail(const struct abs_config *cfg,
                              const char *header_buf,
                              const char *item_id,
                              char *error_buf, size_t error_len,
                              char *text_buf, size_t text_len)
{
    char endpoint[ENDPOINT_BUF_SIZE];
    const char *wifi_result;
    int http_status = 0;
    int bridge_rc;

    rb->splash(HZ, "Loading book details...");
    wifi_result = rb->android_connect_wifi();

    rb->snprintf(endpoint, sizeof(endpoint), "%s/api/items/%s",
                 cfg->server_url, item_id);

    g_detail_response[0] = '\0';
    error_buf[0] = '\0';

    (void)fetch_json_to_buffer_via_download(endpoint, header_buf,
                                            g_detail_response,
                                            sizeof(g_detail_response),
                                            &http_status, &bridge_rc,
                                            error_buf, error_len);

    rb->android_disconnect_wifi();

    if (bridge_rc < 0 || http_status != 200) {
        char redacted_error[ERROR_BUF_SIZE];
        redact_token_text(error_buf, cfg->token,
                          redacted_error, sizeof(redacted_error));
        rb->snprintf(text_buf, text_len,
                     "Book detail fetch failed\n\n"
                     "Book ID:                 %s\n"
                     "WiFi:                    %s\n"
                     "HTTP status:             %d\n"
                     "Bridge rc:               %d\n"
                     "Bridge error (redacted): %s",
                     item_id,
                     wifi_result ? wifi_result : "(null)",
                     http_status, bridge_rc,
                     redacted_error[0] ? redacted_error : "(none)");
        return false;
    }

    return true;
}

static bool sync_book_progress_now(const struct abs_config *cfg,
                                   char *text_buf,
                                   size_t text_len)
{
    struct abs_progress_snapshot local;
    struct abs_progress_state remote;
    char endpoint[ENDPOINT_BUF_SIZE];
    char headers[HEADER_BUF_SIZE + 64];
    char body[192];
    char response_buf[RESPONSE_BUF_SIZE];
    char get_error[ERROR_BUF_SIZE];
    char patch_error[ERROR_BUF_SIZE];
    char post_error[ERROR_BUF_SIZE];
    char parse_error[ERROR_BUF_SIZE];
    char get_error_redacted[ERROR_BUF_SIZE];
    char patch_error_redacted[ERROR_BUF_SIZE];
    char post_error_redacted[ERROR_BUF_SIZE];
    const char *wifi_result;
    int progress_milli;
    int get_status = 0;
    int patch_status = 0;
    int post_status = 0;
    int get_rc = 0;
    int patch_rc = 0;
    int post_rc = 0;
    int remote_ahead_threshold;

    if (!build_local_progress_snapshot(&local, get_error, sizeof(get_error))) {
        rb->snprintf(text_buf, text_len,
                     "Sync Progress unavailable\n\nBook: %s\n\n%s",
                     g_book_detail.title,
                     get_error);
        return false;
    }

    progress_milli = (local.current_time * 1000) / (local.duration > 0 ? local.duration : 1);
    if (progress_milli > 1000)
        progress_milli = 1000;

    if (rb->snprintf(endpoint, sizeof(endpoint), "%s/api/me/progress/%s",
                     cfg->server_url, g_book_detail.item_id) >= (int)sizeof(endpoint)) {
        rb->snprintf(text_buf, text_len,
                     "Sync Progress failed\n\nProgress endpoint is too long.\nLocal progress was not changed.");
        return false;
    }
    rb->snprintf(headers, sizeof(headers),
                 "Authorization: Bearer %s\n"
                 "Accept: application/json\n"
                 "Content-Type: application/json",
                 cfg->token);
    rb->snprintf(body, sizeof(body),
                 "{\"duration\":%d,\"currentTime\":%d,\"progress\":%d.%03d,\"isFinished\":%s}",
                 local.duration,
                 local.current_time,
                 progress_milli / 1000,
                 progress_milli % 1000,
                 local.finished ? "true" : "false");

    rb->splash(HZ, "Syncing progress...");
    wifi_result = rb->android_connect_wifi();

    response_buf[0] = '\0';
    get_error[0] = '\0';
    patch_error[0] = '\0';
    post_error[0] = '\0';
    parse_error[0] = '\0';
    get_rc = rb->android_request(
        "GET", endpoint, headers, NULL,
        response_buf, sizeof(response_buf),
        &get_status, get_error, sizeof(get_error));

    if (get_rc < 0 || get_status == 0) {
        rb->android_disconnect_wifi();
        redact_token_text(get_error, cfg->token,
                          get_error_redacted, sizeof(get_error_redacted));
        rb->snprintf(text_buf, text_len,
                     "Sync Progress failed\n\n"
                     "Phase:         remote fetch\n"
                     "Category:      connection_error\n"
                     "Book:          %s\n"
                     "Local:         %d/%d sec (%d%%)\n"
                     "WiFi:          %s\n"
                     "GET status:    %d\n"
                     "GET rc:        %d (%s)\n"
                     "Local progress was not changed.\n\n"
                     "Hint: check WiFi, server URL, and bridge status before retrying.\n\n"
                     "GET error:\n%s",
                     g_book_detail.title,
                     local.current_time, local.duration, local.percent,
                     wifi_result ? wifi_result : "(null)",
                     get_status, get_rc, android_request_rc_name(get_rc),
                     get_error_redacted[0] ? get_error_redacted : "(none)");
        return false;
    }
    if (is_auth_error_status(get_status)) {
        rb->android_disconnect_wifi();
        redact_token_text(get_error, cfg->token,
                          get_error_redacted, sizeof(get_error_redacted));
        rb->snprintf(text_buf, text_len,
                     "Sync Progress failed\n\n"
                     "Phase:         remote fetch\n"
                     "Category:      auth_error\n"
                     "Book:          %s\n"
                     "Local:         %d/%d sec (%d%%)\n"
                     "WiFi:          %s\n"
                     "GET status:    %d\n"
                     "GET rc:        %d (%s)\n"
                     "Local progress was not changed.\n\n"
                     "Hint: token rejected; verify token in " CONFIG_PATH "\n\n"
                     "GET error:\n%s",
                     g_book_detail.title,
                     local.current_time, local.duration, local.percent,
                     wifi_result ? wifi_result : "(null)",
                     get_status, get_rc, android_request_rc_name(get_rc),
                     get_error_redacted[0] ? get_error_redacted : "(none)");
        return false;
    }
    if (get_status == 404) {
        init_progress_state(&remote);
    } else if (!http_status_is_success(get_status)) {
        rb->android_disconnect_wifi();
        redact_token_text(get_error, cfg->token,
                          get_error_redacted, sizeof(get_error_redacted));
        rb->snprintf(text_buf, text_len,
                     "Sync Progress failed\n\n"
                     "Phase:         remote fetch\n"
                     "Category:      server_error\n"
                     "Book:          %s\n"
                     "Local:         %d/%d sec (%d%%)\n"
                     "WiFi:          %s\n"
                     "GET status:    %d\n"
                     "GET rc:        %d (%s)\n"
                     "Local progress was not changed.\n\n"
                     "Hint: server rejected the progress lookup; inspect server logs/health.\n\n"
                     "GET error:\n%s",
                     g_book_detail.title,
                     local.current_time, local.duration, local.percent,
                     wifi_result ? wifi_result : "(null)",
                     get_status, get_rc, android_request_rc_name(get_rc),
                     get_error_redacted[0] ? get_error_redacted : "(none)");
        return false;
    } else if (!extract_progress_state_from_json(response_buf, &remote,
                                                 parse_error, sizeof(parse_error))) {
        rb->android_disconnect_wifi();
        rb->snprintf(text_buf, text_len,
                     "Sync Progress failed\n\n"
                     "Phase:         remote fetch\n"
                     "Category:      payload_error\n"
                     "Book:          %s\n"
                     "Local:         %d/%d sec (%d%%)\n"
                     "WiFi:          %s\n"
                     "GET status:    %d\n"
                     "GET rc:        %d (%s)\n"
                     "Local progress was not changed.\n\n"
                     "Hint: remote progress response was not usable, so upload was blocked to avoid overwriting newer state.\n\n"
                     "Payload issue:\n%s",
                     g_book_detail.title,
                     local.current_time, local.duration, local.percent,
                     wifi_result ? wifi_result : "(null)",
                     get_status, get_rc, android_request_rc_name(get_rc),
                     parse_error[0] ? parse_error : "(unknown)");
        return false;
    }

    remote_ahead_threshold = local.duration / 100;
    if (remote_ahead_threshold < 30)
        remote_ahead_threshold = 30;

    if (remote.known && remote.current_time > local.current_time + remote_ahead_threshold) {
        rb->android_disconnect_wifi();
        rb->snprintf(text_buf, text_len,
                     "Sync Progress skipped\n\n"
                     "Conflict policy: remote progress is ahead, so this manual spike will not overwrite it automatically.\n\n"
                     "Book:          %s\n"
                     "Local:         %d/%d sec (%d%%)\n"
                     "Remote:        %d/%d sec (%d%%)\n"
                     "GET status:    %d\n\n"
                     "Review the remote position first, then retry only after playback catches up locally.",
                     g_book_detail.title,
                     local.current_time, local.duration, local.percent,
                     remote.current_time < 0 ? 0 : remote.current_time,
                     remote.duration > 0 ? remote.duration : local.duration,
                     remote.percent >= 0 ? remote.percent :
                         ((remote.current_time >= 0 && local.duration > 0) ?
                          (remote.current_time * 100) / local.duration : 0),
                     get_status);
        return false;
    }

    response_buf[0] = '\0';
    patch_rc = rb->android_request(
        "PATCH", endpoint, headers, body,
        response_buf, sizeof(response_buf),
        &patch_status, patch_error, sizeof(patch_error));

    if (!http_status_is_success(patch_status) &&
        (patch_status == 404 || patch_status == 405)) {
        response_buf[0] = '\0';
        post_rc = rb->android_request(
            "POST", endpoint, headers, body,
            response_buf, sizeof(response_buf),
            &post_status, post_error, sizeof(post_error));
    }

    rb->android_disconnect_wifi();

    redact_token_text(patch_error, cfg->token,
                      patch_error_redacted, sizeof(patch_error_redacted));
    redact_token_text(post_error, cfg->token,
                      post_error_redacted, sizeof(post_error_redacted));

    if (http_status_is_success(patch_status) || http_status_is_success(post_status)) {
        rb->snprintf(text_buf, text_len,
                     "Sync Progress uploaded\n\n"
                     "Book:          %s\n"
                     "Method:        %s\n"
                     "Local:         %d/%d sec (%d%%)\n"
                     "WiFi:          %s\n"
                     "Remote policy: never overwrite newer remote progress automatically\n\n"
                     "Endpoint: %s",
                     g_book_detail.title,
                     http_status_is_success(patch_status) ? "PATCH" : "POST",
                     local.current_time, local.duration, local.percent,
                     wifi_result ? wifi_result : "(null)",
                     endpoint);
        return true;
    }

    rb->snprintf(text_buf, text_len,
                 "Sync Progress failed\n\n"
                 "Phase:         upload\n"
                 "Book:          %s\n"
                 "Local:         %d/%d sec (%d%%)\n"
                 "WiFi:          %s\n"
                 "PATCH status:  %d\n"
                 "PATCH rc:      %d (%s)\n"
                 "POST status:   %d\n"
                 "POST rc:       %d (%s)\n"
                 "Local progress was not changed.\n\n"
                 "PATCH error:\n%s\n\n"
                 "POST error:\n%s",
                 g_book_detail.title,
                 local.current_time, local.duration, local.percent,
                 wifi_result ? wifi_result : "(null)",
                 patch_status, patch_rc, android_request_rc_name(patch_rc),
                 post_status, post_rc, android_request_rc_name(post_rc),
                 patch_error_redacted[0] ? patch_error_redacted : "(none)",
                 post_error_redacted[0] ? post_error_redacted :
                     ((patch_status == 404 || patch_status == 405) ? "(none)" : "(not attempted)"));
    return false;
}

/* ---- fake backend --------------------------------------------------------- */

#ifdef AUDIOBOOKSHELF_FAKE_BACKEND

/*
 * Static fake data used when AUDIOBOOKSHELF_FAKE_BACKEND is defined.
 * populate_fake_libraries() / populate_fake_books() / populate_fake_detail()
 * write into the same globals that the real JSON parsers use, so every UI
 * function works unchanged.
 */

struct fake_lib_entry {
    const char *id;
    const char *name;
};

static const struct fake_lib_entry fake_libs[] = {
    { "fake-lib-001", "Audiobooks" },
    { "fake-lib-002", "Podcasts"   },
    { "fake-lib-003", "Short Stories" },
};
#define FAKE_LIB_COUNT 3

struct fake_book_entry {
    const char *id;
    const char *title;
    const char *author;
    const char *series;
};

static const struct fake_book_entry fake_books[] = {
    { "fake-book-001", "The Hobbit",                "J.R.R. Tolkien",     ""                    },
    { "fake-book-002", "Dune",                      "Frank Herbert",      "Dune Chronicles"     },
    { "fake-book-003", "Foundation",                "Isaac Asimov",       "Foundation Series"   },
    { "fake-book-004", "Neuromancer",               "William Gibson",     "Sprawl Trilogy"      },
    { "fake-book-005", "The Left Hand of Darkness", "Ursula K. Le Guin",  "Hainish Cycle"       },
};
#define FAKE_BOOK_COUNT 5

static void populate_fake_libraries(void)
{
    int i;

    g_lib_count = 0;
    for (i = 0; i < FAKE_LIB_COUNT && i < MAX_LIBRARIES; i++) {
        rb->strlcpy(g_lib_ids[i],   fake_libs[i].id,   sizeof(g_lib_ids[i]));
        rb->strlcpy(g_lib_names[i], fake_libs[i].name, sizeof(g_lib_names[i]));
        g_lib_count++;
    }
}

static void populate_fake_books(int page, int page_size)
{
    int start = page * page_size;
    int i;

    g_book_count    = 0;
    g_book_page     = page;
    g_book_limit    = page_size;
    g_book_total    = FAKE_BOOK_COUNT;
    g_book_has_next = false;

    for (i = 0; i < page_size && (start + i) < FAKE_BOOK_COUNT &&
                g_book_count < MAX_BOOKS_PER_PAGE; i++) {
        int src = start + i;

        rb->strlcpy(g_book_ids[g_book_count],
                    fake_books[src].id,
                    sizeof(g_book_ids[g_book_count]));
        build_book_entry(g_book_titles[g_book_count],
                         sizeof(g_book_titles[g_book_count]),
                         fake_books[src].id,
                         fake_books[src].title,
                         fake_books[src].author,
                         fake_books[src].series,
                         "--",
                         "--");
        g_book_count++;
    }

    g_book_has_next = ((page + 1) * page_size) < FAKE_BOOK_COUNT;
}

static void populate_fake_detail(const char *item_id)
{
    int i;

    rb->memset(&g_book_detail, 0, sizeof(g_book_detail));

    if (item_id)
        rb->strlcpy(g_book_detail.item_id, item_id,
                    sizeof(g_book_detail.item_id));

    for (i = 0; i < FAKE_BOOK_COUNT; i++) {
        if (rb->strcmp(fake_books[i].id, item_id) == 0) {
            rb->strlcpy(g_book_detail.title,  fake_books[i].title,
                        sizeof(g_book_detail.title));
            rb->strlcpy(g_book_detail.author, fake_books[i].author,
                        sizeof(g_book_detail.author));
            rb->strlcpy(g_book_detail.series, fake_books[i].series,
                        sizeof(g_book_detail.series));
            rb->strlcpy(g_book_detail.narrator,       "Test Narrator",
                        sizeof(g_book_detail.narrator));
            rb->strlcpy(g_book_detail.published_year, "2024",
                        sizeof(g_book_detail.published_year));
            g_book_detail.duration_seconds  = (i + 1) * 3600 + i * 600;
            g_book_detail.audio_files_known = true;
            g_book_detail.audio_file_count  = i + 1;
            g_book_detail.has_audio_files   = true;
            return;
        }
    }

    /* Unknown id — fill in a minimal placeholder. */
    rb->strlcpy(g_book_detail.title, item_id[0] ? item_id : "(unknown)",
                sizeof(g_book_detail.title));
}

#endif /* AUDIOBOOKSHELF_FAKE_BACKEND */

/* ---- deterministic self-tests -------------------------------------------- */

struct abs_self_test_state {
    int pass;
    int fail;
    char failures[TEXT_BUF_SIZE];
    size_t failures_used;
};

static bool contains_text(const char *haystack, const char *needle)
{
    size_t i;
    size_t hay_len;
    size_t needle_len;

    if (haystack == NULL || needle == NULL)
        return false;

    hay_len = rb->strlen(haystack);
    needle_len = rb->strlen(needle);
    if (needle_len == 0)
        return true;
    if (needle_len > hay_len)
        return false;

    for (i = 0; i <= hay_len - needle_len; i++) {
        if (rb->memcmp(haystack + i, needle, needle_len) == 0)
            return true;
    }

    return false;
}

static void self_test_check(struct abs_self_test_state *state,
                            bool ok,
                            const char *label)
{
    if (ok) {
        state->pass++;
        return;
    }

    state->fail++;
    if (state->failures_used + 2 < sizeof(state->failures)) {
        int written = rb->snprintf(state->failures + state->failures_used,
                                   sizeof(state->failures) - state->failures_used,
                                   "%s- %s",
                                   state->failures_used == 0 ? "" : "\n",
                                   label);
        if (written > 0)
            state->failures_used += (size_t)written;
    }
}

static void self_test_config(struct abs_self_test_state *state)
{
    struct abs_config cfg;
    char buf[128];
    char config_text[512];
    char error_buf[ERROR_BUF_SIZE];

    rb->strlcpy(buf, "  hello  ", sizeof(buf));
    self_test_check(state, rb->strcmp(trim_ws(buf), "hello") == 0,
                    "trim_ws strips outer spaces");

    rb->strlcpy(buf, "\t\nvalue\r\n", sizeof(buf));
    self_test_check(state, rb->strcmp(trim_ws(buf), "value") == 0,
                    "trim_ws strips tabs/newlines");

    self_test_check(state, sanitize_page_size(10) == 10,
                    "page_size keeps normal value");
    self_test_check(state, sanitize_page_size(0) == DEFAULT_PAGE_SIZE,
                    "page_size defaults on zero");
    self_test_check(state, sanitize_page_size(999) == MAX_PAGE_SIZE,
                    "page_size clamps high values");

    rb->snprintf(config_text, sizeof(config_text),
                 "# comment\n"
                 " server_url :  https://abs.example.test///  \n"
                 " token :  secret-token  \n"
                 " library_id: lib-main \n"
                 " download_dir: /sdcard/audiobookshelf/tests/ \n"
                 " page_size: 500 \n");
    self_test_check(state,
                    parse_config_text(&cfg, config_text,
                                      error_buf, sizeof(error_buf)) &&
                    rb->strcmp(cfg.server_url, "https://abs.example.test") == 0 &&
                    rb->strcmp(cfg.token, "secret-token") == 0 &&
                    rb->strcmp(cfg.library_id, "lib-main") == 0 &&
                    rb->strcmp(cfg.download_dir,
                               "/sdcard/audiobookshelf/tests/") == 0 &&
                    cfg.page_size == MAX_PAGE_SIZE,
                    "config parser handles whitespace and normalization");

    rb->strlcpy(config_text,
                "server_url: http://abs.example.test\n"
                "token: secret-token\n",
                sizeof(config_text));
    self_test_check(state,
                    !parse_config_text(&cfg, config_text,
                                       error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "Plain HTTP is not allowed"),
                    "config rejects plain HTTP");

    rb->strlcpy(config_text,
                "server_url: https://abs.example.test\n"
                "token secret-token\n",
                sizeof(config_text));
    self_test_check(state,
                    !parse_config_text(&cfg, config_text,
                                       error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "Malformed config line"),
                    "config rejects malformed line");

    rb->strlcpy(config_text,
                "server_url: https://abs.example.test\n"
                "unknown_key: value\n"
                "token: secret-token\n",
                sizeof(config_text));
    self_test_check(state,
                    parse_config_text(&cfg, config_text,
                                      error_buf, sizeof(error_buf)) &&
                    rb->strcmp(cfg.server_url, "https://abs.example.test") == 0 &&
                    rb->strcmp(cfg.token, "secret-token") == 0,
                    "config ignores unknown keys");

    rb->strlcpy(config_text,
                "server_url: https://abs.example.test\n",
                sizeof(config_text));
    self_test_check(state,
                    !parse_config_text(&cfg, config_text,
                                       error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "Missing 'token'"),
                    "config requires token");
}

static void self_test_libraries(struct abs_self_test_state *state)
{
    char error_buf[ERROR_BUF_SIZE];
    char text_buf[TEXT_BUF_SIZE];
    struct abs_config cfg;
    int i;
    int len;

    init_config_defaults(&cfg);
    rb->strlcpy(cfg.server_url, "https://abs.example.test",
                sizeof(cfg.server_url));
    rb->strlcpy(cfg.token, "secret-token", sizeof(cfg.token));

    rb->strlcpy(g_lib_response,
                "{\"libraries\":[{\"id\":\"lib1\",\"name\":\"Audiobooks\"},"
                "{\"id\":\"lib2\"}]}",
                sizeof(g_lib_response));
    self_test_check(state,
                    parse_libraries((int)rb->strlen(g_lib_response),
                                    error_buf, sizeof(error_buf)) == 2 &&
                    rb->strcmp(g_lib_ids[0], "lib1") == 0 &&
                    rb->strcmp(g_lib_names[0], "Audiobooks") == 0 &&
                    rb->strcmp(g_lib_names[1], "lib2") == 0,
                    "parse_libraries parses ids and fallback names");

    rb->strlcpy(g_lib_response, "{\"libraries\":[{\"id\":\"lib1\"}",
                sizeof(g_lib_response));
    self_test_check(state,
                    parse_libraries((int)rb->strlen(g_lib_response),
                                    error_buf, sizeof(error_buf)) < 0 &&
                    contains_text(error_buf, "truncated"),
                    "parse_libraries rejects truncated JSON");

    rb->strlcpy(g_lib_response, "{\"libraries\":{}}", sizeof(g_lib_response));
    self_test_check(state,
                    parse_libraries((int)rb->strlen(g_lib_response),
                                    error_buf, sizeof(error_buf)) < 0 &&
                    contains_text(error_buf, "no 'libraries' array"),
                    "parse_libraries rejects missing array");

    len = rb->snprintf(g_lib_response, sizeof(g_lib_response),
                       "{\"libraries\":[");
    for (i = 0; i < 1100 && len > 0 && len < (int)sizeof(g_lib_response) - 4; i++)
        len += rb->snprintf(g_lib_response + len,
                            sizeof(g_lib_response) - (size_t)len,
                            "%s{}",
                            i ? "," : "");
    rb->snprintf(g_lib_response + len,
                 sizeof(g_lib_response) - (size_t)len,
                 "]}");
    self_test_check(state,
                    parse_libraries((int)rb->strlen(g_lib_response),
                                    error_buf, sizeof(error_buf)) < 0 &&
                    contains_text(error_buf, "more than") &&
                    contains_text(error_buf, "tokens"),
                    "parse_libraries rejects oversized token sets");

    format_request_failure("Library fetch failed", &cfg, "wifi_ok",
                           401, ANDROID_REQUEST_OK,
                           "server said Unauthorized for secret-token",
                           text_buf, sizeof(text_buf));
    self_test_check(state,
                    contains_text(text_buf, "auth_error") &&
                    contains_text(text_buf, "verify token") &&
                    !contains_text(text_buf, "secret-token"),
                    "request failure maps 401 to auth_error with redaction");

    format_request_failure("Library fetch failed", &cfg, "wifi_down",
                           0, ANDROID_REQUEST_JNI_EXCEPTION,
                           "android request bridge failed",
                           text_buf, sizeof(text_buf));
    self_test_check(state,
                    contains_text(text_buf, "connection_error") &&
                    contains_text(text_buf, "check WiFi") &&
                    contains_text(text_buf, "jni_exception"),
                    "request failure maps transport issues to actionable diagnostics");
}

static void self_test_books(struct abs_self_test_state *state)
{
    char error_buf[ERROR_BUF_SIZE];
    int len;
    int i;

    rb->strlcpy(g_items_response,
                "{\"results\":["
                "{\"id\":\"b1\",\"media\":{\"metadata\":{\"title\":\"Book One\",\"authorName\":\"Author\",\"seriesName\":\"Series\"},\"duration\":400},\"isDownloaded\":true,\"mediaProgress\":{\"currentTime\":100}},"
                "{\"id\":\"b2\",\"title\":\"Standalone\"}],"
                "\"page\":1,\"limit\":2,\"total\":5}",
                sizeof(g_items_response));
    self_test_check(state,
                    parse_books((int)rb->strlen(g_items_response), 0, 2,
                                error_buf, sizeof(error_buf)) == 2 &&
                    g_book_page == 1 && g_book_limit == 2 && g_book_total == 5 &&
                    g_book_has_next &&
                    rb->strcmp(g_book_ids[0], "b1") == 0 &&
                    contains_text(g_book_titles[0], "[b1]") &&
                    contains_text(g_book_titles[0], "Book One") &&
                    contains_text(g_book_titles[0], "Author") &&
                    contains_text(g_book_titles[0], "DL:yes") &&
                    contains_text(g_book_titles[0], "P:25%") &&
                    rb->strcmp(g_book_ids[1], "b2") == 0,
                    "parse_books parses metadata, status, and paging");

    rb->strlcpy(g_items_response,
                "{\"libraryItems\":[{\"id\":\"b3\",\"name\":\"Named\"}],"
                "\"total\":1}",
                sizeof(g_items_response));
    self_test_check(state,
                    parse_books((int)rb->strlen(g_items_response), 0, 20,
                                error_buf, sizeof(error_buf)) == 1 &&
                    rb->strcmp(g_book_ids[0], "b3") == 0 &&
                    contains_text(g_book_titles[0], "[b3]") &&
                    contains_text(g_book_titles[0], "Named") &&
                    contains_text(g_book_titles[0], "DL:--") &&
                    contains_text(g_book_titles[0], "P:--"),
                    "parse_books accepts libraryItems fallback");

    rb->strlcpy(g_items_response, "{\"results\":[{\"id\":\"b1\"}",
                sizeof(g_items_response));
    self_test_check(state,
                    parse_books((int)rb->strlen(g_items_response), 0, 20,
                                error_buf, sizeof(error_buf)) < 0 &&
                    contains_text(error_buf, "truncated"),
                    "parse_books rejects truncated JSON");

    rb->strlcpy(g_items_response, "{\"page\":0}", sizeof(g_items_response));
    self_test_check(state,
                    parse_books((int)rb->strlen(g_items_response), 0, 20,
                                error_buf, sizeof(error_buf)) < 0 &&
                    contains_text(error_buf, "no 'results' array"),
                    "parse_books rejects missing results array");

    rb->strlcpy(g_items_response,
                "{\"results\":[{\"title\":\"Missing Id\"}],\"total\":1}",
                sizeof(g_items_response));
    self_test_check(state,
                    parse_books((int)rb->strlen(g_items_response), 0, 20,
                                error_buf, sizeof(error_buf)) == 0,
                    "parse_books skips items missing ids");

    len = rb->snprintf(g_items_response, sizeof(g_items_response),
                       "{\"results\":[");
    for (i = 0; i < 350 && len > 0 && len < (int)sizeof(g_items_response) - 64; i++)
        len += rb->snprintf(g_items_response + len,
                            sizeof(g_items_response) - (size_t)len,
                            "%s{\"id\":\"b%d\",\"title\":\"T%d\"}",
                            i ? "," : "", i, i);
    rb->snprintf(g_items_response + len,
                 sizeof(g_items_response) - (size_t)len,
                 "],\"total\":350}");
    self_test_check(state,
                    parse_books((int)rb->strlen(g_items_response), 0, 20,
                                error_buf, sizeof(error_buf)) < 0 &&
                    contains_text(error_buf, "more than") &&
                    contains_text(error_buf, "tokens"),
                    "parse_books rejects oversized token sets");
}

static void self_test_book_detail(struct abs_self_test_state *state)
{
    char error_buf[ERROR_BUF_SIZE];
    int len;
    int i;

    rb->strlcpy(g_detail_response,
                "{\"media\":{\"metadata\":{\"title\":\"Detail Book\","
                "\"authorName\":\"Author\",\"seriesName\":\"Series\","
                "\"narratorName\":\"Narrator\",\"publishedYear\":2024},"
                "\"duration\":3661,\"audioFiles\":[{\"ino\":\"part-1\",\"filename\":\"01.mp3\"},{\"ino\":\"part-2\",\"filename\":\"02.m4b\"}]}}",
                sizeof(g_detail_response));
    self_test_check(state,
                    parse_book_detail((int)rb->strlen(g_detail_response),
                                      "book-1", error_buf, sizeof(error_buf)) &&
                    rb->strcmp(g_book_detail.item_id, "book-1") == 0 &&
                    rb->strcmp(g_book_detail.title, "Detail Book") == 0 &&
                    rb->strcmp(g_book_detail.author, "Author") == 0 &&
                    g_book_detail.duration_seconds == 3661 &&
                    g_book_detail.audio_files_known &&
                    g_book_detail.audio_file_count == 2 &&
                    g_book_detail.tracked_audio_file_count == 2 &&
                    g_book_detail.downloadable_audio_file_count == 2 &&
                    g_book_detail.has_audio_files &&
                    rb->strcmp(g_book_detail.audio_file_ids[0], "part-1") == 0 &&
                    rb->strcmp(g_book_detail.audio_file_ids[1], "part-2") == 0 &&
                    rb->strcmp(g_book_detail.audio_file_exts[0], ".mp3") == 0 &&
                    rb->strcmp(g_book_detail.audio_file_exts[1], ".m4b") == 0 &&
                    !g_book_detail.has_single_audio_file,
                    "parse_book_detail parses ordered audio files");

    rb->strlcpy(g_detail_response,
                "{\"media\":{\"metadata\":{},\"audioFiles\":[]}}",
                sizeof(g_detail_response));
    self_test_check(state,
                    parse_book_detail((int)rb->strlen(g_detail_response),
                                      "book-2", error_buf, sizeof(error_buf)) &&
                    rb->strcmp(g_book_detail.title, "(untitled)") == 0 &&
                    g_book_detail.audio_files_known &&
                    g_book_detail.audio_file_count == 0 &&
                    !g_book_detail.has_audio_files,
                    "parse_book_detail handles missing title and empty audio");

    rb->strlcpy(g_detail_response,
                "{\"media\":{\"metadata\":{\"title\":\"Single File\"},"
                "\"audioFiles\":[{\"ino\":\"ino-1\",\"filename\":\"track.M4B\","
                "\"metadata\":{\"ext\":\".M4B\"}}]}}",
                sizeof(g_detail_response));
    self_test_check(state,
                    parse_book_detail((int)rb->strlen(g_detail_response),
                                      "book-6", error_buf, sizeof(error_buf)) &&
                    g_book_detail.has_single_audio_file &&
                    rb->strcmp(g_book_detail.single_audio_ino, "ino-1") == 0 &&
                    rb->strcmp(g_book_detail.single_audio_ext, ".m4b") == 0,
                    "parse_book_detail captures single audio file metadata");

    rb->strlcpy(g_book_detail.local_path,
                "/sdcard/audiobookshelf/downloads/Single_File/track-book-6-ino-1.m4b",
                sizeof(g_book_detail.local_path));
    g_book_detail.local_file_found = true;
    g_book_detail.duration_seconds = 120;
    rb->strlcpy(g_detail_response,
                "{\"media\":{\"duration\":120,\"audioFiles\":[{\"ino\":\"ino-1\",\"filename\":\"track.m4b\"}],\"mediaProgress\":{\"currentTime\":45,\"duration\":120}}}",
                sizeof(g_detail_response));
    self_test_check(state,
                    detail_has_manual_sync_mapping() &&
                    extract_detail_remote_progress(&(struct abs_progress_state){0}) == true &&
                    extract_progress_state_from_json(
                        "{\"currentTime\":30,\"duration\":120,\"progress\":0.250}",
                        &(struct abs_progress_state){0}, error_buf, sizeof(error_buf)) == true &&
                    !extract_progress_state_from_json(
                        "{\"id\":\"missing-progress\"}",
                        &(struct abs_progress_state){0}, error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "usable progress fields"),
                    "manual sync spike parses explicit remote progress payloads");

    rb->strlcpy(g_detail_response, "{\"media\":{\"metadata\":{\"title\":\"Broken\"}",
                sizeof(g_detail_response));
    self_test_check(state,
                    !parse_book_detail((int)rb->strlen(g_detail_response),
                                       "book-3", error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "truncated"),
                    "parse_book_detail rejects truncated JSON");

    rb->strlcpy(g_detail_response, "[]", sizeof(g_detail_response));
    self_test_check(state,
                    !parse_book_detail((int)rb->strlen(g_detail_response),
                                       "book-4", error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "expected JSON object"),
                    "parse_book_detail rejects wrong root type");

    len = rb->snprintf(g_detail_response, sizeof(g_detail_response),
                       "{\"media\":{\"metadata\":{\"title\":\"Huge\"},\"audioFiles\":[");
    for (i = 0; i < 600 && len > 0 && len < (int)sizeof(g_detail_response) - 32; i++)
        len += rb->snprintf(g_detail_response + len,
                            sizeof(g_detail_response) - (size_t)len,
                            "%s{\"i\":%d}", i ? "," : "", i);
    rb->snprintf(g_detail_response + len,
                 sizeof(g_detail_response) - (size_t)len,
                 "]}}");
    self_test_check(state,
                    !parse_book_detail((int)rb->strlen(g_detail_response),
                                       "book-5", error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "more than") &&
                    contains_text(error_buf, "tokens"),
                    "parse_book_detail rejects oversized token sets");
}

static void self_test_paths_and_redaction(struct abs_self_test_state *state)
{
    struct abs_config cfg;
    char out[BOOK_TITLE_SIZE];
    char root_buf[DLOAD_DIR_BUF_SIZE];
    char path_buf[MAX_PATH];
    char error_buf[ERROR_BUF_SIZE];
    char redacted[128];
    int i;

    sanitize_path_component("Book: Title / Part 1", out, sizeof(out));
    self_test_check(state,
                    rb->strcmp(out, "Book_Title_Part_1") == 0,
                    "sanitize_path_component strips separators");

    sanitize_path_component("...", out, sizeof(out));
    self_test_check(state,
                    rb->strcmp(out, "untitled") == 0,
                    "sanitize_path_component falls back to untitled");

    for (i = 0; i < (int)sizeof(out) - 2; i++)
        out[i] = 'A';
    out[sizeof(out) - 2] = '\0';
    sanitize_path_component(out, path_buf, sizeof(path_buf));
    self_test_check(state,
                    rb->strlen(path_buf) < sizeof(path_buf) &&
                    path_buf[0] == 'A',
                    "sanitize_path_component truncates safely");

    self_test_check(state,
                    path_has_traversal("/sdcard/../etc/passwd") &&
                    path_has_traversal("/sdcard/./book") &&
                    !path_has_traversal("/sdcard/audiobookshelf/book.abs"),
                    "path_has_traversal flags traversal only");

    self_test_check(state,
                    path_is_under_base("/sdcard/audiobookshelf/downloads",
                                       SAFE_DOWNLOAD_BASE) &&
                    !path_is_under_base("/sdcard/audiobookshelf_evil",
                                        SAFE_DOWNLOAD_BASE),
                    "path_is_under_base respects boundaries");

    init_config_defaults(&cfg);
    rb->strlcpy(cfg.download_dir, "/sdcard/audiobookshelf/downloads/",
                sizeof(cfg.download_dir));
    self_test_check(state,
                    validate_download_root(&cfg, root_buf, sizeof(root_buf),
                                           error_buf, sizeof(error_buf)) &&
                    rb->strcmp(root_buf, "/sdcard/audiobookshelf/downloads") == 0,
                    "validate_download_root trims trailing slash");

    rb->strlcpy(cfg.download_dir, "/sdcard/.rockbox/downloads",
                sizeof(cfg.download_dir));
    self_test_check(state,
                    !validate_download_root(&cfg, root_buf, sizeof(root_buf),
                                            error_buf, sizeof(error_buf)) &&
                    contains_text(error_buf, "may not be inside /sdcard/.rockbox"),
                    "validate_download_root blocks rockbox tree");

    self_test_check(state,
                    normalize_file_extension("track.MP3", NULL,
                                             out, sizeof(out)) &&
                    rb->strcmp(out, ".mp3") == 0,
                    "normalize_file_extension accepts filename suffixes");

    self_test_check(state,
                    build_download_destination("/sdcard/audiobookshelf/downloads",
                                               "../Bad Book", "id/42",
                                               "ino/7", ".m4b",
                                               0, 1,
                                               path_buf, sizeof(path_buf),
                                               error_buf, sizeof(error_buf)) &&
                    contains_text(path_buf, "/Bad_Book/") &&
                    contains_text(path_buf, "Bad_Book-id_42-ino_7.m4b") &&
                    path_is_under_base(path_buf,
                                       "/sdcard/audiobookshelf/downloads"),
                    "build_download_destination sanitizes title, ids, and extension");

    self_test_check(state,
                    build_temp_download_path(path_buf, out, sizeof(out)) &&
                    contains_text(out, ".part"),
                    "build_temp_download_path appends part suffix safely");

    self_test_check(state,
                    local_index_path_is_safe(&cfg,
                                             "/sdcard/audiobookshelf/downloads/Book/file.m4b") &&
                    !local_index_path_is_safe(&cfg,
                                              "/sdcard/audiobookshelf/other/file.m4b") &&
                    !local_index_path_is_safe(&cfg,
                                              "/sdcard/audiobookshelf/downloads/../evil.m4b"),
                    "local_index_path_is_safe constrains indexed playback paths");

    redact_token_text("Authorization: Bearer token-123 token-123",
                      "token-123", redacted, sizeof(redacted));
    self_test_check(state,
                    !contains_text(redacted, "token-123") &&
                    contains_text(redacted, "[redacted]"),
                    "redact_token_text removes repeated tokens");

    redact_token_text("safe text", NULL, redacted, sizeof(redacted));
    self_test_check(state,
                    rb->strcmp(redacted, "safe text") == 0,
                    "redact_token_text passes through with NULL token");
}

static void run_self_tests(void)
{
    struct abs_self_test_state state;
    char text_buf[TEXT_BUF_SIZE];

    rb->memset(&state, 0, sizeof(state));

    self_test_config(&state);
    self_test_libraries(&state);
    self_test_books(&state);
    self_test_book_detail(&state);
    self_test_paths_and_redaction(&state);

    rb->snprintf(text_buf, sizeof(text_buf),
                 "Self Tests\n\n"
                 "Pass: %d\n"
                 "Fail: %d\n\n"
                 "%s",
                 state.pass,
                 state.fail,
                 state.fail == 0 ?
                     "All deterministic helper/parser checks passed."
                     : state.failures);
    view_text("Diagnostics: Self Tests", text_buf);
}

/* ---- top-level menu sections ---------------------------------------------- */

/*
 * Menu index constants — must match the string order passed to
 * MENUITEM_STRINGLIST in show_main_menu() below.
 */
#define MAIN_MENU_BROWSE      0
#define MAIN_MENU_LOCAL       1
#define MAIN_MENU_DIAG        2
#define MAIN_MENU_HELP        3
#define MAIN_MENU_QUIT        4

#define DIAG_MENU_WIFI        0
#define DIAG_MENU_AUTH        1
#define DIAG_MENU_SELF_TESTS  2
#define DIAG_MENU_BACK        3

#define DIAG_FAKE_MENU_INFO        0
#define DIAG_FAKE_MENU_SELF_TESTS  1
#define DIAG_FAKE_MENU_BACK        2

static void show_diag_build_info(void)
{
    char text_buf[TEXT_BUF_SIZE];

#ifdef AUDIOBOOKSHELF_FAKE_BACKEND
    rb->snprintf(text_buf, sizeof(text_buf),
                 "Audiobookshelf Diagnostics\n\n"
                 "Mode:         FAKE BACKEND\n"
                 "Fake libs:    %d\n"
                 "Fake books:   %d\n\n"
                 "Build:        " __DATE__ " " __TIME__,
                 FAKE_LIB_COUNT, FAKE_BOOK_COUNT);
#else
    rb->snprintf(text_buf, sizeof(text_buf),
                 "Audiobookshelf Diagnostics\n\n"
                 "Mode:         LIVE BACKEND\n"
                 "Build:        " __DATE__ " " __TIME__);
#endif
    view_text("Diagnostics", text_buf);
}

static void show_local_downloads(void)
{
    view_text("Local Downloads",
              "No local downloads found.\n\n"
              "Use Browse Library to download\n"
              "books to your device.\n\n"
              "Downloads are saved to:\n"
              DEFAULT_DOWNLOAD_DIR);
}

#ifndef AUDIOBOOKSHELF_FAKE_BACKEND
static void diag_test_wifi(void)
{
    char text_buf[TEXT_BUF_SIZE];
    const char *wifi_result;

    rb->splash(HZ, "Testing WiFi...");
    wifi_result = rb->android_connect_wifi();
    rb->android_disconnect_wifi();

    rb->snprintf(text_buf, sizeof(text_buf),
                 "WiFi Test\n\nResult: %s",
                 wifi_result ? wifi_result : "(null)");
    view_text("Diagnostics: WiFi", text_buf);
}

static void diag_test_auth(const struct abs_config *cfg,
                           const char *header_buf)
{
    char text_buf[TEXT_BUF_SIZE];
    char error_buf[ERROR_BUF_SIZE];
    char redacted_error[ERROR_BUF_SIZE];
    char response_buf[RESPONSE_BUF_SIZE];
    char endpoint[ENDPOINT_BUF_SIZE];
    const char *wifi_result;
    int http_status = 0;
    int bridge_rc;

    rb->splash(HZ, "Testing auth...");
    wifi_result = rb->android_connect_wifi();

    rb->snprintf(endpoint, sizeof(endpoint), "%s/api/me", cfg->server_url);
    response_buf[0] = '\0';
    error_buf[0]    = '\0';
    bridge_rc = rb->android_request(
        "GET", endpoint, header_buf, NULL,
        response_buf, sizeof(response_buf),
        &http_status, error_buf, sizeof(error_buf));

    rb->android_disconnect_wifi();

    redact_token_text(error_buf, cfg->token,
                      redacted_error, sizeof(redacted_error));

    if (http_status == 200 && android_request_status_ok(http_status, bridge_rc)) {
        rb->snprintf(text_buf, sizeof(text_buf),
                     "Auth Test\n\n"
                     "Server:      %s\n"
                     "WiFi:        %s\n"
                     "HTTP status: %d\n"
                     "Bridge rc:   %d (%s)\n"
                     "Error:       %s",
                     cfg->server_url,
                     wifi_result ? wifi_result : "(null)",
                     http_status, bridge_rc,
                     android_request_rc_name(bridge_rc),
                     redacted_error[0] ? redacted_error : "(none)");
    } else {
        format_request_failure("Auth Test", cfg, wifi_result,
                               http_status, bridge_rc, error_buf,
                               text_buf, sizeof(text_buf));
    }
    view_text("Diagnostics: Auth", text_buf);
}
#endif /* !AUDIOBOOKSHELF_FAKE_BACKEND */

static void show_diagnostics(const struct abs_config *cfg,
                             const char *header_buf)
{
#ifdef AUDIOBOOKSHELF_FAKE_BACKEND
    int menu_sel = DIAG_FAKE_MENU_INFO;
    bool running = true;

    (void)cfg;
    (void)header_buf;
    while (running) {
        MENUITEM_STRINGLIST(diag_menu, "Diagnostics", NULL,
                            "Show Build Info",
                            "Run Self Tests",
                            "Back");
        switch (rb->do_menu(&diag_menu, &menu_sel, NULL, false)) {
        case DIAG_FAKE_MENU_INFO:
            show_diag_build_info();
            break;
        case DIAG_FAKE_MENU_SELF_TESTS:
            run_self_tests();
            break;
        case DIAG_FAKE_MENU_BACK:
        default:
            running = false;
            break;
        }
    }
#else
    int menu_sel = DIAG_MENU_WIFI;
    bool running = true;

    while (running) {
        MENUITEM_STRINGLIST(diag_menu, "Diagnostics", NULL,
                            "Test WiFi",
                            "Test Auth",
                            "Run Self Tests",
                            "Back");
        switch (rb->do_menu(&diag_menu, &menu_sel, NULL, false)) {
        case DIAG_MENU_WIFI:
            diag_test_wifi();
            break;
        case DIAG_MENU_AUTH:
            diag_test_auth(cfg, header_buf);
            break;
        case DIAG_MENU_SELF_TESTS:
            run_self_tests();
            break;
        case DIAG_MENU_BACK:
        default:
            running = false;
            break;
        }
    }
#endif
}

static void show_help_settings(void)
{
    view_text("Help / Settings",
              "Audiobookshelf for Rockbox\n\n"
              "Config: " CONFIG_PATH "\n\n"
              "Keys:\n"
              "  Select  Confirm / open\n"
              "  Back    Return / cancel\n"
              "  Menu    Return to main menu\n\n"
              "server_url  Your ABS server URL\n"
              "token       API bearer token\n"
              "library_id  (optional) skip picker\n"
              "download_dir (optional) save path\n"
              "page_size   (optional, default 20)\n\n"
              "Manual progress sync spike:\n"
              "  - disabled unless a downloaded book has\n"
              "    one unambiguous local file mapping\n"
              "  - only uploads the selected book while\n"
              "    that local file is currently playing\n"
              "  - if remote progress is ahead, sync now\n"
              "    will not overwrite it automatically\n"
              "  - automatic sync remains disabled");
}

/*
 * browse_library() — drives the library -> book -> detail -> download flow.
 *
 * Under AUDIOBOOKSHELF_FAKE_BACKEND the network helpers are skipped;
 * fake data is loaded directly into the shared globals.
 *
 * Under the real backend, the original network path is unchanged.
 *
 * Returns PLUGIN_USB_CONNECTED or PLUGIN_OK.
 */
static enum plugin_status browse_library(struct abs_config *cfg,
                                         const char *header_buf)
{
    char error_buf[ERROR_BUF_SIZE];
    char text_buf[TEXT_BUF_SIZE];
    char session_lib_id[LIB_ID_BUF_SIZE];
    char session_lib_name[LIBRARY_NAME_SIZE];
    char session_book_id[BOOK_ID_SIZE];
    int  page = 0;

    session_lib_id[0]   = '\0';
    session_lib_name[0] = '\0';
    session_book_id[0]  = '\0';

#ifdef AUDIOBOOKSHELF_FAKE_BACKEND

    /* -- fake library selection ------------------------------------------- */
    populate_fake_libraries();

    if (g_lib_count == 0) {
        view_text("Audiobookshelf", "Fake backend: no libraries defined.");
        return PLUGIN_OK;
    }

    {
        int sel = show_library_picker();

        if (sel == -2)
            return PLUGIN_USB_CONNECTED;
        if (sel < 0)
            return PLUGIN_OK;

        rb->strlcpy(session_lib_id,   g_lib_ids[sel],
                    sizeof(session_lib_id));
        rb->strlcpy(session_lib_name, g_lib_names[sel],
                    sizeof(session_lib_name));
    }

    /* -- fake book browsing ----------------------------------------------- */
    while (true) {
        int sel;

        populate_fake_books(page, cfg->page_size);

        if (g_book_count == 0 && page == 0) {
            view_text("Audiobookshelf",
                      "Fake backend: no books in this library.");
            return PLUGIN_OK;
        }

        if (g_book_count == 0 && page > 0) {
            rb->snprintf(text_buf, sizeof(text_buf),
                         "No books on page %d.", page + 1);
            view_text("Audiobookshelf", text_buf);
            page--;
            continue;
        }

        sel = show_book_picker(session_lib_name);
        if (sel == BOOK_PICKER_USB)
            return PLUGIN_USB_CONNECTED;
        if (sel == BOOK_PICKER_CANCEL)
            return PLUGIN_OK;
        if (sel == BOOK_PICKER_PREV_PAGE) {
            if (page > 0)
                page--;
            continue;
        }
        if (sel == BOOK_PICKER_NEXT_PAGE) {
            page++;
            continue;
        }
        if (sel >= 0 && sel < g_book_count) {
            int detail_action;

            rb->strlcpy(session_book_id, g_book_ids[sel],
                        sizeof(session_book_id));

            populate_fake_detail(session_book_id);

            detail_action = show_book_detail_menu();
            if (detail_action == DETAIL_ACTION_USB)
                return PLUGIN_USB_CONNECTED;
            if (detail_action == DETAIL_ACTION_BACK)
                continue;

            view_text("Audiobookshelf",
                      "Action skipped\n\n"
                      "Fake backend: local playback,\n"
                      "download management, and manual\n"
                      "progress sync are not available\n"
                      "in demo mode.");
            return PLUGIN_OK;
        }
    }

#else /* real network backend */

    if (cfg->library_id[0] != '\0') {
        /* library_id configured: skip picker */
        rb->strlcpy(session_lib_id,   cfg->library_id, sizeof(session_lib_id));
        rb->strlcpy(session_lib_name, cfg->library_id, sizeof(session_lib_name));
    } else {
        char endpoint[ENDPOINT_BUF_SIZE];
        int  http_status = 0;
        int  bridge_rc;
        int  json_len;
        int  sel;

        /* Fetch accessible libraries */
        rb->splash(HZ, "Loading libraries...");
        rb->snprintf(endpoint, sizeof(endpoint),
                     "%s/api/libraries", cfg->server_url);
        g_lib_response[0] = '\0';
        error_buf[0]      = '\0';
        http_status       = 0;
        bridge_rc = rb->android_request(
            "GET", endpoint, header_buf, NULL,
            g_lib_response, sizeof(g_lib_response),
            &http_status, error_buf, sizeof(error_buf));

        rb->android_disconnect_wifi();

        if (bridge_rc < 0 || http_status != 200) {
            format_request_failure("Library fetch failed", cfg,
                                   "connected during auth validation",
                                   http_status, bridge_rc, error_buf,
                                   text_buf, sizeof(text_buf));
            view_text("Audiobookshelf", text_buf);
            return PLUGIN_OK;
        }

        json_len = (int)rb->strlen(g_lib_response);
        if (parse_libraries(json_len, error_buf, sizeof(error_buf)) < 0) {
            view_text("Audiobookshelf", error_buf);
            return PLUGIN_OK;
        }

        if (g_lib_count == 0) {
            view_text("Audiobookshelf",
                      "No accessible libraries returned.\n\n"
                      "Add library_id: <id> to\n" CONFIG_PATH "\n"
                      "to bypass library selection.");
            return PLUGIN_OK;
        }

        sel = show_library_picker();
        if (sel == -2)
            return PLUGIN_USB_CONNECTED;
        if (sel < 0)
            return PLUGIN_OK;

        rb->strlcpy(session_lib_id,   g_lib_ids[sel],   sizeof(session_lib_id));
        rb->strlcpy(session_lib_name, g_lib_names[sel], sizeof(session_lib_name));
    }

    /* Browse books page-by-page */
    {
    int effective_page_size = cfg->page_size;

    while (true) {
        int json_len;
        int sel;

        if (!fetch_books_page(cfg, header_buf, session_lib_id, page,
                              &effective_page_size,
                              error_buf, sizeof(error_buf),
                              text_buf, sizeof(text_buf))) {
            view_text("Audiobookshelf", text_buf);
            return PLUGIN_OK;
        }

        json_len = (int)rb->strlen(g_items_response);
        if (parse_books(json_len, page, effective_page_size,
                        error_buf, sizeof(error_buf)) < 0) {
            view_text("Audiobookshelf", error_buf);
            return PLUGIN_OK;
        }

        if (g_book_count == 0 && page == 0) {
            view_text("Audiobookshelf",
                      "This library has no books to browse.");
            return PLUGIN_OK;
        }

        if (g_book_count == 0 && page > 0) {
            rb->snprintf(text_buf, sizeof(text_buf),
                         "No books returned for page %d.\n\n"
                         "You may have reached the end of the library.",
                         page + 1);
            view_text("Audiobookshelf", text_buf);
            page--;
            continue;
        }

        sel = show_book_picker(session_lib_name);
        if (sel == BOOK_PICKER_USB)
            return PLUGIN_USB_CONNECTED;
        if (sel == BOOK_PICKER_CANCEL)
            return PLUGIN_OK;
        if (sel == BOOK_PICKER_PREV_PAGE) {
            if (page > 0)
                page--;
            continue;
        }
        if (sel == BOOK_PICKER_NEXT_PAGE) {
            page++;
            continue;
        }
        if (sel >= 0 && sel < g_book_count) {
            int dlen;
            bool in_detail = true;

            rb->strlcpy(session_book_id, g_book_ids[sel],
                        sizeof(session_book_id));

            if (!fetch_book_detail(cfg, header_buf, session_book_id,
                                   error_buf, sizeof(error_buf),
                                   text_buf, sizeof(text_buf))) {
                view_text("Audiobookshelf", text_buf);
                return PLUGIN_OK;
            }

            dlen = (int)rb->strlen(g_detail_response);
            if (!parse_book_detail(dlen, session_book_id,
                                   error_buf, sizeof(error_buf))) {
                view_text("Audiobookshelf", error_buf);
                return PLUGIN_OK;
            }

            refresh_local_detail_state(cfg, session_lib_id);

            while (in_detail) {
                int detail_action = show_book_detail_menu();

                if (detail_action == DETAIL_ACTION_USB)
                    return PLUGIN_USB_CONNECTED;
                if (detail_action == DETAIL_ACTION_BACK)
                    break;
                if (detail_action == DETAIL_ACTION_PLAY_LOCAL) {
                    if (!play_local_detail_file(cfg, session_lib_id,
                                                error_buf, sizeof(error_buf)))
                        view_text("Audiobookshelf", error_buf);
                    else
                        return PLUGIN_OK;
                    refresh_local_detail_state(cfg, session_lib_id);
                    continue;
                }
                if (detail_action == DETAIL_ACTION_DELETE_LOCAL) {
                    delete_local_detail(cfg, session_lib_id,
                                        text_buf, sizeof(text_buf));
                    view_text("Audiobookshelf", text_buf);
                    refresh_local_detail_state(cfg, session_lib_id);
                    continue;
                }
                if (detail_action == DETAIL_ACTION_SYNC_PROGRESS) {
                    sync_book_progress_now(cfg, text_buf, sizeof(text_buf));
                    view_text("Audiobookshelf", text_buf);
                    continue;
                }

                download_book(cfg,
                              header_buf,
                              session_lib_name,
                              session_lib_id,
                              session_book_id,
                              text_buf,
                              sizeof(text_buf));
                view_text("Audiobookshelf", text_buf);
                refresh_local_detail_state(cfg, session_lib_id);
            }
            continue;
        }
    }
    }

#endif /* AUDIOBOOKSHELF_FAKE_BACKEND */
}

/* ---- plugin entry point --------------------------------------------------- */

enum plugin_status plugin_start(const void *parameter)
{
    struct abs_config cfg;
    char header_buf[HEADER_BUF_SIZE];
    char error_buf[ERROR_BUF_SIZE];
    int menu_sel = MAIN_MENU_BROWSE;
    bool menu_running = true;

    (void)parameter;

#ifdef AUDIOBOOKSHELF_FAKE_BACKEND
    /* Fake mode: config is optional; fill defaults so diagnostics work. */
    init_config_defaults(&cfg);
    (void)read_config(&cfg, error_buf, sizeof(error_buf)); /* best-effort */
    header_buf[0] = '\0';
#else
    /* Real mode: config is required. */
    if (!read_config(&cfg, error_buf, sizeof(error_buf))) {
        view_text("Audiobookshelf", error_buf);
        return PLUGIN_OK;
    }

    rb->snprintf(header_buf, sizeof(header_buf),
                 "Authorization: Bearer %s\nAccept: application/json",
                 cfg.token);
#endif

    /* Top-level menu loop */
    while (menu_running) {
        MENUITEM_STRINGLIST(main_menu, "Audiobookshelf", NULL,
                            "Browse Library",
                            "Local Downloads",
                            "Diagnostics",
                            "Help / Settings",
                            "Quit");

        switch (rb->do_menu(&main_menu, &menu_sel, NULL, false)) {
        case MAIN_MENU_BROWSE:
#ifndef AUDIOBOOKSHELF_FAKE_BACKEND
            /* Connect WiFi and validate token before entering browse. */
            {
                char endpoint[ENDPOINT_BUF_SIZE];
                char response_buf[RESPONSE_BUF_SIZE];
                const char *wifi_result;
                int http_status = 0;
                int bridge_rc;
                char text_buf[TEXT_BUF_SIZE];
                bool auth_ok = true;

                rb->splash(HZ, "WiFi: connecting...");
                wifi_result = rb->android_connect_wifi();

                rb->splash(HZ, "Validating token...");
                rb->snprintf(endpoint, sizeof(endpoint),
                             "%s/api/me", cfg.server_url);
                response_buf[0] = '\0';
                error_buf[0]    = '\0';
                bridge_rc = rb->android_request(
                    "GET", endpoint, header_buf, NULL,
                    response_buf, sizeof(response_buf),
                    &http_status, error_buf, sizeof(error_buf));

                if (!android_request_status_ok(http_status, bridge_rc)) {
                    rb->android_disconnect_wifi();
                    format_request_failure("Auth failed", &cfg, wifi_result,
                                           http_status, bridge_rc, error_buf,
                                           text_buf, sizeof(text_buf));
                    view_text("Audiobookshelf", text_buf);
                    auth_ok = false;
                }

                if (!auth_ok)
                    break;
            }
#endif
            if (browse_library(&cfg, header_buf) == PLUGIN_USB_CONNECTED)
                return PLUGIN_USB_CONNECTED;
            break;

        case MAIN_MENU_LOCAL:
            show_local_downloads();
            break;

        case MAIN_MENU_DIAG:
            show_diagnostics(&cfg, header_buf);
            break;

        case MAIN_MENU_HELP:
            show_help_settings();
            break;

        case MAIN_MENU_QUIT:
        default:
            menu_running = false;
            break;
        }
    }

    return PLUGIN_OK;
}
