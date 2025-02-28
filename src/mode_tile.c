#include "config.h"
#include "label.h"
#include "log.h"
#include "mode.h"
#include "state.h"
#include "utils.h"
#include "utils_cairo.h"

#include <cairo.h>
#include <stdbool.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define MIN_SUB_AREA_SIZE (25 * 50)

void tile_mode_enter(struct state *state) {
    state->mode = &tile_mode_interface;

    if (state->initial_area.w == -1) {
        state->initial_area.x = 0;
        state->initial_area.y = 0;
        state->initial_area.w = state->surface_width;
        state->initial_area.h = state->surface_height;
    } else {
        if (state->initial_area.x < 0) {
            state->initial_area.w += state->initial_area.x;
            state->initial_area.x  = 0;
        }

        if (state->initial_area.y < 0) {
            state->initial_area.h += state->initial_area.y;
            state->initial_area.y  = 0;
        }

        if (state->initial_area.w + state->initial_area.x >
            state->current_output->width) {
            state->initial_area.w =
                state->current_output->width - state->initial_area.x;
        }

        if (state->initial_area.h + state->initial_area.y >
            state->current_output->height) {
            state->initial_area.h =
                state->current_output->height - state->initial_area.y;
        }
    }

    if (state->initial_area.w <= 0 || state->initial_area.h <= 0) {
        state->running = false;
        LOG_ERR(
            "Initial area (%dx%d) is too small.", state->initial_area.w,
            state->initial_area.h
        );
        return;
    }

    const int max_num_sub_areas = 26 * 26;
    const int area_size         = state->initial_area.w * state->initial_area.h;
    const int sub_area_size =
        max(area_size / max_num_sub_areas, MIN_SUB_AREA_SIZE);

    struct tile_mode_state *ms = &state->mode_state.tile;

    ms->sub_area_height = sqrt(sub_area_size / 2.);
    ms->sub_area_rows   = state->initial_area.h / ms->sub_area_height;
    if (ms->sub_area_rows == 0) {
        ms->sub_area_rows = 1;
    }
    ms->sub_area_height_off = state->initial_area.h % ms->sub_area_rows;
    ms->sub_area_height     = state->initial_area.h / ms->sub_area_rows;

    ms->sub_area_width   = sqrt(sub_area_size * 2);
    ms->sub_area_columns = state->initial_area.w / ms->sub_area_width;
    if (ms->sub_area_columns == 0) {
        ms->sub_area_columns = 1;
    }
    ms->sub_area_width_off = state->initial_area.w % ms->sub_area_columns;
    ms->sub_area_width     = state->initial_area.w / ms->sub_area_columns;

    ms->label_symbols =
        label_symbols_from_str(state->config.mode_tile.label_symbols);
    if (ms->label_symbols == NULL) {
        ms->label_selection = NULL;
        state->running      = false;
        return;
    }

    ms->label_selection = label_selection_new(
        ms->label_symbols, ms->sub_area_rows * ms->sub_area_columns
    );
}

// `tile_mode_back` goes back in history. Returns true if there was something to
// go back to.
static bool tile_mode_back(struct tile_mode_state *mode_state) {
    return label_selection_back(mode_state->label_selection) != 0;
}

// `tile_mode_reenter` reenters the tile mode. We assume that the saved state is
// valid and goes back in history once.
void tile_mode_reenter(struct state *state) {
    state->mode = &tile_mode_interface;
    tile_mode_back(&state->mode_state.tile);
}

static struct rect
idx_to_rect(struct tile_mode_state *mode_state, int idx, int x_off, int y_off) {
    int column = idx / mode_state->sub_area_rows;
    int row    = idx % mode_state->sub_area_rows;

    return (struct rect){
        .x = column * mode_state->sub_area_width +
             min(column, mode_state->sub_area_width_off) + x_off,
        .w = mode_state->sub_area_width +
             (column < mode_state->sub_area_width_off ? 1 : 0),
        .y = row * mode_state->sub_area_height +
             min(row, mode_state->sub_area_height_off) + y_off,
        .h = mode_state->sub_area_height +
             (row < mode_state->sub_area_height_off ? 1 : 0),
    };
}

static bool
tile_mode_key(struct state *state, xkb_keysym_t keysym, char *text) {
    struct tile_mode_state *ms = &state->mode_state.tile;

    switch (keysym) {
    case XKB_KEY_BackSpace:
        return tile_mode_back(ms);
        break;

    case XKB_KEY_Escape:
        state->running = false;
        break;
    default:;
        int symbol_idx = label_symbols_find_idx(ms->label_symbols, text);
        if (symbol_idx < 0) {
            return false;
        }

        label_selection_append(ms->label_selection, symbol_idx);

        int idx = label_selection_to_idx(ms->label_selection);
        if (idx > 0) {
            bisect_mode_enter(
                state, idx_to_rect(
                           ms, idx, state->initial_area.x, state->initial_area.y
                       )
            );
        }
        return true;
    }

    return false;
}

void tile_mode_render(struct state *state, cairo_t *cairo) {
    struct mode_tile_config *config = &state->config.mode_tile;
    struct tile_mode_state  *ms     = &state->mode_state.tile;

    cairo_select_font_face(
        cairo, config->label_font_family, CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_NORMAL
    );
    cairo_set_font_size(cairo, (int)(ms->sub_area_height / 2));

    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_u32(cairo, config->unselectable_bg_color);
    cairo_paint(cairo);

    cairo_translate(cairo, state->initial_area.x, state->initial_area.y);

    cairo_set_source_u32(cairo, config->unselectable_bg_color);
    cairo_rectangle(
        cairo, .5, .5, state->initial_area.w - 1, state->initial_area.h - 1
    );
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);

    label_selection_t *curr_label = label_selection_new(
        ms->label_symbols, ms->sub_area_columns * ms->sub_area_rows
    );
    label_selection_set_from_idx(curr_label, 0);

    int  label_str_max_len = label_selection_str_max_len(curr_label) + 1;
    char label_selected_str[label_str_max_len];
    char label_unselected_str[label_str_max_len];

    for (int i = 0; i < ms->sub_area_columns; i++) {
        for (int j = 0; j < ms->sub_area_rows; j++) {
            const int x =
                i * ms->sub_area_width + min(i, ms->sub_area_width_off);
            const int w =
                ms->sub_area_width + (i < ms->sub_area_width_off ? 1 : 0);
            const int y =
                j * ms->sub_area_height + min(j, ms->sub_area_height_off);
            const int h =
                ms->sub_area_height + (j < ms->sub_area_height_off ? 1 : 0);

            const bool selectable =
                label_selection_is_included(curr_label, ms->label_selection);

            cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
            if (selectable) {
                cairo_set_source_u32(cairo, config->selectable_bg_color);
                cairo_rectangle(cairo, x, y, w, h);
                cairo_fill(cairo);

                cairo_set_source_u32(cairo, config->selectable_border_color);
                cairo_rectangle(cairo, x + .5, y + .5, w - 1, h - 1);
                cairo_set_line_width(cairo, 1);
                cairo_stroke(cairo);

                cairo_text_extents_t te_all;
                label_selection_str(curr_label, label_selected_str);
                cairo_text_extents(cairo, label_selected_str, &te_all);

                label_selection_str_split(
                    curr_label, label_selected_str, label_unselected_str,
                    ms->label_selection->next
                );

                cairo_text_extents_t te_selected, te_unselected;
                cairo_text_extents(cairo, label_selected_str, &te_selected);
                cairo_text_extents(cairo, label_unselected_str, &te_unselected);

                // Centers the label.
                cairo_move_to(
                    cairo,
                    x + (w - te_selected.x_advance - te_unselected.x_advance) /
                            2,
                    y + (int)((h + te_all.height) / 2)
                );
                cairo_set_source_u32(cairo, config->label_select_color);
                cairo_show_text(cairo, label_selected_str);
                cairo_set_source_u32(cairo, config->label_color);
                cairo_show_text(cairo, label_unselected_str);
            }

            label_selection_incr(curr_label);
        }
    }

    label_selection_free(curr_label);
    cairo_translate(cairo, -state->initial_area.x, -state->initial_area.y);
}

void tile_mode_state_free(struct tile_mode_state *tms) {
    label_selection_free(tms->label_selection);
    label_symbols_free(tms->label_symbols);
}

struct mode_interface tile_mode_interface = {
    .key    = tile_mode_key,
    .render = tile_mode_render,
};
