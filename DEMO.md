# Live demonstration: locking, concurrency and ACID

This is a script for showing the evaluator, by hand, how the server handles race conditions and crashes. Each scenario takes a minute or two. The automated version of every scenario is in `tests/test_bms.py` (`make test`).

Normally every operation finishes in milliseconds, so two clients never visibly collide. **Demo mode** slows down balance changes so you can see one client wait for another:

| Variable | Effect |
|----------|--------|
| `BMS_DEMO_DELAY=N` (1–60) | Every deposit, withdrawal, transfer, loan payout, repayment and closure holds its account locks for *N* seconds before writing. Every lock that has to wait for another client is logged on the **server's terminal**. |
| `BMS_FAULT=<point>` | The client handler kills itself with `SIGKILL` at that point of a balance change, simulating a crash. Points: `reserved`, `committed`, `applied-one`. |

Both are off unless set. Normal runs are unaffected.

## Setup (once)

Use a separate data directory so your real `data/` is not touched:

```sh
make
export BMS_DATA_DIR=demo-data
BMS_ADMIN_USERNAME=admin BMS_ADMIN_PASSWORD=admin-pw bin/bms_init
BMS_DEMO_DELAY=8 bin/bms_server        # terminal 1: keep it visible
```

The server prints `Demo mode: each balance change holds its account locks for 8 s`.

In a second terminal, run `bin/bms_client`:

1. Log in as `admin` and choose **1. Add New Bank Employee** (e.g. `emp` / `emp-pw`). Exit.
2. Log in as `emp`, choose **1. Add New Customer** three times: `alice`, `bob`, `carol` (note their account numbers, e.g. #3, #4, #5). Exit.
3. Log in as each customer once and **2. Deposit Money** 1000. (Each deposit takes 8 s in demo mode.)

Then open three client terminals and log in as `alice`, `bob` and `carol`.

## Scenario 1: isolation — a reader waits for a writer

**Shows:** record locking, no dirty reads.

1. Alice: **4. Transfer Funds** → to Bob's account → `100` → `1` (confirm). Her client now waits 8 s.
2. Within those 8 s, Bob: **1. View Account Balance**. Bob's client also waits.

Server terminal:

```
[demo pid 2101] holding accounts #3 and #4 for 8 s before the change
[demo pid 2102] waiting for a read lock on users.dat record #4: another client holds it
[demo pid 2101] committing and releasing the lock
[demo pid 2102] got the read lock on users.dat record #4 after 6.1 s
```

Bob sees `1100.00`, the balance *after* the transfer, never a value from the middle of it. Alice sees `New balance: 900.00`.

**What to explain:** the transfer takes `fcntl()` write locks on records #3 and #4 of `users.dat` (`money_lock` in `src/journal.c`, which calls `rec_lock` in `src/store.c`). Bob's read lock on record #4 conflicts with that write lock, so the kernel blocks his process until the transfer has finished.

## Scenario 2: unrelated accounts don't block each other

**Shows:** the locks cover single records, not the whole file.

1. Alice: transfer `10` to Bob again (8 s wait).
2. During it, Carol: **1. View Account Balance** or **2. Deposit Money**.

Carol's balance appears at once, and the server logs no wait for her. Her record (#5) is a different byte range of `users.dat`: `fcntl` locks only `sizeof(User)` bytes at offset `(id − 1) × sizeof(User)`.

## Scenario 3: no deadlock with opposite transfers

**Shows:** lock ordering.

1. Alice: transfer `50` to Bob.
2. Within the 8 s, Bob: transfer `30` to Alice.

Both transfers complete, one after the other:

```
[demo pid 2101] holding accounts #3 and #4 for 8 s before the change
[demo pid 2102] waiting for a read lock on users.dat record #4: another client holds it
[demo pid 2101] committing and releasing the lock
[demo pid 2102] got the read lock on users.dat record #4 after 7.6 s
[demo pid 2102] holding accounts #4 and #3 for 8 s before the change
[demo pid 2102] committing and releasing the lock
```

**What to explain:** if Alice locked #3 then #4 while Bob locked #4 then #3, each could hold one lock and wait forever for the other. `money_lock` always locks accounts in ascending id order, whatever the direction of the transfer, so the second transfer just waits its turn. (The log lists accounts in transfer order; the locks are still taken in id order.)

## Scenario 4: atomicity — a crash mid-transfer

**Shows:** the write-ahead journal. A transfer is four writes: two balances and two history rows.

Stop the server (Ctrl+C) and restart it with a crash point and no delay:

```sh
BMS_FAULT=committed bin/bms_server
```

1. Alice: transfer `100` to Bob. Her client is disconnected: the handler process was killed with `SIGKILL` after the journal entry was saved but before any balance was written.
2. The server terminal shows: `Journal recovery after a handler crash: completed 1 operation(s)`.
3. Stop the server, start it normally (`bin/bms_server`), log in as `emp` and choose **6. View Customer Transactions (Passbook)** for Alice and for Bob. The transfer is there **exactly once** on both sides, and the balances match.

Repeat with `BMS_FAULT=reserved` (crash *before* the journal entry is saved): afterwards **nothing** has changed on either account. There is no state in between.

**What to explain:** the full result (both new balances and both history rows) is written to `journal.dat` and `fsync`ed before anything else is touched. That `fsync` is the commit point. After a crash, a committed entry is replayed by the server, by the next client that locks one of its accounts, or at the next startup. Replaying it twice is harmless, because it writes absolute balances into history rows whose ids were reserved in advance.

## Scenario 5: one session per user

Log in as `alice` from a second client while she is already logged in. The server answers `This user is already logged in from another session.` The session table is in System V shared memory, guarded by a semaphore (`src/session.c`), so two simultaneous logins can't both succeed.

## Clean up

```sh
rm -rf demo-data
unset BMS_DATA_DIR
```

## ACID summary for the viva

See the *ACID properties* section of `README.md` for the mapping from each property to the code.
