/* Fixed-size record files with fcntl() byte-range locking.
 *
 * Locks are POSIX record locks, which belong to a process: the server forks
 * one process per client, so these locks serialise clients against each
 * other. A process must not hold a whole-file lock and record locks on the
 * same file at once (unlocking one would drop the other). */
#ifndef STORE_H
#define STORE_H

#include <fcntl.h>

typedef enum {
    FILE_USERS,
    FILE_TXNS,
    FILE_LOANS,
    FILE_FEEDBACK,
    FILE_JOURNAL,
    FILE_COUNT
} FileId;

/* Version of the record layouts in bms.h, kept in <dir>/format. Data written
 * before the file existed is format 1; bms_migrate converts it. */
#define STORE_FORMAT 2
#define STORE_OLD_FORMAT (-2)

/* Opens (creating if needed) every data file under dir. 0 on success, -1 on
 * error (errno set), STORE_OLD_FORMAT if the data needs bms_migrate. */
int store_open(const char *dir);

/* Optional callbacks for when the process goes from holding no lock to
 * holding one, and back to none. The server uses them to defer output to
 * the client until every lock is released. */
void store_set_lock_hooks(void (*first_locked)(void), void (*all_unlocked)(void));

/* Blocking lock on one record; type is F_RDLCK or F_WRLCK. */
int rec_lock(FileId f, int id, short type);
int rec_unlock(FileId f, int id);

/* Blocking lock on the whole file (current and future records). */
int file_lock(FileId f, short type);
int file_unlock(FileId f);

/* Returns 1 if the record exists and was read, 0 if absent, -1 on error.
 * Caller is responsible for holding an appropriate lock. */
int rec_read(FileId f, int id, void *rec);

/* Overwrites an existing record and fsyncs. Caller holds a write lock. */
int rec_write(FileId f, int id, const void *rec);

/* Number of records. Caller holds a file lock if it needs a stable answer. */
int rec_count(FileId f);

/* Appends rec under a whole-file write lock, assigning and storing its id
 * (first field). Returns the new id, or -1. */
int rec_append(FileId f, void *rec);

/* Same as rec_append, for a caller that already holds the file write lock. */
int rec_append_locked(FileId f, void *rec);

/* Demo aid: BMS_DEMO_DELAY=N (1-60) makes every balance change hold its
 * account locks for N seconds (journal_apply), and every lock that has to
 * wait for another process is logged to stderr, the server's terminal, so a
 * concurrent operation can be seen blocking. Returns N, or 0 when unset. */
int demo_delay(void);
/* printf-style line to stderr, tagged with the pid. */
void demo_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
