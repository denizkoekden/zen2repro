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
var zen2PreemptStats [6]uint64

func diagReport() {
	s := zen2PreemptStats
	fmt.Fprintf(os.Stderr, "ZEN2REPRO preempt total=%d reporting=%d excactive=%d svcactive=%d injected=%d skipped=%d\n",
		s[0], s[1], s[2], s[3], s[4], s[5])
}
