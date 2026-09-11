// suspendlag_linux: Gegentest zu suspendlag.c unter Linux, ohne Runtime.
//
// Ein Kindprozess arbeitet entweder in einer reinen User-Mode-Schleife ("spin") oder
// liest wiederholt eine mit O_DIRECT geschriebene, also nicht im Page Cache liegende
// Kopie einer 96-MB-Datei ueber mmap ("fault", harte Page Faults mit I/O-Wartezeit).
// Der Elternprozess misst dazu entweder die Signal-Latenz (kill(SIGUSR1) bis Handler,
// Modi "spin"/"fault") oder den ptrace-Stop-Vertrag: nach PTRACE_INTERRUPT und waitpid
// darf sich der Registersatz nicht mehr aendern (Modi "ptrace-spin"/"ptrace-fault").
//
// Aufruf: suspendlag_linux spin|fault|ptrace-spin|ptrace-fault [proben=3000] [mb=96]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct shared {
    volatile uint64_t handler_ns;
    volatile uint64_t seq;
    volatile uint64_t majflt;
    volatile uint64_t passes;
    volatile int stop;
};
static struct shared *sh;
static uint64_t bigsize;
static char bigpath[256];

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_sigusr1(int sig) {
    (void)sig;
    sh->handler_ns = now_ns();
    sh->seq++;
}

static int write_file(const char *path, int direct) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0);
    int fd = open(path, flags, 0600);
    if (fd < 0 && direct) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        direct = 0;
    }
    if (fd < 0) return -1;
    static unsigned char *block;
    if (!block && posix_memalign((void **)&block, 4096, 1 << 20)) return -1;
    for (size_t i = 0; i < (1u << 20); i++) block[i] = (unsigned char)(i * 2654435761u >> 24);
    for (uint64_t off = 0; off < bigsize; off += 1 << 20) {
        block[0] = (unsigned char)(off >> 20);
        if (write(fd, block, 1 << 20) != (1 << 20)) { close(fd); return -1; }
    }
    fsync(fd);
    if (!direct) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
    return 0;
}

static void child_spin(void) {
    uint64_t x = 0;
    while (!sh->stop) {
        x++;
        sh->passes = x;
    }
    _exit(0);
}

static void child_fault(void) {
    char copy[300];
    unsigned pass = 0;
    uint64_t sum = 0;
    struct rusage ru;
    while (!sh->stop) {
        snprintf(copy, sizeof copy, "%s.%u", bigpath, pass++);
        if (write_file(copy, 1) < 0) { sleep(1); continue; }
        int fd = open(copy, O_RDONLY);
        if (fd >= 0) {
            const unsigned char *v = mmap(NULL, bigsize, PROT_READ, MAP_PRIVATE, fd, 0);
            if (v != MAP_FAILED) {
                for (uint64_t i = 0; i + 8 <= bigsize && !sh->stop; i += 8) sum += *(const uint64_t *)(v + i);
                munmap((void *)v, bigsize);
            }
            close(fd);
        }
        unlink(copy);
        getrusage(RUSAGE_SELF, &ru);
        sh->majflt = (uint64_t)ru.ru_majflt;
        sh->passes = pass;
    }
    if (sum == 1) printf("%llu\n", (unsigned long long)sum);
    _exit(0);
}

static int regs_differ(const struct user_regs_struct *a, const struct user_regs_struct *b) {
    return memcmp(a, b, sizeof *a) != 0;
}

static void spin_us(double us) {
    uint64_t t0 = now_ns();
    while ((double)(now_ns() - t0) < us * 1000.0) {
    }
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "spin";
    unsigned n = argc > 2 ? (unsigned)atoi(argv[2]) : 3000;
    unsigned mb = argc > 3 ? (unsigned)atoi(argv[3]) : 96;
    bigsize = (uint64_t)mb << 20;
    int use_ptrace = strncmp(mode, "ptrace-", 7) == 0;
    int fault = strstr(mode, "fault") != NULL;

    sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) return 2;
    memset(sh, 0, sizeof *sh);
    snprintf(bigpath, sizeof bigpath, "/tmp/suspendlag_%d.bin", (int)getpid());
    if (fault && write_file(bigpath, 0) < 0) { printf("SUSPENDLAG FEHLER: Datei\n"); return 2; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    pid_t child = fork();
    if (child == 0) {
        if (fault) child_fault(); else child_spin();
    }
    usleep(fault ? 1500000 : 100000);

    unsigned b10 = 0, b100 = 0, b1000 = 0, b10000 = 0, bmore = 0, lost = 0, moved = 0, stopped_ok = 0;
    double maxlat = 0;
    srand((unsigned)now_ns());

    if (use_ptrace) {
        if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) { printf("SUSPENDLAG FEHLER: PTRACE_SEIZE %s\n", strerror(errno)); sh->stop = 1; return 2; }
        for (unsigned i = 0; i < n; i++) {
            spin_us((double)rand() * 3000.0 / RAND_MAX);
            uint64_t t0 = now_ns();
            if (ptrace(PTRACE_INTERRUPT, child, 0, 0) != 0) break;
            int status = 0;
            if (waitpid(child, &status, __WALL) != child || !WIFSTOPPED(status)) break;
            double lat = (double)(now_ns() - t0) / 1000.0;
            if (lat > maxlat) maxlat = lat;
            if (lat <= 10) b10++; else if (lat <= 100) b100++; else if (lat <= 1000) b1000++; else if (lat <= 10000) b10000++; else bmore++;
            struct user_regs_struct r1, r2;
            if (ptrace(PTRACE_GETREGS, child, 0, &r1) == 0) {
                stopped_ok++;
                spin_us(20);
                if (ptrace(PTRACE_GETREGS, child, 0, &r2) == 0 && regs_differ(&r1, &r2)) moved++;
            }
            ptrace(PTRACE_CONT, child, 0, 0);
        }
        sh->stop = 1;
        ptrace(PTRACE_DETACH, child, 0, 0);
    } else {
        for (unsigned i = 0; i < n; i++) {
            spin_us((double)rand() * 3000.0 / RAND_MAX);
            uint64_t seq0 = sh->seq, t0 = now_ns();
            kill(child, SIGUSR1);
            while (sh->seq == seq0 && now_ns() - t0 < 200000000ull) {
            }
            if (sh->seq == seq0) { lost++; continue; }
            double lat = (double)(sh->handler_ns - t0) / 1000.0;
            if (lat > maxlat) maxlat = lat;
            if (lat <= 10) b10++; else if (lat <= 100) b100++; else if (lat <= 1000) b1000++; else if (lat <= 10000) b10000++; else bmore++;
        }
        sh->stop = 1;
    }
    waitpid(child, NULL, 0);
    if (fault) unlink(bigpath);
    printf("SUSPENDLAG_LINUX mode=%s n=%u | latenz<=10us=%u <=100us=%u <=1ms=%u <=10ms=%u >10ms=%u verloren=%u max=%.0fus | "
           "ptrace: gestoppt=%u bewegt_nach_stop=%u | worker: passes=%llu majflt=%llu\n",
           mode, n, b10, b100, b1000, b10000, bmore, lost, maxlat, stopped_ok, moved,
           (unsigned long long)sh->passes, (unsigned long long)sh->majflt);
    return 0;
}
