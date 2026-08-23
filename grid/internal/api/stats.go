package api

import (
	"context"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/homeworldz/server/grid/internal/stats"
)

// statsCacheTTL is how long one reading is reused. The figures are counted
// over days and months, so a reading half a minute old is indistinguishable
// from a fresh one — and this endpoint answers on every load of the login
// page, which is the one place a public grid gets hammered by people who are
// not logged in yet.
const statsCacheTTL = 30 * time.Second

// statsCache holds the last reading and hands it out until it expires. A
// failed reading is not cached: the next request tries again rather than
// serving the failure for half a minute.
type statsCache struct {
	mu       sync.Mutex
	snapshot stats.Snapshot
	takenAt  time.Time
	// now is replaceable by tests.
	now func() time.Time
}

func newStatsCache() *statsCache { return &statsCache{now: time.Now} }

func (c *statsCache) get(ctx context.Context, collector *stats.Collector) (stats.Snapshot, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !c.takenAt.IsZero() && c.now().Sub(c.takenAt) < statsCacheTTL {
		return c.snapshot, nil
	}
	snapshot, err := collector.Collect(ctx)
	if err != nil {
		return stats.Snapshot{}, err
	}
	c.snapshot, c.takenAt = snapshot, c.now()
	return snapshot, nil
}

// gridStats handles GET /v1/stats: the public grid statistics, unauthenticated
// because they are published on the login page to people who have no account
// yet.
//
// Every figure is a count actually taken; a deployment without the stores to
// take them answers 503 rather than a page of zeros.
func (a *API) gridStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		methodNotAllowed(w, http.MethodGet)
		return
	}
	if a.stats == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "stats_unavailable", Message: "grid statistics are unavailable"})
		return
	}
	snapshot, err := a.statsCache.get(r.Context(), a.stats)
	if err != nil {
		a.internalError(w, r, "collect grid statistics", err)
		return
	}
	// Shared caches may hold this as long as the process does; the numbers are
	// public and a browser refreshing the login page need not re-count them.
	w.Header().Set("Cache-Control", "public, max-age="+strconv.Itoa(int(statsCacheTTL/time.Second)))
	writeJSON(w, http.StatusOK, snapshot)
}
