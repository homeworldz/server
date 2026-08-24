package api

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/stats"
)

type countingUsers struct {
	users int
	calls *int
	err   error
}

func (c countingUsers) CountUsers(context.Context) (int, error) {
	if c.calls != nil {
		*c.calls++
	}
	return c.users, c.err
}

type fixedLeases struct{ items []regions.Region }

func (f fixedLeases) List(context.Context) ([]regions.Region, error) { return f.items, nil }

type fixedPresence struct{ items []presence.Presence }

func (f fixedPresence) List(context.Context) ([]presence.Presence, error) { return f.items, nil }

// fixedEvents answers every count with the same number, which is enough to
// prove the figures reach the response body under the right names.
type fixedEvents struct {
	count    int
	distinct int
}

func (f fixedEvents) CountSince(context.Context, eventlog.Kind, time.Time) (int, error) {
	return f.count, nil
}

func (f fixedEvents) DistinctUsersSince(context.Context, eventlog.Kind, time.Time) (int, error) {
	return f.distinct, nil
}

func (f fixedEvents) LatestAt(context.Context, eventlog.Kind) (time.Time, error) {
	return time.Time{}, eventlog.ErrNoEvent
}

func testCollector(t *testing.T, users stats.UserCounter) *stats.Collector {
	t.Helper()
	collector, err := stats.NewCollector(stats.Sources{
		Users: users,
		Provisioned: &memoryRegionStore{items: []provisioning.Region{
			{ID: "online", SizeX: 2, SizeY: 2, Enabled: true},
			{ID: "offline", SizeX: 1, SizeY: 1, Enabled: true},
			{ID: "disabled", SizeX: 1, SizeY: 1},
		}},
		Leases:   fixedLeases{items: []regions.Region{{ID: "online"}}},
		Presence: fixedPresence{items: []presence.Presence{{UserID: "someone"}}},
		Events:   fixedEvents{count: 9, distinct: 4},
	})
	if err != nil {
		t.Fatal(err)
	}
	return collector
}

func TestGridStatsArePublic(t *testing.T) {
	handler := newTestAPI(t, func(options *Options) {
		options.Stats = testCollector(t, countingUsers{users: 11})
	})
	response := httptest.NewRecorder()
	// No Authorization header: the login page reads this before anyone has an
	// account to sign in with.
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/stats", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("status %d: %s", response.Code, response.Body.String())
	}
	var snapshot stats.Snapshot
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil {
		t.Fatal(err)
	}
	if snapshot.Users != 11 || snapshot.UsersOnline != 1 {
		t.Fatalf("users %d, online %d", snapshot.Users, snapshot.UsersOnline)
	}
	if snapshot.Regions != 2 || snapshot.RegionsOnline != 1 || snapshot.RegionsOffline != 1 ||
		snapshot.RegionsUndeployed != 1 || snapshot.RegionEquivalents != 5 {
		t.Fatalf("region figures: %+v", snapshot)
	}
	// Land in square metres is the same land in a unit somebody who has never
	// heard of a region equivalent can compare: five standard regions of
	// 256 m square. The disabled region is in neither figure.
	if snapshot.LandSquareMetres != 5*65536 {
		t.Fatalf("land = %d m2, want %d", snapshot.LandSquareMetres, 5*65536)
	}
	if snapshot.ActiveUsers30d != 4 || snapshot.ActiveUsers60d != 4 || snapshot.Logins24h != 9 {
		t.Fatalf("event figures: %+v", snapshot)
	}
	// No grid start has been recorded, so uptime is absent rather than zero.
	if snapshot.UptimeSeconds != nil || snapshot.GridStartedAt != nil {
		t.Fatalf("uptime reported without a recorded start: %+v", snapshot)
	}
	if response.Header().Get("Cache-Control") == "" {
		t.Fatal("public statistics should be cacheable")
	}
}

// TestGridStatsAreCached: the login page is the busiest unauthenticated page
// on the grid, and every load would otherwise count every account again.
func TestGridStatsAreCached(t *testing.T) {
	calls := 0
	handler := newTestAPI(t, func(options *Options) {
		options.Stats = testCollector(t, countingUsers{users: 11, calls: &calls})
	})
	for range 3 {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/stats", nil))
		if response.Code != http.StatusOK {
			t.Fatalf("status %d: %s", response.Code, response.Body.String())
		}
	}
	if calls != 1 {
		t.Fatalf("counted users %d times, want 1", calls)
	}
}

// TestGridStatsFailuresAreNotCached: a reading that failed must not be served
// for the life of the cache, and a failed count must never become a zero.
func TestGridStatsFailuresAreNotCached(t *testing.T) {
	calls := 0
	handler := newTestAPI(t, func(options *Options) {
		options.Stats = testCollector(t,
			countingUsers{calls: &calls, err: errors.New("database is down")})
	})
	for range 2 {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/stats", nil))
		if response.Code != http.StatusInternalServerError {
			t.Fatalf("status %d: %s", response.Code, response.Body.String())
		}
	}
	if calls != 2 {
		t.Fatalf("retried %d times, want 2", calls)
	}
}

// TestGridStatsWithoutSources: a deployment that cannot count says so.
func TestGridStatsWithoutSources(t *testing.T) {
	handler := newTestAPI(t)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/stats", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status %d: %s", response.Code, response.Body.String())
	}
}
