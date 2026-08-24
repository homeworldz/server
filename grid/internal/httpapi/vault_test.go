package httpapi

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/vault"
)

const (
	testAssetID   = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
	testBlobID    = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
	absentAssetID = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
)

// memoryRegistry is the blob layer the vault verifies against: the checksum and
// length come from here, never from the caller.
type memoryRegistry struct {
	blobs     map[string]assetmeta.Blob
	locations []assetmeta.Location
}

func (r *memoryRegistry) Register(context.Context, assetmeta.Registration) (assetmeta.Asset, error) {
	return assetmeta.Asset{}, assetmeta.ErrConflict
}

func (r *memoryRegistry) Get(ctx context.Context, assetID string) (assetmeta.Asset, error) {
	blob, err := r.Blob(ctx, assetID)
	if err != nil {
		return assetmeta.Asset{}, err
	}
	return assetmeta.Asset{ID: assetID, SHA256: blob.Checksum, Size: blob.ByteLength,
		Locations: r.locations}, nil
}

func (r *memoryRegistry) Blob(_ context.Context, assetID string) (assetmeta.Blob, error) {
	blob, found := r.blobs[assetID]
	if !found {
		return assetmeta.Blob{}, assetmeta.ErrNotFound
	}
	return blob, nil
}

// memoryVault is an in-memory stand-in that keeps the real store's verification
// and idempotency behavior, so handler tests exercise the same outcomes.
type memoryVault struct {
	registry *memoryRegistry
	blobs    map[string][]byte
}

func (v *memoryVault) registered(blobID string) (assetmeta.Blob, bool) {
	for _, blob := range v.registry.blobs {
		if blob.BlobID == blobID {
			return blob, true
		}
	}
	return assetmeta.Blob{}, false
}

func (v *memoryVault) Ingest(_ context.Context, blobID string, content io.Reader) (vault.Blob, error) {
	registered, found := v.registered(blobID)
	if !found {
		return vault.Blob{}, vault.ErrInvalid
	}
	body, err := io.ReadAll(content)
	if err != nil {
		return vault.Blob{}, err
	}
	sum := sha256.Sum256(body)
	if int64(len(body)) != registered.ByteLength || hex.EncodeToString(sum[:]) != registered.Checksum {
		return vault.Blob{}, vault.ErrMismatch
	}
	v.blobs[blobID] = body
	return vault.Blob{BlobID: blobID, ByteLength: registered.ByteLength,
		Checksum: registered.Checksum, IngestedAt: time.Unix(1, 0).UTC()}, nil
}

func (v *memoryVault) Held(_ context.Context, blobID string) (vault.Blob, error) {
	body, found := v.blobs[blobID]
	if !found {
		return vault.Blob{}, vault.ErrNotFound
	}
	registered, _ := v.registered(blobID)
	return vault.Blob{BlobID: blobID, ByteLength: int64(len(body)),
		Checksum: registered.Checksum, IngestedAt: time.Unix(1, 0).UTC()}, nil
}

// HeldAssets answers the bulk question by asking the single one per asset,
// which is what the real store does modulo one round trip.
func (v *memoryVault) HeldAssets(ctx context.Context, assetIDs []string) ([]string, error) {
	held := make([]string, 0, len(assetIDs))
	for _, assetID := range assetIDs {
		blob, err := v.registry.Blob(ctx, assetID)
		if err != nil {
			continue
		}
		if _, err := v.Held(ctx, blob.BlobID); err == nil {
			held = append(held, assetID)
		}
	}
	return held, nil
}

func (v *memoryVault) Open(ctx context.Context, blobID string) (io.ReadCloser, vault.Blob, error) {
	blob, err := v.Held(ctx, blobID)
	if err != nil {
		return nil, vault.Blob{}, err
	}
	return io.NopCloser(bytes.NewReader(v.blobs[blobID])), blob, nil
}

// newVaultHandler wires a registry holding one asset over content, and an empty
// vault, which is the state every asset starts in.
func newVaultHandler(content []byte) (http.Handler, *memoryVault) {
	sum := sha256.Sum256(content)
	registry := &memoryRegistry{blobs: map[string]assetmeta.Blob{
		testAssetID: {BlobID: testBlobID, ByteLength: int64(len(content)),
			Checksum: hex.EncodeToString(sum[:]), ChecksumAlgorithm: "sha256"},
	}}
	store := &memoryVault{registry: registry, blobs: make(map[string][]byte)}
	return New(checker{}, "test", Options{ServiceToken: "secret",
		Assets: registry, Vault: store}), store
}

// requestVault issues a service-authenticated request with a raw (non-JSON) body,
// which the vault endpoints take, and returns the recorder for header and byte
// assertions.
func requestVault(t *testing.T, handler http.Handler, method, path string,
	body []byte, wantStatus int) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	r.Header.Set("Authorization", "Bearer secret")
	if len(body) > 0 {
		r.Header.Set("Content-Type", "application/octet-stream")
	}
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	if w.Code != wantStatus {
		t.Fatalf("%s %s status = %d, want %d: %s", method, path, w.Code, wantStatus, w.Body.String())
	}
	return w
}

func TestVaultAssetLifecycle(t *testing.T) {
	content := []byte("durable inventory-referenced bytes")
	handler, _ := newVaultHandler(content)
	path := "/api/v1/vault/assets/" + testAssetID

	// Registered but not yet ingested: the vault holds nothing, which is the
	// answer the inventory-commit invariant acts on.
	requestVault(t, handler, http.MethodHead, path, nil, http.StatusNotFound)

	recorder := requestVault(t, handler, http.MethodPut, path, content, http.StatusOK)
	var ingested vault.Blob
	if err := json.NewDecoder(recorder.Body).Decode(&ingested); err != nil {
		t.Fatalf("decode ingest response: %v", err)
	}
	if ingested.BlobID != testBlobID || ingested.ByteLength != int64(len(content)) {
		t.Fatalf("ingested blob = %#v", ingested)
	}

	// Re-ingesting the same bytes succeeds; the vault reports what it holds
	// rather than treating a repeat as a conflict.
	requestVault(t, handler, http.MethodPut, path, content, http.StatusOK)

	head := requestVault(t, handler, http.MethodHead, path, nil, http.StatusOK)
	declared := strconv.Itoa(len(content))
	if got := head.Header().Get("Content-Length"); got != declared {
		t.Fatalf("HEAD Content-Length = %q, want %s", got, declared)
	}

	get := requestVault(t, handler, http.MethodGet, path, nil, http.StatusOK)
	if !bytes.Equal(get.Body.Bytes(), content) {
		t.Fatalf("GET body = %q", get.Body.Bytes())
	}
	if got := get.Header().Get("Content-Type"); got != "application/octet-stream" {
		t.Fatalf("GET Content-Type = %q", got)
	}
}

func TestVaultAssetRejectsMismatchedBytes(t *testing.T) {
	handler, _ := newVaultHandler([]byte("what the registry says the asset is"))
	path := "/api/v1/vault/assets/" + testAssetID

	recorder := requestVault(t, handler, http.MethodPut, path,
		[]byte("what the caller actually sent"), http.StatusBadRequest)
	var failure Error
	if err := json.NewDecoder(recorder.Body).Decode(&failure); err != nil {
		t.Fatalf("decode mismatch response: %v", err)
	}
	if failure.Code != "vault_blob_mismatch" {
		t.Fatalf("mismatch error = %#v", failure)
	}
	// Fail closed: the rejected blob must not become readable.
	requestVault(t, handler, http.MethodHead, path, nil, http.StatusNotFound)
}

func TestVaultAssetMissingAndInvalid(t *testing.T) {
	handler, _ := newVaultHandler([]byte("registered content"))

	recorder := requestVault(t, handler, http.MethodGet,
		"/api/v1/vault/assets/"+testAssetID, nil, http.StatusNotFound)
	var failure Error
	if err := json.NewDecoder(recorder.Body).Decode(&failure); err != nil {
		t.Fatalf("decode missing response: %v", err)
	}
	if failure.Code != "vault_blob_not_found" {
		t.Fatalf("missing error = %#v", failure)
	}

	// An unregistered asset has no blob to vouch for, which is a different
	// answer from "the vault does not hold it yet".
	recorder = requestVault(t, handler, http.MethodGet,
		"/api/v1/vault/assets/"+absentAssetID, nil, http.StatusNotFound)
	if err := json.NewDecoder(recorder.Body).Decode(&failure); err != nil {
		t.Fatalf("decode unregistered response: %v", err)
	}
	if failure.Code != "asset_not_found" {
		t.Fatalf("unregistered error = %#v", failure)
	}

	// A path that is not an asset UUID is not a vault route at all.
	requestVault(t, handler, http.MethodGet, "/api/v1/vault/assets/not-a-uuid", nil, http.StatusNotFound)

	recorder = requestVault(t, handler, http.MethodDelete,
		"/api/v1/vault/assets/"+testAssetID, nil, http.StatusMethodNotAllowed)
	if got := recorder.Header().Get("Allow"); got != "GET, HEAD, PUT" {
		t.Fatalf("Allow = %q", got)
	}
}

func TestVaultAssetUnavailableWithoutStore(t *testing.T) {
	handler := New(checker{}, "test", Options{ServiceToken: "secret"})
	recorder := requestVault(t, handler, http.MethodGet,
		"/api/v1/vault/assets/"+testAssetID, nil, http.StatusServiceUnavailable)
	var failure Error
	if err := json.NewDecoder(recorder.Body).Decode(&failure); err != nil {
		t.Fatalf("decode unavailable response: %v", err)
	}
	if failure.Code != "vault_unavailable" {
		t.Fatalf("unavailable error = %#v", failure)
	}
}

func TestVaultAssetRequiresServiceToken(t *testing.T) {
	handler, _ := newVaultHandler([]byte("unauthenticated"))
	r := httptest.NewRequest(http.MethodGet, "/api/v1/vault/assets/"+testAssetID, nil)
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	// The vault stays behind the internal boundary, which is what keeps it out of
	// the viewer data path.
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d, want %d", w.Code, http.StatusUnauthorized)
	}
}

// TestVaultMissingAssets covers the bulk question a region asks at startup.
// The answer is what decides whether bytes are written, so the direction that
// matters is the false negative: an asset the vault does not hold must never be
// reported as held, or the region skips a write and the durability invariant
// quietly loses a blob.
func TestVaultMissingAssets(t *testing.T) {
	content := []byte("bundled asset bytes")
	handler, _ := newVaultHandler(content)
	const strangerID = "77777777-7777-4777-8777-777777777777"

	ask := func(ids ...string) []string {
		t.Helper()
		body, err := json.Marshal(VaultAssetQuery{AssetIDs: ids})
		if err != nil {
			t.Fatal(err)
		}
		request := httptest.NewRequest(http.MethodPost, "/api/v1/vault/assets/missing",
			bytes.NewReader(body))
		request.Header.Set("Authorization", "Bearer secret")
		request.Header.Set("Content-Type", "application/json")
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != http.StatusOK {
			t.Fatalf("status = %d: %s", response.Code, response.Body.String())
		}
		var answer VaultMissingAssets
		if err := json.Unmarshal(response.Body.Bytes(), &answer); err != nil {
			t.Fatal(err)
		}
		return answer.Missing
	}

	// Before the write, the asset is missing — and so is one this grid has
	// never registered, because "write it again" is the safe direction.
	if missing := ask(testAssetID, strangerID); len(missing) != 2 {
		t.Fatalf("missing before ingest = %v, want both", missing)
	}

	requestVault(t, handler, http.MethodPut, "/api/v1/vault/assets/"+testAssetID,
		content, http.StatusOK)

	missing := ask(testAssetID, strangerID)
	if len(missing) != 1 || missing[0] != strangerID {
		t.Fatalf("missing after ingest = %v, want only the stranger", missing)
	}

	// An empty or oversized question is refused rather than answered with a
	// list that means nothing.
	for _, ids := range [][]string{{}, make([]string, maxVaultQuery+1)} {
		body, err := json.Marshal(VaultAssetQuery{AssetIDs: ids})
		if err != nil {
			t.Fatal(err)
		}
		request := httptest.NewRequest(http.MethodPost, "/api/v1/vault/assets/missing",
			bytes.NewReader(body))
		request.Header.Set("Authorization", "Bearer secret")
		request.Header.Set("Content-Type", "application/json")
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != http.StatusBadRequest {
			t.Fatalf("a %d-id question answered %d", len(ids), response.Code)
		}
	}
}
