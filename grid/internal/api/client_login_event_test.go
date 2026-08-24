package api

import (
	"context"
	"errors"
	"net/http"
	"sync"
	"testing"

	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/presence"
)

// recordingEvents captures what the handler wrote. eventlog.Note records on a
// detached context, so the write can land after the response; the mutex is for
// the race detector rather than for any contention worth having.
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

// memoryPresence is who is in-world. Absent is ErrNotFound; err makes presence
// unreadable, which is a third answer and not the same as absent.
type memoryPresence struct {
	present map[string]presence.Presence
	err     error
}

func (m *memoryPresence) List(context.Context) ([]presence.Presence, error) {
	if m.err != nil {
		return nil, m.err
	}
	items := make([]presence.Presence, 0, len(m.present))
	for _, item := range m.present {
		items = append(items, item)
	}
	return items, nil
}

func (m *memoryPresence) Get(_ context.Context, userID string) (presence.Presence, error) {
	if m.err != nil {
		return presence.Presence{}, m.err
	}
	item, ok := m.present[userID]
	if !ok {
		return presence.Presence{}, presence.ErrNotFound
	}
	return item, nil
}

func loginHarness(t *testing.T, here *memoryPresence) (*worldEntryHarness, *recordingEvents) {
	t.Helper()
	events := &recordingEvents{}
	harness := newWorldEntryHarness(t, func(options *Options) {
		options.Presence = here
		options.Events = events
	})
	return harness, events
}

// TestClientWorldEntryRecordsALogin: the client had no login event at all, so
// every figure on the statistics page counted viewer logins and called them
// grid activity. A client that enters the world has logged in.
func TestClientWorldEntryRecordsALogin(t *testing.T) {
	harness, events := loginHarness(t, &memoryPresence{present: map[string]presence.Presence{}})

	if response := harness.open(t, `{}`, harness.token); response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}

	recorded := events.recorded()
	if len(recorded) != 1 {
		t.Fatalf("recorded %d events, want 1: %+v", len(recorded), recorded)
	}
	if recorded[0].Kind != eventlog.KindLogin || recorded[0].UserID != testUserID {
		t.Fatalf("unexpected event: %+v", recorded[0])
	}
	// The region is on the row, the same as on the viewer path, so "where do
	// people arrive" is answerable from the log rather than only "how many".
	if recorded[0].Detail != "Welcome" || recorded[0].RegionID == "" {
		t.Fatalf("event does not name the region: %+v", recorded[0])
	}
}

// TestRegionCrossingIsNotALogin is the half that makes the figure mean
// something. POST /v1/client/session is world entry and it is also what every
// region crossing calls, because the capability manifest is re-resolved per
// region. Recording each call would count an afternoon of border-hopping as a
// day of logins.
func TestRegionCrossingIsNotALogin(t *testing.T) {
	harness, events := loginHarness(t, &memoryPresence{present: map[string]presence.Presence{
		testUserID: {UserID: testUserID, RegionID: "aaaaaaaa-0000-4000-8000-000000000001"},
	}})

	// Already in-world, and now crossing into the region next door.
	if response := harness.open(t, `{"start":"Sandbox/10/20/30"}`, harness.token); response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
	if recorded := events.recorded(); len(recorded) != 0 {
		t.Fatalf("a crossing was recorded as a login: %+v", recorded)
	}
}

// TestUnreadablePresenceRecordsNoLogin: with presence unreadable, whether this
// is a login is unknown. An undercount is a gap somebody can look for; an
// overcount is a wrong number that reads as real.
func TestUnreadablePresenceRecordsNoLogin(t *testing.T) {
	harness, events := loginHarness(t, &memoryPresence{err: errors.New("presence is down")})

	// The session still opens: a login is not failed because the grid could
	// not decide whether to count it.
	if response := harness.open(t, `{}`, harness.token); response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
	if recorded := events.recorded(); len(recorded) != 0 {
		t.Fatalf("recorded a login it could not verify: %+v", recorded)
	}
}

// TestWorldEntryOutlivesAFailedEventWrite: the log is a record of what
// happened, never a gate on it.
func TestWorldEntryOutlivesAFailedEventWrite(t *testing.T) {
	harness, events := loginHarness(t, &memoryPresence{present: map[string]presence.Presence{}})
	events.err = errors.New("event log is down")

	if response := harness.open(t, `{}`, harness.token); response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
}
