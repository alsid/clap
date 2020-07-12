#ifndef __CLAP_UI_H__
#define __CLAP_UI_H__

#include "object.h"
#include "model.h"
#include "scene.h"

#define UI_AF_TOP    0x1
#define UI_AF_BOTTOM 0x2
#define UI_AF_LEFT   0x4
#define UI_AF_RIGHT  0x8
#define UI_AF_HCENTER (UI_AF_LEFT | UI_AF_RIGHT)
#define UI_AF_VCENTER (UI_AF_TOP | UI_AF_BOTTOM)
#define UI_AF_CENTER (UI_AF_VCENTER | UI_AF_HCENTER)

struct ui_element {
    struct ref       ref;
    struct entity3d *entity;
    struct ui_element *parent;
    unsigned long    affinity;
    bool             prescaled;
    float            x_off, y_off;
    float            width, height;
    float            actual_x;
    float            actual_y;
    float            actual_w;
    float            actual_h;
};

//int ui_element_init(struct scene *s, float x, float y, float w, float h);
struct ui {
    struct model3d     *_model;
    struct list        txmodels;
    struct shader_prog *prog;
    int width, height;
};

struct ui_element *ui_element_new(struct ui *ui, struct ui_element *parent, struct model3dtx *txmodel,
                                  unsigned long affinity, float x_off, float y_off, float w, float h);

int ui_init(struct ui *ui, int width, int height);
void ui_update(struct ui *ui);

#endif /* __CLAP_UI_H__ */