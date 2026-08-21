package inventory

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

const zeroUUID = "00000000-0000-0000-0000-000000000000"

var (
	ErrFolderConflict     = errors.New("inventory folder already exists")
	ErrFolderNotFound     = errors.New("inventory parent folder not found")
	ErrInvalidFolder      = errors.New("inventory folder is invalid")
	ErrItemConflict       = errors.New("inventory item already exists")
	ErrItemNotFound       = errors.New("inventory item not found")
	ErrItemFolderNotFound = errors.New("inventory item folder not found")
	ErrInvalidItem        = errors.New("inventory item is invalid")
)

type Folder struct {
	ID          string `json:"id"`
	OwnerUserID string `json:"ownerUserId"`
	ParentID    string `json:"parentId"`
	Name        string `json:"name"`
	TypeDefault int    `json:"typeDefault"`
	Version     int64  `json:"version"`
}

type Item struct {
	ID                  string    `json:"id"`
	OwnerUserID         string    `json:"ownerUserId"`
	CreatorUserID       string    `json:"creatorUserId"`
	FolderID            string    `json:"folderId"`
	AssetID             string    `json:"assetId"`
	AssetType           int       `json:"assetType"`
	InventoryType       int       `json:"inventoryType"`
	Name                string    `json:"name"`
	Description         string    `json:"description"`
	Flags               uint32    `json:"flags"`
	BasePermissions     uint32    `json:"basePermissions"`
	CurrentPermissions  uint32    `json:"currentPermissions"`
	EveryonePermissions uint32    `json:"everyonePermissions"`
	NextPermissions     uint32    `json:"nextPermissions"`
	SaleType            int       `json:"saleType"`
	SalePrice           int       `json:"salePrice"`
	CreatedAt           time.Time `json:"createdAt"`
}

// FolderVersions maps a folder id to its version. A mutation returns the
// versions its own transaction produced, which is the only way the number can
// describe the change it accompanies: read afterwards, in a separate query, it
// reports whatever the folder looked like once every concurrent mutation had
// also landed. AIS sends these to the viewer, and a viewer told a version that
// belongs to somebody else's change re-reads and re-reconciles the outfit it
// is already wearing.
type FolderVersions map[string]int64

type Store interface {
	EnsureSystemFolders(context.Context, string) ([]Folder, error)
	CreateFolder(context.Context, Folder) (Folder, error)
	UpdateFolder(context.Context, Folder) (Folder, error)
	ListFolders(context.Context, string) ([]Folder, error)
	CreateItem(context.Context, Item) (Item, error)
	CreateItems(context.Context, []Item) ([]Item, FolderVersions, error)
	UpdateItem(context.Context, Item) (Item, error)
	UpdateItemAsset(context.Context, string, string, string) (Item, error)
	DeleteItem(context.Context, string, string) (Item, error)
	EnsureItem(context.Context, Item) (bool, error)
	ListItems(context.Context, string) ([]Item, error)
	PurgeFolder(context.Context, string, string) ([]string, []string, Folder, error)
}

type PostgresStore struct{ db *sql.DB }

func NewPostgresStore(db *sql.DB) *PostgresStore { return &PostgresStore{db: db} }

func SystemFolderID(userID string, folderType int) string {
	return deterministicUUID(userID + "\x00homeworldz-inventory-folder\x00" + strconv.Itoa(folderType))
}

func deterministicUUID(seed string) string {
	value := sha256.Sum256([]byte(seed))
	value[6] = (value[6] & 0x0f) | 0x80
	value[8] = (value[8] & 0x3f) | 0x80
	encoded := hex.EncodeToString(value[:16])
	return encoded[0:8] + "-" + encoded[8:12] + "-" + encoded[12:16] + "-" +
		encoded[16:20] + "-" + encoded[20:32]
}

func DefaultWearables(userID string) []Item {
	const fullPermissions = uint32(0x7fffffff)
	bodyPartsID := SystemFolderID(userID, 13)
	clothingID := SystemFolderID(userID, 5)
	currentOutfitID := SystemFolderID(userID, 46)
	definitions := []struct {
		name          string
		wearableType  uint32
		assetID       string
		assetType     int
		inventoryType int
		folderID      string
	}{
		{"Default Shape", 0, "66c41e39-38f9-f75a-024e-585989bfab73", 13, 18, bodyPartsID},
		// The skin declares all 37 parameters avatar_lad.xml defines for its
		// type. The asset it replaces declared 26, and Firestorm logged
		// "Wearable parameter mismatch. Reading in 26 from file, but created 37
		// from avatar parameters" on every login that loaded it. A new id
		// rather than corrected bytes under the old one: assets are
		// content-addressed, and the region re-stores bundled files by their
		// filename UUID at every startup, so editing in place would rebind an
		// id to a different checksum. Avatars already wearing the old skin keep
		// it and look identical — every added parameter defaults to 0, which is
		// what the viewer was filling in anyway.
		//
		// Named "Default Skin" rather than the "Sexy - Female Skin" it carried
		// from wherever it was imported. One skin asset serves every avatar:
		// sex is shape parameter 80, not a property of the skin, so a gendered
		// name described nothing true about the file.
		{"Default Skin", 1, "00000000-0000-1111-9999-000000000202", 13, 18, bodyPartsID},
		{"Default Hair", 2, "d342e6c0-b9d2-11dc-95ff-0800200c9a66", 13, 18, bodyPartsID},
		{"Default Eyes", 3, "4bb6fa4d-1cd2-498a-a84c-95c1a0e745a7", 13, 18, bodyPartsID},
		{"Default Shirt", 4, "00000000-38f9-1111-024e-222222111110", 5, 18, clothingID},
		{"Default Pants", 5, "00000000-38f9-1111-024e-222222111120", 5, 18, clothingID},
	}
	items := make([]Item, 0, len(definitions)*2)
	for _, definition := range definitions {
		itemID := deterministicUUID(userID + "\x00homeworldz-default-wearable-item\x00" +
			strconv.FormatUint(uint64(definition.wearableType), 10))
		items = append(items, Item{
			ID: itemID, OwnerUserID: userID, FolderID: definition.folderID,
			AssetID:   definition.assetID,
			AssetType: definition.assetType, InventoryType: definition.inventoryType,
			Name: definition.name, Flags: definition.wearableType,
			BasePermissions: fullPermissions, CurrentPermissions: fullPermissions,
			NextPermissions: fullPermissions,
		})
		items = append(items, Item{
			ID: deterministicUUID(userID + "\x00homeworldz-current-outfit-link\x00" +
				strconv.FormatUint(uint64(definition.wearableType), 10)),
			OwnerUserID: userID, FolderID: currentOutfitID, AssetID: itemID,
			AssetType: 24, InventoryType: 18, Name: definition.name, Flags: definition.wearableType,
			BasePermissions: fullPermissions, CurrentPermissions: fullPermissions,
			NextPermissions: fullPermissions,
		})
	}
	return items
}

// DefaultOutfitInitialized distinguishes an established avatar from an account
// that has never received its initial outfit. The deterministic shape item is
// created with the outfit and remains in the user's Body Parts folder when the
// user later changes what is worn.
func DefaultOutfitInitialized(userID string, items []Item) bool {
	markerID := deterministicUUID(userID + "\x00homeworldz-default-wearable-item\x000")
	for _, item := range items {
		if item.ID == markerID && item.OwnerUserID == userID {
			return true
		}
	}
	return false
}

// DefaultOutfitNeedsRepair identifies the narrow interrupted-initialization
// state where every original default wearable still exists but Current Outfit
// has no entries at all. A non-empty COF is always left alone so an established
// avatar's chosen outfit is never replaced by the defaults.
func DefaultOutfitNeedsRepair(userID string, items []Item) bool {
	if !DefaultOutfitInitialized(userID, items) {
		return false
	}
	currentOutfitID := SystemFolderID(userID, 46)
	for _, item := range items {
		if item.OwnerUserID == userID && item.FolderID == currentOutfitID {
			return false
		}
	}
	defaults := DefaultWearables(userID)
	remainingSources := make(map[string]bool, len(defaults)/2)
	for index := 0; index < len(defaults); index += 2 {
		remainingSources[defaults[index].ID] = false
	}
	for _, item := range items {
		if _, expected := remainingSources[item.ID]; expected && item.OwnerUserID == userID {
			remainingSources[item.ID] = true
		}
	}
	for _, found := range remainingSources {
		if !found {
			return false
		}
	}
	return true
}

func SystemFolders(userID string) []Folder {
	rootID := SystemFolderID(userID, 8)
	folders := []Folder{{rootID, userID, zeroUUID, "My Inventory", 8, 1}}
	for _, folder := range []struct {
		name       string
		folderType int
	}{
		{"Textures", 0}, {"Sounds", 1}, {"Calling Cards", 2}, {"Landmarks", 3},
		{"Clothing", 5}, {"Objects", 6}, {"Notecards", 7}, {"Scripts", 10},
		{"Body Parts", 13}, {"Trash", 14}, {"Photo Album", 15}, {"Lost And Found", 16},
		{"Animations", 20}, {"Gestures", 21}, {"Favorites", 23}, {"Current Outfit", 46},
		{"My Outfits", 48}, {"Received Items", 50}, {"Settings", 56}, {"Materials", 57},
	} {
		folders = append(folders, Folder{SystemFolderID(userID, folder.folderType), userID, rootID,
			folder.name, folder.folderType, 1})
	}
	callingCardsID := SystemFolderID(userID, 2)
	friendsID := deterministicUUID(userID + "\x00homeworldz-calling-cards-folder\x00friends")
	allID := deterministicUUID(userID + "\x00homeworldz-calling-cards-folder\x00all")
	folders = append(folders,
		Folder{friendsID, userID, callingCardsID, "Friends", 2, 1},
		Folder{allID, userID, friendsID, "All", 2, 1})
	return folders
}

func (s *PostgresStore) EnsureSystemFolders(ctx context.Context, userID string) ([]Folder, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, fmt.Errorf("begin inventory folder transaction: %w", err)
	}
	defer tx.Rollback()
	for _, folder := range SystemFolders(userID) {
		var parent any
		if folder.ParentID != zeroUUID {
			parent = folder.ParentID
		}
		if _, err := tx.ExecContext(ctx, `
			INSERT INTO inventory_folders (id, owner_user_id, parent_id, name, type_default, version)
			VALUES ($1, $2, $3, $4, $5, 1) ON CONFLICT (id) DO NOTHING`,
			folder.ID, folder.OwnerUserID, parent, folder.Name, folder.TypeDefault); err != nil {
			return nil, fmt.Errorf("ensure inventory folder type %d: %w", folder.TypeDefault, err)
		}
	}
	folders, err := listFolders(ctx, tx, userID)
	if err != nil {
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, fmt.Errorf("commit inventory folders: %w", err)
	}
	return folders, nil
}

func (s *PostgresStore) ListFolders(ctx context.Context, userID string) ([]Folder, error) {
	return listFolders(ctx, s.db, userID)
}

func (s *PostgresStore) CreateFolder(ctx context.Context, folder Folder) (Folder, error) {
	folder.Name = strings.TrimSpace(folder.Name)
	if folder.ID == "" || folder.OwnerUserID == "" || folder.ParentID == "" ||
		folder.ParentID == zeroUUID || len(folder.Name) == 0 || len(folder.Name) > 255 ||
		(folder.TypeDefault != -1 && folder.TypeDefault != 47) {
		return Folder{}, ErrInvalidFolder
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Folder{}, fmt.Errorf("begin inventory folder creation: %w", err)
	}
	defer tx.Rollback()
	var parentVersion int64
	var parentType int
	if err := tx.QueryRowContext(ctx, `SELECT version, type_default FROM inventory_folders
		WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`, folder.ParentID, folder.OwnerUserID).
		Scan(&parentVersion, &parentType); errors.Is(err, sql.ErrNoRows) {
		return Folder{}, ErrFolderNotFound
	} else if err != nil {
		return Folder{}, fmt.Errorf("find inventory parent folder: %w", err)
	}
	if folder.TypeDefault == 47 && parentType != 48 {
		return Folder{}, ErrInvalidFolder
	}
	result, err := tx.ExecContext(ctx, `INSERT INTO inventory_folders
		(id, owner_user_id, parent_id, name, type_default, version)
		VALUES ($1, $2, $3, $4, $5, 1) ON CONFLICT (id) DO NOTHING`,
		folder.ID, folder.OwnerUserID, folder.ParentID, folder.Name, folder.TypeDefault)
	if err != nil {
		return Folder{}, fmt.Errorf("create inventory folder: %w", err)
	}
	created, err := result.RowsAffected()
	if err != nil {
		return Folder{}, fmt.Errorf("count created inventory folder: %w", err)
	}
	if created == 0 {
		return Folder{}, ErrFolderConflict
	}
	if _, err := tx.ExecContext(ctx, `UPDATE inventory_folders
		SET version = version + 1, updated_at = now() WHERE id = $1 AND owner_user_id = $2`,
		folder.ParentID, folder.OwnerUserID); err != nil {
		return Folder{}, fmt.Errorf("update inventory parent version: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Folder{}, fmt.Errorf("commit inventory folder creation: %w", err)
	}
	folder.Version = 1
	return folder, nil
}

func (s *PostgresStore) UpdateFolder(ctx context.Context, folder Folder) (Folder, error) {
	folder.Name = strings.TrimSpace(folder.Name)
	if folder.ID == "" || folder.OwnerUserID == "" || folder.ParentID == "" ||
		len(folder.Name) == 0 || len(folder.Name) > 255 ||
		(folder.TypeDefault != -1 && folder.TypeDefault != 47) {
		return Folder{}, ErrInvalidFolder
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Folder{}, fmt.Errorf("begin inventory folder update: %w", err)
	}
	defer tx.Rollback()
	var existing Folder
	if err := tx.QueryRowContext(ctx, `SELECT id, owner_user_id, parent_id, name, type_default, version
		FROM inventory_folders WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`,
		folder.ID, folder.OwnerUserID).Scan(&existing.ID, &existing.OwnerUserID, &existing.ParentID,
		&existing.Name, &existing.TypeDefault, &existing.Version); errors.Is(err, sql.ErrNoRows) {
		return Folder{}, ErrFolderNotFound
	} else if err != nil {
		return Folder{}, fmt.Errorf("find inventory folder for update: %w", err)
	}
	if existing.TypeDefault != folder.TypeDefault ||
		(existing.TypeDefault != -1 && existing.TypeDefault != 47) {
		return Folder{}, ErrInvalidFolder
	}
	parentChanged := existing.ParentID != folder.ParentID
	if parentChanged {
		var destinationVersion int64
		var destinationType int
		if err := tx.QueryRowContext(ctx, `SELECT version, type_default FROM inventory_folders
			WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`, folder.ParentID, folder.OwnerUserID).
			Scan(&destinationVersion, &destinationType); errors.Is(err, sql.ErrNoRows) {
			return Folder{}, ErrFolderNotFound
		} else if err != nil {
			return Folder{}, fmt.Errorf("find inventory folder destination: %w", err)
		}
		if folder.TypeDefault == 47 && destinationType != 48 && destinationType != 14 {
			return Folder{}, ErrInvalidFolder
		}
		var createsCycle bool
		if err := tx.QueryRowContext(ctx, `WITH RECURSIVE descendants AS (
			SELECT id FROM inventory_folders WHERE parent_id = $1 AND owner_user_id = $2
			UNION ALL
			SELECT child.id FROM inventory_folders child JOIN descendants parent ON child.parent_id = parent.id
			WHERE child.owner_user_id = $2)
			SELECT EXISTS (SELECT 1 FROM descendants WHERE id = $3)`, folder.ID, folder.OwnerUserID,
			folder.ParentID).Scan(&createsCycle); err != nil {
			return Folder{}, fmt.Errorf("check inventory folder move cycle: %w", err)
		}
		if createsCycle || folder.ParentID == folder.ID {
			return Folder{}, ErrInvalidFolder
		}
	}
	if existing.Name == folder.Name && !parentChanged {
		return existing, nil
	}
	if err := tx.QueryRowContext(ctx, `UPDATE inventory_folders
		SET parent_id = $3, name = $4, version = version + 1, updated_at = now()
		WHERE id = $1 AND owner_user_id = $2 RETURNING version`,
		folder.ID, folder.OwnerUserID, folder.ParentID, folder.Name).Scan(&folder.Version); err != nil {
		return Folder{}, fmt.Errorf("update inventory folder: %w", err)
	}
	if parentChanged {
		for _, parentID := range []string{existing.ParentID, folder.ParentID} {
			if _, err := tx.ExecContext(ctx, `UPDATE inventory_folders SET version = version + 1,
				updated_at = now() WHERE id = $1 AND owner_user_id = $2`, parentID, folder.OwnerUserID); err != nil {
				return Folder{}, fmt.Errorf("update moved inventory folder parent version: %w", err)
			}
		}
	}
	if err := tx.Commit(); err != nil {
		return Folder{}, fmt.Errorf("commit inventory folder update: %w", err)
	}
	return folder, nil
}

// creatorColumn is the creator_user_id column value for an item, normalizing
// the two spellings of "no creator": the empty string and the zero UUID both
// store NULL — the column references users(id), which no creator-less item can
// satisfy — and the item itself carries the zero UUID, the form ListItems
// reports NULL as.
func creatorColumn(item *Item) any {
	if item.CreatorUserID == "" || item.CreatorUserID == zeroUUID {
		item.CreatorUserID = zeroUUID
		return nil
	}
	return item.CreatorUserID
}

func (s *PostgresStore) EnsureItem(ctx context.Context, item Item) (bool, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, fmt.Errorf("begin inventory item transaction: %w", err)
	}
	defer tx.Rollback()
	creator := creatorColumn(&item)
	result, err := tx.ExecContext(ctx, `
		INSERT INTO inventory_items
			(id, owner_user_id, creator_user_id, folder_id, asset_id, asset_type, inventory_type,
			 name, description, flags, base_permissions, current_permissions, everyone_permissions,
			 next_permissions, sale_type, sale_price)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)
		ON CONFLICT (id) DO UPDATE SET
			creator_user_id = EXCLUDED.creator_user_id, folder_id = EXCLUDED.folder_id,
			asset_id = EXCLUDED.asset_id, asset_type = EXCLUDED.asset_type,
			inventory_type = EXCLUDED.inventory_type, name = EXCLUDED.name,
			description = EXCLUDED.description, flags = EXCLUDED.flags,
			base_permissions = EXCLUDED.base_permissions,
			current_permissions = EXCLUDED.current_permissions,
			everyone_permissions = EXCLUDED.everyone_permissions,
			next_permissions = EXCLUDED.next_permissions, sale_type = EXCLUDED.sale_type,
			sale_price = EXCLUDED.sale_price, updated_at = now()
		WHERE (inventory_items.creator_user_id, inventory_items.folder_id, inventory_items.asset_id,
			inventory_items.asset_type, inventory_items.inventory_type, inventory_items.name,
			inventory_items.description, inventory_items.flags, inventory_items.base_permissions,
			inventory_items.current_permissions, inventory_items.everyone_permissions,
			inventory_items.next_permissions, inventory_items.sale_type, inventory_items.sale_price)
		IS DISTINCT FROM
			(EXCLUDED.creator_user_id, EXCLUDED.folder_id, EXCLUDED.asset_id, EXCLUDED.asset_type,
			 EXCLUDED.inventory_type, EXCLUDED.name, EXCLUDED.description, EXCLUDED.flags,
			 EXCLUDED.base_permissions, EXCLUDED.current_permissions, EXCLUDED.everyone_permissions,
			 EXCLUDED.next_permissions, EXCLUDED.sale_type, EXCLUDED.sale_price)`,
		item.ID, item.OwnerUserID, creator, item.FolderID, item.AssetID,
		item.AssetType, item.InventoryType, item.Name, item.Description, item.Flags, item.BasePermissions,
		item.CurrentPermissions, item.EveryonePermissions, item.NextPermissions, item.SaleType, item.SalePrice)
	if err != nil {
		return false, fmt.Errorf("ensure inventory item: %w", err)
	}
	count, err := result.RowsAffected()
	if err != nil {
		return false, fmt.Errorf("count ensured inventory item: %w", err)
	}
	if count != 0 {
		if _, err := tx.ExecContext(ctx, `UPDATE inventory_folders
			SET version = version + 1, updated_at = now()
			WHERE id = $1 AND owner_user_id = $2`, item.FolderID, item.OwnerUserID); err != nil {
			return false, fmt.Errorf("update inventory folder version: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return false, fmt.Errorf("commit inventory item: %w", err)
	}
	return count != 0, nil
}

func (s *PostgresStore) CreateItem(ctx context.Context, item Item) (Item, error) {
	item.Name = strings.TrimSpace(item.Name)
	if item.ID == "" || item.OwnerUserID == "" ||
		item.FolderID == "" || item.AssetID == "" || len(item.Name) == 0 ||
		len(item.Name) > 255 || len(item.Description) > 1024 || item.AssetType < 0 ||
		item.AssetType > 127 || item.InventoryType < 0 || item.InventoryType > 127 ||
		item.SaleType < 0 || item.SaleType > 3 || item.SalePrice < 0 {
		return Item{}, ErrInvalidItem
	}
	// A creator-less item is legal — default wearables are made without one,
	// and creator_user_id is ON DELETE SET NULL — so a copy of one must
	// round-trip the zero UUID that ListItems reports for it.
	creator := creatorColumn(&item)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Item{}, fmt.Errorf("begin inventory item creation: %w", err)
	}
	defer tx.Rollback()
	var folderVersion int64
	if err := tx.QueryRowContext(ctx, `SELECT version FROM inventory_folders
		WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`, item.FolderID, item.OwnerUserID).
		Scan(&folderVersion); errors.Is(err, sql.ErrNoRows) {
		return Item{}, ErrItemFolderNotFound
	} else if err != nil {
		return Item{}, fmt.Errorf("find inventory item folder: %w", err)
	}
	result, err := tx.ExecContext(ctx, `INSERT INTO inventory_items
		(id, owner_user_id, creator_user_id, folder_id, asset_id, asset_type, inventory_type,
		 name, description, flags, base_permissions, current_permissions, everyone_permissions,
		 next_permissions, sale_type, sale_price)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)
		ON CONFLICT (id) DO NOTHING`, item.ID, item.OwnerUserID, creator,
		item.FolderID, item.AssetID, item.AssetType, item.InventoryType, item.Name,
		item.Description, item.Flags, item.BasePermissions, item.CurrentPermissions,
		item.EveryonePermissions, item.NextPermissions, item.SaleType, item.SalePrice)
	if err != nil {
		return Item{}, fmt.Errorf("create inventory item: %w", err)
	}
	created, err := result.RowsAffected()
	if err != nil {
		return Item{}, fmt.Errorf("count created inventory item: %w", err)
	}
	if created == 0 {
		return Item{}, ErrItemConflict
	}
	if _, err := tx.ExecContext(ctx, `UPDATE inventory_folders
		SET version = version + 1, updated_at = now()
		WHERE id = $1 AND owner_user_id = $2`, item.FolderID, item.OwnerUserID); err != nil {
		return Item{}, fmt.Errorf("update inventory item folder version: %w", err)
	}
	if err := tx.QueryRowContext(ctx, `SELECT created_at FROM inventory_items
		WHERE id = $1 AND owner_user_id = $2`, item.ID, item.OwnerUserID).Scan(&item.CreatedAt); err != nil {
		return Item{}, fmt.Errorf("read created inventory item: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Item{}, fmt.Errorf("commit inventory item creation: %w", err)
	}
	return item, nil
}

func (s *PostgresStore) CreateItems(ctx context.Context, items []Item) ([]Item, FolderVersions, error) {
	if len(items) == 0 || len(items) > 256 {
		return nil, nil, ErrInvalidItem
	}
	ownerID := items[0].OwnerUserID
	folderID := items[0].FolderID
	seen := make(map[string]bool, len(items))
	for index := range items {
		items[index].Name = strings.TrimSpace(items[index].Name)
		item := items[index]
		if item.ID == "" || item.OwnerUserID == "" ||
			item.OwnerUserID != ownerID || item.FolderID == "" || item.FolderID != folderID ||
			item.AssetID == "" || len(item.Name) == 0 || len(item.Name) > 255 ||
			len(item.Description) > 1024 || item.AssetType < 0 || item.AssetType > 127 ||
			item.InventoryType < 0 || item.InventoryType > 127 || item.SaleType < 0 ||
			item.SaleType > 3 || item.SalePrice < 0 || seen[item.ID] {
			return nil, nil, ErrInvalidItem
		}
		seen[item.ID] = true
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, nil, fmt.Errorf("begin inventory item batch creation: %w", err)
	}
	defer tx.Rollback()
	var folderVersion int64
	if err := tx.QueryRowContext(ctx, `SELECT version FROM inventory_folders
		WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`, folderID, ownerID).
		Scan(&folderVersion); errors.Is(err, sql.ErrNoRows) {
		return nil, nil, ErrItemFolderNotFound
	} else if err != nil {
		return nil, nil, fmt.Errorf("find inventory item batch folder: %w", err)
	}
	for index := range items {
		item := &items[index]
		err := tx.QueryRowContext(ctx, `INSERT INTO inventory_items
			(id, owner_user_id, creator_user_id, folder_id, asset_id, asset_type, inventory_type,
			 name, description, flags, base_permissions, current_permissions, everyone_permissions,
			 next_permissions, sale_type, sale_price)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)
			ON CONFLICT (id) DO NOTHING RETURNING created_at`, item.ID, item.OwnerUserID,
			creatorColumn(item), item.FolderID, item.AssetID, item.AssetType, item.InventoryType,
			item.Name, item.Description, item.Flags, item.BasePermissions, item.CurrentPermissions,
			item.EveryonePermissions, item.NextPermissions, item.SaleType, item.SalePrice).
			Scan(&item.CreatedAt)
		if errors.Is(err, sql.ErrNoRows) {
			return nil, nil, ErrItemConflict
		}
		if err != nil {
			return nil, nil, fmt.Errorf("create inventory item batch member: %w", err)
		}
	}
	var bumped int64
	if err := tx.QueryRowContext(ctx, `UPDATE inventory_folders
		SET version = version + $3, updated_at = now()
		WHERE id = $1 AND owner_user_id = $2
		RETURNING version`, folderID, ownerID, len(items)).Scan(&bumped); err != nil {
		return nil, nil, fmt.Errorf("update inventory item batch folder version: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return nil, nil, fmt.Errorf("commit inventory item batch creation: %w", err)
	}
	return items, FolderVersions{folderID: bumped}, nil
}

func (s *PostgresStore) UpdateItem(ctx context.Context, item Item) (Item, error) {
	item.Name = strings.TrimSpace(item.Name)
	if item.ID == "" || item.OwnerUserID == "" || item.FolderID == "" ||
		len(item.Name) == 0 || len(item.Name) > 255 || len(item.Description) > 1024 ||
		item.SaleType < 0 || item.SaleType > 3 || item.SalePrice < 0 {
		return Item{}, ErrInvalidItem
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Item{}, fmt.Errorf("begin inventory item update: %w", err)
	}
	defer tx.Rollback()
	var existing Item
	if err := tx.QueryRowContext(ctx, `SELECT id, owner_user_id, COALESCE(creator_user_id::text, $3),
		folder_id, asset_id, asset_type, inventory_type, name, description, flags, base_permissions,
		current_permissions, everyone_permissions, next_permissions, sale_type, sale_price, created_at
		FROM inventory_items WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`,
		item.ID, item.OwnerUserID, zeroUUID).Scan(&existing.ID, &existing.OwnerUserID,
		&existing.CreatorUserID, &existing.FolderID, &existing.AssetID, &existing.AssetType,
		&existing.InventoryType, &existing.Name, &existing.Description, &existing.Flags,
		&existing.BasePermissions, &existing.CurrentPermissions, &existing.EveryonePermissions,
		&existing.NextPermissions, &existing.SaleType, &existing.SalePrice, &existing.CreatedAt); errors.Is(err, sql.ErrNoRows) {
		return Item{}, ErrItemNotFound
	} else if err != nil {
		return Item{}, fmt.Errorf("find inventory item for update: %w", err)
	}
	if item.FolderID != existing.FolderID {
		var destinationVersion int64
		if err := tx.QueryRowContext(ctx, `SELECT version FROM inventory_folders
			WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`, item.FolderID, item.OwnerUserID).
			Scan(&destinationVersion); errors.Is(err, sql.ErrNoRows) {
			return Item{}, ErrItemFolderNotFound
		} else if err != nil {
			return Item{}, fmt.Errorf("find inventory item destination folder: %w", err)
		}
	}
	item.CreatorUserID = existing.CreatorUserID
	item.AssetID = existing.AssetID
	item.AssetType = existing.AssetType
	item.InventoryType = existing.InventoryType
	item.CreatedAt = existing.CreatedAt
	if item.FolderID == existing.FolderID && item.Name == existing.Name &&
		item.Description == existing.Description && item.Flags == existing.Flags &&
		item.BasePermissions == existing.BasePermissions &&
		item.CurrentPermissions == existing.CurrentPermissions &&
		item.EveryonePermissions == existing.EveryonePermissions &&
		item.NextPermissions == existing.NextPermissions && item.SaleType == existing.SaleType &&
		item.SalePrice == existing.SalePrice {
		return existing, nil
	}
	if _, err := tx.ExecContext(ctx, `UPDATE inventory_items SET folder_id = $3, name = $4,
		description = $5, flags = $6, base_permissions = $7, current_permissions = $8,
		everyone_permissions = $9, next_permissions = $10, sale_type = $11, sale_price = $12,
		updated_at = now() WHERE id = $1 AND owner_user_id = $2`, item.ID, item.OwnerUserID,
		item.FolderID, item.Name, item.Description, item.Flags, item.BasePermissions,
		item.CurrentPermissions, item.EveryonePermissions, item.NextPermissions,
		item.SaleType, item.SalePrice); err != nil {
		return Item{}, fmt.Errorf("update inventory item: %w", err)
	}
	for _, folderID := range []string{existing.FolderID, item.FolderID} {
		if _, err := tx.ExecContext(ctx, `UPDATE inventory_folders SET version = version + 1,
			updated_at = now() WHERE id = $1 AND owner_user_id = $2`, folderID, item.OwnerUserID); err != nil {
			return Item{}, fmt.Errorf("update inventory item folder version: %w", err)
		}
		if existing.FolderID == item.FolderID {
			break
		}
	}
	if err := tx.Commit(); err != nil {
		return Item{}, fmt.Errorf("commit inventory item update: %w", err)
	}
	return item, nil
}

func (s *PostgresStore) UpdateItemAsset(ctx context.Context, ownerID, itemID, assetID string) (Item, error) {
	if ownerID == "" || itemID == "" || assetID == "" {
		return Item{}, ErrInvalidItem
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Item{}, fmt.Errorf("begin inventory item asset update: %w", err)
	}
	defer tx.Rollback()
	var item Item
	if err := tx.QueryRowContext(ctx, `SELECT id, owner_user_id, COALESCE(creator_user_id::text, $3),
		folder_id, asset_id, asset_type, inventory_type, name, description, flags, base_permissions,
		current_permissions, everyone_permissions, next_permissions, sale_type, sale_price, created_at
		FROM inventory_items WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`,
		itemID, ownerID, zeroUUID).Scan(&item.ID, &item.OwnerUserID, &item.CreatorUserID,
		&item.FolderID, &item.AssetID, &item.AssetType, &item.InventoryType, &item.Name,
		&item.Description, &item.Flags, &item.BasePermissions, &item.CurrentPermissions,
		&item.EveryonePermissions, &item.NextPermissions, &item.SaleType, &item.SalePrice,
		&item.CreatedAt); errors.Is(err, sql.ErrNoRows) {
		return Item{}, ErrItemNotFound
	} else if err != nil {
		return Item{}, fmt.Errorf("find inventory item for asset update: %w", err)
	}
	editableType := ((item.AssetType == 5 || item.AssetType == 13) && item.InventoryType == 18) ||
		(item.AssetType == 7 && item.InventoryType == 7) ||
		(item.AssetType == 10 && item.InventoryType == 10) ||
		(item.AssetType == 21 && item.InventoryType == 20)
	if !editableType ||
		item.CurrentPermissions&0x00004000 == 0 {
		return Item{}, ErrInvalidItem
	}
	if item.AssetID == assetID {
		return item, nil
	}
	if _, err := tx.ExecContext(ctx, `UPDATE inventory_items SET asset_id = $3, updated_at = now()
		WHERE id = $1 AND owner_user_id = $2`, itemID, ownerID, assetID); err != nil {
		return Item{}, fmt.Errorf("update inventory item asset: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `UPDATE inventory_folders SET version = version + 1,
		updated_at = now() WHERE id = $1 AND owner_user_id = $2`, item.FolderID, ownerID); err != nil {
		return Item{}, fmt.Errorf("update inventory item asset folder version: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Item{}, fmt.Errorf("commit inventory item asset update: %w", err)
	}
	item.AssetID = assetID
	return item, nil
}

func (s *PostgresStore) DeleteItem(ctx context.Context, userID, itemID string) (Item, error) {
	if userID == "" || itemID == "" {
		return Item{}, ErrInvalidItem
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Item{}, fmt.Errorf("begin inventory item deletion: %w", err)
	}
	defer tx.Rollback()
	var item Item
	if err := tx.QueryRowContext(ctx, `DELETE FROM inventory_items
		WHERE id = $1 AND owner_user_id = $2 RETURNING id, owner_user_id,
		COALESCE(creator_user_id::text, $3), folder_id, asset_id, asset_type, inventory_type,
		name, description, flags, base_permissions, current_permissions, everyone_permissions,
		next_permissions, sale_type, sale_price, created_at`, itemID, userID, zeroUUID).
		Scan(&item.ID, &item.OwnerUserID, &item.CreatorUserID, &item.FolderID, &item.AssetID,
			&item.AssetType, &item.InventoryType, &item.Name, &item.Description, &item.Flags,
			&item.BasePermissions, &item.CurrentPermissions, &item.EveryonePermissions,
			&item.NextPermissions, &item.SaleType, &item.SalePrice, &item.CreatedAt); errors.Is(err, sql.ErrNoRows) {
		return Item{}, ErrItemNotFound
	} else if err != nil {
		return Item{}, fmt.Errorf("delete inventory item: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `UPDATE inventory_folders SET version = version + 1,
		updated_at = now() WHERE id = $1 AND owner_user_id = $2`, item.FolderID, userID); err != nil {
		return Item{}, fmt.Errorf("update deleted inventory item folder version: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Item{}, fmt.Errorf("commit inventory item deletion: %w", err)
	}
	return item, nil
}

func (s *PostgresStore) PurgeFolder(ctx context.Context, userID, folderID string) ([]string, []string, Folder, error) {
	if userID == "" || folderID == "" {
		return nil, nil, Folder{}, ErrInvalidFolder
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, nil, Folder{}, fmt.Errorf("begin inventory folder purge: %w", err)
	}
	defer tx.Rollback()
	var folder Folder
	if err := tx.QueryRowContext(ctx, `SELECT id, owner_user_id, COALESCE(parent_id::text, $3),
		name, type_default, version FROM inventory_folders
		WHERE id = $1 AND owner_user_id = $2 FOR UPDATE`, folderID, userID, zeroUUID).
		Scan(&folder.ID, &folder.OwnerUserID, &folder.ParentID, &folder.Name,
			&folder.TypeDefault, &folder.Version); errors.Is(err, sql.ErrNoRows) {
		return nil, nil, Folder{}, ErrFolderNotFound
	} else if err != nil {
		return nil, nil, Folder{}, fmt.Errorf("find inventory folder to purge: %w", err)
	}
	rows, err := tx.QueryContext(ctx, `WITH RECURSIVE tree AS (
		SELECT id FROM inventory_folders WHERE parent_id = $1 AND owner_user_id = $2
		UNION ALL
		SELECT child.id FROM inventory_folders child JOIN tree parent ON child.parent_id = parent.id
		WHERE child.owner_user_id = $2)
		SELECT 'folder', id FROM tree
		UNION ALL
		SELECT 'item', item.id FROM inventory_items item
		WHERE item.owner_user_id = $2 AND (item.folder_id = $1 OR item.folder_id IN (SELECT id FROM tree))`,
		folderID, userID)
	if err != nil {
		return nil, nil, Folder{}, fmt.Errorf("list inventory folder purge contents: %w", err)
	}
	var folderIDs, itemIDs []string
	for rows.Next() {
		var kind, id string
		if err := rows.Scan(&kind, &id); err != nil {
			rows.Close()
			return nil, nil, Folder{}, fmt.Errorf("scan inventory folder purge contents: %w", err)
		}
		if kind == "folder" {
			folderIDs = append(folderIDs, id)
		} else {
			itemIDs = append(itemIDs, id)
		}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, nil, Folder{}, fmt.Errorf("iterate inventory folder purge contents: %w", err)
	}
	if err := rows.Close(); err != nil {
		return nil, nil, Folder{}, fmt.Errorf("close inventory folder purge contents: %w", err)
	}
	var directCount int64
	if err := tx.QueryRowContext(ctx, `SELECT
		(SELECT count(*) FROM inventory_folders WHERE parent_id = $1 AND owner_user_id = $2) +
		(SELECT count(*) FROM inventory_items WHERE folder_id = $1 AND owner_user_id = $2)`,
		folderID, userID).Scan(&directCount); err != nil {
		return nil, nil, Folder{}, fmt.Errorf("count direct inventory purge contents: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM inventory_items
		WHERE folder_id = $1 AND owner_user_id = $2`, folderID, userID); err != nil {
		return nil, nil, Folder{}, fmt.Errorf("delete direct inventory purge items: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM inventory_folders
		WHERE parent_id = $1 AND owner_user_id = $2`, folderID, userID); err != nil {
		return nil, nil, Folder{}, fmt.Errorf("delete inventory purge folders: %w", err)
	}
	if directCount > 0 {
		if err := tx.QueryRowContext(ctx, `UPDATE inventory_folders SET version = version + $3,
			updated_at = now() WHERE id = $1 AND owner_user_id = $2 RETURNING version`,
			folderID, userID, directCount).Scan(&folder.Version); err != nil {
			return nil, nil, Folder{}, fmt.Errorf("update purged inventory folder version: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, nil, Folder{}, fmt.Errorf("commit inventory folder purge: %w", err)
	}
	return folderIDs, itemIDs, folder, nil
}

func (s *PostgresStore) ListItems(ctx context.Context, userID string) ([]Item, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT id, owner_user_id, COALESCE(creator_user_id::text, $2), folder_id, asset_id,
		       asset_type, inventory_type, name, description, flags, base_permissions,
		       current_permissions, everyone_permissions, next_permissions, sale_type, sale_price, created_at
		FROM inventory_items WHERE owner_user_id = $1 ORDER BY folder_id, name, id`, userID, zeroUUID)
	if err != nil {
		return nil, fmt.Errorf("list inventory items: %w", err)
	}
	defer rows.Close()
	var items []Item
	for rows.Next() {
		var item Item
		if err := rows.Scan(&item.ID, &item.OwnerUserID, &item.CreatorUserID, &item.FolderID, &item.AssetID,
			&item.AssetType, &item.InventoryType, &item.Name, &item.Description, &item.Flags,
			&item.BasePermissions, &item.CurrentPermissions, &item.EveryonePermissions, &item.NextPermissions,
			&item.SaleType, &item.SalePrice, &item.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan inventory item: %w", err)
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate inventory items: %w", err)
	}
	return items, nil
}

type folderQuery interface {
	QueryContext(context.Context, string, ...any) (*sql.Rows, error)
}

func listFolders(ctx context.Context, query folderQuery, userID string) ([]Folder, error) {
	rows, err := query.QueryContext(ctx, `
		SELECT id, owner_user_id, COALESCE(parent_id::text, $2), name, type_default, version
		FROM inventory_folders WHERE owner_user_id = $1
		ORDER BY parent_id NULLS FIRST, type_default, id`, userID, zeroUUID)
	if err != nil {
		return nil, fmt.Errorf("list inventory folders: %w", err)
	}
	defer rows.Close()
	var folders []Folder
	for rows.Next() {
		var folder Folder
		if err := rows.Scan(&folder.ID, &folder.OwnerUserID, &folder.ParentID, &folder.Name,
			&folder.TypeDefault, &folder.Version); err != nil {
			return nil, fmt.Errorf("scan inventory folder: %w", err)
		}
		folders = append(folders, folder)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate inventory folders: %w", err)
	}
	return folders, nil
}
