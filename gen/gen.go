//go:build ignore

package main

// Erzeugt blob.bin: jedes 8-Byte-Wort ist eine reine Funktion seines Offsets,
// damit die Verifikation jede Abweichung auf das Byte genau lokalisieren kann.
import (
	"encoding/binary"
	"os"
	"strconv"
)

func Word(off uint64) uint64 { return (off + 1) * 0x9E3779B97F4A7C15 }

func main() {
	mb, _ := strconv.Atoi(os.Args[1])
	n := mb * 1024 * 1024
	buf := make([]byte, n)
	for i := 0; i+8 <= n; i += 8 {
		binary.LittleEndian.PutUint64(buf[i:], Word(uint64(i)))
	}
	if err := os.WriteFile("blob.bin", buf, 0o644); err != nil {
		panic(err)
	}
}
