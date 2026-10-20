/* Converts a data directory written before <dir>/format existed (format 1)
 * to the current record layouts. The original files are kept in
 * <dir>/backup-format1/. Run it with the server stopped. */
#include "bms.h"
#include "store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Format 1 layouts. */
typedef struct {
    int32_t id, role, active, reserved;
    int64_t balance;
    char username[USERNAME_LEN];
    char pwhash[PWHASH_LEN];
    char name[NAME_LEN];
    char phone[PHONE_LEN];
    char address[ADDRESS_LEN];
} UserV1;

typedef struct {
    int32_t id, customer_id, status, employee_id;
    int64_t amount;
    int32_t tenure_months, reserved;
    int64_t applied_at, decided_at;
    char purpose[PURPOSE_LEN];
} LoanV1;

typedef struct {
    int32_t id, customer_id, reviewed, reserved;
    int64_t timestamp;
    char text[FEEDBACK_LEN];
} FeedbackV1;

static void convert_user(const void *in, void *out)
{
    const UserV1 *o = in;
    User *u = out;

    u->id = o->id;
    u->role = o->role;
    u->active = o->active;
    u->balance = o->balance;
    memcpy(u->username, o->username, sizeof(u->username));
    memcpy(u->pwhash, o->pwhash, sizeof(u->pwhash));
    memcpy(u->name, o->name, sizeof(u->name));
    memcpy(u->phone, o->phone, sizeof(u->phone));
    memcpy(u->address, o->address, sizeof(u->address));
}

static void convert_loan(const void *in, void *out)
{
    const LoanV1 *o = in;
    Loan *l = out;

    l->id = o->id;
    l->customer_id = o->customer_id;
    l->status = o->status;
    l->employee_id = o->employee_id;
    l->amount = o->amount;
    l->tenure_months = o->tenure_months;
    l->applied_at = o->applied_at;
    l->decided_at = o->decided_at;
    memcpy(l->purpose, o->purpose, sizeof(l->purpose));
}

static void convert_feedback(const void *in, void *out)
{
    const FeedbackV1 *o = in;
    Feedback *f = out;

    f->id = o->id;
    f->customer_id = o->customer_id;
    f->reviewed = o->reviewed;
    f->timestamp = o->timestamp;
    memcpy(f->text, o->text, sizeof(f->text));
}

/* Writes the converted copy of dir/name as dir/name.new. */
static int convert(const char *dir, const char *name, size_t old_size, size_t new_size,
                   void (*fn)(const void *, void *))
{
    char path[512], tmp[520];
    char in[sizeof(UserV1) + sizeof(LoanV1) + sizeof(FeedbackV1)];
    char out[sizeof(User) + sizeof(Loan) + sizeof(Feedback)];
    struct stat st;
    long n = 0;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (stat(path, &st) < 0) {
        perror(path);
        return -1;
    }
    if (st.st_size % (off_t)old_size != 0) {
        fprintf(stderr, "%s: size %lld is not a whole number of format-1 records.\n", path,
                (long long)st.st_size);
        return -1;
    }
    FILE *src = fopen(path, "rb"), *dst = fopen(tmp, "wb");
    if (!src || !dst) {
        perror(src ? tmp : path);
        if (src)
            fclose(src);
        if (dst)
            fclose(dst);
        return -1;
    }
    while (fread(in, old_size, 1, src) == 1) {
        memset(out, 0, new_size);
        fn(in, out);
        if (fwrite(out, new_size, 1, dst) != 1)
            break;
        n++;
    }
    int ok = !ferror(src) && n * (off_t)old_size == st.st_size && fflush(dst) == 0 &&
             fsync(fileno(dst)) == 0;
    fclose(src);
    if (fclose(dst) != 0)
        ok = 0;
    if (!ok) {
        fprintf(stderr, "%s: conversion failed.\n", path);
        return -1;
    }
    printf("%s: %ld record(s) converted.\n", name, n);
    return 0;
}

/* Moves dir/name into the backup and dir/name.new into its place. */
static int swap_in(const char *dir, const char *backup, const char *name)
{
    char path[512], tmp[520], bak[600];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    snprintf(bak, sizeof(bak), "%s/%s", backup, name);
    if (rename(path, bak) < 0 || rename(tmp, path) < 0) {
        perror(name);
        return -1;
    }
    return 0;
}

int main(void)
{
    const char *dir = getenv("BMS_DATA_DIR") ? getenv("BMS_DATA_DIR") : "data";
    static const struct {
        const char *name;
        size_t old_size, new_size;
        void (*fn)(const void *, void *);
    } files[] = {
        {"users.dat", sizeof(UserV1), sizeof(User), convert_user},
        {"loans.dat", sizeof(LoanV1), sizeof(Loan), convert_loan},
        {"feedback.dat", sizeof(FeedbackV1), sizeof(Feedback), convert_feedback},
    };
    const int nfiles = (int)(sizeof(files) / sizeof(files[0]));
    char path[512], backup[512];

    umask(077); /* the data files are private to the server's user */
    snprintf(path, sizeof(path), "%s/format", dir);
    if (access(path, F_OK) == 0) {
        printf("%s already records its format; nothing to migrate.\n", dir);
        return 0;
    }
    snprintf(backup, sizeof(backup), "%s/backup-format1", dir);
    if (mkdir(backup, 0700) < 0) {
        fprintf(stderr, "Cannot create %s: %s\n", backup, strerror(errno));
        return 1;
    }
    /* Convert everything before replacing anything. Transactions and the
     * journal kept their layout and stay as they are. */
    for (int i = 0; i < nfiles; i++) {
        if (convert(dir, files[i].name, files[i].old_size, files[i].new_size, files[i].fn) < 0)
            return 1;
    }
    for (int i = 0; i < nfiles; i++) {
        if (swap_in(dir, backup, files[i].name) < 0)
            return 1;
    }
    FILE *f = fopen(path, "w");
    if (!f || fprintf(f, "%d\n", STORE_FORMAT) < 0 || fclose(f) != 0) {
        perror(path);
        return 1;
    }
    printf("%s migrated to format %d. Originals kept in %s.\n", dir, STORE_FORMAT, backup);
    return 0;
}
