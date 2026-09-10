# zen2repro

Minimaler Reproducer fuer sporadische Startup-Crashes von Go-Binaries auf AMD Zen 2 unter Windows
(siehe <https://github.com/golang/go/issues/79249>).

## Was es zeigt

Ein Go-Programm mit konstant 0,6 MB `.text` und einem per `go:embed` in `.rdata` gelegten Blob variabler Groesse.
`init()` verifiziert den Blob linear, jedes 8-Byte-Wort ist eine reine Funktion seines Offsets. Damit ist die
Code-Groesse festgehalten und nur der zusammenhaengende Read-Only-Bereich variiert.

Gemessen am 2026-09-10, 40 Kaltstarts je Groesse und Host (jeder Start eine frische Kopie unter neuem Pfad, damit
das Image-Section-Mapping neu aufgebaut wird):

| Blob (`.rdata`-Seiten) | EPYC 7302 (Zen 2) | EPYC 7532 (Zen 2) | EPYC 7551P (Zen 1) | Xeon E5-2640 v4 |
| --- | ---: | ---: | ---: | ---: |
| 8 MB (2.270) | 0/40 | 0/40 | 0/40 | 0/40 |
| 24 MB (6.400) | 3/40 | 5/40 | 0/40 | 0/40 |
| 48 MB (12.600) | 7/40 | 2/40 | 0/40 | 0/40 |
| 96 MB (24.798) | 14/40 | 12/40 | 0/40 | 0/40 |

Alle Hosts: Windows Server 2016, Go 1.26.8, identische Binaries. Zen 2 ist `AMD64 Family 23 Model 49 Stepping 0`,
dieselbe CPUID wie der Threadripper 3970X im Go-Issue.

Der Fehler ist praktisch immer ein harter Fault, ganz ueberwiegend `unexpected fault address 0xffffffffffffffff`,
nie eine still verfaelschte Byte-Folge. (Aufgeklaert im Abschnitt "Ergebnis": es ist keine falsche
Adressuebersetzung, sondern ein zweites Mal ausgefuehrter Load mit veraltetem RIP.)

## Bauen

```sh
go run gen/gen.go 96          # erzeugt blob.bin mit 96 MB
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build --ldflags "-s" -trimpath -o zen2repro-96mb.exe .
```

Mit Stock-Go baut das ohne weiteres. Die Ausgabe der `preemptM`-Zaehler (Abschnitt "Register-Dump") braucht den
Runtime-Fork und das Build-Tag `zen2diag`:

```sh
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 <fork>/bin/go build -tags zen2diag -o zen2repro-96mb-diag.exe .
```

`peinspect.go.txt` ist ein kleiner PE-Section-Dumper (stdlib `debug/pe`), mit dem sich pruefen laesst, dass der Blob
wirklich in `.rdata` gelandet ist und wie lang der zusammenhaengende Read-Only-Lauf ist.

## Messen

Wichtig: warme Wiederholungen reproduzieren **nicht**. Es braucht einen
Kaltstart, also pro Durchgang eine frische Kopie des Binaries unter neuem
Pfad. Die Dateidaten liegen dabei im Cache, gelesen wird also aus RAM. Es ist
nicht der Plattenzugriff, sondern das frisch aufgebaute Image-Section-Mapping.

## Ergebnis (2026-09-10)

Gos asynchrone Preemption (`runtime.preemptM` in `os_windows.go`: `SuspendThread`, `GetThreadContext`,
`PushCall(asyncPreempt, pc)`, `SetThreadContext`, `ResumeThread`) trifft den Main-Thread, waehrend er im harten Page
Fault steht. Der gelesene Kontext ist der Trap-Frame des Faults; der Registerstand beim Crash entspricht einer
Injektion, die erst bei einem spaeteren Kernel-Eintritt wirksam wird: `asyncPreempt` laeuft mit den dann aktuellen
Registern und kehrt per RET auf den alten PC zurueck (sparsamste Erklaerung, die Kernelseite ist nicht direkt
beobachtet). Der Befehl am Fault-PC laeuft ein zweites Mal.

### Register-Dump

Toolchain aus dem Fork `denizkoekden/go`, Branch `zen2-inpage-diag` (Commits acae25be13, ceb6cdb6c6, 5b2fa0e342):
die Runtime druckt beim fatalen Fault Exception-Art, PC und alle 16 GPRs plus die `preemptM`-Zaehler; `main.go` meldet
vorab `ZEN2REPRO base=<Blob-Basis> n=<Laenge>` und nach `init` die Zaehler (`//go:linkname` auf
`runtime.zen2PreemptStats`). Der Compiler verschmilzt die acht Byte-Loads zu `MOVQ 0(SI)(DX*1), SI` (Basis SI, eine
Instruktion vorher aus `main.blob` geladen, Index DX, Ergebnis wieder in SI); `go tool objdump -s main.init.0` zeigt
die Adresse.

22 Crashes (game090, game049): PC immer auf diesem Load, RCX = len, RBX = i+8, RDX = i, und SI = `word(i)`, also das
Ergebnis genau dieses Loads (16x mit RDI = i+7, 3x am `CMPQ SI, DI` danach), oder SI = len aus dem
Bounds-Check-Bereich (3x, Fault-Adresse = len + i; ein aelterer Crash ohne Registerdump passt rechnerisch dazu). Die
Blob-Basis liegt bei Seitenoffset 0xc02, der erste Load auf eine neue Seite ist der bei i = 0x3f8 mod 4096; drei
Samples sind exakt dieser Load, die uebrigen 3 bis 80 Iterationen dahinter.

### A/B (frische Kopie pro Lauf, interleaved, 40 Runden je Arm)

| Was | Host | normal | geaendert |
| --- | --- | ---: | ---: |
| Reproducer, `GODEBUG=asyncpreemptoff=1` | game060 / game067 | 14/40 / 7/40 | 0/40 / 0/40 |
| Reproducer, Diag-Runtime, `ZEN2SKIP=1` | game060 / game067 | 12/40 / 12/40 | 0/40 / 0/40 |
| rclone 1.75.1 `rclone version`, `GODEBUG=asyncpreemptoff=1` | game090 / game049 | 4/40 / 0/40 | 0/40 / 0/40 |
| rclone installiert gegen 1.75.1 mit Go-Fix, ohne GODEBUG | game060 / game067 | 2/40 / 1/40 | 0/40 / 0/40 |

Installiert waren rclone 1.73.3 (game060) und 1.71.2 (game067). Im `ZEN2SKIP`-Arm (keine Injektion bei
`CONTEXT_EXCEPTION_ACTIVE`) meldete der Kernel den Thread in rund einem Drittel aller Kontext-Reads als
`EXCEPTION_ACTIVE`; die 120 Injektionen je 40 Laeufe in Threads ausserhalb einer Exception blieben folgenlos.

### Fix

Branch `zen2-preempt-fix` (Commit 22f597afb4, Basis e51216de8e): `CONTEXT_EXCEPTION_REQUEST` beim `GetThreadContext`
setzen und bei `CONTEXT_EXCEPTION_ACTIVE` nicht injizieren, sysmon versucht es beim naechsten Tick. Cross-Builds fuer
windows/amd64, arm64 und 386 laufen. Workaround ohne Rebuild: `GODEBUG=asyncpreemptoff=1`.

### Schalter

- `ZEN2LOOPS=<n>`: Durchlaeufe ueber den Blob.
- `ZEN2MIT=prefetch|split<N>`: die widerlegten Mitigationen (PrefetchVirtualMemory, VirtualProtect-Striping).
- `ZEN2SKIP=1`: nur mit der Diag-Runtime, keine Injektion bei `EXCEPTION_ACTIVE`.
- `GODEBUG=asyncpreemptoff=1`: Standard-Go, schaltet die asynchrone Preemption ab.
