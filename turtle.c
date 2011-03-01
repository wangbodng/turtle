#include <string.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <errno.h>
#include <glib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "haildb.h"
#include "st.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CODE_AT (__FILE__ ":" TOSTRING(__LINE__))
#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN CODE_AT

#define min(a, b) ((a) > (b) ? (b) : (a))

#define handle_error(msg) \
    do { \
        char buf[1024]; \
        strerror_r(errno, buf, sizeof(buf)); \
        g_error("%s: %s", msg, buf); \
        exit(EXIT_FAILURE); \
    } while (0);

#define handle_fatal_ib_error(err) \
    do { g_critical("%s", ib_strerror(err)); exit(EXIT_FAILURE); } while (0)

#define handle_ib_error(err) \
    do { g_error("%s", ib_strerror(err)); } while (0)

static char level_char(GLogLevelFlags level) {
    switch (level) {
        case G_LOG_LEVEL_ERROR:
            return 'E';
        case G_LOG_LEVEL_CRITICAL:
            return 'C';
        case G_LOG_LEVEL_WARNING:
            return 'W';
        case G_LOG_LEVEL_MESSAGE:
            return 'M';
        case G_LOG_LEVEL_INFO:
            return 'I';
        case G_LOG_LEVEL_DEBUG:
            return 'D';
        default:
            return 'U';
    }
    return '?';
}

static int db_log_func(ib_msg_stream_t strm, const char *format, ...) {
    (void)strm;
    va_list ap;
    va_start(ap, format);
    g_logv("innodb", G_LOG_LEVEL_INFO, format, ap);
    va_end(ap);
    return 0;
}

static void log_func(const gchar *log_domain,
    GLogLevelFlags log_level,
    const gchar *message,
    gpointer user_data)
{
    (void)user_data;
    struct tm t;
    time_t ti = st_time();
    struct timeval tv;
    uint64_t usec;
    const char *domain = "";
    gettimeofday(&tv, NULL);
    usec = (uint64_t)(tv.tv_sec * 1000000) + tv.tv_usec;
    localtime_r(&ti, &t);
    if (log_domain) {
        domain = strrchr(log_domain, '/');
        if (domain)
            ++domain;
        else
            domain = log_domain;
    }
    size_t len = strlen(message);
    gboolean addnewline = TRUE;
    if (len >= 1 && message[len-1] == '\n')
        addnewline = FALSE;
    fprintf(stderr, "%c%02d%02d %02u:%02u:%02u.%06u %5u:%05u %s] %s%s",
        level_char(log_level),
        1 + t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
        (int)st_utime(),
        (uint32_t)syscall(SYS_gettid),
        (uint32_t)((intptr_t)st_thread_self() % 100000),
        domain,
        message,
        (addnewline ? "\n" : ""));
}

static GThreadPool *wpool = NULL;
static GThreadPool *rpool = NULL;

struct query_s {
    char *buf;
    char **args;
    ssize_t len;
    int pipe_fd;
};
typedef struct query_s query_t;

static void notify_pipe(query_t *q) {
    char tmp[] = "\0";
    ssize_t nw;
    if (q->pipe_fd != -1) {
        nw = write(q->pipe_fd, tmp, 1);
        q->pipe_fd = -1; /* only write once, zero pipe so we can't write again */
        if (nw != 1) { g_error("error writing to pipe_fd: %zd", nw); }
    } else {
        g_warning("notify_pipe fd invalid");
    }
}

static void set_func(gpointer data, gpointer user_data) {
    (void)user_data;
    ib_err_t status;
    query_t *q = (query_t *)data;

    ib_trx_t trx = ib_trx_begin(IB_TRX_REPEATABLE_READ);
    ib_crsr_t crsr = NULL;

    status = ib_cursor_open_table("turtledb/store", trx, &crsr);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    status = ib_cursor_lock(crsr, IB_LOCK_IX);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    ib_tpl_t tpl = ib_clust_read_tuple_create(crsr);
    if (tpl == NULL) { goto done; }

    ib_ulint_t klen = strlen(q->args[1]);
    status = ib_col_set_value(tpl, 0, q->args[1], klen);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    ib_ulint_t vlen = strlen(q->args[2]);
    status = ib_col_set_value(tpl, 1, q->args[2], vlen);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    status = ib_cursor_insert_row(crsr, tpl);
    if (status == DB_DUPLICATE_KEY) {
        g_debug("found dup key, updating...");
        ib_tpl_t key_tpl = ib_sec_search_tuple_create(crsr);
        if (key_tpl == NULL) { goto done; }

        status = ib_col_set_value(key_tpl, 0, q->args[1], klen);
        if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

        int res = 0;
        status = ib_cursor_moveto(crsr, key_tpl, IB_CUR_GE, &res);
        ib_tpl_t old_tpl = NULL;
        ib_tpl_t new_tpl = NULL;

        ib_tuple_delete(key_tpl);

        old_tpl = ib_clust_read_tuple_create(crsr);
        if (old_tpl == NULL) { goto done; }
        new_tpl = ib_clust_read_tuple_create(crsr);
        if (new_tpl == NULL) { goto done; }

        status = ib_cursor_read_row(crsr, old_tpl);
        if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

        status = ib_tuple_copy(new_tpl, old_tpl);
        if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

        ib_ulint_t len = ib_col_get_len(old_tpl, 1);
        const char *val = ib_col_get_value(old_tpl, 1);

        if (vlen != len || memcmp(val, q->args[2], len) != 0) {
            val = strndup(val, len);
            g_debug("old val: [%s](%lu) new: [%s](%lu)\n", val, len, q->args[2], vlen);
            free((char *)val);

            status = ib_col_set_value(new_tpl, 1, q->args[2], vlen);
            if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

            status = ib_cursor_update_row(crsr, old_tpl, new_tpl);
            if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }
        } else {
            g_debug("old == new, not updating");
        }

        ib_tuple_delete(old_tpl);
        ib_tuple_delete(new_tpl);
    } else if (status != DB_SUCCESS) {
        handle_ib_error(status);
        goto done;
    }

    ib_tuple_delete(tpl);

    status = ib_cursor_close(crsr);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    status = ib_trx_commit(trx);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

done:
    notify_pipe(q);
}

static void get_func(gpointer data, gpointer user_data) {
    (void)user_data;
    ib_err_t status;
    query_t *q = (query_t *)data;

    ib_trx_t trx = ib_trx_begin(IB_TRX_REPEATABLE_READ);
    ib_crsr_t crsr = NULL;

    status = ib_cursor_open_table("turtledb/store", trx, &crsr);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    status = ib_cursor_lock(crsr, IB_LOCK_IS);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    ib_tpl_t key_tpl = ib_sec_search_tuple_create(crsr);
    if (key_tpl == NULL) { goto done; }

    status = ib_col_set_value(key_tpl, 0, q->args[1], strlen(q->args[1]));
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    int res = 0;
    status = ib_cursor_moveto(crsr, key_tpl, IB_CUR_GE, &res);
    //if (res != -1) { g_error("key (%s) not found: %d", args[1], res); goto done; }
    if (status == DB_SUCCESS) {
        ib_tpl_t old_tpl = NULL;

        old_tpl = ib_clust_read_tuple_create(crsr);
        if (old_tpl == NULL) { goto done; }

        status = ib_cursor_read_row(crsr, old_tpl);
        if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

        ib_ulint_t len = ib_col_get_len(old_tpl, 1);
        const char *val = ib_col_get_value(old_tpl, 1);

        if (val == NULL) { goto done; }
        val = strndup(val, len);
        q->len = snprintf(q->buf, 1024, "GET %s %s\n", q->args[1], val);
        free((char *)val);

        ib_tuple_delete(old_tpl);
    } else if (status == DB_END_OF_INDEX) {
        g_warning("key (%s) not found", q->args[1]);
    } else {
        handle_ib_error(status);
        goto done;
    }

    ib_tuple_delete(key_tpl);

    status = ib_cursor_close(crsr);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

    status = ib_trx_commit(trx);
    if (status != DB_SUCCESS) { handle_ib_error(status); goto done; }

done:
    notify_pipe(q);
}

static void *handle_connection(void *arg) {
    st_netfd_t nfd = (st_netfd_t)arg;
    int pipes[2];

    if (pipe(pipes) < 0) { goto done; }
    st_netfd_t p_nfd = st_netfd_open(pipes[0]);

    char buf[4*1024];
    for (;;) {
        ssize_t nr = st_read(nfd, buf, sizeof(buf), ST_UTIME_NO_TIMEOUT);
        if (nr <= 0) break;
        buf[nr] = 0;
        char **args = g_strsplit_set(buf, " \r\n", -1);
        if (g_strv_length(args) < 2) {
            g_strfreev(args);
            goto done;
        }
        query_t *q = g_slice_new(query_t);
        q->buf = buf;
        q->args = args;
        q->pipe_fd = pipes[1];
        q->len = nr;

        if (g_strcmp0("SET", args[0]) == 0) {
            if (g_strv_length(q->args) < 3) {
                goto done;
            }
            /* TODO check GError */
            GError *err = NULL;
            g_thread_pool_push(wpool, q, &err);
            if (err == NULL) {
                char tmp[1];
                ssize_t nr = st_read(p_nfd, tmp, sizeof(tmp), ST_UTIME_NO_TIMEOUT);
                g_assert(nr == 1);
            } else {
                g_error("error pushing work to thread pool");
            }
        } else if (g_strcmp0("GET", args[0]) == 0) {
            /* TODO check GError */
            GError *err = NULL;
            g_thread_pool_push(rpool, q, &err);
            if (err == NULL) {
                char tmp[1];
                ssize_t nr = st_read(p_nfd, tmp, sizeof(tmp), ST_UTIME_NO_TIMEOUT);
                g_assert(nr == 1);
            } else {
                g_error("error pushing work to thread pool");
            }
        }
        g_strfreev(args);
        ssize_t nw = st_write(nfd, q->buf, q->len, ST_UTIME_NO_TIMEOUT);
        g_slice_free(query_t, q);
        if (nw != q->len) break;
    }
done:
    g_message("closing client");
    st_netfd_close(nfd);
    return NULL;
}

static void *accept_loop(void *arg) {
    st_netfd_t server_nfd = (st_netfd_t)arg;
    st_netfd_t client_nfd;
    struct sockaddr_in from;
    int fromlen = sizeof(from);

    for (;;) {
        client_nfd = st_accept(server_nfd,
            (struct sockaddr *)&from, &fromlen, ST_UTIME_NO_TIMEOUT);
        g_message("accepted");
        if (st_thread_create(handle_connection,
            (void *)client_nfd, 0, 1024 * 1024) == NULL)
        {
            handle_error("st_thread_create");
        }
    }
    return NULL;
}

static st_thread_t listen_server(void) {
    int sock;
    int n;
    struct sockaddr_in serv_addr;

    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        handle_error("socket");
    }

    n = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0) {
        handle_error("setsockopt SO_REUSEADDR");
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(5555);
    serv_addr.sin_addr.s_addr = inet_addr("0.0.0.0");

    if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        handle_error("bind");
    }

    if (listen(sock, 10) < 0) {
        handle_error("listen");
    }

    st_netfd_t server_nfd = st_netfd_open_socket(sock);
    return st_thread_create(accept_loop, (void *)server_nfd, 0, 1024 * 1024);
}

int main(int argc, char *argv[]) {
    sigset_t mask;
    int sfd;
    struct signalfd_siginfo fdsi;
    struct timeval tv;

    /* seed random */
    gettimeofday(&tv, NULL);
    srandom(tv.tv_usec);

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);

    /* Block signals so that they aren't handled                                                                                                                                                                                
       according to their default dispositions */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) { handle_error("sigprocmask"); }
    /* i don't use SFD_NONBLOCK here because st_netfd_open will set O_NONBLOCK */
    sfd = signalfd(-1, &mask, 0);
    if (sfd == -1) { handle_error("signalfd"); }
    /* init glib threads */
    g_thread_init(NULL);

    g_log_set_default_handler(log_func, NULL);

    /* init state threads */
    if (st_init() < 0) { handle_error("state threads"); }

    ib_err_t status;
    status = ib_init();
    if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

    ib_logger_set(db_log_func, NULL);

    status = ib_startup("barracuda");
    if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

    if (ib_database_create("turtledb")) {
        ib_tbl_sch_t sch = NULL;
        ib_idx_sch_t idx_sch = NULL;
        /* create schema */
        status = ib_table_schema_create("turtledb/store", &sch, IB_TBL_COMPACT, 0);
        if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

        status = ib_table_schema_add_col(sch, "tkey", IB_VARBINARY, IB_COL_NOT_NULL, 0, 1024);
        if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

        status = ib_table_schema_add_col(sch, "tvalue", IB_BLOB, IB_COL_NONE, 0, 16*1024*1024);
        if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

        /* create index */
        status = ib_table_schema_add_index(sch, "PRIMARY", &idx_sch);
        if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

        status = ib_index_schema_add_col(idx_sch, "tkey", 0);
        if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

        status = ib_index_schema_set_clustered(idx_sch);
        if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

        /* create table */
        ib_trx_t trx = ib_trx_begin(IB_TRX_REPEATABLE_READ);
        status = ib_schema_lock_exclusive(trx);
        if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

        ib_id_t tid;
        status = ib_table_create(trx, sch, &tid);
        if (status == DB_SUCCESS) {
            status = ib_trx_commit(trx);
            if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }
            g_message("store table created!");
        } else if (status == DB_TABLE_IS_BEING_USED) {
            g_message("store table exists");
            status = ib_trx_rollback(trx);
            if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }
        } else {
            handle_ib_error(status);
            status = ib_trx_rollback(trx);
            if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }
            goto shutdown;
        }

        if (sch != NULL) ib_table_schema_delete(sch);
    }

    wpool = g_thread_pool_new(set_func, NULL, 10, FALSE, NULL);
    if (wpool == NULL) { handle_error("creating thread pool failed"); }
    rpool = g_thread_pool_new(get_func, NULL, 10, FALSE, NULL);
    if (rpool == NULL) { handle_error("creating thread pool failed"); }

    listen_server();

    st_netfd_t sig_nfd = st_netfd_open(sfd);
    if (sig_nfd == NULL) { handle_error("st_netfd_open sig fd"); }

    ssize_t nr = st_read_fully(sig_nfd, &fdsi, sizeof(fdsi), ST_UTIME_NO_TIMEOUT);
    if (nr != sizeof(fdsi)) { handle_error("read sig struct"); }

    switch (fdsi.ssi_signo) {
        case SIGINT:
            g_debug("got SIGINT\n");
            break;
        case SIGQUIT:
            g_debug("got SIGQUIT\n");
            break;
        default:
            g_debug("unexpected signal: %d\n", fdsi.ssi_signo);
            break;
    }

shutdown:
    status = ib_shutdown(IB_SHUTDOWN_NORMAL);
    if (status != DB_SUCCESS) { handle_fatal_ib_error(status); }

    exit(EXIT_SUCCESS);
    /* TODO: should probably properly exit all state threads */
    st_thread_exit(NULL);
    g_warn_if_reached();
    return EXIT_FAILURE;

}