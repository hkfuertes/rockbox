/*
 * MIT License
 *
 * Copyright (c) 2010 Serge Zaitsev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * jsmn — minimalistic JSON tokenizer
 * https://github.com/zserge/jsmn
 *
 * All functions declared static so this header may be included in a single
 * translation unit without link-time conflicts.
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT    = 1,
    JSMN_ARRAY     = 2,
    JSMN_STRING    = 3,
    JSMN_PRIMITIVE = 4
} jsmntype_t;

enum jsmnerr {
    JSMN_ERROR_NOMEM = -1, /* token array too small */
    JSMN_ERROR_INVAL = -2, /* invalid character in JSON string */
    JSMN_ERROR_PART  = -3  /* JSON string is incomplete */
};

typedef struct {
    jsmntype_t type;
    int start; /* first character inside the token */
    int end;   /* one-past-last character (exclusive) */
    int size;  /* objects: key count; arrays: element count */
} jsmntok_t;

typedef struct {
    unsigned int pos;
    unsigned int toknext;
    int          toksuper; /* index of deepest open container, or -1 */
} jsmn_parser;

static void jsmn_init(jsmn_parser *parser);
static int  jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
                        jsmntok_t *tokens, unsigned int num_tokens);

/* ---- implementation (all internal helpers use double-underscore prefix) -- */

static jsmntok_t *jsmn__alloc_token(jsmn_parser *p,
                                     jsmntok_t *tokens, size_t num_tokens)
{
    jsmntok_t *tok;
    if (p->toknext >= num_tokens)
        return NULL;
    tok = &tokens[p->toknext++];
    tok->start = tok->end = -1;
    tok->size  = 0;
    return tok;
}

static void jsmn__fill_token(jsmntok_t *token, jsmntype_t type,
                              int start, int end)
{
    token->type  = type;
    token->start = start;
    token->end   = end;
    token->size  = 0;
}

static int jsmn__parse_primitive(jsmn_parser *p, const char *js, size_t len,
                                  jsmntok_t *tokens, size_t num_tokens)
{
    jsmntok_t *token;
    int start = (int)p->pos;

    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        switch (js[p->pos]) {
        case '\t': case '\r': case '\n': case ' ':
        case ',':  case ']':  case '}':
            goto found;
        default:
            if ((unsigned char)js[p->pos] < 32) {
                p->pos = (unsigned int)start;
                return JSMN_ERROR_INVAL;
            }
        }
    }
found:
    if (tokens == NULL) {
        p->pos--;
        return 0;
    }
    token = jsmn__alloc_token(p, tokens, num_tokens);
    if (token == NULL) {
        p->pos = (unsigned int)start;
        return JSMN_ERROR_NOMEM;
    }
    jsmn__fill_token(token, JSMN_PRIMITIVE, start, (int)p->pos);
    p->pos--;
    return 0;
}

static int jsmn__parse_string(jsmn_parser *p, const char *js, size_t len,
                               jsmntok_t *tokens, size_t num_tokens)
{
    jsmntok_t *token;
    int start = (int)p->pos;

    p->pos++; /* skip opening quote */

    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        char c = js[p->pos];

        if (c == '"') {
            if (tokens == NULL)
                return 0;
            token = jsmn__alloc_token(p, tokens, num_tokens);
            if (token == NULL) {
                p->pos = (unsigned int)start;
                return JSMN_ERROR_NOMEM;
            }
            /* start+1 skips opening quote; end = closing quote position */
            jsmn__fill_token(token, JSMN_STRING, start + 1, (int)p->pos);
            return 0;
        }

        if (c == '\\' && p->pos + 1 < len) {
            int i;
            p->pos++;
            switch (js[p->pos]) {
            case '"': case '/': case '\\': case 'b':
            case 'f': case 'r': case 'n':  case 't':
                break;
            case 'u':
                p->pos++;
                for (i = 0; i < 4 && p->pos < len; i++, p->pos++) {
                    char h = js[p->pos];
                    if (!((h >= '0' && h <= '9') ||
                          (h >= 'A' && h <= 'F') ||
                          (h >= 'a' && h <= 'f'))) {
                        p->pos = (unsigned int)start;
                        return JSMN_ERROR_INVAL;
                    }
                }
                p->pos--;
                break;
            default:
                p->pos = (unsigned int)start;
                return JSMN_ERROR_INVAL;
            }
        }
    }
    p->pos = (unsigned int)start;
    return JSMN_ERROR_PART;
}

static void jsmn_init(jsmn_parser *parser)
{
    parser->pos      = 0;
    parser->toknext  = 0;
    parser->toksuper = -1;
}

static int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
                       jsmntok_t *tokens, unsigned int num_tokens)
{
    int r;
    int i;
    jsmntok_t *token;
    int count = (int)parser->toknext;

    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char       c;
        jsmntype_t type;

        c = js[parser->pos];
        switch (c) {
        case '{': case '[':
            count++;
            if (tokens != NULL) {
                token = jsmn__alloc_token(parser, tokens, num_tokens);
                if (token == NULL)
                    return JSMN_ERROR_NOMEM;
                if (parser->toksuper != -1)
                    tokens[parser->toksuper].size++;
                token->type     = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
                token->start    = (int)parser->pos;
                parser->toksuper = (int)(parser->toknext - 1);
            }
            break;

        case '}': case ']':
            if (tokens == NULL)
                break;
            type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
            /* find the matching unclosed container */
            for (i = (int)parser->toknext - 1; i >= 0; i--) {
                token = &tokens[i];
                if (token->start != -1 && token->end == -1) {
                    if (token->type != type)
                        return JSMN_ERROR_INVAL;
                    parser->toksuper = -1;
                    token->end = (int)parser->pos + 1;
                    break;
                }
            }
            if (i == -1)
                return JSMN_ERROR_INVAL;
            /* restore toksuper to the nearest still-open container */
            for (; i >= 0; i--) {
                token = &tokens[i];
                if (token->start != -1 && token->end == -1) {
                    parser->toksuper = i;
                    break;
                }
            }
            break;

        case '"':
            r = jsmn__parse_string(parser, js, len, tokens, num_tokens);
            if (r < 0)
                return r;
            count++;
            if (parser->toksuper != -1 && tokens != NULL)
                tokens[parser->toksuper].size++;
            break;

        case '\t': case '\r': case '\n': case ' ':
            break;

        case ':':
            /* next value belongs to the most recently parsed key */
            parser->toksuper = (int)parser->toknext - 1;
            break;

        case ',':
            /* after a value, restore toksuper to the enclosing container */
            if (tokens != NULL && parser->toksuper != -1 &&
                tokens[parser->toksuper].type != JSMN_ARRAY &&
                tokens[parser->toksuper].type != JSMN_OBJECT) {
                for (i = (int)parser->toknext - 1; i >= 0; i--) {
                    if ((tokens[i].type == JSMN_ARRAY ||
                         tokens[i].type == JSMN_OBJECT) &&
                        tokens[i].start != -1 && tokens[i].end == -1) {
                        parser->toksuper = i;
                        break;
                    }
                }
            }
            break;

        default:
            r = jsmn__parse_primitive(parser, js, len, tokens, num_tokens);
            if (r < 0)
                return r;
            count++;
            if (parser->toksuper != -1 && tokens != NULL)
                tokens[parser->toksuper].size++;
            break;
        }
    }

    if (tokens != NULL) {
        for (i = (int)parser->toknext - 1; i >= 0; i--) {
            if (tokens[i].start != -1 && tokens[i].end == -1)
                return JSMN_ERROR_PART;
        }
    }

    return count;
}

#endif /* JSMN_H */
