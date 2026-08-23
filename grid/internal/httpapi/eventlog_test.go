package httpapi

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

// recordingEvents captures what the grid recorded. Note writes on a detached
// context from the handler goroutine, so the mutex is not decoration.
type recordingEvents struct {
	mu     sync.Mutex
	events []eventlog.Event
	err    error
}

func (r *recordingEvents) Record(_ context.Context, event eventlog.Event) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.err != nil {
		return r.err
	}
	r.events = append(r.events, event)
	return nil
}

func (r *recordingEvents) recorded() []eventlog.Event {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]eventlog.Event(nil), r.events...)
}

func (r *recordingEvents) only(t *testing.T, kind eventlog.Kind) eventlog.Event {
	t.Helper()
	var found []eventlog.Event
	for _, event := range r.recorded() {
		if event.Kind == kind {
			found = append(found, event)
		}
	}
	if len(found) != 1 {
		t.Fatalf("recorded %d %s events, want 1: %#v", len(found), kind, r.recorded())
	}
	return found[0]
}

// TestViewerLoginIsRecorded: the login figures on the statistics page count
// these rows, so a login that reached a region must produce exactly one, and a
// login that failed must produce none.
func TestViewerLoginIsRecorded(t *testing.T) {
	identities := newMemoryIdentityStore()
	user, err := identities.CreateUser(context.Background(), "test.user", "development-password")
	if err != nil {
		t.Fatal(err)
	}
	regionStore := newMemoryRegionStore()
	target, err := regionStore.Register(context.Background(), regions.Registration{
		Name: "Welcome", GridX: 1001, GridY: 1002,
		PublicEndpoint: "http://127.0.0.1:42001", ViewerPort: 43002, LeaseDuration: time.Minute,
	})
	if err != nil {
		t.Fatal(err)
	}
	provisionedPath := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(provisionedPath, []byte(fmt.Sprintf(
		`[{"id":%q,"name":"Welcome","mapX":1001,"mapY":1002,"accessKey":"welcome-key"}]`, target.ID)), 0600); err != nil {
		t.Fatal(err)
	}
	provisioned, err := provisioning.Load(provisionedPath)
	if err != nil {
		t.Fatal(err)
	}
	events := &recordingEvents{}
	handler := New(checker{}, "test", Options{
		Identity: identities, Regions: regionStore, Provisioned: provisioned,
		Inventory: &memoryInventoryStore{folders: make(map[string][]inventory.Folder)},
		Events:    events,
	})

	fields := viewerResponse(t, handler, viewerRequest("Test", "User", "development-password", "last"))
	if fields["login"].text() != "true" {
		t.Fatalf("login failed: %q %q", fields["reason"].text(), fields["message"].text())
	}
	login := events.only(t, eventlog.KindLogin)
	if login.UserID != user.ID || login.RegionID != target.ID || login.Detail != "Welcome" {
		t.Fatalf("login event = %#v", login)
	}

	// A refused password is not a login and must not be counted as one.
	before := len(events.recorded())
	fields = viewerResponse(t, handler, viewerRequest("Test", "User", "wrong-password", "last"))
	if fields["login"].text() == "true" {
		t.Fatal("expected the wrong password to be refused")
	}
	if after := len(events.recorded()); after != before {
		t.Fatalf("a failed login recorded %d events", after-before)
	}
}

// TestClearedPresenceIsRecordedAsLogout: clearing presence is the grid's only
// account of an avatar leaving, and a clear that found nothing to clear is not
// a departure.
func TestClearedPresenceIsRecordedAsLogout(t *testing.T) {
	store := newMemoryPresenceStore()
	events := &recordingEvents{}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Presence: store, Events: events})
	const userID = "20000000-0000-4000-8000-000000000001"
	const regionID = "30000000-0000-4000-8000-000000000001"

	requestRegion[Error](t, handler, http.MethodDelete, "/api/v1/presence/"+userID, "", http.StatusNotFound)
	if recorded := events.recorded(); len(recorded) != 0 {
		t.Fatalf("clearing nothing recorded %#v", recorded)
	}

	requestRegion[any](t, handler, http.MethodPut, "/api/v1/presence/"+userID,
		`{"regionId":"`+regionID+`"}`, http.StatusOK)
	requestRegion[any](t, handler, http.MethodDelete, "/api/v1/presence/"+userID, "", http.StatusNoContent)
	if logout := events.only(t, eventlog.KindLogout); logout.UserID != userID {
		t.Fatalf("logout event = %#v", logout)
	}
}

// TestTransitKindDecidesTheEvent: teleports and border crossings are the same
// request with a different reason, and the statistics report them apart.
func TestTransitKindDecidesTheEvent(t *testing.T) {
	for name, expected := range map[string]eventlog.Kind{
		`,"kind":"teleport"`: eventlog.KindTeleport,
		`,"kind":"crossing"`: eventlog.KindCrossing,
		"":                   eventlog.KindTransit,
	} {
		t.Run(string(expected), func(t *testing.T) {
			identities := newMemoryIdentityStore()
			user, err := identities.CreateUser(context.Background(), "test.user", "development-password")
			if err != nil {
				t.Fatal(err)
			}
			session, err := identities.CreateSession(context.Background(), "test.user",
				"development-password", time.Hour)
			if err != nil {
				t.Fatal(err)
			}
			const source = "11111111-1111-4111-8111-111111111111"
			const destination = "22222222-2222-4222-8222-222222222222"
			if err := identities.AssignViewerDestination(context.Background(), session.ID, 1234, source); err != nil {
				t.Fatal(err)
			}
			regionStore := newMemoryRegionStore()
			for _, item := range []struct {
				id, name string
				x        int
			}{{source, "Welcome", 1000}, {destination, "Sandbox", 1001}} {
				if _, err := regionStore.RegisterProvisioned(context.Background(), item.id, regions.Registration{
					Name: item.name, GridX: item.x, GridY: 1000, PublicEndpoint: "http://region.example",
					ViewerPort: 42002, LeaseDuration: time.Minute,
				}); err != nil {
					t.Fatal(err)
				}
			}
			events := &recordingEvents{}
			handler := New(checker{}, "test", Options{
				ServiceToken: "secret", Identity: identities, Regions: regionStore,
				Transits: newMemoryTransitStore(identities.now), Events: events,
			})
			body := `{"id":"33333333-3333-4333-8333-333333333333","agentId":"` + user.ID +
				`","sessionId":"` + session.ID + `","sourceRegionId":"` + source +
				`","destinationRegionId":"` + destination +
				`","position":{"x":200,"y":184,"z":30},"lookAt":{"x":1,"y":0,"z":0},"flying":true` +
				name + `}`
			requestRegion[any](t, handler, http.MethodPost, "/api/v1/transits", body, http.StatusOK)
			event := events.only(t, expected)
			if event.UserID != user.ID || event.RegionID != destination {
				t.Fatalf("%s event = %#v", expected, event)
			}
		})
	}
}
