package api

import (
	"net/http"
	"testing"

	"github.com/homeworldz/server/grid/internal/inventory"
)

// TestClientWorldEntryBootstrapsInventory: an account registered on the
// website and used only in the client reached a region with no system folders
// and nothing to wear. Viewer login has prepared both since inventory existed,
// so the fault was invisible to anybody who had ever opened Firestorm with the
// same account — that one login repaired it silently.
func TestClientWorldEntryBootstrapsInventory(t *testing.T) {
	store := &memoryInventoryStore{}
	harness := newWorldEntryHarness(t, func(options *Options) { options.Inventory = store })

	if response := harness.open(t, `{}`, harness.token); response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}

	if store.ensuredFolders != len(inventory.SystemFolders(testUserID)) {
		t.Fatalf("created %d system folders, want %d",
			store.ensuredFolders, len(inventory.SystemFolders(testUserID)))
	}
	if want := len(inventory.DefaultWearables(testUserID)); len(store.ensuredItems) != want {
		t.Fatalf("created %d default items, want %d", len(store.ensuredItems), want)
	}
	// The same ids viewer login would have produced. They are derived from the
	// user id, so an account that later opens Firestorm finds its own outfit
	// rather than a second one beside it.
	for index, item := range inventory.DefaultWearables(testUserID) {
		if store.ensuredItems[index].ID != item.ID {
			t.Fatalf("item %d = %q, want %q", index, store.ensuredItems[index].ID, item.ID)
		}
	}
}

// TestSecondWorldEntryWritesNothing: bootstrap runs on every world entry,
// which includes every region crossing. An established avatar must come out
// the other side untouched, or a crossing would rewrite the outfit somebody
// changed.
func TestSecondWorldEntryWritesNothing(t *testing.T) {
	store := &memoryInventoryStore{}
	harness := newWorldEntryHarness(t, func(options *Options) { options.Inventory = store })

	if response := harness.open(t, `{}`, harness.token); response.Code != http.StatusOK {
		t.Fatalf("first entry: %d", response.Code)
	}
	folders, items := store.ensuredFolders, len(store.ensuredItems)

	if response := harness.open(t, `{"start":"Sandbox/10/20/30"}`, harness.token); response.Code != http.StatusOK {
		t.Fatalf("second entry: %d", response.Code)
	}
	if store.ensuredFolders != folders || len(store.ensuredItems) != items {
		t.Fatalf("a crossing wrote inventory: folders %d->%d items %d->%d",
			folders, store.ensuredFolders, items, len(store.ensuredItems))
	}
}
