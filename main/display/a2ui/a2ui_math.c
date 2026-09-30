/**
 * @file a2ui_math.c
 * @brief 设备端 LaTeX 子集：解析 → TeX 盒模型（width/height/depth）→ LVGL 绝对定位。
 *
 * 绘制要点：lv_label 必须按「数学基线」放置（label_y = bl - font_ascent），
 * 不能把盒顶当作 label 顶，否则不同升部字符（如 n 与 !）会错位。
 * 大括号：矮用整字；高用 U+239B.. 零件竖拼。分数线/根号顶线为实心 lv_obj。
 */

#include "a2ui_math.h"

#include "fontpack_lvgl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

static const char *TAG = "a2ui_math";

#define MATH_POOL_MAX 160
#define MATH_LATEX_MAX 768
#define MATH_CHILD_MAX 24

/* ---------- AST ---------- */

typedef enum {
    MN_ORD = 0,   /* 原子（字符/符号串） */
    MN_ROW,       /* 水平列表 */
    MN_FRAC,
    MN_SQRT,
    MN_SCRIPT,    /* base + optional sub/sup */
    MN_DELIM,     /* left body right */
    MN_SPACE,
} math_kind_t;

typedef enum {
    MF_ITALIC = 0,
    MF_UPRIGHT,
    MF_BOLD,
} math_face_t;

typedef struct MathNode MathNode;

struct MathNode {
    math_kind_t kind;
    math_face_t face;
    uint16_t size; /* px */
    /* ORD: UTF-8 in text[]; SPACE: width in space_w */
    char text[24];
    int16_t space_w;
    char left_delim;  /* ( [ { | . */
    char right_delim;
    MathNode *a; /* frac num / sqrt body / script base / delim body */
    MathNode *b; /* frac den / script sub */
    MathNode *c; /* script sup / sqrt index */
    MathNode *kids[MATH_CHILD_MAX];
    uint8_t nkids;
    /* layout：TeX 盒 w / h(基线上) / d(基线下)；place 后 bl=绝对基线 y，x/y=盒顶左 */
    int16_t w, h, d;
    int16_t x, y;
    int16_t bl;
};

typedef struct {
    MathNode pool[MATH_POOL_MAX];
    int n;
    const char *p;
    const char *end;
    bool display;
    uint16_t size_text;
    uint16_t size_script;
    bool ok;
} MathParser;

/* ---------- fonts ---------- */

static void glyph_metrics(const lv_font_t *font, uint32_t cp, int16_t *adv, int16_t *above,
                          int16_t *below);

static const lv_font_t *math_font(uint16_t size)
{
    const lv_font_t *f = fontpack_lv_font_get(size, A2UI_MATH_BPP);
    if (!f) {
        f = fontpack_lv_font_get(A2UI_MATH_SIZE_TEXT, A2UI_MATH_BPP);
    }
    if (!f) {
        f = fontpack_lv_font_ui();
    }
    return f;
}

static int16_t font_ascent(const lv_font_t *font)
{
    if (!font) {
        return A2UI_MATH_SIZE_TEXT;
    }
    int16_t a = (int16_t)(font->line_height - font->base_line);
    return a > 0 ? a : (int16_t)font->line_height;
}

static int16_t glyph_bitmap_top(const lv_font_t *font, uint32_t cp)
{
    int16_t adv, above, below;
    glyph_metrics(font, cp, &adv, &above, &below);
    (void)adv;
    (void)below;
    return above; /* = bitmap_top */
}

static int16_t glyph_advance(const lv_font_t *font, uint32_t cp)
{
    if (!font) {
        return 0;
    }
    lv_font_glyph_dsc_t dsc;
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
        return (int16_t)(font->line_height / 3);
    }
    return (int16_t)dsc.adv_w;
}

static void glyph_metrics(const lv_font_t *font, uint32_t cp, int16_t *adv, int16_t *above,
                          int16_t *below)
{
    *adv = 0;
    *above = 0;
    *below = 0;
    if (!font) {
        return;
    }
    lv_font_glyph_dsc_t dsc;
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
        *adv = (int16_t)(font->line_height / 3);
        *above = (int16_t)(font->line_height - font->base_line);
        *below = (int16_t)font->base_line;
        return;
    }
    *adv = (int16_t)dsc.adv_w;
    /* ofs_y = bitmap_top - box_h → bitmap_top = ofs_y + box_h；底相对基线 = ofs_y */
    int16_t top = (int16_t)(dsc.ofs_y + dsc.box_h);
    int16_t bot = dsc.ofs_y;
    if (top > *above) {
        *above = top;
    }
    if (-bot > *below) {
        *below = (int16_t)(-bot);
    }
    if (*above == 0 && *below == 0) {
        *above = (int16_t)(font->line_height - font->base_line);
        *below = (int16_t)font->base_line;
    }
}

/* ---------- UTF-8 helpers ---------- */

static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = 0;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = 0;
    return 4;
}

static uint32_t math_italic_cp(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return 0x1D434u + (cp - 'A');
    }
    if (cp >= 'a' && cp <= 'z') {
        /* U+1D455 未定义，h → U+210E */
        if (cp == 'h') {
            return 0x210Eu;
        }
        uint32_t base = 0x1D44Eu + (cp - 'a');
        if (cp > 'h') {
            base -= 1; /* 跳过空洞后码位连续：实际 a..g = 1D44E..1D454, i=1D456... */
            /* Unicode: a=1D44E ... g=1D454, (1D455 reserved), i=1D456 ... z=1D467
             * So for c>'h': offset = (c-'a') but after h the code is 1D44E+(c-'a')
             * Wait: i should be 1D456 = 1D44E + 8, and i-'a'=8. h would be 1D44E+7=1D455.
             * So only h is special; i..z use 1D44E+(c-'a') which lands on 1D456.. correctly
             * because 1D455 is skipped in assignment... 1D44E+8 = 1D456. Yes for i.
             * For h: 1D44E+7 = 1D455 which is reserved → use 210E.
             */
        }
        return 0x1D44Eu + (cp - 'a');
    }
    return cp;
}

static uint32_t math_bold_cp(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return 0x1D400u + (cp - 'A');
    }
    if (cp >= 'a' && cp <= 'z') {
        return 0x1D41Au + (cp - 'a');
    }
    return cp;
}

/* ---------- pool / parse ---------- */

static MathNode *alloc_node(MathParser *mp, math_kind_t kind)
{
    if (mp->n >= MATH_POOL_MAX) {
        mp->ok = false;
        return NULL;
    }
    MathNode *n = &mp->pool[mp->n++];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->face = MF_ITALIC;
    n->size = mp->size_text;
    return n;
}

static void skip_ws(MathParser *mp)
{
    while (mp->p < mp->end && (*mp->p == ' ' || *mp->p == '\t' || *mp->p == '\n' || *mp->p == '\r')) {
        mp->p++;
    }
}

static bool starts_with(MathParser *mp, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(mp->end - mp->p) >= n && memcmp(mp->p, s, n) == 0;
}

static void strip_math_delimiters(char *buf)
{
    /* 去掉外层 $ / $$ / \( \) / \[ \] */
    char *s = buf;
    while (*s == ' ' || *s == '\t' || *s == '\n') {
        s++;
    }
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n')) {
        s[--len] = 0;
    }
    if (len >= 2 && s[0] == '$' && s[len - 1] == '$') {
        if (len >= 4 && s[1] == '$' && s[len - 2] == '$') {
            memmove(buf, s + 2, len - 4);
            buf[len - 4] = 0;
        } else {
            memmove(buf, s + 1, len - 2);
            buf[len - 2] = 0;
        }
        return;
    }
    if (len >= 4 && s[0] == '\\' && s[1] == '(' && s[len - 2] == '\\' && s[len - 1] == ')') {
        memmove(buf, s + 2, len - 4);
        buf[len - 4] = 0;
        return;
    }
    if (len >= 4 && s[0] == '\\' && s[1] == '[' && s[len - 2] == '\\' && s[len - 1] == ']') {
        memmove(buf, s + 2, len - 4);
        buf[len - 4] = 0;
        return;
    }
    if (s != buf) {
        memmove(buf, s, len + 1);
    }
}

static MathNode *parse_expr(MathParser *mp, char stop);
static MathNode *parse_atom(MathParser *mp);
static MathNode *parse_scripted(MathParser *mp);

typedef struct {
    const char *name;
    const char *utf8; /* precomposed UTF-8 or NULL if special */
    uint32_t cp;
} CmdMap;

static const CmdMap k_cmds[] = {
    {"approx", NULL, 0x2248},
    {"pm", NULL, 0x00B1},
    {"mp", NULL, 0x2213},
    {"times", NULL, 0x00D7},
    {"cdot", NULL, 0x22C5},
    {"div", NULL, 0x00F7},
    {"leq", NULL, 0x2264},
    {"geq", NULL, 0x2265},
    {"neq", NULL, 0x2260},
    {"ne", NULL, 0x2260},
    {"infty", NULL, 0x221E},
    {"sum", NULL, 0x2211},
    {"prod", NULL, 0x220F},
    {"int", NULL, 0x222B},
    {"partial", NULL, 0x2202},
    {"nabla", NULL, 0x2207},
    {"infty", NULL, 0x221E},
    {"rightarrow", NULL, 0x2192},
    {"leftarrow", NULL, 0x2190},
    {"to", NULL, 0x2192},
    {"infty", NULL, 0x221E},
    {"alpha", NULL, 0x03B1},
    {"beta", NULL, 0x03B2},
    {"gamma", NULL, 0x03B3},
    {"delta", NULL, 0x03B4},
    {"epsilon", NULL, 0x03B5},
    {"varepsilon", NULL, 0x03B5},
    {"zeta", NULL, 0x03B6},
    {"eta", NULL, 0x03B7},
    {"theta", NULL, 0x03B8},
    {"iota", NULL, 0x03B9},
    {"kappa", NULL, 0x03BA},
    {"lambda", NULL, 0x03BB},
    {"mu", NULL, 0x03BC},
    {"nu", NULL, 0x03BD},
    {"xi", NULL, 0x03BE},
    {"pi", NULL, 0x03C0},
    {"rho", NULL, 0x03C1},
    {"sigma", NULL, 0x03C3},
    {"tau", NULL, 0x03C4},
    {"upsilon", NULL, 0x03C5},
    {"phi", NULL, 0x03C6},
    {"chi", NULL, 0x03C7},
    {"psi", NULL, 0x03C8},
    {"omega", NULL, 0x03C9},
    {"Gamma", NULL, 0x0393},
    {"Delta", NULL, 0x0394},
    {"Theta", NULL, 0x0398},
    {"Lambda", NULL, 0x039B},
    {"Xi", NULL, 0x039E},
    {"Pi", NULL, 0x03A0},
    {"Sigma", NULL, 0x03A3},
    {"Phi", NULL, 0x03A6},
    {"Psi", NULL, 0x03A8},
    {"Omega", NULL, 0x03A9},
    {"ell", NULL, 0x2113},
    {"hbar", NULL, 0x210F},
    {"cdotp", NULL, 0x00B7},
    {"dots", NULL, 0x2026},
    {"cdots", NULL, 0x22EF},
    {"ldots", NULL, 0x2026},
    {NULL, NULL, 0},
};

static MathNode *make_ord_cp(MathParser *mp, uint32_t cp, math_face_t face, uint16_t size)
{
    MathNode *n = alloc_node(mp, MN_ORD);
    if (!n) {
        return NULL;
    }
    n->face = face;
    n->size = size;
    if (face == MF_ITALIC && ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))) {
        cp = math_italic_cp(cp);
    } else if (face == MF_BOLD && ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))) {
        cp = math_bold_cp(cp);
    }
    utf8_encode(cp, n->text);
    return n;
}

static MathNode *make_ord_utf8(MathParser *mp, const char *utf8, math_face_t face, uint16_t size)
{
    MathNode *n = alloc_node(mp, MN_ORD);
    if (!n) {
        return NULL;
    }
    n->face = face;
    n->size = size;
    strncpy(n->text, utf8, sizeof(n->text) - 1);
    return n;
}

static bool row_push(MathNode *row, MathNode *kid)
{
    if (!row || !kid || row->nkids >= MATH_CHILD_MAX) {
        return false;
    }
    row->kids[row->nkids++] = kid;
    return true;
}

static MathNode *parse_group(MathParser *mp)
{
    skip_ws(mp);
    if (mp->p >= mp->end || *mp->p != '{') {
        return parse_atom(mp);
    }
    mp->p++;
    MathNode *inner = parse_expr(mp, '}');
    if (mp->p < mp->end && *mp->p == '}') {
        mp->p++;
    }
    return inner;
}

static MathNode *parse_command(MathParser *mp)
{
    mp->p++; /* skip \ */
    if (mp->p >= mp->end) {
        return NULL;
    }

    /* \, \; \quad \! */
    if (*mp->p == ',' || *mp->p == ';' || *mp->p == '!' || *mp->p == ' ') {
        char sp = *mp->p++;
        MathNode *n = alloc_node(mp, MN_SPACE);
        if (!n) {
            return NULL;
        }
        if (sp == ',') {
            n->space_w = 3;
        } else if (sp == ';') {
            n->space_w = 5;
        } else if (sp == '!') {
            n->space_w = -2;
        } else {
            n->space_w = 4;
        }
        return n;
    }

    char name[32];
    int ni = 0;
    if (isalpha((unsigned char)*mp->p)) {
        while (mp->p < mp->end && isalpha((unsigned char)*mp->p) && ni < (int)sizeof(name) - 1) {
            name[ni++] = *mp->p++;
        }
        name[ni] = 0;
        /* 吃掉命令后一个空格（LaTeX 惯例） */
        if (mp->p < mp->end && *mp->p == ' ') {
            mp->p++;
        }
    } else {
        name[0] = *mp->p++;
        name[1] = 0;
    }

    if (strcmp(name, "frac") == 0) {
        MathNode *n = alloc_node(mp, MN_FRAC);
        if (!n) {
            return NULL;
        }
        n->a = parse_group(mp);
        n->b = parse_group(mp);
        return n;
    }
    if (strcmp(name, "sqrt") == 0) {
        MathNode *n = alloc_node(mp, MN_SQRT);
        if (!n) {
            return NULL;
        }
        skip_ws(mp);
        if (mp->p < mp->end && *mp->p == '[') {
            mp->p++;
            n->c = parse_expr(mp, ']');
            if (mp->p < mp->end && *mp->p == ']') {
                mp->p++;
            }
        }
        n->a = parse_group(mp);
        return n;
    }
    /* \left 由 parse_atom / parse_left_right 处理，不会走到这里 */
    if (strcmp(name, "mathrm") == 0 || strcmp(name, "rm") == 0 || strcmp(name, "text") == 0 ||
        strcmp(name, "operatorname") == 0) {
        MathNode *g;
        if (strcmp(name, "rm") == 0) {
            /* \rm e → 下一 atom 直立 ASCII */
            skip_ws(mp);
            if (mp->p < mp->end && isalpha((unsigned char)*mp->p)) {
                char ch = *mp->p++;
                return make_ord_cp(mp, (uint32_t)(unsigned char)ch, MF_UPRIGHT, mp->size_text);
            }
            g = parse_atom(mp);
        } else {
            g = parse_group(mp);
        }
        if (!g) {
            return NULL;
        }
        g->face = MF_UPRIGHT;
        if (g->kind == MN_ORD) {
            /* 斜体数学字母 → 直立 ASCII */
            const char *s = g->text;
            uint32_t cp = (uint8_t)s[0];
            if ((uint8_t)s[0] >= 0xF0) {
                cp = ((uint8_t)s[0] & 7) << 18 | ((uint8_t)s[1] & 63) << 12 |
                     ((uint8_t)s[2] & 63) << 6 | ((uint8_t)s[3] & 63);
            } else if ((uint8_t)s[0] >= 0xE0) {
                cp = ((uint8_t)s[0] & 15) << 12 | ((uint8_t)s[1] & 63) << 6 | ((uint8_t)s[2] & 63);
            }
            if (cp >= 0x1D434 && cp <= 0x1D44D) {
                utf8_encode('A' + (cp - 0x1D434), g->text);
            } else if (cp == 0x210E) {
                utf8_encode('h', g->text);
            } else if (cp >= 0x1D44E && cp <= 0x1D467) {
                utf8_encode('a' + (cp - 0x1D44E), g->text);
            }
        } else if (g->kind == MN_ROW) {
            for (uint8_t i = 0; i < g->nkids; i++) {
                if (g->kids[i]) {
                    g->kids[i]->face = MF_UPRIGHT;
                }
            }
        }
        return g;
    }
    if (strcmp(name, "mathbf") == 0) {
        MathNode *g = parse_group(mp);
        if (g && g->kind == MN_ORD) {
            g->face = MF_BOLD;
        }
        return g;
    }
    if (strcmp(name, "mathit") == 0) {
        return parse_group(mp);
    }
    if (strcmp(name, "quad") == 0) {
        MathNode *n = alloc_node(mp, MN_SPACE);
        if (n) {
            n->space_w = (int16_t)(mp->size_text);
        }
        return n;
    }
    if (strcmp(name, "qquad") == 0) {
        MathNode *n = alloc_node(mp, MN_SPACE);
        if (n) {
            n->space_w = (int16_t)(mp->size_text * 2);
        }
        return n;
    }
    if (strcmp(name, "{") == 0 || strcmp(name, "}") == 0 || strcmp(name, "_") == 0 ||
        strcmp(name, "^") == 0 || strcmp(name, "%") == 0 || strcmp(name, "#") == 0 ||
        strcmp(name, "&") == 0) {
        return make_ord_utf8(mp, name, MF_UPRIGHT, mp->size_text);
    }

    for (const CmdMap *c = k_cmds; c->name; c++) {
        if (strcmp(name, c->name) == 0) {
            MathNode *n = make_ord_cp(mp, c->cp, MF_UPRIGHT, mp->display ? mp->size_text : mp->size_text);
            if (n && (strcmp(name, "sum") == 0 || strcmp(name, "prod") == 0 ||
                      strcmp(name, "int") == 0) &&
                mp->display) {
                n->size = A2UI_MATH_SIZE_DISPLAY;
            }
            return n;
        }
    }

    /* 未知命令：原样显示 */
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "\\%s", name);
    return make_ord_utf8(mp, tmp, MF_UPRIGHT, mp->size_script);
}

static MathNode *parse_left_right(MathParser *mp)
{
    /* 已看到 \left */
    skip_ws(mp);
    char ld = '.';
    if (mp->p < mp->end) {
        if (*mp->p == '\\' && mp->p + 1 < mp->end && (mp->p[1] == '{' || mp->p[1] == '}')) {
            ld = mp->p[1];
            mp->p += 2;
        } else {
            ld = *mp->p++;
        }
    }
    MathNode *body = parse_expr(mp, 0); /* 停在 \right */
    char rd = '.';
    skip_ws(mp);
    if (starts_with(mp, "\\right")) {
        mp->p += 6;
        skip_ws(mp);
        if (mp->p < mp->end) {
            if (*mp->p == '\\' && mp->p + 1 < mp->end && (mp->p[1] == '{' || mp->p[1] == '}')) {
                rd = mp->p[1];
                mp->p += 2;
            } else {
                rd = *mp->p++;
            }
        }
    }
    MathNode *n = alloc_node(mp, MN_DELIM);
    if (!n) {
        return body;
    }
    n->left_delim = ld;
    n->right_delim = rd;
    n->a = body;
    return n;
}

static MathNode *parse_atom(MathParser *mp)
{
    skip_ws(mp);
    if (mp->p >= mp->end) {
        return NULL;
    }

    if (starts_with(mp, "\\right")) {
        return NULL; /* 让上层停止 */
    }
    if (starts_with(mp, "\\left")) {
        mp->p += 5;
        return parse_left_right(mp);
    }

    if (*mp->p == '\\') {
        return parse_command(mp);
    }
    if (*mp->p == '{') {
        return parse_group(mp);
    }
    if (*mp->p == '}') {
        return NULL;
    }

    /* 普通字符：合并连续 ORD（字母数字）*/
    if ((unsigned char)*mp->p >= 0x80) {
        /* UTF-8 多字节：当一个 ORD */
        const char *start = mp->p;
        unsigned char c0 = (unsigned char)*mp->p;
        int len = 1;
        if ((c0 & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c0 & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c0 & 0xF8) == 0xF0) {
            len = 4;
        }
        if (mp->p + len > mp->end) {
            len = (int)(mp->end - mp->p);
        }
        mp->p += len;
        char tmp[8];
        if (len >= (int)sizeof(tmp)) {
            len = (int)sizeof(tmp) - 1;
        }
        memcpy(tmp, start, (size_t)len);
        tmp[len] = 0;
        return make_ord_utf8(mp, tmp, MF_UPRIGHT, mp->size_text);
    }

    char ch = *mp->p++;
    if (ch == '^' || ch == '_') {
        mp->p--; /* 脚本由 parse_scripted 处理 */
        return NULL;
    }

    math_face_t face = MF_ITALIC;
    if (isdigit((unsigned char)ch) || strchr("+-*/=<>|!(),.;:?'", ch)) {
        face = MF_UPRIGHT;
    }
    return make_ord_cp(mp, (uint32_t)(unsigned char)ch, face, mp->size_text);
}

static MathNode *parse_script_piece(MathParser *mp)
{
    skip_ws(mp);
    if (mp->p < mp->end && *mp->p == '{') {
        return parse_group(mp);
    }
    /* 单 atom */
    return parse_atom(mp);
}

static MathNode *parse_scripted(MathParser *mp)
{
    MathNode *base = parse_atom(mp);
    if (!base) {
        return NULL;
    }
    MathNode *sub = NULL;
    MathNode *sup = NULL;
    for (;;) {
        skip_ws(mp);
        if (mp->p >= mp->end) {
            break;
        }
        if (*mp->p == '_') {
            mp->p++;
            MathNode *p = parse_script_piece(mp);
            if (p) {
                p->size = mp->size_script;
            }
            sub = p;
            continue;
        }
        if (*mp->p == '^') {
            mp->p++;
            MathNode *p = parse_script_piece(mp);
            if (p) {
                p->size = mp->size_script;
            }
            sup = p;
            continue;
        }
        break;
    }
    if (!sub && !sup) {
        return base;
    }
    MathNode *n = alloc_node(mp, MN_SCRIPT);
    if (!n) {
        return base;
    }
    n->a = base;
    n->b = sub;
    n->c = sup;
    return n;
}

static MathNode *parse_expr(MathParser *mp, char stop)
{
    MathNode *row = alloc_node(mp, MN_ROW);
    if (!row) {
        return NULL;
    }
    while (mp->p < mp->end) {
        skip_ws(mp);
        if (mp->p >= mp->end) {
            break;
        }
        if (stop && *mp->p == stop) {
            break;
        }
        if (*mp->p == '}') {
            break;
        }
        if (starts_with(mp, "\\right")) {
            break;
        }
        MathNode *piece = parse_scripted(mp);
        if (!piece) {
            /* parse_atom 返回 NULL：可能是 ^/_ 孤立或 right */
            if (mp->p < mp->end && (*mp->p == '^' || *mp->p == '_')) {
                /* 无 base 的脚本：用空 base */
                MathNode *empty = alloc_node(mp, MN_ORD);
                if (empty) {
                    empty->text[0] = 0;
                }
                MathNode *sub = NULL, *sup = NULL;
                while (mp->p < mp->end && (*mp->p == '^' || *mp->p == '_')) {
                    char m = *mp->p++;
                    MathNode *p = parse_script_piece(mp);
                    if (p) {
                        p->size = mp->size_script;
                    }
                    if (m == '_') {
                        sub = p;
                    } else {
                        sup = p;
                    }
                }
                MathNode *sc = alloc_node(mp, MN_SCRIPT);
                if (sc) {
                    sc->a = empty;
                    sc->b = sub;
                    sc->c = sup;
                    piece = sc;
                }
            } else {
                break;
            }
        }
        if (piece && !row_push(row, piece)) {
            mp->ok = false;
            break;
        }
    }
    if (row->nkids == 1) {
        return row->kids[0];
    }
    return row;
}

static MathNode *parse_latex(MathParser *mp, const char *latex)
{
    char buf[MATH_LATEX_MAX];
    if (!latex) {
        latex = "";
    }
    strncpy(buf, latex, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    strip_math_delimiters(buf);
    mp->p = buf;
    mp->end = buf + strlen(buf);
    mp->ok = true;
    mp->n = 0;
    return parse_expr(mp, 0);
}

/* ---------- layout ---------- */

static void layout_node(MathNode *n);

static void layout_ord(MathNode *n)
{
    const lv_font_t *font = math_font(n->size);
    int16_t w = 0, above = 0, below = 0;
    const char *s = n->text;
    while (*s) {
        uint32_t cp = 0;
        uint8_t c0 = (uint8_t)*s;
        int len = 1;
        if (c0 < 0x80) {
            cp = c0;
        } else if ((c0 & 0xE0) == 0xC0) {
            cp = ((c0 & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
            len = 2;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = ((c0 & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
            len = 3;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = ((c0 & 0x07) << 18) | (((uint8_t)s[1] & 0x3F) << 12) |
                 (((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F);
            len = 4;
        }
        /* upright 字母：若仍是数学斜体码点且 face upright，尝试用 ASCII */
        if (n->face == MF_UPRIGHT && cp >= 0x1D400 && cp <= 0x1D7FF) {
            /* 粗体/斜体区：映射回 ASCII 再画直立（MiSans/LM 直立） */
            if (cp >= 0x1D434 && cp <= 0x1D44D) {
                cp = 'A' + (cp - 0x1D434);
            } else if (cp >= 0x1D44E && cp <= 0x1D467) {
                cp = 'a' + (cp - 0x1D44E);
                if (cp >= 'h') {
                    /* 已跳过空洞时需校正——简化：直接用 UTF8 原文若是斜体则保持 */
                }
            }
            utf8_encode(cp, n->text); /* 仅单字母 ORD 常见 */
            s = n->text;
            continue;
        }
        int16_t adv, a, b;
        glyph_metrics(font, cp, &adv, &a, &b);
        w = (int16_t)(w + adv);
        if (a > above) {
            above = a;
        }
        if (b > below) {
            below = b;
        }
        s += len;
    }
    if (above == 0 && below == 0 && font) {
        above = (int16_t)(font->line_height - font->base_line);
        below = (int16_t)font->base_line;
    }
    n->w = w;
    n->h = above;
    n->d = below;
}

static void layout_row(MathNode *n)
{
    int16_t w = 0, h = 0, d = 0;
    for (uint8_t i = 0; i < n->nkids; i++) {
        MathNode *k = n->kids[i];
        if (!k) {
            continue;
        }
        layout_node(k);
        w = (int16_t)(w + k->w);
        if (k->h > h) {
            h = k->h;
        }
        if (k->d > d) {
            d = k->d;
        }
    }
    n->w = w;
    n->h = h;
    n->d = d;
}

static void layout_frac(MathNode *n)
{
    if (n->a) {
        layout_node(n->a);
    }
    if (n->b) {
        layout_node(n->b);
    }
    int16_t nw = n->a ? n->a->w : 0;
    int16_t dw = n->b ? n->b->w : 0;
    const int16_t rule = 2;
    const int16_t gap = 5; /* 分子/分母与分数线间隙，避免压字 */
    int16_t inner = nw > dw ? nw : dw;
    n->w = (int16_t)(inner + 8);
    int16_t num_h = n->a ? (int16_t)(n->a->h + n->a->d) : 0;
    int16_t den_h = n->b ? (int16_t)(n->b->h + n->b->d) : 0;
    /* 数学轴 = 分数线中线 */
    n->h = (int16_t)(num_h + gap + rule / 2);
    n->d = (int16_t)(den_h + gap + (rule - rule / 2));
}

static void layout_sqrt(MathNode *n)
{
    if (n->a) {
        layout_node(n->a);
    }
    if (n->c) {
        n->c->size = A2UI_MATH_SIZE_SCRIPT;
        layout_node(n->c);
    }
    const lv_font_t *font = math_font(n->size);
    int16_t rad_adv = 0, rad_above = 0, rad_below = 0;
    glyph_metrics(font, 0x221A, &rad_adv, &rad_above, &rad_below);
    if (rad_adv < 12) {
        rad_adv = (int16_t)(n->size * 2 / 3);
    }
    /* √ 顶对齐 vinculum：整字高度 rad_h 从顶线往下挂，左撇勾在字形下部 */
    const int16_t over_gap = 3;
    const int16_t rad_h = (int16_t)(rad_above + rad_below);
    int16_t body_h = n->a ? n->a->h : 0;
    int16_t body_d = n->a ? n->a->d : 0;
    int16_t idx_w = n->c ? n->c->w : 0;
    n->w = (int16_t)(idx_w + rad_adv + (n->a ? n->a->w : 0) + 8);
    n->h = (int16_t)(body_h + over_gap);
    /* 顶线在 bl-body_h-over_gap；√ 底相对 bl = -body_h-over_gap+rad_h */
    int16_t rad_depth = (int16_t)(rad_h - body_h - over_gap);
    if (rad_depth < 0) {
        rad_depth = 0;
    }
    n->d = body_d > rad_depth ? body_d : rad_depth;
}

static void layout_script(MathNode *n)
{
    if (n->a) {
        layout_node(n->a);
    }
    if (n->b) {
        n->b->size = A2UI_MATH_SIZE_SCRIPT;
        layout_node(n->b);
    }
    if (n->c) {
        n->c->size = A2UI_MATH_SIZE_SCRIPT;
        layout_node(n->c);
    }
    int16_t base_w = n->a ? n->a->w : 0;
    int16_t sw = n->b ? n->b->w : 0;
    int16_t pw = n->c ? n->c->w : 0;
    int16_t script_w = sw > pw ? sw : pw;
    n->w = (int16_t)(base_w + script_w);
    n->h = n->a ? n->a->h : 0;
    n->d = n->a ? n->a->d : 0;
    if (n->c) {
        int16_t top = (int16_t)(n->h + n->c->h / 2 + 2);
        if (top > n->h) {
            n->h = top;
        }
    }
    if (n->b) {
        int16_t bot = (int16_t)(n->d + n->b->d / 2 + 2);
        if (bot > n->d) {
            n->d = bot;
        }
    }
}

static int16_t delim_fixed_size_for(int16_t content_hd, uint16_t base_size)
{
    int16_t need = content_hd;
    if (need <= (int16_t)(base_size + 4)) {
        return (int16_t)base_size;
    }
    if (need <= 36 + 8) {
        return 36;
    }
    return 36; /* 更高走拼接 */
}

static bool delim_needs_stretch(int16_t content_hd, uint16_t base_size)
{
    return content_hd > 36 + 10 && content_hd > (int16_t)(base_size * 2);
}

static void layout_delim(MathNode *n)
{
    if (n->a) {
        layout_node(n->a);
    }
    int16_t ch = n->a ? n->a->h : 0;
    int16_t cd = n->a ? n->a->d : 0;
    int16_t hd = (int16_t)(ch + cd);
    int16_t dw = 0;
    if (n->left_delim != '.') {
        if (delim_needs_stretch(hd, n->size)) {
            dw = (int16_t)(glyph_advance(math_font(n->size), 0x239B) + 2);
        } else {
            uint16_t sz = (uint16_t)delim_fixed_size_for(hd, n->size);
            char d = n->left_delim;
            uint32_t cp = (d == '{' || d == '}') ? (uint32_t)'{' : (uint32_t)(unsigned char)d;
            dw = glyph_advance(math_font(sz), cp);
        }
    }
    int16_t dw2 = 0;
    if (n->right_delim != '.') {
        if (delim_needs_stretch(hd, n->size)) {
            dw2 = (int16_t)(glyph_advance(math_font(n->size), 0x239E) + 2);
        } else {
            uint16_t sz = (uint16_t)delim_fixed_size_for(hd, n->size);
            char d = n->right_delim;
            uint32_t cp = (d == '{' || d == '}') ? (uint32_t)'}' : (uint32_t)(unsigned char)d;
            dw2 = glyph_advance(math_font(sz), cp);
        }
    }
    n->w = (int16_t)(dw + (n->a ? n->a->w : 0) + dw2);
    n->h = ch;
    n->d = cd;
    /* 定界符至少覆盖内容 */
    int16_t half = (int16_t)(hd / 2);
    if (half + 2 > n->h) {
        n->h = (int16_t)(half + 2);
    }
    if (hd - n->h > n->d) {
        n->d = (int16_t)(hd - n->h);
    }
}

static void layout_node(MathNode *n)
{
    if (!n) {
        return;
    }
    switch (n->kind) {
    case MN_ORD:
        layout_ord(n);
        break;
    case MN_ROW:
        layout_row(n);
        break;
    case MN_FRAC:
        layout_frac(n);
        break;
    case MN_SQRT:
        layout_sqrt(n);
        break;
    case MN_SCRIPT:
        layout_script(n);
        break;
    case MN_DELIM:
        layout_delim(n);
        break;
    case MN_SPACE:
        n->w = n->space_w > 0 ? n->space_w : 0;
        n->h = 0;
        n->d = 0;
        break;
    }
}

/* 第二遍：相对根内容区；baseline_y 为数学基线（分式则为分数线轴） */
static void place_node(MathNode *n, int16_t x, int16_t baseline_y);

static void place_row(MathNode *n, int16_t x, int16_t baseline_y)
{
    n->x = x;
    n->bl = baseline_y;
    n->y = (int16_t)(baseline_y - n->h);
    int16_t cx = x;
    for (uint8_t i = 0; i < n->nkids; i++) {
        MathNode *k = n->kids[i];
        if (!k) {
            continue;
        }
        place_node(k, cx, baseline_y);
        cx = (int16_t)(cx + k->w);
    }
}

static void place_node(MathNode *n, int16_t x, int16_t baseline_y)
{
    if (!n) {
        return;
    }
    n->x = x;
    n->bl = baseline_y;
    n->y = (int16_t)(baseline_y - n->h);
    switch (n->kind) {
    case MN_ROW:
        place_row(n, x, baseline_y);
        break;
    case MN_FRAC: {
        const int16_t gap = 5;
        const int16_t rule = 2;
        /* baseline_y = 分数线中线 */
        if (n->a) {
            int16_t num_bl = (int16_t)(baseline_y - gap - rule / 2 - n->a->d);
            int16_t nx = (int16_t)(x + (n->w - n->a->w) / 2);
            place_node(n->a, nx, num_bl);
        }
        if (n->b) {
            int16_t den_bl = (int16_t)(baseline_y + gap + (rule - rule / 2) + n->b->h);
            int16_t dx = (int16_t)(x + (n->w - n->b->w) / 2);
            place_node(n->b, dx, den_bl);
        }
        break;
    }
    case MN_SQRT:
        if (n->c) {
            /* 指数靠左上 */
            int16_t idx_bl = (int16_t)(baseline_y - n->h + n->c->h + 2);
            place_node(n->c, x, idx_bl);
        }
        if (n->a) {
            int16_t radical_w = glyph_advance(math_font(n->size), 0x221A);
            int16_t idx_w = n->c ? n->c->w : 0;
            place_node(n->a, (int16_t)(x + idx_w + radical_w + 2), baseline_y);
        }
        break;
    case MN_SCRIPT: {
        if (n->a) {
            place_node(n->a, x, baseline_y);
        }
        int16_t bx = (int16_t)(x + (n->a ? n->a->w : 0));
        if (n->c) {
            /* 上标：基座升部附近 */
            int16_t sbl = (int16_t)(baseline_y - (n->a ? (n->a->h * 2 / 3) : 8));
            place_node(n->c, bx, sbl);
        }
        if (n->b) {
            int16_t sbl = (int16_t)(baseline_y + (n->a ? (n->a->d * 2 / 3) : 4) + n->b->h / 2);
            place_node(n->b, bx, sbl);
        }
        break;
    }
    case MN_DELIM: {
        int16_t ch = n->a ? n->a->h : 0;
        int16_t cd = n->a ? n->a->d : 0;
        int16_t hd = (int16_t)(ch + cd);
        int16_t dw = 0;
        if (n->left_delim != '.') {
            if (delim_needs_stretch(hd, n->size)) {
                dw = (int16_t)(glyph_advance(math_font(n->size), 0x239B) + 2);
            } else {
                uint16_t sz = (uint16_t)delim_fixed_size_for(hd, n->size);
                dw = glyph_advance(math_font(sz), (uint32_t)(unsigned char)n->left_delim);
            }
        }
        if (n->a) {
            place_node(n->a, (int16_t)(x + dw), baseline_y);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------- LVGL paint ----------
 * lv_label 按字体固定 ascent（line_height-base_line）定基线，
 * 因此必须「数学基线 → label 顶」，不能把盒顶当 label 顶。
 * ∫/√/∑ 等降部/升部常超出 line_height，须加高并 OVERFLOW_VISIBLE。
 */

static void utf8_max_extents(const lv_font_t *font, const char *s, int16_t *max_above,
                             int16_t *max_below)
{
    *max_above = 0;
    *max_below = 0;
    if (!font || !s) {
        return;
    }
    while (*s) {
        uint32_t cp = 0;
        uint8_t c0 = (uint8_t)*s;
        int len = 1;
        if (c0 < 0x80) {
            cp = c0;
        } else if ((c0 & 0xE0) == 0xC0) {
            cp = ((c0 & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
            len = 2;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = ((c0 & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
            len = 3;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = ((c0 & 0x07) << 18) | (((uint8_t)s[1] & 0x3F) << 12) |
                 (((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F);
            len = 4;
        }
        int16_t adv, a, b;
        glyph_metrics(font, cp, &adv, &a, &b);
        (void)adv;
        if (a > *max_above) {
            *max_above = a;
        }
        if (b > *max_below) {
            *max_below = b;
        }
        s += len;
    }
}

static void add_label_bl(lv_obj_t *parent, const char *txt, const lv_font_t *font, int16_t x,
                         int16_t baseline_y)
{
    if (!txt || !txt[0] || !font) {
        return;
    }
    int16_t max_above = 0, max_below = 0;
    utf8_max_extents(font, txt, &max_above, &max_below);

    const int16_t asc = font_ascent(font);
    int16_t top_extra = 0;
    if (max_above > asc) {
        top_extra = (int16_t)(max_above - asc + 2);
    }

    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, font, 0);
    lv_obj_set_style_text_color(lab, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(lab, 0, 0);
    if (top_extra > 0) {
        /* 垫高：LVGL 基线 = label_y + pad_top + ascent = 数学基线 */
        lv_obj_set_style_pad_top(lab, top_extra, 0);
    }
    lv_obj_add_flag(lab, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* pad_top + ascent + 真实降部；覆盖 ∫ 上下两端 */
    int16_t need_h = (int16_t)(top_extra + asc + max_below + 8);
    if (need_h < (int16_t)font->line_height + top_extra + 8) {
        need_h = (int16_t)(font->line_height + top_extra + 8);
    }
    lv_obj_set_height(lab, need_h);
    lv_obj_set_pos(lab, x, (int32_t)baseline_y - asc - top_extra);
}

/** 将字形顶边对齐到 desired_top（√ 与顶线衔接；零件定界符）。 */
static void add_label_glyph_top(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                                uint32_t cp, int16_t x, int16_t desired_top)
{
    if (!txt || !txt[0] || !font) {
        return;
    }
    int16_t bm_top = glyph_bitmap_top(font, cp);
    /* glyph_top = label_y + ascent - bm_top  ⇒  label_y = desired_top - ascent + bm_top */
    int16_t label_y = (int16_t)(desired_top - font_ascent(font) + bm_top);
    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, font, 0);
    lv_obj_set_style_text_color(lab, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(lab, 0, 0);
    lv_obj_add_flag(lab, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    /* 显式加高，避免 label 默认行高裁掉 √ 左撇勾 */
    int16_t adv = 0, above = 0, below = 0;
    glyph_metrics(font, cp, &adv, &above, &below);
    int16_t need_h = (int16_t)(font_ascent(font) + below + 4);
    if (need_h < font->line_height) {
        need_h = (int16_t)font->line_height;
    }
    lv_obj_set_height(lab, need_h);
    lv_obj_set_pos(lab, x, label_y);
    (void)adv;
    (void)above;
}

static void add_rule(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_pos(r, x, y);
}

static void paint_stretch_delim(lv_obj_t *parent, char delim, bool left, int16_t x, int16_t top,
                                int16_t total_h, uint16_t size)
{
    const lv_font_t *font = math_font(size);
    uint32_t top_cp, mid_cp, bot_cp;
    if (delim == '(' || delim == ')') {
        if (left) {
            top_cp = 0x239B;
            mid_cp = 0x239C;
            bot_cp = 0x239D;
        } else {
            top_cp = 0x239E;
            mid_cp = 0x239F;
            bot_cp = 0x23A0;
        }
    } else if (delim == '[' || delim == ']') {
        if (left) {
            top_cp = 0x23A1;
            mid_cp = 0x23A2;
            bot_cp = 0x23A3;
        } else {
            top_cp = 0x23A4;
            mid_cp = 0x23A5;
            bot_cp = 0x23A6;
        }
    } else if (delim == '{' || delim == '}') {
        if (left) {
            top_cp = 0x23A7;
            mid_cp = 0x23A8;
            bot_cp = 0x23A9;
        } else {
            top_cp = 0x23AB;
            mid_cp = 0x23AC;
            bot_cp = 0x23AD;
        }
    } else {
        char t[8];
        utf8_encode((uint32_t)(unsigned char)delim, t);
        add_label_glyph_top(parent, t, font, (uint32_t)(unsigned char)delim, x, top);
        return;
    }

    char t1[8], tm[8], t2[8], te[8];
    utf8_encode(top_cp, t1);
    utf8_encode(mid_cp, tm);
    utf8_encode(bot_cp, t2);
    utf8_encode(0x23AA, te);

    int16_t piece_h = (int16_t)(font ? font->line_height : size);
    if (piece_h < 8) {
        piece_h = (int16_t)size;
    }
    add_label_glyph_top(parent, t1, font, top_cp, x, top);
    int16_t y = (int16_t)(top + piece_h - 6);
    int16_t bot_y = (int16_t)(top + total_h - piece_h);
    if (bot_y < top) {
        bot_y = top;
    }
    if (delim == '{' || delim == '}') {
        int16_t mid_y = (int16_t)(top + total_h / 2 - piece_h / 2);
        while (y + piece_h < mid_y) {
            add_label_glyph_top(parent, te, font, 0x23AA, x, y);
            y = (int16_t)(y + piece_h - 8);
        }
        add_label_glyph_top(parent, tm, font, mid_cp, x, mid_y);
        y = (int16_t)(mid_y + piece_h - 6);
        while (y + piece_h < bot_y) {
            add_label_glyph_top(parent, te, font, 0x23AA, x, y);
            y = (int16_t)(y + piece_h - 8);
        }
    } else {
        while (y + piece_h < bot_y) {
            add_label_glyph_top(parent, tm, font, mid_cp, x, y);
            y = (int16_t)(y + piece_h - 8);
        }
    }
    add_label_glyph_top(parent, t2, font, bot_cp, x, bot_y);
}

static void paint_node(lv_obj_t *parent, MathNode *n);

static void paint_node(lv_obj_t *parent, MathNode *n)
{
    if (!n) {
        return;
    }
    switch (n->kind) {
    case MN_ORD:
        add_label_bl(parent, n->text, math_font(n->size), n->x, n->bl);
        break;
    case MN_ROW:
        for (uint8_t i = 0; i < n->nkids; i++) {
            paint_node(parent, n->kids[i]);
        }
        break;
    case MN_FRAC: {
        paint_node(parent, n->a);
        paint_node(parent, n->b);
        /* 分数线画在数学轴（n->bl），厚度 2，顶边 = bl-1 */
        add_rule(parent, (int16_t)(n->x + 2), (int16_t)(n->bl - 1), (int16_t)(n->w - 4), 2);
        break;
    }
    case MN_SQRT: {
        if (n->c) {
            paint_node(parent, n->c);
        }
        paint_node(parent, n->a);
        const lv_font_t *font = math_font(n->size);
        int16_t idx_w = n->c ? n->c->w : 0;
        int16_t rad_adv = 0, rad_a = 0, rad_b = 0;
        glyph_metrics(font, 0x221A, &rad_adv, &rad_a, &rad_b);
        if (rad_adv < 12) {
            rad_adv = (int16_t)(n->size * 2 / 3);
        }
        const int16_t over_gap = 3;
        int16_t body_h = n->a ? n->a->h : 0;
        int16_t body_bl = n->a ? n->a->bl : n->bl;
        /* 顶线与 √ 顶对齐，左撇勾在字形下方完整画出 */
        int16_t over_y = (int16_t)(body_bl - body_h - over_gap);
        if (over_y < n->y) {
            over_y = n->y;
        }
        char rad[8];
        utf8_encode(0x221A, rad);
        add_label_glyph_top(parent, rad, font, 0x221A, (int16_t)(n->x + idx_w), over_y);
        if (n->a) {
            /* 顶线从 √ 斜勾右缘接到内容右端 */
            int16_t over_x = (int16_t)(n->x + idx_w + rad_adv - 2);
            if (over_x < n->x + idx_w + 8) {
                over_x = (int16_t)(n->x + idx_w + 8);
            }
            add_rule(parent, over_x, over_y, (int16_t)(n->a->w + 6), 2);
        }
        (void)rad_a;
        (void)rad_b;
        break;
    }
    case MN_SCRIPT:
        paint_node(parent, n->a);
        paint_node(parent, n->b);
        paint_node(parent, n->c);
        break;
    case MN_DELIM: {
        int16_t ch = n->a ? n->a->h : 0;
        int16_t cd = n->a ? n->a->d : 0;
        int16_t hd = (int16_t)(ch + cd);
        int16_t top = n->a ? (int16_t)(n->a->bl - n->a->h) : n->y;
        int16_t cx = n->x;
        if (n->left_delim != '.') {
            if (delim_needs_stretch(hd, n->size)) {
                paint_stretch_delim(parent, n->left_delim, true, cx, top, hd, n->size);
                cx = (int16_t)(cx + glyph_advance(math_font(n->size), 0x239B) + 2);
            } else {
                uint16_t sz = (uint16_t)delim_fixed_size_for(hd, n->size);
                char t[8];
                uint32_t cp = (uint32_t)(unsigned char)n->left_delim;
                utf8_encode(cp, t);
                const lv_font_t *f = math_font(sz);
                add_label_bl(parent, t, f, cx, n->bl);
                cx = (int16_t)(cx + glyph_advance(f, cp));
            }
        }
        paint_node(parent, n->a);
        if (n->right_delim != '.') {
            int16_t rx = (int16_t)(n->x + n->w);
            if (delim_needs_stretch(hd, n->size)) {
                int16_t dw = (int16_t)(glyph_advance(math_font(n->size), 0x239E) + 2);
                paint_stretch_delim(parent, n->right_delim, false, (int16_t)(rx - dw), top, hd,
                                    n->size);
            } else {
                uint16_t sz = (uint16_t)delim_fixed_size_for(hd, n->size);
                char t[8];
                uint32_t cp = (uint32_t)(unsigned char)n->right_delim;
                utf8_encode(cp, t);
                const lv_font_t *f = math_font(sz);
                int16_t adv = glyph_advance(f, cp);
                add_label_bl(parent, t, f, (int16_t)(rx - adv), n->bl);
            }
        }
        (void)cx;
        break;
    }
    case MN_SPACE:
        break;
    }
}

static MathNode *build_tree(MathParser *mp, const char *latex, bool display)
{
    memset(mp, 0, sizeof(*mp));
    mp->display = display;
    mp->size_text = A2UI_MATH_SIZE_TEXT;
    (void)display;
    mp->size_script = A2UI_MATH_SIZE_SCRIPT;
    fontpack_lv_ensure_ready();
    MathNode *root = parse_latex(mp, latex);
    if (!root || !mp->ok) {
        return NULL;
    }
    layout_node(root);
    place_node(root, 0, root->h);
    return root;
}

static lv_obj_t *create_from_tree(lv_obj_t *parent, MathNode *root, int32_t max_width)
{
    (void)max_width;
    int16_t total_h = (int16_t)(root->h + root->d);
    if (total_h < 8) {
        total_h = 8;
    }
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, root->w + 4, total_h + 4);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    /* √ 等降部超出行盒时仍可见 */
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    paint_node(box, root);
    return box;
}

lv_obj_t *a2ui_math_create(lv_obj_t *parent, const char *latex, bool display_style,
                           int32_t max_width)
{
    if (!parent) {
        return NULL;
    }
    MathParser *mp = heap_caps_malloc(sizeof(MathParser), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp) {
        mp = malloc(sizeof(MathParser));
    }
    if (!mp) {
        ESP_LOGE(TAG, "oom parser");
        return NULL;
    }
    MathNode *root = build_tree(mp, latex, display_style);
    if (!root) {
        ESP_LOGW(TAG, "parse fail: %.40s", latex ? latex : "");
        free(mp);
        lv_obj_t *fb = lv_label_create(parent);
        lv_label_set_text(fb, latex ? latex : "?");
        lv_obj_set_style_text_font(fb, math_font(A2UI_MATH_SIZE_SCRIPT), 0);
        return fb;
    }
    lv_obj_t *box = create_from_tree(parent, root, max_width);
    free(mp);
    return box;
}

int32_t a2ui_math_measure_height(const char *latex, bool display_style, int32_t max_width)
{
    (void)max_width;
    MathParser *mp = heap_caps_malloc(sizeof(MathParser), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp) {
        mp = malloc(sizeof(MathParser));
    }
    if (!mp) {
        return -1;
    }
    MathNode *root = build_tree(mp, latex, display_style);
    int32_t h = -1;
    if (root) {
        h = (int32_t)root->h + root->d + 8;
    }
    free(mp);
    return h;
}
