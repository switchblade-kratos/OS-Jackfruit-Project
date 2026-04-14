/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Complete implementation covering:
 *   - Task 1: Multi-container runtime with parent supervisor
 *   - Task 2: Supervisor CLI and signal handling (UNIX socket IPC)
 *   - Task 3: Bounded-buffer logging with producer/consumer threads
 *   - Task 4: Kernel module integration via ioctl
 *   - Task 6: Resource cleanup
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ─── Constants ──────────────────────────────────────────────── */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MSG_LEN     256
#define CHILD_CMD_LEN       256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 64          /* bounded buffer slots */
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MAX_CONTAINERS      32
#define MONITOR_DEV         "/dev/container_monitor"

/* ─── Command / State Enums ──────────────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_HARD_LIMIT_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ─── Container Metadata ─────────────────────────────────────── */
typedef struct container_record {
    int    used;
    char   id[CONTAINER_ID_LEN];
    pid_t  host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int    exit_code;
    int    exit_signal;
    int    stop_requested;   /* set before signalling from 'stop' command */
    char   log_path[PATH_MAX];
    int    pipe_read_fd;     /* supervisor reads container output from here */
    pthread_t producer_tid;
} container_record_t;

/* ─── Bounded Log Buffer ─────────────────────────────────────── */
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t     head;
    size_t     tail;
    size_t     count;
    int        shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

/* ─── IPC Structs ────────────────────────────────────────────── */
typedef struct {
    command_kind_t kind;
    char   container_id[CONTAINER_ID_LEN];
    char   rootfs[PATH_MAX];
    char   command[CHILD_CMD_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int    nice_value;
} control_request_t;

typedef struct {
    int  status;       /* 0 = ok, non-zero = error */
    char message[CONTROL_MSG_LEN];
} control_response_t;

/* ─── Child Config (passed through clone stack) ──────────────── */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_CMD_LEN];
    int  nice_value;
    int  pipe_write_fd;  /* child writes stdout/stderr here */
} child_config_t;

/* ─── Supervisor Context ─────────────────────────────────────── */
typedef struct {
    int    server_fd;
    int    monitor_fd;
    volatile int should_stop;
    pthread_t    consumer_tid;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t containers[MAX_CONTAINERS];
} supervisor_ctx_t;

/* Producer thread arg */
typedef struct {
    supervisor_ctx_t   *ctx;
    container_record_t *rec;
} producer_arg_t;

/* Global supervisor pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ═══════════════════════════════════════════════════════════════
 *  USAGE
 * ═══════════════════════════════════════════════════════════════ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ═══════════════════════════════════════════════════════════════
 *  ARGUMENT PARSING HELPERS
 * ═══════════════════════════════════════════════════════════════ */
static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    errno = 0;
    unsigned long mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                 char *argv[], int start_index)
{
    for (int i = start_index; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end = NULL;
            errno = 0;
            long nv = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid --nice value (must be -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    case CONTAINER_EXITED:            return "exited";
    default:                          return "unknown";
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  BOUNDED BUFFER  (Task 3)
 *
 *  Why mutex + condition variables?
 *  - Producers block (not spin) when the buffer is full → no wasted CPU
 *  - Consumers block when empty → no busy-wait
 *  - pthread_cond_broadcast on shutdown wakes all waiters so threads
 *    can exit promptly without deadlocking
 *
 *  Race conditions without synchronisation:
 *  - head/tail/count are read-modify-write; concurrent updates corrupt them
 *  - a producer could overwrite a slot the consumer hasn't read yet
 *  - shutdown flag set without broadcast could leave threads sleeping forever
 * ═══════════════════════════════════════════════════════════════ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    int rc;
    if ((rc = pthread_mutex_init(&b->mutex, NULL)) != 0) return rc;
    if ((rc = pthread_cond_init(&b->not_empty, NULL)) != 0) {
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    if ((rc = pthread_cond_init(&b->not_full, NULL)) != 0) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* Returns 0 on success, -1 if shutting down */
static int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* Returns 1 if item retrieved, 0 if shutdown and buffer empty */
static int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0) {   /* shutdown + empty → done */
        pthread_mutex_unlock(&b->mutex);
        return 0;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  LOG CONSUMER THREAD  (Task 3)
 *  Drains the bounded buffer and writes to per-container log files.
 * ═══════════════════════════════════════════════════════════════ */
static void *log_consumer(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item)) {
        /* Find the log path for this container */
        char log_path[PATH_MAX] = {0};
        pthread_mutex_lock(&ctx->metadata_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (ctx->containers[i].used &&
                strcmp(ctx->containers[i].id, item.container_id) == 0) {
                snprintf(log_path, sizeof(log_path), "%s",
                         ctx->containers[i].log_path);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0') continue;

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;
        if (write(fd, item.data, item.length) < 0) { /* log write error, non-fatal */ }
        close(fd);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 *  LOG PRODUCER THREAD  (Task 3)
 *  Reads from the container pipe and pushes into the bounded buffer.
 * ═══════════════════════════════════════════════════════════════ */
static void *log_producer(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    supervisor_ctx_t *ctx = pa->ctx;
    container_record_t *rec = pa->rec;
    free(pa);

    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(rec->pipe_read_fd, buf, sizeof(buf) - 1)) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        snprintf(item.container_id, sizeof(item.container_id), "%s", rec->id);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        bounded_buffer_push(&ctx->log_buffer, &item);
    }

    close(rec->pipe_read_fd);
    rec->pipe_read_fd = -1;
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 *  CONTAINER CHILD ENTRY POINT  (Task 1)
 * ═══════════════════════════════════════════════════════════════ */
static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the pipe write end */
    if (dup2(cfg->pipe_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->pipe_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->pipe_write_fd);

    /* Set hostname to container ID (UTS namespace) */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");  /* non-fatal */

    /* Apply nice value if requested */
/* Apply nice value if requested */
    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0) {
            perror("nice");
        }
    }
    /* chroot into the container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc so ps, top etc work inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        /* Non-fatal — /proc may already be mounted or not needed */
        perror("mount /proc");
    }

    /* Execute the requested command */

    char *args[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", args);
    
    /* execv only returns on error */
    perror("execv");
    return 1;
}


/* ═══════════════════════════════════════════════════════════════
 *  KERNEL MODULE HELPERS  (Task 4)
 * ═══════════════════════════════════════════════════════════════ */
static int register_with_monitor(int fd, const char *cid, pid_t pid,
                                  unsigned long soft, unsigned long hard)
{
    if (fd < 0) return 0;  /* module not loaded — skip silently */
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, cid, sizeof(req.container_id) - 1);
    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl MONITOR_REGISTER");
        return -1;
    }
    return 0;
}

static int unregister_from_monitor(int fd, const char *cid, pid_t pid)
{
    if (fd < 0) return 0;
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, cid, sizeof(req.container_id) - 1);
    ioctl(fd, MONITOR_UNREGISTER, &req);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  REAP CHILDREN  (Task 1 + Task 6)
 * ═══════════════════════════════════════════════════════════════ */
static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            container_record_t *r = &ctx->containers[i];
            if (!r->used || r->host_pid != pid) continue;

            if (WIFEXITED(status)) {
                r->exit_code = WEXITSTATUS(status);
                r->state     = CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                r->exit_signal = WTERMSIG(status);
                /* Classify termination reason */
                if (r->stop_requested) {
                    r->state = CONTAINER_STOPPED;
                } else if (r->exit_signal == SIGKILL) {
                    r->state = CONTAINER_HARD_LIMIT_KILLED;
                } else {
                    r->state = CONTAINER_KILLED;
                }
            }

            unregister_from_monitor(ctx->monitor_fd, r->id, pid);
            fprintf(stdout,
                    "[SUPERVISOR] container %s (pid %d) exited: state=%s\n",
                    r->id, pid, state_to_string(r->state));
            fflush(stdout);
            break;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  SIGNAL HANDLERS
 * ═══════════════════════════════════════════════════════════════ */
static void handle_sigchld(int sig)
{
    (void)sig;
    /* reaping done in main loop via WNOHANG */
}

static void handle_sigterm(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  LAUNCH ONE CONTAINER  (Task 1)
 * ═══════════════════════════════════════════════════════════════ */
static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             container_record_t **out_rec)
{
    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (ctx->containers[i].used &&
            strcmp(ctx->containers[i].id, req->container_id) == 0 &&
            ctx->containers[i].state == CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            return -EEXIST;
        }
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!ctx->containers[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        return -ENOMEM;
    }

    container_record_t *r = &ctx->containers[slot];
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->pipe_read_fd = -1;
    snprintf(r->id, sizeof(r->id), "%s", req->container_id);
    r->soft_limit_bytes = req->soft_limit_bytes;
    r->hard_limit_bytes = req->hard_limit_bytes;
    r->state            = CONTAINER_STARTING;
    r->started_at       = time(NULL);

    /* Build log path */
    mkdir(LOG_DIR, 0755);
    snprintf(r->log_path, sizeof(r->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create pipe for container stdout/stderr */
    int pipefd[2];
    if (pipe(pipefd) < 0) return -errno;

    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { close(pipefd[0]); close(pipefd[1]); return -ENOMEM; }

    /* Set up child config */
    child_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        free(stack); close(pipefd[0]); close(pipefd[1]); return -ENOMEM;
    }
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->id, sizeof(cfg->id), "%s", r->id);
    snprintf(cfg->rootfs, sizeof(cfg->rootfs), "%s", req->rootfs);
    snprintf(cfg->command, sizeof(cfg->command), "%s", req->command);
    cfg->nice_value    = req->nice_value;
    cfg->pipe_write_fd = pipefd[1];

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);

    free(stack);
    close(pipefd[1]);   /* supervisor doesn't need write end */

    if (pid < 0) {
        free(cfg);
        close(pipefd[0]);
        return -errno;
    }
    free(cfg);  /* cfg was copied into the child's stack before clone returns */

    pthread_mutex_lock(&ctx->metadata_lock);
    r->host_pid     = pid;
    r->pipe_read_fd = pipefd[0];
    r->state        = CONTAINER_RUNNING;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel module */
    register_with_monitor(ctx->monitor_fd, r->id, pid,
                          req->soft_limit_bytes, req->hard_limit_bytes);

    /* Spawn producer thread for this container's pipe */
    producer_arg_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->ctx = ctx;
        pa->rec = r;
        pthread_create(&r->producer_tid, NULL, log_producer, pa);
    }

    if (out_rec) *out_rec = r;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  HANDLE ONE CLI REQUEST  (Task 2)
 * ═══════════════════════════════════════════════════════════════ */static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t  req;
    control_response_t resp;

    memset(&resp, 0, sizeof(resp));

    ssize_t n = recv(client_fd,
                     &req,
                     sizeof(req),
                     0);

    if (n != sizeof(req)) {

        resp.status = -1;
        snprintf(resp.message,
                 sizeof(resp.message),
                 "bad request size");

        send(client_fd,
             &resp,
             sizeof(resp),
             0);

        return;
    }

    printf("Client connected!\n");
    printf("Received command kind: %d\n",
           req.kind);

    switch (req.kind) {

    case CMD_START: {

        printf("Handling START command for %s\n",
               req.container_id);

        int rc = launch_container(ctx,
                                  &req,
                                  NULL);

        if (rc == 0) {

            printf("Container '%s' started.\n",
                   req.container_id);

            resp.status = 0;

            snprintf(resp.message,
                     sizeof(resp.message),
                     "started container %s",
                     req.container_id);
        }
        else {

            printf("Failed to start container '%s'\n",
                   req.container_id);

            resp.status = rc;

            snprintf(resp.message,
                     sizeof(resp.message),
                     "start failed");
        }

        break;
    }

    case CMD_RUN: {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Run not implemented. Use 'start' instead.");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_PS: {
        /* This prints to Terminal 1 (The Supervisor console) */
        printf("\n--- Active Containers ---\n");
        printf("%-12s %-8s %-20s %-12s %-12s\n", "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)");
        printf("----------------------------------------------------------------------\n");
        
        pthread_mutex_lock(&ctx->metadata_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (ctx->containers[i].used) {
                /* Map the enum number to a string */
                const char* state_str = "UNKNOWN";
                switch (ctx->containers[i].state) {
                    case CONTAINER_STARTING:          state_str = "STARTING"; break;
                    case CONTAINER_RUNNING:           state_str = "RUNNING"; break;
                    case CONTAINER_STOPPED:           state_str = "STOPPED"; break;
                    case CONTAINER_KILLED:            state_str = "KILLED"; break;
                    case CONTAINER_HARD_LIMIT_KILLED: state_str = "HARD_LIMIT_KILLED"; break;
                    case CONTAINER_EXITED:            state_str = "EXITED"; break;
                }

                /* Convert bytes to MiB for easier reading in the terminal */
                unsigned long soft_mib = ctx->containers[i].soft_limit_bytes / (1024 * 1024);
                unsigned long hard_mib = ctx->containers[i].hard_limit_bytes / (1024 * 1024);

                printf("%-12s %-8d %-20s %-12lu %-12lu\n",
                    ctx->containers[i].id,
                    ctx->containers[i].host_pid,
                    state_str,
                    soft_mib,
                    hard_mib);
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Send a simple confirmation back to the CLI (Terminal 2) */
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "PS table updated in supervisor terminal.");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }
	case CMD_STOP: {

		printf("Handling STOP command for container: %s\n",
		       req.container_id);

		pthread_mutex_lock(&ctx->metadata_lock);

		container_record_t *r = NULL;

		/* Find container by ID only */

		for (int i = 0; i < MAX_CONTAINERS; i++) {

		    if (ctx->containers[i].used &&
		        strcmp(ctx->containers[i].id,
		               req.container_id) == 0) {

		        r = &ctx->containers[i];
		        break;
		    }
		}

		if (!r) {

		    pthread_mutex_unlock(&ctx->metadata_lock);

		    printf("Container '%s' not found\n",
		           req.container_id);

		    resp.status = -1;

		    snprintf(resp.message,
		             sizeof(resp.message),
		             "container not found");

		    break;
		}

		/* Now check if running */

		if (r->state != CONTAINER_RUNNING) {

		    pthread_mutex_unlock(&ctx->metadata_lock);

		    printf("Container '%s' not running\n",
		           req.container_id);

		    resp.status = -1;

		    snprintf(resp.message,
		             sizeof(resp.message),
		             "container not running");

		    break;
		}

		pid_t pid = r->host_pid;

		printf("Stopping container '%s' PID %d\n",
		       r->id,
		       pid);

		r->stop_requested = 1;

		pthread_mutex_unlock(&ctx->metadata_lock);

		/* Force kill */


		kill(pid, SIGKILL);

		printf("Container '%s' killed successfully\n",
		       req.container_id);

		resp.status = 0;

		printf("Container '%s' killed successfully\n",
		       req.container_id);

		resp.status = 0;

		snprintf(resp.message,
		         sizeof(resp.message),
		         "container stopped");

		break;
	}
	case CMD_LOGS: {
        printf("Handling LOGS command for %s\n", req.container_id);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "--- Logs for %s ---", req.container_id);
        
        /* 1. Send the standard response header first */
        send(client_fd, &resp, sizeof(resp), 0);

        /* 2. Find the log file path */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);

        /* 3. Open the file and stream it directly over the socket to the client */
        int fd = open(log_path, O_RDONLY);
        if (fd >= 0) {
            char file_buf[1024];
            ssize_t bytes_read;
            while ((bytes_read = read(fd, file_buf, sizeof(file_buf))) > 0) {
                send(client_fd, file_buf, bytes_read, 0);
            }
            close(fd);
        } else {
            char *err_msg = "[No logs found or container hasn't produced output yet]\n";
            send(client_fd, err_msg, strlen(err_msg), 0);
        }

        /* 4. Return early so we don't accidentally send the response twice */
        return;
    }


    default:

        printf("Unknown command received: %d\n",
               req.kind);

        resp.status = -1;

        snprintf(resp.message,
                 sizeof(resp.message),
                 "unknown command");

        break;
    }

    send(client_fd,
         &resp,
         sizeof(resp),
         0);
}

/* ═══════════════════════════════════════════════════════════════
 *  SUPERVISOR MAIN LOOP  (Task 2)
 * ═══════════════════════════════════════════════════════════════ */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Init mutex */
    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    /* Init bounded buffer */
    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("bounded_buffer_init");
        return 1;
    }

    /* Open kernel module if available */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr,
                "[SUPERVISOR] note: %s not found, memory monitoring disabled\n",
                MONITOR_DEV);
        ctx.monitor_fd = -1;
    }

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* Create UNIX domain socket for CLI IPC (Path B) */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(ctx.server_fd); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); close(ctx.server_fd); return 1;
    }

    /* Install signal handlers */
    struct sigaction sa_chld = { .sa_handler = handle_sigchld,
                                 .sa_flags   = SA_RESTART | SA_NOCLDSTOP };
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = { .sa_handler = handle_sigterm,
                                 .sa_flags   = SA_RESTART };
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* Start log consumer thread */
    pthread_create(&ctx.consumer_tid, NULL, log_consumer, &ctx);

	fprintf(stdout, "\n[SUPERVISOR] Daemon successfully started.\n");
    fprintf(stdout, "  -> Initializing from: %s\n", rootfs);
    fprintf(stdout, "  -> Listening on:      %s\n\n", CONTROL_PATH);
    fflush(stdout);
    
    /* Non-blocking accept loop */
    fd_set rfds;
    struct timeval tv;
    while (!ctx.should_stop) {
        reap_children(&ctx);

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 200000;

        int rc = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (rc == 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            perror("accept"); break;
        }
        handle_client(&ctx, client_fd);
        close(client_fd);
    }

    /* ── Orderly Shutdown (Task 6) ── */
    fprintf(stdout, "[SUPERVISOR] shutting down...\n"); fflush(stdout);

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (ctx.containers[i].used &&
            ctx.containers[i].state == CONTAINER_RUNNING) {
            ctx.containers[i].stop_requested = 1;
            kill(ctx.containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for children */
    while (waitpid(-1, NULL, WNOHANG) > 0 || errno != ECHILD) {
        reap_children(&ctx);
        usleep(100000);
        int any = 0;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++)
            if (ctx.containers[i].used &&
                ctx.containers[i].state == CONTAINER_RUNNING) { any = 1; break; }
        pthread_mutex_unlock(&ctx.metadata_lock);
        if (!any) break;
    }

    /* Join producer threads */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (ctx.containers[i].used && ctx.containers[i].producer_tid) {
            pthread_t tid = ctx.containers[i].producer_tid;
            pthread_mutex_unlock(&ctx.metadata_lock);
            pthread_join(tid, NULL);
            pthread_mutex_lock(&ctx.metadata_lock);
            ctx.containers[i].producer_tid = 0;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Drain and stop log consumer */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.consumer_tid, NULL);

    /* Cleanup */
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stdout, "[SUPERVISOR] clean exit\n"); fflush(stdout);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  CLI CLIENT  (Task 2 — Path B)
 * ═══════════════════════════════════════════════════════════════ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s. Is the supervisor running?\n",
                CONTROL_PATH);
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    control_response_t resp;
    ssize_t n = recv(fd, &resp, sizeof(resp), 0);
    if (n <= 0) { perror("recv"); close(fd); return 1; }

    printf("%s\n", resp.message);

    /* For 'logs', read any extra data the supervisor streams back */
    if (req->kind == CMD_LOGS && resp.status == 0) {
        char chunk[CONTROL_MSG_LEN];
        ssize_t r;
        while ((r = recv(fd, chunk, sizeof(chunk), 0)) > 0)
            fwrite(chunk, 1, (size_t)r, stdout);
    }

    close(fd);
    return resp.status != 0 ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  CLI ENTRY POINTS
 * ═══════════════════════════════════════════════════════════════ */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}


static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("Please use the 'start' command to launch containers.\n");
    return 0;
}

static int cmd_ps(int argc, char *argv[]) {
    (void)argc; (void)argv;
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}
static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ═══════════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
	if (strcmp(argv[1], "run")    == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")     == 0) return cmd_ps(argc, argv);
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
} 
