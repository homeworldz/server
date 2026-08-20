package httpapi

import (
	"bytes"
	"encoding/json"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

func TestAPICatalog(t *testing.T) {
	handler := New(checker{}, "test", Options{
		GridPublicURL: "https://grid.example.com",
		WebsiteAPIURL: "https://api.example.com",
		AboutURL:      "https://example.com/",
		SupportURL:    "https://example.com/faq",
	})
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest("GET", "/.well-known/api-catalog", nil))
	if response.Code != 200 {
		t.Fatalf("catalog status = %d: %s", response.Code, response.Body.String())
	}
	if kind := response.Header().Get("Content-Type"); kind != "application/linkset+json" {
		t.Fatalf("catalog content type = %q", kind)
	}
	var catalog struct {
		Linkset []struct {
			Anchor      string `json:"anchor"`
			ServiceDesc []struct {
				Href string `json:"href"`
				Type string `json:"type"`
			} `json:"service-desc"`
			ServiceDoc []struct {
				Href string `json:"href"`
			} `json:"service-doc"`
			Status []struct {
				Href string `json:"href"`
			} `json:"status"`
		} `json:"linkset"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &catalog); err != nil {
		t.Fatalf("decode catalog: %v\n%s", err, response.Body.String())
	}
	if len(catalog.Linkset) != 2 {
		t.Fatalf("linkset entries = %d, want 2:\n%s", len(catalog.Linkset), response.Body.String())
	}
	grid := catalog.Linkset[0]
	if grid.Anchor != "https://grid.example.com/" ||
		len(grid.ServiceDesc) != 1 || grid.ServiceDesc[0].Href != "https://grid.example.com/openapi.yaml" ||
		grid.ServiceDesc[0].Type != "application/yaml" ||
		len(grid.ServiceDoc) != 1 || grid.ServiceDoc[0].Href != "https://example.com/" ||
		len(grid.Status) != 1 || grid.Status[0].Href != "https://grid.example.com/ready" {
		t.Fatalf("grid entry = %#v", grid)
	}
	site := catalog.Linkset[1]
	if site.Anchor != "https://api.example.com/v1" ||
		len(site.ServiceDesc) != 1 || site.ServiceDesc[0].Href != "https://grid.example.com/openapi-public.yaml" {
		t.Fatalf("website entry = %#v", site)
	}

	// Every advertised description must actually answer.
	for _, path := range []string{"/openapi.yaml", "/openapi-public.yaml"} {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest("GET", path, nil))
		if response.Code != 200 ||
			!strings.HasPrefix(response.Body.String(), "openapi:") {
			t.Fatalf("%s answered %d: %.60s", path, response.Code, response.Body.String())
		}
		if kind := response.Header().Get("Content-Type"); kind != "application/yaml" {
			t.Fatalf("%s content type = %q", path, kind)
		}
	}
}

// An unconfigured website API is omitted from the catalog rather than
// advertised as a dead anchor.
func TestAPICatalogOmitsUnconfiguredWebsiteAPI(t *testing.T) {
	handler := New(checker{}, "test", Options{GridPublicURL: "https://grid.example.com"})
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest("GET", "/.well-known/api-catalog", nil))
	var catalog struct {
		Linkset []json.RawMessage `json:"linkset"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &catalog); err != nil {
		t.Fatal(err)
	}
	if len(catalog.Linkset) != 1 {
		t.Fatalf("linkset entries = %d, want the grid alone:\n%s",
			len(catalog.Linkset), response.Body.String())
	}
}

// The embedded specifications are copies (go:embed cannot reach outside the
// module); this is what keeps them identical to the canonical files in the
// repository's api/ directory.
func TestEmbeddedSpecificationsMatchCanonical(t *testing.T) {
	for embedded, canonical := range map[string]string{
		"apispec/openapi.yaml":        "../../../api/openapi.yaml",
		"apispec/openapi-public.yaml": "../../../api/openapi-public.yaml",
	} {
		copied, err := apiSpecifications.ReadFile(embedded)
		if err != nil {
			t.Fatal(err)
		}
		source, err := os.ReadFile(canonical)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(copied, source) {
			t.Fatalf("%s has drifted from %s: re-copy the canonical file", embedded, canonical)
		}
	}
}
