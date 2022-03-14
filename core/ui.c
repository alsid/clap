// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <stdlib.h>
#include <errno.h>
#include "model.h"
#include "render.h"
#include "shader.h"
#include "messagebus.h"
#include "input.h"
#include "ui.h"
#include "font.h"
#include "render.h"

static struct model3d *ui_quad;
static struct model3dtx *ui_quadtx;

static texture_t *ui_fbo_tex; /* XXX */
static struct model3dtx *ui_pip;

static bool __ui_element_is_visible(struct ui_element *uie, struct ui *ui)
{
    if (uie->affinity & UI_SZ_NORES)
        return true;
    if (uie->actual_x + uie->actual_w < 0)
        return false;
    if (uie->actual_x > ui->width)
        return false;
    if (uie->actual_y + uie->height < 0)
        return false;
    if (uie->actual_y > ui->height)
        return false;
    if (uie->force_hidden)
        return false;

    return true;
}

static void ui_element_position(struct ui_element *uie, struct ui *ui)
{
    struct entity3d *e = uie->entity;
    float parent_width = ui->width, parent_height = ui->height;
    float x_off, y_off;

    if (uie->actual_x >= 0.0)
        return;

    if (uie->parent) {
        if (uie->parent->actual_x < 0.0)
            ui_element_position(uie->parent, ui);
        /*trace("parent: %f/%f/%f/%f\n",
              uie->parent->actual_x,
              uie->parent->actual_y,
              uie->parent->actual_w,
              uie->parent->actual_h
             );*/
        parent_width = uie->parent->actual_w;
        parent_height = uie->parent->actual_h;
    }

    x_off = uie->x_off < 1.0 && uie->x_off > 0 ? uie->x_off * parent_width : uie->x_off;
    y_off = uie->y_off < 1.0 && uie->y_off > 0 ? uie->y_off * parent_height : uie->y_off;
    uie->actual_w = uie->width < 1.0 ? uie->width * parent_width : uie->width;
    uie->actual_h = uie->height < 1.0 ? uie->height * parent_height : uie->height;
    if (uie->parent && !(uie->affinity & UI_SZ_NORES)) {
        /* clamp child's w/h to parent's */
        uie->actual_w = min(uie->actual_w, parent_width - x_off);
        uie->actual_h = min(uie->actual_h, parent_height - y_off);
    }

    if (uie->affinity & UI_AF_TOP) {
        if (uie->affinity & UI_AF_BOTTOM) {
            /* ignore y_off: vertically centered */
            uie->actual_y = (parent_height - uie->actual_h) / 2;
        } else {
            uie->actual_y = parent_height - y_off - uie->actual_h;
        }
    } else if (uie->affinity & UI_AF_BOTTOM) {
        uie->actual_y = y_off;
    }

    if (uie->affinity & UI_AF_RIGHT) {
        if (uie->affinity & UI_AF_LEFT) {
            /* ignore x_off: horizontally centered */
            uie->actual_x = (parent_width - uie->actual_w) / 2;
        } else {
            uie->actual_x = parent_width - x_off - uie->actual_w;
        }
    } else if (uie->affinity & UI_AF_LEFT) {
        uie->actual_x = x_off;
    }

    if (uie->parent) {
        uie->actual_x += uie->parent->actual_x;
        uie->actual_y += uie->parent->actual_y;
    }

compute_mx:
    /* We might want force_invisible also */
    e->visible = __ui_element_is_visible(uie, ui) ? 1 : 0;
    /*trace("VIEWPORT %fx%f; xywh: %f %f %f %f\n", parent_width, parent_height,
          uie->actual_x, uie->actual_y, uie->actual_w, uie->actual_h);*/
    mat4x4_identity(e->mx->m);
    mat4x4_translate_in_place(e->mx->m, uie->actual_x, uie->actual_y, 0.0);
    //dbg("## positioning '%s' at %f,%f\n", entity_name(e), uie->actual_x, uie->actual_y);
    if (!uie->prescaled)
        mat4x4_scale_aniso(e->mx->m, e->mx->m, uie->actual_w, uie->actual_h, 1.0);
}

int ui_element_update(struct entity3d *e, void *data)
{
    struct ui_element *uie = e->priv;
    struct ui *ui = uie->ui;
    mat4x4 p;

    ui_element_position(uie, ui);
    if (!e->visible)
        return 0;

    mat4x4_identity(p);
    mat4x4_ortho(p, 0, (float)ui->width, 0, (float)ui->height, 1.0, -1.0);
    mat4x4_mul(e->mx->m, p, e->mx->m);

    return 0;
}

static void ui_debug_update(struct ui *ui);
static void ui_reset_positioning(struct entity3d *e, void *data)
{
    struct ui_element *uie = e->priv;
    /* XXX */
    uie->actual_x = uie->actual_y = uie->actual_w = uie->actual_h = -1.0;
}

static void ui_roll_done(void);
static bool ui_roll_finished;

void ui_update(struct ui *ui)
{
    ui_debug_update(ui);

    /* XXX: this is double for_each, make better */
    mq_for_each(&ui->mq, ui_reset_positioning, NULL);
    mq_update(&ui->mq);
    if (ui_roll_finished)
        ui_roll_done();
}

static void ui_element_destroy(struct entity3d *e)
{
    struct ui_element *uie = e->priv;

    ref_put(uie);
}

static void ui_element_drop(struct ref *ref)
{
    struct ui_element *uie = container_of(ref, struct ui_element, ref);
    struct ui_element *child, *iter;

    trace("dropping ui_element\n");

    list_for_each_entry_iter(child, iter, &uie->children, child_entry) {
        list_del(&child->child_entry);
        child->parent = NULL;
        ref_put(child);
    }

    if (uie->parent) {
        list_del(&uie->child_entry);
        ref_put(uie->parent);
    }

    ui_element_animations_done(uie);
    err_on(!list_empty(&uie->children), "ui_element still has children\n");
    ref_put_last(uie->entity);
}

DECLARE_REFCLASS(ui_element);

struct ui_element *ui_element_new(struct ui *ui, struct ui_element *parent, struct model3dtx *txmodel,
                                  unsigned long affinity, float x_off, float y_off, float w, float h)
{
    struct ui_element *uie;
    struct entity3d *e = entity3d_new(txmodel);

    if (!e)
        return NULL;

    uie = ref_new(ui_element);
    uie->entity = e;
    uie->entity->destroy = ui_element_destroy;
    uie->ui = ui;
    if (parent) {
        uie->parent = ref_get(parent);
        list_append(&parent->children, &uie->child_entry);
    }

    uie->affinity = affinity;
    uie->width    = w;
    uie->height   = h;
    uie->x_off    = x_off;
    uie->y_off    = y_off;
    list_init(&uie->children);
    list_init(&uie->animation);

    //dbg("VIEWPORT: %ux%u; width %u -> %f; height %u -> %f\n", ui->width, ui->height, w, width, h, height);

    e->update  = ui_element_update;
    e->priv    = uie;
    e->visible = 1;
    e->color_pt = COLOR_PT_NONE;
    e->color[3] = 1.0;

    list_append(&txmodel->entities, &e->entry);
    ui_element_position(uie, ui);

    return uie;
}

static void ui_add_model(struct ui *ui, struct model3dtx *txmodel)
{
    mq_add_model(&ui->mq, txmodel);
}

static void ui_add_model_tail(struct ui *ui, struct model3dtx *txmodel)
{
    mq_add_model_tail(&ui->mq, txmodel);
}

static int ui_model_init(struct ui *ui)
{
    float x = 0.f, y = 0.f, w = 1.f, h = 1.f;
    struct shader_prog *prog = shader_prog_find(ui->prog, "ui"); /* XXX */

    if (!prog) {
        err("couldn't load 'ui' shaders\n");
        return -EINVAL;
    }

    ui_quad = model3d_new_quad(prog, x, y, 0.1, w, h);
    ui_quad->cull_face = false;
    ui_quad->alpha_blend = true;
    model3d_set_name(ui_quad, "ui_quad");
    /* XXX: maybe a "textured_model" as another interim object */
    ui_quadtx = model3dtx_new(ui_quad, "transparent.png");
    ref_put(ui_quad);
    ui_add_model_tail(ui, ui_quadtx);
    ref_put(prog);  /* matches shader_prog_find() above */
    return 0;
}

struct ui_text {
    struct font         *font;
    struct ui_element   *parent;
    char                *str;
    struct ui_element   *uietex;
    unsigned long       flags;
    unsigned int        nr_uies;
    unsigned int        nr_lines;
    /* total width of all glyphs in each line, not counting whitespace */
    unsigned int        *line_w; /* array of int x nr_lines */
    /* width of one whitespace for each line */
    unsigned int        *line_ws; /* likewise */
    /* number of words in each line */
    unsigned int        *line_nrw; /* likewise */
    int                 width, height, y_off;
    int                 margin_x, margin_y;
    struct ref          ref;
};

static void ui_text_drop(struct ref *ref)
{
    struct ui_text *uit = container_of(ref, struct ui_text, ref);
    unsigned int    i;

    trace("dropping ui_text\n");
    texture_deinit(uit->uietex->entity->txmodel->texture);
    ref_put_last(uit->uietex);
    free(uit->str);
}

DECLARE_REFCLASS(ui_text);
//#define UIT_REFLOW  1

/*
 * TODO:
 *  + optional reflow
 */
static void ui_text_measure(struct ui_text *uit)
{
    unsigned int i, w = 0, nr_words = 0, nonws_w = 0, ws_w;
    int h_top = 0, h_bottom = 0;
    size_t len = strlen(uit->str);
    struct glyph *glyph;

    free(uit->line_nrw);
    free(uit->line_ws);
    free(uit->line_w);

    glyph = font_get_glyph(uit->font, '-');
    ws_w = glyph->width;
    for (i = 0; i <= len; i++) {
        if (uit->str[i] == '\n' || !uit->str[i]) { /* end of line */
            nr_words++;

            CHECK(uit->line_w = realloc(uit->line_w, sizeof(*uit->line_w) * (uit->nr_lines + 1)));
            CHECK(uit->line_ws = realloc(uit->line_ws, sizeof(*uit->line_ws) * (uit->nr_lines + 1)));
            CHECK(uit->line_nrw = realloc(uit->line_nrw, sizeof(*uit->line_nrw) * (uit->nr_lines + 1)));
            uit->line_w[uit->nr_lines] = nonws_w;// + ws_w * (nr_words - 1);
            uit->line_nrw[uit->nr_lines] = nr_words - 1;
            w = max(w, nonws_w + ws_w * (nr_words - 1));
            uit->nr_lines++;
            nonws_w = nr_words = 0;
            continue;
        }

        if (isspace(uit->str[i])) {
            nr_words++;
            continue;
        }

        glyph = font_get_glyph(uit->font, uit->str[i]);
        nonws_w += glyph->advance_x >> 6;
        if (glyph->bearing_y < 0) {
            h_top = max(h_top, (int)glyph->height + glyph->bearing_y);
            h_bottom = max(h_bottom, -glyph->bearing_y);
        } else {
            h_top = max(h_top, glyph->bearing_y);
            h_bottom = max(h_bottom, max((int)glyph->height - glyph->bearing_y, 0));
        }
    }

    for (i = 0; i < uit->nr_lines; i++)
        if ((uit->flags & UI_AF_VCENTER) == UI_AF_VCENTER)
            uit->line_ws[i] = uit->line_nrw[i] ? (w - uit->line_w[i]) / uit->line_nrw[i] : 0;
        else
            uit->line_ws[i] = ws_w;

    uit->width  = w;
    uit->y_off = h_top;
    uit->height = (h_top + h_bottom) * uit->nr_lines;
}

static inline int x_off(struct ui_text *uit, unsigned int line)
{
    int x = uit->margin_x;

    if (uit->flags & UI_AF_RIGHT) {
        if (uit->flags & UI_AF_LEFT) {
            if (uit->line_w[line])
               x += (uit->width - uit->line_w[line]) / 2;
        } else {
            x = uit->width + uit->margin_x - uit->line_w[line] -
                uit->line_ws[line] * uit->line_nrw[line];
        }
    }

    return x;
}

static struct model3dtx *ui_txm_find_by_texture(struct ui *ui, texture_t *tex)
{
    struct model3dtx *txmodel;

    /* XXX: need trees for better search, these lists are actually long */
    /* XXX^2: struct mq */
    list_for_each_entry(txmodel, &ui->mq.txmodels, entry) {
        /*
         * Since it's already on the list, the "extra" list
         * reference is already taken, the next ui element
         * to use it needs only its own reference.
         */
        if (texture_id(txmodel->texture) == texture_id(tex))
            return txmodel;
    }

    return NULL;
}

struct ui_text *
ui_render_string(struct ui *ui, struct font *font, struct ui_element *parent,
                 const char *str, float *color, unsigned long flags)
{
    size_t len = strlen(str);
    struct ui           fbo_ui;
    struct ui_element   **uies;
    struct model3dtx    *txm, *txmtex;
    struct fbo          *fbo;
    struct ui_text      *uit;
    struct shader_prog  *prog;
    struct glyph        *glyph;
    struct model3d      *m;
    unsigned int        i, line;
    float               x, y;

    if (!flags)
        flags = UI_AF_VCENTER;

    CHECK(uit       = ref_new(ui_text));
    uit->flags      = flags;
    uit->margin_x   = 10;
    uit->margin_y   = 10;
    uit->parent     = parent;
    CHECK(uit->str  = strdup(str));
    uit->font       = font;

    ui_text_measure(uit);

    fbo_ui.width = uit->width + uit->margin_x * 2;
    fbo_ui.height = uit->height + uit->margin_y * 2;
    fbo_ui.prog = ui->prog;
    mq_init(&fbo_ui.mq, &fbo_ui);
    fbo = fbo_new(fbo_ui.width, fbo_ui.height);
    // fbo->retain_tex = 1;

    if (uit->parent) {
        uit->parent->width = uit->width + uit->margin_x * 2;
        uit->parent->height = uit->height + uit->margin_y * 2;
        ui_element_position(uit->parent, ui);
    }
    y = (float)uit->margin_y + uit->y_off;
    dbg_on(y < 0, "y: %f, height: %d y_off: %d, margin_y: %d\n",
           y, uit->height, uit->y_off, uit->margin_y);
    CHECK(prog      = shader_prog_find(ui->prog, "glyph"));
    CHECK(uies = calloc(len, sizeof(struct ui_element *)));
    uit->nr_uies = len;
    for (line = 0, i = 0, x = x_off(uit, line); i < len; i++) {
        if (str[i] == '\n') {
            line++;
            y += (uit->height / uit->nr_lines);
            x = x_off(uit, line);
            continue;
        }
        if (isspace(str[i])) {
            x += uit->line_ws[line];
            continue;
        }
        glyph   = font_get_glyph(uit->font, str[i]);
        txm = ui_txm_find_by_texture(&fbo_ui, &glyph->tex);
        if (!txm) {
            m       = model3d_new_quad(prog, 0, 0, 0, glyph->width, glyph->height);
            //model3d_set_name(m, "glyph");
            model3d_set_name(m, "glyph_%s_%c", font_name(uit->font), str[i]);
            m->cull_face = false;
            m->alpha_blend = true;
            txm = model3dtx_new_texture(ref_pass(m), &glyph->tex);
            ui_add_model(&fbo_ui, txm);
        }
        /* uies[i] consumes (holds the only reference to) txm */
        uies[i] = ui_element_new(&fbo_ui, NULL, txm, UI_AF_TOP | UI_AF_LEFT,
                                      x + glyph->bearing_x,
                                      y - glyph->bearing_y,
                                      glyph->width, glyph->height);
        ref_only(uies[i]->entity);
        ref_only(uies[i]);
        uies[i]->entity->color[0] = color[0];
        uies[i]->entity->color[1] = color[1];
        uies[i]->entity->color[2] = color[2];
        uies[i]->entity->color[3] = color[3];
        uies[i]->entity->color_pt = COLOR_PT_ALL;

        uies[i]->prescaled = true;
        
        /* XXX: to trigger ui_element_position() XXX */
        uies[i]->actual_x = uies[i]->actual_y = -1;
        // uies[i]->actual_w = glyph->width;
        // uies[i]->actual_h = glyph->height;
        entity3d_update(uies[i]->entity, &fbo_ui);
        x += glyph->advance_x >> 6;
    }

    fbo_prepare(fbo);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    // dbg("rendering '%s' uit(%dx%d) to FBO %d (%dx%d)\n", str, uit->width, uit->height,
    //     fbo->fbo, fbo->width, fbo->height);
    models_render(&fbo_ui.mq, NULL, NULL, NULL, NULL, NULL, NULL);
    mq_release(&fbo_ui.mq);
    fbo_done(fbo, ui->width, ui->height);

    ref_put(prog);

    free(uies);
    free(uit->line_nrw);
    free(uit->line_ws);
    free(uit->line_w);

    prog = shader_prog_find(ui->prog, "ui");
    m = model3d_new_quad(prog, 0, 1, 0, 1, -1);
    model3d_set_name(m, "ui_text: '%s'", str);
    m->cull_face = false;
    m->alpha_blend = true;
    txmtex = model3dtx_new_texture(ref_pass(m), texture_clone(&fbo->tex));
    ref_put_last(fbo);
    ui_add_model(ui, txmtex);
    ref_put(prog);

    uit->uietex = ui_element_new(ui, parent, ref_pass(txmtex),
                                 parent ? UI_AF_CENTER : UI_AF_HCENTER | UI_AF_BOTTOM,
                                 0, 0, fbo_ui.width, fbo_ui.height);
    ref_only(uit->uietex->entity);
    ref_only(uit->uietex);

    return uit;
}

static const char *menu_font = "MorganChalk-L3aJy.ttf";
static struct ui_element *ui_roll_element;
static struct ui_text *ui_roll_text;

static void ui_roll_done(void)
{
    if (!ui_roll_element || !ui_roll_text)
        return;

    ref_put_last(ui_roll_text);
    // ref_put_last(ui_roll_element);

    ui_roll_text    = NULL;
    ui_roll_element = NULL;
}

static int ui_roll_update(struct entity3d *e, void *data)
{
    struct ui_element *uie = e->priv;
    struct ui *ui = uie->ui;
    mat4x4 p;


    if (uie->y_off == ui->height + uie->height) {
        dbg("credit roll done at %f\n", uie->y_off);
        ui_roll_finished = true;
        return 0;
    }
    uie->y_off++;
    // uie->entity->dy = uie->actual_y = uie->y_off;
    ui_element_update(e, data);

    return 0;
}

static void ui_roll_init(struct ui *ui)
{
    float color[] = { 0.7, 0.7, 0.7, 1.0 };
    struct lib_handle *lh;
    LOCAL(char, buffer);
    struct font *font;
    size_t       size;

    lh = lib_read_file(RES_ASSET, "TODO.txt", (void **)&buffer, &size);
    font            = font_open("Cabal-w5j3.ttf", 24);
    // ui_roll_element = ui_element_new(ui, NULL, ui_quadtx, UI_AF_TOP | UI_AF_HCENTER/* | UI_SZ_NORES*/, 0, ui->height, 300, 100);
    // ui_roll_element->entity->update = ui_roll_update;

    ui_roll_text = ui_render_string(ui, font, NULL, buffer, color, UI_AF_HCENTER | UI_AF_BOTTOM | UI_SZ_NORES);
    ui_roll_element = ui_roll_text->uietex;
    // ui_roll_element->affinity |= UI_SZ_NORES;
    ui_roll_element->entity->update = ui_roll_update;
    ui_roll_element->y_off = -ui_roll_element->height;
    ui_element_position(ui_roll_text->uietex, ui);
    ref_put_last(lh);
    buffer = NULL;

    font_put(font);
}

static bool display_fps;
static struct ui_text *bottom_uit;
static struct ui_element *bottom_element;
static const char **ui_debug_mods = NULL;
static unsigned int nr_ui_debug_mods;
static unsigned int ui_debug_current;
static char **ui_debug_strs;
static struct ui_text *debug_uit;
static struct ui_element *debug_element;
static struct font *debug_font;

static char **ui_debug_mod_str(const char *mod)
{
    int i;

    mod = str_basename(mod);
    for (i = 0; i < nr_ui_debug_mods; i++)
        if (!strcmp(mod, ui_debug_mods[i]))
            goto found;
    
    CHECK(ui_debug_mods = realloc(ui_debug_mods, sizeof(char *) * (i + 1)));
    CHECK(ui_debug_strs = realloc(ui_debug_strs, sizeof(char *) * (i + 1)));
    nr_ui_debug_mods++;

    ui_debug_mods[i] = mod;
    ui_debug_strs[i] = NULL;
found:
    return &ui_debug_strs[i];
}

static void ui_debug_update(struct ui *ui)
{
    struct font *font;
    float color[] = { 0.9, 0.1, 0.2, 1.0 };
    char *str;

    if (!ui_debug_strs || !nr_ui_debug_mods)
        return;

    str = ui_debug_strs[ui_debug_current];
    // char *x = ref_classes_get_string();
    // if (ui_debug_str && !strcmp(x, ui_debug_str))
    //     return;

    // ui_debug_str = x;
    if (debug_uit) {
        ref_put_last(debug_uit);
        debug_uit = NULL;
    } else if (str) {
        debug_element = ui_element_new(ui, NULL, ui_quadtx, UI_AF_BOTTOM | UI_AF_LEFT, 0.01, 50, 400, 150);
    }
    if (str) {
        font = font_get(debug_font);
        debug_uit = ui_render_string(ui, font, debug_element, str, color, UI_AF_LEFT);
        font_put(font);
    }
}

void __ui_debug_printf(const char *mod, const char *fmt, ...)
{
    char **pstr = ui_debug_mod_str(mod);
    char *old = *pstr;
    va_list va;
    int ret;

    va_start(va, fmt);
    ret = vasprintf(pstr, fmt, va);
    va_end(va);

    if (ret < 0)
        *pstr = NULL;
    else
        free(old);
}

static void ui_element_for_each_child(struct ui_element *uie, void (*cb)(struct ui_element *x, void *data), void *data)
{
    struct ui_element *child, *itc;

    list_for_each_entry_iter(child, itc, &uie->children, child_entry) {
        ui_element_for_each_child(child, cb, data);
    }
    cb(uie, data);
}

static void __set_visibility(struct ui_element *uie, void *data)
{
    uie->entity->visible = !!*(int *)data;
    uie->force_hidden = !*(int *)data;
}

void ui_element_set_visibility(struct ui_element *uie, int visible)
{
    ui_element_for_each_child(uie, __set_visibility, &visible);
}

static void __set_alpha(struct ui_element *uie, void *data)
{
    uie->entity->color[3] = *(float *)data;
}

void ui_element_set_alpha_one(struct ui_element *uie, float alpha)
{
    __set_alpha(uie, &alpha);
}

void ui_element_set_alpha(struct ui_element *uie, float alpha)
{
    ui_element_for_each_child(uie, __set_alpha, &alpha);
}

static struct ui_widget *ui_menu_new(struct ui *ui, const char **items, unsigned int nr_items);
static void ui_menu_done(struct ui *ui);

/* XXX: basic font picker */
static const char *font_names[] = {
    "AovelSansRounded-rdDL.ttf", "BeckyTahlia-MP6r.ttf",
    "Cabal-w5j3.ttf",
    "LiberationSansBold.ttf",    "MorganChalk-L3aJy.ttf",
    "Pixellettersfull-BnJ5.ttf", "RoughenCornerRegular-7RjV.ttf",
    "ShortBaby-Mg2w.ttf",        "ToThePointRegular-n9y4.ttf",
};

static void do_fonts(struct ui *ui, const char *font_name)
{
    int i;

    for (i = 0; i < array_size(font_names); i++) {
        if (!strcmp(font_name, font_names[i]))
            goto found;
    }

    return;
found:
    menu_font = font_names[i];
    ui_menu_done(ui);
}

static void do_debugs(struct ui *ui, const char *debug_name)
{
    int i;

    for (i = 0; i < nr_ui_debug_mods; i++) {
        if (!strcmp(debug_name, ui_debug_mods[i]))
            goto found;
    }

    return;
found:
    ui_debug_current = i;
    ui_menu_done(ui);
}

static const char *help_items[] = { "...todo", "...help", "...credits" };
static const char *hud_items[] = { "FPS", "Build", "Limeric" };
static const char *pip_items[] = { "+float TL", "+float TR", "+left half", "+right half" };
static void menu_onclick(struct ui_element *uie, float x, float y)
{
    int nr = (long)uie->priv;
    struct ui *ui = uie->ui;

    if (!strcmp(ui->menu->texts[nr]->str, "Help")) {
        ref_put_last(ui->menu);
        ui->menu = ui_menu_new(ui, help_items, array_size(help_items));
    } else if (!strcmp(ui->menu->texts[nr]->str, "Exit")) {
        ui_menu_done(ui);
        gl_request_exit();
    } else if (!strcmp(ui->menu->texts[nr]->str, "HUD")) {
        ref_put_last(ui->menu);
        ui->menu = ui_menu_new(ui, hud_items, array_size(hud_items));
    } else if (!strcmp(ui->menu->texts[nr]->str, "PIP")) {
        ref_put_last(ui->menu);
        ui->menu = ui_menu_new(ui, pip_items, array_size(pip_items));
    } else if (!strcmp(ui->menu->texts[nr]->str, "Fonts")) {
        ref_put_last(ui->menu);
        ui->menu = ui_menu_new(ui, font_names, array_size(font_names));
    } else if (!strcmp(ui->menu->texts[nr]->str, "Monitor")) {
        ref_put_last(ui->menu);
        ui->menu = ui_menu_new(ui, ui_debug_mods, nr_ui_debug_mods);
    } else if (!strcmp(ui->menu->texts[nr]->str, "Fullscreen")) {
        struct message_input mi;
        memset(&mi, 0, sizeof(mi));
        mi.fullscreen = 1;
        message_input_send(&mi, NULL);
    } else if (!strcmp(ui->menu->texts[nr]->str, "FPS")) {
        if (display_fps) {
            display_fps = false;
            ref_put_last(bottom_uit);
            ref_put_last(bottom_element);
            bottom_uit = NULL;
        } else {
            display_fps = true;
        }
    } else if (!strcmp(ui->menu->texts[nr]->str, "Devel")) {
        struct message m;
        memset(&m, 0, sizeof(m));
        m.type = MT_COMMAND;
        m.cmd.toggle_fuzzer = 1;
        message_send(&m);
        ui_menu_done(ui); /* cancels modality */
    } else if (!strcmp(ui->menu->texts[nr]->str, "Autopilot")) {
        struct message m;
        memset(&m, 0, sizeof(m));
        m.type = MT_COMMAND;
        m.cmd.toggle_autopilot = 1;
        message_send(&m);
        ui_menu_done(ui); /* cancels modality */
    } else if (!strcmp(ui->menu->texts[nr]->str, "...todo")) {
        ui_roll_init(ui);
        ui_menu_done(ui); /* cancels modality */
    } else {
        do_fonts(ui, ui->menu->texts[nr]->str);
        do_debugs(ui, ui->menu->texts[nr]->str);
    }
}

static void ui_widget_drop(struct ref *ref)
{
    struct ui_widget *uiw = container_of(ref, struct ui_widget, ref);
    int i;

    for (i = 0; i < uiw->nr_uies; i++) {
        ref_put(uiw->texts[i]);
        ref_put(uiw->uies[i]);
    }
    ref_put_last(uiw->root);
    free(uiw->texts);
    free(uiw->uies);
}

DECLARE_REFCLASS(ui_widget);

static struct ui_widget *
ui_widget_new(struct ui *ui, unsigned int nr_items, unsigned long affinity,
              float x_off, float y_off, float w, float h)
{
    struct ui_widget *uiw;

    CHECK(uiw        = ref_new(ui_widget));
    CHECK(uiw->uies  = calloc(nr_items, sizeof(struct ui_element *)));
    /* XXX: render texts to FBOs to textures */
    CHECK(uiw->texts = calloc(nr_items, sizeof(struct ui_text *)));
    CHECK(uiw->root  = ui_element_new(ui, NULL, ui_quadtx, affinity, x_off, y_off, w, h));
    uiw->nr_uies = nr_items;

    return uiw;
}

/*
 * XXX
 * also, 4 items
 * 
 * + parent uie
 */
static struct ui_widget *ui_wheel_new(struct ui *ui, const char **items)
{
    float quad_color[] = { 0.0, 0.3, 0.1, 1.0 };
    float color[] = { 0.7, 0.7, 0.7, 1.0 };
    unsigned long affs[] = {
        UI_AF_TOP     | UI_AF_HCENTER,
        UI_AF_VCENTER | UI_AF_RIGHT,
        UI_AF_BOTTOM  | UI_AF_HCENTER,
        UI_AF_VCENTER | UI_AF_LEFT,
    };
    enum uie_mv motions[] = {
        UIE_MV_Y_OFF,
        UIE_MV_X_OFF,
        UIE_MV_X_OFF,
        UIE_MV_Y_OFF,
    };
    float width, height;
    struct ui_widget *wheel;
    struct font *font;
    int i;

    /* XXX^0: affinity and placement hardcoded */
    CHECK(wheel = ui_widget_new(ui, 4, UI_AF_VCENTER | UI_AF_HCENTER, 0, 0, 0.3, 0.3));
    wheel->focus   = -1;

    /* XXX^2: font global/hardcoded */
    font = font_open("Pixellettersfull-BnJ5.ttf", 48);
    for (i = 0, width = 0.0, height = 0.0; i < 4; i++) {
        /* XXX^3: element placement hardcoded */
        wheel->uies[i]           = ui_element_new(ui, wheel->root, ui_quadtx, affs[i], 0, 0, 300, 100);
        wheel->uies[i]->on_click = menu_onclick;
        wheel->uies[i]->priv     = (void *)(long)i;

        memcpy(&wheel->uies[i]->entity->color, quad_color, sizeof(quad_color));
        wheel->uies[i]->entity->color_pt = COLOR_PT_ALL;
        /* XXX^5: animations hardcoded */
        // uia_skip_frames(wheel->uies[i], i * 7);
        uia_set_visible(wheel->uies[i], 1);
        uia_lin_float(wheel->uies[i], ui_element_set_alpha_one, 0, 1.0, 100);
        uia_cos_move(wheel->uies[i], motions[i], i < 2 ? 200 : 1, i < 2 ? 1 : 200, 30, 1.0, 0.0);

        CHECK(wheel->texts[i] = ui_render_string(ui, font, wheel->uies[i], items[i], color, 0));
        width = max(width, wheel->uies[i]->width);
        height = max(height, wheel->uies[i]->height);
        ui_element_set_visibility(wheel->uies[i], 0);
    }
    for (i = 0; i < 4; i++) {
        wheel->uies[i]->width = width;
        wheel->uies[i]->height = height;
        // if (i > 0)
        //     wheel->uies[i]->y_off = 10 + (4 + height) * i;
    }
    font_put(font);

    return wheel;
}
static struct ui_widget *ui_menu_new(struct ui *ui, const char **items, unsigned int nr_items)
{
    float quad_color[] = { 0.0, 0.1, 0.5, 1.0 };
    float color[] = { 0.5, 0.3, 0.4, 1.0 };
    float off, width, height;
    struct ui_widget *menu;
    struct model3dtx *txm;
    struct model3d *model;
    struct font *font;
    int i;

    /* XXX^0: affinity and placement hardcoded */
    CHECK(menu = ui_widget_new(ui, nr_items, UI_AF_VCENTER | UI_AF_RIGHT, 10, 10, 500, 0.8));
    menu->focus   = -1;

    model = model3d_new_quad(ui->prog, 0, 0, 0.05, 1, 1);
    model3d_set_name(model, "ui_menu");
    // /* XXX^1: texture model(s) hardcoded */
    txm = model3dtx_new(ref_pass(model), "green.png");
    ui_add_model(ui, txm);
    /* XXX^2: font global/hardcoded */
    font = font_open(menu_font, 48);
    for (i = 0, off = 0.0, width = 0.0, height = 0.0; i < nr_items; i++) {
        /* XXX^3: element placement hardcoded */
        menu->uies[i]           = ui_element_new(ui, menu->root, txm, UI_AF_TOP | UI_AF_RIGHT, 10, 10 + off, 300, 100);
        menu->uies[i]->on_click = menu_onclick;
        menu->uies[i]->priv     = (void *)(long)i;

        memcpy(&menu->uies[i]->entity->color, quad_color, sizeof(quad_color));
        menu->uies[i]->entity->color_pt = COLOR_PT_ALL;

        /* XXX^5: animations hardcoded */
        uia_skip_frames(menu->uies[i], i * 7);
        uia_set_visible(menu->uies[i], 1);
        uia_lin_float(menu->uies[i], ui_element_set_alpha, 0, 1.0, 100);
        uia_cos_move(menu->uies[i], UIE_MV_X_OFF, 200, 1, 30, 1.0, 0.0);

        CHECK(menu->texts[i] = ui_render_string(ui, font, menu->uies[i], items[i], color, 0));
        menu->texts[i]->uietex->entity->color_pt = COLOR_PT_NONE;
        width = max(width, menu->uies[i]->width);
        height = max(height, menu->uies[i]->height);
        off += menu->uies[i]->height + 4 /* XXX: margin */;
        ui_element_set_visibility(menu->uies[i], 0);
    }
    /* menu item uies hold the only references to this txm */
    // ref_put(txm);
    for (i = 0; i < nr_items; i++) {
        menu->uies[i]->width = width;
        menu->uies[i]->height = height;
        if (i > 0)
            menu->uies[i]->y_off = 10 + (4 + height) * i;
    }
    font_put(font);

    return menu;
}

static void ui_widget_pick_rel(struct ui_widget *uiw, int dpos)
{
    if (!dpos)
        return;

    /* out-of-focus animation */
    if (uiw->focus >= 0)
        uia_lin_move(uiw->uies[uiw->focus], UIE_MV_X_OFF, 20, 1, 10);

    if (dpos + (int)uiw->focus < 0)
        uiw->focus = uiw->nr_uies - 1;
    else if (dpos + uiw->focus >= uiw->nr_uies)
        uiw->focus = 0;
    else
        uiw->focus += dpos;

    /* in-focus-animation */
    uia_lin_move(uiw->uies[uiw->focus], UIE_MV_X_OFF, 1, 20, 10);
}

static const char *menu_items[] = { "HUD", "Monitor", "Fullscreen", "PIP", "Autopilot", "Fonts", "Settings", "Network", "Devel", "Help", "Exit" };
static void ui_menu_init(struct ui *ui)
{
    ui->menu = ui_menu_new(ui, menu_items, array_size(menu_items));
    /* XXX: should send a message to others that the UI owns inputs now */
    ui->modal = true;
}

static void ui_menu_done(struct ui *ui)
{
    ref_put(ui->menu);
    ui->menu = NULL;
    ui->modal = false;
}

static int ui_widget_within(struct ui_widget *uiw, int x, int y)
{
    struct ui_element *child;
    int i;

    for (i = 0; i < uiw->nr_uies; i++) {
        child = uiw->uies[i];

        if (x >= child->actual_x && x < child->actual_x + child->actual_w && y >= child->actual_y &&
            y < child->actual_y + child->actual_h)
            return i;
    }

    return -1;
}

static void ui_widget_hover(struct ui_widget *uiw, int x, int y)
{
    int n = ui_widget_within(uiw, x, y);

    if (n == uiw->focus)
        return;

    if (uiw->focus >= 0)
        uia_lin_move(uiw->uies[uiw->focus], UIE_MV_X_OFF, 20, 1, 10);
    if (n >= 0)
        uia_lin_move(uiw->uies[n], UIE_MV_X_OFF, 1, 20, 10);
    uiw->focus = n;
}

static void ui_menu_click(struct ui_widget *uiw, int x, int y)
{
    int n = ui_widget_within(uiw, x, y);
    struct ui_element *child;

    if (n < 0) {
        ui_menu_done(uiw->root->ui); /* cancels modality */
        return;
    }

    child = uiw->uies[n];
    child->on_click(child, (float)x - child->actual_x, (float)y - child->actual_y);
}

static int ui_handle_command(struct message *m, void *data)
{
    struct font *font = font_get_default();
    float color[] = { 0.7, 0.7, 0.7, 1.0 };
    struct ui *ui = data;
    LOCAL(char, str);

    if (m->type != MT_COMMAND)
        return 0;

    if (m->cmd.status && display_fps) {
        if (bottom_uit) {
            ref_put_last(bottom_uit);
        } else {
            bottom_element = ui_element_new(ui, NULL, ui_quadtx, UI_AF_BOTTOM | UI_AF_RIGHT, 0.01, 50, 400, 150);
        }
        CHECK(asprintf(&str, "FPS: %d\nTime: %d:%02d", m->cmd.fps,
                       m->cmd.sys_seconds / 60, m->cmd.sys_seconds % 60));
        bottom_uit = ui_render_string(ui, font, bottom_element, str, color, UI_AF_RIGHT);
    } else if (m->cmd.menu_enter) {
        ui_menu_init(ui);
    } else if (m->cmd.menu_exit) {
        ui_menu_done(ui);
    }
    font_put(font);

    return 0;
}

struct ui_element_match_struct {
    struct ui_element   *match;
    int                 x, y;
};

static void ui_element_match(struct entity3d *e, void *data)
{
    struct ui_element_match_struct *sd = data;

    if (sd->match)
        return;

    if (sd->x >= e->dx && sd->x < e->dx && sd->y >= e->dy && sd->y < e->dy)
        sd->match = e->priv;
}

static bool ui_element_click(struct ui *ui, int x, int y)
{
    struct ui_element_match_struct sd = {
        .x = x,
        .y = y,
    };

    mq_for_each(&ui->mq, ui_element_match, &sd);
    if (sd.match && sd.match->on_click) {
        sd.match->on_click(sd.match, x - sd.match->x_off, y - sd.match->y_off);
        return true;
    }

    return false;
}

static int ui_handle_input(struct message *m, void *data)
{
    struct ui *ui = data;

    if (m->input.menu_toggle) {
        if (ui->menu)
            ui_menu_done(ui);
        else
            ui_menu_init(ui);
    } else if (m->input.mouse_click) {
        if (!ui->menu) {
            if (!ui_element_click(ui, m->input.x, m->input.y))
                ui_menu_init(ui);
        } else
            ui_menu_click(ui->menu, m->input.x, (int)ui->height - m->input.y);
    }

    if (!ui->modal)
        return 0;
    
    if (m->input.mouse_move)
        ui_widget_hover(ui->menu, m->input.x, (int)ui->height - m->input.y);
    
    /* UI owns the inputs */
    ui->mod_y += m->input.delta_ly;
    if (m->input.up || ui->mod_y <= -100) {
        // select previous
        ui->mod_y = 0;
        ui_widget_pick_rel(ui->menu, -1);
    } else if (m->input.down || ui->mod_y >= 100) {
        // select next
        ui->mod_y = 0;
        ui_widget_pick_rel(ui->menu, 1);
    } else if (m->input.left || m->input.delta_lx < 0 || m->input.back) {
        // go back
        ui_menu_done(ui);
    } else if (m->input.right || m->input.delta_lx > 0 || m->input.enter) {
        // enter
        if (ui->menu->focus >= 0)
            ui->menu->uies[ui->menu->focus]->on_click(ui->menu->uies[ui->menu->focus], 0, 0);
    }
    return 0;
}

static struct ui_text *limeric_uit;
static struct ui_text *build_uit;
struct ui_element *uie0, *uie1;

void ui_pip_update(struct ui *ui, struct fbo *fbo)
{
    struct model3d *m;
    struct shader_prog *prog;

    ui_fbo_tex = &fbo->tex;

    if (ui_pip)
        ref_put(ui_pip);
    if (uie0)
        ref_put(uie0);

    prog = shader_prog_find(ui->prog, "ui");
    m = model3d_new_quad(prog, 0, 1, 0.1, 1, -1);
    m->cull_face = false;
    m->alpha_blend = false;
    ui_pip = model3dtx_new_texture(ref_pass(m), ui_fbo_tex);
    ui_add_model_tail(ui, ui_pip);
    ref_put(prog);
    dbg("### ui_pip tex: %d width: %d height: %d\n", texture_id(ui_fbo_tex), fbo->width, fbo->height);
    if (fbo->width < fbo->height)
        uie0 = ui_element_new(ui, NULL, ui_pip, UI_AF_VCENTER | UI_AF_LEFT, 0, 0, fbo->width, fbo->height);
    else
        uie0 = ui_element_new(ui, NULL, ui_pip, UI_AF_TOP | UI_AF_HCENTER, 0, 0, fbo->width, fbo->height);
    uie0->entity->color_pt = COLOR_PT_NONE;
}

static void build_onclick(struct ui_element *uie, float x, float y)
{
    dbg("build onclick\n");
}

static const char text_str[] =
    "On the chest of a barmaid in Sale\n"
    "Were tattooed all the prices of ale;\n"
    "And on her behind, for the sake of the blind,\n"
    "Was the same information in Braille";

static struct ui_widget *wheel;
static const char *wheel_items[] = { "^", ">", "v", "<" };
extern const char *build_date;
int ui_init(struct ui *ui, int width, int height)
{
    struct font *font;
    float color[] = { 0.7, 0.7, 0.7, 1.0 };

    mq_init(&ui->mq, ui);
    lib_request_shaders("glyph", &ui->prog);
    lib_request_shaders("ui", &ui->prog);

    ui->click = sound_load("stapler.ogg");
    sound_set_gain(ui->click, 0.2);
    debug_font = font_open("Pixellettersfull-BnJ5.ttf", 40);
    font = font_open("Pixellettersfull-BnJ5.ttf", 32);
    ui_model_init(ui);
    uie1 = ui_element_new(ui, NULL, ui_quadtx, UI_AF_TOP    | UI_AF_LEFT, 10, 10, 300, 100);
    uie1->on_click = build_onclick;
    //limeric_uit = ui_render_string(ui, font, uie0, text_str, color, 0/*UI_AF_RIGHT*/);
    build_uit = ui_render_string(ui, font, uie1, build_date, color, 0);

    wheel = ui_wheel_new(ui, wheel_items);
    font_put(font);
    subscribe(MT_COMMAND, ui_handle_command, ui);
    subscribe(MT_INPUT, ui_handle_input, ui);
    return 0;
}

void ui_done(struct ui *ui)
{
    if (ui->menu)
        ui_menu_done(ui);

    font_put(debug_font);
    if (uie0)
        ref_put(uie0);
    ref_put_last(build_uit);
    ref_put_last(uie1);
    if (display_fps && bottom_uit) {
        ref_put_last(bottom_uit);
        ref_put_last(bottom_element);
    }
    //ref_put_last(limeric_uit);
    /* XXX */
    if (nr_ui_debug_mods) {
        ref_put(debug_element);
        ref_put_last(debug_uit);
    }
    ui_roll_done();

    mq_release(&ui->mq);
}

void ui_show(struct ui *ui)
{
}
