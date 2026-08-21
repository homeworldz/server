package inventory

import (
	"context"
	"errors"
	"fmt"
)

// ErrAssetNotDurable refuses an inventory write whose asset bytes the vault
// does not hold and could not obtain — the inventory-commit invariant of
// ADR 0026 saying no.
var ErrAssetNotDurable = errors.New("inventory item asset is not durable")

// DurabilityKeeper answers whether an asset's bytes — its whole reference
// closure, since inventory-to-asset is 1:N — are safe in the vault, and puts
// them there if not. The asset type tells the keeper how to parse the bytes
// for references; -1 means unknown, treated as opaque. It is an interface
// here rather than an import so the inventory store keeps the vault at arm's
// length: this package knows only that some writes must be vouched for.
type DurabilityKeeper interface {
	EnsureDurable(ctx context.Context, assetID string, assetType int) error
}

// Asset types whose asset_id is not an asset at all. A link's asset_id names
// the inventory item it points at, so there are no bytes to make durable, and
// asking the registry about one would fail every time.
//
// A calling card's asset_id is the avatar it names, which is a user id rather
// than an asset id and has no bytes anywhere. Firestorm creates the agent's own
// card at login when Friends/All does not already hold one
// (llfriendcard.cpp, create_agent_calling_card_name_cb), so every first login
// reaches this path.
const (
	assetTypeCallingCard = 2
	assetTypeLink        = 24
	assetTypeLinkFolder  = 25
)

// WithDurability wraps a store so that every path creating or re-pointing an
// inventory reference to an asset must first make that asset's bytes durable.
//
// The wrapper sits at the store rather than in each handler on purpose: there
// are eight call sites today across ordinary copies, AIS, and the outfit
// paths, and the invariant is worth exactly nothing if the ninth forgets. A
// nil keeper returns the store unwrapped, which is how tests and tools that
// have no vault keep working.
func WithDurability(store Store, keeper DurabilityKeeper) Store {
	if keeper == nil {
		return store
	}
	return &durableStore{Store: store, keeper: keeper}
}

type durableStore struct {
	Store
	keeper DurabilityKeeper
}

// referencesBytes reports whether an item's asset_id names bytes that must be
// durable before the row may exist.
func referencesBytes(item Item) bool {
	return item.AssetID != "" && item.AssetID != zeroUUID &&
		item.AssetType != assetTypeLink && item.AssetType != assetTypeLinkFolder &&
		item.AssetType != assetTypeCallingCard
}

func (s *durableStore) ensure(ctx context.Context, assetID string, assetType int) error {
	// Both errors are wrapped: callers switch on ErrAssetNotDurable to know an
	// inventory write was refused, and on the keeper's own error to know
	// whether the bytes are missing, unregistered, or simply unreachable right
	// now — which is the difference between "give up" and "try again".
	if err := s.keeper.EnsureDurable(ctx, assetID, assetType); err != nil {
		return fmt.Errorf("%w: %s: %w", ErrAssetNotDurable, assetID, err)
	}
	return nil
}

func (s *durableStore) CreateItem(ctx context.Context, item Item) (Item, error) {
	if referencesBytes(item) {
		if err := s.ensure(ctx, item.AssetID, item.AssetType); err != nil {
			return Item{}, err
		}
	}
	return s.Store.CreateItem(ctx, item)
}

func (s *durableStore) CreateItems(ctx context.Context, items []Item) ([]Item, FolderVersions, error) {
	// Every asset in the batch is made durable before any row is written, so a
	// partially durable batch never becomes a partially durable folder.
	seen := make(map[string]struct{}, len(items))
	for _, item := range items {
		if !referencesBytes(item) {
			continue
		}
		if _, done := seen[item.AssetID]; done {
			continue
		}
		seen[item.AssetID] = struct{}{}
		if err := s.ensure(ctx, item.AssetID, item.AssetType); err != nil {
			return nil, nil, err
		}
	}
	return s.Store.CreateItems(ctx, items)
}

// repointsAsset reports whether an update changes which bytes an item names.
// A lookup that fails answers true: an unknown state keeps the obligation
// rather than quietly dropping it.
func (s *durableStore) repointsAsset(ctx context.Context, item Item) bool {
	items, err := s.Store.ListItems(ctx, item.OwnerUserID)
	if err != nil {
		return true
	}
	for _, existing := range items {
		if existing.ID == item.ID {
			return existing.AssetID != item.AssetID
		}
	}
	return true
}

func (s *durableStore) UpdateItem(ctx context.Context, item Item) (Item, error) {
	// An update that re-points an item at different bytes is a new inventory
	// reference and carries the same obligation as creating one. An update that
	// leaves the asset alone — a move, a rename, a permission change — creates
	// no new reference and must not be gated.
	//
	// Gating it made an item whose bytes were lost impossible to get rid of:
	// moving it to Trash is an update, the update demanded the asset be made
	// durable, and bytes that no longer exist anywhere cannot be. The operator
	// hit this on a gesture from before the vault, whose asset row survives and
	// whose content does not — deleting it was refused 409 asset_not_durable,
	// leaving an item that fails to load at every login and cannot be removed.
	// The invariant is meant to stop dead references being created, not to trap
	// the ones that already went dead.
	if referencesBytes(item) && s.repointsAsset(ctx, item) {
		if err := s.ensure(ctx, item.AssetID, item.AssetType); err != nil {
			return Item{}, err
		}
	}
	return s.Store.UpdateItem(ctx, item)
}

// UpdateItemAsset is the path a saved wearable, notecard, or script takes: the
// viewer uploads new bytes to a region, the region registers them, and the item
// is re-pointed at the new asset. It is the single most important place for
// this check, because it is where a user's own creations enter inventory.
func (s *durableStore) UpdateItemAsset(ctx context.Context, ownerID, itemID, assetID string) (Item, error) {
	if assetID != "" && assetID != zeroUUID {
		// The wire carries no type here; the item being re-pointed already
		// knows its own. A save never changes an item's asset type, so the
		// existing type is the right parser for the new bytes. An item that
		// cannot be found parses as opaque and the underlying update reports
		// the real not-found.
		assetType := -1
		if items, err := s.Store.ListItems(ctx, ownerID); err == nil {
			for _, existing := range items {
				if existing.ID == itemID {
					assetType = existing.AssetType
					break
				}
			}
		}
		if err := s.ensure(ctx, assetID, assetType); err != nil {
			return Item{}, err
		}
	}
	return s.Store.UpdateItemAsset(ctx, ownerID, itemID, assetID)
}

func (s *durableStore) EnsureItem(ctx context.Context, item Item) (bool, error) {
	if referencesBytes(item) {
		if err := s.ensure(ctx, item.AssetID, item.AssetType); err != nil {
			return false, err
		}
	}
	return s.Store.EnsureItem(ctx, item)
}
