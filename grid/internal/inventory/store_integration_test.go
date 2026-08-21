package inventory

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/identifier"
	_ "github.com/jackc/pgx/v5/stdlib"
)

func TestPostgresSystemFolderLifecycle(t *testing.T) {
	databaseURL := os.Getenv("HOMEWORLDZ_TEST_DATABASE_URL")
	if databaseURL == "" {
		t.Skip("HOMEWORLDZ_TEST_DATABASE_URL is not configured")
	}
	db, err := sql.Open("pgx", databaseURL)
	if err != nil {
		t.Fatal(err)
	}
	// Registered before any row cleanup so it runs last: t.Cleanup is
	// last-in-first-out, and a deferred close would instead run before every
	// cleanup below, leaving them to fail silently against a closed pool.
	t.Cleanup(func() { _ = db.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	userID, err := identifier.NewUUID()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.ExecContext(ctx, `INSERT INTO users (id, username, password_hash)
		VALUES ($1, $2, 'integration-only')`, userID, fmt.Sprintf("inventory.%d", time.Now().UnixNano())); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _, _ = db.Exec("DELETE FROM users WHERE id = $1", userID) })
	store := NewPostgresStore(db)
	first, err := store.EnsureSystemFolders(ctx, userID)
	if err != nil {
		t.Fatal(err)
	}
	second, err := store.EnsureSystemFolders(ctx, userID)
	if err != nil {
		t.Fatal(err)
	}
	if len(first) != 23 || len(second) != len(first) {
		t.Fatalf("folder counts = %d, %d", len(first), len(second))
	}
	customFolderID, _ := identifier.NewUUID()
	custom, err := store.CreateFolder(ctx, Folder{ID: customFolderID, OwnerUserID: userID,
		ParentID: first[0].ID, Name: "Integration Projects", TypeDefault: -1})
	if err != nil || custom.Version != 1 {
		t.Fatalf("create custom folder = %#v, error = %v", custom, err)
	}
	if _, err := store.CreateFolder(ctx, custom); !errors.Is(err, ErrFolderConflict) {
		t.Fatalf("duplicate custom folder error = %v", err)
	}
	custom.Name = "Renamed Integration Projects"
	custom, err = store.UpdateFolder(ctx, custom)
	if err != nil || custom.Version != 2 || custom.Name != "Renamed Integration Projects" {
		t.Fatalf("update custom folder = %#v, error = %v", custom, err)
	}
	custom.ParentID = SystemFolderID(userID, 14)
	custom, err = store.UpdateFolder(ctx, custom)
	if err != nil || custom.Version != 3 || custom.ParentID != SystemFolderID(userID, 14) {
		t.Fatalf("move custom folder = %#v, error = %v", custom, err)
	}
	itemID, _ := identifier.NewUUID()
	assetID, _ := identifier.NewUUID()
	item := Item{ID: itemID, OwnerUserID: userID, CreatorUserID: userID,
		FolderID: custom.ID, AssetID: assetID, AssetType: 13, InventoryType: 18,
		Name: "Integration Shape", BasePermissions: 0x7fffffff, CurrentPermissions: 0x7fffffff}
	if inserted, err := store.EnsureItem(ctx, item); err != nil || !inserted {
		t.Fatalf("first ensure item inserted = %v, error = %v", inserted, err)
	}
	if inserted, err := store.EnsureItem(ctx, item); err != nil || inserted {
		t.Fatalf("second ensure item inserted = %v, error = %v", inserted, err)
	}
	replacementAssetID, _ := identifier.NewUUID()
	item.AssetID = replacementAssetID
	if updated, err := store.EnsureItem(ctx, item); err != nil || !updated {
		t.Fatalf("changed ensure item updated = %v, error = %v", updated, err)
	}
	editedWearableAssetID, _ := identifier.NewUUID()
	editedWearable, err := store.UpdateItemAsset(ctx, userID, itemID, editedWearableAssetID)
	if err != nil || editedWearable.AssetID != editedWearableAssetID ||
		editedWearable.CreatorUserID != userID {
		t.Fatalf("update wearable asset = %#v, error = %v", editedWearable, err)
	}
	replacementAssetID = editedWearableAssetID
	uploadedItemID, _ := identifier.NewUUID()
	uploadedAssetID, _ := identifier.NewUUID()
	uploaded := Item{ID: uploadedItemID, OwnerUserID: userID, CreatorUserID: userID,
		FolderID: SystemFolderID(userID, 0), AssetID: uploadedAssetID,
		AssetType: 0, InventoryType: 0, Name: "Uploaded Texture",
		BasePermissions: 0x7fffffff, CurrentPermissions: 0x7fffffff,
		NextPermissions: 0x7fffffff}
	if created, err := store.CreateItem(ctx, uploaded); err != nil || created.CreatedAt.IsZero() {
		t.Fatalf("create uploaded item = %#v, error = %v", created, err)
	}
	if _, err := store.CreateItem(ctx, uploaded); !errors.Is(err, ErrItemConflict) {
		t.Fatalf("duplicate uploaded item error = %v", err)
	}
	// A copy of a creator-less item (NULL creator_user_id, as the default
	// wearables have) arrives at CreateItem carrying the zero UUID that
	// ListItems reports for it, and must store NULL — the reference to
	// users(id) permits nothing else.
	creatorlessItemID, _ := identifier.NewUUID()
	creatorless := uploaded
	creatorless.ID = creatorlessItemID
	creatorless.CreatorUserID = zeroUUID
	creatorless.Name = "Creatorless Texture Copy"
	if created, err := store.CreateItem(ctx, creatorless); err != nil ||
		created.CreatorUserID != zeroUUID {
		t.Fatalf("create creatorless item = %#v, error = %v", created, err)
	}
	var creatorIsNull bool
	if err := db.QueryRowContext(ctx, `SELECT creator_user_id IS NULL FROM inventory_items
		WHERE id = $1`, creatorlessItemID).Scan(&creatorIsNull); err != nil || !creatorIsNull {
		t.Fatalf("creatorless item creator IS NULL = %v, error = %v", creatorIsNull, err)
	}
	linkOneID, _ := identifier.NewUUID()
	linkTwoID, _ := identifier.NewUUID()
	currentOutfitID := SystemFolderID(userID, 46)
	links, linkVersions, err := store.CreateItems(ctx, []Item{
		{ID: linkOneID, OwnerUserID: userID, CreatorUserID: userID, FolderID: currentOutfitID,
			AssetID: itemID, AssetType: 24, InventoryType: 18, Name: "Integration Shape",
			BasePermissions: 0x7fffffff, CurrentPermissions: 0x7fffffff},
		{ID: linkTwoID, OwnerUserID: userID, CreatorUserID: userID, FolderID: currentOutfitID,
			AssetID: uploadedItemID, AssetType: 24, InventoryType: 0, Name: "Integration Texture",
			BasePermissions: 0x7fffffff, CurrentPermissions: 0x7fffffff},
	})
	if err != nil || len(links) != 2 || links[0].CreatedAt.IsZero() || links[1].CreatedAt.IsZero() {
		t.Fatalf("create inventory links = %#v, error = %v", links, err)
	}
	// The batch reports the version its own transaction produced, read before
	// it committed anything else. Two links means two increments, and the
	// number must be the folder's own — not whatever a later query happens to
	// see once other mutations have landed.
	folders, err := store.ListFolders(ctx, userID)
	if err != nil {
		t.Fatal(err)
	}
	var outfitVersion int64
	for _, folder := range folders {
		if folder.ID == currentOutfitID {
			outfitVersion = folder.Version
		}
	}
	if linkVersions[currentOutfitID] != outfitVersion {
		t.Fatalf("batch reported Current Outfit version %d, folder says %d",
			linkVersions[currentOutfitID], outfitVersion)
	}
	uploaded.Name = "Renamed Uploaded Texture"
	uploaded.Description = "AIS integration update"
	uploaded.FolderID = custom.ID
	updated, err := store.UpdateItem(ctx, uploaded)
	if err != nil || updated.Name != uploaded.Name || updated.FolderID != custom.ID ||
		updated.AssetID != uploadedAssetID || updated.CreatorUserID != userID {
		t.Fatalf("updated uploaded item = %#v, error = %v", updated, err)
	}
	items, err := store.ListItems(ctx, userID)
	if err != nil || len(items) != 5 {
		t.Fatalf("inventory items = %#v, error = %v", items, err)
	}
	found := map[string]bool{}
	for _, listed := range items {
		found[listed.AssetID] = true
	}
	if !found[uploadedAssetID] || !found[replacementAssetID] {
		t.Fatalf("inventory assets = %#v", found)
	}
	deleted, err := store.DeleteItem(ctx, userID, uploadedItemID)
	if err != nil || deleted.AssetID != uploadedAssetID {
		t.Fatalf("deleted uploaded item = %#v, error = %v", deleted, err)
	}
	if _, err := store.DeleteItem(ctx, userID, uploadedItemID); !errors.Is(err, ErrItemNotFound) {
		t.Fatalf("delete missing uploaded item error = %v", err)
	}
	removedFolders, removedItems, trash, err := store.PurgeFolder(ctx, userID, SystemFolderID(userID, 14))
	if err != nil || len(removedFolders) != 1 || removedFolders[0] != custom.ID ||
		len(removedItems) != 1 || removedItems[0] != itemID || trash.TypeDefault != 14 {
		t.Fatalf("purged trash folders = %#v, items = %#v, trash = %#v, error = %v",
			removedFolders, removedItems, trash, err)
	}
}
