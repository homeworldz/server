package httpapi

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/assetmeta"
)

type memoryAssetStore struct{ assets map[string]assetmeta.Asset }

func (s *memoryAssetStore) Register(_ context.Context, input assetmeta.Registration) (assetmeta.Asset, error) {
	if existing, found := s.assets[input.ID]; found {
		if existing.CreatorUserID != input.CreatorUserID || existing.SHA256 != input.SHA256 || existing.Size != input.Size {
			return assetmeta.Asset{}, assetmeta.ErrConflict
		}
		existing.Bake = existing.Bake || input.Bake
		s.assets[input.ID] = existing
		for _, location := range existing.Locations {
			if location.Endpoint == input.Endpoint {
				return existing, nil
			}
		}
		existing.Locations = append(existing.Locations, assetmeta.Location{
			Endpoint: input.Endpoint, Origin: input.Origin, VerifiedAt: time.Now(),
		})
		s.assets[input.ID] = existing
		return existing, nil
	}
	asset := assetmeta.Asset{
		ID: input.ID, CreatorUserID: input.CreatorUserID, SHA256: input.SHA256, Size: input.Size,
		Bake: input.Bake, Locations: []assetmeta.Location{{Endpoint: input.Endpoint, Origin: input.Origin, VerifiedAt: time.Now()}},
	}
	s.assets[input.ID] = asset
	return asset, nil
}

func (s *memoryAssetStore) Get(_ context.Context, id string) (assetmeta.Asset, error) {
	asset, found := s.assets[id]
	if !found {
		return assetmeta.Asset{}, assetmeta.ErrNotFound
	}
	return asset, nil
}

// Blob stands in for the blob an asset names; the fake keys blobs by asset,
// which is enough for callers that only need the checksum and length.
func (s *memoryAssetStore) Blob(_ context.Context, assetID string) (assetmeta.Blob, error) {
	asset, found := s.assets[assetID]
	if !found {
		return assetmeta.Blob{}, assetmeta.ErrNotFound
	}
	return assetmeta.Blob{BlobID: "b" + assetID[1:], ByteLength: asset.Size,
		Checksum: asset.SHA256, ChecksumAlgorithm: "sha256"}, nil
}

func TestAssetMetadataEndpoints(t *testing.T) {
	store := &memoryAssetStore{assets: make(map[string]assetmeta.Asset)}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Assets: store})
	const id = "10000000-0000-4000-8000-000000000001"
	const creator = "20000000-0000-4000-8000-000000000001"
	const hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
	body := `{"id":"` + id + `","creatorUserId":"` + creator + `","sha256":"` + hash +
		`","size":332,"endpoint":"http://region.example:42001/","origin":true}`
	created := requestRegion[assetmeta.Asset](t, handler, http.MethodPost,
		"/api/v1/assets", body, http.StatusCreated)
	if created.ID != id || created.CreatorUserID != creator || len(created.Locations) != 1 ||
		created.Locations[0].Endpoint != "http://region.example:42001" || !created.Locations[0].Origin {
		t.Fatalf("created asset metadata = %#v", created)
	}
	fetched := requestRegion[assetmeta.Asset](t, handler, http.MethodGet,
		"/api/v1/assets/"+id, "", http.StatusOK)
	if fetched.SHA256 != hash || fetched.Size != 332 {
		t.Fatalf("fetched asset metadata = %#v", fetched)
	}
	conflict := requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/assets", `{"id":"`+id+`","creatorUserId":"`+creator+
			`","sha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",`+
			`"size":332,"endpoint":"http://region.example:42001","origin":true}`,
		http.StatusConflict)
	if conflict.Code != "asset_registration_conflict" {
		t.Fatalf("asset conflict = %#v", conflict)
	}
	missing, err := store.Get(context.Background(), "30000000-0000-4000-8000-000000000001")
	if !errors.Is(err, assetmeta.ErrNotFound) || missing.ID != "" {
		t.Fatalf("missing asset = %#v, %v", missing, err)
	}
}
