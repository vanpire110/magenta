#include "libc.h"
#include <pthread.h>

#include <runtime/mutex.h>

static struct atfork_funcs {
    void (*prepare)(void);
    void (*parent)(void);
    void (*child)(void);
    struct atfork_funcs *prev, *next;
} * funcs;

static mxr_mutex_t lock;

void __fork_handler(int who) {
    struct atfork_funcs* p;
    if (!funcs)
        return;
    if (who < 0) {
        mxr_mutex_lock(&lock);
        for (p = funcs; p; p = p->next) {
            if (p->prepare)
                p->prepare();
            funcs = p;
        }
    } else {
        for (p = funcs; p; p = p->prev) {
            if (!who && p->parent)
                p->parent();
            else if (who && p->child)
                p->child();
            funcs = p;
        }
        mxr_mutex_unlock(&lock);
    }
}

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    struct atfork_funcs* new = malloc(sizeof *new);
    if (!new)
        return -1;

    mxr_mutex_lock(&lock);
    new->next = funcs;
    new->prev = 0;
    new->prepare = prepare;
    new->parent = parent;
    new->child = child;
    if (funcs)
        funcs->prev = new;
    funcs = new;
    mxr_mutex_unlock(&lock);
    return 0;
}
