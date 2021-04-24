#ifndef __CLAP_TERRAIN_H__
#define __CLAP_TERRAIN_H__

#include "object.h"

struct terrain {
    struct ref     ref;
    struct entity3d *entity;
    long           seed;
    float          *map, *map0;
    float x, y, z;
    unsigned int   side;
    unsigned int   nr_vert;
};

float terrain_height(struct terrain *t, float x, float z);
void terrain_normal(struct terrain *t, float x, float z, vec3 n);
struct terrain *terrain_init(struct scene *s, float x, float y, float z, float side, unsigned int nr_v);
struct terrain *terrain_init_circular_maze(struct scene *s, float x, float y, float z, float side, unsigned int nr_v, unsigned int nr_levels);
void terrain_done(struct terrain *terrain);

#endif /* __CLAP_TERRAIN_H__ */
