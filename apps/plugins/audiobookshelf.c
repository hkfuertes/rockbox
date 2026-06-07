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
#define JSON_TOKEN_COUNT        1024
#define MAX_LIBRARIES           32
#define LIBRARY_NAME_SIZE       64
#define LIBRARY_ID_SIZE         128
#define MAX_BOOKS_PER_PAGE      MAX_PAGE_SIZE
#define BOOK_TITLE_SIZE         160
#define BOOK_ID_SIZE            128
#define BOOK_META_SIZE          64
#define LIST_TITLE_SIZE         96

#define BOOK_PICKER_CANCEL      (-1)
#define BOOK_PICKER_USB         (-2)
#define BOOK_PICKER_PREV_PAGE   (-3)
#define BOOK_PICKER_NEXT_PAGE   (-4)

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
static char      g_list_title[LIST_TITLE_SIZE];

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
    char session_book_title[BOOK_TITLE_SIZE];
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
    session_book_title[0] = '\0';

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
            rb->strlcpy(session_book_id, g_book_ids[sel],
                        sizeof(session_book_id));
            rb->strlcpy(session_book_title, g_book_titles[sel],
                        sizeof(session_book_title));
            break;
        }
    }

    /* 8. Confirm selection for the next slice. */
    rb->snprintf(text_buf, sizeof(text_buf),
                 "Audiobookshelf: Book selected\n\n"
                 "Server:       %s\n"
                 "Library:      %s\n"
                 "Library ID:   %s\n"
                 "Book:         %s\n"
                 "Book ID:      %s\n"
                 "Download dir: %s\n"
                 "Page size:    %d",
                 cfg.server_url,
                 session_lib_name[0] ? session_lib_name : "(unknown)",
                 session_lib_id,
                 session_book_title[0] ? session_book_title : "(unknown)",
                 session_book_id,
                 cfg.download_dir,
                 cfg.page_size);
    view_text("Audiobookshelf", text_buf);
    return PLUGIN_OK;
}
