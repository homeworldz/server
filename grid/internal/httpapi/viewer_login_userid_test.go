package httpapi

import "testing"

// A viewer splits what the person typed on the first space, period or
// underscore and sends the two halves. These are the pairs it produces for the
// spellings someone actually types, and every one of them has to resolve to the
// account's permanent userid.
func TestViewerLoginUserid(t *testing.T) {
	for _, testCase := range []struct {
		name  string
		first string
		last  string
		want  string
	}{
		{"userid as typed", "jim", "tarber", "jim.tarber"},
		{"two words", "Jim", "Tarber", "jim.tarber"},
		{"mixed case", "JIM", "TaRbEr", "jim.tarber"},
		{"underscore separator", "jim", "tarber", "jim.tarber"},
		// The Linden sentinel for a one-word name. Joining it would look for
		// "jim.resident", which is not what anyone is called here.
		{"resident dropped", "jim", "Resident", "jim"},
		{"resident lowercase", "jim", "resident", "jim"},
		{"no last name", "jim", "", "jim"},
		// "Mr. President" splits at the period, so the halves arrive as "Mr"
		// and "President" and have to rejoin to the userid registration
		// derived from that same display name.
		{"display name with a period", "Mr", "President", "mr.president"},
		{"apostrophe survives", "jim", "O'Brien", "jim.o'brien"},
		// Interior spaces and characters outside the userid alphabet used to
		// pass straight through and produce a userid no row can hold.
		{"interior space", "jim", "tarber jr", "jim.tarber.jr"},
		{"stray punctuation", "jim!", "tarber?", "jim.tarber"},
		{"repeated separators collapse", "jim", "..tarber", "jim.tarber"},
		// Nothing usable is not a userid, and must not become one by trimming.
		{"nothing usable", "!!!", "???", ""},
		{"empty", "", "", ""},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			if got := viewerLoginUserid(testCase.first, testCase.last); got != testCase.want {
				t.Fatalf("viewerLoginUserid(%q, %q) = %q, want %q",
					testCase.first, testCase.last, got, testCase.want)
			}
		})
	}
}
