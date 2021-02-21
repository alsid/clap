#ifndef __CLAP_MODEL_H__
#define __CLAP_MODEL_H__

//#include <ode/ode.h>
#include "common.h"
#include "object.h"
#include "librarian.h"
#include "display.h" /* XXX: OpenGL headers are included there */
#include "objfile.h"
#include "physics.h"
#include "matrix.h"

struct scene;
struct shader_prog;

struct light {
    GLfloat pos[3];
    GLfloat color[3];
};

struct model3d {
    char                *name;
    struct ref          ref;
    struct shader_prog  *prog;
    bool                cull_face;
    bool                alpha_blend;
    GLuint              vertex_obj;
    GLuint              index_obj;
    GLuint              tex_obj;
    GLuint              norm_obj;
    GLuint              nr_vertices;
    float               *vx;
    float               *norm;
    size_t              vxsz;
    unsigned short      *idx;
    size_t              idxsz;
};

struct model3dtx {
    struct model3d *model;
    GLuint         texture_id;
    bool           external_tex;
    struct ref     ref;
    struct list    entry;              /* link to scene/ui->txmodels */
    struct list    entities;           /* links entity3d->entry */
};

struct model3d *model3d_new_from_vectors(const char *name, struct shader_prog *p, GLfloat *vx, size_t vxsz,
                                         GLushort *idx, size_t idxsz, GLfloat *tx, size_t txsz, GLfloat *norm,
                                         size_t normsz);
struct model3d *model3d_new_from_model_data(const char *name, struct shader_prog *p, struct model_data *md);
void model3d_set_name(struct model3d *m, const char *fmt, ...);
struct model3dtx *model3dtx_new(struct model3d *m, const char *name);
struct model3dtx *model3dtx_new_from_buffer(struct model3d *model, void *buffer, size_t length);
struct model3dtx *model3dtx_new_txid(struct model3d *model, unsigned int txid);
struct model3d *model3d_new_cube(struct shader_prog *p);
struct model3d *model3d_new_quad(struct shader_prog *p, float x, float y, float z, float w, float h);
void model3dtx_prepare(struct model3dtx *m);
void model3dtx_done(struct model3dtx *m);
void model3dtx_draw(struct model3dtx *m);
struct lib_handle *lib_request_obj(const char *name, struct scene *scene);
struct lib_handle *lib_request_bin_vec(const char *name, struct scene *scene);

static inline const char *txmodel_name(struct model3dtx *txm)
{
    return txm->model->name;
}

/* XXX: find a better place; util.h not good */
static inline float cos_interp(float a, float b, float blend)
{
    double theta = blend * M_PI;
    float  f = (1.f - cos(theta)) / 2.f;
    return a * (1.f - f) + b * f;
}

static inline float barrycentric(vec3 p1, vec3 p2, vec3 p3, vec2 pos)
{
    float det = (p2[2] - p3[2]) * (p1[0] - p3[0]) + (p3[0] - p2[0]) * (p1[2] - p3[2]);
    float l1  = ((p2[2] - p3[2]) * (pos[0] - p3[0]) + (p3[0] - p2[0]) * (pos[1] - p3[2])) / det;
    float l2  = ((p3[2] - p1[2]) * (pos[0] - p3[0]) + (p1[0] - p3[0]) * (pos[1] - p3[2])) / det;
    float l3  = 1.0f - l1 - l2;
    return l1 * p1[1] + l2 * p2[1] + l3 * p3[1];
}

struct entity3d {
    struct model3dtx *txmodel;
    struct matrix4f  *mx;
    //struct matrix4f  *base_mx;
    struct ref       ref;
    struct list      entry;     /* link to txmodel->entities */
    unsigned int     visible;
    void *phys_geom; /* XXX: probably don't need both of these */
    struct phys_body *phys_body;
    GLfloat color[4];
    GLfloat dx, dy, dz;
    GLfloat rx, ry, rz;
    GLfloat scale;
    int (*update)(struct entity3d *e, void *data);
    void *priv;
};

void model3dtx_add_entity(struct model3dtx *txm, struct entity3d *e);
void models_render(struct list *list, struct light *light, struct matrix4f *view_mx, struct matrix4f *inv_view_mx,
                   struct matrix4f *proj_mx, struct entity3d *focus);

static inline const char *entity_name(struct entity3d *e)
{
    return txmodel_name(e->txmodel);
}

struct entity3d *entity3d_new(struct model3dtx *txm);
void entity3d_update(struct entity3d *e, void *data);
void entity3d_put(struct entity3d *e);
void entity3d_move(struct entity3d *e, float dx, float dy, float dz);
void entity3d_add_physics(struct entity3d *e, double mass, enum geom_type geom_type, double geom_off, double geom_radius);
void create_entities(struct model3dtx *txmodel);

#endif /* __CLAP_MODEL_H__ */
