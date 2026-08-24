package inventory

import (
	"context"
	"errors"
	"testing"
)

// fakeBootstrapStore is the four calls Bootstrap makes, and a record of what
// it wrote. failOn makes one call fail, which is how the "nothing half-done"
// property below gets tested.
type fakeBootstrapStore struct {
	folders []Folder
	items   []Item
	ensured []Item
	calls   []string
	failOn  string
}

var errStoreDown = errors.New("store is down")

func (f *fakeBootstrapStore) note(call string) error {
	f.calls = append(f.calls, call)
	if f.failOn == call {
		return errStoreDown
	}
	return nil
}

func (f *fakeBootstrapStore) EnsureSystemFolders(_ context.Context, userID string) ([]Folder, error) {
	if err := f.note("EnsureSystemFolders"); err != nil {
		return nil, err
	}
	f.folders = SystemFolders(userID)
	return f.folders, nil
}

func (f *fakeBootstrapStore) ListItems(context.Context, string) ([]Item, error) {
	if err := f.note("ListItems"); err != nil {
		return nil, err
	}
	return f.items, nil
}

func (f *fakeBootstrapStore) EnsureItem(_ context.Context, item Item) (bool, error) {
	if err := f.note("EnsureItem"); err != nil {
		return false, err
	}
	f.ensured = append(f.ensured, item)
	f.items = append(f.items, item)
	return true, nil
}

func (f *fakeBootstrapStore) ListFolders(context.Context, string) ([]Folder, error) {
	if err := f.note("ListFolders"); err != nil {
		return nil, err
	}
	return f.folders, nil
}

const bootstrapUser = "77777777-7777-4777-8777-777777777777"

// TestBootstrapDressesANewAvatar is the whole reason this exists: an account
// that has never entered the world gets its folder skeleton and a default
// outfit. Client world entry did none of this until 2026-08-24.
func TestBootstrapDressesANewAvatar(t *testing.T) {
	store := &fakeBootstrapStore{}
	folders, err := Bootstrap(context.Background(), store, bootstrapUser)
	if err != nil {
		t.Fatal(err)
	}
	if len(folders) != len(SystemFolders(bootstrapUser)) {
		t.Fatalf("returned %d folders, want %d", len(folders), len(SystemFolders(bootstrapUser)))
	}
	if want := DefaultWearables(bootstrapUser); len(store.ensured) != len(want) {
		t.Fatalf("wrote %d items, want %d", len(store.ensured), len(want))
	}
}

// TestBootstrapLeavesAnEstablishedAvatarAlone: this runs on every world entry,
// and on the client that includes every region crossing. Rewriting the default
// outfit there would undo whatever the person is actually wearing.
func TestBootstrapLeavesAnEstablishedAvatarAlone(t *testing.T) {
	store := &fakeBootstrapStore{items: DefaultWearables(bootstrapUser)}
	if _, err := Bootstrap(context.Background(), store, bootstrapUser); err != nil {
		t.Fatal(err)
	}
	if len(store.ensured) != 0 {
		t.Fatalf("wrote %d items for an established avatar", len(store.ensured))
	}
	// And it does not re-read what it did not change: EnsureSystemFolders
	// already returned the user's whole folder list.
	for _, call := range store.calls {
		if call == "ListFolders" {
			t.Fatalf("re-read folders after writing nothing: %v", store.calls)
		}
	}
}

// TestBootstrapRepairsAnInterruptedInitialization covers the narrow case the
// viewer path carried: the wearables exist but the links into the outfit
// folder do not, because initialization died between the two. Only the links
// are rewritten, so an avatar that has since edited its shape keeps the edit.
func TestBootstrapRepairsAnInterruptedInitialization(t *testing.T) {
	defaults := DefaultWearables(bootstrapUser)
	parts := make([]Item, 0, len(defaults)/2)
	for index := 0; index < len(defaults); index += 2 {
		parts = append(parts, defaults[index])
	}
	store := &fakeBootstrapStore{items: parts}
	if !DefaultOutfitNeedsRepair(bootstrapUser, parts) {
		t.Fatal("the fixture is not the case this test is about")
	}

	if _, err := Bootstrap(context.Background(), store, bootstrapUser); err != nil {
		t.Fatal(err)
	}
	if len(store.ensured) != len(defaults)/2 {
		t.Fatalf("repaired %d items, want %d", len(store.ensured), len(defaults)/2)
	}
	for index, item := range store.ensured {
		if item.ID != defaults[index*2+1].ID {
			t.Fatalf("repair %d wrote %q, want the link %q", index, item.ID, defaults[index*2+1].ID)
		}
	}
}

// TestBootstrapWithoutAStore: a deployment serving no inventory gets the
// deterministic skeleton rather than an error. Those ids are derived from the
// user id, so they are the ids a store would have created.
func TestBootstrapWithoutAStore(t *testing.T) {
	folders, err := Bootstrap(context.Background(), nil, bootstrapUser)
	if err != nil {
		t.Fatal(err)
	}
	if len(folders) != len(SystemFolders(bootstrapUser)) {
		t.Fatalf("returned %d folders", len(folders))
	}
}

// TestBootstrapReportsWhichStepFailed: both callers turn this into a refused
// login, and the operator's only clue is what the error says.
func TestBootstrapReportsWhichStepFailed(t *testing.T) {
	for _, failing := range []string{"EnsureSystemFolders", "ListItems", "EnsureItem", "ListFolders"} {
		store := &fakeBootstrapStore{failOn: failing}
		folders, err := Bootstrap(context.Background(), store, bootstrapUser)
		if err == nil {
			t.Fatalf("%s failed and Bootstrap did not", failing)
		}
		if !errors.Is(err, errStoreDown) {
			t.Fatalf("%s: error does not wrap the cause: %v", failing, err)
		}
		if folders != nil {
			t.Fatalf("%s: returned folders alongside an error", failing)
		}
	}
}
