#include "net.h"
#include "ops.h"
#include "store.h"

#include <limits.h>
#include <string.h>

#define STAFF_MASK ((1u << ROLE_EMPLOYEE) | (1u << ROLE_MANAGER))

/* Any user, including administrators (their own name, phone, address). */
static void modify_user(void)
{
    long id;
    unsigned mask = (1u << ROLE_CUSTOMER) | STAFF_MASK | (1u << ROLE_ADMIN);

    list_users(mask);
    if (prompt_int("User ID: ", 1, INT_MAX, &id) < 0)
        return;
    modify_user_interactive((int)id, mask);
}

/* Puts the open applications assigned to emp_id back in the pending queue
 * so a manager can reassign them. Returns how many, or -1 on error. */
static int release_employee_loans(int emp_id)
{
    Loan l;
    int released = 0;

    if (file_lock(FILE_LOANS, F_WRLCK) < 0)
        return -1;
    int n = rec_count(FILE_LOANS);
    for (int id = 1; id <= n; id++) {
        if (rec_read(FILE_LOANS, id, &l) != 1 || l.employee_id != emp_id ||
            (l.status != LOAN_ASSIGNED && l.status != LOAN_PROCESSING))
            continue;
        l.prev_employee_id = emp_id;
        l.employee_id = 0;
        l.status = LOAN_PENDING;
        if (rec_write(FILE_LOANS, id, &l) < 0) {
            released = -1;
            break;
        }
        released++;
    }
    file_unlock(FILE_LOANS);
    return released;
}

static void report_released(int n)
{
    if (n < 0)
        net_puts("Internal error: could not release this employee's loan applications.\n");
    else if (n > 0)
        net_printf("%d open loan application(s) returned to the pending queue.\n", n);
}

/* Bank staff move between Employee and Manager. */
static void manage_roles(void)
{
    long id, choice;
    User u;
    int promoted = 0;

    list_users((1u << ROLE_EMPLOYEE) | (1u << ROLE_MANAGER));
    if (prompt_int("Employee ID: ", 1, INT_MAX, &id) < 0)
        return;
    if (get_user((int)id, &u) != 1 || (u.role != ROLE_EMPLOYEE && u.role != ROLE_MANAGER)) {
        net_puts("No such employee or manager.\n");
        return;
    }
    net_printf("#%d %s (%s) currently has role: %s\n", u.id, u.name, u.username,
               role_name(u.role));
    if (prompt_int("New role: 1. Employee  2. Manager\nOption: ", 1, 2, &choice) < 0)
        return;
    int new_role = choice == 1 ? ROLE_EMPLOYEE : ROLE_MANAGER;

    if (rec_lock(FILE_USERS, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock record.\n");
        return;
    }
    if (rec_read(FILE_USERS, (int)id, &u) == 1 &&
        (u.role == ROLE_EMPLOYEE || u.role == ROLE_MANAGER)) {
        int was_employee = u.role == ROLE_EMPLOYEE;
        u.role = new_role;
        if (rec_write(FILE_USERS, (int)id, &u) == 0) {
            net_printf("#%d is now %s.\n", u.id, role_name(u.role));
            promoted = was_employee && new_role == ROLE_MANAGER;
        } else {
            net_puts("Internal error: could not save.\n");
        }
    }
    rec_unlock(FILE_USERS, (int)id);

    /* After the role change is saved, so no new assignment can slip in. */
    if (promoted)
        report_released(release_employee_loans((int)id));
}

/* Staff who leave the bank lose access; their open loan applications go
 * back to the pending queue. */
static void set_staff_active(void)
{
    long id, choice, confirm;
    User u;
    char reason[NOTE_LEN] = "";
    int released = 0;

    list_users(STAFF_MASK);
    if (prompt_int("Employee ID: ", 1, INT_MAX, &id) < 0)
        return;
    if (get_user((int)id, &u) != 1 || !(STAFF_MASK & (1u << u.role))) {
        net_puts("No such employee or manager.\n");
        return;
    }
    net_printf("#%d %s (%s), %s, is currently %s.\n", u.id, u.name, u.username,
               role_name(u.role), u.active ? "ACTIVE" : "DEACTIVATED");
    if (!u.active && u.status_note[0])
        net_printf("Deactivated because: %s\n", u.status_note);
    if (prompt_int("1. Activate  2. Deactivate\nOption: ", 1, 2, &choice) < 0)
        return;
    if ((choice == 1) == (u.active != 0)) {
        net_printf("#%d is already %s.\n", u.id, u.active ? "ACTIVE" : "DEACTIVATED");
        return;
    }
    if (choice == 2) {
        if (prompt_reason("Reason for deactivating: ", reason, sizeof(reason)) < 0)
            return;
        if (prompt_int("They will be logged out and unable to log in.\n"
                       "1. Deactivate  2. Cancel\nOption: ", 1, 2, &confirm) < 0 ||
            confirm != 1) {
            net_puts("Cancelled.\n");
            return;
        }
    }
    if (rec_lock(FILE_USERS, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock record.\n");
        return;
    }
    if (rec_read(FILE_USERS, (int)id, &u) == 1 && (STAFF_MASK & (1u << u.role))) {
        u.active = choice == 1;
        strcpy(u.status_note, reason);
        if (rec_write(FILE_USERS, (int)id, &u) == 0) {
            net_printf("#%d is now %s.\n", u.id, u.active ? "ACTIVE" : "DEACTIVATED");
            released = !u.active;
        } else {
            net_puts("Internal error: could not save.\n");
        }
    }
    rec_unlock(FILE_USERS, (int)id);

    /* After the record is saved, so no new assignment can slip in. */
    if (released)
        report_released(release_employee_loans((int)id));
}

/* For staff and customers who forgot their password: passwords are stored
 * only as hashes, so the administrator sets a new one. This also lifts a
 * lock left by too many failed logins. */
static void reset_password(void)
{
    long id;
    unsigned mask = (1u << ROLE_CUSTOMER) | (1u << ROLE_EMPLOYEE) | (1u << ROLE_MANAGER);
    char pw[PASSWORD_MAX + 8], confirm[PASSWORD_MAX + 8], hash[PWHASH_LEN];
    User u;

    list_users(mask);
    if (prompt_int("User ID: ", 1, INT_MAX, &id) < 0)
        return;
    if (get_user((int)id, &u) != 1 || !(mask & (1u << u.role))) {
        net_puts("No such customer, employee or manager.\n");
        return;
    }
    prompt_secret("New password: ", pw, sizeof(pw));
    prompt_secret("Confirm new password: ", confirm, sizeof(confirm));
    if (!valid_password(pw)) {
        net_printf("Invalid password: 1-%d characters.\n", PASSWORD_MAX - 1);
        return;
    }
    if (strcmp(pw, confirm) != 0) {
        net_puts("Passwords do not match.\n");
        return;
    }
    if (hash_password(pw, hash, sizeof(hash)) < 0) {
        net_puts("Internal error while hashing password.\n");
        return;
    }
    if (rec_lock(FILE_USERS, (int)id, F_WRLCK) < 0) {
        net_puts("Internal error: could not lock record.\n");
        return;
    }
    if (rec_read(FILE_USERS, (int)id, &u) == 1 && (mask & (1u << u.role))) {
        int was_locked = u.locked_until > 0;
        strcpy(u.pwhash, hash);
        u.failed_logins = 0;
        u.locked_until = 0;
        if (rec_write(FILE_USERS, (int)id, &u) == 0)
            net_printf("Password for #%d (%s) has been reset%s.\n", u.id, u.username,
                       was_locked ? " and the login lock lifted" : "");
        else
            net_puts("Internal error: could not save.\n");
    }
    rec_unlock(FILE_USERS, (int)id);
}

void admin_menu(int uid)
{
    User me;
    long choice;

    for (;;) {
        if (session_still_valid(uid, ROLE_ADMIN, &me) < 0)
            return;
        net_printf("\n===== Administrator Menu (%s) =====\n"
                   " 1. Add New Bank Employee\n"
                   " 2. Modify Customer/Employee Details\n"
                   " 3. Manage User Roles\n"
                   " 4. Activate/Deactivate Bank Staff\n"
                   " 5. Reset User Password\n"
                   " 6. Change Password\n"
                   " 7. Logout\n"
                   " 8. Exit\n",
                   me.username);
        if (prompt_int("Choice: ", 1, 8, &choice) < 0)
            continue;
        /* Re-check: the account may have changed while the user was typing. */
        if (session_still_valid(uid, ROLE_ADMIN, &me) < 0)
            return;
        switch (choice) {
        case 1: create_user_interactive(ROLE_EMPLOYEE, uid); break;
        case 2: modify_user(); break;
        case 3: manage_roles(); break;
        case 4: set_staff_active(); break;
        case 5: reset_password(); break;
        case 6: change_password(uid); break;
        case 7: return;
        case 8: exit_session();
        }
    }
}
