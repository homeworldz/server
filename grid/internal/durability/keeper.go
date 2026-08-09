// Package durability enforces the inventory-commit invariant of ADR 0026: an
// inventory item may only be committed when the vault already holds the
// verified blobs for the asset it references — the item's whole reference
// closure, not just the UUID on the row.
//
// Inventory-to-asset is 1:N. An object asset names the textures on its faces
// and the assets in its task inventory; a nested object is itself an asset
// with a closure of its own; wearables name textures; gestures name
// animations and sounds; notecards can embed items. The keeper walks that
// closure breadth-first: ensure an asset's bytes are vaulted, parse the
// vault's own copy by type (ADR 0028 forbids trusting a region-supplied
// reference list), and recurse over what it names.
//
// Making an asset durable means fetching the bytes from a location the
// registry already records and ingesting them, which is deliberately the
// same act as the adoption backfill — one mechanism, so a grid that has been
// running without the vault repairs itself as inventory is touched rather
// than needing a separate reconciliation path. Fetching rather than waiting
// for a region to push is what makes the invariant independent of region
// cooperation, which ADR 0026 requires. A region write-through (PUT to the
// vault) remains worthwhile as an optimization; it is never the thing
// durability depends on.
//
// References that name no registered asset are external — viewer built-in
// textures, plain colors, cross-grid content — and are recorded rather than
// fatal: failing the commit would block every object wearing a stock
// texture, and the grid cannot fetch what was never registered with it. A
// *registered* reference whose bytes no location serves is a different
// story: the closure is genuinely incomplete, and the commit is refused
// rather than pretending the item is whole.
package durability

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/assetrefs"
	"github.com/homeworldz/server/grid/internal/vault"
)

var (
	// ErrUnregistered is an asset the registry has never heard of. Inventory may
	// not reference bytes the grid cannot describe.
	ErrUnregistered = errors.New("asset is not registered")
	// ErrUnfetchable is a registered asset whose bytes no registered location
	// will serve. This is the state the grid was in before the vault: the
	// registry names origins that no longer hold the content, and nothing can
	// recover it.
	ErrUnfetchable = errors.New("asset bytes are not available from any registered location")
	// ErrVaultUnavailable keeps an inventory write from committing when the
	// durability question cannot be answered at all. Failing closed is the
	// point: a commit that skipped the check is exactly the silent data loss
	// the vault exists to prevent.
	ErrVaultUnavailable = errors.New("asset vault is unavailable")
)

// maxClosure bounds a hostile or pathological reference graph. Real content
// is nowhere near it: the user-scale example — a two-prim linkset, six unique
// textures per prim, two scripts — is a fifteen-asset closure.
const maxClosure = 10000

// parseCap bounds how much of a blob is read back for reference parsing.
// Reference-bearing assets are text serializations, small by nature; a
// "gesture" this size is not one.
const parseCap = 8 << 20

// Registry is the part of the asset registry the keeper reads.
type Registry interface {
	Get(ctx context.Context, assetID string) (assetmeta.Asset, error)
	Blob(ctx context.Context, assetID string) (assetmeta.Blob, error)
}

type Keeper struct {
	registry Registry
	vault    vault.Store
	client   *http.Client
	token    string
	logger   *slog.Logger
}

// New builds a keeper. The service token is the internal-tier credential the
// grid already shares with its regions, which is what lets the grid read an
// asset back out of the region that registered it.
func New(registry Registry, store vault.Store, serviceToken string, client *http.Client) *Keeper {
	if client == nil {
		client = &http.Client{Timeout: 30 * time.Second}
	}
	return &Keeper{registry: registry, vault: store, client: client, token: serviceToken,
		logger: slog.Default()}
}

// WithLogger routes the keeper's reports — external references, repairs made
// mid-commit — somewhere an operator will see them.
func (k *Keeper) WithLogger(logger *slog.Logger) *Keeper {
	if logger != nil {
		k.logger = logger
	}
	return k
}

// EnsureDurable returns nil once the vault holds every blob in the reference
// closure of assetID. assetType is the type of the root asset (the inventory
// item's asset_type); referenced assets carry their types in the referencing
// format. assetrefs.TypeUnknown is legal and means the root is treated as
// opaque bytes: vaulted, not parsed.
//
// It is safe to call on every inventory write: for an already-durable
// closure the cost is one indexed lookup per closure member, and the fetches
// only happen the first time an asset becomes inventory-referenced (or the
// first time an old one is touched after the vault arrived).
func (k *Keeper) EnsureDurable(ctx context.Context, assetID string, assetType int) error {
	if k == nil || k.vault == nil || k.registry == nil {
		return ErrVaultUnavailable
	}
	type node struct {
		id     string
		typ    int
		root   bool
		bearer string // the asset whose bytes named this one, for reporting
	}
	queue := []node{{id: assetID, typ: assetType, root: true}}
	seen := map[string]bool{assetID: true}
	for visited := 0; len(queue) > 0; visited++ {
		if visited >= maxClosure {
			return fmt.Errorf("%w: reference closure of %s exceeds %d assets",
				ErrUnfetchable, assetID, maxClosure)
		}
		current := queue[0]
		queue = queue[1:]

		blob, err := k.registry.Blob(ctx, current.id)
		if errors.Is(err, assetmeta.ErrNotFound) {
			if current.root {
				k.logger.Warn("inventory commit refused: asset is not registered",
					"assetId", current.id)
				return ErrUnregistered
			}
			// External reference: recorded, never fatal (package comment).
			k.logger.Info("asset reference is external",
				"assetId", current.id, "referencedBy", current.bearer)
			continue
		} else if err != nil {
			return fmt.Errorf("read asset blob: %w", err)
		}

		held := false
		switch _, err := k.vault.Held(ctx, blob.BlobID); {
		case err == nil:
			held = true
		case errors.Is(err, vault.ErrNotFound):
		case errors.Is(err, vault.ErrInvalid):
			if current.root {
				return ErrUnregistered
			}
			k.logger.Warn("asset reference has an unusable blob registration",
				"assetId", current.id, "referencedBy", current.bearer)
			continue
		default:
			return fmt.Errorf("%w: %s", ErrVaultUnavailable, err)
		}
		if !held {
			if err := k.fetchIntoVault(ctx, current.id, blob.BlobID); err != nil {
				if current.root {
					return err
				}
				// A registered reference nothing serves: the closure is
				// incomplete and the commit must say so, naming the asset.
				return fmt.Errorf("%w: referenced asset %s (via %s): %s",
					ErrUnfetchable, current.id, current.bearer, err)
			}
		}

		if !assetrefs.Bearing(current.typ) {
			continue
		}
		if blob.ByteLength > parseCap {
			k.logger.Warn("reference-bearing asset exceeds the parse cap, treated as opaque",
				"assetId", current.id, "byteLength", blob.ByteLength)
			continue
		}
		content, err := k.readVault(ctx, blob.BlobID, blob.ByteLength)
		if err != nil {
			return fmt.Errorf("%w: %s", ErrVaultUnavailable, err)
		}
		for _, reference := range assetrefs.Gather(current.typ, content) {
			if seen[reference.ID] {
				continue
			}
			seen[reference.ID] = true
			queue = append(queue, node{id: reference.ID, typ: reference.Type,
				bearer: current.id})
		}
	}
	return nil
}

func (k *Keeper) readVault(ctx context.Context, blobID string, length int64) ([]byte, error) {
	content, _, err := k.vault.Open(ctx, blobID)
	if err != nil {
		return nil, err
	}
	defer content.Close()
	return io.ReadAll(io.LimitReader(content, length))
}

// fetchIntoVault reads an asset out of a registered location and writes it
// through to the vault. Origins first: a location that claims to hold the
// only copy is the one most worth reading before it disappears, which is the
// whole failure this prevents.
func (k *Keeper) fetchIntoVault(ctx context.Context, assetID, blobID string) error {
	asset, err := k.registry.Get(ctx, assetID)
	if errors.Is(err, assetmeta.ErrNotFound) {
		return ErrUnregistered
	} else if err != nil {
		return fmt.Errorf("read asset: %w", err)
	}
	locations := make([]assetmeta.Location, 0, len(asset.Locations))
	for _, location := range asset.Locations {
		if location.Origin {
			locations = append(locations, location)
		}
	}
	for _, location := range asset.Locations {
		if !location.Origin {
			locations = append(locations, location)
		}
	}
	var lastErr error
	for _, location := range locations {
		started := time.Now()
		if err := k.ingestFrom(ctx, location.Endpoint, assetID, blobID); err != nil {
			// Named individually because the reasons differ in what they ask
			// of an operator — a refused connection is a stale registration,
			// a slow failure is a location that accepted the connection and
			// then could not answer, which is the shape a region blocked on
			// its own grid call makes.
			k.logger.Warn("asset location did not serve its bytes",
				"assetId", assetID, "endpoint", location.Endpoint,
				"elapsedMs", time.Since(started).Milliseconds(), "error", err)
			lastErr = err
			continue
		}
		k.logger.Info("asset made durable", "assetId", assetID)
		return nil
	}
	// The refusal itself is reported, not only the attempts: an inventory
	// commit answered 409 with nothing in the log naming the asset costs a
	// deploy to diagnose, and the three refusal codes mean entirely different
	// things.
	k.logger.Warn("asset could not be made durable",
		"assetId", assetID, "locations", len(locations))
	if lastErr != nil {
		return fmt.Errorf("%w: %s", ErrUnfetchable, lastErr)
	}
	return ErrUnfetchable
}

// ingestFrom reads an asset back out of one registered location and writes it
// through to the vault. The bytes are never trusted: the vault verifies them
// against the registry's checksum and length, so a region serving the wrong
// content fails the ingest instead of corrupting the durable copy.
func (k *Keeper) ingestFrom(ctx context.Context, endpoint, assetID, blobID string) error {
	address, err := url.Parse(strings.TrimRight(endpoint, "/") + "/api/v1/assets/" + assetID)
	if err != nil {
		return fmt.Errorf("location endpoint is unusable: %w", err)
	}
	if address.Scheme != "http" && address.Scheme != "https" {
		return fmt.Errorf("location endpoint is not http: %s", endpoint)
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, address.String(), nil)
	if err != nil {
		return err
	}
	if k.token != "" {
		request.Header.Set("Authorization", "Bearer "+k.token)
	}
	response, err := k.client.Do(request)
	if err != nil {
		return err
	}
	defer func() {
		// Drain briefly so the connection can be reused; a body we are
		// abandoning is not worth reading in full.
		_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
		response.Body.Close()
	}()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("location %s answered %d", endpoint, response.StatusCode)
	}
	if _, err := k.vault.Ingest(ctx, blobID, response.Body); err != nil {
		return fmt.Errorf("ingest from %s: %w", endpoint, err)
	}
	return nil
}
