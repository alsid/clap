// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#define DEBUG
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include "librarian.h"
#include "common.h"
#include "render.h"
#include "matrix.h"
#include "util.h"
#include "object.h"
#include "mesh.h"
#include "model.h"
#include "pngloader.h"
#include "physics.h"
#include "shader.h"
#include "scene.h"
#include "ui-debug.h"

/****************************************************************************
 * model3d
 * the actual rendered model
 ****************************************************************************/

static void model3d_drop(struct ref *ref)
{
    struct model3d *m = container_of(ref, struct model3d, ref);
    struct animation *an;
    struct joint *joint;
    struct pose *pose;
    int i;

    glDeleteBuffers(1, &m->vertex_obj);
    for (i = 0; i < m->nr_lods; i++)
        glDeleteBuffers(1, &m->index_obj[i]);
    if (m->norm_obj)
        glDeleteBuffers(1, &m->norm_obj);
    if (m->tex_obj)
        glDeleteBuffers(1, &m->tex_obj);
    if (m->nr_joints) {
        GL(glDeleteBuffers(1, &m->joints_obj));
        GL(glDeleteBuffers(1, &m->weights_obj));
    }
    if (gl_does_vao())
        glDeleteVertexArrays(1, &m->vao);
    /* delete gl buffers */
    ref_put(m->prog);
    trace("dropping model '%s'\n", m->name);
    darray_for_each(an, &m->anis)
        free(an->channels);
    darray_clearout(&m->anis.da);
    for (i = 0; i < m->nr_joints; i++)
        darray_clearout(&m->joints[i].children.da);
    free(m->joints);
    free(m->name);
}

DECLARE_REFCLASS(model3d);

static int load_gl_texture_buffer(struct shader_prog *p, void *buffer, int width, int height,
                                  int has_alpha, GLuint target, GLint loc, texture_t *tex)
{
    GLuint color_type = has_alpha ? GL_RGBA : GL_RGB;
    if (!buffer)
        return -EINVAL;

    //hexdump(buffer, 16);
    texture_init_target(tex, target);
    texture_filters(tex, GL_REPEAT, GL_NEAREST);
    GL(glUniform1i(loc, target - GL_TEXTURE0));

    // Bind it
    texture_load(tex, color_type, width, height, buffer);

    return 0;
}

static int model3d_add_texture(struct model3dtx *txm, const char *name)
{
    int width = 0, height = 0, has_alpha = 0, ret;
    unsigned char *buffer = fetch_png(name, &width, &height, &has_alpha);

    shader_prog_use(txm->model->prog);
    ret = load_gl_texture_buffer(txm->model->prog, buffer, width, height, has_alpha, GL_TEXTURE0,
                                 txm->model->prog->texture_map, txm->texture);
    shader_prog_done(txm->model->prog);
    free(buffer);

    dbg("loaded texture %d %s %dx%d\n", texture_id(txm->texture), name, width, height);

    return ret;
}

/* XXX: actually, make it take the decoded texture, not the png */
static int model3d_add_texture_from_buffer(struct model3dtx *txm, GLuint target, void *input, size_t length)
{
    struct shader_prog *prog = txm->model->prog;
    texture_t *targets[] = { txm->texture, txm->normals };
    GLint locs[] = { prog->texture_map, prog->normal_map };
    int width, height, has_alpha, ret;
    unsigned char *buffer;

    // dbg("## shader '%s' texture_map: %d normal_map: %d\n", prog->name, prog->texture_map, prog->normal_map);
    buffer = decode_png(input, length, &width, &height, &has_alpha);
    shader_prog_use(prog);
    ret = load_gl_texture_buffer(prog, buffer, width, height, has_alpha, target,
                                 locs[target - GL_TEXTURE0],
                                 targets[target - GL_TEXTURE0]);
    shader_prog_done(prog);
    free(buffer);
    dbg("loaded texture%d %d %dx%d\n", target - GL_TEXTURE0, texture_id(txm->texture), width, height);

    return ret;
}

static int model3dtx_make(struct ref *ref)
{
    struct model3dtx *txm = container_of(ref, struct model3dtx, ref);
    txm->texture = &txm->_texture;
    txm->normals = &txm->_normals;
    list_init(&txm->entities);
    return 0;
}

static bool model3dtx_tex_is_ext(struct model3dtx *txm)
{
    return txm->texture != &txm->_texture;
}

static void model3dtx_drop(struct ref *ref)
{
    struct model3dtx *txm = container_of(ref, struct model3dtx, ref);
    const char *name = txm->model->name;

    trace("dropping model3dtx [%s]\n", name);
    list_del(&txm->entry);
    /* XXX this is a bit XXX */
    if (model3dtx_tex_is_ext(txm))
        texture_done(txm->texture);
    else
        texture_deinit(txm->texture);
    if (txm->normals)
        texture_deinit(txm->normals);
    ref_put(txm->model);
}

DECLARE_REFCLASS2(model3dtx);

struct model3dtx *model3dtx_new(struct model3d *model, const char *name)
{
    struct model3dtx *txm = ref_new(model3dtx);

    if (!txm)
        return NULL;

    txm->model = ref_get(model);
    model3d_add_texture(txm, name);
    txm->roughness = 0.65;
    txm->metallic = 0.45;

    return txm;
}

struct model3dtx *model3dtx_new_from_buffer(struct model3d *model, void *buffer, size_t length)
{
    struct model3dtx *txm = ref_new(model3dtx);

    if (!txm)
        return NULL;

    txm->model = ref_get(model);
    model3d_add_texture_from_buffer(txm, GL_TEXTURE0, buffer, length);

    return txm;
}

struct model3dtx *model3dtx_new_from_buffers(struct model3d *model, void *tex, size_t texsz, void *norm, size_t normsz)
{
    struct model3dtx *txm = ref_new(model3dtx);

    if (!txm)
        return NULL;

    txm->model = ref_get(model);
    model3d_add_texture_from_buffer(txm, GL_TEXTURE0, tex, texsz);
    model3d_add_texture_from_buffer(txm, GL_TEXTURE1, norm, normsz);

    return txm;
}

struct model3dtx *model3dtx_new_texture(struct model3d *model, texture_t *tex)
{
    struct model3dtx *txm = ref_new(model3dtx);

    if (!txm)
        return NULL;

    txm->model = ref_get(model);
    txm->texture = tex;
    txm->external_tex = true;

    return txm;
}

static void load_gl_buffer(GLint loc, void *data, GLuint type, size_t sz, GLuint *obj,
                           GLint nr_coords, GLenum target)
{
    GL(glGenBuffers(1, obj));
    GL(glBindBuffer(target, *obj));
    GL(glBufferData(target, sz, data, GL_STATIC_DRAW));
    
    /*
     *   <attr number>
     *   <number of elements in a vector>
     *   <type>
     *   <is normalized?>
     *   <stride> for interleaving
     *   <offset>
     */
    if (loc >= 0) {
        // glEnableVertexAttribArray(loc);
        GL(glVertexAttribPointer(loc, nr_coords, type, GL_FALSE, 0, (void *)0));
    }
    
    GL(glBindBuffer(target, 0));
}

void model3d_set_name(struct model3d *m, const char *fmt, ...)
{
    va_list ap;

    free(m->name);
    va_start(ap, fmt);
    CHECK(vasprintf(&m->name, fmt, ap));
    va_end(ap);
}

static void model3d_calc_aabb(struct model3d *m, float *vx, size_t vxsz)
{
    int i;

    vxsz /= sizeof(float);
    vxsz /= 3;
    for (i = 0; i < vxsz; i += 3) {
        m->aabb[0] = min(vx[i + 0], m->aabb[0]);
        m->aabb[1] = max(vx[i + 0], m->aabb[1]);
        m->aabb[2] = min(vx[i + 1], m->aabb[2]);
        m->aabb[3] = max(vx[i + 1], m->aabb[3]);
        m->aabb[4] = min(vx[i + 2], m->aabb[4]);
        m->aabb[5] = max(vx[i + 2], m->aabb[5]);
    }

    // dbg("bounding box for '%s': %f..%f,%f..%f,%f..%f\n", m->name,
    //     m->aabb[0], m->aabb[1], m->aabb[2], m->aabb[3], m->aabb[4], m->aabb[5]);
}

float model3d_aabb_X(struct model3d *m)
{
    return fabs(m->aabb[1] - m->aabb[0]);
}

float model3d_aabb_Y(struct model3d *m)
{
    return fabs(m->aabb[3] - m->aabb[2]);
}

float model3d_aabb_Z(struct model3d *m)
{
    return fabs(m->aabb[5] - m->aabb[4]);
}

static void model3d_prepare(struct model3d *m);
static void model3d_done(struct model3d *m);

void model3d_add_tangents(struct model3d *m, float *tg, size_t tgsz)
{
    if (m->prog->tangent == -1) {
        dbg("no tangent input in program '%s'\n", m->prog->name);
        return;
    }

    shader_prog_use(m->prog);
    model3d_prepare(m);
    load_gl_buffer(m->prog->tangent, tg, GL_FLOAT, tgsz, &m->tangent_obj, 4, GL_ARRAY_BUFFER);
    model3d_done(m);
    shader_prog_done(m->prog);
}

int model3d_add_skinning(struct model3d *m, unsigned char *joints, size_t jointssz,
                         float *weights, size_t weightssz, size_t nr_joints, mat4x4 *invmxs)
{
    int v, j, jmax = 0;
    int *fjoints;

    if (jointssz != m->nr_vertices * 4 ||
        weightssz != m->nr_vertices * 4 * sizeof(float)) {
        err("wrong amount of joints or weights: %zu <> %d, %zu <> %lu\n",
            jointssz, m->nr_vertices * 4, weightssz, m->nr_vertices * 4 * sizeof(float));
        return -1;
    }

    CHECK(fjoints = calloc(m->nr_vertices * 4, sizeof(*fjoints)));

    /* incoming ivec4 joints and vec4 weights */
    for (v = 0; v < m->nr_vertices * 4; v++) {
        jmax = max(jmax, joints[v]);
        if (jmax >= 50)
            goto out_err;
        fjoints[v] = joints[v];
    }
    assert(jmax == nr_joints - 1);
    dbg("## max joints: %d\n", jmax);

    CHECK(m->joints = calloc(nr_joints, sizeof(struct model_joint)));
    for (j = 0; j < nr_joints; j++) {
        memcpy(&m->joints[j].invmx, invmxs[j], sizeof(mat4x4));
        darray_init(&m->joints[j].children);
    }

    shader_prog_use(m->prog);
    if (gl_does_vao())
        glBindVertexArray(m->vao);
    load_gl_buffer(m->prog->joints, joints, GL_FLOAT, m->nr_vertices * 4 * sizeof(float),
                   &m->joints_obj, 4, GL_ARRAY_BUFFER);
    // load_gl_buffer(m->prog->joints, joints, GL_UNSIGNED_BYTE, m->nr_vertices * 4, &m->joints_obj, 4, GL_ARRAY_BUFFER);
    load_gl_buffer(m->prog->weights, weights, GL_FLOAT, m->nr_vertices * 4 * sizeof(float),
                   &m->weights_obj, 4, GL_ARRAY_BUFFER);
    if (gl_does_vao())
        glBindVertexArray(0);
    shader_prog_done(m->prog);
    free(fjoints);

    m->nr_joints = nr_joints;
    return 0;

out_err:
    free(fjoints);
    return -1;
}

struct model3d *
model3d_new_from_vectors(const char *name, struct shader_prog *p, GLfloat *vx, size_t vxsz,
                         GLushort *idx, size_t idxsz, GLfloat *tx, size_t txsz,
                         GLfloat *norm, size_t normsz)
{
    struct model3d *m;

    m = ref_new(model3d);
    if (!m)
        return NULL;

    CHECK(m->name = strdup(name));
    m->prog = ref_get(p);
    m->cull_face = true;
    m->alpha_blend = false;
    model3d_calc_aabb(m, vx, vxsz);
    darray_init(&m->anis);

    if (gl_does_vao()) {
        GL(glGenVertexArrays(1, &m->vao));
        GL(glBindVertexArray(m->vao));
    }

    shader_prog_use(p);
    load_gl_buffer(m->prog->pos, vx, GL_FLOAT, vxsz, &m->vertex_obj, 3, GL_ARRAY_BUFFER);
    load_gl_buffer(-1, idx, GL_UNSIGNED_SHORT, idxsz, &m->index_obj[0], 0, GL_ELEMENT_ARRAY_BUFFER);
    m->nr_lods++;

    if (txsz)
        load_gl_buffer(m->prog->tex, tx, GL_FLOAT, txsz, &m->tex_obj, 2, GL_ARRAY_BUFFER);

    if (normsz)
        load_gl_buffer(m->prog->norm, norm, GL_FLOAT, normsz, &m->norm_obj, 3, GL_ARRAY_BUFFER);
    shader_prog_done(p);

    m->cur_lod = -1;
    m->nr_vertices = vxsz / sizeof(*vx) / 3; /* XXX: could be GLuint? */
    m->nr_faces[0] = idxsz / sizeof(*idx); /* XXX: could be GLuint? */
    /*dbg("created model '%s' vobj: %d iobj: %d nr_vertices: %d\n",
        m->name, m->vertex_obj, m->index_obj, m->nr_vertices);*/

    return m;
}

struct model3d *model3d_new_from_mesh(const char *name, struct shader_prog *p, struct mesh *mesh)
{
    unsigned short *lod = NULL;
    struct model3d *m;
    ssize_t nr_idx;
    int level;

    m = model3d_new_from_vectors(name, p,
                                 mesh_vx(mesh), mesh_vx_sz(mesh),
                                 mesh_idx(mesh), mesh_idx_sz(mesh),
                                 mesh_tx(mesh), mesh_tx_sz(mesh),
                                 mesh_norm(mesh), mesh_norm_sz(mesh));

    if (gl_does_vao())
        GL(glBindVertexArray(m->vao));
    shader_prog_use(m->prog);

    for (level = 0, nr_idx = mesh_nr_idx(mesh); level < LOD_MAX - 1; level++) {
        nr_idx = mesh_idx_to_lod(mesh, level, &lod, nr_idx);
        if (nr_idx < 0)
            break;
        dbg("lod%d for '%s' idx: %zd -> %zd\n", level, m->name, mesh_nr_idx(mesh), nr_idx);
        load_gl_buffer(-1, lod, GL_UNSIGNED_SHORT, nr_idx * mesh_idx_stride(mesh),
                       &m->index_obj[m->nr_lods], 0, GL_ELEMENT_ARRAY_BUFFER);
        m->nr_faces[m->nr_lods] = nr_idx;
        m->nr_lods++;
    }

    shader_prog_done(m->prog);
    if (gl_does_vao())
        GL(glBindVertexArray(0));

    return m;
}

struct model3d *
model3d_new_from_model_data(const char *name, struct shader_prog *p, struct model_data *md)
{
    struct model3d *m;
    size_t vxsz, idxsz, txsz;
    GLfloat *tx, *norm;
    GLushort *idx;

    model_data_to_vectors(md, &tx, &txsz, &norm, &vxsz, &idx, &idxsz);
    /* now the easy part */
    m = model3d_new_from_vectors(name, p, md->v, vxsz, idx, idxsz, tx, txsz, norm, vxsz);
    model_data_free(md);
    free(tx);
    free(norm);
    free(idx);

    return m;
}

static void model3d_set_lod(struct model3d *m, unsigned int lod)
{
    if (lod >= m->nr_lods)
        lod = max(0, m->nr_lods - 1);

    if (lod == m->cur_lod)
        return;

    if (m->cur_lod < 0)
        GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->index_obj[lod]));
    m->cur_lod = lod;
}

static void model3d_prepare(struct model3d *m)
{
    struct shader_prog *p = m->prog;

    if (gl_does_vao())
        GL(glBindVertexArray(m->vao));
    if (m->cur_lod >= 0)
        GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->index_obj[m->cur_lod]));
    GL(glBindBuffer(GL_ARRAY_BUFFER, m->vertex_obj));
    GL(glVertexAttribPointer(p->pos, 3, GL_FLOAT, GL_FALSE, 0, (void *)0));
    GL(glEnableVertexAttribArray(p->pos));

    if (m->norm_obj && p->norm >= 0) {
        GL(glBindBuffer(GL_ARRAY_BUFFER, m->norm_obj));
        GL(glVertexAttribPointer(p->norm, 3, GL_FLOAT, GL_FALSE, 0, (void *)0));
        GL(glEnableVertexAttribArray(p->norm));
    }
    if (m->tangent_obj && p->tangent >= 0) {
        GL(glBindBuffer(GL_ARRAY_BUFFER, m->tangent_obj));
        GL(glVertexAttribPointer(p->tangent, 4, GL_FLOAT, GL_FALSE, 0, (void *)0));
        GL(glEnableVertexAttribArray(p->tangent));
    }
    if (p->joints >= 0 && m->nr_joints && m->joints_obj >= 0) {
        GL(glBindBuffer(GL_ARRAY_BUFFER, m->joints_obj));
        GL(glVertexAttribPointer(p->joints, 4, GL_BYTE, GL_FALSE, 0, (void *)0));
        GL(glEnableVertexAttribArray(p->joints));
    }
    if (p->weights >= 0 && m->nr_joints && m->weights_obj >= 0) {
        GL(glBindBuffer(GL_ARRAY_BUFFER, m->weights_obj));
        GL(glVertexAttribPointer(p->weights, 4, GL_FLOAT, GL_FALSE, 0, (void *)0));
        GL(glEnableVertexAttribArray(p->weights));
    }
}

/* Cube and quad */
#include "primitives.c"

void model3dtx_prepare(struct model3dtx *txm)
{
    struct shader_prog *p = txm->model->prog;
    struct model3d *   m = txm->model;

    model3d_prepare(txm->model);

    if (m->tex_obj && texture_loaded(txm->texture)) {
        GL(glBindBuffer(GL_ARRAY_BUFFER, m->tex_obj));
        GL(glVertexAttribPointer(p->tex, 2, GL_FLOAT, GL_FALSE, 0, (void *)0));
        GL(glEnableVertexAttribArray(p->tex));
        GL(glActiveTexture(GL_TEXTURE0));
        GL(glBindTexture(GL_TEXTURE_2D, texture_id(txm->texture)));
        GL(glUniform1i(p->texture_map, 0));
    }

    if (txm->normals && texture_loaded(txm->normals)) {
        GL(glActiveTexture(GL_TEXTURE1));
        GL(glBindTexture(GL_TEXTURE_2D, texture_id(txm->normals)));
        GL(glUniform1i(p->normal_map, 1));
    }
}

void model3dtx_draw(struct model3dtx *txm)
{
    struct model3d *m = txm->model;

    /* GL_UNSIGNED_SHORT == typeof *indices */
    GL(glDrawElements(GL_TRIANGLES, m->nr_faces[m->cur_lod], GL_UNSIGNED_SHORT, 0));
}

static void model3d_done(struct model3d *m)
{
    struct shader_prog *p = m->prog;

    GL(glDisableVertexAttribArray(p->pos));
    if (m->norm_obj)
        GL(glDisableVertexAttribArray(p->norm));
    if (m->tangent_obj)
        GL(glDisableVertexAttribArray(p->tangent));
    if (m->nr_joints) {
        GL(glDisableVertexAttribArray(p->joints));
        GL(glDisableVertexAttribArray(p->weights));
    }

    /* both need to be bound for glDrawElements() */
    GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    GL(glBindBuffer(GL_ARRAY_BUFFER, 0));
    if (gl_does_vao())
        GL(glBindVertexArray(0));
    m->cur_lod = -1;
}

void model3dtx_done(struct model3dtx *txm)
{
    struct shader_prog *p = txm->model->prog;

    if (txm->model->tex_obj/* && txm->texture_id*/) {
        GL(glDisableVertexAttribArray(p->tex));
        GL(glActiveTexture(GL_TEXTURE0));
        GL(glBindTexture(GL_TEXTURE_2D, 0));
    }
    if (txm->normals) {
        GL(glActiveTexture(GL_TEXTURE1));
        GL(glBindTexture(GL_TEXTURE_2D, 0));
    }

    model3d_done(txm->model);
}

static int fbo_create(void)
{
    unsigned int fbo;

    GL(glGenFramebuffers(1, &fbo));
    GL(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    return fbo;
}

static void fbo_texture_init(struct fbo *fbo)
{
    texture_init(&fbo->tex);
    texture_filters(&fbo->tex, GL_CLAMP_TO_EDGE, GL_LINEAR);
    texture_fbo(&fbo->tex, GL_COLOR_ATTACHMENT0, GL_RGBA, fbo->width, fbo->height);
}

static int fbo_depth_texture(struct fbo *fbo)
{
    int tex;

    texture_init(&fbo->depth);
    texture_filters(&fbo->tex, GL_CLAMP_TO_EDGE, GL_LINEAR);
    texture_fbo(&fbo->tex, GL_DEPTH_ATTACHMENT, GL_DEPTH_COMPONENT, fbo->width, fbo->height);

    return tex;
}

static int fbo_depth_buffer(struct fbo *fbo)
{
    unsigned int buf;

    GL(glGenRenderbuffers(1, &buf));
    GL(glBindRenderbuffer(GL_RENDERBUFFER, buf));
    GL(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, fbo->width, fbo->height));
    GL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, buf));
    GL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    return buf;
}

void fbo_resize(struct fbo *fbo, int width, int height)
{
    if (!fbo)
        return;
    fbo->width = width;
    fbo->height = height;
    GL(glFinish());
    texture_resize(&fbo->tex, width, height);
    texture_resize(&fbo->depth, width, height);

    if (fbo->depth_buf) {
        GL(glBindRenderbuffer(GL_RENDERBUFFER, fbo->depth_buf));
        GL(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, fbo->width, fbo->height));
        GL(glBindRenderbuffer(GL_RENDERBUFFER, 0));
    }
}

void fbo_prepare(struct fbo *fbo)
{
    GL(glBindFramebuffer(GL_FRAMEBUFFER, fbo->fbo));
    GL(glViewport(0, 0, fbo->width, fbo->height));
}

void fbo_done(struct fbo *fbo, int width, int height)
{
    GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GL(glViewport(0, 0, width, height));
}

static void fbo_drop(struct ref *ref)
{
    struct fbo *fbo = container_of(ref, struct fbo, ref);

    // dbg("dropping FBO %d: %d/%d/%d\n", fbo->fbo, fbo->tex, fbo->depth_tex, fbo->depth_buf);
    GL(glDeleteFramebuffers(1, &fbo->fbo));
    if (!fbo->retain_tex)
        texture_deinit(&fbo->tex);
    texture_done(&fbo->depth);
    if (fbo->depth_buf >= 0)
        GL(glDeleteRenderbuffers(1, (GLuint *)&fbo->depth_buf));
    // ref_free(fbo);
}

DECLARE_REFCLASS(fbo);

struct fbo *fbo_new(int width, int height)
{
    struct fbo *fbo;
    int ret;

    CHECK(fbo = ref_new(fbo));
    fbo->depth_buf = -1;
    fbo->width = width;
    fbo->height = height;
    fbo->fbo = fbo_create();
    fbo_texture_init(fbo);
    //fbo->depth_tex = fbo_depth_texture(fbo);
    fbo->depth_buf = fbo_depth_buffer(fbo);
    ret = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (ret != GL_FRAMEBUFFER_COMPLETE)
        dbg("## framebuffer status: %d\n", ret);
    GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

    return fbo;
}

static void animation_destroy(struct animation *an)
{
    free(an->channels);
    ref_put(an->model);
}

struct animation *animation_new(struct model3d *model, const char *name, unsigned int nr_channels)
{
    struct animation *an;

    CHECK(an = darray_add(&model->anis.da));
    an->name = name;
    an->model = model;
    an->nr_channels = nr_channels;
    an->cur_channel = 0;
    CHECK(an->channels = calloc(an->nr_channels, sizeof(struct channel)));

    return an;
}

void animation_add_channel(struct animation *an, size_t frames, float *time, float *data,
                           size_t data_stride, unsigned int target, unsigned int path)
{
    if (an->cur_channel == an->nr_channels)
        return;

    an->channels[an->cur_channel].time = memdup(time, sizeof(float) * frames);
    an->channels[an->cur_channel].data = memdup(data, data_stride * frames);
    an->channels[an->cur_channel].nr = frames;
    an->channels[an->cur_channel].stride = data_stride;
    an->channels[an->cur_channel].target = target;
    an->channels[an->cur_channel].path = path;
    an->cur_channel++;

    an->time_end = max(an->time_end, time[frames - 1]/* + time[1] - time[0]*/);
}

void models_render(struct mq *mq, struct light *light, struct camera *camera,
                   struct matrix4f *proj_mx, struct entity3d *focus,
                   unsigned long *count)
{
    struct entity3d *e;
    struct shader_prog *prog = NULL;
    struct model3d *model;
    struct model3dtx *txmodel;
    struct matrix4f *view_mx, *inv_view_mx;
    unsigned long nr_txms = 0, nr_ents = 0;

    if (camera) {
        view_mx = camera->view_mx;
        inv_view_mx = camera->inv_view_mx;
    }

    list_for_each_entry(txmodel, &mq->txmodels, entry) {
        err_on(list_empty(&txmodel->entities), "txm '%s' has no entities\n",
               txmodel_name(txmodel));
        model = txmodel->model;
        model->cur_lod = 0;
        /* XXX: model-specific draw method */
        if (model->cull_face) {
            GL(glEnable(GL_CULL_FACE));
            GL(glCullFace(GL_BACK));
        } else {
            GL(glDisable(GL_CULL_FACE));
        }
        /* XXX: only for UIs */
        if (model->alpha_blend) {
            GL(glEnable(GL_BLEND));
            GL(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        } else {
            GL(glDisable(GL_BLEND));
        }
        //dbg("rendering model '%s'\n", model->name);
        if (model->prog != prog) {
            if (prog)
                shader_prog_done(prog);

            prog = model->prog;
            shader_prog_use(prog);
            trace("rendering model '%s' using '%s'\n", model->name, prog->name);

            if (light && prog->data.lightp >= 0 && prog->data.lightc >= 0) {
                GL(glUniform3fv(prog->data.lightp, 1, light->pos));
                GL(glUniform3fv(prog->data.lightc, 1, light->color));
            }

            if (view_mx && prog->data.viewmx >= 0)
                /* View matrix is the same for all entities and models */
                GL(glUniformMatrix4fv(prog->data.viewmx, 1, GL_FALSE, view_mx->cell));
            if (inv_view_mx && prog->data.inv_viewmx >= 0)
                GL(glUniformMatrix4fv(prog->data.inv_viewmx, 1, GL_FALSE, inv_view_mx->cell));

            /* Projection matrix is the same for everything, but changes on resize */
            if (proj_mx && prog->data.projmx >= 0)
                GL(glUniformMatrix4fv(prog->data.projmx, 1, GL_FALSE, proj_mx->cell));
        }

        model3dtx_prepare(txmodel);
        if (prog->data.use_normals >= 0 && txmodel->normals)
            GL(glUniform1f(prog->data.use_normals, texture_id(txmodel->normals) ? 1.0 : 0.0));

        if (prog->data.shine_damper >= 0 && prog->data.reflectivity >= 0) {
            GL(glUniform1f(prog->data.shine_damper, txmodel->roughness));
            GL(glUniform1f(prog->data.reflectivity, txmodel->metallic));
        }

        list_for_each_entry (e, &txmodel->entities, entry) {
            float hc[] = { 0.7, 0.7, 0.0, 1.0 }, nohc[] = { 0.0, 0.0, 0.0, 0.0 };
            if (!e->visible) {
                //dbg("skipping element of '%s'\n", entity_name(e));
                continue;
            }

            dbg_on(!e->visible, "rendering an invisible entity!\n");

            if (camera) {
                vec3 dist = { e->dx, e->dy, e->dz };
                unsigned int lod;

                vec3_sub(dist, dist, camera->ch->pos);
                lod = vec3_len(dist) / 80;
                model3d_set_lod(model, lod);
            }
#ifndef EGL_EGL_PROTOTYPES
            if (focus == e) {
                GL(glPolygonMode(GL_FRONT_AND_BACK, GL_LINE));
            } else {
                GL(glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
            }
#endif
            if (prog->data.color >= 0)
                GL(glUniform4fv(prog->data.color, 1, e->color));
            if (prog->data.colorpt >= 0)
                GL(glUniform1f(prog->data.colorpt, 0.5 * e->color_pt));
            if (focus && prog->data.highlight >= 0)
                GL(glUniform4fv(prog->data.highlight, 1, focus == e ? (GLfloat *)hc : (GLfloat *)nohc));

            if (model->nr_joints && model->anis.da.nr_el && prog->data.joint_transforms >= 0) {
                GL(glUniform1f(prog->data.use_skinning, 1.0));
                GL(glUniformMatrix4fv(prog->data.joint_transforms, model->nr_joints,
                                      GL_FALSE, (GLfloat *)e->joint_transforms));
            } else {
                GL(glUniform1f(prog->data.use_skinning, 0.0));
            }
            if (prog->data.ray >= 0) {
                vec3 ray = { 0, 0, 0 };
                if (focus) {
                    ray[0] = focus->dx;
                    ray[1] = focus->dz;
                    ray[2] = 1.0;
                }
                GL(glUniform3fv(prog->data.ray, 1, ray));
            }
            if (prog->data.transmx >= 0) {
                /* Transformation matrix is different for each entity */
                GL(glUniformMatrix4fv(prog->data.transmx, 1, GL_FALSE, (GLfloat *)e->mx));
            }

            model3dtx_draw(txmodel);
            nr_ents++;
        }
        model3dtx_done(txmodel);
        nr_txms++;
        //dbg("RENDERED model '%s': %lu\n", txmodel->model->name, nr_ents);
    }

    //dbg("RENDERED: %lu/%lu\n", nr_txms, nr_ents);
    if (count)
        *count = nr_txms;
    if (prog)
        shader_prog_done(prog);
}

static void model_obj_loaded(struct lib_handle *h, void *data)
{
    struct shader_prog *prog;
    struct scene *s = data;
    struct model_data * md;
    struct model3d *m;

    prog = shader_prog_find(s->prog, "model");
    dbg("loaded '%s' %p %zu %d\n", h->name, h->buf, h->size, h->state);
    if (!h->buf)
        return;

    md = model_data_new_from_obj(h->buf, h->size);
    if (!md)
        return;

    m = model3d_new_from_model_data(h->name, prog, md);
    ref_put(prog); /* matches shader_prog_find() above */
    ref_put(h);

    //scene_add_model(s, m);
    s->_model = m;
}

static void model_bin_vec_loaded(struct lib_handle *h, void *data)
{
    struct shader_prog *prog;
    struct scene *s = data;
    struct model3d *m;
    struct bin_vec_header *hdr = h->buf;
    float *vx, *tx, *norm;
    unsigned short *idx;

    prog = shader_prog_find(s->prog, "model");

    dbg("loaded '%s' nr_vertices: %" PRIu64 "\n", h->name, hdr->nr_vertices);
    vx   = h->buf + sizeof(*hdr);
    tx   = h->buf + sizeof(*hdr) + hdr->vxsz;
    norm = h->buf + sizeof(*hdr) + hdr->vxsz + hdr->txsz;
    idx  = h->buf + sizeof(*hdr) + hdr->vxsz + hdr->txsz + hdr->vxsz;
    m = model3d_new_from_vectors(h->name, prog, vx, hdr->vxsz, idx, hdr->idxsz, tx, hdr->txsz, norm, hdr->vxsz);
    ref_put(prog);  /* matches shader_prog_find() above */
    ref_put(h);

    //scene_add_model(s, m);
    s->_model = m;
}

struct lib_handle *lib_request_obj(const char *name, struct scene *scene)
{
    struct lib_handle *lh;
    lh = lib_request(RES_ASSET, name, model_obj_loaded, scene);
    return lh;
}

struct lib_handle *lib_request_bin_vec(const char *name, struct scene *scene)
{
    struct lib_handle *lh;
    lh = lib_request(RES_ASSET, name, model_bin_vec_loaded, scene);
    return lh;
}

/*int model3d_new(const char *name)
{
    size_t size;
    char *buf;
    int ret;

    ret = lib_read_file(RES_ASSET, name, &buf, &size);
    if (ret)
        return ret;

    return 0;
}*/

/****************************************************************************
 * entity3d
 * instance of the model3d
 ****************************************************************************/

float entity3d_aabb_X(struct entity3d *e)
{
    return model3d_aabb_X(e->txmodel->model) * e->scale;
}

float entity3d_aabb_Y(struct entity3d *e)
{
    return model3d_aabb_Y(e->txmodel->model) * e->scale;
}

float entity3d_aabb_Z(struct entity3d *e)
{
    return model3d_aabb_Z(e->txmodel->model) * e->scale;
}

void entity3d_aabb_center(struct entity3d *e, vec3 center)
{
    center[0] = entity3d_aabb_X(e) + e->dx;
    center[1] = entity3d_aabb_Y(e) + e->dy;
    center[2] = entity3d_aabb_Z(e) + e->dz;
}

void model3d_skeleton_add(struct model3d *model, int joint, int parent)
{}

static void channel_time_to_idx(struct channel *chan, float time, int start, int *prev, int *next)
{
    int i;

    if (time < chan->time[0])
        goto tail;

    for (i = start; i < chan->nr && time > chan->time[i]; i++)
        ;
    if (i == chan->nr)
        goto tail;

    *prev = max(i - 1, 0);
    *next = *prev + 1;
    return;

tail:
    *prev = chan->nr - 1;
    *next = 0;
}

static float quat_dot(quat a, quat b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void vec3_interp(vec3 res, vec3 a, vec3 b, float fac)
{
    float rfac = 1.f - fac;

    res[0] = fac * a[0] + rfac * b[0];
    res[1] = fac * a[1] + rfac * b[1];
    res[2] = fac * a[2] + rfac * b[2];
}

static void quat_interp(quat res, quat a, quat b, float fac)
{
    float dot = quat_dot(a, b);
    float rfac = 1.f - fac;

    if (dot < 0) {
        res[3] = rfac * a[3] - fac * b[3];
        res[0] = rfac * a[0] - fac * b[0];
        res[1] = rfac * a[1] - fac * b[1];
        res[2] = rfac * a[2] - fac * b[2];
    } else {
        res[3] = rfac * a[3] + fac * b[3];
        res[0] = rfac * a[0] + fac * b[0];
        res[1] = rfac * a[1] + fac * b[1];
        res[2] = rfac * a[2] + fac * b[2];
    }
}

static void channel_transform(struct entity3d *e, struct channel *chan, float time)
{
    struct model3d *model = e->txmodel->model;
    void *p_data, *n_data;
    struct joint *joint = &e->joints[chan->target];
    float p_time, n_time, delta, fac;
    int prev, next;
    mat4x4 rot;

    channel_time_to_idx(chan, time, joint->off[chan->path], &prev, &next);
    joint->off[chan->path] = min(prev, next);

    p_time = chan->time[prev];
    n_time = chan->time[next];
    delta = fabs(n_time - p_time);
    if (p_time > n_time)
        fac = (delta - fmodf(n_time - time, delta)) / (p_time - n_time);
    else
        fac = (time - p_time) / (n_time - p_time);

    p_data = (void *)chan->data + prev * chan->stride;
    n_data = (void *)chan->data + next * chan->stride;

    switch (chan->path) {
    case PATH_TRANSLATION: {
        vec3 *p_pos = p_data, *n_pos = n_data, interp;

        vec3_interp(interp, *p_pos, *n_pos, fac);
        memcpy(joint->translation, interp, chan->stride);
        break;
    }
    case PATH_ROTATION: {
        quat *n_rot = n_data, *p_rot = p_data, interp;

        quat_interp(interp, *p_rot, *n_rot, fac);
        memcpy(joint->rotation, interp, chan->stride);
        break;
    }
    case PATH_SCALE:
        memcpy(joint->scale, p_data, chan->stride);
        break;
    }
}

static void channels_transform(struct entity3d *e, struct animation *an, float time)
{
    int ch;

    for (ch = 0; ch < an->nr_channels; ch++)
        channel_transform(e, &an->channels[ch], time);
}

static void one_joint_transform(struct entity3d *e, int joint, int parent)
{
    struct model3d *model = e->txmodel->model;
    mat4x4 *parentjt = NULL, R, *invglobal;
    struct joint *j = &e->joints[joint];
    mat4x4 *jt, T;
    int *child;

    /* inverse bind matrix */
    invglobal = &model->joints[joint].invmx;

    jt = &j->global;
    mat4x4_identity(*jt);

    if (parent >= 0)
        parentjt = &e->joints[parent].global;
    if (parentjt)
        mat4x4_mul(*jt, *parentjt, *jt);

    mat4x4_translate(T,
                     j->translation[0],
                     j->translation[1],
                     j->translation[2]);
    mat4x4_mul(*jt, *jt, T);
    mat4x4_from_quat(R, j->rotation);
    mat4x4_mul(*jt, *jt, R);
    mat4x4_scale_aniso(*jt, *jt,
                       j->scale[0],
                       j->scale[1],
                       j->scale[2]);

    /*
     * jt = parentjt * T * R * S
     * joint_transform = invglobal * jt
     */
    mat4x4_mul(e->joint_transforms[joint], *jt, *invglobal);

    darray_for_each(child, &model->joints[joint].children)
        one_joint_transform(e, *child, joint);
}

static void animation_start(struct entity3d *e, int ani)
{
    struct model3d *model = e->txmodel->model;
    struct animation *an;
    struct channel *chan;
    int j, ch;

    if (ani >= model->anis.da.nr_el)
        ani %= model->anis.da.nr_el;
    an = &model->anis.x[ani];
    for (ch = 0; ch < an->nr_channels; ch++) {
        chan = &an->channels[ch];
        e->joints[chan->target].off[chan->path] = 0;
    }
    e->ani_frame = 0;
}

#define FRAMERATE 48
static void animated_update(struct entity3d *e)
{
    struct model3d *model = e->txmodel->model;
    struct animation *an = &model->anis.x[e->animation];
    int i;

    for (i = 0; i < model->nr_joints; i++) {
        memset(e->joints[i].translation, 0, sizeof(vec3));
        e->joints[i].rotation[0] = 0;
        e->joints[i].rotation[1] = 0;
        e->joints[i].rotation[2] = 0;
        e->joints[i].rotation[3] = 1;
        e->joints[i].scale[0] = 1;
        e->joints[i].scale[1] = 1;
        e->joints[i].scale[2] = 1;
    }
    channels_transform(e, an, (float)e->ani_frame / (float)FRAMERATE);
    one_joint_transform(e, 0, -1);

    if (++e->ani_frame >= an->time_end * FRAMERATE)
        animation_start(e, e->animation + 1);
}

static int default_update(struct entity3d *e, void *data)
{
    struct scene *scene = data;

    mat4x4_identity(e->mx->m);
    // phys_body_update(e);
    // if (e->dy <= scene->limbo_height && e->phys_body) {
    //     phys_body_done(e->phys_body);
    //     e->phys_body = NULL;
    //     dbg("entity '%s' lost its physics\n", entity_name(e));
    // }
    //struct scene *scene = data;
    mat4x4_translate_in_place(e->mx->m, e->dx, e->dy, e->dz);
    mat4x4_rotate_X(e->mx->m, e->mx->m, e->rx);
    mat4x4_rotate_Y(e->mx->m, e->mx->m, e->ry);
    mat4x4_rotate_Z(e->mx->m, e->mx->m, e->rz);
    mat4x4_scale_aniso(e->mx->m, e->mx->m, e->scale, e->scale, e->scale);
    if (entity_animated(e))
        animated_update(e);

    return 0;
}

void entity3d_reset(struct entity3d *e)
{
    default_update(e, NULL);
}

static void entity3d_drop(struct ref *ref)
{
    struct entity3d *e = container_of(ref, struct entity3d, ref);
    trace("dropping entity3d\n");
    list_del(&e->entry);
    ref_put(e->txmodel);
    
    if (e->phys_body) {
        phys_body_done(e->phys_body);
        e->phys_body = NULL;
    }
    free(e->joints);
    free(e->joint_transforms);
    free(e->collision_vx);
    free(e->collision_idx);
    free(e->mx);
}

DECLARE_REFCLASS(entity3d);

struct entity3d *entity3d_new(struct model3dtx *txm)
{
    struct model3d *model = txm->model;
    struct entity3d *e;

    /*
     * XXX this is ef'ed up
     * The reason is that mq_release() iterates txms' entities list
     * and when the last entity drops, the txm would disappear,
     * making it impossible to continue. Fixing that.
     */
    // ref_shared(txm);
    e = ref_new(entity3d);
    if (!e)
        return NULL;

    e->txmodel = ref_get(txm);
    e->mx = mx_new();
    e->update  = default_update;
    if (model->anis.da.nr_el) {
        CHECK(e->joints = calloc(model->nr_joints, sizeof(*e->joints)));
        CHECK(e->joint_transforms = calloc(model->nr_joints, sizeof(mat4x4)));
    }

    return e;
}

/* XXX: static inline? via macro? */
void entity3d_put(struct entity3d *e)
{
    ref_put(e);
}

void entity3d_update(struct entity3d *e, void *data)
{
    if (e->update)
        e->update(e, data);
}

void entity3d_add_physics(struct entity3d *e, double mass, int class, int type, double geom_off, double geom_radius, double geom_length)
{
    struct model3d *m = e->txmodel->model;

    e->phys_body = phys_body_new(phys, e, class, type, mass);
    /* we calculate geom_off instead */
    geom_off = e->phys_body->yoffset;
    if (geom_off) {
        dVector3 org = { e->dx, e->dx, e->dy + geom_off, e->dz }, dir = { 0, -1.0, 0 };
        e->phys_body->yoffset = geom_off;
        // e->phys_body->ray = dCreateRay(phys->space, geom_off);
        // dGeomRaySet(e->phys_body->ray, org[0], org[1], org[2], dir[0], dir[1], dir[2]);
        // dGeomSetData(e->phys_body->ray, e);
        // dbg("RAY('%s') (%f,%f,%f) -> (%f,%f,%f)\n", entity_name(e),
        //     org[0], org[1], org[2], dir[0], dir[1], dir[2]);
    }
}

void entity3d_position(struct entity3d *e, float x, float y, float z)
{
    e->dx = x;
    e->dy = y;
    e->dz = z;
    if (e->phys_body) {
        dBodySetPosition(e->phys_body->body, e->dx, e->dy + e->phys_body->yoffset, e->dz);
        // dBodySetLinearVel(e->phys_body->body, 0, 0, 0);
        // dBodySetAngularVel(e->phys_body->body, 0, 0, 0);
        //dBodyDisable(e->phys_body->body);
    }
}

void entity3d_move(struct entity3d *e, float dx, float dy, float dz)
{
    entity3d_position(e, e->dx + dx, e->dy + dy, e->dz + dz);
}

void model3dtx_add_entity(struct model3dtx *txm, struct entity3d *e)
{
    list_append(&txm->entities, &e->entry);
}

void create_entities(struct model3dtx *txmodel)
{
    long i;

    return;
    for (i = 0; i < 16; i++) {
        struct entity3d *e = entity3d_new(txmodel);
        float a = 0, b = 0, c = 0;

        if (!e)
            return;

        a = (float)rand()*20 / (float)RAND_MAX;
        b = (float)rand()*20 / (float)RAND_MAX;
        c = (float)rand()*20 / (float)RAND_MAX;
        a *= i & 1 ? 1 : -1;
        b *= i & 2 ? 1 : -1;
        c *= i & 4 ? 1 : -1;
        //msg("[%f,%f,%f]\n", a, b, c);
        //mat4x4_identity(e->base_mx->m);
        //mat4x4_translate_in_place(e->base_mx->m, a, b, c);
        e->scale = 1.0;
        //mat4x4_scale_aniso(e->base_mx->m, e->base_mx->m, e->scale, e->scale, e->scale);
        /*mat4x4_scale(e->base_mx->m, e->base_mx->m, 0.05);*/

        e->dx      = a;
        e->dy      = b;
        e->dz      = c;
        //e->mx      = e->base_mx;
        default_update(e, NULL);
        e->update  = default_update;
        e->priv    = (void *)i;
        e->visible = 1;
        model3dtx_add_entity(txmodel, e);
    }
}

void mq_init(struct mq *mq, void *priv)
{
    list_init(&mq->txmodels);
    mq->priv = priv;
}

void mq_release(struct mq *mq)
{
    struct model3dtx *txmodel;
    struct entity3d *ent;

    do {
        bool done = false;

        txmodel = list_first_entry(&mq->txmodels, struct model3dtx, entry);

        do {
            if (list_empty(&txmodel->entities))
                break;

            ent = list_first_entry(&txmodel->entities, struct entity3d, entry);
            if (ent == list_last_entry(&txmodel->entities, struct entity3d, entry)) {
                done = true;
                ref_only(txmodel);
            }

            if (ent->destroy)
                ent->destroy(ent);
            else
                ref_put(ent);
        } while (!done);
    } while (!list_empty(&mq->txmodels));
}

void mq_for_each(struct mq *mq, void (*cb)(struct entity3d *, void *), void *data)
{
    struct model3dtx *txmodel, *ittxm;
    struct entity3d *ent, *itent;

    list_for_each_entry(txmodel, &mq->txmodels, entry) {
        list_for_each_entry_iter(ent, itent, &txmodel->entities, entry) {
            cb(ent, data);
        }
    }
}

void mq_update(struct mq *mq)
{
    mq_for_each(mq, entity3d_update, mq->priv);
}

struct model3dtx *mq_model_first(struct mq *mq)
{
    return list_first_entry(&mq->txmodels, struct model3dtx, entry);
}

struct model3dtx *mq_model_last(struct mq *mq)
{
    return list_last_entry(&mq->txmodels, struct model3dtx, entry);
}

void mq_add_model(struct mq *mq, struct model3dtx *txmodel)
{
    txmodel = ref_pass(txmodel);
    list_append(&mq->txmodels, &txmodel->entry);
}

void mq_add_model_tail(struct mq *mq, struct model3dtx *txmodel)
{
    txmodel = ref_pass(txmodel);
    list_prepend(&mq->txmodels, &txmodel->entry);
}

struct model3dtx *mq_nonempty_txm_next(struct mq *mq, struct model3dtx *txm, bool fwd)
{
    struct model3dtx *first_txm = mq_model_first(mq);
    struct model3dtx *last_txm = mq_model_last(mq);
    struct model3dtx *next_txm;

    if (list_empty(&mq->txmodels))
        return NULL;

    if (!txm)
        txm = fwd ? last_txm : first_txm;
    next_txm = txm;

    do {
        if (next_txm == first_txm)
            next_txm = last_txm;
        else if (next_txm == last_txm)
            next_txm = first_txm;
        else
            next_txm = fwd ?
                list_next_entry(next_txm, entry) :
                list_prev_entry(next_txm, entry);
    } while (list_empty(&next_txm->entities) && next_txm != txm);

    return list_empty(&next_txm->entities) ? NULL : next_txm;
}
