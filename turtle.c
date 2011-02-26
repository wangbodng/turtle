#include <string.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <errno.h>
#include <glib.h>
#include "haildb.h"
#include "st.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CODE_AT (__FILE__ ":" TOSTRING(__LINE__))
#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN CODE_AT

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

    st_thread_exit(NULL);
    g_warn_if_reached();
    return EXIT_FAILURE;

}