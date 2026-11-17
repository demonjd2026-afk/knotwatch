/* On-target test for mutex-owner tracking. Not part of the demo.
 *
 *   kwt_locks threads   two threads other than the main thread take two
 *                       locks in opposite order; the main thread joins one
 *   kwt_locks shared    parent and child take two process-shared locks in
 *                       opposite order
 *
 * Either way the program deadlocks on purpose and never exits. knotwatch
 * should name the owner of each lock: a thread that is not thread 1 in the
 * first case, a thread of another process in the second. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

static void take_both(pthread_mutex_t *first, pthread_mutex_t *second)
{
    pthread_mutex_lock(first);
    sleep(1); /* let the other side take its first lock */
    pthread_mutex_lock(second); /* never returns */
}

static void *a_then_b(void *arg)
{
    (void)arg;
    take_both(&lock_a, &lock_b);
    return NULL;
}

static void *b_then_a(void *arg)
{
    (void)arg;
    take_both(&lock_b, &lock_a);
    return NULL;
}

static int run_threads(void)
{
    pthread_t t2, t3;
    if (pthread_create(&t2, NULL, a_then_b, NULL) != 0 ||
        pthread_create(&t3, NULL, b_then_a, NULL) != 0) {
        fputs("kwt_locks: pthread_create failed\n", stderr);
        return 1;
    }
    pthread_join(t2, NULL); /* never returns */
    return 0;
}

static int run_shared(void)
{
    pthread_mutex_t *m = mmap(NULL, 2 * sizeof(*m), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        perror("kwt_locks: mmap");
        return 1;
    }
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_mutex_init(&m[0], &attr) != 0 || pthread_mutex_init(&m[1], &attr) != 0) {
        fputs("kwt_locks: cannot create process-shared mutexes\n", stderr);
        return 1;
    }
    pid_t child = fork();
    if (child == -1) {
        perror("kwt_locks: fork");
        return 1;
    }
    if (child == 0) {
        take_both(&m[1], &m[0]);
    } else {
        take_both(&m[0], &m[1]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "threads") == 0) {
        return run_threads();
    }
    if (argc == 2 && strcmp(argv[1], "shared") == 0) {
        return run_shared();
    }
    fputs("usage: kwt_locks threads|shared\n", stderr);
    return 64;
}
