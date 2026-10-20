#include "journal.h"
#include "store.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void fault_point(const char *name)
{
    const char *f = getenv("BMS_FAULT");

    if (f && strcmp(f, name) == 0)
        raise(SIGKILL);
}

static int set_has(const LockSet *s, int id)
{
    for (int i = 0; i < s->n; i++) {
        if (s->ids[i] == id)
            return 1;
    }
    return 0;
}

static int set_add(LockSet *s, int id)
{
    int i;

    if (id < 1 || set_has(s, id))
        return 0;
    if (s->n == MONEY_LOCK_MAX)
        return -1;
    for (i = s->n; i > 0 && s->ids[i - 1] > id; i--)
        s->ids[i] = s->ids[i - 1];
    s->ids[i] = id;
    s->n++;
    return 0;
}

static void unlock_all(LockSet *s)
{
    for (int i = s->n - 1; i >= 0; i--)
        rec_unlock(FILE_USERS, s->ids[i]);
}

static int lock_all(LockSet *s)
{
    for (int i = 0; i < s->n; i++) {
        if (rec_lock(FILE_USERS, s->ids[i], F_WRLCK) < 0) {
            while (--i >= 0)
                rec_unlock(FILE_USERS, s->ids[i]);
            return -1;
        }
    }
    return 0;
}

static int entry_valid(const JournalEntry *e)
{
    if (e->naccounts < 1 || e->naccounts > JOURNAL_MAX_ACCOUNTS || e->ntxns < 0 ||
        e->ntxns > JOURNAL_MAX_TXNS)
        return 0;
    for (int k = 0; k < e->naccounts; k++) {
        if (e->account_id[k] < 1)
            return 0;
    }
    return 1;
}

static int pending_entry(const JournalEntry *e)
{
    return e->state == JOURNAL_COMMITTED && entry_valid(e);
}

/* Writes everything e describes, then frees its slot. The caller holds all
 * of e's accounts. With inject set, the "applied-one" fault point fires
 * after the first account is written. */
static int finish(const JournalEntry *e, int inject)
{
    User u;
    JournalEntry freed;

    for (int k = 0; k < e->naccounts; k++) {
        if (rec_read(FILE_USERS, e->account_id[k], &u) != 1)
            return -1;
        u.balance = e->balance[k];
        if (rec_write(FILE_USERS, e->account_id[k], &u) < 0)
            return -1;
        if (inject && k == 0)
            fault_point("applied-one");
    }
    for (int i = 0; i < e->ntxns; i++) {
        const Transaction *t = &e->txn[i];
        if (rec_lock(FILE_TXNS, t->id, F_WRLCK) < 0)
            return -1;
        int r = rec_write(FILE_TXNS, t->id, t);
        rec_unlock(FILE_TXNS, t->id);
        if (r < 0)
            return -1;
    }

    memset(&freed, 0, sizeof(freed));
    freed.id = e->id;
    if (file_lock(FILE_JOURNAL, F_WRLCK) < 0)
        return -1;
    int r = rec_write(FILE_JOURNAL, e->id, &freed);
    file_unlock(FILE_JOURNAL);
    return r;
}

/* Copies every committed entry touching locked into *out, and adds their
 * other accounts to *want. Returns the number of entries, or -1. */
static int collect(const LockSet *locked, LockSet *want, JournalEntry **out)
{
    JournalEntry e, *list = NULL;
    int count = 0, cap = 0, err = 0;

    if (file_lock(FILE_JOURNAL, F_RDLCK) < 0)
        return -1;
    int n = rec_count(FILE_JOURNAL);
    for (int id = 1; id <= n && !err; id++) {
        if (rec_read(FILE_JOURNAL, id, &e) != 1 || !pending_entry(&e))
            continue;
        int touches = 0;
        for (int k = 0; k < e.naccounts; k++)
            touches |= set_has(locked, e.account_id[k]);
        if (!touches)
            continue;
        for (int k = 0; k < e.naccounts; k++)
            err |= set_add(want, e.account_id[k]) < 0;
        if (count == cap) {
            cap = cap ? cap * 2 : 4;
            JournalEntry *p = realloc(list, (size_t)cap * sizeof(*p));
            if (!p) {
                err = 1;
                break;
            }
            list = p;
        }
        list[count++] = e;
    }
    file_unlock(FILE_JOURNAL);
    if (err) {
        free(list);
        return -1;
    }
    *out = list;
    return count;
}

int money_lock(const int *ids, int n, LockSet *held)
{
    LockSet s = {0};
    JournalEntry *pending = NULL;
    int npending;

    for (int i = 0; i < n; i++) {
        if (set_add(&s, ids[i]) < 0)
            return -1;
    }
    /* A committed entry on one of our accounts belongs to a dead handler
     * (a live owner would still hold that account). Redoing it needs all of
     * its accounts, so grow the set and relock in order until it is closed. */
    for (;;) {
        LockSet want = s;
        if (lock_all(&s) < 0)
            return -1;
        npending = collect(&s, &want, &pending);
        if (npending < 0) {
            unlock_all(&s);
            return -1;
        }
        if (want.n == s.n)
            break;
        free(pending);
        unlock_all(&s);
        s = want;
    }
    for (int i = 0; i < npending; i++) {
        if (finish(&pending[i], 0) < 0) {
            free(pending);
            unlock_all(&s);
            return -1;
        }
    }
    free(pending);
    *held = s;
    return 0;
}

void money_unlock(LockSet *held)
{
    unlock_all(held);
    held->n = 0;
}

void journal_set_balance(JournalEntry *e, int account_id, int64_t balance)
{
    e->account_id[e->naccounts] = account_id;
    e->balance[e->naccounts] = balance;
    e->naccounts++;
}

void journal_add_txn(JournalEntry *e, int customer_id, int type, int64_t amount,
                     int64_t balance_after, int counterparty_id)
{
    e->txn[e->ntxns++] = (Transaction){
        .customer_id = customer_id,
        .type = type,
        .counterparty_id = counterparty_id,
        .amount = amount,
        .balance_after = balance_after,
        .timestamp = (int64_t)time(NULL),
    };
}

/* Writes e to a free journal slot (or a new one) and fsyncs it. */
static int commit(JournalEntry *e)
{
    JournalEntry slot;
    int r = -1;

    if (file_lock(FILE_JOURNAL, F_WRLCK) < 0)
        return JOURNAL_NOT_COMMITTED;
    int n = rec_count(FILE_JOURNAL);
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_JOURNAL, id, &slot) == 1 && slot.state == JOURNAL_FREE) {
            e->id = id;
            r = rec_write(FILE_JOURNAL, id, e);
            break;
        }
    }
    if (r < 0 && e->id == 0)
        r = rec_append_locked(FILE_JOURNAL, e) < 0 ? -1 : 0;
    if (r < 0 && e->id > 0 && e->id <= rec_count(FILE_JOURNAL)) {
        /* The write may or may not have reached the disk: try to free the
         * slot so the operation is cleanly not done. */
        memset(&slot, 0, sizeof(slot));
        slot.id = e->id;
        if (rec_write(FILE_JOURNAL, e->id, &slot) < 0) {
            file_unlock(FILE_JOURNAL);
            return JOURNAL_INCOMPLETE;
        }
    }
    file_unlock(FILE_JOURNAL);
    return r < 0 ? JOURNAL_NOT_COMMITTED : JOURNAL_OK;
}

int journal_apply(JournalEntry *e)
{
    Transaction placeholder;
    int delay = demo_delay();

    if (delay) {
        if (e->naccounts == 2)
            demo_log("holding accounts #%d and #%d for %d s before the change\n",
                     e->account_id[0], e->account_id[1], delay);
        else
            demo_log("holding account #%d for %d s before the change\n", e->account_id[0],
                     delay);
        sleep((unsigned)delay);
        demo_log("committing and releasing the lock\n");
    }

    /* Reserve the log rows' ids now, so a redo writes them in place instead
     * of appending duplicates. Until filled in, a reserved row has
     * customer_id 0 and belongs to no history. */
    if (file_lock(FILE_TXNS, F_WRLCK) < 0)
        return JOURNAL_NOT_COMMITTED;
    for (int i = 0; i < e->ntxns; i++) {
        memset(&placeholder, 0, sizeof(placeholder));
        int id = rec_append_locked(FILE_TXNS, &placeholder);
        if (id < 0) {
            file_unlock(FILE_TXNS);
            return JOURNAL_NOT_COMMITTED;
        }
        e->txn[i].id = id;
    }
    file_unlock(FILE_TXNS);
    fault_point("reserved");

    e->id = 0;
    e->state = JOURNAL_COMMITTED;
    int r = commit(e);
    if (r != JOURNAL_OK)
        return r;
    fault_point("committed");
    return finish(e, 1) < 0 ? JOURNAL_INCOMPLETE : JOURNAL_OK;
}

int journal_recover_all(void)
{
    JournalEntry e;
    LockSet held;
    int done = 0;

    for (int id = 1;; id++) {
        if (file_lock(FILE_JOURNAL, F_RDLCK) < 0)
            return -1;
        int n = rec_count(FILE_JOURNAL);
        int found = id <= n && rec_read(FILE_JOURNAL, id, &e) == 1 && pending_entry(&e);
        file_unlock(FILE_JOURNAL);
        if (id > n)
            return done;
        if (!found)
            continue;
        /* money_lock redoes every committed entry on these accounts. */
        if (money_lock(e.account_id, e.naccounts, &held) < 0)
            return -1;
        money_unlock(&held);
        done++;
    }
}
