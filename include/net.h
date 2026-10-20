/* Line-oriented I/O with the connected client, plus input prompts.
 * Each forked handler serves exactly one connection. */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

/* on_disconnect runs (then the process exits) when the client goes away. */
void net_init(int fd, void (*on_disconnect)(void));

void net_send(const char *buf, size_t len);
void net_puts(const char *s);
void net_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Between net_hold() and net_flush() output is collected in memory instead of
 * sent. The server holds output while any record lock is held, so a client
 * that stops reading can never keep a lock held and stall other clients. */
void net_hold(void);
void net_flush(void);

/* Async-signal-safe shutdown request: ends the connection at once if the
 * handler is waiting on the client, otherwise the next time it would. */
void net_request_stop(void);

/* Reads one line (without newline) into buf. Never returns on disconnect. */
void net_readline(char *buf, size_t size);

/* Prompts; surrounding whitespace is trimmed. */
void prompt_line(const char *label, char *buf, size_t size);
void prompt_secret(const char *label, char *buf, size_t size);

/* Return 0 and set *out on valid input, -1 (after telling the user) if not. */
int prompt_int(const char *label, long min, long max, long *out);
int prompt_amount(const char *label, int64_t *paise);

/* Formats paise as rupees, e.g. 12345 -> "123.45". */
const char *fmt_amount(int64_t paise, char *buf, size_t size);
const char *fmt_time(int64_t t, char *buf, size_t size);

#endif
