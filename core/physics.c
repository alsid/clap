// SPDX-License-Identifier: Apache-2.0
#include <ode/ode.h>
#include "common.h"
#include "character.h"
#include "linmath.h"
#include "model.h"
#include "physics.h"
#include "ui-debug.h"

static struct phys _phys;
struct phys *phys = &_phys; /* XXX */
static DECLARE_LIST(phys_bodies);

struct entity3d *phys_body_entity(struct phys_body *body)
{
    return dGeomGetData(body->geom);
}

const dReal *phys_body_position(struct phys_body *body)
{
    if (phys_body_has_body(body))
        return dBodyGetPosition(body->body);
    return dGeomGetPosition(body->geom);
}

const dReal *phys_body_rotation(struct phys_body *body)
{
    if (phys_body_has_body(body))
        return dBodyGetRotation(body->body);
    return dGeomGetRotation(body->geom);
}

/*
 * Separate thread? Emscripten won't be happy.
 */
#define MAX_CONTACTS 16

static bool geom_and_other_by_class(dGeomID o1, dGeomID o2, int class, dGeomID *match, dGeomID *other)
{
    *match = *other = NULL;

    if (dGeomGetClass(o1) == class) {
        *match = o1;
        *other = o2;
        return true;
    } else if (dGeomGetClass(o2) == class) {
        *match = o2;
        *other = o1;
        return true;
    }

    return false;
}

static bool entity_and_other_by_class(dGeomID o1, dGeomID o2, int class, struct entity3d **match,
                                      struct entity3d **other)
{
    dGeomID _match, _other;

    *match = *other = NULL;
    if (geom_and_other_by_class(o1, o2, class, &_match, &_other)) {
        *match = dGeomGetData(_match);
        *other = dGeomGetData(_other);
        return true;
    }

    return false;
}

void phys_body_stick(struct phys_body *body, dContact *contact)
{
    struct entity3d *e = phys_body_entity(body);
    struct character *c = e->priv;
    dMatrix3 R;
    dJointID j;

    /* should also warn */
    if (!phys_body_has_body(body))
        return;

    if (c) {
        c->normal[0] = contact->geom.normal[0];
        c->normal[1] = contact->geom.normal[1];
        c->normal[2] = contact->geom.normal[2];
    }

    j = dJointCreateContact(phys->world, phys->contact, contact);
    dJointAttach(j, body->body, NULL);

    if (dJointGetBody(body->lmotor, 0))
        return;

    dJointAttach(body->lmotor, body->body, NULL);
    dJointSetLMotorParam(body->lmotor, dParamVel1, 0);
    dJointSetLMotorParam(body->lmotor, dParamVel2, 0);
    dJointSetLMotorParam(body->lmotor, dParamVel3, 0);
    dBodySetMaxAngularSpeed(body->body, 0);
    dBodySetLinearDampingThreshold(body->body, 0.001);
}

static void phys_contact_surface(struct entity3d *e1, struct entity3d *e2, dContact *contact, int nc)
{
    int i;

    for (i = 0; i < nc; i++) {
        memset(&contact[i], 0, sizeof(dContact));
        contact[i].surface.mode = /*dContactBounce | */dContactSoftCFM | dContactSoftERP;
        contact[i].surface.mu = /*bounce != 0 ? dInfinity : */0;
        contact[i].surface.mu2 = 0;
        contact[i].surface.bounce = 0.01;
        contact[i].surface.bounce_vel = 10.0;
        contact[i].surface.soft_cfm = 0.01;
        contact[i].surface.soft_erp = 0;
    }
}

static const char *class_str(int class)
{
    static const char *classes[] = {
        [dSphereClass]             = "sphere",
        [dBoxClass]                = "box",
        [dCapsuleClass]            = "capsule",
        [dCylinderClass]           = "cylinder",
        [dPlaneClass]              = "plane",
        [dRayClass]                = "ray",
        [dConvexClass]             = "convex",
        [dGeomTransformClass]      = "geom_transform",
        [dTriMeshClass]            = "trimesh",
        [dHeightfieldClass]        = "heightfield",
        [dSimpleSpaceClass]        = "simple_space",
        [dHashSpaceClass]          = "hash_space",
        [dSweepAndPruneSpaceClass] = "sap_space",
        [dQuadTreeSpaceClass]      = "quadtree_space",
    };

    if (class < array_size(classes) && classes[class])
        return classes[class];
    return "<unknown>";
}

static void near_callback(void *data, dGeomID o1, dGeomID o2)
{
    bool ground;
    dContact contact[MAX_CONTACTS];
    dBodyID b1 = dGeomGetBody(o1);
    dBodyID b2 = dGeomGetBody(o2);
    dReal bounce = 0.0, bounce_vel = 0.0;
    struct list *pen = data;
    dJointID j;
    int i, nc;

    phys_contact_surface(NULL, NULL, contact, MAX_CONTACTS);

    nc = dCollide(o1, o2, MAX_CONTACTS, &contact[0].geom, sizeof(dContact));
    if (nc) {
        for (i = 0; i < nc; i++) {
            dGeomID g1 = contact[i].geom.g1;
            dGeomID g2 = contact[i].geom.g2;
            struct entity3d *e1 = dGeomGetData(g1);
            struct entity3d *e2 = dGeomGetData(g2);
            struct entity3d *e_ground, *e_other;

            b1 = dGeomGetBody(g1);
            b2 = dGeomGetBody(g2);
            phys_body_update(e1);
            phys_body_update(e2);
            j = dJointCreateContact(phys->world, phys->contact, &contact[i]);
            dJointAttach(j, b1, b2);
            if (!phys_body_has_body(e1->phys_body)) {
                e_ground = e1;
                e_other = e2;
                ground = true;
            } else if (!phys_body_has_body(e2->phys_body)) {
                e_ground = e2;
                e_other = e1;
                ground  = true;
            } else {
                if (e1->priv) {
                    e_other = e1;
                    e_ground = e2;
                    ground = true;
                } else if (e2->priv) {
                    e_other = e2;
                    e_ground = e1;
                    ground = true;
                }
                ground   = false;
            }

            if (ground) {
                const char *name = entity_name(e_other);
                vec3 norm = { contact[i].geom.normal[0], contact[i].geom.normal[1], contact[i].geom.normal[2] };

                if (!e_other->phys_body)
                    return;

                phys_body_stick(e_other->phys_body, &contact[i]);
                e_other->phys_body->pen_depth += contact[i].geom.depth;
                vec3_scale(norm, norm, contact[i].geom.depth);
                vec3_add(e_other->phys_body->pen_norm, e_other->phys_body->pen_norm, norm);
                list_del(&e_other->phys_body->pen_entry);
                list_append(pen, &e_other->phys_body->pen_entry);
            }
        }
    }
}

struct contact {
    dContact *contact;
    int nc;
};

static void got_contact(void *data, dGeomID o1, dGeomID o2)
{
    struct contact *c = data;
    dContact contact;

    phys_contact_surface(dGeomGetData(o1), dGeomGetData(o2), c->contact ? c->contact : &contact, 1);
    c->nc = dCollide(o1, o2, 1, c->contact ? &c->contact->geom : &contact.geom, sizeof(dContact));
}

struct entity3d *phys_ray_cast(struct entity3d *e, vec3 start, vec3 dir, double *pdist)
{
    struct entity3d *target = NULL;
    dGeomID ray = NULL;
    dContact contact;
    struct contact c = { .contact = &contact };
    vec3 _start = { start[0], start[1], start[2] };
    vec3 comp;
    int try = 0;

    ray = dCreateRay(phys->space, *pdist);
retry:
    dGeomRaySetClosestHit(ray, 1);
    /* avoid self-intersections as much as possible */
    dGeomRaySetBackfaceCull(ray, 1);
    phys_contact_surface(NULL, NULL, &contact, 1);
    dGeomRaySet(ray, _start[0], _start[1], _start[2], dir[0], dir[1], dir[2]);
    dSpaceCollide2(ray, (dGeomID)phys->space, &c, &got_contact);
    if (c.nc) {
        target = dGeomGetData(dGeomGetClass(contact.geom.g1) == dRayClass ? contact.geom.g2 : contact.geom.g1);
        if (target) {
            if (e == target && try++ < 10) {
                vec3_scale(comp, dir, contact.geom.depth + 1e-3);
                vec3_add(_start, _start, comp);
                goto retry;
            }
        }

        vec3_sub(comp, _start, start);
        *pdist = contact.geom.depth + vec3_len(comp);
    }
    dGeomDestroy(ray);

    // if (nc)
    //     ui_debug_printf("[%f,%f,%f]->[%f,%f,%f] %s@%f(-%f)(%d) [%f,%f,%f] try %d", _start[0], _start[1], _start[2],
    //                     dir[0], dir[1], dir[2], entity_name(target), *pdist, vec3_len(comp), nc,
    //                     contact.geom.pos[0], contact.geom.pos[1], contact.geom.pos[2], try);
    return c.nc ? target : NULL;
}

void phys_ground_entity(struct entity3d *e)
{
    struct entity3d *collision;
    vec3 start = { e->dx, e->dy, e->dz };
    vec3 dir = { 0, -1, 0 };
    double dist = 1e6;

    collision = phys_ray_cast(e, start, dir, &dist);
    /* floating in space? */
    if (!collision)
        return;

    e->dy -= dist;
}

bool phys_body_is_grounded(struct phys_body *body)
{
    struct entity3d *e = phys_body_entity(body);
    dVector3 dir = { 0, -1, 0 };
    dGeomID ray, *ground;
    const dReal *pos, epsilon = 1e-3;
    struct contact contact = {};

    if (!phys_body_has_body(body))
        return true;

    if (!dJointGetBody(body->lmotor, 0))
        return false;

    dSpaceCollide2(body->geom, (dGeomID)phys->ground_space, &contact, &got_contact);
    if (contact.nc)
        return true;

    pos = phys_body_position(body);
    ray = dCreateRay(phys->space, body->yoffset - body->ray_off + epsilon);
    dGeomRaySet(ray, pos[0], pos[1] - body->ray_off, pos[2], dir[0], dir[1], dir[2]);
    dSpaceCollide2(ray, (dGeomID)phys->ground_space, &contact, got_contact);
    dGeomDestroy(ray);

    return !!contact.nc;
}

bool phys_body_ground_collide(struct phys_body *body)
{
    struct entity3d *e = phys_body_entity(body);
    dReal epsilon = 1e-3;
    dReal ray_len = body->yoffset - body->ray_off + epsilon;
    dVector3 dir = { 0, -ray_len, 0 };
    //struct character *ch = e->priv;
    dContact contact;
    struct contact c = { .contact = &contact };
    dGeomID ray, *ground;
    const dReal *pos;

    if (!phys_body_has_body(body))
        return true;

    phys_contact_surface(NULL, NULL, &contact, 1);

    /*
     * Check if our capsule intersects with anything
     */
    dSpaceCollide2(body->geom, (dGeomID)phys->ground_space, &c, got_contact);
    if (c.nc) {
        dVector3 up = { 0, 1, 0 };
        dReal upness = dDot(c.contact->geom.normal, up, 3);

        if (upness > 0.95) {
            entity3d_move(e, 0, ray_len + c.contact->geom.depth, 0);
        } else if (upness > 0.3) {
            entity3d_move(e, 0, c.contact->geom.depth, 0);
        } else {
            dJointAttach(body->lmotor, NULL, NULL);
        }
    }

    pos = phys_body_position(body);
    ray = dCreateRay(phys->space, ray_len);
    dGeomRaySet(ray, pos[0], pos[1] - body->ray_off, pos[2], dir[0], dir[1], dir[2]);
    //dGeomSetBody(ray, body->body);
    // nc = dCollide(ray, *ground, 1, &contact.geom, sizeof(dContact));
    dSpaceCollide2(ray, (dGeomID)phys->ground_space, &c, got_contact);
    dGeomDestroy(ray);
    if (c.nc)
        goto got_it;

    return false;

got_it:
    if (ray_len - c.contact->geom.depth > epsilon) {
        entity3d_move(e, 0, ray_len - c.contact->geom.depth, 0);
        ui_debug_printf("RAY '%s' collides with %s at %f/%f (%f,%f,%f) normal %f,%f,%f\n", entity_name(e), class_str(dGeomGetClass(contact.geom.g2)),
            contact.geom.depth, ray_len, e->dx, e->dy, e->dz,
            contact.geom.normal[0], contact.geom.normal[1], contact.geom.normal[2]);
    }
    phys_body_stick(e->phys_body, &contact);

    return true;
}

void phys_step(unsigned long frame_count)
{
    struct phys_body *pb, *itpb;
    DECLARE_LIST(pen);

    /* this is instead called in character update */
    // list_for_each_entry(pb, &phys_bodies, entry) {
    //     if (phys_body_ground_collide(pb)) {
    //     }
    // }

    dSpaceCollide2((dGeomID)phys->ground_space, (dGeomID)phys->character_space,
                   &pen, near_callback);

    list_for_each_entry_iter(pb, itpb, &pen, pen_entry) {
        const dReal     *pos = phys_body_position(pb);
        struct entity3d *e = phys_body_entity(pb);
        struct character *c  = e->priv;
        vec3             off = { pos[0], pos[1], pos[2] };
        dMatrix3 R;

        /*if (pb->lmotor) {
            dJointDestroy(pb->lmotor);
            pb->lmotor = NULL;
        }*/

        /* XXX */
        if (c) {
            c->ragdoll = 0;
        }
        //dbg("clearing velocities from '%s'\n", entity_name(e));
        // dBodySetLinearVel(pb->body, 0, 0, 0);
        // dBodySetAngularVel(pb->body, 0, 0, 0);
        dRSetIdentity(R);
        dRFromEulerAngles(R, e->rx, e->ry, e->rz);
        if (phys_body_has_body(pb))
            dBodySetRotation(pb->body, R);
        else
            dGeomSetRotation(pb->geom, R);
        // dBodyDisable(pb->body);
        if (pb->pen_depth > 0 && vec3_len(pb->pen_norm) > 0) {
            vec3_sub(off, off, pb->pen_norm);
            dBodySetPosition(pb->body, off[0], off[1], off[2]);
            phys_body_ground_collide(pb);
        }
        list_del(&pb->pen_entry);
        pb->pen_depth = 0;
        pb->pen_norm[0] = pb->pen_norm[1] = pb->pen_norm[2] = 0.0;
    }

    /* XXX: quick step fails in quickstep.cpp:3267 */
    dWorldQuickStep(phys->world, 0.01 * frame_count);
    dJointGroupEmpty(phys->contact);
}

static void dGetEulerAngleFromRot(const dMatrix3 mRot, dReal *rX, dReal *rY, dReal *rZ)
{
    *rY = asin(mRot[0 * 4 + 2]);
    if (*rY < M_PI / 2) {
        if (*rY > -M_PI / 2) {
            *rX = atan2(-mRot[1 * 4 + 2], mRot[2 * 4 + 2]);
            *rZ = atan2(-mRot[0 * 4 + 1], mRot[0 * 4 + 0]);
        } else {
            // not unique
            *rX = -atan2(mRot[1 * 4 + 0], mRot[1 * 4 + 1]);
            *rZ = REAL(0.0);
        }
    } else {
        // not unique
        *rX = atan2(mRot[1 * 4 + 0], mRot[1 * 4 + 1]);
        *rZ = REAL(0.0);
    }
}

int phys_body_update(struct entity3d *e)
{
    const dReal *pos;
    const dReal *rot;
    const dReal *vel;
    dReal       rx, ry, rz;

    if (!e->phys_body || !phys_body_has_body(e->phys_body))
        return 0;

    pos = phys_body_position(e->phys_body);
    e->dx = pos[0];
    e->dy = pos[1] - e->phys_body->yoffset;
    e->dz = pos[2];
    vel = dBodyGetLinearVel(e->phys_body->body);

    return dCalcVectorLength3(vel) > 1e-3 ? 1 : 0;
}

dGeomID phys_geom_capsule_new(struct phys *phys, struct phys_body *body, struct entity3d *e,
                              double mass, double geom_radius, double geom_offset)
{
    float r = 0.0, length = 0.0, off = 0.0, X, Y, Z;
    int direction;
    dGeomID g;
    dMass m;

    X = entity3d_aabb_X(e);
    Y = entity3d_aabb_Y(e);
    Z = entity3d_aabb_Z(e);
    /*
     * capsule seems to be always be vertical (direction 2 -- Y positive),
     * so we need to use offset rotation for horizontal cases
     */
    direction = xmax3(X, Y, Z) + 1;
    switch (direction) {
        case 1:
            // direction = 2; /* XXX */
            // abort();
            // break;
        case 2:
            /*
             * Upright
             */
            r      = geom_radius ? geom_radius : min3(X, Y, Z) / 2;
            length = max(Y / 2 - r * 2, 0);
            off    = geom_offset ? geom_offset : Y / 2;
            body->ray_off = r + length / 2;
            break;
        case 3:
            direction = 3;

            /*
             * Puppy
             */
            r      = geom_radius ? geom_radius : X / 2;
            length = Z - r * 2;
            off    = geom_offset ? geom_offset : (Y - r * 2) / 2;
            body->ray_off = r;
            break;
    }
    
    /*
     * XXX: The above logic is busted: ray length ends up zero.
     */
    // dbg("CAPSULE('%s') dir=%d r=%f length=%f yoff=%f ray_off=%f ray_len=%f\n", entity_name(e), direction, r, length,
    //     off, body->ray_off, off - body->ray_off);

    if (length)
        CHECK(g = dCreateCapsule(phys->space, r, length));
    else
        CHECK(g = dCreateSphere(phys->space, r));
    //dGeomSetData(g, e);

    body->geom = g;
    body->yoffset = off;
    if (phys_body_has_body(body)) {
        dMassSetZero(&body->mass);
        if (length)
            dMassSetCapsuleTotal(&m, mass, direction, r, length);
        else
            dMassSetSphereTotal(&m, mass, r);
        dBodySetMass(body->body, &m);
    }

    return g;
}

dGeomID phys_geom_trimesh_new(struct phys *phys, struct phys_body *body, struct entity3d *e, double mass)
{
    dTriMeshDataID meshdata = dGeomTriMeshDataCreate();
    struct model3d *m = e->txmodel->model;
    unsigned short *idx = m->collision_idx;
    size_t idxsz = m->collision_idxsz;
    size_t vxsz = m->collision_vxsz;
    float *vx = m->collision_vx;
    dGeomID trimesh = NULL;
    dReal *tnorm = NULL;
    dReal *tvx = NULL;
    dTriIndex *tidx;
    int i;

    idxsz /= sizeof(unsigned short);
    CHECK(tidx = calloc(idxsz, sizeof(*tidx))); /* XXX: refcounting, or tied to model? */
    for (i = 0; i < idxsz; i += 3) {
        /* swap i+1 and i+2 on either side to switch winding */
        tidx[i + 0] = idx[i + 0];
        tidx[i + 1] = idx[i + 1];
        tidx[i + 2] = idx[i + 2];
    }

    vxsz /= sizeof(GLfloat);
    CHECK(tvx = calloc(vxsz, sizeof(*tvx)));
    for (i = 0; i < vxsz; i += 3) {
        /* apply rotation and scale */
        mat4x4 trans;
        vec4 pos = { vx[i + 0], vx[i + 1], vx[i + 2], 1 };
        vec4 res;
        mat4x4_identity(trans);
        mat4x4_rotate_X(trans, trans, e->rx);
        mat4x4_rotate_Y(trans, trans, e->ry);
        mat4x4_rotate_Z(trans, trans, e->rz);
        mat4x4_scale_aniso(trans, trans, e->scale, e->scale, e->scale);
        mat4x4_mul_vec4(res, trans, pos);
        tvx[i + 0] = res[0];
        tvx[i + 1] = res[1];
        tvx[i + 2] = res[2];
    }
    /*if (m->norm) {
        CHECK(tnorm = calloc(vxsz, sizeof(*tnorm)));
        for (i = 0; i < vxsz; i += 3) {
            //vec3_norm(&norm[i], &norm[i]);
            tnorm[i + 0] = m->norm[i + 0];
            tnorm[i + 1] = m->norm[i + 1];
            tnorm[i + 2] = m->norm[i + 2];
            dNormalize3(&tnorm[i]);
        }
    }*/
#ifdef dDOUBLE
    dGeomTriMeshDataBuildDouble(meshdata, tvx, 3 * sizeof(dReal), vxsz / 3, tidx, idxsz, 3 * sizeof(dTriIndex));
#else
    dGeomTriMeshDataBuildSingle1(meshdata, tvx, 3 * sizeof(float), vxsz / 3, tidx, idxsz, 3 * sizeof(dTriIndex), NULL);
#endif
    //dGeomTriMeshDataBuildSimple(meshdata, tvx, vxsz, tidx, idxsz);
    dGeomTriMeshDataPreprocess2(meshdata, (1U << dTRIDATAPREPROCESS_BUILD_FACE_ANGLES), NULL);
    //dGeomTriMeshDataPreprocess2(meshdata, (1U << dTRIDATAPREPROCESS_BUILD_CONCAVE_EDGES), NULL);
    CHECK(trimesh = dCreateTriMesh(phys->space, meshdata, NULL, NULL, NULL));
    //dGeomSetData(trimesh, e);

    /*
     * XXX: terrain.c corner case, calls here directly with body==NULL
     * XXX: make it create PHYS_GEOM "body" instead.
     */
    if (body) {
        body->geom = trimesh;
        if (phys_body_has_body(body)) {
            dMassSetTrimeshTotal(&body->mass, mass, body->geom);
            dGeomSetPosition(body->geom, -body->mass.c[0], -body->mass.c[1], -body->mass.c[2]);
            dMassTranslate(&body->mass, -body->mass.c[0], -body->mass.c[1], -body->mass.c[2]);
            dBodySetMass(body->body, &body->mass);
        }
    }

    return trimesh;
}

struct phys_body *phys_body_new(struct phys *phys, struct entity3d *entity, int class,
                                double geom_radius, double geom_offset, int type, double mass)
{
    bool has_body = type == PHYS_BODY;
    struct phys_body *body;
    dMatrix3 rot;
    dMass m;

    CHECK(body = calloc(1, sizeof(*body)));
    list_init(&body->pen_entry);
    body->phys = phys;
    
    if (has_body)
        body->body = dBodyCreate(phys->world);

    if (class == dTriMeshClass) {
        phys_geom_trimesh_new(phys, body, entity, mass);
    } else if (class == dSphereClass) {
        dMassSetZero(&m);
        dMassSetSphereTotal(&m, mass, 0.1);
    } else if (class == dCapsuleClass) {
        phys_geom_capsule_new(phys, body, entity, mass, geom_radius * entity->scale,
                              geom_offset * entity->scale);
    }

    dRSetIdentity(rot);
    if (has_body) {
        dBodySetPosition(body->body, entity->dx, entity->dy + body->yoffset, entity->dz);
        dBodySetRotation(body->body, rot);
        dGeomSetBody(body->geom, body->body);
        dBodySetData(body->body, entity);
        if (class == dCapsuleClass) {
            // Capsule geometry assumes that Z goes upwards,
            // so cylinder's axis is parallel to Z axis.
            // We need to rotate the local geometry so that
            // cylinder's axis is parallel to Y axis.
            dMatrix3 R;
            dRFromAxisAndAngle(R, 1.0, 1.0, 1.0, -M_PI * 2.0 / 3.0);
            dGeomSetOffsetRotation(body->geom, R);
        }
        dSpaceRemove(phys->space, body->geom);
        dSpaceAdd(phys->character_space, body->geom);
    } else {
        dGeomSetPosition(body->geom, entity->dx, entity->dy + body->yoffset, entity->dz);
        if (class == dCapsuleClass) {
            // A similar orientation fix is needed for geometries
            // that don't have a body; in this case,
            // we can just set its rotation, since it is not
            // going to change.
            dRFromAxisAndAngle(rot, 1.0, 1.0, 1.0, -M_PI * 2.0 / 3.0);
        }
        
        dGeomSetRotation(body->geom, rot);
        dSpaceRemove(phys->space, body->geom);
        dSpaceAdd(phys->ground_space, body->geom);
    }
    dGeomSetData(body->geom, entity);
    list_append(&phys_bodies, &body->entry);

    if (has_body) {
        body->lmotor = dJointCreateLMotor(phys->world, 0);
        dJointSetLMotorNumAxes(body->lmotor, 3);
        dJointSetLMotorAxis(body->lmotor, 0, 0, 1, 0, 0);
        dJointSetLMotorAxis(body->lmotor, 1, 0, 0, 1, 0);
        dJointSetLMotorAxis(body->lmotor, 2, 0, 0, 0, 1);
        dJointSetLMotorParam(body->lmotor, dParamFMax1, 50);
        dJointSetLMotorParam(body->lmotor, dParamFMax2, 5);
        dJointSetLMotorParam(body->lmotor, dParamFMax3, 50);
    }

    return body;
}

void phys_body_done(struct phys_body *body)
{
    list_del(&body->entry);
    if (body->geom)
        dGeomDestroy(body->geom);
    if (body->body)
        dBodyDestroy(body->body);
    body->geom = NULL;
    body->body = NULL;
    free(body);
}

static void ode_error(int errnum, const char *msg, va_list ap)
{
    vlogg(ERR, "ODE", -1, "", msg, ap);
    vlogg(ERR, "ODE", -1, "\n", msg, ap);
}

static void ode_debug(int errnum, const char *msg, va_list ap)
{
    vlogg(DBG, "ODE", -1, "", msg, ap);
    vlogg(DBG, "ODE", -1, "\n", msg, ap);
    abort();
}

static void ode_message(int errnum, const char *msg, va_list ap)
{
    vlogg(NORMAL, "ODE", -1, "\n", msg, ap);
}

int phys_init(void)
{
    dInitODE2(0);
    // dSetErrorHandler(ode_error);
    dSetDebugHandler(ode_debug);
    dSetMessageHandler(ode_message);
    phys->world = dWorldCreate();
    phys->space = dHashSpaceCreate(0);
    phys->collision = dHashSpaceCreate(phys->space);
    phys->character_space = dHashSpaceCreate(phys->space);
    phys->ground_space = dHashSpaceCreate(phys->space);
    phys->contact = dJointGroupCreate(0);
    dWorldSetGravity(phys->world, 0, -9.8, 0);
    // dWorldSetCFM(phys->world, 1e-5);
    // dWorldSetERP(phys->world, 0.8);
    //dWorldSetContactSurfaceLayer(phys->world, 0.001);
    dWorldSetLinearDamping(phys->world, 0.001);

    return 0;
}

void phys_done(void)
{
    dSpaceDestroy(phys->ground_space);
    dSpaceDestroy(phys->character_space);
    dSpaceDestroy(phys->collision);
    dSpaceDestroy(phys->space);
    dWorldDestroy(phys->world);
    dCloseODE();
}

void phys_rotation_to_mat4x4(const dReal *rot, const dReal *pos, mat4x4 *m)
{
    float *mx = (float *)*m;
    mx[0] = rot[0];
    mx[1] = rot[1];
    mx[2] = rot[2];
    mx[3] = 0;
    mx[4] = rot[4];
    mx[5] = rot[5];
    mx[6] = rot[6];
    mx[7] = 0;
    mx[8] = rot[8];
    mx[9] = rot[9];
    mx[10] = rot[10];
    mx[11] = 0;
    mx[12] = pos ? pos[0] : 0;
    mx[13] = pos ? pos[1] : 0;
    mx[14] = pos ? pos[2] : 0;
    mx[15] = 1;
}

void phys_debug_draw(struct scene *scene, struct phys_body *body)
{
    vec3 start, end;
    const dReal *pos = dGeomGetPosition(body->geom);
    dReal r, len = 0;
    const dReal *rot;
    mat4x4 _rot, _rot_tmp;
    int class = dGeomGetClass(body->geom);

    if (class == dCapsuleClass)
        dGeomCapsuleGetParams(body->geom, &r, &len);
    else if (class == dSphereClass)
        r = dGeomSphereGetRadius(body->geom);
    else
        return;

    rot = dGeomGetRotation(body->geom);
    phys_rotation_to_mat4x4(rot, NULL, &_rot_tmp);
    mat4x4_invert(_rot, _rot_tmp);
    _rot[3][0] = pos[0];
    _rot[3][1] = pos[1];
    _rot[3][2] = pos[2];
    
    start[0] = -r;
    start[1] = -r;
    start[2] = -len / 2 - r;
    end[0] = r;
    end[1] = r;
    end[2] = len / 2 + r;
    debug_draw_line(scene, start, end, &_rot);
    start[0] = r;
    start[1] = r;
    end[0] = -r;
    end[1] = -r;
    debug_draw_line(scene, start, end, &_rot);
    start[0] = -r;
    start[1] = r;
    end[0] = r;
    end[1] = -r;
    debug_draw_line(scene, start, end, &_rot);
    start[0] = r;
    start[1] = -r;
    end[0] = -r;
    end[1] = r;
    debug_draw_line(scene, start, end, &_rot);
    start[0] = r;
    start[1] = -r;
    end[0] = -r;
    end[1] = r;
    debug_draw_line(scene, start, end, &_rot);
}
