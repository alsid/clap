// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include "base64.h"
#include "common.h"
#include "json.h"
#include "librarian.h"
#include "model.h"
#include "object.h"
#include "pngloader.h"
#include "scene.h"
#include "shader.h"

#define DATA_URI "data:application/octet-stream;base64,"

struct gltf_bufview {
    unsigned int buffer;
    size_t       offset;
    size_t       length;
};

enum {
    T_VEC2 = 0,
    T_VEC3,
    T_VEC4,
    T_MAT4,
    T_SCALAR,
};

static const char *types[] = {
    [T_VEC2]   = "VEC2",
    [T_VEC3]   = "VEC3",
    [T_VEC4]   = "VEC4",
    [T_MAT4]   = "MAT4",
    [T_SCALAR] = "SCALAR"
};

static const size_t typesz[] = {
    [T_VEC2]   = 2,
    [T_VEC3]   = 3,
    [T_VEC4]   = 4,
    [T_MAT4]   = 16,
    [T_SCALAR] = 1,
};

/* these correspond to GL_* macros */
enum {
    COMP_BYTE           = 0x1400,
    COMP_UNSIGNED_BYTE  = 0x1401,
    COMP_SHORT          = 0x1402,
    COMP_UNSIGNED_SHORT = 0x1403,
    COMP_INT            = 0x1404,
    COMP_UNSIGNED_INT   = 0x1405,
    COMP_FLOAT          = 0x1406,
    COMP_2_BYTES        = 0x1407,
    COMP_3_BYTES        = 0x1408,
    COMP_4_BYTES        = 0x1409,
    COMP_DOUBLE         = 0x140A,
};

/* XXX: this is wasteful for a lookup table, use a switch instead */
static const size_t comp_sizes[] = {
    [COMP_BYTE]           = 1,
    [COMP_UNSIGNED_BYTE]  = 1,
    [COMP_SHORT]          = 2,
    [COMP_UNSIGNED_SHORT] = 2,
    [COMP_INT]            = 4,
    [COMP_UNSIGNED_INT]   = 4,
    [COMP_FLOAT]          = 4,
    [COMP_2_BYTES]        = 2,
    [COMP_3_BYTES]        = 3,
    [COMP_4_BYTES]        = 4,
    [COMP_DOUBLE]         = 8,
};

static const char *comp_types[] = {
    [COMP_BYTE]           = "BYTE",
    [COMP_UNSIGNED_BYTE]  = "UNSIGNED_BYTE",
    [COMP_SHORT]          = "SHORT",
    [COMP_UNSIGNED_SHORT] = "UNSIGNED_SHORT",
    [COMP_INT]            = "INT",
    [COMP_UNSIGNED_INT]   = "UNSIGNED_INT",
    [COMP_FLOAT]          = "FLOAT",
    [COMP_2_BYTES]        = "2_BYTES",
    [COMP_3_BYTES]        = "3_BYTES",
    [COMP_4_BYTES]        = "4_BYTES",
    [COMP_DOUBLE]         = "DOUBLE",
};

struct gltf_accessor {
    unsigned int bufview;
    unsigned int comptype;
    unsigned int count;
    unsigned int type;
    size_t       offset;
};

struct gltf_node {
    const char  *name;
    quat        rotation;
    vec3        scale;
    vec3        translation;
    struct list children;
    struct list entry;
    int         mesh;
    int         skin;
    unsigned int nr_children;
    int         *ch_arr;
};

struct gltf_skin {
    int         invmxs;
    const char  *name;
    int         *joints;
    int         *nodes;
    unsigned int nr_joints;
};

struct gltf_mesh {
    const char  *name;
    int         indices;
    int         material;
    int         POSITION;
    int         NORMAL;
    int         TEXCOORD_0;
    int         TANGENT;
    int         COLOR_0;
    int         JOINTS_0;
    int         WEIGHTS_0;
};

static void gltf_mesh_init(struct gltf_mesh *mesh, const char *name,
                           int indices, int material)
{
    mesh->name = strdup(name);
    mesh->indices = indices;
    mesh->material = material;
    mesh->POSITION = -1;
    mesh->NORMAL = -1;
    mesh->TEXCOORD_0 = -1;
    mesh->TANGENT = -1;
    mesh->COLOR_0 = -1;
    mesh->JOINTS_0 = -1;
    mesh->WEIGHTS_0 = -1;
}

enum {
    I_STEP = 0,
    I_LINEAR,
    I_CUBICSPLINE,
    I_NONE,
};

static const char *interps[] = {
    [I_STEP]        = "STEP",
    [I_LINEAR]      = "LINEAR",
    [I_CUBICSPLINE] = "CUBICSPLINE",
    [I_NONE]        = "NONE"
};

struct gltf_sampler {
    int    input;
    int    output;
    int    interp;
};

enum chan_path {
    PATH_TRANSLATION = 0,
    PATH_ROTATION,
    PATH_SCALE,
    PATH_NONE,
};

static const char *paths[] = {
    [PATH_TRANSLATION] = "translation",
    [PATH_ROTATION]    = "rotation",
    [PATH_SCALE]       = "scale",
    [PATH_NONE]        = "none",
};

struct gltf_channel {
    int             sampler;
    int             node;
    enum chan_path  path;
};

struct gltf_animation {
    const char                  *name;
    darray(struct gltf_sampler, samplers);
    darray(struct gltf_channel, channels);
};

struct gltf_material {
    int    base_tex;
    int    normal_tex;
    double metallic;
    double roughness;
};

struct gltf_data {
    struct scene *scene;
    darray(void *,                buffers);
    darray(struct gltf_bufview,   bufvws);
    darray(struct gltf_accessor,  accrs);
    darray(struct gltf_mesh,      meshes);
    darray(struct gltf_material,  mats);
    darray(struct gltf_node,      nodes);
    darray(struct gltf_animation, anis);
    darray(struct gltf_skin,      skins);
    // struct darray        texs;
    unsigned int         *imgs;
    unsigned int         *texs;
    int                  root_node;
    unsigned int nr_imgs;
    unsigned int nr_texs;
    unsigned int texid;
};

void gltf_free(struct gltf_data *gd)
{
    struct gltf_skin *skin;
    struct gltf_node *node;
    int i, j;

    for (i = 0; i < gd->anis.da.nr_el; i++) {
        struct gltf_animation *ani = &gd->anis.x[i];
        free((void *)ani->name);

        darray_clearout(&ani->channels.da);
        darray_clearout(&ani->samplers.da);
    }

    for (i = 0; i < gd->buffers.da.nr_el; i++)
        free(gd->buffers.x[i]);

    for (i = 0; i < gd->meshes.da.nr_el; i++)
        free((void *)gd->meshes.x[i].name);
    darray_for_each(node, &gd->nodes) {
        free((void *)node->name);
        free(node->ch_arr);
    }
    darray_for_each(skin, &gd->skins) {
        free((void *)skin->joints);
        free(skin->nodes);
        free((void *)skin->name);
    }
    darray_clearout(&gd->buffers.da);
    darray_clearout(&gd->bufvws.da);
    darray_clearout(&gd->meshes.da);
    darray_clearout(&gd->accrs.da);
    darray_clearout(&gd->nodes.da);
    darray_clearout(&gd->mats.da);
    darray_clearout(&gd->anis.da);
    darray_clearout(&gd->skins.da);
    free(gd->imgs);
    free(gd->texs);
    free(gd);
}

int gltf_get_meshes(struct gltf_data *gd)
{
    return gd->meshes.da.nr_el;
}

int gltf_mesh_by_name(struct gltf_data *gd, const char *name)
{
    int i;

    for (i = 0; i < gd->meshes.da.nr_el; i++)
        if (!strcasecmp(name, gd->meshes.x[i].name))
            return i;//&gd->meshes.x[i];

    return -1;
}

struct gltf_mesh *gltf_mesh(struct gltf_data *gd, int mesh)
{
    return &gd->meshes.x[mesh];
}

const char *gltf_mesh_name(struct gltf_data *gd, int mesh)
{
    struct gltf_mesh *m = gltf_mesh(gd, mesh);

    if (!m)
        return NULL;

    return m->name;
}

struct gltf_accessor *gltf_accessor(struct gltf_data *gd, int accr)
{
    struct gltf_accessor *ga = &gd->accrs.x[accr];

    if (!ga)
        return NULL;

    return ga;
}

size_t gltf_accessor_stride(struct gltf_data *gd, int accr)
{
    struct gltf_accessor *ga = gltf_accessor(gd, accr);

    return typesz[ga->type] * comp_sizes[ga->comptype];
}

size_t gltf_accessor_nr(struct gltf_data *gd, int accr)
{
    struct gltf_accessor *ga = gltf_accessor(gd, accr);

    return ga->count;
}

struct gltf_bufview *gltf_bufview_accr(struct gltf_data *gd, int accr)
{
    struct gltf_accessor *ga = &gd->accrs.x[accr];

    if (!ga)
        return NULL;

    return &gd->bufvws.x[ga->bufview];
}

struct gltf_bufview *gltf_bufview_tex(struct gltf_data *gd, int tex)
{
    return &gd->bufvws.x[gd->imgs[gd->texs[tex]]];
}

void *gltf_accessor_buf(struct gltf_data *gd, int accr)
{
    struct gltf_accessor *ga = &gd->accrs.x[accr];
    struct gltf_bufview *bv;

    bv = gltf_bufview_accr(gd, accr);
    if (!bv)
        return NULL;

    return gd->buffers.x[bv->buffer] + ga->offset + bv->offset;
}

void *gltf_accessor_element(struct gltf_data *gd, int accr, size_t el)
{
    struct gltf_accessor *ga = &gd->accrs.x[accr];
    struct gltf_bufview *bv;
    size_t elsz;
    void *buf;

    bv = gltf_bufview_accr(gd, accr);
    if (!bv)
        return NULL;

    buf = gd->buffers.x[bv->buffer] + ga->offset + bv->offset;
    elsz = comp_sizes[ga->comptype];

    return buf + elsz * el;
}

unsigned int gltf_accessor_sz(struct gltf_data *gd, int accr)
{
    struct gltf_bufview *bv = gltf_bufview_accr(gd, accr);
    int buf;

    if (!bv)
        return 0;

    return bv->length;
}

#define GLTF_MESH_ATTR(_attr, _name, _type) \
_type *gltf_ ## _name(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_mesh *m = gltf_mesh(gd, mesh); \
    return m ? gltf_accessor_buf(gd, m->_attr) : NULL; \
} \
unsigned int gltf_ ## _name ## sz(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_mesh *m = gltf_mesh(gd, mesh); \
    return m ? gltf_accessor_sz(gd, m->_attr) : 0; \
} \
bool gltf_has_ ## _name(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_mesh *m = gltf_mesh(gd, mesh); \
    return m ? (m->_attr != -1) : false; \
} \
size_t gltf_ ## _name ## _stride(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_mesh *m = gltf_mesh(gd, mesh); \
    struct gltf_accessor *ga; \
    if (!m) return 0; \
    return gltf_accessor_stride(gd, m->_attr); \
} \
size_t gltf_nr_ ## _name(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_mesh *m = gltf_mesh(gd, mesh); \
    struct gltf_accessor *ga; \
    if (!m) return 0; \
    return gltf_accessor_nr(gd, m->_attr); \
}

GLTF_MESH_ATTR(POSITION,   vx,      float)
GLTF_MESH_ATTR(indices,    idx,     unsigned short)
GLTF_MESH_ATTR(TEXCOORD_0, tx,      float)
GLTF_MESH_ATTR(NORMAL,     norm,    float)
GLTF_MESH_ATTR(TANGENT,    tangent, float)
GLTF_MESH_ATTR(COLOR_0,    color,   float)
GLTF_MESH_ATTR(JOINTS_0,   joints,  unsigned char)
GLTF_MESH_ATTR(WEIGHTS_0,  weights, float)

struct gltf_material *gltf_material(struct gltf_data *gd, int mesh)
{
    struct gltf_mesh *m = gltf_mesh(gd, mesh);
    int mat;

    if (!m)
        return NULL;

    return &gd->mats.x[m->material];
}

#define GLTF_MAT_TEX(_attr, _name) \
bool gltf_has_ ## _name(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_material *mat = gltf_material(gd, mesh); \
    int tex; \
    if (!mat) \
        return false; \
    tex = mat->_attr ## _tex; \
    return tex >= 0; \
} \
void *gltf_ ## _name(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_material *mat = gltf_material(gd, mesh); \
    int tex = mat->_attr ## _tex; \
    struct gltf_bufview *bv = gltf_bufview_tex(gd, tex); \
 \
    return gd->buffers.x[bv->buffer] + bv->offset; \
} \
unsigned int gltf_ ## _name ## sz(struct gltf_data *gd, int mesh) \
{ \
    struct gltf_material *mat = gltf_material(gd, mesh); \
    int tex = mat->_attr ## _tex; \
    struct gltf_bufview *bv = gltf_bufview_tex(gd, tex); \
 \
    return bv->length; \
}

GLTF_MAT_TEX(base, tex)
GLTF_MAT_TEX(normal, nmap)

int gltf_root_mesh(struct gltf_data *gd)
{
    int root = gd->root_node;

    /* not detected -- use mesh 0 */
    if (root < 0)
        return 0;

    return gd->nodes.x[root].mesh;
}

int gltf_mesh_skin(struct gltf_data *gd, int mesh)
{
    int i;

    if (!gltf_has_joints(gd, mesh) || !gltf_has_weights(gd, mesh))
        return -1;

    for (i = 0; i < gd->nodes.da.nr_el; i++)
        if (gd->nodes.x[i].mesh == mesh && gd->nodes.x[i].skin >= 0)
            return gd->nodes.x[i].skin;
    return -1;
}

bool gltf_mesh_is_skinned(struct gltf_data *gd, int mesh)
{
    int skin = gltf_mesh_skin(gd, mesh);

    if (skin >= 0)
        return true;

    return false;
}

static void nodes_print(struct gltf_data *gd, struct gltf_node *node, int level)
{
    int child;

    list_init(&node->children);
    dbg("%.*s-> node '%s'\n", level, "----------", node->name);
    for (child = 0; child < node->nr_children; child++) {
        nodes_print(gd, &gd->nodes.x[node->ch_arr[child]], level + 1);
        list_append(&node->children, &gd->nodes.x[node->ch_arr[child]].entry);
    }
}

static void gltf_load_animations(struct gltf_data *gd, JsonNode *anis)
{
    JsonNode *n;

    if (!anis)
        return;

    /* Animations */
    for (n = anis->children.head; n; n = n->next) {
        JsonNode *jname, *jchans, *jsamplers, *jn;
        struct gltf_animation *ani;

        jname = json_find_member(n, "name");
        jchans = json_find_member(n, "channels");
        jsamplers = json_find_member(n, "samplers");

        CHECK(ani = darray_add(&gd->anis.da));
        if (jname && jname->tag == JSON_STRING)
            ani->name = strdup(jname->string_);

        darray_init(&ani->channels);
        darray_init(&ani->samplers);
        for (jn = jchans->children.head; jn; jn = jn->next) {
            JsonNode *jsampler, *jtarget, *jnode, *jpath;
            struct gltf_channel *chan;

            CHECK(chan = darray_add(&ani->channels.da));
            chan->sampler = -1;
            chan->node = -1;
            chan->path = PATH_NONE;

            if (jn->tag != JSON_OBJECT)
                continue;

            jsampler = json_find_member(jn, "sampler");
            if (jsampler && jsampler->tag == JSON_NUMBER)
                chan->sampler = jsampler->number_;
            jtarget = json_find_member(jn, "target");
            if (jtarget && jtarget->tag == JSON_OBJECT) {
                int i;

                jnode = json_find_member(jtarget, "node");
                if (jnode && jnode->tag == JSON_NUMBER)
                    chan->node = jnode->number_;
                jpath = json_find_member(jtarget, "path");
                if (jpath && jpath->tag == JSON_STRING) {
                    for (i = 0; i < array_size(paths); i++)
                        if (!strcmp(paths[i], jpath->string_))
                            goto found_path;
                    continue;
                found_path:
                    chan->path = i;
                }
                // dbg("## chan node: %d path: %s\n", chan->node, paths[chan->path]);
            }
        }

        for (jn = jsamplers->children.head; jn; jn = jn->next) {
            JsonNode *jinput, *joutput, *jinterp;
            struct gltf_sampler *sampler;
            int i;

            CHECK(sampler = darray_add(&ani->samplers.da));
            sampler->input = -1;
            sampler->output = -1;
            sampler->interp = -1;

            if (jn->tag != JSON_OBJECT)
                continue;

            jinput = json_find_member(jn, "input");
            if (jinput && jinput->tag == JSON_NUMBER)
                sampler->input = jinput->number_;

            joutput = json_find_member(jn, "output");
            if (joutput && joutput->tag == JSON_NUMBER)
                sampler->output = joutput->number_;
            jinterp = json_find_member(jn, "interpolation");
            if (jinterp && jinterp->tag == JSON_STRING) {
                for (i = 0; i < array_size(interps); i++)
                    if (!strcmp(interps[i], jinterp->string_))
                        goto found_interp;
                continue;
            found_interp:
                sampler->interp = i;
            }
            // dbg("## sampler input: %d output: %d interp: %s\n",
            //     sampler->input, sampler->output, interps[sampler->interp]);
        }
    }
}

static void gltf_load_skins(struct gltf_data *gd, JsonNode *skins)
{
    JsonNode *n;

    if (!skins)
        return;

    for (n = skins->children.head; n; n = n->next) {
        JsonNode *jmat, *jname, *jjoints, *jj;
        struct gltf_skin *skin;

        CHECK(skin = darray_add(&gd->skins.da));
        skin->invmxs = -1;

        jmat = json_find_member(n, "inverseBindMatrices");
        if (jmat && jmat->tag == JSON_NUMBER)
            skin->invmxs = jmat->number_;

        jname = json_find_member(n, "name");
        if (jname && jname->tag == JSON_STRING)
            skin->name = strdup(jname->string_);

        jjoints = json_find_member(n, "joints");
        if (jjoints && jjoints->tag == JSON_ARRAY) {
            int j;
            skin->joints = json_int_array_alloc(jjoints, &skin->nr_joints);
            skin->nodes = calloc(skin->nr_joints, sizeof(int));
            for (j = 0; j < skin->nr_joints; j++)
                skin->nodes[skin->joints[j]] = j;
        }
        dbg("skin '%s' nr_joints: %d\n", skin->name, skin->nr_joints);
    }
}

static void gltf_onload(struct lib_handle *h, void *data)
{
    JsonNode *nodes, *mats, *meshes, *texs, *imgs, *accrs, *bufvws, *bufs;
    JsonNode *scenes, *scene, *skins, *anis;
    JsonNode *root = json_decode(h->buf);
    struct gltf_data *gd = data;
    JsonNode *n;

    dbg("loading '%s'\n", h->name);
    if (!root) {
        warn("couldn't parse '%s'\n", h->name);
        return;
    }

    gd->root_node = -1;
    darray_init(&gd->nodes);
    darray_init(&gd->meshes);
    darray_init(&gd->bufvws);
    darray_init(&gd->mats);
    darray_init(&gd->accrs);
    darray_init(&gd->buffers);
    darray_init(&gd->anis);
    darray_init(&gd->skins);

    scenes = json_find_member(root, "scenes");
    scene = json_find_member(root, "scene");
    nodes = json_find_member(root, "nodes");
    mats = json_find_member(root, "materials");
    meshes = json_find_member(root, "meshes");
    anis = json_find_member(root, "animations");
    texs = json_find_member(root, "textures");
    imgs = json_find_member(root, "images");
    skins = json_find_member(root, "skins");
    accrs = json_find_member(root, "accessors");
    bufvws = json_find_member(root, "bufferViews");
    bufs = json_find_member(root, "buffers"); 
    if (scenes->tag != JSON_ARRAY ||
        scene->tag != JSON_NUMBER ||
        nodes->tag != JSON_ARRAY ||
        mats->tag != JSON_ARRAY ||
        meshes->tag != JSON_ARRAY ||
        (anis && anis->tag != JSON_ARRAY) ||
        texs->tag != JSON_ARRAY ||
        imgs->tag != JSON_ARRAY ||
        accrs->tag != JSON_ARRAY ||
        bufvws->tag != JSON_ARRAY ||
        bufs->tag != JSON_ARRAY) {
        dbg("type error %d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d\n",
            scenes->tag, scene->tag, nodes->tag, mats->tag,
            meshes->tag, texs->tag, imgs->tag, accrs->tag,
            bufvws->tag, bufs->tag, anis->tag
        );
        return;
    }

    /* Nodes */
    for (n = nodes->children.head; n; n = n->next) {
        JsonNode *jname, *jmesh, *jskin, *jchildren, *jrot, *jtrans, *jscale;
        struct gltf_node *node;

        if (n->tag != JSON_OBJECT)
            continue;

        jname = json_find_member(n, "name");
        jmesh = json_find_member(n, "mesh");
        jskin = json_find_member(n, "skin");
        jchildren = json_find_member(n, "children");
        jrot = json_find_member(n, "rotation");
        jtrans = json_find_member(n, "translation");
        jscale = json_find_member(n, "scale");
        if (!jname || jname->tag != JSON_STRING) /* actually, there only name is guaranteed */
            continue;

        CHECK(node = darray_add(&gd->nodes.da));
        node->name = strdup(jname->string_);
        if (jmesh && jmesh->tag == JSON_NUMBER)
            node->mesh = jmesh->number_;
        if (jskin && jskin->tag == JSON_NUMBER)
            node->skin = jskin->number_;
        if (jrot && jrot->tag == JSON_ARRAY)
            CHECK0(json_float_array(jrot, node->rotation, array_size(node->rotation)));
        if (jtrans && jtrans->tag == JSON_ARRAY)
            CHECK0(json_float_array(jtrans, node->translation, array_size(node->translation)));
        if (jscale && jscale->tag == JSON_ARRAY)
            CHECK0(json_float_array(jscale, node->scale, array_size(node->scale)));
        if (jchildren && jchildren->tag == JSON_ARRAY) 
            CHECK(node->ch_arr = json_int_array_alloc(jchildren, &node->nr_children));
    }
    /* unpack node.children arrays */

    /* Scenes */
    for (n = scenes->children.head; n; n = n->next) {
        JsonNode *jname, *jnodes;
        unsigned int nr_nodes;
        int *nodes, i;

        if (n->tag != JSON_OBJECT)
            continue;

        jname = json_find_member(n, "name");
        jnodes = json_find_member(n, "nodes");

        if (!jname || jname->tag != JSON_STRING)
            continue;
        if (!jnodes || jnodes->tag != JSON_ARRAY)
            continue;

        nodes = json_int_array_alloc(jnodes, &nr_nodes);
        if (!nodes || !nr_nodes)
            continue;

        for (i = 0; i < nr_nodes; i++) {
            struct gltf_node *node = darray_get(&gd->nodes.da, nodes[i]);

            if (!node)
                continue;
            if (!strcmp(node->name, "Light") || !strcmp(node->name, "Camera"))
                continue;
            gd->root_node = nodes[i];
            dbg("root node: '%s'\n", node->name);
            break;
        }
        free(nodes);
    }

    // nodes_print(gd, &gd->nodes.x[gd->root_node], 0);

    /* Buffers */
    for (n = bufs->children.head; n; n = n->next) {
        JsonNode *jlen, *juri;
        size_t   len, dlen, slen;
        void **buf;

        if (n->tag != JSON_OBJECT)
            continue;

        jlen = json_find_member(n, "byteLength");
        juri = json_find_member(n, "uri");
        if (!jlen && !juri)
            continue;

        len = jlen->number_;
        if (juri->tag != JSON_STRING ||
            strlen(juri->string_) < sizeof(DATA_URI) - 1 ||
            strncmp(juri->string_, DATA_URI, sizeof(DATA_URI) - 1))
            continue;

        slen = strlen(juri->string_) - sizeof(DATA_URI) + 1;
        len = max(len, base64_decoded_length(slen));

        CHECK(buf = darray_add(&gd->buffers.da));
        CHECK(*buf = malloc(len));
        dlen = base64_decode(*buf, len, juri->string_ + sizeof(DATA_URI) - 1, slen);
        // dbg("buffer %d: byteLength=%d uri length=%d dlen=%d/%d slen=%d '%.10s' errno=%d\n",
        //     gd->nr_buffers, (int)jlen->number_,
        //     strlen(juri->string_), dlen, len,
        //     slen, juri->string_ + sizeof(DATA_URI) - 1, errno);
    }

    /* BufferViews */
    for (n = bufvws->children.head; n; n = n->next) {
        JsonNode *jbuf, *jlen, *joff;
        struct gltf_bufview *bv;

        jbuf = json_find_member(n, "buffer");
        jlen = json_find_member(n, "byteLength");
        joff = json_find_member(n, "byteOffset");
        if (!jbuf || !jlen || !joff)
            continue;

        if (jbuf->number_ >= gd->buffers.da.nr_el)
            continue;

        CHECK(bv = darray_add(&gd->bufvws.da));
        bv->buffer = jbuf->number_;
        bv->offset = joff->number_;
        bv->length = jlen->number_;
        // dbg("buffer view %d: buf %d offset %zu size %zu\n", gd->nr_bufvws, bv->buffer,
        //     bv->offset, bv->length);
    }

    /* Accessors */
    for (n = accrs->children.head; n; n = n->next) {
        JsonNode *jbufvw, *jcount, *jtype, *jcomptype, *joffset;
        struct gltf_accessor *ga;
        int i;
        
        jbufvw = json_find_member(n, "bufferView");
        joffset = json_find_member(n, "byteOffset");
        jcount = json_find_member(n, "count");
        jtype = json_find_member(n, "type");
        jcomptype = json_find_member(n, "componentType");
        if (!jbufvw || !jcount || !jtype || !jcomptype)
            continue;
        
        if (jbufvw->number_ >= gd->bufvws.da.nr_el)
            continue;
        
        for (i = 0; i < array_size(types); i++)
            if (!strcmp(types[i], jtype->string_))
                break;
        
        if (i == array_size(types))
            continue;

        CHECK(ga = darray_add(&gd->accrs.da));
        ga->bufview = jbufvw->number_;
        ga->comptype = jcomptype->number_;
        ga->count = jcount->number_;
        ga->type = i;
        if (joffset && joffset->tag == JSON_NUMBER)
            ga->offset = joffset->number_;

        // dbg("accessor %d: bufferView: %d count: %d componentType: %d type: %s\n", gd->nr_accrs,
        //     ga->bufview, ga->count, ga->comptype,
        //     types[i]);
    }

    gltf_load_animations(gd, anis);
    gltf_load_skins(gd, skins);

    /* Images */
    for (n = imgs->children.head; n; n = n->next) {
        JsonNode *jbufvw, *jmime, *jname;

        jbufvw = json_find_member(n, "bufferView");
        jmime = json_find_member(n, "mimeType");
        jname = json_find_member(n, "name");
        if (!jbufvw || !jmime || !jname)
            continue;
        
        if (strcmp(jmime->string_, "image/png"))
            continue;

        if (jbufvw->number_ >= gd->bufvws.da.nr_el)
            continue;

        CHECK(gd->imgs = realloc(gd->imgs, (gd->nr_imgs + 1) * sizeof(unsigned int)));
        gd->imgs[gd->nr_imgs] = jbufvw->number_;
        dbg("image %d: bufferView: %d\n", gd->nr_imgs, gd->imgs[gd->nr_imgs]);
        gd->nr_imgs++;
    }

    /* Textures */
    for (n = texs->children.head; n; n = n->next) {
        JsonNode *jsrc; // mipmapping: *jsampler

        //jsampler = json_find_member(n, "sampler");
        jsrc = json_find_member(n, "source");
        if (!jsrc)
            continue;
        if (jsrc->number_ >= gd->nr_imgs)
            continue;

        CHECK(gd->texs = realloc(gd->texs, (gd->nr_texs + 1) * sizeof(unsigned int)));
        gd->texs[gd->nr_texs] = jsrc->number_;
        // dbg("texture %d: source: %d\n", gd->nr_texs, gd->texs[gd->nr_texs]);
        gd->nr_texs++;
    }

    /* Materials */
    for (n = mats->children.head; n; n = n->next) {
        struct gltf_material *mat;
        JsonNode *jwut, *jpbr;

        jpbr = json_find_member(n, "pbrMetallicRoughness");
        if (!jpbr || jpbr->tag != JSON_OBJECT)
            continue;

        jwut = json_find_member(jpbr, "baseColorTexture");
        /*
         * this means the model was exported from blender with "emission" shader
         * instead of principal BSDF; we'll still handle it, but print a warning
         */
        if (!jwut) {
            jwut = json_find_member(n, "emissiveTexture");
            warn("found emissiveTexture; this is probably not what you want\n");
        }
        if (!jwut || jwut->tag != JSON_OBJECT)
            continue;
        jwut = json_find_member(jwut, "index");
        if (jwut->tag != JSON_NUMBER || jwut->number_ >= gd->nr_texs)
            continue;

        CHECK(mat = darray_add(&gd->mats.da));
        mat->base_tex = jwut->number_;
        mat->normal_tex = -1;

        if (jpbr) {
            jwut = json_find_member(jpbr, "metallicFactor");
            if (jwut && jwut->tag == JSON_NUMBER)
                mat->metallic = jwut->number_;
        
            jwut = json_find_member(jpbr, "roughnessFactor");
            if (jwut && jwut->tag == JSON_NUMBER)
                mat->roughness = jwut->number_;
        
            jwut = json_find_member(n, "normalTexture");
            if (jwut && jwut->tag == JSON_OBJECT) {
                jwut = json_find_member(jwut, "index");
                if (jwut->tag == JSON_NUMBER && jwut->number_ < gd->nr_texs)
                    mat->normal_tex = jwut->number_;
            }
        }

        dbg("material %d: tex: %d nmap: %d met: %f rough: %f\n",
            gd->mats.da.nr_el - 1, mat->base_tex,
            mat->normal_tex,
            mat->metallic,
            mat->roughness
        );
    }

    for (n = meshes->children.head; n; n = n->next) {
        JsonNode *jname, *jprim, *jattr, *jindices, *jmat, *p;
        struct gltf_mesh *mesh;

        jname = json_find_member(n, "name"); /* like, "Cube", thanks blender */
        jprim = json_find_member(n, "primitives");
        if (!jname || !jprim)
            continue;
        if (jprim->tag != JSON_ARRAY)
            continue;

        /* XXX: why is this an array? */
        jprim = jprim->children.head;
        if (!jprim)
            continue;
        jindices = json_find_member(jprim, "indices");
        jmat = json_find_member(jprim, "material");
        jattr = json_find_member(jprim, "attributes");
        if (!jattr || jattr->tag != JSON_OBJECT || !jindices || !jmat)
            continue;

        CHECK(mesh = darray_add(&gd->meshes.da));
        gltf_mesh_init(mesh, jname->string_,
                       jindices->number_, jmat->number_);
        for (p = jattr->children.head; p; p = p->next) {
            if (!strcmp(p->key, "POSITION") && p->tag == JSON_NUMBER)
                mesh->POSITION = p->number_;
            else if (!strcmp(p->key, "NORMAL") && p->tag == JSON_NUMBER)
                mesh->NORMAL = p->number_;
            else if (!strcmp(p->key, "TANGENT") && p->tag == JSON_NUMBER)
                mesh->TANGENT = p->number_;
            else if (!strcmp(p->key, "TEXCOORD_0") && p->tag == JSON_NUMBER)
                mesh->TEXCOORD_0 = p->number_;
            else if (!strcmp(p->key, "COLOR_0") && p->tag == JSON_NUMBER)
                mesh->COLOR_0 = p->number_;
            else if (!strcmp(p->key, "JOINTS_0") && p->tag == JSON_NUMBER)
                mesh->JOINTS_0 = p->number_;
            else if (!strcmp(p->key, "WEIGHTS_0") && p->tag == JSON_NUMBER)
                mesh->WEIGHTS_0 = p->number_;
        }
        // dbg("mesh %d: '%s' POSITION: %d\n", gd->nr_meshes, jname->string_, mesh->POSITION);
    }

    json_free(root);
    ref_put(h);

    return;
}

void gltf_mesh_data(struct gltf_data *gd, int mesh, float **vx, size_t *vxsz, unsigned short **idx, size_t *idxsz,
                    float **tx, size_t *txsz, float **norm, size_t *normsz)
{
    struct gltf_mesh *m = &gd->meshes.x[mesh];

    if (m)
        return;

    if (vx) {
        *vx = gltf_vx(gd, mesh);
        *vxsz = gltf_vxsz(gd, mesh);
    }
    if (idx) {
        *idx = gltf_idx(gd, mesh);
        *idxsz = gltf_idxsz(gd, mesh);
    }
    if (tx) {
        *tx = gltf_tx(gd, mesh);
        *txsz = gltf_txsz(gd, mesh);
    }
    if (norm) {
        *norm = gltf_norm(gd, mesh);
        *normsz = gltf_normsz(gd, mesh);
    }
}

int gltf_skin_node_to_joint(struct gltf_data *gd, int skin, int node)
{
    return gd->skins.x[skin].nodes[node];
}

void gltf_instantiate_one(struct gltf_data *gd, int mesh)
{
    struct model3dtx *txm;
    struct model3d   *m;
    struct mesh *me;
    int skin;

    if (mesh < 0 || mesh >= gd->meshes.da.nr_el)
        return;

    me = mesh_new(gltf_mesh_name(gd, mesh));
    mesh_attr_dup(me, MESH_VX, gltf_vx(gd, mesh), gltf_vx_stride(gd, mesh), gltf_nr_vx(gd, mesh));
    mesh_attr_dup(me, MESH_TX, gltf_tx(gd, mesh), gltf_tx_stride(gd, mesh), gltf_nr_tx(gd, mesh));
    mesh_attr_dup(me, MESH_IDX, gltf_idx(gd, mesh), gltf_idx_stride(gd, mesh), gltf_nr_idx(gd, mesh));
    if (gltf_has_norm(gd, mesh))
        mesh_attr_dup(me, MESH_NORM, gltf_norm(gd, mesh), gltf_norm_stride(gd, mesh), gltf_nr_norm(gd, mesh));
    if (gltf_has_weights(gd, mesh))
        mesh_attr_dup(me, MESH_WEIGHTS, gltf_weights(gd, mesh), gltf_weights_stride(gd, mesh), gltf_nr_weights(gd, mesh));
    mesh_optimize(me);

    m = model3d_new_from_mesh(gltf_mesh_name(gd, mesh), gd->scene->prog, me);
    if (gltf_has_tangent(gd, mesh)) {
        model3d_add_tangents(m, gltf_tangent(gd, mesh), gltf_tangentsz(gd, mesh));
        dbg("added tangents for mesh '%s'\n", gltf_mesh_name(gd, mesh));
    }

    gd->scene->_model = m;
    if (gltf_has_nmap(gd, mesh)) {
        txm = model3dtx_new_from_buffers(ref_pass(m), gltf_tex(gd, mesh), gltf_texsz(gd, mesh),
                                        gltf_nmap(gd, mesh), gltf_nmapsz(gd, mesh));
        dbg("added textures %d, %d for mesh '%s'\n", texture_id(txm->texture), texture_id(txm->normals), gltf_mesh_name(gd, mesh));
    } else {
        txm = model3dtx_new_from_buffer(ref_pass(m), gltf_tex(gd, mesh), gltf_texsz(gd, mesh));
    }

    skin = gltf_mesh_skin(gd, mesh);
    if (skin >= 0) {
        int mxaccr = gd->skins.x[skin].invmxs, i;
        struct gltf_animation *ga;
        struct animation *an;

        model3d_add_skinning(gd->scene->_model,
                             gltf_joints(gd, mesh), gltf_jointssz(gd, mesh),
                             mesh_weights(me), mesh_weights_sz(me));
        gd->scene->_model->invmxs = memdup(gltf_accessor_buf(gd, mxaccr),
                                           gltf_accessor_sz(gd, mxaccr));
        darray_for_each(ga, &gd->anis) {
            struct gltf_accessor *accr;
            int frame;

            CHECK(an = animation_new(gd->scene->_model, gd->anis.x[0].name));
            dbg("## animation '%s'\n", an->name);
            /* number of keyframes */
            accr = gltf_accessor(gd, ga->samplers.x[0].input);
            for (frame = 0; frame < accr->count; frame++) {
                float *time = gltf_accessor_element(gd, ga->samplers.x[0].input, frame);
                struct gltf_channel *chan;
                struct pose *pose;
                int joint;

                // dbg("## frame %d: time: %f\n", frame, *time);
                pose = animation_pose_add(an);
                pose->frame = *time;

                darray_for_each(chan, &ga->channels) {
                    int joint = gltf_skin_node_to_joint(gd, skin, chan->node);
                    int sampler = ga->samplers.x[chan->sampler].output;
                    float *x = gltf_accessor_element(gd, sampler, frame);
                    struct gltf_node *node = &gd->nodes.x[chan->node];
                    int i;

                    for (i = 0; i < node->nr_children; i++) {
                        int *child = darray_add(&pose->joints.x[joint].children.da);
                        *child = gltf_skin_node_to_joint(gd, skin, node->ch_arr[i]);
                        // dbg("#### joint %d child of %d\n", *child, joint);
                    }
                    switch (chan->path) {
                        case PATH_TRANSLATION:
                            memcpy(pose->joints.x[joint].translation, x, sizeof(vec3));
                            break;
                        case PATH_ROTATION:
                            memcpy(pose->joints.x[joint].rotation, x, sizeof(quad_t));
                            break;
                        case PATH_SCALE:
                            memcpy(pose->joints.x[joint].scale, x, sizeof(vec3));
                            break;
                        default:
                            continue;
                    }
                    // dbg("## joint: %d node: %d tr [%f,%f,%f] R [%f,%f,%f,%f] S [%f,%f,%f]\n",
                    //     joint, chan->node,
                    //     pose->joints.x[joint].translation[0],
                    //     pose->joints.x[joint].translation[1],
                    //     pose->joints.x[joint].translation[2],
                    //     pose->joints.x[joint].rotation[0],
                    //     pose->joints.x[joint].rotation[1],
                    //     pose->joints.x[joint].rotation[2],
                    //     pose->joints.x[joint].rotation[3],
                    //     pose->joints.x[joint].scale[0],
                    //     pose->joints.x[joint].scale[1],
                    //     pose->joints.x[joint].scale[2]
                    // );
                }
            }
        }
    }
    ref_put(me);
    txm->metallic = clampf(gltf_material(gd, mesh)->metallic, 0.1, 1.0);
    txm->roughness = clampf(gltf_material(gd, mesh)->roughness, 0.2, 1.0);

    scene_add_model(gd->scene, txm);
}

void gltf_instantiate_all(struct gltf_data *gd)
{
    int i;

    for (i = 0; i < gd->meshes.da.nr_el; i++)
        gltf_instantiate_one(gd, i);
}

struct gltf_data *gltf_load(struct scene *scene, const char *name)
{
    struct gltf_data  *gd;
    struct lib_handle *lh;

    CHECK(gd = calloc(1, sizeof(*gd)));
    gd->scene = scene;
    lh = lib_request(RES_ASSET, name, gltf_onload, gd);
    ref_put(lh);

    return gd;
}
