/* SPDX-License-Identifier: Apache-2.0 */
#ifndef __CLAP_CHARACTER_H__
#define __CLAP_CHARACTER_H__

#include "anictl.h"
#include "matrix.h"
#include "messagebus.h"
#include "model.h"
#include "physics.h"
#include "scene.h"

struct motionctl {
    struct timespec ts;
    struct timespec dash_started;
    float   lin_speed;
    float   ang_speed;
    float   h_ang_speed;
    float   ls_left;
    float   ls_right;
    float   ls_up;
    float   ls_down;
    float   ls_dx;
    float   ls_dy;
    float   rs_left;
    float   rs_right;
    float   rs_up;
    float   rs_down;
    float   rs_dx;
    float   rs_dy;
    bool    rs_height;
    bool    jump;
};

struct character {
    struct ref  ref;
    struct entity3d *entity;
    int (*orig_update)(struct entity3d *, void *);
    struct camera *camera;
    struct motionctl mctl;
    /* XXX: the below double entity's: dx,dy,dz,rx,ry,rz */
    GLfloat pos[3];
    GLfloat pitch;  /* left/right */
    GLfloat yaw;    /* sideways */
    GLfloat roll;   /* up/down */
    vec3    motion;
    vec3    angle;
    vec3    normal;
    double  speed;
    float   yaw_turn;
    float   pitch_turn;
    struct list entry;
    struct anictl anictl;
    int     moved;
    int     ragdoll;
    int     stuck;
    bool    dashing;
    bool    jumping;
};

static inline struct entity3d *character_entity(struct character *c)
{
    return c->entity;
}

static inline const char *character_name(struct character *c)
{
    return entity_name(character_entity(c));
}

struct character *character_new(struct model3dtx *txm, struct scene *s);
void character_handle_input(struct character *ch, struct scene *s, struct message *m);
bool character_is_grounded(struct character *ch, struct scene *s);
void character_move(struct character *ch, struct scene *s);

#endif /* __CLAP_CHARACTER_H__ */
