#ifndef BRICKOS_GS_POWER_CONTROL_H
#define BRICKOS_GS_POWER_CONTROL_H
#include "session.h"
#define GS_POWER_MAGIC UINT32_C(0x47535003)
#define GS_POWER_LEASE_MS 15000
#define GS_POWER_RECEIPT_MS 500
enum gs_power_kind { GS_POWER_LEASE=1, GS_POWER_SHORT, GS_POWER_LONG, GS_POWER_RESULT, GS_POWER_YIELDED, GS_POWER_RECLAIMED };
struct gs_power_message {
    uint32_t magic,kind,busy,success;
    uint64_t issued_ms,coordinator_start;
    uint32_t coordinator_pid,standby;
    char transaction[GS_ID];
    struct gs_session session;
};
struct gs_power_server {
    int listener,client;
    pid_t client_pid;
    uint64_t client_start,sent_ms,process_start;
    char socket_path[108];
};
struct gs_power_client {
    int fd,valid,pending;
    pid_t coordinator_pid;
    uint64_t coordinator_start,sent_ms;
    char path[108];
    struct gs_power_message lease,request;
};
struct gs_power_transactions {
    struct { char issuer[17]; uint64_t sequence; uint32_t kind; int success; } issuers[16];
    unsigned count;
};
// Monotonic stroke numbers per random inputd lifetime. Older replay never
// becomes fresh, even after the result for a newer stroke replaced it.
int gs_power_transaction_begin(struct gs_power_transactions *transactions,const struct gs_power_message *request,int *success);
void gs_power_transaction_finish(struct gs_power_transactions *transactions,const struct gs_power_message *request,int success);
int gs_power_wire_valid(const struct gs_power_message *message,uint64_t now);
// Receive and validate against the clock after recv, not an earlier loop scan.
int gs_power_receive(int fd,struct gs_power_message *message);
int gs_power_lease_valid(const struct gs_power_message *message);
int gs_power_server_open(struct gs_power_server *server,const char *directory);
/* Publishes only a validated live-session lease. Returns one fresh request,
 * zero when idle, or -1 for a malformed/disconnected peer (the session survives). */
int gs_power_server_poll(struct gs_power_server *server,const struct gs_session *session,
                         int eligible,int busy,int standby,struct gs_power_message *request,uint64_t now);
int gs_power_server_result(struct gs_power_server *server,const struct gs_power_message *request,int success,uint64_t now);
void gs_power_server_close(struct gs_power_server *server);
int gs_power_client_init(struct gs_power_client *client,const char *directory);
int gs_power_client_poll(struct gs_power_client *client,uint64_t now);
int gs_power_client_request(struct gs_power_client *client,enum gs_power_kind kind,const char *transaction,uint64_t now);
int gs_power_client_yielded(struct gs_power_client *client,uint64_t now);
int gs_power_client_reclaimed(struct gs_power_client *client,uint64_t now);
void gs_power_client_close(struct gs_power_client *client);
#endif
