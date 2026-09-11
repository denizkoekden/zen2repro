//go:build zen2diag

package main

// Nur mit dem Runtime-Fork denizkoekden/go, Branch zen2-inpage-diag, bauen
// (go build -tags zen2diag): die Zaehler aus preemptM gibt es in Stock-Go nicht.

import (
	"fmt"
	"os"
	_ "unsafe"
)

// Zaehler aus preemptM: total, reporting, excactive, svcactive, injected, skipped.
//
//go:linkname zen2PreemptStats runtime.zen2PreemptStats
var zen2PreemptStats [16]uint64

func diagReport() {
	s := zen2PreemptStats
	fmt.Fprintf(os.Stderr, "ZEN2REPRO preempt total=%d reporting=%d excactive=%d svcactive=%d injected=%d skipped=%d moved=%d moved_excactive=%d moved_noreport=%d noreport2=%d lagn=%d lag10=%d lag50=%d lag300=%d lagmore=%d lagmax=%d\n",
		s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15])
}
