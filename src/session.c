#include "session.h"

#include <errno.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_SESSIONS 256

typedef struct {
    int uid; /* 0 = free slot */
    pid_t pid;
} Slot;

static int shm_id = -1;
static int sem_id = -1;
static Slot *table;

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

static int sem_change(short delta)
{
    /* SEM_UNDO: if a handler dies inside the critical section the kernel
     * gives the semaphore back. */
    struct sembuf op = {.sem_num = 0, .sem_op = delta, .sem_flg = SEM_UNDO};
    while (semop(sem_id, &op, 1) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int session_init(void)
{
    shm_id = shmget(IPC_PRIVATE, sizeof(Slot) * MAX_SESSIONS, IPC_CREAT | 0600);
    if (shm_id < 0)
        return -1;
    table = shmat(shm_id, NULL, 0);
    if (table == (void *)-1)
        return -1;
    for (int i = 0; i < MAX_SESSIONS; i++)
        table[i] = (Slot){0, 0};

    sem_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_id < 0)
        return -1;
    union semun arg = {.val = 1};
    if (semctl(sem_id, 0, SETVAL, arg) < 0)
        return -1;
    return 0;
}

void session_destroy(void)
{
    if (table && table != (void *)-1)
        shmdt(table);
    if (shm_id >= 0)
        shmctl(shm_id, IPC_RMID, NULL);
    if (sem_id >= 0)
        semctl(sem_id, 0, IPC_RMID);
}

static int pid_alive(pid_t pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

int session_acquire(int uid)
{
    int result = -1;
    int free_slot = -1;

    if (sem_change(-1) < 0)
        return -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (table[i].uid != 0 && !pid_alive(table[i].pid))
            table[i] = (Slot){0, 0}; /* handler died without logging out */
        if (table[i].uid == uid) {
            result = 1;
            goto out;
        }
        if (table[i].uid == 0 && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0) {
        table[free_slot] = (Slot){uid, getpid()};
        result = 0;
    }
out:
    sem_change(1);
    return result;
}

void session_release(int uid)
{
    if (sem_change(-1) < 0)
        return;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (table[i].uid == uid && table[i].pid == getpid())
            table[i] = (Slot){0, 0};
    }
    sem_change(1);
}
