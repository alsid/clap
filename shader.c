#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "common.h"
//#include "matrix.h"
#include "display.h"
#include "util.h"
#include "object.h"
#include "shader.h"
#include "common.h"
#include "librarian.h"
#include "scene.h"

static GLuint loadShader(GLenum shaderType, const char *shaderSource)
{
    GLuint shader = glCreateShader(shaderType);
    if (!shader) {
        err("couldn't create shader\n");
        return -1;
    }
    glShaderSource(shader, 1, &shaderSource, NULL);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen) {
            char *buf = malloc(infoLen);
            if (buf) {
                glGetShaderInfoLog(shader, infoLen, NULL, buf);
                err("Could not Compile Shader %d:\n%s\n", shaderType, buf);
                free(buf);
                err("--> %s <--\n", shaderSource);
            }
            glDeleteShader(shader);
            shader = 0;
        }
    }
    return shader;
}

static GLuint createProgram(const char* vertexSource, const char * fragmentSource)
{
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = loadShader(GL_FRAGMENT_SHADER, fragmentSource);
    GLuint program = glCreateProgram();
    GLint linkStatus = GL_FALSE;

    if (!vertexShader || !fragmentShader || !program) {
        err("vshader: %d fshader: %d program: %d\n",
            vertexShader, fragmentShader, program);
        return 0;
    }

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus != GL_TRUE) {
        GLint bufLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
        if (bufLength) {
            char *buf = malloc(bufLength);
            if (buf) {
                glGetProgramInfoLog(program, bufLength, NULL, buf);
                err("Could not link program:\n%s\n", buf);
                free(buf);
            }
        }
        glDeleteProgram(program);
        program = 0;
    }
    dbg("vshader: %d fshader: %d program: %d link: %d\n",
        vertexShader, fragmentShader, program, linkStatus);

    return program;
}

struct shader_var {
    char *name;
    GLint loc;
    struct shader_var *next;
};

/* XXX use a hashlist */
GLint shader_prog_find_var(struct shader_prog *p, const char *var)
{
    struct shader_var *v;

    for (v = p->var; v; v = v->next)
        if (!strcmp(v->name, var))
            return v->loc;
    return -1;
}

static int shader_prog_scan(struct shader_prog *p, const char *txt)
{
    static const char *vars[] = {"uniform", "attribute"};/* "varying"? */
    const char *pos, *pend;
    struct shader_var *v;
    int i;

    for (i = 0; i < array_size(vars); i++) {
        for (pos = txt;;) {
            pos = strstr(pos, vars[i]);
            if (!pos)
                break;

            /* skip the variable qualifier */
            pos = skip_nonspace(pos);
            /* whitespace */
            pos = skip_space(pos);
            /* skip the type */
            pos = skip_nonspace(pos);
            /* whitespace */
            pos = skip_space(pos);
            /* the actual variable */
            for (pend = pos; *pend && !isspace(*pend) && *pend != ';'; pend++)
                ;
            v = malloc(sizeof(*v));
            if (!v)
                return -ENOMEM;
            v->name = strndup(pos, pend - pos);
            switch (i) {
                case 1:
                    v->loc = glGetAttribLocation(p->prog, v->name);
                    break;
                case 0:
                    v->loc = glGetUniformLocation(p->prog, v->name);
                    break;
            }
            //dbg("# found var '%s' @%d\n", v->name, v->loc);
            v->next = p->var;
            p->var = v;
            pos = pend;
        }
    }

    return 0;
}

static void shader_prog_drop(struct ref *ref)
{
    struct shader_prog *p = container_of(ref, struct shader_prog, ref);

    dbg("dropping shader '%s'\n", p->name);
    free(p);
}

struct shader_prog *
shader_prog_from_strings(const char *name, const char *vsh, const char *fsh)
{
    struct shader_prog *p;

    p = ref_new(struct shader_prog, ref, shader_prog_drop);
    if (!p)
        return NULL;

    p->name = name;
    p->prog = createProgram(vsh, fsh);
    if (!p->prog) {
        err("couldn't create program '%s'\n", name);
        free(p);
        return NULL;
    }

    shader_prog_use(p);
    shader_prog_scan(p, vsh);
    shader_prog_scan(p, fsh);
    shader_prog_done(p);
    p->pos = shader_prog_find_var(p, "position");
    p->norm = shader_prog_find_var(p, "normal");
    p->tex = shader_prog_find_var(p, "tex");
    return p;
}

void shader_prog_use(struct shader_prog *p)
{
    ref_get(p);
    glUseProgram(p->prog);
}

void shader_prog_done(struct shader_prog *p)
{
    glUseProgram(0);
    ref_put(&p->ref);
}

/* XXX: or do this at build time? */
static void shader_preprocess(char *text, size_t size)
{
#ifndef CONFIG_GLES
    const char *strip[] = { "precision" };
    char *      p       = text;
    char *      end     = text + size;
    int         i;

    for (p = text; p < text + size;) {
        for (i = 0; i < array_size(strip); i++) {
            int l = strlen(strip[i]);
            if (p + l < end && !strncmp(p, strip[i], l)) {
                *p++ = '/';
                *p++ = '/';
            }
        }
        p = (char *)skip_nonspace(p);
        p = (char *)skip_space(p);
    }
#endif /* CONFIG_GLES */
}

struct shader_prog *shader_prog_find(struct shader_prog *prog, const char *name)
{
    for (; prog; prog = prog->next)
        if (!strcmp(prog->name, name))
            return ref_get(prog);

    return NULL;
}

int lib_request_shaders(const char *name, struct shader_prog **progp)
{
    //char *nvert CUX(string), *nfrag CUX(string), *vert CUX(string), *frag CUX(string);
    LOCAL(char, nvert);
    LOCAL(char, nfrag);
    LOCAL(char, vert);
    LOCAL(char, frag);
    struct shader_prog *p;
    size_t vsz, fsz;
    int ret;

    ret = asprintf(&nvert, "%s.vert", name);
    ret = asprintf(&nfrag, "%s.frag", name);
    ret = lib_read_file(RES_ASSET, nvert, (void **)&vert, &vsz);
    ret = lib_read_file(RES_ASSET, nfrag, (void **)&frag, &fsz);
    shader_preprocess(vert, vsz);
    shader_preprocess(frag, fsz);
    //dbg("%s/%s vsz %zu fsz %zu\n", nvert, nfrag, vsz, fsz);
    p           = shader_prog_from_strings(name, vert, frag);
    p->next     = *progp;
    *progp = p;

    return 0;
}
