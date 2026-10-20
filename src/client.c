/* Terminal client: relays the server's menus to stdout and the user's input
 * to the server. All banking logic lives on the server. */
#include "bms.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_tty, noecho_tty;
static volatile sig_atomic_t echo_off;

static void set_echo(int on)
{
    struct termios t;

    if (!isatty(STDIN_FILENO))
        return;
    if (!on && !echo_off) {
        tcgetattr(STDIN_FILENO, &saved_tty);
        t = saved_tty;
        t.c_lflag &= (tcflag_t)~ECHO;
        noecho_tty = t;
        echo_off = 1; /* before the change: a signal handler must undo it */
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    } else if (on && echo_off) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty);
        echo_off = 0;
        write(STDOUT_FILENO, "\n", 1);
    }
}

/* Ctrl+C, Ctrl+\\, hangup, kill: give the terminal its echo back, then die
 * from the same signal as usual (SA_RESETHAND restored the default). */
static void on_fatal_signal(int sig)
{
    if (echo_off)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty);
    raise(sig);
}

/* Ctrl+Z at a password prompt: echo on while suspended, off again on fg. */
static void on_stop_signal(int sig)
{
    if (echo_off)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty);
    signal(sig, SIG_DFL);
    raise(sig); /* blocked while in here; stops us on return */
}

static void on_continue(int sig)
{
    struct sigaction sa = {.sa_handler = on_stop_signal};

    (void)sig;
    sigaction(SIGTSTP, &sa, NULL);
    if (echo_off)
        tcsetattr(STDIN_FILENO, TCSANOW, &noecho_tty);
}

static void install_signal_handlers(void)
{
    struct sigaction sa = {.sa_handler = on_fatal_signal, .sa_flags = SA_RESETHAND};
    const int fatal[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP};

    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        sigaction(fatal[i], &sa, NULL);
    sa.sa_flags = 0;
    sa.sa_handler = on_stop_signal;
    sigaction(SIGTSTP, &sa, NULL);
    sa.sa_handler = on_continue;
    sigaction(SIGCONT, &sa, NULL);
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res, *ai;
    int fd = -1;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Cannot resolve %s: %s\n", host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : (getenv("BMS_PORT") ? getenv("BMS_PORT") : "8080");
    char buf[4096];

    int fd = connect_to(host, port);
    if (fd < 0) {
        fprintf(stderr, "Could not connect to %s:%s\n", host, port);
        return 1;
    }
    install_signal_handlers();

    struct pollfd pfd[2] = {
        {.fd = fd, .events = POLLIN},
        {.fd = STDIN_FILENO, .events = POLLIN},
    };
    int nfds = 2;

    for (;;) {
        if (poll(pfd, (nfds_t)nfds, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break; /* server closed the connection */
            size_t out = 0;
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == NOECHO_MARK)
                    set_echo(0);
                else
                    buf[out++] = buf[i];
            }
            write_all(STDOUT_FILENO, buf, out);
        }
        if (nfds == 2 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                shutdown(fd, SHUT_WR); /* no more input; drain server output */
                nfds = 1;
                continue;
            }
            if (write_all(fd, buf, (size_t)n) < 0)
                break;
            if (memchr(buf, '\n', (size_t)n))
                set_echo(1);
        }
    }
    set_echo(1);
    close(fd);
    return 0;
}
