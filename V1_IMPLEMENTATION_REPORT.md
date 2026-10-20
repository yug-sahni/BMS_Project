# V1 Implementation Report — Banking Management System

Specification: `Mini Project - Banking Management System.docx`. Requirement IDs refer to `V1_IMPLEMENTATION_PLAN.md` §2.

## 1. Features implemented

| Req | Feature | Where |
|-----|---------|-------|
| C1, E1, M1, A1 | Single login screen; the role comes from the account. SHA-512 hashed passwords. Deactivated accounts are refused. **One session per user**, enforced through a shared-memory table guarded by a semaphore. | `server.c` `login()`, `session.c`, `auth.c` |
| C2 | View account balance (also shows the account number). The account number also appears in the login message, the customer menu header, and when an employee creates the customer. | `customer.c` `view_balance`, `server.c` `login()` |
| C3 / C4 | Deposit / withdraw, with amount validation, no overdraft, balance limit. Crash-atomic through the journal (§7). | `customer.c` |
| C5 | Transfer funds. Both records are locked in id order. The destination must be an active customer and can't be your own account. It is checked as soon as the account number is entered, and the customer confirms the recipient's name and the amount before any money moves (added after V1). Crash-atomic through the journal (§7). | `customer.c` `transfer`, `journal.c` |
| C6 | Apply for a loan (amount, tenure, purpose) → PENDING | `customer.c` `apply_loan` |
| C7, E8, M5, A5 | Change password: verifies the current one, asks for confirmation | `common_ops.c` `change_password` |
| — | View my loan applications: amount, tenure, status, applied and decided dates (added after V1, see §6) | `customer.c` `view_loans` |
| C8 | Add feedback | `customer.c` `add_feedback` |
| C9 | View transaction history | `common_ops.c` `print_transactions` |
| C10/11, E9/10, M6/7, A6/7 | Logout returns to the login screen. Exit closes the connection. | all menus, `server.c` `exit_session` |
| E2 | Add new customer, with a unique username (checked under an exclusive file lock) | `common_ops.c` `create_user_interactive` |
| E3 | Modify customer details (name, phone, address); customer list with IDs shown first | `employee.c`, `common_ops.c` `modify_user_interactive` |
| E4 | Process loan applications: an assigned application moves to PROCESSING, with applicant details shown | `employee.c` `process_loan` |
| E5 | Approve/reject loans that are being processed. The application is checked and shown before the decision is asked. Approval credits the loan amount to the customer's account (added after V1, see §6). | `employee.c` `decide_loan` |
| E6 | View assigned loan applications | `employee.c` `view_assigned` |
| E7 | View customer transactions (passbook); customer list with IDs shown first | `employee.c` `passbook` |
| M2 | Activate/deactivate customer accounts, with a customer list showing each status. A deactivated customer is logged out at their next action. | `manager.c` `set_customer_active` |
| M3 | Assign loan applications to employees, listing open applications and active employees. The employee is checked under the loan's lock (§7). | `manager.c` `assign_loan` |
| M4 | Review customer feedback and mark it reviewed | `manager.c` `review_feedback` |
| A2 | Add new bank employee | `admin.c` |
| A3 | Modify customer/employee details; lists customers and staff with IDs first | `admin.c` `modify_user` |
| A4 | Manage user roles (Employee ↔ Manager); lists all staff with IDs first. Promoting an employee returns their open loans to the pending queue (§7). | `admin.c` `manage_roles` |
| — | Reset user password (added after V1; not in the spec). The administrator sets a new password for a customer, employee or manager who forgot theirs. Passwords are stored only as hashes, so they can't be recovered. The admin's own password can't be reset this way. | `admin.c` `reset_password` |
| T1 | TCP socket server with many concurrent clients, plus a terminal client | `server.c`, `client.c` |
| T2 | System calls: `socket/bind/listen/accept/connect`, `fork`, `waitpid`, `sigaction`, `kill`, `raise`, `open/pread/pwrite/fsync/fstat/mkdir`, `fcntl(F_SETLKW)` record locks, `shmget/shmat/shmctl`, `semget/semop/semctl` (with `SEM_UNDO`), `poll`, `read/write`, `tcsetattr` | throughout |
| T3, T4 | Per-record read/write locks, ordered locking for transfers, whole-file locks for appends and scans, `fsync` after every write, write-ahead journal for every balance change (§7). Property-by-property ACID mapping: README, *Concurrency and ACID properties*; live demo: `DEMO.md` (§9). | `store.c`, `journal.c` and callers |
| T5 | Class diagram | `class_diagram.puml` / `class_diagram.png` (§9); module overview in `V1_IMPLEMENTATION_PLAN.md` §5.3 |

Not in the spec, but needed to run at all: `bms_init` creates the first administrator from operator input, because the document gives the admin no bootstrap path.

## 2. Files created

In V1, no existing project files were modified. The Word document and `readme.txt` are untouched.

- `Makefile`, `.env.example`, `README.md`, `V1_IMPLEMENTATION_PLAN.md`, `V1_IMPLEMENTATION_REPORT.md`
- `include/bms.h`, `include/store.h`, `include/session.h`, `include/net.h`, `include/ops.h`
- `src/server.c`, `src/client.c`, `src/bms_init.c`, `src/store.c`, `src/session.c`, `src/net.c`, `src/auth.c`, `src/common_ops.c`, `src/customer.c`, `src/employee.c`, `src/manager.c`, `src/admin.c`
- `tests/test_bms.py`
- Build output: `bin/` (from `make`)

## 3. Commands

```sh
make                       # build (gcc -Wall -Wextra -Werror)
bin/bms_init               # create data dir + first administrator
bin/bms_server             # start server (BMS_PORT, BMS_DATA_DIR)
bin/bms_client [host] [port]
make test                  # integration tests
```

## 4. Tests executed and actual results

- `make clean && make`: builds all four binaries (`bms_server`, `bms_client`, `bms_init`, `bms_migrate`) with `-Wall -Wextra -Werror` and no warnings.
- `python3 tests/test_bms.py`: **42 tests, all OK** (about 40 s). These are the 19 V1 tests, the 9 tests added in §7, `test_admin_reset_password`, `test_account_number_shown`, `test_loan_approval_credits_customer`, the 10 tests added in §8 and the demo-mode test of §9. The list below describes the tests up to §7; §8 and §9 list the rest. `test_transfer` now also covers cancelling a transfer, and `test_loan_workflow` covers a decision refused before it is asked. I ran it 3 times in a row with identical results. Afterwards no `bms_server` processes from the tests were left and `ipcs` showed no leftover shared memory or semaphores.
  - Login: wrong password or unknown user rejected, success, logout, exit, invalid menu input
  - One session per user: a second login is refused; it is allowed again after logout and after an abrupt disconnect
  - 12 simultaneous logins of the same user: exactly 1 succeeds
  - Data persists across a server restart
  - Customer: deposit, withdraw, overdraft, invalid amounts, transfer (to self, to a missing account, to a non-customer, insufficient funds), history, change password, loan application, feedback
  - Employee: add customer (duplicate or invalid input), modify customer, staff not modifiable by employees, passbook
  - Loan workflow: assign → view assigned → decide-before-process refused → process → approve/reject. Another employee's loan is refused, and a closed loan can't be reassigned.
  - Manager: deactivation blocks login and incoming transfers and logs out a live session; reactivation; feedback review
  - Admin: modify customer and employee, role promotion and demotion take effect at the next login
  - **Concurrency:** 8 customers each make 25 transfers to a hub plus 25 opposite-direction transfers to neighbours, while the hub withdraws 25 times. Money is conserved exactly, there is no deadlock, and every account's last history row equals its balance.
  - **Concurrency:** two employees create the same username at once; exactly one succeeds.
  - **Concurrency:** 20 clients each make 10 deposits at once; every balance is correct.
  - **Robustness (§7):** a client stops reading a 300,000-row history, and another customer can still deposit and withdraw. `SIGTERM` to the server disconnects a logged-in client with a shutdown message, and the server restarts cleanly. Promoting an employee returns their open loan to the queue, and it can be reassigned and approved. The client restores terminal echo after Ctrl+Z, turns it off again after `fg`, and restores it after Ctrl+C at a password prompt (tested under a pseudo-terminal).
  - **Crash recovery (§7):** a handler is killed (`BMS_FAULT`, `SIGKILL`) during a transfer of 100.00 from a 500.00 account, at each stage. Before the commit: both balances and histories are unchanged. After the commit, before any write: the server completes the transfer when it reaps the handler. After debiting the source only, with the server parent frozen: the payee's next balance check completes it, and replaying the journal again later changes nothing. Whole server down with a committed entry on disk: it is completed at startup. In every completed case the balances are 400.00 / 100.00, each history has exactly one matching row, and the journal slot is freed.
- Manual check: ran the real `bms_client` under a pseudo-terminal (`script`). Passwords were not echoed and login and exit worked.
- Manual check: `bms_init` runs interactively and refuses to re-initialise a populated data directory.

## 5. Known limitations

1. **Crash atomicity:** fixed after V1 by the write-ahead journal (§7). Every balance change goes through it: deposits, withdrawals and transfers, and since §8 loan payouts, loan repayments and account-closure payouts. Staff operations (detail edits, activation, roles) each write a single record, so they need no journal. A loan's status is a separate write from its payout or repayment; the code detects a crash between the two from the history rows, so the money is never moved twice (§6, and `test_crash_during_loan_payout_pays_once` / `test_crash_during_repayment_completes_once`). One side effect: if a handler dies after reserving history row ids but before committing, those rows stay as unused placeholders (`customer_id` 0) that no history shows. The journal guards against process crashes and power loss, provided `fsync` really reaches stable storage. It does not guard against a corrupted or lost disk.
2. **Multithreading:** the document lists multithreading among the allowed mechanisms. V1 uses one process per client instead, because POSIX `fcntl` record locks are per process and would not isolate threads from each other. Processes, file locks, semaphores and shared-memory IPC are used.
3. Stopping the server (`SIGINT`/`SIGTERM`) stops new connections and asks every handler to finish its current operation. Each handler then disconnects its client the next time it waits on it. A handler that hasn't exited after 3 seconds is killed with `SIGKILL`. A kill could land mid-operation only if a handler was stuck waiting for a lock, which limitation 1 covers.
4. Transport is plain TCP with no encryption. Passwords are hashed at rest but sent in clear over the socket. The document doesn't ask for TLS.
5. ~~No limit on login attempts.~~ Fixed in §8 (item 6): an account is locked for 15 minutes after 5 consecutive wrong passwords.
6. Only IPv4 for the server listener.
7. The server's terminal menus are the only interface (no GUI), which matches the socket client/server requirement.

## 6. Requirements not implemented, and why

| Item | Reason |
|------|--------|
| ~~"Closing" accounts (in the employee description, not in the employee feature list)~~ | **Added in §8 (item 8):** employee **Close Customer Account** pays out the balance through the journal and closes the account for good. |
| ~~Loan disbursement on approval~~ | **Added after V1 at the user's request** (this reverses plan assumption 5). Approving pays the amount into the customer's account through the journal, as a `LOAN CREDIT` history row that names the loan. Approval is refused, and the application stays PROCESSING, if the account is deactivated or would exceed the balance limit. The payment and the status change are two writes. Before paying, `decide_loan` looks for an existing `LOAN CREDIT` row for the loan, so a loan paid just before a crash is recorded as APPROVED and never paid twice, whatever the employee chooses next (`CrashRecoveryTests.test_crash_during_loan_payout_pays_once`). |
| ~~Customer viewing their loan status~~ | **Added after V1 at the user's request:** customer menu option 6, *View My Loan Applications*. |

Every requirement in the document is now implemented and tested.

## 7. Post-V1 fixes

A code review after V1 found the following problems. I wrote each regression test first and confirmed it failed on the V1 code. All four now pass along with the original 19 tests.

| # | Problem in V1 | Fix | Regression test |
|---|---------------|-----|-----------------|
| 1 | **A client that stopped reading could stall every other client.** Listings and confirmation messages were written to the socket while file or record locks were held. When the client's socket buffer filled, `write()` blocked with the lock still held. A held read lock on `users.dat` or `transactions.dat` then blocked every deposit, withdrawal, transfer and new user. | `store.c` counts the locks a handler holds and calls hooks when it takes its first lock and releases its last one. The server installs `net_hold`/`net_flush` as those hooks, so output is buffered in memory while any lock is held and sent only after the last unlock. This covers every current and future lock site. `net_readline` flushes first as a safeguard. | `RobustnessTests.test_stalled_reader_does_not_block_others` |
| 2 | **`SIGTERM` to the server left handlers running without IPC.** The parent removed the semaphore and shared memory while connected handlers kept running. Their later logins failed with "Server busy" and logouts could not free their session slot. `SA_NOCLDWAIT` also meant a dead handler's PID could be reused while still listed in the session table. | The parent now tracks its children and reaps them itself (`SIGCHLD` handler plus `waitpid`), so a PID cannot be reused before it is reaped. On shutdown it sends `SIGTERM` to each handler, waits up to 3 s, then sends `SIGKILL` to any left, and only then removes the IPC objects. A handler leaves only while it waits on its client (`net_request_stop`), so it never stops mid-operation. | `RobustnessTests.test_shutdown_disconnects_clients` |
| 3 | **Promoting an employee stranded their loans, and a manager could be assigned one.** Applications that were ASSIGNED or PROCESSING stayed with a user who could no longer process them. `assign_loan` also checked the employee's role before locking the loan, so a role change in between could assign a loan to a manager. | `manage_roles` saves the new role first, then returns the employee's open applications to PENDING (unassigned) under the loans file lock, and reports how many it returned. `assign_loan` checks the employee while holding the loan's lock, so a promotion that comes after the check waits for that lock and then releases the loan. | `StaffTests.test_promoting_employee_returns_open_loans` (the role-change race is timing-dependent and is not covered by a test) |
| 4 | **The client could leave the terminal without echo.** Ctrl+C, Ctrl+Z or a hangup at a password prompt left echo off. | `client.c` installs handlers for `SIGINT`, `SIGQUIT`, `SIGTERM` and `SIGHUP` that restore the terminal and then re-raise the signal. On `SIGTSTP` the handler restores echo before stopping, and on `SIGCONT` it turns echo off again. | `ClientTests.test_client_restores_echo_on_signals` |

Files changed: `include/net.h`, `include/store.h`, `src/net.c`, `src/store.c`, `src/server.c`, `src/manager.c`, `src/admin.c`, `src/client.c`, `tests/test_bms.py`, `README.md`.

### 7.1 Write-ahead journal

The review's last finding (§5 limitation 1) was that a transfer's four writes (two balances, two history rows) were not atomic against a crash. A handler killed between them could make money vanish or leave a balance change with no history row. Deposits and withdrawals had the same flaw on a smaller scale (balance written, row not). All three now go through a redo journal (`include/journal.h`, `src/journal.c`, new data file `journal.dat`):

1. `money_lock()` write-locks the operation's accounts in ascending id order. This takes over from the old ordered locking in `transfer`.
2. The operation computes its result as a `JournalEntry`: the new absolute balances and the full history rows.
3. `journal_apply()` reserves the history row ids under the `transactions.dat` file lock (placeholder rows, `customer_id` 0). It then writes the entry to a free journal slot and fsyncs it. **This is the commit point.**
4. It writes the balances and the history rows at their reserved ids, then frees the slot.

Recovery redoes a committed entry, and redo is **idempotent**: it writes absolute balances (only the `balance` field, so a concurrent name or password edit survives) and rows at fixed ids, so running it twice changes nothing. There are three triggers:
- **Next access.** A committed entry's accounts stay locked by its owner until the entry is freed. So if `money_lock()` finds a committed entry on an account it has just locked, the owner must be dead. It redoes the entry before the caller reads the balance. If the entry also involves accounts it hasn't locked, it releases everything and relocks the larger set in ascending order, so the deadlock-free ordering still holds. Every balance change goes through `money_lock()`, so no update can be applied on top of a half-finished one and later overwritten by the redo.
- **Handler crash.** When the parent reaps a handler that died from a signal or exited non-zero, it runs `journal_recover_all()`. Balances that other menus read without `money_lock` (passbook, applicant details) are then current again straight away.
- **Startup.** `bms_server` replays the journal before it accepts connections, and refuses to start if replay fails.

The lock order is user records → `transactions.dat` → `journal.dat`, with at most one of the last two held at a time. That matches every other code path. Output is still deferred while any lock is held (fix 1).

| Regression test | Crash point | Expected |
|---|---|---|
| `CrashRecoveryTests.test_crash_before_commit_changes_nothing` | `reserved` | Nothing changed |
| `CrashRecoveryTests.test_crash_after_commit_completed_by_server` | `committed` | Completed by the parent after reaping |
| `CrashRecoveryTests.test_crash_mid_apply_completed_by_next_access` | `applied-one` (source debited only), parent frozen | Completed by the payee's next balance check; a later replay is a no-op |
| `CrashRecoveryTests.test_startup_recovery` | Committed entry left on disk while the server is down | Completed at startup |

Crash points are injected with the test-only `BMS_FAULT` environment variable. It makes the handler `SIGKILL` itself at the named point, and recovery never triggers it.

Files changed for the journal: new `include/journal.h` and `src/journal.c`. Changed `include/bms.h` (`JournalEntry`), `include/store.h` and `src/store.c` (`journal.dat`), `src/customer.c` (deposit, withdraw, transfer, view balance), `src/server.c` (recovery), `include/ops.h` and `src/common_ops.c` (`log_txn` removed), `Makefile`, `tests/test_bms.py` and `README.md`.

## 8. Business-rule fixes

A review after V1 found menu items that were implemented literally but left out what a bank would obviously need next. All were fixed at the user's request. The added rules are listed in the README under *Business rules*.

| # | Gap | Fix | Tests |
|---|-----|-----|-------|
| 1 | Staff could never lose access | Admin **Activate/Deactivate Bank Staff**, with a reason and confirmation. Deactivated staff are logged out and their open loans return to the pending queue. | `StaffTests.test_staff_deactivation` |
| 2 | Loans were paid out but never owed | Zero-interest repayment: customer **Repay Loan**; outstanding, installment, next due date and overdue amount shown in **View My Loans**; new status REPAID. Amounts come from `LOAN REPAY` history rows written by the journal. A crash after the payment is recorded as REPAID on the next attempt, without paying twice. | `test_loan_repayment`, `CrashRecoveryTests.test_crash_during_repayment_completes_once` |
| 3 | No limits on applications | At most 1,00,00,000.00 per application; one application under review; none while a loan is unpaid (checked under the loans file lock). Approval is also refused while the customer owes on another loan (checked under the account lock). | `test_apply_loan_and_feedback`, `test_loan_approval_credits_customer` |
| 4 | Rejections had no reason | A reason is required and shown to the customer. | `test_loan_workflow` |
| 5 | Deactivation ignored money and loans | Shows the frozen balance, unpaid loans and open applications. Needs a reason and confirmation. Open applications are rejected. | `test_manager_activate_deactivate` |
| 6 | Unlimited password guessing | 5 failures lock the account for 15 minutes (the password isn't checked while locked), and a password reset lifts the lock. A connection is dropped after 3 failures. | `LoginLockTests` |
| 7 | One-way feedback | The manager can reply. The customer has **View My Feedback**, showing read status and the reply. | `test_manager_review_feedback` |
| 8 | No account closing | Employee **Close Customer Account**: pays out the balance through the journal (`CLOSURE PAYOUT`) and closes for good. Refused for unpaid loans (checked again under the account lock) and for frozen accounts. Closed accounts can't log in, receive transfers or be reactivated. | `test_close_account`, `test_cannot_close_with_unpaid_loan` |
| 9 | An employee could approve loans for customers they opened | `User.created_by` is recorded. Such a loan can't be assigned to, processed by or decided by that employee. | `test_separation_of_duties` |
| 10 | The admin couldn't edit their own details | The admin's own record is included in **Modify Customer/Employee Details**. | `test_admin_modify_and_roles` |
| 11 | Silent reassignment of a loan being processed | Needs confirmation. `Loan.prev_employee_id` drives a notice to the previous employee. Assigning to the employee who already holds the loan no longer resets PROCESSING to ASSIGNED. | `test_reassignment_tells_first_employee` |

**Data format.** `User`, `Loan` and `Feedback` gained fields, so the files changed layout. `$BMS_DATA_DIR/format` now records the version (2). The server and `bms_init` refuse format-1 data, and the new `bin/bms_migrate` converts it, keeping the originals in `backup-format1/` (`MigrationTests`). `Transaction` and `JournalEntry` are unchanged, so history and pending journal entries carry over as they are.

**Menus renumbered.** Customer: 13 items (adds View My Loans, Repay Loan, View My Feedback). Employee: 10 (adds Close Customer Account). Admin: 8 (adds Activate/Deactivate Bank Staff). Manager: unchanged.

**Tests.** `python3 tests/test_bms.py`: 41 tests, all OK.

## 9. Evaluation support

Changes made so the evaluation criteria (class diagram, working code, concurrency and synchronization, ACID) can be checked directly:

- **Class diagram:** `class_diagram.puml` (PlantUML source) and the rendered `class_diagram.png` and `class_diagram.svg`. It uses PlantUML's built-in layout engine, so rendering needs only Java and `plantuml.jar`, not Graphviz. The records, enums, modules and executables are drawn as classes with stereotypes, since the code is C.
- **Demo mode:** `BMS_DEMO_DELAY=N` makes every balance change hold its account locks for *N* seconds, and `store.c` logs every lock that has to wait for another process to the server's terminal. This makes blocking visible during a live demo with several clients. It is off unless set. Test: `DemoModeTests.test_balance_check_waits_for_transfer` (a balance check made during a transfer waits for it, then sees the finished result).
- **`DEMO.md`:** a step-by-step live demonstration: a reader waiting for a writer, unrelated accounts not blocking each other, opposite transfers not deadlocking, a crash mid-transfer (`BMS_FAULT`), and one session per user.
- **README:** a *Concurrency and ACID properties* section maps each synchronization mechanism and each ACID property to the code and the tests that prove it.
