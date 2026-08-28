package config

import "testing"

// TestGridIdentityDefaultsFromTheGridsOwnDomain covers the rule that replaced a
// hardcoded nick.
//
// The nick was "homeworldz" for every build, so a developer's localhost
// published the same identity as grid.homeworldz.com and both sat in one
// viewer's grid manager under one nick (2026-08-28). A viewer keys its saved
// grid entries on the nick, so two grids answering to one are two grids it
// cannot tell apart. No single constant can be right for both, which is why the
// answer comes from the grid's own public URL.
func TestGridIdentityDefaultsFromTheGridsOwnDomain(t *testing.T) {
	for _, testCase := range []struct {
		publicURL string
		nick      string
		name      string
	}{
		// The real grid and anything under it.
		{"https://grid.homeworldz.com", "homeworldz", "Homeworldz"},
		{"https://homeworldz.com", "homeworldz", "Homeworldz"},
		{"https://beta.grid.homeworldz.com/", "homeworldz", "Homeworldz"},
		// Case and port must not change the answer.
		{"https://GRID.HOMEWORLDZ.COM:8002", "homeworldz", "Homeworldz"},
		// Everything else, including the default local address.
		{"http://127.0.0.1:42000", "homeworldz-local", "Homeworldz Local"},
		{"http://localhost:42100", "homeworldz-local", "Homeworldz Local"},
		// A look-alike ends with the string and not with the domain. Getting
		// this wrong would let someone else's host publish our identity.
		{"https://homeworldz.com.example.net", "homeworldz-local", "Homeworldz Local"},
		{"https://nothomeworldz.com", "homeworldz-local", "Homeworldz Local"},
		// Unparseable or absent is not the official grid.
		{"", "homeworldz-local", "Homeworldz Local"},
		{"://broken", "homeworldz-local", "Homeworldz Local"},
	} {
		nick, name := GridIdentity(testCase.publicURL, "", "")
		if nick != testCase.nick || name != testCase.name {
			t.Errorf("GridIdentity(%q) = %q/%q, want %q/%q",
				testCase.publicURL, nick, name, testCase.nick, testCase.name)
		}
	}
}

// An operator's own values win everywhere, including on the official domain —
// the derivation fills gaps, it does not overrule anybody.
func TestGridIdentityKeepsWhatWasConfigured(t *testing.T) {
	nick, name := GridIdentity("https://grid.homeworldz.com", "mine", "Mine")
	if nick != "mine" || name != "Mine" {
		t.Fatalf("configured values were overridden: %q/%q", nick, name)
	}
	// Each is filled independently: setting one must not force the other.
	if nick, name = GridIdentity("http://localhost:9000", "", "My Grid"); nick != "homeworldz-local" || name != "My Grid" {
		t.Fatalf("half-configured = %q/%q", nick, name)
	}
	if nick, name = GridIdentity("http://localhost:9000", "mine", ""); nick != "mine" || name != "Homeworldz Local" {
		t.Fatalf("half-configured = %q/%q", nick, name)
	}
}
