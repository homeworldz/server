package httpapi

import (
	"bytes"
	"encoding/xml"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

func TestViewerGridInfo(t *testing.T) {
	handler := New(nil, "test", Options{GridPublicURL: "http://grid.example:8002/", GridName: "Homeworldz Local",
		GridNick:    "homeworldz",
		AboutURL:    "https://example.com/",
		SupportURL:  "https://example.com/faq",
		RegisterURL: "https://accounts.example.com/register",
		PasswordURL: "https://accounts.example.com/forgot"})
	request := httptest.NewRequest(http.MethodGet, "/get_grid_info", nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "application/xml; charset=utf-8" {
		t.Fatalf("status = %d, content type = %q", response.Code, response.Header().Get("Content-Type"))
	}
	var info viewerGridInfo
	if err := xml.Unmarshal(response.Body.Bytes(), &info); err != nil {
		t.Fatalf("decode grid info: %v", err)
	}
	if info.GridNick != "homeworldz" || info.GridName != "Homeworldz Local" || info.Platform != "OpenSim" ||
		info.Login != "http://grid.example:8002/login" || info.Welcome != "http://grid.example:8002/welcome" ||
		info.Helper != "http://grid.example:8002/" {
		t.Fatalf("unexpected grid info: %#v", info)
	}
	// The human-facing pages come from configuration, not from the grid's own
	// public URL: the site and the account site are separate deployments, and
	// deriving them here would have published grid.example links for both.
	// An unconfigured nick must not be the public grid's. A viewer keys its
	// saved grid entries on the nick, so a default of "homeworldz" made every
	// install — a developer's localhost included — indistinguishable from
	// grid.homeworldz.com in the grid manager (found 2026-08-28 with both in
	// one viewer's list under one nick).
	{
		bare := New(nil, "test", Options{GridPublicURL: "http://localhost:42100/"})
		bareRequest := httptest.NewRequest(http.MethodGet, "/get_grid_info", nil)
		bareResponse := httptest.NewRecorder()
		bare.ServeHTTP(bareResponse, bareRequest)
		var bareInfo viewerGridInfo
		if err := xml.Unmarshal(bareResponse.Body.Bytes(), &bareInfo); err != nil {
			t.Fatalf("decode default grid info: %v", err)
		}
		if bareInfo.GridNick == "homeworldz" {
			t.Fatal("an unconfigured grid claims the public grid's nick")
		}
		if bareInfo.GridNick != "homeworldz-local" {
			t.Fatalf("default nick = %q, want homeworldz-local", bareInfo.GridNick)
		}
	}
	if info.About != "https://example.com/" || info.Help != "https://example.com/faq" ||
		info.Register != "https://accounts.example.com/register" ||
		info.Password != "https://accounts.example.com/forgot" {
		t.Fatalf("unexpected grid info pages: %#v", info)
	}
	// login is the XML-RPC endpoint, never the website's sign-in page.
	if strings.Contains(info.Login, "accounts.example.com") {
		t.Fatalf("login must stay the grid's own endpoint: %q", info.Login)
	}
}

// An unconfigured grid publishes no human-facing pages at all. Emitting empty
// elements would leave a viewer's grid manager showing links that go nowhere,
// which reads as configured-and-broken rather than not configured.
func TestViewerGridInfoOmitsUnconfiguredPages(t *testing.T) {
	handler := New(nil, "test", Options{GridPublicURL: "http://grid.example:8002/", GridName: "Homeworldz Local"})
	request := httptest.NewRequest(http.MethodGet, "/get_grid_info", nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	body := response.Body.String()
	for _, element := range []string{"<about>", "<help>", "<register>", "<password>"} {
		if strings.Contains(body, element) {
			t.Fatalf("unconfigured grid published %s: %s", element, body)
		}
	}
	// Search and message are never published, configured or not.
	for _, element := range []string{"<search>", "<message>"} {
		if strings.Contains(body, element) {
			t.Fatalf("published unserved %s: %s", element, body)
		}
	}
}

func TestViewerWelcomePage(t *testing.T) {
	request := httptest.NewRequest(http.MethodGet, "/welcome", nil)
	response := httptest.NewRecorder()
	New(nil, "test", Options{GridName: "Homeworldz Local"}).ServeHTTP(response, request)
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "text/html; charset=utf-8" ||
		!strings.Contains(response.Body.String(), `src="data:image/svg+xml;base64,`) ||
		!strings.Contains(response.Body.String(), "Welcome to Homeworldz Local.") {
		t.Fatalf("unexpected welcome response: %d %q", response.Code, response.Body.String())
	}
}

func TestViewerLoginGETShowsWelcomePage(t *testing.T) {
	request := httptest.NewRequest(http.MethodGet, "/login", nil)
	response := httptest.NewRecorder()
	New(nil, "test", Options{}).ServeHTTP(response, request)
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "text/html; charset=utf-8" ||
		!strings.Contains(response.Body.String(), `src="data:image/svg+xml;base64,`) {
		t.Fatalf("unexpected login page response: %d %q", response.Code, response.Body.String())
	}
}

func TestViewerLogo(t *testing.T) {
	request := httptest.NewRequest(http.MethodGet, "/assets/homeworldz.svg", nil)
	response := httptest.NewRecorder()
	New(nil, "test", Options{}).ServeHTTP(response, request)
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "image/svg+xml; charset=utf-8" ||
		!strings.Contains(response.Body.String(), "<svg") {
		t.Fatalf("unexpected logo response: %d %q", response.Code, response.Body.String())
	}
}

func TestEmbeddedViewerLogoMatchesProjectLogo(t *testing.T) {
	projectLogo, err := os.ReadFile("../../../homeworldz.svg")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(homeworldzLogo, projectLogo) {
		t.Fatal("embedded viewer logo differs from the project logo")
	}
}
