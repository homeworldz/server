package stats

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

// The trailing windows the public figures are quoted over. Thirty and sixty
// days are what a person comparing grids expects to see, because they are what
// every other grid's statistics page publishes.
const (
	activeWindow     = 30 * 24 * time.Hour
	longActiveWindow = 60 * 24 * time.Hour
	recentWindow     = 24 * time.Hour
)

// standardRegionArea is a standard region in square metres: 256 m on a side.
// Region sizes are already counted in standard regions, so this converts the
// whole grid's land in one multiplication.
const standardRegionArea = 256 * 256

// UserCounter answers how many user accounts exist. Satisfied by the identity
// store's Postgres implementation; narrow so tests need not build the whole
// store.
type UserCounter interface {
	CountUsers(context.Context) (int, error)
}

// RegionLister is the slice of the provisioning store the collector needs:
// every region the operator has defined, running or not.
type RegionLister interface {
	List(context.Context) ([]provisioning.Region, error)
}

// LeaseLister lists regions holding a live lease — the ones actually running.
// The lease store's List already excludes expired leases.
type LeaseLister interface {
	List(context.Context) ([]regions.Region, error)
}

// PresenceLister lists avatars currently in-world.
type PresenceLister interface {
	List(context.Context) ([]presence.Presence, error)
}

// Sources are the five stores a snapshot is assembled from. All are required:
// a snapshot with a missing source would have to report zero for figures it
// could not read, and a zero that means "unavailable" is the one thing a
// statistics page must never publish.
type Sources struct {
	Users       UserCounter
	Provisioned RegionLister
	Leases      LeaseLister
	Presence    PresenceLister
	Events      eventlog.Counter
}

// Collector reads one Snapshot per call. It holds no cache of its own: the
// caller that needs one (the public endpoint, answering every page load)
// caches the Snapshot, and the caller that must not (the daily row) does not.
type Collector struct {
	sources Sources
	// now is replaceable by tests; every window is measured from it.
	now func() time.Time
}

func NewCollector(sources Sources) (*Collector, error) {
	switch {
	case sources.Users == nil:
		return nil, errors.New("stats: a user counter is required")
	case sources.Provisioned == nil:
		return nil, errors.New("stats: a provisioned region lister is required")
	case sources.Leases == nil:
		return nil, errors.New("stats: a region lease lister is required")
	case sources.Presence == nil:
		return nil, errors.New("stats: a presence lister is required")
	case sources.Events == nil:
		return nil, errors.New("stats: an event counter is required")
	}
	return &Collector{sources: sources, now: time.Now}, nil
}

// Snapshot is one reading of the grid, published at /v1/stats and recorded as
// one row of the daily CSV.
//
// Every count is a real count taken at CapturedAt. A source that fails takes
// the whole snapshot with it rather than contributing a zero, because a zero
// here reads as an exodus rather than as a database that was unreachable.
type Snapshot struct {
	CapturedAt time.Time `json:"capturedAt"`
	// Users is every account that exists; UsersOnline is how many avatars are
	// in-world at this instant.
	Users       int `json:"users"`
	UsersOnline int `json:"usersOnline"`
	// ActiveUsers30d and ActiveUsers60d are distinct people who logged in
	// during the trailing window — the figure other grids publish as their
	// monthly active users. Someone who logged in forty times counts once.
	ActiveUsers30d int `json:"activeUsers30d"`
	ActiveUsers60d int `json:"activeUsers60d"`
	// Logins24h and Logins30d count logins rather than people.
	Logins24h        int `json:"logins24h"`
	Logins30d        int `json:"logins30d"`
	Registrations30d int `json:"registrations30d"`
	// Teleports24h and Crossings24h are counted apart because a border
	// crossing is not a journey anyone chose to make, and a busy border would
	// otherwise read as heavy teleport traffic.
	Teleports24h int `json:"teleports24h"`
	Crossings24h int `json:"crossings24h"`
	// Regions is every enabled region, running or not — the grid's advertised
	// land. RegionsOnline holds a live lease and RegionsOffline does not.
	// RegionsUndeployed counts defined-but-disabled regions, which are part
	// of neither Regions nor RegionEquivalents and are advertised nowhere.
	Regions           int `json:"regions"`
	RegionsOnline     int `json:"regionsOnline"`
	RegionsOffline    int `json:"regionsOffline"`
	RegionsUndeployed int `json:"regionsUndeployed"`
	// RegionEquivalents is enabled land in 256 m x 256 m standard regions, so
	// a 4x2 rectangle is eight.
	RegionEquivalents int `json:"regionEquivalents"`
	// LandSquareMetres is that same land in square metres, which is the unit
	// a person who has never heard of a region equivalent can compare against
	// a map or another grid. Derived, not measured: a standard region is
	// 256 m square, so it is exactly RegionEquivalents x 65536. Published
	// because deriving it is only obvious once somebody has been told the
	// region size, and a statistics page should not need a footnote.
	LandSquareMetres int64 `json:"landSquareMetres"`
	// GridStartedAt is the most recent recorded grid start and UptimeSeconds
	// the interval since. Both are absent — not zero — on a grid whose log
	// carries no start yet, which is any grid that has not restarted since
	// the event log was added.
	GridStartedAt *time.Time `json:"gridStartedAt,omitempty"`
	UptimeSeconds *int64     `json:"uptimeSeconds,omitempty"`
}

// Collect reads every source and returns the assembled snapshot, or the first
// failure.
func (c *Collector) Collect(ctx context.Context) (Snapshot, error) {
	at := c.now().UTC()
	snapshot := Snapshot{CapturedAt: at}

	users, err := c.sources.Users.CountUsers(ctx)
	if err != nil {
		return Snapshot{}, fmt.Errorf("count users: %w", err)
	}
	snapshot.Users = users

	online, err := c.sources.Presence.List(ctx)
	if err != nil {
		return Snapshot{}, fmt.Errorf("list presence: %w", err)
	}
	snapshot.UsersOnline = len(online)

	if err := c.collectRegions(ctx, &snapshot); err != nil {
		return Snapshot{}, err
	}
	if err := c.collectEvents(ctx, at, &snapshot); err != nil {
		return Snapshot{}, err
	}
	return snapshot, nil
}

// collectRegions counts defined regions by state and totals enabled land in
// standard-region chunks: a provisioned size is already in 256 m units, so a
// 4x2 rectangle is eight equivalents.
func (c *Collector) collectRegions(ctx context.Context, snapshot *Snapshot) error {
	defined, err := c.sources.Provisioned.List(ctx)
	if err != nil {
		return fmt.Errorf("list provisioned regions: %w", err)
	}
	leased, err := c.sources.Leases.List(ctx)
	if err != nil {
		return fmt.Errorf("list region leases: %w", err)
	}
	live := make(map[string]bool, len(leased))
	for _, region := range leased {
		live[region.ID] = true
	}
	for _, region := range defined {
		if !region.Enabled {
			snapshot.RegionsUndeployed++
			continue
		}
		snapshot.Regions++
		snapshot.RegionEquivalents += region.SizeX * region.SizeY
		if live[region.ID] {
			snapshot.RegionsOnline++
		} else {
			snapshot.RegionsOffline++
		}
	}
	snapshot.LandSquareMetres = int64(snapshot.RegionEquivalents) * standardRegionArea
	return nil
}

// collectEvents fills in every figure that comes from the event log, and the
// uptime the most recent recorded grid start implies.
func (c *Collector) collectEvents(ctx context.Context, at time.Time, snapshot *Snapshot) error {
	counts := []struct {
		into     *int
		kind     eventlog.Kind
		since    time.Time
		distinct bool
	}{
		{&snapshot.ActiveUsers30d, eventlog.KindLogin, at.Add(-activeWindow), true},
		{&snapshot.ActiveUsers60d, eventlog.KindLogin, at.Add(-longActiveWindow), true},
		{&snapshot.Logins24h, eventlog.KindLogin, at.Add(-recentWindow), false},
		{&snapshot.Logins30d, eventlog.KindLogin, at.Add(-activeWindow), false},
		{&snapshot.Registrations30d, eventlog.KindRegistration, at.Add(-activeWindow), false},
		{&snapshot.Teleports24h, eventlog.KindTeleport, at.Add(-recentWindow), false},
		{&snapshot.Crossings24h, eventlog.KindCrossing, at.Add(-recentWindow), false},
	}
	for _, count := range counts {
		var value int
		var err error
		if count.distinct {
			value, err = c.sources.Events.DistinctUsersSince(ctx, count.kind, count.since)
		} else {
			value, err = c.sources.Events.CountSince(ctx, count.kind, count.since)
		}
		if err != nil {
			return err
		}
		*count.into = value
	}

	started, err := c.sources.Events.LatestAt(ctx, eventlog.KindGridStart)
	if errors.Is(err, eventlog.ErrNoEvent) {
		return nil
	}
	if err != nil {
		return err
	}
	// A start recorded ahead of the reading — a clock adjustment, or a
	// snapshot taken by a host whose clock trails the grid's — would publish a
	// negative uptime. Report the start and no duration rather than a nonsense
	// one.
	snapshot.GridStartedAt = &started
	if seconds := int64(at.Sub(started) / time.Second); seconds >= 0 {
		snapshot.UptimeSeconds = &seconds
	}
	return nil
}
