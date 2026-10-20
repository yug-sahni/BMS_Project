/* Creates the data files and the first administrator account.
 * Credentials come from BMS_ADMIN_USERNAME / BMS_ADMIN_PASSWORD or are
 * prompted for; nothing is hardcoded. Refuses to run on a non-empty store. */
#include "ops.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void read_line(const char *label, char *buf, size_t size, int secret)
{
    struct termios old, t;
    int tty = secret && isatty(STDIN_FILENO);
    size_t len = 0;
    char c;

    write(STDOUT_FILENO, label, strlen(label));
    if (tty) {
        tcgetattr(STDIN_FILENO, &old);
        t = old;
        t.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    while (read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {
        if (len + 1 < size)
            buf[len++] = c;
    }
    buf[len] = '\0';
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        write(STDOUT_FILENO, "\n", 1);
    }
}

int main(void)
{
    const char *data_dir = getenv("BMS_DATA_DIR") ? getenv("BMS_DATA_DIR") : "data";
    char username[USERNAME_LEN + 8], password[PASSWORD_MAX + 8], confirm[PASSWORD_MAX + 8];
    User u;

    int opened = store_open(data_dir);
    if (opened == STORE_OLD_FORMAT) {
        fprintf(stderr, "%s holds data in an older format; run bin/bms_migrate first.\n",
                data_dir);
        return 1;
    }
    if (opened < 0) {
        perror("Opening data files");
        return 1;
    }
    if (rec_count(FILE_USERS) != 0) {
        fprintf(stderr, "%s already contains users; refusing to re-initialise.\n", data_dir);
        return 1;
    }

    if (getenv("BMS_ADMIN_USERNAME"))
        snprintf(username, sizeof(username), "%s", getenv("BMS_ADMIN_USERNAME"));
    else
        read_line("Administrator username: ", username, sizeof(username), 0);
    if (getenv("BMS_ADMIN_PASSWORD")) {
        snprintf(password, sizeof(password), "%s", getenv("BMS_ADMIN_PASSWORD"));
    } else {
        read_line("Administrator password: ", password, sizeof(password), 1);
        read_line("Confirm password: ", confirm, sizeof(confirm), 1);
        if (strcmp(password, confirm) != 0) {
            fprintf(stderr, "Passwords do not match.\n");
            return 1;
        }
    }
    if (!valid_username(username) || !valid_password(password)) {
        fprintf(stderr, "Invalid username (letters, digits, '_', '.', '-'; max %d) or "
                        "password (1-%d characters).\n",
                USERNAME_LEN - 1, PASSWORD_MAX - 1);
        return 1;
    }

    memset(&u, 0, sizeof(u));
    strcpy(u.username, username);
    strcpy(u.name, "Administrator");
    u.role = ROLE_ADMIN;
    u.active = 1;
    if (hash_password(password, u.pwhash, sizeof(u.pwhash)) < 0) {
        fprintf(stderr, "Could not hash password.\n");
        return 1;
    }

    if (file_lock(FILE_USERS, F_WRLCK) < 0) {
        perror("Locking users file");
        return 1;
    }
    int id = rec_count(FILE_USERS) == 0 ? rec_append_locked(FILE_USERS, &u) : -1;
    file_unlock(FILE_USERS);
    if (id < 0) {
        fprintf(stderr, "Could not create administrator.\n");
        return 1;
    }
    printf("Data directory '%s' initialised. Administrator '%s' created (ID %d).\n", data_dir,
           u.username, id);
    return 0;
}
