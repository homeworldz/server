package durability

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/assetrefs"
	"github.com/homeworldz/server/grid/internal/vault"
)

// fixture is a grid in miniature: a registry of assets, a vault, and one
// region-shaped HTTP server holding whichever assets still have bytes.
type fixture struct {
	registry *stubRegistry
	vault    *stubVault
	region   *httptest.Server
	served   map[string][]byte
	fetches  map[string]int
}

type asset struct {
	id      string
	content []byte
	// gone marks an asset the registry records but no location serves — the
	// pre-vault loss state.
	gone bool
	// unregistered marks bytes referenced by something but never registered:
	// an external reference (stock textures, cross-grid content).
	unregistered bool
}

type stubRegistry struct {
	blobs     map[string]assetmeta.Blob // assetID -> blob
	locations map[string][]assetmeta.Location
}

func (r *stubRegistry) Get(_ context.Context, id string) (assetmeta.Asset, error) {
	blob, found := r.blobs[id]
	if !found {
		return assetmeta.Asset{}, assetmeta.ErrNotFound
	}
	return assetmeta.Asset{ID: id, SHA256: blob.Checksum, Size: blob.ByteLength,
		Locations: r.locations[id]}, nil
}

func (r *stubRegistry) Blob(_ context.Context, id string) (assetmeta.Blob, error) {
	blob, found := r.blobs[id]
	if !found {
		return assetmeta.Blob{}, assetmeta.ErrNotFound
	}
	return blob, nil
}

type stubVault struct {
	registry *stubRegistry
	held     map[string][]byte // blobID -> bytes
	fails    bool
}

func (v *stubVault) registered(blobID string) (assetmeta.Blob, bool) {
	for _, blob := range v.registry.blobs {
		if blob.BlobID == blobID {
			return blob, true
		}
	}
	return assetmeta.Blob{}, false
}

func (v *stubVault) Ingest(_ context.Context, blobID string, content io.Reader) (vault.Blob, error) {
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
	v.held[blobID] = body
	return vault.Blob{BlobID: blobID, ByteLength: registered.ByteLength,
		Checksum: registered.Checksum}, nil
}

// HeldAssets is not on the durability path; the keeper asks about one blob at
// a time. Present so the double still satisfies the store.
func (v *stubVault) HeldAssets(context.Context, []string) ([]string, error) { return nil, nil }

func (v *stubVault) Held(_ context.Context, blobID string) (vault.Blob, error) {
	if v.fails {
		return vault.Blob{}, errors.New("vault index unreachable")
	}
	body, found := v.held[blobID]
	if !found {
		return vault.Blob{}, vault.ErrNotFound
	}
	registered, _ := v.registered(blobID)
	return vault.Blob{BlobID: blobID, ByteLength: int64(len(body)),
		Checksum: registered.Checksum}, nil
}

func (v *stubVault) Open(ctx context.Context, blobID string) (io.ReadCloser, vault.Blob, error) {
	blob, err := v.Held(ctx, blobID)
	if err != nil {
		return nil, vault.Blob{}, err
	}
	return io.NopCloser(bytes.NewReader(v.held[blobID])), blob, nil
}

func newFixture(t *testing.T, assets ...asset) (*Keeper, *fixture) {
	t.Helper()
	f := &fixture{
		registry: &stubRegistry{blobs: make(map[string]assetmeta.Blob),
			locations: make(map[string][]assetmeta.Location)},
		served:  make(map[string][]byte),
		fetches: make(map[string]int),
	}
	f.vault = &stubVault{registry: f.registry, held: make(map[string][]byte)}
	f.region = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := strings.TrimPrefix(r.URL.Path, "/api/v1/assets/")
		body, found := f.served[id]
		if !found {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		f.fetches[id]++
		w.Write(body)
	}))
	t.Cleanup(f.region.Close)
	for _, item := range assets {
		if item.unregistered {
			continue
		}
		sum := sha256.Sum256(item.content)
		f.registry.blobs[item.id] = assetmeta.Blob{BlobID: "blob-" + item.id,
			ByteLength: int64(len(item.content)), Checksum: hex.EncodeToString(sum[:]),
			ChecksumAlgorithm: "sha256"}
		f.registry.locations[item.id] = []assetmeta.Location{
			{Endpoint: f.region.URL, Origin: true}}
		if !item.gone {
			f.served[item.id] = item.content
		}
	}
	keeper := New(f.registry, f.vault, "secret", &http.Client{Timeout: 5 * time.Second})
	return keeper, f
}

const (
	rootID    = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
	textureID = "11111111-2222-4333-8444-555555555555"
	scriptID  = "99999999-8888-4777-8666-555555555555"
	nestedID  = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff"
	innerID   = "12121212-3434-4565-8787-909090909090"
	stockID   = "89556747-24cb-43ed-920b-47caed15465f" // never registered
)

// objectWith builds a homeworldz-object-v1 serialization whose texture entry
// carries defaultTexture and whose task inventory carries the given items.
func objectWith(defaultTexture string, contents ...[2]string) []byte {
	entryRaw, _ := hex.DecodeString(strings.ReplaceAll(defaultTexture, "-", ""))
	entry := append(entryRaw, 0) // default texture, empty exception list
	body := `{"format":"homeworldz-object-v1","name":"box","textureEntry":"` +
		hex.EncodeToString(entry) + `","taskInventory":[`
	for index, item := range contents {
		if index > 0 {
			body += ","
		}
		body += `{"assetId":"` + item[0] + `","assetType":` + item[1] + `}`
	}
	return []byte(body + "]}")
}

// TestEnsureDurableWalksTheClosure is the user-scale case: an object whose
// faces wear a texture and whose contents hold a script and a nested object,
// the nested object holding another texture. One inventory commit must leave
// all five assets vaulted.
func TestEnsureDurableWalksTheClosure(t *testing.T) {
	texture := []byte("texture bytes")
	script := []byte("default { state_entry() {} }")
	inner := []byte("inner texture bytes")
	nested := objectWith(innerID)
	root := objectWith(textureID, [2]string{scriptID, "10"}, [2]string{nestedID, "6"})
	keeper, f := newFixture(t,
		asset{id: rootID, content: root},
		asset{id: textureID, content: texture},
		asset{id: scriptID, content: script},
		asset{id: nestedID, content: nested},
		asset{id: innerID, content: inner},
	)
	if err := keeper.EnsureDurable(context.Background(), rootID, assetrefs.TypeObject); err != nil {
		t.Fatalf("EnsureDurable = %v", err)
	}
	for _, id := range []string{rootID, textureID, scriptID, nestedID, innerID} {
		if !bytes.Equal(f.vault.held["blob-"+id], f.served[id]) {
			t.Fatalf("vault does not hold %s", id)
		}
	}
	// A second commit of the same closure fetches nothing.
	before := len(f.fetches)
	if err := keeper.EnsureDurable(context.Background(), rootID, assetrefs.TypeObject); err != nil {
		t.Fatalf("second EnsureDurable = %v", err)
	}
	total := 0
	for _, count := range f.fetches {
		total += count
	}
	if len(f.fetches) != before || total != 5 {
		t.Fatalf("fetches after second pass = %v", f.fetches)
	}
}

func TestEnsureDurableToleratesExternalReferencesAndCycles(t *testing.T) {
	// The root wears a stock texture (never registered) and contains an object
	// that references the root back. Externals are recorded, not fatal; the
	// cycle terminates on the seen set.
	nested := objectWith(stockID, [2]string{rootID, "6"})
	root := objectWith(stockID, [2]string{nestedID, "6"})
	keeper, f := newFixture(t,
		asset{id: rootID, content: root},
		asset{id: nestedID, content: nested},
		asset{id: stockID, unregistered: true},
	)
	if err := keeper.EnsureDurable(context.Background(), rootID, assetrefs.TypeObject); err != nil {
		t.Fatalf("EnsureDurable = %v", err)
	}
	if len(f.vault.held) != 2 {
		t.Fatalf("vault holds %d blobs, want 2", len(f.vault.held))
	}
}

func TestEnsureDurableRefusesAnIncompleteClosure(t *testing.T) {
	// The script inside the box is registered but no location serves it. The
	// commit must refuse and name the missing asset — an item is only durable
	// when its whole closure is.
	root := objectWith(textureID, [2]string{scriptID, "10"})
	keeper, _ := newFixture(t,
		asset{id: rootID, content: root},
		asset{id: textureID, content: []byte("texture bytes")},
		asset{id: scriptID, content: []byte("lost script"), gone: true},
	)
	err := keeper.EnsureDurable(context.Background(), rootID, assetrefs.TypeObject)
	if !errors.Is(err, ErrUnfetchable) || !strings.Contains(err.Error(), scriptID) {
		t.Fatalf("EnsureDurable = %v, want ErrUnfetchable naming %s", err, scriptID)
	}
}

func TestEnsureDurableFetchesFromARegisteredLocation(t *testing.T) {
	content := []byte("bytes a region still holds")
	keeper, f := newFixture(t, asset{id: textureID, content: content})
	if err := keeper.EnsureDurable(context.Background(), textureID, assetrefs.TypeTexture); err != nil {
		t.Fatalf("EnsureDurable = %v", err)
	}
	if !bytes.Equal(f.vault.held["blob-"+textureID], content) {
		t.Fatalf("vault holds %q", f.vault.held["blob-"+textureID])
	}
	if f.fetches[textureID] != 1 {
		t.Fatalf("fetches = %d, want 1", f.fetches[textureID])
	}
}

func TestEnsureDurablePrefersOriginsAndFallsBack(t *testing.T) {
	content := []byte("bytes only the replica still serves")
	dead := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNotFound)
	}))
	defer dead.Close()
	var order []string
	replica := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		order = append(order, "replica")
		w.Write(content)
	}))
	defer replica.Close()

	keeper, f := newFixture(t, asset{id: textureID, content: content})
	// The origin is listed second but must be tried first: a location claiming
	// to hold the only copy is the one worth reading before it disappears.
	f.registry.locations[textureID] = []assetmeta.Location{
		{Endpoint: replica.URL},
		{Endpoint: dead.URL, Origin: true},
	}
	if err := keeper.EnsureDurable(context.Background(), textureID, assetrefs.TypeTexture); err != nil {
		t.Fatalf("EnsureDurable = %v", err)
	}
	if len(order) != 1 || !bytes.Equal(f.vault.held["blob-"+textureID], content) {
		t.Fatalf("fetch order = %v", order)
	}
}

func TestEnsureDurableReportsUnfetchableBytes(t *testing.T) {
	keeper, _ := newFixture(t, asset{id: textureID, content: []byte("gone"), gone: true})
	if err := keeper.EnsureDurable(context.Background(), textureID,
		assetrefs.TypeTexture); !errors.Is(err, ErrUnfetchable) {
		t.Fatalf("EnsureDurable = %v, want ErrUnfetchable", err)
	}
	// No locations at all is the same refusal, not a pass.
	empty, f := newFixture(t, asset{id: textureID, content: []byte("nowhere")})
	f.registry.locations[textureID] = nil
	delete(f.served, textureID)
	if err := empty.EnsureDurable(context.Background(), textureID,
		assetrefs.TypeTexture); !errors.Is(err, ErrUnfetchable) {
		t.Fatalf("EnsureDurable with no locations = %v, want ErrUnfetchable", err)
	}
}

func TestEnsureDurableRefusesBytesThatDoNotMatchTheRegistry(t *testing.T) {
	// A region serving the wrong bytes fails the ingest instead of corrupting
	// the durable copy — the untrusted-region boundary of ADR 0028 holding.
	keeper, f := newFixture(t, asset{id: textureID, content: []byte("what the registry recorded")})
	f.served[textureID] = []byte("something else entirely")
	if err := keeper.EnsureDurable(context.Background(), textureID,
		assetrefs.TypeTexture); !errors.Is(err, ErrUnfetchable) {
		t.Fatalf("EnsureDurable = %v, want ErrUnfetchable", err)
	}
	if len(f.vault.held) != 0 {
		t.Fatal("vault kept bytes that did not match the registry")
	}
}

func TestEnsureDurableUnknownAssetAndUnavailableVault(t *testing.T) {
	keeper, f := newFixture(t, asset{id: textureID, content: []byte("registered")})
	if err := keeper.EnsureDurable(context.Background(),
		"dddddddd-dddd-4ddd-8ddd-dddddddddddd", assetrefs.TypeTexture); !errors.Is(err, ErrUnregistered) {
		t.Fatalf("unknown asset = %v, want ErrUnregistered", err)
	}
	// A vault that cannot answer must not be read as "durable": failing closed
	// is the whole point of the invariant.
	f.vault.fails = true
	if err := keeper.EnsureDurable(context.Background(), textureID,
		assetrefs.TypeTexture); !errors.Is(err, ErrVaultUnavailable) {
		t.Fatalf("unavailable vault = %v, want ErrVaultUnavailable", err)
	}
	var absent *Keeper
	if err := absent.EnsureDurable(context.Background(), textureID,
		assetrefs.TypeTexture); !errors.Is(err, ErrVaultUnavailable) {
		t.Fatalf("nil keeper = %v, want ErrVaultUnavailable", err)
	}
}
