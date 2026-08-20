package httpapi

import (
	"embed"
	"encoding/json"
	"net/http"
)

// The grid publishes an API catalog for automated discovery (RFC 9727): a
// linkset (RFC 9264) at /.well-known/api-catalog naming each API this
// publisher serves, where its machine-readable description lives, where a
// person reads about it, and where its status can be checked.
//
// The OpenAPI documents are embedded copies of the canonical files in the
// repository's api/ directory — embedded because the deployed artifact is a
// bare binary, copied because go:embed cannot reach outside the module. A
// test compares the copies against the canonical files so they cannot drift
// silently. They are served at root-level paths rather than under /api/,
// which the internal-tier middleware token-gates.

//go:embed apispec/openapi.yaml apispec/openapi-public.yaml
var apiSpecifications embed.FS

// linksetEntry is one context in an RFC 9264 linkset: the API it anchors and
// the typed links that describe it. Relation names are JSON members, target
// attributes ride each link object.
type linksetEntry struct {
	Anchor      string        `json:"anchor"`
	ServiceDesc []linksetLink `json:"service-desc,omitempty"`
	ServiceDoc  []linksetLink `json:"service-doc,omitempty"`
	Status      []linksetLink `json:"status,omitempty"`
}

type linksetLink struct {
	Href string `json:"href"`
	Type string `json:"type,omitempty"`
}

// apiCatalog answers /.well-known/api-catalog. Entries are built from what
// this deployment actually serves and advertises: the grid's own API always,
// the browser-facing API only when its public base is configured — an
// unconfigured deployment advertises nothing rather than a dead link, the
// same rule get_grid_info follows.
func (a *API) apiCatalog(w http.ResponseWriter, r *http.Request) {
	grid := linksetEntry{
		Anchor: a.publicURL + "/",
		ServiceDesc: []linksetLink{
			{Href: a.publicURL + "/openapi.yaml", Type: "application/yaml"}},
		Status: []linksetLink{
			{Href: a.publicURL + "/ready", Type: "application/json"}},
	}
	if a.aboutURL != "" {
		grid.ServiceDoc = []linksetLink{{Href: a.aboutURL, Type: "text/html"}}
	}
	entries := []linksetEntry{grid}
	if a.websiteAPIURL != "" {
		site := linksetEntry{
			Anchor: a.websiteAPIURL + "/v1",
			ServiceDesc: []linksetLink{
				{Href: a.publicURL + "/openapi-public.yaml", Type: "application/yaml"}},
		}
		if a.supportURL != "" {
			site.ServiceDoc = []linksetLink{{Href: a.supportURL, Type: "text/html"}}
		}
		entries = append(entries, site)
	}
	w.Header().Set("Content-Type", "application/linkset+json")
	_ = json.NewEncoder(w).Encode(map[string][]linksetEntry{"linkset": entries})
}

// apiSpecification serves one embedded OpenAPI document.
func (a *API) apiSpecification(name string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		content, err := apiSpecifications.ReadFile("apispec/" + name)
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{
				Code: "specification_unavailable", Message: "the API description could not be read"})
			return
		}
		w.Header().Set("Content-Type", "application/yaml")
		_, _ = w.Write(content)
	}
}
