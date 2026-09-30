/*******************************************************************
 *
 * main.c - LVGL simulator for GNU/Linux
 *
 * Based on the original file from the repository
 *
 * @note eventually this file won't contain a main function and will
 * become a library supporting all major operating systems
 *
 * To see how each driver is initialized check the
 * 'src/lib/display_backends' directory
 *
 * - Clean up
 * - Support for multiple backends at once
 *   2025 EDGEMTech Ltd.
 *
 * Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 ******************************************************************/
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"

#include "src/lib/driver_backends.h"
#include "src/lib/simulator_util.h"
#include "src/lib/simulator_settings.h"

/* Internal functions */
static void configure_simulator(int argc, char ** argv);
static void print_lvgl_version(void);
static void print_usage(void);
static void music_player_create(void);

void music_player_set_song(const char * name);
void music_player_push_lyric(const char * text);

LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk)

#define SCREEN_SIZE        360
#define LYRIC_SLOT_COUNT   4
#define LYRIC_ANIM_MS      750
#define VINYL_SIZE         220
#define VINYL_ROT_PERIOD   8000

typedef struct {
    int32_t  y;
    lv_opa_t opa;
} slot_pose_t;

/* Pose by visual rank: 0 = fading off the top, N-1 = current (newest).
 * Travel is small — feel comes mostly from opacity, not motion. */
static const slot_pose_t LYRIC_RANK_POSE[LYRIC_SLOT_COUNT] = {
    { -54, LV_OPA_TRANSP },
    { -22, 70            },
    {  10, 160           },
    {  42, LV_OPA_COVER  },
};
#define LYRIC_INCOMING_Y   62

static lv_obj_t * song_title_label;
static lv_obj_t * vinyl_disc;
static lv_obj_t * lyric_slots[LYRIC_SLOT_COUNT];
static int        lyric_head;

/* contains the name of the selected backend if user
 * has specified one on the command line */
static char * selected_backend;

/* Global simulator settings, defined in lv_linux_backend.c */
extern simulator_settings_t settings;

/**
 * @brief Print LVGL version
 */
static void print_lvgl_version(void)
{
    fprintf(stdout, "%d.%d.%d-%s\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LVGL_VERSION_INFO);
}

/**
 * @brief Print usage information
 */
static void print_usage(void)
{
    fprintf(stdout, "\nlvglsim [-V] [-B] [-b backend_name] [-W window_width] [-H window_height]\n\n");
    fprintf(stdout, "-V print LVGL version\n");
    fprintf(stdout, "-B list supported backends\n");
}

/**
 * @brief Configure simulator
 * @description process arguments recieved by the program to select
 * appropriate options
 * @param argc the count of arguments in argv
 * @param argv The arguments
 */
static void configure_simulator(int argc, char ** argv)
{
    int opt = 0;

    selected_backend = NULL;
    driver_backends_register();

    const char * env_w = getenv("LV_SIM_WINDOW_WIDTH");
    const char * env_h = getenv("LV_SIM_WINDOW_HEIGHT");
    /* Default values */
    // settings.window_width = atoi(env_w ? env_w : "800");
    // settings.window_height = atoi(env_h ? env_h : "480");
    settings.window_width  = 360;
    settings.window_height = 360;

    /* Parse the command-line options. */
    while((opt = getopt(argc, argv, "b:fmW:H:BVh")) != -1) {
        switch(opt) {
            case 'h':
                print_usage();
                exit(EXIT_SUCCESS);
                break;
            case 'V':
                print_lvgl_version();
                exit(EXIT_SUCCESS);
                break;
            case 'B':
                driver_backends_print_supported();
                exit(EXIT_SUCCESS);
                break;
            case 'b':
                if(driver_backends_is_supported(optarg) == 0) {
                    die("error no such backend: %s\n", optarg);
                }
                selected_backend = strdup(optarg);
                break;
            case 'W': settings.window_width = atoi(optarg); break;
            case 'H': settings.window_height = atoi(optarg); break;
            case ':':
                print_usage();
                die("Option -%c requires an argument.\n", optopt);
                break;
            case '?': print_usage(); die("Unknown option -%c.\n", optopt);
        }
    }
}

/**
 * @brief entry point
 * @description start a demo
 * @param argc the count of arguments in argv
 * @param argv The arguments
 */
int main(int argc, char ** argv)
{

    configure_simulator(argc, argv);

    /* Initialize LVGL. */
    lv_init();

    /* Initialize the configured backend */
    if(driver_backends_init_backend(selected_backend) == -1) {
        die("Failed to initialize display backend");
    }

    /* Enable for EVDEV support */
#if LV_USE_EVDEV
    if(driver_backends_init_backend("EVDEV") == -1) {
        die("Failed to initialize evdev");
    }
#endif

    /*Create a Demo*/
    music_player_create();

    /* Enter the run loop of the selected backend */
    driver_backends_run_loop();

    return 0;
}

static void anim_y_cb(void * obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_opa_cb(void * obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_rot_cb(void * obj, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)obj, v, 0);
}

static void slot_animate_to(lv_obj_t * label, const slot_pose_t * pose, bool instant)
{
    if(instant) {
        lv_anim_delete(label, anim_y_cb);
        lv_anim_delete(label, anim_opa_cb);
        lv_obj_set_y(label, pose->y);
        lv_obj_set_style_text_opa(label, pose->opa, 0);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_duration(&a, LYRIC_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(label), pose->y);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, lv_obj_get_style_text_opa(label, 0), pose->opa);
    lv_anim_start(&a);
}

void music_player_set_song(const char * name)
{
    if(song_title_label) lv_label_set_text(song_title_label, name);
}

void music_player_push_lyric(const char * text)
{
    if(text == NULL) return;

    int new_head = (lyric_head + 1) % LYRIC_SLOT_COUNT;
    lv_obj_t * incoming = lyric_slots[new_head];

    slot_pose_t hidden = { LYRIC_INCOMING_Y, LV_OPA_TRANSP };
    slot_animate_to(incoming, &hidden, true);
    lv_label_set_text(incoming, text);

    lyric_head = new_head;

    for(int s = 0; s < LYRIC_SLOT_COUNT; s++) {
        int rank = (s - (lyric_head + 1) + LYRIC_SLOT_COUNT) % LYRIC_SLOT_COUNT;
        slot_animate_to(lyric_slots[s], &LYRIC_RANK_POSE[rank], false);
    }
}

static const char * DEMO_LYRICS[] = {
    "故事的小黄花",
    "从出生那年就飘着",
    "童年的荡秋千",
    "随记忆一直晃到现在",
    "吹着前奏望着天空",
    "我想起花瓣试着掉落",
    "为你翘课的那一天",
    "花落的那一天",
    "教室的那一间",
    "消失的下雨天",
    "我好想再淋一遍",
    "没想到失去的勇气我还留着",
    "好想再问一遍",
    "你会等待还是离开",
};

static void demo_lyric_timer_cb(lv_timer_t * t)
{
    static uint32_t i = 0;
    music_player_push_lyric(DEMO_LYRICS[i % (sizeof(DEMO_LYRICS) / sizeof(DEMO_LYRICS[0]))]);
    i++;
    LV_UNUSED(t);
}

static lv_obj_t * make_lyric_label(lv_obj_t * parent)
{
    lv_obj_t * lab = lv_label_create(parent);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lab, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lab, SCREEN_SIZE - 100);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_align(lab, LV_ALIGN_TOP_MID);
    lv_obj_set_x(lab, 0);
    lv_label_set_text(lab, "");
    lv_obj_set_style_text_opa(lab, LV_OPA_TRANSP, 0);
    lv_obj_set_y(lab, LYRIC_INCOMING_Y);
    return lab;
}

/* Vinyl record: three concentric circles — body, silver ring, center label —
 * drawn as children so the whole disc can be rotated via transform_rotation. */
static lv_obj_t * make_vinyl_disc(lv_obj_t * parent)
{
    lv_obj_t * disc = lv_obj_create(parent);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, VINYL_SIZE, VINYL_SIZE);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(0x0B0B0D), 0);
    lv_obj_set_style_bg_grad_color(disc, lv_color_hex(0x1E1E22), 0);
    lv_obj_set_style_bg_grad_dir(disc, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(0x2A2A2E), 0);
    lv_obj_set_style_border_width(disc, 1, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);

    /* Rotation pivot is the disc center. LVGL uses 0.1 degree units. */
    lv_obj_set_style_transform_pivot_x(disc, VINYL_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(disc, VINYL_SIZE / 2, 0);

    /* Grooves: a few darker rings drawn as nested borders via child objects. */
    const int groove_insets[] = { 20, 44, 68 };
    for(size_t i = 0; i < sizeof(groove_insets) / sizeof(groove_insets[0]); i++) {
        lv_obj_t * g = lv_obj_create(disc);
        lv_obj_remove_style_all(g);
        int sz = VINYL_SIZE - groove_insets[i] * 2;
        lv_obj_set_size(g, sz, sz);
        lv_obj_center(g);
        lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(g, lv_color_hex(0x17171A), 0);
        lv_obj_set_style_border_width(g, 1, 0);
        lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Center label (the paper sticker on a real record). */
    lv_obj_t * center = lv_obj_create(disc);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, 74, 74);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0xB8352E), 0);
    lv_obj_set_style_bg_grad_color(center, lv_color_hex(0x7A1F1A), 0);
    lv_obj_set_style_bg_grad_dir(center, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    /* Spindle hole in the middle of the sticker. */
    lv_obj_t * hole = lv_obj_create(center);
    lv_obj_remove_style_all(hole);
    lv_obj_set_size(hole, 10, 10);
    lv_obj_center(hole);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hole, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_remove_flag(hole, LV_OBJ_FLAG_SCROLLABLE);

    return disc;
}

static void vinyl_start_spinning(lv_obj_t * disc)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, disc);
    lv_anim_set_exec_cb(&a, anim_rot_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_duration(&a, VINYL_ROT_PERIOD);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

#if LV_USE_SNAPSHOT
static void bmp_write_le16(FILE * f, unsigned v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
}

static void bmp_write_le32(FILE * f, unsigned v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f);
    fputc((int)((v >> 24) & 0xff), f);
}

static bool save_xrgb_draw_buf_as_bmp(const lv_draw_buf_t * db, const char * path)
{
    uint32_t w = db->header.w;
    uint32_t h = db->header.h;
    uint32_t src_stride = db->header.stride;
    if(w == 0 || h == 0) return false;

    uint32_t row_bytes = w * 3;
    uint32_t pad = (4 - (row_bytes % 4)) % 4;
    uint32_t dst_row = row_bytes + pad;
    uint32_t image_size = dst_row * h;
    uint32_t file_size = 54 + image_size;

    FILE * f = fopen(path, "wb");
    if(f == NULL) return false;

    fputc('B', f);
    fputc('M', f);
    bmp_write_le32(f, file_size);
    bmp_write_le16(f, 0);
    bmp_write_le16(f, 0);
    bmp_write_le32(f, 54);
    bmp_write_le32(f, 40);
    bmp_write_le32(f, w);
    bmp_write_le32(f, h);
    bmp_write_le16(f, 1);
    bmp_write_le16(f, 24);
    bmp_write_le32(f, 0);
    bmp_write_le32(f, image_size);
    bmp_write_le32(f, 0);
    bmp_write_le32(f, 0);
    bmp_write_le32(f, 0);
    bmp_write_le32(f, 0);

    static const uint8_t zero_pad[4] = { 0, 0, 0, 0 };
    for(int row = (int)h - 1; row >= 0; row--) {
        const uint8_t * src = db->data + (uint32_t)row * src_stride;
        for(uint32_t x = 0; x < w; x++) {
            uint32_t p;
            memcpy(&p, src + x * 4, sizeof(p));
            uint8_t bgr[3] = { (uint8_t)(p & 0xff), (uint8_t)((p >> 8) & 0xff), (uint8_t)((p >> 16) & 0xff) };
            if(fwrite(bgr, 1, 3, f) != 3) {
                fclose(f);
                return false;
            }
        }
        if(pad && fwrite(zero_pad, 1, pad, f) != pad) {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

static void export_vinyl_disc_snapshot_cb(lv_timer_t * t)
{
    const char * path = (const char *)lv_timer_get_user_data(t);
    if(vinyl_disc == NULL || path == NULL) {
        lv_timer_delete(t);
        return;
    }

    lv_draw_buf_t * shot = lv_snapshot_take(vinyl_disc, LV_COLOR_FORMAT_XRGB8888);
    if(shot) {
        if(save_xrgb_draw_buf_as_bmp(shot, path)) {
            fprintf(stderr, "vinyl_disc snapshot saved to %s\n", path);
        }
        else {
            fprintf(stderr, "failed to write vinyl_disc snapshot to %s\n", path);
        }
        lv_draw_buf_destroy(shot);
    }
    else {
        fprintf(stderr, "lv_snapshot_take(vinyl_disc) failed\n");
    }
    lv_timer_delete(t);
}
#endif /* LV_USE_SNAPSHOT */

static void music_player_create(void)
{
    lv_obj_t * scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0D), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Spinning vinyl fills most of the circular screen. */
    vinyl_disc = make_vinyl_disc(scr);
    lv_obj_center(vinyl_disc);
    vinyl_start_spinning(vinyl_disc);

    /* A soft vignette ring to blend the disc into the round screen edge. */
    lv_obj_t * vignette = lv_obj_create(scr);
    lv_obj_remove_style_all(vignette);
    lv_obj_set_size(vignette, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(vignette);
    lv_obj_set_style_radius(vignette, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(vignette, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(vignette, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(vignette, 14, 0);
    lv_obj_set_style_border_opa(vignette, LV_OPA_70, 0);
    lv_obj_remove_flag(vignette, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Song title sits above the disc. */
    song_title_label = lv_label_create(scr);
    lv_label_set_text(song_title_label, "晴天 · 周杰伦");
    lv_obj_set_style_text_color(song_title_label, lv_color_hex(0xF2F2F2), 0);
    lv_obj_set_style_text_font(song_title_label, &lv_font_source_han_sans_sc_16_cjk, 0);
    /* ~25% larger than base 16px CJK (only 14/16 baked in tree); scale from center. */
    lv_obj_set_style_transform_pivot_x(song_title_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(song_title_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale(song_title_label, 320, 0);
    lv_obj_set_style_text_align(song_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(song_title_label, SCREEN_SIZE - 100);
    lv_label_set_long_mode(song_title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_align(song_title_label, LV_ALIGN_TOP_MID, 0, 40);

    /* Lyric stage floats over the disc center. Transparent bg so grooves show through. */
    lv_obj_t * stage = lv_obj_create(scr);
    lv_obj_remove_style_all(stage);
    lv_obj_set_size(stage, SCREEN_SIZE - 80, 130);
    lv_obj_align(stage, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_clip_corner(stage, true, 0);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for(int i = 0; i < LYRIC_SLOT_COUNT; i++) {
        lyric_slots[i] = make_lyric_label(stage);
    }
    lyric_head = LYRIC_SLOT_COUNT - 1;

    lv_timer_create(demo_lyric_timer_cb, 2600, NULL);

#if LV_USE_SNAPSHOT
    {
        const char * snap_path = getenv("LV_VINYL_SNAPSHOT");
        if(snap_path == NULL || snap_path[0] == '\0') snap_path = "vinyl_disc.bmp";
        lv_timer_create(export_vinyl_disc_snapshot_cb, 150, (void *)snap_path);
    }
#endif
}
