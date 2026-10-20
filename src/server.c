/* Banking Management System server.
 *
 * The parent accepts TCP connections and fork()s one handler process per
 * client. Handlers share the data files (fcntl record locks) and the session
 * table (System V shared memory + semaphore). On SIGINT/SIGTERM the parent
 * stops its handlers, waits for them, and only then removes the IPC objects. */
#include "journal.h"
#include "net.h"
#include "ops.h"
#include "session.h"
#include "store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long handlers get to finish their current operation on shutdown. */
#define SHUTDOWN_GRACE_MS 3000

static volatile sig_atomic_t stop_requested;
static int current_uid; /* logged-in user of this handler, 0 if none */

/* Live handler pids. A pid stays here until reaped, so it cannot be reused
 * by an unrelated process while we might still signal it. */
static pid_t *children;
static size_t nchildren, children_cap;
static int child_crashed; /* a handler died abnormally: replay the journal */

static void log_msg(const char *msg)
{
    write(STDERR_FILENO, msg, strlen(msg));
}

static void on_stop_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void on_child_stop_signal(int sig)
{
    (void)sig;
    net_request_stop();
}

static void on_sigchld(int sig)
{
    (void)sig; /* only interrupts accept(); children are reaped in the loop */
}

static int add_child(pid_t pid)
{
    if (nchildren == children_cap) {
        size_t cap = children_cap ? children_cap * 2 : 64;
        pid_t *p = realloc(children, cap * sizeof(*p));
        if (!p)
            return -1;
        children = p;
        children_cap = cap;
    }
    children[nchildren++] = pid;
    return 0;
}

/* Reaps finished handlers; with block set, waits for at least one. */
static void reap_children(int block)
{
    pid_t pid;
    int status;

    while (nchildren > 0 && (pid = waitpid(-1, &status, block ? 0 : WNOHANG)) != 0) {
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            nchildren = 0; /* ECHILD: none left */
            return;
        }
        if (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0))
            child_crashed = 1;
        for (size_t i = 0; i < nchildren; i++) {
            if (children[i] == pid) {
                children[i] = children[--nchildren];
                break;
            }
        }
        block = 0;
    }
}

/* Completes operations a crashed handler committed to the journal but did
 * not finish. (Its accounts are also repaired lazily by money_lock; this
 * keeps balances shown by other menus current.) */
static void recover_journal(const char *when)
{
    char msg[128];
    int n = journal_recover_all();

    if (n < 0)
        snprintf(msg, sizeof(msg), "Journal recovery %s failed\n", when);
    else if (n > 0)
        snprintf(msg, sizeof(msg), "Journal recovery %s: completed %d operation(s)\n", when, n);
    else
        return;
    log_msg(msg);
}

static void stop_children(void)
{
    struct timespec tick = {0, 50 * 1000 * 1000};

    for (size_t i = 0; i < nchildren; i++)
        kill(children[i], SIGTERM);
    for (int waited = 0; nchildren > 0 && waited < SHUTDOWN_GRACE_MS; waited += 50) {
        nanosleep(&tick, NULL);
        reap_children(0);
    }
    for (size_t i = 0; i < nchildren; i++)
        kill(children[i], SIGKILL); /* stuck: still holds the pid, so safe */
    while (nchildren > 0)
        reap_children(1);
}

static void release_current_session(void)
{
    if (current_uid) {
        session_release(current_uid);
        current_uid = 0;
    }
}

void exit_session(void)
{
    net_puts("Goodbye.\n");
    release_current_session();
    _exit(0);
}

/* Checks the password and keeps the failed-login count, under the user's
 * record lock. Returns 1 if it matches, 0 if not, -1 if the account is
 * locked (message sent in every failing case). */
static int check_login(int uid, const char *password, User *u)
{
    char until[32];
    int64_t now = (int64_t)time(NULL);
    int ok;

    if (rec_lock(FILE_USERS, uid, F_WRLCK) < 0) {
        net_puts("Internal error: please try again.\n");
        return 0;
    }
    if (rec_read(FILE_USERS, uid, u) != 1) {
        rec_unlock(FILE_USERS, uid);
        net_puts("Invalid username or password.\n");
        return 0;
    }
    /* While locked the password is not even checked, so guessing on is
     * useless. */
    if (u->locked_until > now) {
        rec_unlock(FILE_USERS, uid);
        net_printf("Too many failed logins: this account is locked until %s. "
                   "An administrator can unlock it by resetting the password.\n",
                   fmt_time(u->locked_until, until, sizeof(until)));
        return -1;
    }
    ok = verify_password(password, u->pwhash);
    int failures = ok ? 0 : u->failed_logins + 1;
    int lock = failures >= LOGIN_MAX_FAILURES;
    if (failures != u->failed_logins || u->locked_until) {
        u->failed_logins = lock ? 0 : failures;
        u->locked_until = lock ? now + LOGIN_LOCK_SECONDS : 0;
        rec_write(FILE_USERS, uid, u);
    }
    rec_unlock(FILE_USERS, uid);
    if (lock)
        net_printf("Invalid username or password. Too many failed logins: this account is "
                   "locked for %d minutes.\n", LOGIN_LOCK_SECONDS / 60);
    else if (!ok)
        net_puts("Invalid username or password.\n");
    return ok;
}

static void login(void)
{
    static int failures; /* on this connection */
    char username[USERNAME_LEN + 8], password[PASSWORD_MAX + 8], when[32];
    User u;

    prompt_line("Username: ", username, sizeof(username));
    prompt_secret("Password: ", password, sizeof(password));
    int uid = find_user_by_username(username, &u);
    int ok = 0;
    if (uid <= 0)
        net_puts("Invalid username or password.\n");
    else
        ok = check_login(uid, password, &u);
    if (ok != 1) {
        if (++failures >= LOGIN_MAX_TRIES_PER_CONN) {
            net_puts("Too many failed logins on this connection. Disconnecting.\n");
            exit_session();
        }
        return;
    }
    failures = 0;
    if (u.closed_at) {
        net_printf("This account was closed on %s.\n", fmt_time(u.closed_at, when, sizeof(when)));
        return;
    }
    if (!u.active) {
        net_puts("This account is deactivated. Please contact the bank.\n");
        return;
    }
    int r = session_acquire(uid);
    if (r == 1) {
        net_puts("This user is already logged in from another session.\n");
        return;
    }
    if (r < 0) {
        net_puts("Server busy: could not create session. Try again later.\n");
        return;
    }
    current_uid = uid;
    if (u.role == ROLE_CUSTOMER)
        net_printf("Welcome, %s! Logged in as Customer, account number %d.\n", u.name, uid);
    else
        net_printf("Welcome, %s! Logged in as %s.\n", u.name, role_name(u.role));

    switch (u.role) {
    case ROLE_CUSTOMER: customer_menu(uid); break;
    case ROLE_EMPLOYEE: employee_menu(uid); break;
    case ROLE_MANAGER: manager_menu(uid); break;
    case ROLE_ADMIN: admin_menu(uid); break;
    }
    release_current_session();
    net_puts("Logged out.\n");
}

static void handle_client(int fd)
{
    long choice;

    net_init(fd, release_current_session);
    store_set_lock_hooks(net_hold, net_flush);
    for (;;) {
        net_puts("\n===== Banking Management System =====\n"
                 " 1. Login\n"
                 " 2. Exit\n");
        if (prompt_int("Choice: ", 1, 2, &choice) < 0)
            continue;
        if (choice == 1)
            login();
        else
            exit_session();
    }
}

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (fd < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void)
{
    const char *data_dir = getenv("BMS_DATA_DIR") ? getenv("BMS_DATA_DIR") : "data";
    const char *port_s = getenv("BMS_PORT") ? getenv("BMS_PORT") : "8080";
    char *end;
    long port = strtol(port_s, &end, 10);
    struct sigaction sa;

    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid BMS_PORT: %s\n", port_s);
        return 1;
    }
    int opened = store_open(data_dir);
    if (opened == STORE_OLD_FORMAT) {
        fprintf(stderr, "%s holds data in an older format; run bin/bms_migrate first.\n",
                data_dir);
        return 1;
    }
    if (opened < 0) {
        perror("Opening data files");
        return 1;
    }
    if (journal_recover_all() < 0) {
        fprintf(stderr, "Journal recovery failed; refusing to start on inconsistent data.\n");
        return 1;
    }
    if (rec_count(FILE_USERS) == 0)
        fprintf(stderr, "Warning: no users in %s. Run bms_init to create the administrator.\n",
                data_dir);
    if (session_init() < 0) {
        perror("Creating session table");
        session_destroy();
        return 1;
    }

    /* No SA_RESTART: these interrupt accept() so the loop reaps / stops. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sa.sa_handler = on_stop_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = listen_on((int)port);
    if (lfd < 0) {
        perror("Listening");
        session_destroy();
        return 1;
    }
    fprintf(stderr, "BMS server listening on port %ld, data in %s\n", port, data_dir);
    if (demo_delay())
        fprintf(stderr, "Demo mode: each balance change holds its account locks for %d s "
                        "(BMS_DEMO_DELAY)\n", demo_delay());

    while (!stop_requested) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        reap_children(0);
        if (child_crashed) {
            child_crashed = 0;
            recover_journal("after a handler crash");
        }
        if (cfd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            /* Finish the current operation, then leave at the next wait on
             * the client (see net_request_stop). */
            sa.sa_handler = on_child_stop_signal;
            sigaction(SIGINT, &sa, NULL);
            sigaction(SIGTERM, &sa, NULL);
            sa.sa_handler = SIG_DFL;
            sigaction(SIGCHLD, &sa, NULL);
            close(lfd);
            handle_client(cfd);
            _exit(0);
        }
        if (pid < 0)
            log_msg("fork failed; dropping connection\n");
        else if (add_child(pid) < 0)
            log_msg("out of memory tracking handler\n");
        close(cfd);
    }

    log_msg("Shutting down.\n");
    close(lfd);
    stop_children();
    recover_journal("at shutdown");
    session_destroy();
    return 0;
}
