#include "net.h"
#include "bms.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int conn_fd = -1;
static void (*disconnect_cb)(void);
static char rbuf[4096];
static size_t rlen, rpos;

static int holding, hold_overflow;
static char *hold_buf;
static size_t hold_len, hold_cap;

static volatile sig_atomic_t stop_requested, waiting_on_client;
static const char stop_msg[] = "\nServer is shutting down. Goodbye.\n";

void net_init(int fd, void (*on_disconnect)(void))
{
    conn_fd = fd;
    disconnect_cb = on_disconnect;
    rlen = rpos = 0;
}

static void disconnect(void) __attribute__((noreturn));
static void disconnect(void)
{
    if (disconnect_cb)
        disconnect_cb();
    close(conn_fd);
    _exit(0);
}

void net_request_stop(void)
{
    stop_requested = 1;
    if (waiting_on_client) {
        write(conn_fd, stop_msg, sizeof(stop_msg) - 1);
        _exit(0);
    }
}

/* Called before blocking on the client: from here until done_waiting() a
 * stop request ends the process directly from the signal handler. */
static void start_waiting(void)
{
    waiting_on_client = 1;
    if (stop_requested)
        net_request_stop();
}

static void done_waiting(void)
{
    waiting_on_client = 0;
}

static void hold_append(const char *buf, size_t len)
{
    if (hold_overflow)
        return;
    if (hold_len + len > hold_cap) {
        size_t cap = hold_cap ? hold_cap : 65536;
        while (cap < hold_len + len)
            cap *= 2;
        char *p = realloc(hold_buf, cap);
        if (!p) {
            hold_overflow = 1;
            return;
        }
        hold_buf = p;
        hold_cap = cap;
    }
    memcpy(hold_buf + hold_len, buf, len);
    hold_len += len;
}

void net_send(const char *buf, size_t len)
{
    if (holding) {
        hold_append(buf, len);
        return;
    }
    start_waiting();
    while (len > 0) {
        ssize_t n = write(conn_fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            disconnect();
        }
        buf += n;
        len -= (size_t)n;
    }
    done_waiting();
}

void net_hold(void)
{
    holding = 1;
    hold_len = 0;
    hold_overflow = 0;
}

void net_flush(void)
{
    holding = 0;
    net_send(hold_buf, hold_len);
    if (hold_overflow)
        net_puts("(output truncated: server out of memory)\n");
    hold_len = 0;
    if (hold_cap > (1u << 20)) { /* don't keep a huge listing's buffer around */
        free(hold_buf);
        hold_buf = NULL;
        hold_cap = 0;
    }
}

void net_puts(const char *s)
{
    net_send(s, strlen(s));
}

void net_printf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    net_send(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static int next_byte(void)
{
    if (rpos == rlen) {
        ssize_t n;
        start_waiting();
        do {
            n = read(conn_fd, rbuf, sizeof(rbuf));
        } while (n < 0 && errno == EINTR);
        done_waiting();
        if (n <= 0)
            return -1;
        rlen = (size_t)n;
        rpos = 0;
    }
    return (unsigned char)rbuf[rpos++];
}

void net_readline(char *buf, size_t size)
{
    size_t len = 0;
    int c;

    if (holding)
        net_flush();
    while ((c = next_byte()) != '\n') {
        if (c < 0)
            disconnect();
        if (c == '\r')
            continue;
        if (len + 1 < size)
            buf[len++] = (char)c; /* overlong input is truncated */
    }
    buf[len] = '\0';
}

static void trim(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;

    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    while (s[start] && isspace((unsigned char)s[start]))
        start++;
    memmove(s, s + start, len - start + 1);
    for (char *p = s; *p; p++) {
        if (iscntrl((unsigned char)*p))
            *p = ' ';
    }
}

void prompt_line(const char *label, char *buf, size_t size)
{
    net_puts(label);
    net_readline(buf, size);
    trim(buf);
}

void prompt_secret(const char *label, char *buf, size_t size)
{
    char mark = NOECHO_MARK;

    net_send(&mark, 1);
    net_puts(label);
    net_readline(buf, size);
}

int prompt_int(const char *label, long min, long max, long *out)
{
    char line[64];
    char *end;

    prompt_line(label, line, sizeof(line));
    errno = 0;
    long v = strtol(line, &end, 10);
    if (line[0] == '\0' || *end != '\0' || errno || v < min || v > max) {
        net_printf("Invalid input: enter a whole number between %ld and %ld.\n", min, max);
        return -1;
    }
    *out = v;
    return 0;
}

int prompt_amount(const char *label, int64_t *paise)
{
    char line[64];
    int64_t whole = 0, frac = 0;
    int frac_digits = 0, int_digits = 0;
    const char *p;

    prompt_line(label, line, sizeof(line));
    for (p = line; isdigit((unsigned char)*p); p++) {
        if (++int_digits > 12)
            goto bad;
        whole = whole * 10 + (*p - '0');
    }
    if (*p == '.') {
        for (p++; isdigit((unsigned char)*p); p++) {
            if (++frac_digits > 2)
                goto bad;
            frac = frac * 10 + (*p - '0');
        }
    }
    if (*p != '\0' || (int_digits == 0 && frac_digits == 0))
        goto bad;
    if (frac_digits == 1)
        frac *= 10;
    *paise = whole * 100 + frac;
    if (*paise <= 0 || *paise > MAX_AMOUNT)
        goto bad;
    return 0;
bad:
    net_puts("Invalid amount: enter a positive value with at most 2 decimals"
             " (max 1000000000.00).\n");
    return -1;
}

const char *fmt_amount(int64_t paise, char *buf, size_t size)
{
    const char *sign = paise < 0 ? "-" : "";
    int64_t a = paise < 0 ? -paise : paise;

    snprintf(buf, size, "%s%lld.%02lld", sign, (long long)(a / 100), (long long)(a % 100));
    return buf;
}

const char *fmt_time(int64_t t, char *buf, size_t size)
{
    time_t tt = (time_t)t;
    struct tm tm;

    if (t == 0 || !localtime_r(&tt, &tm))
        snprintf(buf, size, "-");
    else
        strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}
