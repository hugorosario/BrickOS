#define _GNU_SOURCE
#include "bridge.h"
#include "gesture.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *(*lookup)(void *,const char *);
static void (*run_core)(void);
static bool (*load_core)(const void *,size_t);
static pthread_once_t lookup_once=PTHREAD_ONCE_INIT,session_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t pending_lock=PTHREAD_MUTEX_INITIALIZER;
static struct gs_session session;
static struct gs_store store;
static atomic_int enabled,initial_resume;
static int hello;
static atomic_int in_menu,active_owed;
static struct gs_stage pending;
static unsigned char *expected;
static size_t expected_bytes;
static struct gs_entry selected;
static char load_transaction[GS_ID];

static void open_lookup(void) {
#ifdef __aarch64__
    lookup=dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.17");
#else
    lookup=dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.2.5");
#endif
}
static void open_session(void) {
    pending.directory=pending.lock=-1;
    if (gs_session_environment(&session,gs_observer_session) || gs_store_open_namespace(&store,session.sd,session.store_name)) return;
    if (gs_gesture_start()) { gs_store_close(&store); return; }
    enabled=1;
    initial_resume=getenv("BRICKOS_GS_RESUME")!=NULL;
    atomic_store(&active_owed,initial_resume ? 0:1);
}
struct gs_session *gs_observer_session(void) { pthread_once(&session_once,open_session); return enabled?&session:NULL; }
struct gs_store *gs_observer_store(void) { return enabled?&store:NULL; }
int gs_bridge_enabled(void) {
    struct gs_session *s=gs_observer_session();
    return s && run_core && load_core && !gs_process_matches(s->pid,s->process_start,&s->identity,s->append);
}
void gs_observer_menu(int active,int resume) {
    atomic_store(&in_menu,active);
    atomic_store(&active_owed,!active && resume);
}
void gs_observer_disable(void) { enabled=0; atomic_store(&active_owed,0); }
int gs_observer_load(struct gs_stage *stage,unsigned char *memory,size_t bytes,
                     const struct gs_entry *entry,const char *transaction) {
    pthread_mutex_lock(&pending_lock);
    if (expected || pending.directory>=0 || !load_core) { pthread_mutex_unlock(&pending_lock); return -1; }
    pending=*stage; stage->directory=stage->lock=-1;
    expected=memory; expected_bytes=bytes; selected=*entry; memcpy(load_transaction,transaction,GS_ID);
    pthread_mutex_unlock(&pending_lock); return 0;
}
int gs_observer_recovery_quit(void) {
    pthread_mutex_lock(&pending_lock);
    if (!initial_resume) { pthread_mutex_unlock(&pending_lock); return -1; }
    if (pending.directory>=0) {
        (void)unlinkat(pending.directory,pending.state,0); gs_stage_close(&pending);
    }
    free(expected); expected=NULL; expected_bytes=0;
    atomic_store(&active_owed,0);
    pthread_mutex_unlock(&pending_lock); return 0;
}
static void observed_run(void) {
    run_core();
    if (!hello) {
        hello=1;
        if (gs_message_send(&session,GS_HELLO,1,NULL,NULL)) enabled=0;
    }
    if (enabled && !atomic_load(&in_menu) && atomic_exchange(&active_owed,0))
        (void)gs_message_send(&session,GS_ACTIVE,1,NULL,NULL);
}
static bool observed_load(const void *memory,size_t bytes) {
    pthread_mutex_lock(&pending_lock);
    int matched=expected && bytes==expected_bytes && !memcmp(memory,expected,bytes);
    pthread_mutex_unlock(&pending_lock);
    bool accepted=load_core(memory,bytes);
    pthread_mutex_lock(&pending_lock);
    if (expected) {
        int success=accepted && matched && gs_monotonic_ms()<pending.deadline_ms;
        fprintf(stderr,"GameSwitcher load accepted=%d payload_exact=%d success=%d bytes=%zu\n",accepted,matched,success,bytes);
        (void)gs_message_send(&session,GS_LOAD_RESULT,success,&selected,load_transaction);
        if (success) {
            // The core consumed the exact payload; staging can now be released.
            (void)unlinkat(pending.directory,pending.state,0);
            gs_stage_close(&pending); initial_resume=0; atomic_store(&active_owed,1);
        }
        // Rejected/mismatched loads leave staging reserved until orderly recovery.
        free(expected); expected=NULL; expected_bytes=0;
    }
    pthread_mutex_unlock(&pending_lock); return accepted;
}
__attribute__((visibility("default")))
void *dlsym(void *handle,const char *symbol) {
    pthread_once(&lookup_once,open_lookup);
    if (!lookup) return NULL;
    void *value=lookup(handle,symbol);
    if (!value || (strcmp(symbol,"retro_run") && strcmp(symbol,"retro_unserialize"))) return value;
    struct gs_session *s=gs_observer_session();
    if (!s || gs_session_core(s,value)) return value;
    if (!strcmp(symbol,"retro_run")) { run_core=value; return observed_run; }
    load_core=value; return observed_load;
}
