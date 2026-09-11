// suspendlag: misst, wie lange ein Windows-Thread nach SuspendThread + GetThreadContext
// noch weiterlaeuft. Kein Go, keine CLR, nur Win32.
//
// Ein Worker-Thread rechnet entweder in einer reinen User-Mode-Schleife ("spin") oder
// liest wiederholt eine frische Kopie einer grossen Datei ueber eine neue Mapping-Section
// ("fault", Page Faults wie beim Kaltstart eines Binaries). Der Hauptthread friert ihn
// zu zufaelligen Zeitpunkten ein, liest den Kontext mit CONTEXT_EXCEPTION_REQUEST und
// liest dann so lange weiter, bis sich RIP, RSP und die Integer-Register nicht mehr
// aendern. Die Zeit bis zur letzten Aenderung ist der Nachlauf.
//
// Aufruf: suspendlag.exe spin|fault|image [iterationen=2000] [dateigroesse_mb=256]
// "image": frische Kopie des eigenen Binaries als SEC_IMAGE mappen und den 96-MB-Blob lesen.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONTEXT_EXCEPTION_ACTIVE
#define CONTEXT_EXCEPTION_ACTIVE 0x08000000
#define CONTEXT_SERVICE_ACTIVE 0x10000000
#define CONTEXT_EXCEPTION_REQUEST 0x40000000
#define CONTEXT_EXCEPTION_REPORTING 0x80000000
#endif

// 96 MB Nutzlast im eigenen .rdata, damit eine frische Kopie dieses Binaries als
// Image-Section dieselben Page Faults liefert wie der Kaltstart des Go-Reproducers.
static const unsigned char blob[96u << 20] = {1, 2, 3, 4, 5, 6, 7, 8};

static volatile LONG stop;
static volatile uint64_t counter;
static char bigpath[MAX_PATH];
static uint64_t bigsize;
static LARGE_INTEGER qpf;

static double us_since(LARGE_INTEGER t0) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t0.QuadPart) * 1e6 / (double)qpf.QuadPart;
}

static DWORD WINAPI spin_worker(LPVOID p) {
    uint64_t x = 0;
    (void)p;
    while (!stop) {
        x += 1;
        counter = x;
    }
    return 0;
}

static DWORD WINAPI fault_worker(LPVOID p) {
    uint64_t sum = 0;
    unsigned pass = 0;
    char copypath[MAX_PATH];
    (void)p;
    while (!stop) {
        snprintf(copypath, sizeof copypath, "%s.%u.tmp", bigpath, pass++);
        if (!CopyFileA(bigpath, copypath, FALSE)) {
            Sleep(10);
            continue;
        }
        HANDLE h = CreateFileA(copypath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            HANDLE m = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
            if (m) {
                const unsigned char *v = (const unsigned char *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
                if (v) {
                    for (uint64_t i = 0; i + 8 <= bigsize && !stop; i += 8) {
                        sum += *(const uint64_t *)(v + i);
                        counter = sum;
                    }
                    UnmapViewOfFile(v);
                }
                CloseHandle(m);
            }
            CloseHandle(h);
        }
        DeleteFileA(copypath);
    }
    return (DWORD)sum;
}

static DWORD WINAPI image_worker(LPVOID p) {
    uint64_t sum = 0;
    unsigned pass = 0;
    char self[MAX_PATH], copypath[MAX_PATH], tmp[MAX_PATH];
    (void)p;
    GetModuleFileNameA(NULL, self, sizeof self);
    GetTempPathA(sizeof tmp, tmp);
    uintptr_t rva = (uintptr_t)blob - (uintptr_t)GetModuleHandleA(NULL);
    while (!stop) {
        snprintf(copypath, sizeof copypath, "%ssuspendlag_img_%lu_%u.exe", tmp, (unsigned long)GetCurrentProcessId(), pass++);
        if (!CopyFileA(self, copypath, FALSE)) {
            Sleep(10);
            continue;
        }
        HANDLE h = CreateFileA(copypath, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            HANDLE m = CreateFileMappingA(h, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
            if (m) {
                const unsigned char *v = (const unsigned char *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
                if (v) {
                    const unsigned char *b = v + rva;
                    for (uint64_t i = 0; i + 8 <= sizeof blob && !stop; i += 8) {
                        sum += *(const uint64_t *)(b + i);
                        counter = sum;
                    }
                    UnmapViewOfFile(v);
                }
                CloseHandle(m);
            }
            CloseHandle(h);
        }
        DeleteFileA(copypath);
    }
    return (DWORD)sum;
}

static int make_bigfile(uint64_t size) {
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(bigpath, sizeof bigpath, "%ssuspendlag_%lu.bin", tmp, (unsigned long)GetCurrentProcessId());
    HANDLE h = CreateFileA(bigpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    static unsigned char block[1 << 20];
    for (size_t i = 0; i < sizeof block; i++) block[i] = (unsigned char)(i * 2654435761u >> 24);
    for (uint64_t off = 0; off < size; off += sizeof block) {
        DWORD w;
        block[0] = (unsigned char)(off >> 20);
        if (!WriteFile(h, block, sizeof block, &w, NULL)) { CloseHandle(h); return 0; }
    }
    CloseHandle(h);
    return 1;
}

typedef struct {
    unsigned n, rep, exc, svc, noreport;
    unsigned moved, moved_exc, moved_svc, moved_plain, moved_noreport;
    unsigned lag10, lag100, lag1000, lagmore, still;
    double lagmax_us, readmax_us;
} stats_t;

static int regs_differ(const CONTEXT *a, const CONTEXT *b) {
    return a->Rip != b->Rip || a->Rsp != b->Rsp || a->Rax != b->Rax || a->Rcx != b->Rcx || a->Rdx != b->Rdx ||
           a->Rbx != b->Rbx || a->Rsi != b->Rsi || a->Rdi != b->Rdi || a->R8 != b->R8 || a->R9 != b->R9 ||
           a->R10 != b->R10 || a->R11 != b->R11 || a->R12 != b->R12 || a->R13 != b->R13 || a->R14 != b->R14 ||
           a->R15 != b->R15;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "spin";
    unsigned iters = argc > 2 ? (unsigned)atoi(argv[2]) : 2000;
    unsigned mb = argc > 3 ? (unsigned)atoi(argv[3]) : 256;
    QueryPerformanceFrequency(&qpf);
    bigsize = (uint64_t)mb << 20;

    int fault = strcmp(mode, "fault") == 0, image = strcmp(mode, "image") == 0;
    if (fault && !make_bigfile(bigsize)) {
        printf("SUSPENDLAG FEHLER: Datendatei nicht anlegbar\n");
        return 2;
    }
    HANDLE worker = CreateThread(NULL, 0, image ? image_worker : fault ? fault_worker : spin_worker, NULL, 0, NULL);
    if (!worker) return 2;
    Sleep(fault || image ? 1500 : 100);
    double maxwait = (image || fault) ? 300.0 : 2500.0;

    __attribute__((aligned(16))) CONTEXT c1, c2;
    stats_t s;
    memset(&s, 0, sizeof s);
    srand(GetTickCount());
    for (unsigned it = 0; it < iters; it++) {
        // Zufaelliger Abstand, damit die Stichprobe nicht am 1-ms-Takt haengt.
        LARGE_INTEGER w;
        QueryPerformanceCounter(&w);
        double wait = (double)rand() * maxwait / (double)RAND_MAX;
        while (us_since(w) < wait) {
        }
        if (SuspendThread(worker) == (DWORD)-1) continue;
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        memset(&c1, 0, sizeof c1);
        c1.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_EXCEPTION_REQUEST;
        if (!GetThreadContext(worker, &c1)) { ResumeThread(worker); continue; }
        double read_us = us_since(t0);
        if (read_us > s.readmax_us) s.readmax_us = read_us;
        DWORD f = c1.ContextFlags;
        int rep = (f & CONTEXT_EXCEPTION_REPORTING) != 0, exc = (f & CONTEXT_EXCEPTION_ACTIVE) != 0,
            svc = (f & CONTEXT_SERVICE_ACTIVE) != 0;
        s.n++;
        if (rep) s.rep++;
        if (exc) s.exc++;
        if (svc) s.svc++;
        if (!rep) s.noreport++;

        CONTEXT prev = c1;
        double last_change = 0;
        int still = 0;
        for (;;) {
            memset(&c2, 0, sizeof c2);
            c2.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (!GetThreadContext(worker, &c2)) break;
            double now = us_since(t0);
            if (regs_differ(&prev, &c2)) {
                prev = c2;
                last_change = now;
            } else if (now - last_change > 200.0) {
                break;
            }
            if (now > 5000.0) { still = 1; break; }
        }
        if (last_change > 0) {
            s.moved++;
            if (exc) s.moved_exc++;
            else if (svc) s.moved_svc++;
            else s.moved_plain++;
            if (!rep) s.moved_noreport++;
            if (still) s.still++;
            else if (last_change <= 10) s.lag10++;
            else if (last_change <= 100) s.lag100++;
            else if (last_change <= 1000) s.lag1000++;
            else s.lagmore++;
            if (last_change > s.lagmax_us) s.lagmax_us = last_change;
        }
        ResumeThread(worker);
    }
    InterlockedExchange(&stop, 1);
    WaitForSingleObject(worker, 10000);
    if (fault) DeleteFileA(bigpath);

    printf("SUSPENDLAG mode=%s n=%u reporting=%u excactive=%u svcactive=%u noreport=%u | moved=%u (exc=%u svc=%u plain=%u noreport=%u) "
           "lag<=10us=%u <=100us=%u <=1000us=%u >1000us=%u still@5ms=%u lagmax=%.0fus | readmax=%.0fus\n",
           mode, s.n, s.rep, s.exc, s.svc, s.noreport, s.moved, s.moved_exc, s.moved_svc, s.moved_plain, s.moved_noreport,
           s.lag10, s.lag100, s.lag1000, s.lagmore, s.still, s.lagmax_us, s.readmax_us);
    return 0;
}
