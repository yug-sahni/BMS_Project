#include "ops.h"
#include "net.h"
#include "store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const char *role_name(int role)
{
    switch (role) {
    case ROLE_CUSTOMER: return "Customer";
    case ROLE_EMPLOYEE: return "Employee";
    case ROLE_MANAGER: return "Manager";
    case ROLE_ADMIN: return "Administrator";
    default: return "Unknown";
    }
}

const char *txn_type_name(int type)
{
    switch (type) {
    case TXN_DEPOSIT: return "DEPOSIT";
    case TXN_WITHDRAW: return "WITHDRAW";
    case TXN_TRANSFER_OUT: return "TRANSFER OUT";
    case TXN_TRANSFER_IN: return "TRANSFER IN";
    case TXN_LOAN_CREDIT: return "LOAN CREDIT";
    case TXN_LOAN_REPAY: return "LOAN REPAY";
    case TXN_CLOSURE: return "CLOSURE PAYOUT";
    default: return "UNKNOWN";
    }
}

const char *loan_status_name(int status)
{
    switch (status) {
    case LOAN_PENDING: return "PENDING";
    case LOAN_ASSIGNED: return "ASSIGNED";
    case LOAN_PROCESSING: return "PROCESSING";
    case LOAN_APPROVED: return "APPROVED";
    case LOAN_REJECTED: return "REJECTED";
    case LOAN_REPAID: return "REPAID";
    default: return "UNKNOWN";
    }
}

const char *account_status(const User *u)
{
    if (u->closed_at)
        return "closed";
    return u->active ? "active" : "deactivated";
}

int valid_username(const char *s)
{
    size_t len = strlen(s);

    if (len == 0 || len >= USERNAME_LEN)
        return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-')
            return 0;
    }
    return 1;
}

int valid_password(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && len < PASSWORD_MAX;
}

int valid_phone(const char *s)
{
    size_t len = strlen(s);

    if (len == 0 || len >= PHONE_LEN)
        return 0;
    if (*s == '+')
        s++;
    if (*s == '\0')
        return 0;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s))
            return 0;
    }
    return 1;
}

int get_user(int id, User *out)
{
    if (id < 1)
        return 0;
    if (rec_lock(FILE_USERS, id, F_RDLCK) < 0)
        return -1;
    int r = rec_read(FILE_USERS, id, out);
    rec_unlock(FILE_USERS, id);
    return r;
}

/* Caller holds a lock on the users file. */
static int scan_username(const char *username, User *out)
{
    int n = rec_count(FILE_USERS);
    User u;

    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_USERS, id, &u) == 1 && strcmp(u.username, username) == 0) {
            if (out)
                *out = u;
            return id;
        }
    }
    return 0;
}

int find_user_by_username(const char *username, User *out)
{
    if (file_lock(FILE_USERS, F_RDLCK) < 0)
        return 0;
    int id = scan_username(username, out);
    file_unlock(FILE_USERS);
    return id;
}

/* Prompts for a text field; empty input keeps current (if keep is set). */
static int prompt_text(const char *label, char *field, size_t size, int keep)
{
    char buf[256];

    prompt_line(label, buf, sizeof(buf));
    if (buf[0] == '\0') {
        if (keep)
            return 0;
        net_puts("This field cannot be empty.\n");
        return -1;
    }
    if (strlen(buf) >= size) {
        net_printf("Too long: at most %zu characters.\n", size - 1);
        return -1;
    }
    strcpy(field, buf);
    return 0;
}

static int prompt_phone(const char *label, char *field, int keep)
{
    char buf[64];

    prompt_line(label, buf, sizeof(buf));
    if (buf[0] == '\0' && keep)
        return 0;
    if (!valid_phone(buf)) {
        net_printf("Invalid phone: digits only (optional leading +), at most %d characters.\n",
                   PHONE_LEN - 1);
        return -1;
    }
    strcpy(field, buf);
    return 0;
}

int create_user_interactive(int role, int creator)
{
    User u;
    char password[PASSWORD_MAX + 8];

    memset(&u, 0, sizeof(u));
    prompt_line("Username: ", u.username, sizeof(u.username));
    if (!valid_username(u.username)) {
        net_printf("Invalid username: 1-%d characters, letters, digits, '_', '.', '-'.\n",
                   USERNAME_LEN - 1);
        return -1;
    }
    prompt_secret("Initial password: ", password, sizeof(password));
    if (!valid_password(password)) {
        net_printf("Invalid password: 1-%d characters.\n", PASSWORD_MAX - 1);
        return -1;
    }
    if (prompt_text("Full name: ", u.name, sizeof(u.name), 0) < 0 ||
        prompt_phone("Phone: ", u.phone, 0) < 0 ||
        prompt_text("Address: ", u.address, sizeof(u.address), 0) < 0)
        return -1;
    if (hash_password(password, u.pwhash, sizeof(u.pwhash)) < 0) {
        net_puts("Internal error while hashing password.\n");
        return -1;
    }
    u.role = role;
    u.active = 1;
    u.balance = 0;
    u.created_by = creator;

    /* Uniqueness check and append happen under one exclusive file lock. */
    if (file_lock(FILE_USERS, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock user file.\n");
        return -1;
    }
    int id = -1;
    if (scan_username(u.username, NULL) > 0)
        net_printf("Username '%s' is already taken.\n", u.username);
    else if ((id = rec_append_locked(FILE_USERS, &u)) < 0)
        net_puts("Internal error: could not save user.\n");
    file_unlock(FILE_USERS);
    if (id > 0)
        net_printf("%s created. %s: %d, username: %s\n", role_name(role),
                   role == ROLE_CUSTOMER ? "Account number" : "ID", id, u.username);
    return id;
}

void modify_user_interactive(int id, unsigned allowed_mask)
{
    User u, cur;

    if (get_user(id, &cur) != 1 || !(allowed_mask & (1u << cur.role))) {
        net_puts("No such user with a role you may modify.\n");
        return;
    }
    net_printf("Current details of %s #%d (%s):\n  Name: %s\n  Phone: %s\n  Address: %s\n",
               role_name(cur.role), cur.id, cur.username, cur.name, cur.phone, cur.address);
    net_puts("Enter new values (leave blank to keep current).\n");
    u = cur;
    if (prompt_text("Full name: ", u.name, sizeof(u.name), 1) < 0 ||
        prompt_phone("Phone: ", u.phone, 1) < 0 ||
        prompt_text("Address: ", u.address, sizeof(u.address), 1) < 0)
        return;

    if (rec_lock(FILE_USERS, id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock record.\n");
        return;
    }
    /* Re-read under the lock so a concurrent balance update is not lost. */
    if (rec_read(FILE_USERS, id, &cur) == 1 && (allowed_mask & (1u << cur.role))) {
        strcpy(cur.name, u.name);
        strcpy(cur.phone, u.phone);
        strcpy(cur.address, u.address);
        if (rec_write(FILE_USERS, id, &cur) == 0)
            net_puts("Details updated.\n");
        else
            net_puts("Internal error: could not save.\n");
    } else {
        net_puts("User changed meanwhile; nothing updated.\n");
    }
    rec_unlock(FILE_USERS, id);
}

void change_password(int uid)
{
    char old_pw[PASSWORD_MAX + 8], new_pw[PASSWORD_MAX + 8], confirm[PASSWORD_MAX + 8];
    char hash[PWHASH_LEN];
    User u;

    prompt_secret("Current password: ", old_pw, sizeof(old_pw));
    prompt_secret("New password: ", new_pw, sizeof(new_pw));
    prompt_secret("Confirm new password: ", confirm, sizeof(confirm));
    if (!valid_password(new_pw)) {
        net_printf("Invalid password: 1-%d characters.\n", PASSWORD_MAX - 1);
        return;
    }
    if (strcmp(new_pw, confirm) != 0) {
        net_puts("Passwords do not match.\n");
        return;
    }
    if (hash_password(new_pw, hash, sizeof(hash)) < 0) {
        net_puts("Internal error while hashing password.\n");
        return;
    }
    if (rec_lock(FILE_USERS, uid, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock record.\n");
        return;
    }
    if (rec_read(FILE_USERS, uid, &u) != 1 || !verify_password(old_pw, u.pwhash)) {
        net_puts("Current password is incorrect.\n");
    } else {
        strcpy(u.pwhash, hash);
        if (rec_write(FILE_USERS, uid, &u) == 0)
            net_puts("Password changed.\n");
        else
            net_puts("Internal error: could not save.\n");
    }
    rec_unlock(FILE_USERS, uid);
}

int session_still_valid(int uid, int role, User *out)
{
    if (get_user(uid, out) != 1 || !out->active || out->role != role) {
        net_puts("Your account was deactivated or its role changed. Logging out.\n");
        return -1;
    }
    return 0;
}

void print_transactions(int customer_id)
{
    Transaction t;
    char amt[32], bal[32], when[32];
    int shown = 0;

    if (file_lock(FILE_TXNS, F_RDLCK) < 0) {
        net_puts("Internal error: could not lock transactions.\n");
        return;
    }
    int n = rec_count(FILE_TXNS);
    net_printf("%-6s %-19s %-13s %16s %16s %s\n", "Txn", "Date/Time", "Type", "Amount",
               "Balance", "Counterparty");
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_TXNS, id, &t) != 1 || t.customer_id != customer_id)
            continue;
        char party[24] = "-";
        if (t.type == TXN_LOAN_CREDIT || t.type == TXN_LOAN_REPAY)
            snprintf(party, sizeof(party), "loan %d", t.counterparty_id);
        else if (t.counterparty_id)
            snprintf(party, sizeof(party), "#%d", t.counterparty_id);
        net_printf("%-6d %-19s %-13s %16s %16s %s\n", t.id,
                   fmt_time(t.timestamp, when, sizeof(when)), txn_type_name(t.type),
                   fmt_amount(t.amount, amt, sizeof(amt)),
                   fmt_amount(t.balance_after, bal, sizeof(bal)), party);
        shown++;
    }
    file_unlock(FILE_TXNS);
    if (!shown)
        net_puts("(no transactions)\n");
}

void print_loan_header(void)
{
    net_printf("%-6s %-9s %16s %-7s %-10s %-9s %s\n", "LoanID", "Customer", "Amount", "Months",
               "Status", "Employee", "Purpose");
}

void print_loan(const Loan *l)
{
    char amt[32], emp[16] = "-";

    if (l->employee_id)
        snprintf(emp, sizeof(emp), "#%d", l->employee_id);
    net_printf("%-6d #%-8d %16s %-7d %-10s %-9s %s\n", l->id, l->customer_id,
               fmt_amount(l->amount, amt, sizeof(amt)), l->tenure_months,
               loan_status_name(l->status), emp, l->purpose);
}

void list_users(unsigned role_mask)
{
    User u;
    int shown = 0;

    if (file_lock(FILE_USERS, F_RDLCK) < 0) {
        net_puts("Internal error: could not lock user file.\n");
        return;
    }
    int n = rec_count(FILE_USERS);
    net_printf("  %-6s %-20s %-30s %-13s %s\n", "ID", "Username", "Name", "Role", "Status");
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_USERS, id, &u) == 1 && (role_mask & (1u << u.role))) {
            net_printf("  #%-5d %-20s %-30s %-13s %s\n", u.id, u.username, u.name,
                       role_name(u.role), account_status(&u));
            shown++;
        }
    }
    file_unlock(FILE_USERS);
    if (!shown)
        net_puts("  (none)\n");
}

int loan_txn_total(int customer_id, int loan_id, int type, int64_t *sum)
{
    Transaction t;

    *sum = 0;
    if (file_lock(FILE_TXNS, F_RDLCK) < 0)
        return -1;
    int n = rec_count(FILE_TXNS);
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_TXNS, id, &t) != 1 || t.type != type)
            continue;
        if ((customer_id && t.customer_id != customer_id) ||
            (loan_id && t.counterparty_id != loan_id))
            continue;
        *sum += t.amount;
    }
    file_unlock(FILE_TXNS);
    return 0;
}

int loan_outstanding(const Loan *l, int64_t *out)
{
    int64_t repaid;

    *out = 0;
    if (l->status != LOAN_APPROVED)
        return 0;
    if (loan_txn_total(l->customer_id, l->id, TXN_LOAN_REPAY, &repaid) < 0)
        return -1;
    *out = l->amount > repaid ? l->amount - repaid : 0;
    return 0;
}

int customer_loan_debt(int customer_id, int64_t *out)
{
    int64_t credited, repaid;

    if (loan_txn_total(customer_id, 0, TXN_LOAN_CREDIT, &credited) < 0 ||
        loan_txn_total(customer_id, 0, TXN_LOAN_REPAY, &repaid) < 0)
        return -1;
    *out = credited > repaid ? credited - repaid : 0;
    return 0;
}

int64_t loan_installment(const Loan *l)
{
    return (l->amount + l->tenure_months - 1) / l->tenure_months;
}

int loan_is_open(const Loan *l)
{
    return l->status == LOAN_PENDING || l->status == LOAN_ASSIGNED ||
           l->status == LOAN_PROCESSING;
}

int reject_open_loans(int customer_id, const char *note)
{
    Loan l;
    int rejected = 0;

    if (file_lock(FILE_LOANS, F_WRLCK) < 0)
        return -1;
    int n = rec_count(FILE_LOANS);
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_LOANS, id, &l) != 1 || l.customer_id != customer_id ||
            !loan_is_open(&l))
            continue;
        l.status = LOAN_REJECTED;
        l.decided_at = (int64_t)time(NULL);
        snprintf(l.note, sizeof(l.note), "%s", note);
        if (rec_write(FILE_LOANS, id, &l) < 0) {
            rejected = -1;
            break;
        }
        rejected++;
    }
    file_unlock(FILE_LOANS);
    return rejected;
}

int prompt_reason(const char *label, char *buf, size_t size)
{
    prompt_line(label, buf, size);
    if (buf[0] == '\0') {
        net_puts("A reason is required.\n");
        return -1;
    }
    return 0;
}
