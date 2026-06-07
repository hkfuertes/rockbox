/***************************************************************************
 * Audiobookshelf plugin for Rockbox/Android
 *
 * Reads config from /sdcard/.rockbox/audiobookshelf.cfg, connects WiFi
 * through the Android bridge, validates the Audiobookshelf Bearer token,
 * fetches accessible libraries (when library_id is not configured), shows
 * a Rockbox list picker, and reports the result.  Tokens are never shown.
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
#define JSON_TOKEN_COUNT        256
#define MAX_LIBRARIES           32
#define LIBRARY_NAME_SIZE       64
#define LIBRARY_ID_SIZE         128

/* ---- config --------------------------------------------------------------- */
struct abs_config {
    char server_url[URL_BUF_SIZE];
    char token[TOKEN_BUF_SIZE];
    char library_id[LIB_ID_BUF_SIZE];
    char download_dir[DLOAD_DIR_BUF_SIZE];
    int  page_size;
};

/* ---- static globals (library list state) --------------------------------- */
static char      g_lib_names[MAX_LIBRARIES][LIBRARY_NAME_SIZE];
static char      g_lib_ids[MAX_LIBRARIES][LIBRARY_ID_SIZE];
static int       g_lib_count;
static char      g_lib_response[LIBRARIES_RESPONSE_SIZE];
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
    if (len >= buf_len)
        len = buf_len - 1;
    rb->memcpy(buf, js + tok->start, len);
    buf[len] = '\0';
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
 * into g_lib_names[] and g_lib_ids[].  Sets g_lib_count on success.
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
    int libs_idx = -1;
    i = 1;
    while (i < r) {
        if (g_tokens[i].type == JSMN_STRING &&
            tok_eq(g_lib_response, &g_tokens[i], "libraries") &&
            i + 1 < r && g_tokens[i + 1].type == JSMN_ARRAY) {
            libs_idx = i + 1;
            break;
        }
        i++; /* skip key */
        if (i < r)
            i = skip_value(i, r, g_tokens); /* skip value + subtree */
    }

    if (libs_idx < 0) {
        rb->snprintf(error_buf, error_len,
                     "Library response has no 'libraries' array (%d tokens). "
                     "Starts: %.40s",
                     r, g_lib_response);
        return -1;
    }

    /* Walk each library object inside the array. */
    int arr_end = g_tokens[libs_idx].end; /* character position of ']' + 1 */
    i = libs_idx + 1;                     /* first element */

    while (i < r && g_tokens[i].start < arr_end &&
           g_lib_count < MAX_LIBRARIES) {
        if (g_tokens[i].type != JSMN_OBJECT) {
            i = skip_value(i, r, g_tokens);
            continue;
        }

        int obj_end  = g_tokens[i].end;
        char id[LIBRARY_ID_SIZE];
        char name[LIBRARY_NAME_SIZE];
        id[0]   = '\0';
        name[0] = '\0';
        i++; /* move to first key inside the object */

        while (i + 1 < r && g_tokens[i].start < obj_end) {
            if (g_tokens[i].type == JSMN_STRING) {
                if (tok_eq(g_lib_response, &g_tokens[i], "id") &&
                    g_tokens[i + 1].type == JSMN_STRING) {
                    tok_copy(g_lib_response, &g_tokens[i + 1],
                             id, sizeof(id));
                } else if (tok_eq(g_lib_response, &g_tokens[i], "name") &&
                           g_tokens[i + 1].type == JSMN_STRING) {
                    tok_copy(g_lib_response, &g_tokens[i + 1],
                             name, sizeof(name));
                }
            }
            i++;                                     /* skip key */
            if (i < r)
                i = skip_value(i, r, g_tokens);     /* skip value + subtree */
        }

        if (id[0] != '\0') {
            rb->strlcpy(g_lib_ids[g_lib_count], id,
                        sizeof(g_lib_ids[g_lib_count]));
            rb->strlcpy(g_lib_names[g_lib_count],
                        name[0] != '\0' ? name : id,
                        sizeof(g_lib_names[g_lib_count]));
            g_lib_count++;
        }
    }

    return g_lib_count;
}

/* ---- library picker list UI ----------------------------------------------- */

static const char *lib_list_get_name(int selected_item, void *data,
                                      char *buffer, size_t buffer_len)
{
    (void)data;
    if (selected_item < 0 || selected_item >= g_lib_count)
        return "???";
    rb->strlcpy(buffer, g_lib_names[selected_item], buffer_len);
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
    const char *wifi_result;
    int http_status = 0;
    int bridge_rc;

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

    session_lib_id[0]   = '\0';
    session_lib_name[0] = '\0';

    if (cfg.library_id[0] != '\0') {
        /* library_id configured: skip picker */
        rb->strlcpy(session_lib_id, cfg.library_id, sizeof(session_lib_id));
        rb->strlcpy(session_lib_name, cfg.library_id,
                    sizeof(session_lib_name));
        rb->android_disconnect_wifi();
    } else {
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
        int json_len = (int)rb->strlen(g_lib_response);
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
        int sel = show_library_picker();
        if (sel == -2)
            return PLUGIN_USB_CONNECTED;
        if (sel < 0)
            return PLUGIN_OK; /* cancelled */

        rb->strlcpy(session_lib_id, g_lib_ids[sel], sizeof(session_lib_id));
        rb->strlcpy(session_lib_name, g_lib_names[sel],
                    sizeof(session_lib_name));
    }

    /* 7. Confirm selection */
    rb->snprintf(text_buf, sizeof(text_buf),
                 "Audiobookshelf: Ready\n\n"
                 "Server:       %s\n"
                 "Library:      %s\n"
                 "Library ID:   %s\n"
                 "Download dir: %s\n"
                 "Page size:    %d",
                 cfg.server_url,
                 session_lib_name[0] ? session_lib_name : "(unknown)",
                 session_lib_id,
                 cfg.download_dir,
                 cfg.page_size);
    view_text("Audiobookshelf", text_buf);
    return PLUGIN_OK;
}
