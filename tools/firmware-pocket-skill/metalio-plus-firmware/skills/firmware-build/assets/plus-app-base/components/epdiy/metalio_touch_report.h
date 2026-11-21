#pragma once

/* Pure report state: no coordinates, clock, RTOS or controller writes. */
#include <stdbool.h>
#include <stdint.h>

enum {
    METALIO_FT_DOWN = 0,
    METALIO_FT_UP = 1,
    METALIO_FT_CONTACT = 2,
    METALIO_FT_RESERVED = 3,
};

typedef struct {
    bool down;
    bool waiting_release;
    bool ambiguous;
    bool saw_contact;
    uint8_t track_id;
    uint8_t last_event;
} metalio_touch_report_state_t;

typedef struct {
    bool release;
    bool confirmed_release;
    bool pressed;
    bool recovered_edge;
    bool duplicate_down;
    bool id_changed;
    bool suppressed;
    bool invalid;
} metalio_touch_report_result_t;

static inline void metalio_touch_report_reset(metalio_touch_report_state_t* state,
                                               bool require_release) {
    const metalio_touch_report_state_t clear = {
        .waiting_release = require_release,
        .last_event = METALIO_FT_RESERVED,
    };
    *state = clear;
}

static inline metalio_touch_report_result_t metalio_touch_report_sample(
    metalio_touch_report_state_t* state, uint8_t count, uint8_t event, uint8_t track_id) {
    metalio_touch_report_result_t out = {0};
    if (count == 0) {
        /* Also deliver idle reports: a scene fence may be waiting to re-arm. */
        out.release = true;
        out.confirmed_release = true;
        metalio_touch_report_reset(state, false);
        return out;
    }
    if (count > 1 || event > METALIO_FT_CONTACT || track_id >= 0x0f) {
        /* First-slot changes cannot identify a new single-finger click.
         * Keep the old gesture until TD_STATUS really reaches zero. */
        state->ambiguous = true;
        state->saw_contact = false;
        out.suppressed = true;
        out.invalid = count > 2 || event > METALIO_FT_CONTACT || track_id >= 0x0f;
        return out;
    }
    if (state->waiting_release || state->ambiguous) {
        out.suppressed = true;
        return out;
    }
    if (state->down && track_id != state->track_id) {
        /* An ID change alone is not evidence of an up/new-down pair. */
        state->ambiguous = true;
        state->saw_contact = false;
        out.id_changed = true;
        out.suppressed = true;
        return out;
    }
    if (event == METALIO_FT_UP) {
        if (state->down) {
            /* The ID matches and this gesture never became multi-touch. */
            out.release = true;
            out.confirmed_release = true;
            metalio_touch_report_reset(state, false);
        } else {
            out.suppressed = true;
        }
        return out;
    }
    if (event == METALIO_FT_DOWN && state->down) {
        if (state->saw_contact && state->last_event == METALIO_FT_CONTACT) {
            /* A new controller DOWN after CONTACT can expose a missed UP.
             * This synthetic boundary must never re-arm a scene/Flash fence. */
            out.release = true;
            out.recovered_edge = true;
            state->saw_contact = false;
        } else {
            out.duplicate_down = true;
        }
    }
    state->down = true;
    state->track_id = track_id;
    state->last_event = event;
    if (event == METALIO_FT_CONTACT) state->saw_contact = true;
    out.pressed = true;
    return out;
}
