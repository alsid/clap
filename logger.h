#ifndef __CLAP_LOGGER_H__
#define __CLAP_LOGGER_H__

#ifndef MODNAME
#define MODNAME __BASE_FILE__
#endif

#include <sys/types.h>
#include <assert.h>
#include <time.h>

#define LOG_RB_MAX 512
#define LOG_STDIO   1
#define LOG_RB      2
#define LOG_QUIET   4
#define LOG_DEFAULT  (LOG_STDIO)
#define LOG_FULL     (LOG_STDIO | LOG_RB)

enum {
    FTRACE = -3,
    VDBG = -2,
    DBG,
    NORMAL = 0,
    WARN,
    ERR,
};

extern unsigned int abort_on_error;

struct log_entry {
    struct timespec ts;     /* timestamp */
    const char      *mod;   /* module */
    const char      *func;  /* function */
    char            *msg;   /* payload */
    int             line;
    int             level;
};

int rb_sink_add(void (*flush)(struct log_entry *e, void *data), void *data, int filter, int fill);
void rb_sink_del(void *data);

void hexdump(unsigned char *buf, size_t size);
void log_init(unsigned int flags);
void logg(int level, const char *mod, int line, const char *func, char *fmt, ...);
#define trace(args...) \
    logg(VDBG, MODNAME, __LINE__, __func__, ## args);
#define trace_on(_c, args...) do { if ((_c)) trace("condition '" # _c "': " args); } while (0)
#define dbg(args...) \
    logg(DBG, MODNAME, __LINE__, __func__, ## args);

#define dbg_on(_c, args...) do { if ((_c)) dbg("condition '" # _c "': " args); } while (0)
#define dbg_once(args...) do { static int __printed = 0; if (!__printed++) dbg(args) } while (0)
#define msg(args...) \
    logg(NORMAL, MODNAME, __LINE__, __func__, ## args);
#define warn(args...) \
    logg(WARN, MODNAME, __LINE__, __func__, ## args);
#define warn_on(_c, args...) do { if ((_c)) warn("condition '" # _c "': " args); } while (0)
#define err(args...) \
    logg(ERR, MODNAME, __LINE__, __func__, ## args);
#define err_on_cond(_c, _cc, args...) do { if ((_c)) { err("condition '" # _cc "': " args); assert(!abort_on_error); } } while (0)
#define err_on(_c, args...) err_on_cond(_c, __stringify(_c), ## args)

#endif /* __CLAP_LOGGER_H__ */
