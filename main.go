package main

// Minimaler Reproducer fuer die Zen-2-Image-Korruption.
//
// Der Blob liegt per go:embed als string in .rdata. Die Verifikation laeuft in init(),
// also in derselben Phase, in der die echten rclone-Crashes passieren, und liest den
// Blob linear durch, was jede Seite genau einmal erstmalig anfasst.
//
// Gelesen wird byteweise direkt aus dem string, damit es echte rodata-Zugriffe sind
// und keine Kopie in den Heap.
//
// Exit 0 = sauber, Exit 1 = Korruption, mit Offset und Ist/Soll.

import (
	_ "embed"
	"fmt"
	"os"
	"strconv"

	"unsafe"

	"zen2repro/prefetch"
)

//go:embed blob.bin
var blob string

func word(off uint64) uint64 { return (off + 1) * 0x9E3779B97F4A7C15 }

var (
	bad         int
	firstReport string
)

func init() {
	n := len(blob)
	// Basis und Laenge vorab melden, damit ein Register-Dump beim Crash gegen
	// den echten Blob-Anfang gehalten werden kann.
	fmt.Fprintf(os.Stderr, "ZEN2REPRO base=%p n=%d\n", unsafe.StringData(blob), n)
	// ZEN2LOOPS wiederholt den Durchlauf. Damit laesst sich die Anzahl der
	// Schleifendurchlaeufe von der Groesse des angefassten Speichers trennen:
	// 8 MB x 12 macht genauso viele Iterationen wie 96 MB x 1, fasst aber nur
	// ein Zwoelftel der Seiten an, und nach dem ersten Durchlauf ist alles warm.
	loops := 1
	if v := os.Getenv("ZEN2LOOPS"); v != "" {
		if k, err := strconv.Atoi(v); err == nil && k > 0 {
			loops = k
		}
	}
	Loops = loops
	for pass := 0; pass < loops; pass++ {
		for i := 0; i+8 <= n; i += 8 {
			got := uint64(blob[i]) | uint64(blob[i+1])<<8 | uint64(blob[i+2])<<16 | uint64(blob[i+3])<<24 |
				uint64(blob[i+4])<<32 | uint64(blob[i+5])<<40 | uint64(blob[i+6])<<48 | uint64(blob[i+7])<<56
			want := word(uint64(i))
			if got != want {
				bad++
				if bad <= 5 {
					firstReport += fmt.Sprintf("KORRUPT off=%d (seite %d) soll=%#016x ist=%#016x\n", i, i/4096, want, got)
				}
			}
		}
	}
}

var Loops int

func main() {
	diagReport()
	if bad > 0 {
		fmt.Printf("ZEN2REPRO FAIL blob=%dMB mit=%s/%v/%d abweichende_woerter=%d\n%s", len(blob)/1048576, prefetch.Mode, prefetch.Done, prefetch.Pages, bad, firstReport)
		os.Exit(1)
	}
	fmt.Printf("ZEN2REPRO OK blob=%dMB mit=%s/%v/%d loops=%d faults=%d\n", len(blob)/1048576, prefetch.Mode, prefetch.Done, prefetch.Pages, Loops, prefetch.Faults())
}
