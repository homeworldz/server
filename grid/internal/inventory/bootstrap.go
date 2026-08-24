package inventory

import (
	"context"
	"fmt"
)

// Bootstrapper is the slice of Store that preparing an avatar's inventory
// needs. Narrow on purpose: a login path has no business writing anything else
// on the way in, and the narrow interface is what lets the website API — whose
// inventory view is read-only — hold this one capability without holding the
// whole store.
type Bootstrapper interface {
	EnsureSystemFolders(ctx context.Context, userID string) ([]Folder, error)
	ListItems(ctx context.Context, userID string) ([]Item, error)
	EnsureItem(ctx context.Context, item Item) (bool, error)
	ListFolders(ctx context.Context, userID string) ([]Folder, error)
}

// Bootstrap prepares an avatar's inventory for entering the world and returns
// its folders: the system folder skeleton, and the default outfit for an
// account that has never worn anything.
//
// It lives here rather than in a login handler because there are two logins.
// Viewer login has done this since inventory existed; client world entry never
// did, so an account created through the website and used only in the client
// arrived with no system folders and nothing to wear — invisible until it
// happened to log in through Firestorm once, which repaired it silently and
// hid the fault (found 2026-08-24 sweeping for paths only one transport runs).
//
// A nil store means the deployment serves no inventory, and the deterministic
// skeleton is the honest answer: those folder ids are derived from the user id,
// so they are the same ids the store would have created.
func Bootstrap(ctx context.Context, store Bootstrapper, userID string) ([]Folder, error) {
	if store == nil {
		return SystemFolders(userID), nil
	}
	folders, err := store.EnsureSystemFolders(ctx, userID)
	if err != nil {
		return nil, fmt.Errorf("ensure system folders: %w", err)
	}
	existing, err := store.ListItems(ctx, userID)
	if err != nil {
		return nil, fmt.Errorf("list inventory: %w", err)
	}
	defaults := DefaultWearables(userID)
	switch {
	case !DefaultOutfitInitialized(userID, existing):
		for _, item := range defaults {
			if _, err := store.EnsureItem(ctx, item); err != nil {
				return nil, fmt.Errorf("create default outfit: %w", err)
			}
		}
	case DefaultOutfitNeedsRepair(userID, existing):
		// The odd entries are the links into the outfit folder; the parts
		// themselves are already there. Repair replaces the links only, so an
		// avatar that has since edited its shape keeps the edit.
		for index := 1; index < len(defaults); index += 2 {
			if _, err := store.EnsureItem(ctx, defaults[index]); err != nil {
				return nil, fmt.Errorf("repair default outfit: %w", err)
			}
		}
	default:
		// An established avatar: nothing was written, so the folders above are
		// current. EnsureSystemFolders returns the user's whole folder list and
		// not just the system ones, so this is the same answer the re-read
		// below would give, one query cheaper.
		return folders, nil
	}
	folders, err = store.ListFolders(ctx, userID)
	if err != nil {
		return nil, fmt.Errorf("re-read folders: %w", err)
	}
	return folders, nil
}
