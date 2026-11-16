/* whoblocks: show which threads are blocked on which process, and report a
 * cycle if processes are waiting on each other. Built step by step in the
 * codelab in this directory. QNX OS 8.0; run as root. */
#include <ctype.h>
#include <devctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/states.h>
#include <unistd.h>

#define MAX_WAITS 512

struct wait {
    pid_t pid;    /* the blocked thread's process */
    int tid;      /* the blocked thread */
    pid_t server; /* the process it is waiting for */
    int sent;     /* 1 = SEND-blocked, 0 = REPLY-blocked */
};

static struct wait waits[MAX_WAITS];
static int n_waits;

/* Step 3: the name of a process is the last part of /proc/<pid>/exefile. */
static const char *name_of(pid_t pid)
{
    static char path[64], exe[1024];
    snprintf(path, sizeof(path), "/proc/%d/exefile", (int)pid);
    int fd = open(path, O_RDONLY);
    ssize_t n = fd == -1 ? -1 : read(fd, exe, sizeof(exe) - 1);
    if (fd != -1) {
        close(fd);
    }
    if (n <= 0) {
        return "?";
    }
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    return slash ? slash + 1 : exe;
}

/* Step 2: ask the process manager about every thread of one process. */
static void scan_process(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ctl", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        return; /* gone already, or not ours to look at */
    }
    procfs_status status;
    memset(&status, 0, sizeof(status));
    /* The call returns the first thread whose id is >= status.tid. */
    for (status.tid = 1;
         devctl(fd, DCMD_PROC_TIDSTATUS, &status, sizeof(status), NULL) == EOK;
         status.tid++) {
        if (status.state != STATE_SEND && status.state != STATE_REPLY) {
            continue;
        }
        if (status.blocked.connect.pid == pid) {
            continue; /* waiting on a thread of its own process */
        }
        if (n_waits < MAX_WAITS) {
            waits[n_waits].pid = pid;
            waits[n_waits].tid = status.tid;
            waits[n_waits].server = status.blocked.connect.pid;
            waits[n_waits].sent = status.state == STATE_SEND;
            n_waits++;
        }
    }
    close(fd);
}

/* Step 4: the process that `pid` is waiting for, or 0 if it is not waiting. */
static pid_t waits_for(pid_t pid)
{
    for (int i = 0; i < n_waits; i++) {
        if (waits[i].pid == pid) {
            return waits[i].server;
        }
    }
    return 0;
}

int main(void)
{
    /* Step 1: every process is a numbered directory under /proc. */
    DIR *proc = opendir("/proc");
    if (proc == NULL) {
        perror("/proc");
        return 1;
    }
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (isdigit((unsigned char)entry->d_name[0])) {
            scan_process((pid_t)atoi(entry->d_name));
        }
    }
    closedir(proc);

    for (int i = 0; i < n_waits; i++) {
        printf("%s[%d] thread %d is %s-blocked on ", name_of(waits[i].pid),
               (int)waits[i].pid, waits[i].tid, waits[i].sent ? "SEND" : "REPLY");
        printf("%s[%d]\n", name_of(waits[i].server), (int)waits[i].server);
    }

    /* Step 4: follow "is waiting for" from each process. If the walk comes
     * back to where it started, those processes wait on each other. Each
     * cycle is printed once, from its lowest pid. */
    int cycles = 0;
    for (int i = 0; i < n_waits; i++) {
        pid_t start = waits[i].pid;
        pid_t at = waits_for(start);
        int lowest = 1;
        for (int steps = 0; at != 0 && at != start && steps < n_waits; steps++) {
            lowest = lowest && at > start;
            at = waits_for(at);
        }
        if (at != start || !lowest || (i > 0 && waits[i - 1].pid == start)) {
            continue;
        }
        printf("cycle: %s[%d]", name_of(start), (int)start);
        for (at = waits_for(start); at != start; at = waits_for(at)) {
            printf(" -> %s[%d]", name_of(at), (int)at);
        }
        printf(" -> %s[%d]\n", name_of(start), (int)start);
        cycles++;
    }
    return cycles ? 2 : 0;
}
