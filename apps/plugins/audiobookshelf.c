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

#define LIBRARIES_RESPONSE_SIZE 4096
#define ITEMS_RESPONSE_SIZE     16384
#define DETAIL_RESPONSE_SIZE    32768
#define JSON_TOKEN_COUNT        1024
#define MAX_LIBRARIES           32
#define LIBRARY_NAME_SIZE       64
#define LIBRARY_ID_SIZE         128
#define MAX_BOOKS_PER_PAGE      MAX_PAGE_SIZE
#define BOOK_TITLE_SIZE         160
#define BOOK_ID_SIZE            128
#define BOOK_META_SIZE          64
#define DETAIL_LINE_SIZE        192
#define MAX_DETAIL_LINES        10
#define LIST_TITLE_SIZE         96

#define BOOK_PICKER_CANCEL      (-1)
#define BOOK_PICKER_USB         (-2)
#define BOOK_PICKER_PREV_PAGE   (-3)
#define BOOK_PICKER_NEXT_PAGE   (-4)

#define DETAIL_ACTION_BACK      0
#define DETAIL_ACTION_DOWNLOAD  1
#define DETAIL_ACTION_USB      -2

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
    int duration_seconds;
    int audio_file_count;
    bool audio_files_known;
    bool has_audio_files;
};

static struct abs_book_detail g_book_detail;
static char      g_detail_lines[MAX_DETAIL_LINES][DETAIL_LINE_SIZE];
static int       g_detail_line_count;
static int       g_detail_action_start;

static jsmntok_t g_tokens[JSON_TOKEN_COUNT];

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
                                       char *dest_buf,
                                       size_t dest_len,
                                       char *error_buf,
                                       size_t error_len)
{
    char folder[BOOK_TITLE_SIZE];
    char file_title[BOOK_TITLE_SIZE];
    char file_id[BOOK_ID_SIZE];

    sanitize_path_component(title, folder, sizeof(folder));
    sanitize_path_component(title, file_title, sizeof(file_title));
    sanitize_path_component(item_id, file_id, sizeof(file_id));

    if (path_has_traversal(folder) || path_has_traversal(file_title) ||
        path_has_traversal(file_id)) {
        rb->snprintf(error_buf, error_len,
                     "Download destination rejected before bridge: sanitized title is unsafe");
        return false;
    }

    rb->snprintf(dest_buf, dest_len, "%s/%s/%s-%s.abs",
                 root,
                 folder,
                 file_title,
                 file_id);

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
    char dest_buf[MAX_PATH];
    char error_buf[ERROR_BUF_SIZE];
    char redacted_error[ERROR_BUF_SIZE];
    const char *wifi_result;
    const char *book_title;
    int http_status = 0;
    int bridge_rc;

    book_title = g_book_detail.title[0] != '\0' ? g_book_detail.title : "(untitled)";
    error_buf[0] = '\0';
    redacted_error[0] = '\0';

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

    if (!build_download_destination(root_buf, book_title, item_id,
                                    dest_buf, sizeof(dest_buf),
                                    error_buf, sizeof(error_buf))) {
        rb->snprintf(text_buf, text_len,
                     "Download blocked\n\n"
                     "Book: %s\n"
                     "Book ID: %s\n"
                     "Root: %s\n\n"
                     "%s",
                     book_title,
                     item_id,
                     root_buf,
                     error_buf);
        return false;
    }

    rb->snprintf(endpoint, sizeof(endpoint), "%s/api/items/%s/download",
                 cfg->server_url, item_id);

    rb->splash(HZ, "WiFi: connecting...");
    wifi_result = rb->android_connect_wifi();

    rb->splash(HZ, "Download: running...");
    bridge_rc = rb->android_download(endpoint,
                                     header_buf,
                                     dest_buf,
                                     &http_status,
                                     error_buf,
                                     sizeof(error_buf));

    rb->android_disconnect_wifi();
    redact_token_text(error_buf, cfg->token,
                      redacted_error, sizeof(redacted_error));

    rb->snprintf(text_buf, text_len,
                 "%s\n\n"
                 "Server: %s\n"
                 "Library: %s\n"
                 "Library ID: %s\n"
                 "Book: %s\n"
                 "Book ID: %s\n"
                 "Destination: %s\n"
                 "WiFi: %s\n"
                 "HTTP status: %d\n"
                 "Bridge rc: %d\n"
                 "Bridge error (redacted): %s",
                 (bridge_rc >= 0 && http_status >= 200 && http_status < 300) ?
                    "Download finished" : "Download failed",
                 cfg->server_url,
                 library_name && library_name[0] ? library_name : "(unknown)",
                 library_id && library_id[0] ? library_id : "(unknown)",
                 book_title,
                 item_id && item_id[0] ? item_id : "(unknown)",
                 dest_buf,
                 safe_text(wifi_result),
                 http_status,
                 bridge_rc,
                 safe_text(redacted_error));

    return bridge_rc >= 0 && http_status >= 200 && http_status < 300;
}

/* ---- config parsing ------------------------------------------------------- */

static const char *config_error_hint =
    "Expected entries in " CONFIG_PATH ":\n"
    "  server_url: https://your.abs.server\n"
    "  token: your_api_token\n"
    "  library_id: (optional)\n"
    "  download_dir: (optional)\n"
    "  page_size: (optional, default 20)";

static bool read_config(struct abs_config *cfg,
                        char *error_buf, size_t error_len)
{
    int fd;
    char line[LINE_BUF_SIZE];

    cfg->server_url[0] = '\0';
    cfg->token[0]      = '\0';
    cfg->library_id[0] = '\0';
    cfg->page_size     = DEFAULT_PAGE_SIZE;
    rb->strlcpy(cfg->download_dir, DEFAULT_DOWNLOAD_DIR,
                sizeof(cfg->download_dir));

    fd = rb->open(CONFIG_PATH, O_RDONLY);
    if (fd < 0) {
        rb->snprintf(error_buf, error_len,
                     "Cannot open " CONFIG_PATH "\n\n%s", config_error_hint);
        return false;
    }

    while (rb->read_line(fd, line, sizeof(line)) > 0) {
        char *t = trim_ws(line);
        char *v;

        if (t[0] == '\0' || t[0] == '#')
            continue;

        if (!rb->strncmp(t, "server_url:", 11)) {
            v = trim_ws(t + 11);
            rb->strlcpy(cfg->server_url, v, sizeof(cfg->server_url));
        } else if (!rb->strncmp(t, "token:", 6)) {
            v = trim_ws(t + 6);
            rb->strlcpy(cfg->token, v, sizeof(cfg->token));
        } else if (!rb->strncmp(t, "library_id:", 11)) {
            v = trim_ws(t + 11);
            rb->strlcpy(cfg->library_id, v, sizeof(cfg->library_id));
        } else if (!rb->strncmp(t, "download_dir:", 13)) {
            v = trim_ws(t + 13);
            if (v[0] != '\0')
                rb->strlcpy(cfg->download_dir, v, sizeof(cfg->download_dir));
        } else if (!rb->strncmp(t, "page_size:", 10)) {
            v = trim_ws(t + 10);
            if (v[0] != '\0')
                cfg->page_size = rb->atoi(v);
        }
    }

    rb->close(fd);
    cfg->page_size = sanitize_page_size(cfg->page_size);

    if (cfg->server_url[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Missing 'server_url' in " CONFIG_PATH "\n\n%s",
                     config_error_hint);
        return false;
    }

    if (cfg->token[0] == '\0') {
        rb->snprintf(error_buf, error_len,
                     "Missing 'token' in " CONFIG_PATH "\n\n%s",
                     config_error_hint);
        return false;
    }

    return true;
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
        char id[BOOK_ID_SIZE];
        char title[BOOK_TITLE_SIZE];
        char author[BOOK_META_SIZE];
        char series[BOOK_META_SIZE];

        if (g_tokens[i].type != JSMN_OBJECT) {
            i = skip_value(i, r, g_tokens);
            continue;
        }

        id[0] = '\0';
        title[0] = '\0';
        author[0] = '\0';
        series[0] = '\0';

        id_idx = find_object_value(g_items_response, g_tokens, r, i, "id");
        title_idx = find_object_value(g_items_response, g_tokens, r, i, "title");
        if (title_idx < 0)
            title_idx = find_object_value(g_items_response, g_tokens, r, i, "name");
        author_idx = find_object_value(g_items_response, g_tokens, r, i,
                                       "authorName");
        series_idx = find_object_value(g_items_response, g_tokens, r, i,
                                       "seriesName");

        media_idx = find_object_value(g_items_response, g_tokens, r, i, "media");
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

        if (id[0] != '\0') {
            rb->strlcpy(g_book_ids[g_book_count], id,
                        sizeof(g_book_ids[g_book_count]));
            build_book_title(g_book_titles[g_book_count],
                             sizeof(g_book_titles[g_book_count]),
                             title, author, series);
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

    g_detail_action_start = g_detail_line_count;
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
            if (sel == g_detail_action_start) {
                result = DETAIL_ACTION_DOWNLOAD;
                done = true;
            } else if (sel == g_detail_action_start + 1) {
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

static bool fetch_books_page(const struct abs_config *cfg,
                             const char *header_buf,
                             const char *library_id,
                             int page,
                             char *error_buf, size_t error_len,
                             char *text_buf, size_t text_len)
{
    char endpoint[ENDPOINT_BUF_SIZE];
    const char *wifi_result;
    int http_status = 0;
    int bridge_rc;

    rb->splashf(HZ, "Loading page %d...", page + 1);
    wifi_result = rb->android_connect_wifi();

    rb->snprintf(endpoint, sizeof(endpoint),
                 "%s/api/libraries/%s/items?limit=%d&page=%d&sort=media.metadata.title&desc=0&minified=1",
                 cfg->server_url, library_id, cfg->page_size, page);

    g_items_response[0] = '\0';
    error_buf[0] = '\0';

    bridge_rc = rb->android_request(
        "GET", endpoint, header_buf, NULL,
        g_items_response, sizeof(g_items_response),
        &http_status, error_buf, error_len);

    rb->android_disconnect_wifi();

    if (bridge_rc < 0 || http_status != 200) {
        rb->snprintf(text_buf, text_len,
                     "Book list fetch failed\n\n"
                     "Library ID:   %s\n"
                     "Page:         %d\n"
                     "Page size:    %d\n"
                     "WiFi:         %s\n"
                     "HTTP status:  %d\n"
                     "Bridge rc:    %d\n\n"
                     "Error:\n%s",
                     library_id, page, cfg->page_size,
                     wifi_result ? wifi_result : "(null)",
                     http_status, bridge_rc,
                     error_buf[0] ? error_buf : "(none)");
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

    bridge_rc = rb->android_request(
        "GET", endpoint, header_buf, NULL,
        g_detail_response, sizeof(g_detail_response),
        &http_status, error_buf, error_len);

    rb->android_disconnect_wifi();

    if (bridge_rc < 0 || http_status != 200) {
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
                     error_buf[0] ? error_buf : "(none)");
        return false;
    }

    return true;
}

/* ---- plugin entry point --------------------------------------------------- */

enum plugin_status plugin_start(const void *parameter)
{
    struct abs_config cfg;
    char header_buf[HEADER_BUF_SIZE];
    char endpoint[ENDPOINT_BUF_SIZE];
    char response_buf[RESPONSE_BUF_SIZE];
    char error_buf[ERROR_BUF_SIZE];
    char text_buf[TEXT_BUF_SIZE];
    char session_lib_id[LIB_ID_BUF_SIZE];
    char session_lib_name[LIBRARY_NAME_SIZE];
    char session_book_id[BOOK_ID_SIZE];
    const char *wifi_result;
    int http_status = 0;
    int bridge_rc;
    int page = 0;

    (void)parameter;

    /* 1. Read config */
    if (!read_config(&cfg, error_buf, sizeof(error_buf))) {
        view_text("Audiobookshelf", error_buf);
        return PLUGIN_OK;
    }

    /* Build Authorization header — token is never placed in text_buf below. */
    rb->snprintf(header_buf, sizeof(header_buf),
                 "Authorization: Bearer %s\nAccept: application/json",
                 cfg.token);

    /* 2. Connect WiFi */
    rb->splash(HZ, "WiFi: connecting...");
    wifi_result = rb->android_connect_wifi();

    /* 3. Validate token via GET /api/me */
    rb->splash(HZ, "Validating token...");
    rb->snprintf(endpoint, sizeof(endpoint), "%s/api/me", cfg.server_url);
    response_buf[0] = '\0';
    error_buf[0]    = '\0';
    bridge_rc = rb->android_request(
        "GET", endpoint, header_buf, NULL,
        response_buf, sizeof(response_buf),
        &http_status, error_buf, sizeof(error_buf));

    if (http_status != 200) {
        rb->android_disconnect_wifi();
        rb->snprintf(text_buf, sizeof(text_buf),
                     "Auth failed\n\n"
                     "Server:      %s\n\n"
                     "WiFi:        %s\n"
                     "HTTP status: %d\n"
                     "Bridge rc:   %d\n\n"
                     "Error:\n%s",
                     cfg.server_url,
                     wifi_result ? wifi_result : "(null)",
                     http_status, bridge_rc,
                     error_buf[0] ? error_buf : "(none)");
        view_text("Audiobookshelf", text_buf);
        return PLUGIN_OK;
    }

    session_lib_id[0]     = '\0';
    session_lib_name[0]   = '\0';
    session_book_id[0]    = '\0';

    if (cfg.library_id[0] != '\0') {
        /* library_id configured: skip picker */
        rb->strlcpy(session_lib_id, cfg.library_id, sizeof(session_lib_id));
        rb->strlcpy(session_lib_name, cfg.library_id,
                    sizeof(session_lib_name));
        rb->android_disconnect_wifi();
    } else {
        int json_len;
        int sel;

        /* 4. Fetch accessible libraries */
        rb->splash(HZ, "Loading libraries...");
        rb->snprintf(endpoint, sizeof(endpoint),
                     "%s/api/libraries", cfg.server_url);
        g_lib_response[0] = '\0';
        error_buf[0]      = '\0';
        http_status       = 0;
        bridge_rc = rb->android_request(
            "GET", endpoint, header_buf, NULL,
            g_lib_response, sizeof(g_lib_response),
            &http_status, error_buf, sizeof(error_buf));

        /* All network operations done — disconnect before blocking UI */
        rb->android_disconnect_wifi();

        if (http_status != 200) {
            rb->snprintf(text_buf, sizeof(text_buf),
                         "Library fetch failed\n\n"
                         "Server:      %s\n"
                         "HTTP status: %d\n"
                         "Bridge rc:   %d\n\n"
                         "Error:\n%s",
                         cfg.server_url, http_status, bridge_rc,
                         error_buf[0] ? error_buf : "(none)");
            view_text("Audiobookshelf", text_buf);
            return PLUGIN_OK;
        }

        /* 5. Parse library JSON */
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

        /* 6. Show picker */
        sel = show_library_picker();
        if (sel == -2)
            return PLUGIN_USB_CONNECTED;
        if (sel < 0)
            return PLUGIN_OK; /* cancelled */

        rb->strlcpy(session_lib_id, g_lib_ids[sel], sizeof(session_lib_id));
        rb->strlcpy(session_lib_name, g_lib_names[sel],
                    sizeof(session_lib_name));
    }

    /* 7. Browse books page-by-page. */
    while (true) {
        int json_len;
        int sel;

        if (!fetch_books_page(&cfg, header_buf, session_lib_id, page,
                              error_buf, sizeof(error_buf),
                              text_buf, sizeof(text_buf))) {
            view_text("Audiobookshelf", text_buf);
            return PLUGIN_OK;
        }

        json_len = (int)rb->strlen(g_items_response);
        if (parse_books(json_len, page, cfg.page_size,
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
            int detail_action;
            int json_len;

            rb->strlcpy(session_book_id, g_book_ids[sel],
                        sizeof(session_book_id));

            if (!fetch_book_detail(&cfg, header_buf, session_book_id,
                                   error_buf, sizeof(error_buf),
                                   text_buf, sizeof(text_buf))) {
                view_text("Audiobookshelf", text_buf);
                return PLUGIN_OK;
            }

            json_len = (int)rb->strlen(g_detail_response);
            if (!parse_book_detail(json_len, session_book_id,
                                   error_buf, sizeof(error_buf))) {
                view_text("Audiobookshelf", error_buf);
                return PLUGIN_OK;
            }

            detail_action = show_book_detail_menu();
            if (detail_action == DETAIL_ACTION_USB)
                return PLUGIN_USB_CONNECTED;
            if (detail_action == DETAIL_ACTION_BACK)
                continue;

            download_book(&cfg,
                          header_buf,
                          session_lib_name,
                          session_lib_id,
                          session_book_id,
                          text_buf,
                          sizeof(text_buf));
            view_text("Audiobookshelf", text_buf);
            return PLUGIN_OK;
        }
    }
}
