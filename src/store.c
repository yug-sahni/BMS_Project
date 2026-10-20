#include "store.h"
#include "bms.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *const file_names[FILE_COUNT] = {
    "users.dat", "transactions.dat", "loans.dat", "feedback.dat", "journal.dat",
};

static const size_t rec_sizes[FILE_COUNT] = {
    sizeof(User), sizeof(Transaction), sizeof(Loan), sizeof(Feedback), sizeof(JournalEntry),
};

static int fds[FILE_COUNT] = {-1, -1, -1, -1, -1};

static int locks_held;
static void (*hook_first_locked)(void);
static void (*hook_all_unlocked)(void);

void store_set_lock_hooks(void (*first_locked)(void), void (*all_unlocked)(void))
{
    hook_first_locked = first_locked;
    hook_all_unlocked = all_unlocked;
}

/* Returns the format recorded in dir, writing the current one into a store
 * that has no data yet. 1 means data from before formats were recorded. */
static int check_format(const char *dir)
{
    char path[512], buf[16] = "";
    int fd;

    snprintf(path, sizeof(path), "%s/format", dir);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n < 0)
            return -1;
        buf[n] = '\0';
        return atoi(buf);
    }
    if (errno != ENOENT)
        return -1;
    for (int i = 0; i < FILE_COUNT; i++) {
        if (rec_count(i) != 0)
            return 1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return errno == EEXIST ? check_format(dir) : -1;
    int len = snprintf(buf, sizeof(buf), "%d\n", STORE_FORMAT);
    int r = write(fd, buf, (size_t)len) == len && fsync(fd) == 0 ? STORE_FORMAT : -1;
    close(fd);
    return r;
}

int store_open(const char *dir)
{
    char path[512];

    if (mkdir(dir, 0700) < 0 && errno != EEXIST)
        return -1;
    for (int i = 0; i < FILE_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, file_names[i]);
        fds[i] = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fds[i] < 0)
            return -1;
    }
    int format = check_format(dir);
    if (format < 0)
        return -1;
    return format == STORE_FORMAT ? 0 : STORE_OLD_FORMAT;
}

static off_t rec_offset(FileId f, int id)
{
    return (off_t)(id - 1) * (off_t)rec_sizes[f];
}

int demo_delay(void)
{
    static int delay = -1;

    if (delay < 0) {
        const char *s = getenv("BMS_DEMO_DELAY");
        int n = s ? atoi(s) : 0;
        delay = n > 0 && n <= 60 ? n : 0;
    }
    return delay;
}

void demo_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    int len = snprintf(buf, sizeof(buf), "[demo pid %d] ", (int)getpid());

    va_start(ap, fmt);
    len += vsnprintf(buf + len, sizeof(buf) - (size_t)len, fmt, ap);
    va_end(ap);
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    write(STDERR_FILENO, buf, (size_t)len);
}

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* In demo mode, tries the lock without waiting first, and logs when it has
 * to wait for another process and when it finally gets the lock. */
static int demo_wait(FileId f, struct flock *fl)
{
    const char *what = fl->l_type == F_RDLCK ? "read" : "write";
    char where[64];

    if (fcntl(fds[f], F_SETLK, fl) == 0)
        return 1;
    if (errno != EACCES && errno != EAGAIN)
        return 0; /* let the blocking path report it */
    if (fl->l_len == 0)
        snprintf(where, sizeof(where), "the whole of %s", file_names[f]);
    else
        snprintf(where, sizeof(where), "%s record #%lld", file_names[f],
                 (long long)(fl->l_start / fl->l_len + 1));
    demo_log("waiting for a %s lock on %s: another client holds it\n", what, where);
    double t = now_seconds();
    int r;
    while ((r = fcntl(fds[f], F_SETLKW, fl)) < 0 && errno == EINTR)
        ;
    if (r == 0)
        demo_log("got the %s lock on %s after %.1f s\n", what, where, now_seconds() - t);
    return r == 0 ? 1 : -1;
}

static int set_lock(FileId f, short type, off_t start, off_t len)
{
    struct flock fl = {
        .l_type = type,
        .l_whence = SEEK_SET,
        .l_start = start,
        .l_len = len,
    };
    if (type != F_UNLCK && demo_delay()) {
        int r = demo_wait(f, &fl);
        if (r != 0)
            return r > 0 ? 0 : -1;
    }
    while (fcntl(fds[f], F_SETLKW, &fl) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

static int acquire(FileId f, short type, off_t start, off_t len)
{
    if (set_lock(f, type, start, len) < 0)
        return -1;
    if (locks_held++ == 0 && hook_first_locked)
        hook_first_locked();
    return 0;
}

static int release(FileId f, off_t start, off_t len)
{
    int r = set_lock(f, F_UNLCK, start, len);
    if (locks_held > 0 && --locks_held == 0 && hook_all_unlocked)
        hook_all_unlocked();
    return r;
}

int rec_lock(FileId f, int id, short type)
{
    return acquire(f, type, rec_offset(f, id), (off_t)rec_sizes[f]);
}

int rec_unlock(FileId f, int id)
{
    return release(f, rec_offset(f, id), (off_t)rec_sizes[f]);
}

int file_lock(FileId f, short type)
{
    return acquire(f, type, 0, 0); /* len 0 = to end of file, incl. growth */
}

int file_unlock(FileId f)
{
    return release(f, 0, 0);
}

int rec_count(FileId f)
{
    struct stat st;

    if (fstat(fds[f], &st) < 0)
        return -1;
    return (int)(st.st_size / (off_t)rec_sizes[f]);
}

int rec_read(FileId f, int id, void *rec)
{
    if (id < 1)
        return 0;
    ssize_t n = pread(fds[f], rec, rec_sizes[f], rec_offset(f, id));
    if (n < 0)
        return -1;
    if ((size_t)n != rec_sizes[f])
        return 0;
    return 1;
}

static int write_all(FileId f, const void *rec, off_t off)
{
    const char *p = rec;
    size_t left = rec_sizes[f];

    while (left > 0) {
        ssize_t n = pwrite(fds[f], p, left, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        off += n;
        left -= (size_t)n;
    }
    return fsync(fds[f]);
}

int rec_write(FileId f, int id, const void *rec)
{
    if (id < 1 || id > rec_count(f))
        return -1;
    return write_all(f, rec, rec_offset(f, id));
}

int rec_append_locked(FileId f, void *rec)
{
    int count = rec_count(f);
    if (count < 0)
        return -1;
    int id = count + 1;
    memcpy(rec, &id, sizeof(int32_t));
    if (write_all(f, rec, rec_offset(f, id)) < 0)
        return -1;
    return id;
}

int rec_append(FileId f, void *rec)
{
    if (file_lock(f, F_WRLCK) < 0)
        return -1;
    int id = rec_append_locked(f, rec);
    file_unlock(f);
    return id;
}
