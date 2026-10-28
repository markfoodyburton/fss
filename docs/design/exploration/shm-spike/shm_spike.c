/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * shm_spike.c — throwaway platform spike: prove shm_open + named semaphores
 * work on THIS machine (macOS/darwin) for an SPSC cross-process ring handshake.
 *
 * This file has NO dependency on FSS headers or build system.  It is
 * reference/exploration only — do NOT add to CTest.
 *
 * Compile and run (macOS — no extra link flags needed; POSIX shm + sem are in
 * the default libSystem):
 *
 *   cc -o /tmp/shm_spike \
 *      docs/design/exploration/shm-spike/shm_spike.c \
 *      && /tmp/shm_spike
 *
 * macOS notes confirmed by running:
 *   - shm_open / ftruncate / mmap:  works normally.
 *   - sem_open (named semaphore):   works; sem_init(process-shared) is NOT
 *     implemented on macOS (returns ENOSYS), so named sems are the right
 *     choice.  sem_open names must start with '/'.
 *   - No -lrt, no -lpthread needed on macOS (everything is in libSystem).
 *   - SHM segment persists in /var/folders/... under the hood; shm_unlink
 *     removes the name but the mapping stays valid until munmap.
 *
 * Protocol (1-slot SPSC handshake):
 *   sem_empty starts at 1  (sender may fill the slot)
 *   sem_full  starts at 0  (receiver waits for data)
 *
 *   Sender:  sem_wait(empty) → write slot → sem_post(full)
 *   Receiver: sem_wait(full) → read slot  → sem_post(empty)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <errno.h>

/* ── segment layout ── */
#define SHM_NAME      "/fss_shm_spike"
#define SEM_FULL      "/fss_spike_full"
#define SEM_EMPTY     "/fss_spike_empty"

#define PAYLOAD_MAX   64
#define N_MESSAGES    5

typedef struct {
    uint32_t seq;
    char     payload[PAYLOAD_MAX];
} slot_t;

/* ── helpers ── */
static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

int main(void)
{
    /* --- create / size the SHM segment ------------------------------------ */
    /* O_CREAT|O_EXCL so a stale name from a previous crash is caught early;
     * unlink first to handle that case cleanly. */
    shm_unlink(SHM_NAME);   /* ignore error — may not exist */
    sem_unlink(SEM_FULL);
    sem_unlink(SEM_EMPTY);

    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) die("shm_open");

    if (ftruncate(fd, sizeof(slot_t)) < 0) die("ftruncate");

    slot_t *slot = mmap(NULL, sizeof(slot_t),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (slot == MAP_FAILED) die("mmap");
    close(fd);   /* fd no longer needed once mapped */

    /* --- create named semaphores ------------------------------------------ */
    /* sem_empty = 1: slot is free for the sender to fill.
     * sem_full  = 0: no data yet; receiver blocks. */
    sem_t *sem_full  = sem_open(SEM_FULL,  O_CREAT | O_EXCL, 0600, 0);
    sem_t *sem_empty = sem_open(SEM_EMPTY, O_CREAT | O_EXCL, 0600, 1);
    if (sem_full == SEM_FAILED)  die("sem_open(full)");
    if (sem_empty == SEM_FAILED) die("sem_open(empty)");

    /* --- fork -------------------------------------------------------------- */
    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        /* ===== CHILD = RECEIVER ===== */
        int errors = 0;
        for (int i = 0; i < N_MESSAGES; i++) {
            if (sem_wait(sem_full) < 0) die("child: sem_wait(full)");

            /* verify */
            char expected[PAYLOAD_MAX];
            snprintf(expected, sizeof(expected), "ping %d", i);
            if (slot->seq != (uint32_t)i ||
                strncmp(slot->payload, expected, PAYLOAD_MAX) != 0) {
                fprintf(stderr,
                    "[FAIL] child: msg %d: got seq=%u payload='%s', "
                    "expected seq=%d payload='%s'\n",
                    i, slot->seq, slot->payload, i, expected);
                errors++;
            }

            if (sem_post(sem_empty) < 0) die("child: sem_post(empty)");
        }

        /* child cleanup */
        sem_close(sem_full);
        sem_close(sem_empty);
        munmap(slot, sizeof(slot_t));

        exit(errors == 0 ? 0 : 1);

    } else {
        /* ===== PARENT = SENDER ===== */
        for (int i = 0; i < N_MESSAGES; i++) {
            if (sem_wait(sem_empty) < 0) die("parent: sem_wait(empty)");

            slot->seq = (uint32_t)i;
            snprintf(slot->payload, PAYLOAD_MAX, "ping %d", i);

            if (sem_post(sem_full) < 0) die("parent: sem_post(full)");
        }

        /* wait for child */
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);
        int child_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;

        /* --- cleanup -------------------------------------------------------- */
        sem_close(sem_full);
        sem_close(sem_empty);
        sem_unlink(SEM_FULL);
        sem_unlink(SEM_EMPTY);
        munmap(slot, sizeof(slot_t));
        shm_unlink(SHM_NAME);

        if (child_ok) {
            printf("[OK] shm-spike: %d messages crossed the process boundary\n",
                   N_MESSAGES);
            return 0;
        } else {
            fprintf(stderr, "[FAIL] shm-spike: child reported errors "
                            "(exit status %d)\n", WEXITSTATUS(wstatus));
            return 1;
        }
    }
}
