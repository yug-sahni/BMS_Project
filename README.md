# Banking Management System 

A CS-513 System Software mini project. It is a multi-client TCP banking server written in C on Linux. Customers, employees, managers and administrators log in from a terminal client. The server keeps all data in binary record files. It uses `fcntl()` record locks, a System V semaphore, shared memory, and one `fork()`ed process per client to stay consistent under concurrent use.

The requirements come from `Mini Project - Banking Management System.docx`. Design and assumptions are in `V1_IMPLEMENTATION_PLAN.md`. Results are in `V1_IMPLEMENTATION_REPORT.md`.

## Requirements

- Linux, `gcc`, `make`
- `libcrypt` (part of glibc/libxcrypt; header `crypt.h`), used for SHA-512 password hashing
- `python3` (standard library only), needed only to run the tests

## Build

```sh
make            # builds bin/bms_server, bin/bms_client, bin/bms_init, bin/bms_migrate
```

## Configure

All settings are environment variables (see `.env.example`):

| Variable | Default | Used by |
|----------|---------|---------|
| `BMS_PORT` | `8080` | server, client |
| `BMS_DATA_DIR` | `data` | server, init |
| `BMS_ADMIN_USERNAME`, `BMS_ADMIN_PASSWORD` | prompt | init only |
| `BMS_FAULT` | unset | server; **tests and demos only**: simulates a crash at a journal point (see `include/journal.h`) |
| `BMS_DEMO_DELAY` | unset | server; **demos only**: every balance change holds its locks for this many seconds (1–60), and lock waits are logged (see `DEMO.md`) |

## First-time setup: create the administrator

```sh
bin/bms_init          # prompts for the administrator username and password
```

This creates `$BMS_DATA_DIR` and the first administrator. It refuses to run if the data directory already has users.

### Upgrading an existing data directory

The record layouts changed when the business rules below were added. `$BMS_DATA_DIR/format` records the layout version, and the server refuses to start on data from before that file existed. Convert such data once, with the server **stopped**:

```sh
bin/bms_migrate       # converts users.dat, loans.dat, feedback.dat in $BMS_DATA_DIR
```

The original files are kept in `$BMS_DATA_DIR/backup-format1/`. Customers opened before the migration have no recorded opener, so the separation-of-duties rule doesn't apply to them.

## Run

```sh
bin/bms_server                     # terminal 1 (Ctrl+C to stop)
bin/bms_client [host] [port]       # terminal 2, 3, … (defaults: 127.0.0.1, $BMS_PORT or 8080)
```

Typical first session:

1. Log in as the administrator. Choose **Add New Bank Employee** to create staff. Choose **Manage User Roles** to promote one of them to Manager.
2. Log in as an employee and choose **Add New Customer**. The server prints the customer's account number. Customers also see it when they log in and in their menu header.
3. Customers log in to deposit, withdraw, transfer (to another customer's account number, after confirming the recipient's name), apply for loans, give feedback, and view their history.
4. The manager assigns loan applications to employees. The employee chooses **Process Loan Applications**, then **Approve/Reject Loans**. Approving credits the loan amount to the customer's account; rejecting needs a reason, which the customer sees. Customers follow their loans with **View My Loans** and pay them back with **Repay Loan**.

If a customer or staff member forgets their password, the administrator can set a new one with **Reset User Password**. Passwords are stored only as hashes and can't be recovered.

### Business rules

- **Loans** carry no interest. The customer repays the principal in monthly installments of amount ÷ tenure, rounded up. **View My Loans** shows what is outstanding, the installment, the next due date and any overdue amount. A loan becomes REPAID when the last rupee is paid. Loan balances are worked out from the `LOAN CREDIT` / `LOAN REPAY` history rows, which the journal writes together with the balance, so they can't disagree after a crash.
- **Applications:** at most 1,00,00,000.00 per application, one application under review at a time, and no new application while an earlier loan is unpaid. Approval is refused if the customer still owes on another loan.
- **Separation of duties:** the employee who opened a customer's account can't be assigned, process or decide that customer's loans.
- **Reassignment:** moving a loan that another employee already holds needs confirmation. That employee then sees a notice in **View Assigned Loan Applications**, and the same notice if they try to work on it.
- **Deactivating a customer** (manager) shows the frozen balance, unpaid loans and open applications, and needs a reason and confirmation. Open applications are rejected. The reason is shown to staff (manager, passbook), not to the customer.
- **Closing an account** (employee, **Close Customer Account**) pays out the balance as a `CLOSURE PAYOUT` row and closes the account for good. It's refused while the customer owes on a loan, and for a deactivated (frozen) account. A closed account can't log in, receive transfers or be reactivated.
- **Staff accounts:** the administrator can deactivate employees and managers (**Activate/Deactivate Bank Staff**), with a reason. They're logged out at their next action, and their open loan applications go back to the pending queue. The administrator can also edit their own details.
- **Failed logins:** after 5 consecutive wrong passwords, the account can't log in for 15 minutes; while locked, the password isn't even checked. **Reset User Password** lifts the lock. A connection is dropped after 3 failed logins.
- **Feedback:** the manager can mark feedback as reviewed or reply to it. Customers see their feedback, whether it has been read, and any reply under **View My Feedback**.

Amounts are entered in rupees with up to 2 decimals, for example `250.75`. Each user can have only one active session.

## Concurrency and ACID properties

The server `fork()`s one process per client, and all of them work on the same data files. `DEMO.md` shows each of the points below live, with two or more clients.

**Synchronization mechanisms**

| Mechanism | Where | Protects |
|-----------|-------|----------|
| `fcntl()` record locks (`F_RDLCK` / `F_WRLCK` on one record's byte range) | `src/store.c`: `rec_lock` | One user, loan or feedback record. Readers share; a writer excludes everyone. |
| `fcntl()` whole-file locks | `src/store.c`: `file_lock` | Appending a record (new id), scans that must see a stable file, the journal's slot table |
| Ordered locking of accounts | `src/journal.c`: `money_lock` | Transfers in opposite directions can't deadlock: accounts are always locked in ascending id order, and a loan before its account |
| System V semaphore + shared memory | `src/session.c` | The one-session-per-user table shared by every handler process |
| Deferred output while locked | `src/store.c` hooks, `net_hold` / `net_flush` | A client that stops reading can't keep a lock held and stall others |

**ACID**

| Property | How it is met | Code | Proven by |
|----------|---------------|------|-----------|
| **Atomicity** | A balance change (deposit, withdrawal, transfer, loan payout, repayment, closure) is first written in full to `journal.dat`, both new balances and both history rows, and `fsync`ed. Only then are the records updated. A crash before that commit point changes nothing; after it, the entry is redone by the server when it reaps the crashed handler, by the next client that locks one of its accounts, or at startup. Every other operation writes a single record. | `src/journal.c`: `journal_apply`, `money_lock`, `journal_recover_all` | `CrashRecoveryTests` (crash at each stage of a transfer, a loan payout and a repayment) |
| **Consistency** | Every rule is checked while the records are locked: no overdraft, amount and balance limits, active accounts only, no transfer to yourself, loan limits. Loan balances are derived from the history rows, so they can't disagree with account balances. Each account's last history row always equals its balance. | `src/customer.c`, `src/employee.c`, `src/common_ops.c`: `loan_outstanding` | `test_concurrent_transfers_conserve_money` (total money conserved under 400+ concurrent transfers) |
| **Isolation** | An operation write-locks every account it changes for its whole read-check-write sequence, and reads a balance under a lock too, so no one sees or overwrites a half-done change (no lost updates, no dirty reads). Different accounts are different byte ranges and don't block each other. | `src/store.c`: `rec_lock`; `src/journal.c`: `money_lock` | `test_many_concurrent_clients`, `test_concurrent_duplicate_username`, `test_concurrent_login_race`, `DemoModeTests` |
| **Durability** | Every record write is followed by `fsync()`, and the journal entry is `fsync`ed before it counts as committed. A reported success has reached the disk. | `src/store.c`: `rec_write`; `src/journal.c`: `commit` | `test_data_persists_across_restart`, `test_startup_recovery` |

## Test

```sh
make test       # or: python3 tests/test_bms.py
```

The suite starts a real server with a temporary data directory and a free port for each test. It drives the server over TCP and covers every role's operations, the validation rules, one-session-per-user, and concurrency: simultaneous transfers conserve money, opposite transfers don't deadlock, simultaneous logins and duplicate usernames are handled. Robustness tests check that a client that stops reading cannot stall other clients, that shutting down the server disconnects every client cleanly, that promoting or deactivating an employee returns their open loans to the queue, every business rule above (loan limits, repayment, separation of duties, reassignment notices, deactivation, account closure, login lockout, feedback replies), migration of old-format data, and that the client restores terminal echo when interrupted at a password prompt. Crash-recovery tests kill a handler at each stage of a transfer, and during a loan payout and a repayment, and check that each is either fully done or not done at all. A demo-mode test checks that a balance check made during a slowed-down transfer waits for it and then sees the finished result.

## Layout

```
include/   bms.h (record layouts), store.h, session.h, net.h, ops.h, journal.h
src/       server.c (accept + fork), session.c (shm + semaphore), store.c (record I/O + fcntl locks),
           journal.c (write-ahead journal for balance changes),
           net.c (socket line I/O, prompts), auth.c (crypt), common_ops.c,
           customer.c, employee.c, manager.c, admin.c, client.c, bms_init.c, bms_migrate.c
tests/     test_bms.py
class_diagram.puml   class diagram (PlantUML source; rendered as class_diagram.png and class_diagram.svg)
DEMO.md    step-by-step live demonstration of locking, concurrency and crash recovery
```

Data files are `users.dat`, `transactions.dat`, `loans.dat`, `feedback.dat` and `journal.dat` in `$BMS_DATA_DIR`. Each is an array of fixed-size records, and record *N* sits at offset `(N-1) * sizeof(record)`.

Every deposit, withdrawal and transfer is first written to `journal.dat` and fsynced. Only then are balances and history rows updated. If the server or a handler dies part-way, the operation is completed from the journal: by the next client to touch one of its accounts, by the server as soon as it notices the crash, or at the next startup. `journal.dat` is normally all free slots; don't delete it while the server is stopped after a crash.
