package httpapi

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/homeworldz/server/grid/internal/assetmeta"
)

const testAvatarID = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"

// newAppearanceHandler registers one asset over content, vaulted, marked as a
// bake or not.
func newAppearanceHandler(content []byte, bake bool) http.Handler {
	sum := sha256.Sum256(content)
	registry := &memoryRegistry{
		blobs: map[string]assetmeta.Blob{
			testAssetID: {BlobID: testBlobID, ByteLength: int64(len(content)),
				Checksum: hex.EncodeToString(sum[:]), ChecksumAlgorithm: "sha256"},
		},
		bakes: map[string]bool{testAssetID: bake},
	}
	store := &memoryVault{registry: registry,
		blobs: map[string][]byte{testBlobID: content}}
	return New(checker{}, "test", Options{ServiceToken: "secret",
		GridPublicURL: "https://grid.example", Assets: registry, Vault: store})
}

// requestAppearance issues the request a viewer makes: no credentials at all.
func requestAppearance(handler http.Handler, path string) *httptest.ResponseRecorder {
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest(http.MethodGet, path, nil))
	return w
}

func TestAppearanceServiceServesABake(t *testing.T) {
	content := []byte("j2c bake bytes")
	handler := newAppearanceHandler(content, true)
	w := requestAppearance(handler, "/appearance/texture/"+testAvatarID+"/head/"+testAssetID)
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", w.Code, w.Body.String())
	}
	if !bytes.Equal(w.Body.Bytes(), content) {
		t.Fatalf("body = %q, want the vaulted bytes", w.Body.Bytes())
	}
	if got := w.Header().Get("Content-Type"); got != "image/x-j2c" {
		t.Fatalf("content type = %q", got)
	}
}

// The route is public, so an asset that is not a bake must be as absent as one
// that does not exist — otherwise it reads any vaulted notecard by uuid.
func TestAppearanceServiceRefusesWhatIsNotABake(t *testing.T) {
	handler := newAppearanceHandler([]byte("a private notecard"), false)
	w := requestAppearance(handler, "/appearance/texture/"+testAvatarID+"/head/"+testAssetID)
	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404: %s", w.Code, w.Body.String())
	}
	if bytes.Contains(w.Body.Bytes(), []byte("notecard")) {
		t.Fatal("the refused asset's bytes were served")
	}
}

func TestAppearanceServiceRejectsMalformedPaths(t *testing.T) {
	handler := newAppearanceHandler([]byte("j2c bake bytes"), true)
	for _, path := range []string{
		"/appearance/texture/" + testAvatarID + "/nose/" + testAssetID,
		"/appearance/texture/not-a-uuid/head/" + testAssetID,
		"/appearance/texture/" + testAvatarID + "/head/not-a-uuid",
		"/appearance/texture/" + testAvatarID + "/head/" + testAssetID + "/extra",
		"/appearance/image/" + testAvatarID + "/head/" + testAssetID,
		"/appearance/",
	} {
		if w := requestAppearance(handler, path); w.Code != http.StatusNotFound {
			t.Errorf("%s status = %d, want 404", path, w.Code)
		}
	}
}

func TestAppearanceServiceIsNotPublishedWithoutAVault(t *testing.T) {
	handler := New(checker{}, "test", Options{ServiceToken: "secret",
		Assets: &memoryAssetStore{assets: map[string]assetmeta.Asset{}}})
	if w := requestAppearance(handler, "/appearance/texture/"+testAvatarID+"/head/"+testAssetID); w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", w.Code)
	}
	a := &API{publicURL: "https://grid.example"}
	if got := a.appearanceServiceURL(); got != "" {
		t.Fatalf("address = %q with no vault, want none", got)
	}
	a.assets, a.vault = &memoryAssetStore{}, &memoryVault{}
	if got := a.appearanceServiceURL(); got != "https://grid.example/appearance/" {
		t.Fatalf("address = %q; a viewer appends texture/... with no separator", got)
	}
}
