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
                     "Bridge rc:    %d\n\n"
                     "Error:\n%s",
                     library_id, page, cfg->page_size,
                     wifi_result ? wifi_result : "(null)",
                     http_status, bridge_rc,
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

    bridge_rc = rb->android_request(
        "GET", endpoint, header_buf, NULL,
        g_detail_response, sizeof(g_detail_response),
        &http_status, error_buf, error_len);

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
                "\"duration\":3661,\"audioFiles\":[{\"index\":0},{\"index\":1}]}}",
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
                    g_book_detail.has_audio_files,
                    "parse_book_detail parses metadata and audio files");

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
                    build_download_destination("/sdcard/audiobookshelf/downloads",
                                               "../Bad Book", "id/42",
                                               path_buf, sizeof(path_buf),
                                               error_buf, sizeof(error_buf)) &&
                    contains_text(path_buf, "/Bad_Book/") &&
                    contains_text(path_buf, "Bad_Book-id_42.abs") &&
                    path_is_under_base(path_buf,
                                       "/sdcard/audiobookshelf/downloads"),
                    "build_download_destination sanitizes title and id");

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

    if (http_status == 200 && bridge_rc >= 0) {
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
              "page_size   (optional, default 20)");
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
                      "Download skipped\n\n"
                      "Fake backend: downloads are not\n"
                      "available in demo mode.");
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
    while (true) {
        int json_len;
        int sel;

        if (!fetch_books_page(cfg, header_buf, session_lib_id, page,
                              error_buf, sizeof(error_buf),
                              text_buf, sizeof(text_buf))) {
            view_text("Audiobookshelf", text_buf);
            return PLUGIN_OK;
        }

        json_len = (int)rb->strlen(g_items_response);
        if (parse_books(json_len, page, cfg->page_size,
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
            int dlen;

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

            detail_action = show_book_detail_menu();
            if (detail_action == DETAIL_ACTION_USB)
                return PLUGIN_USB_CONNECTED;
            if (detail_action == DETAIL_ACTION_BACK)
                continue;

            download_book(cfg,
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

                if (bridge_rc < 0 || http_status != 200) {
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
