package attachments

import (
	"context"
	"database/sql"
	"fmt"
)

// Attachment is one worn item: the inventory item and the point it is worn on.
// The object asset is deliberately absent — the region resolves the item
// through inventory when it re-attaches, so this record cannot go stale against
// the item it names.
type Attachment struct {
	ItemID          string `json:"itemId"`
	AttachmentPoint int    `json:"attachmentPoint"`
}

type Store interface {
	ListWorn(ctx context.Context, userID string) ([]Attachment, error)
	Wear(ctx context.Context, userID, itemID string, point int) error
	TakeOff(ctx context.Context, userID, itemID string) error
}

type PostgresStore struct{ db *sql.DB }

func NewPostgresStore(db *sql.DB) *PostgresStore { return &PostgresStore{db: db} }

func (s *PostgresStore) ListWorn(ctx context.Context, userID string) ([]Attachment, error) {
	// Joined to inventory, and Trash excluded, for the reason written out in
	// the gestures store: worn_attachments has no foreign key to
	// inventory_items, so deleting a worn item leaves the wear behind. Deleting
	// from a viewer moves the item to Trash rather than removing the row, so
	// testing that the item merely exists would keep re-attaching something the
	// user has thrown away — at every login, with nothing left to take off.
	// Recursive because Trash holds folders as well as items.
	rows, err := s.db.QueryContext(ctx,
		`WITH RECURSIVE trashed AS (
		     SELECT id FROM inventory_folders
		      WHERE owner_user_id = $1 AND type_default = 14
		     UNION ALL
		     SELECT f.id FROM inventory_folders AS f
		       JOIN trashed AS t ON f.parent_id = t.id
		 )
		 SELECT a.item_id, a.attachment_point FROM worn_attachments AS a
		   JOIN inventory_items AS i ON i.id = a.item_id AND i.owner_user_id = a.user_id
		  WHERE a.user_id = $1 AND i.folder_id NOT IN (SELECT id FROM trashed)
		  ORDER BY a.worn_at`, userID)
	if err != nil {
		return nil, fmt.Errorf("list worn attachments: %w", err)
	}
	defer rows.Close()
	var result []Attachment
	for rows.Next() {
		var worn Attachment
		if err := rows.Scan(&worn.ItemID, &worn.AttachmentPoint); err != nil {
			return nil, fmt.Errorf("scan worn attachment: %w", err)
		}
		result = append(result, worn)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate worn attachments: %w", err)
	}
	return result, nil
}

// Wear records an item as worn, moving it if it already is: the same item worn
// again is one attachment on a new point, never two.
func (s *PostgresStore) Wear(ctx context.Context, userID, itemID string, point int) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO worn_attachments (user_id, item_id, attachment_point) VALUES ($1, $2, $3)
		 ON CONFLICT (user_id, item_id)
		 DO UPDATE SET attachment_point = EXCLUDED.attachment_point, worn_at = now()`,
		userID, itemID, point)
	if err != nil {
		return fmt.Errorf("wear attachment: %w", err)
	}
	return nil
}

func (s *PostgresStore) TakeOff(ctx context.Context, userID, itemID string) error {
	_, err := s.db.ExecContext(ctx,
		`DELETE FROM worn_attachments WHERE user_id = $1 AND item_id = $2`, userID, itemID)
	if err != nil {
		return fmt.Errorf("take off attachment: %w", err)
	}
	return nil
}
