// Package prefetch enthaelt die Gegenmassnahmen-Kandidaten fuer die Zen-2-Image-Korruption.
// Es wird als eigenes Paket importiert, damit sein init() vor dem init() des main-Pakets laeuft.
//
// Steuerung ueber die Umgebungsvariable ZEN2MIT:
//
//	(leer)    nichts tun, Referenzarm
//	prefetch  PrefetchVirtualMemory ueber das ganze Image (widerlegt, nur noch als Vergleich)
//	split<N>  jede N-te Seite der .rdata per VirtualProtect auf RW umstellen, um die
//	          Attribut-Uniformitaet zu brechen, an der das TLB-Page-Coalescing haengt
package prefetch

import (
	"os"
	"strconv"
	"strings"
	"syscall"
	"unsafe"
)

type memoryRangeEntry struct {
	VirtualAddress unsafe.Pointer
	NumberOfBytes  uintptr
}

type moduleInfo struct {
	BaseOfDll   uintptr
	SizeOfImage uint32
	EntryPoint  uintptr
}

const (
	pageReadonly  = 0x02
	pageReadwrite = 0x04
)

var (
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	psapi    = syscall.NewLazyDLL("psapi.dll")

	procPrefetchVirtualMemory = kernel32.NewProc("PrefetchVirtualMemory")
	procGetModuleHandleW      = kernel32.NewProc("GetModuleHandleW")
	procGetCurrentProcess     = kernel32.NewProc("GetCurrentProcess")
	procGetModuleInformation  = psapi.NewProc("GetModuleInformation")
	procVirtualProtect        = kernel32.NewProc("VirtualProtect")
	procGetProcessMemoryInfo  = psapi.NewProc("GetProcessMemoryInfo")
)

type processMemoryCounters struct {
	Cb                         uint32
	PageFaultCount             uint32
	PeakWorkingSetSize         uintptr
	WorkingSetSize             uintptr
	QuotaPeakPagedPoolUsage    uintptr
	QuotaPagedPoolUsage        uintptr
	QuotaPeakNonPagedPoolUsage uintptr
	QuotaNonPagedPoolUsage     uintptr
	PagefileUsage              uintptr
	PeakPagefileUsage          uintptr
}

// Faults liefert die bisherige Page-Fault-Zahl des Prozesses. Damit laesst sich
// pruefen, ob eine Massnahme die Faults wirklich wegnimmt oder nur so tut.
func Faults() uint32 {
	proc, _, _ := procGetCurrentProcess.Call()
	var pmc processMemoryCounters
	pmc.Cb = uint32(unsafe.Sizeof(pmc))
	r, _, _ := procGetProcessMemoryInfo.Call(proc, uintptr(unsafe.Pointer(&pmc)), uintptr(pmc.Cb))
	if r == 0 {
		return 0
	}
	return pmc.PageFaultCount
}

// Mode und Done beschreiben, was tatsaechlich lief, damit ein stiller Fehlschlag
// nicht als "Massnahme wirkt nicht" durchgeht.
var (
	Mode  string
	Done  bool
	Pages int
)

// rdataRange liest die Section-Tabelle des bereits geladenen Images aus dem Speicher
// und liefert Anfang und Laenge der ersten Read-Only-Datensection.
func rdataRange(base uintptr) (uintptr, uintptr) {
	peOff := *(*uint32)(unsafe.Pointer(base + 0x3c))
	nt := base + uintptr(peOff)
	numSections := *(*uint16)(unsafe.Pointer(nt + 6))
	sizeOptionalHeader := *(*uint16)(unsafe.Pointer(nt + 20))
	sectionTable := nt + 24 + uintptr(sizeOptionalHeader)
	for i := uintptr(0); i < uintptr(numSections); i++ {
		h := sectionTable + i*40
		name := (*[8]byte)(unsafe.Pointer(h))
		if strings.TrimRight(string(name[:]), "\x00") != ".rdata" {
			continue
		}
		virtSize := *(*uint32)(unsafe.Pointer(h + 8))
		virtAddr := *(*uint32)(unsafe.Pointer(h + 12))
		return base + uintptr(virtAddr), uintptr(virtSize)
	}
	return 0, 0
}

func init() {
	Mode = os.Getenv("ZEN2MIT")
	if Mode == "" {
		return
	}
	base, _, _ := procGetModuleHandleW.Call(0)
	if base == 0 {
		return
	}
	proc, _, _ := procGetCurrentProcess.Call()

	if Mode == "prefetch" {
		var mi moduleInfo
		r, _, _ := procGetModuleInformation.Call(proc, base, uintptr(unsafe.Pointer(&mi)), unsafe.Sizeof(mi))
		if r == 0 || mi.SizeOfImage == 0 {
			return
		}
		entry := memoryRangeEntry{unsafe.Pointer(mi.BaseOfDll), uintptr(mi.SizeOfImage)}
		ok, _, _ := procPrefetchVirtualMemory.Call(proc, 1, uintptr(unsafe.Pointer(&entry)), 0)
		Done = ok != 0
		return
	}

	if !strings.HasPrefix(Mode, "split") {
		return
	}
	stride, err := strconv.Atoi(strings.TrimPrefix(Mode, "split"))
	if err != nil || stride < 1 {
		return
	}
	start, size := rdataRange(base)
	if start == 0 {
		return
	}
	// Jede stride-te Seite bekommt ein abweichendes Schutzattribut. Damit endet der Lauf
	// gleich-attributierter PTEs alle stride Seiten, das Coalescing kann keine langen
	// Eintraege mehr bilden. Lesbar bleibt alles, geschrieben wird nichts.
	var old uint32
	n := 0
	for off := uintptr(0); off < size; off += uintptr(stride) * 4096 {
		r, _, _ := procVirtualProtect.Call(start+off, 4096, pageReadwrite, uintptr(unsafe.Pointer(&old)))
		if r != 0 {
			n++
		}
	}
	Pages = n
	Done = n > 0
}
