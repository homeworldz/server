package stats

import (
	"context"
	"errors"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

type fixedCounts struct {
	users int
	err   error
}

func (f fixedCounts) CountUsers(context.Context) (int, error) { return f.users, f.err }

type fixedRegions struct {
	regions []provisioning.Region
	err     error
}

func (f fixedRegions) List(context.Context) ([]provisioning.Region, error) {
	return f.regions, f.err
}

type fixedLeases struct {
	regions []regions.Region
	err     error
}

func (f fixedLeases) List(context.Context) ([]regions.Region, error) { return f.regions, f.err }

type fixedPresence struct {
	presences []presence.Presence
	err       error
}

func (f fixedPresence) List(context.Context) ([]presence.Presence, error) {
	return f.presences, f.err
}

// fixedEvents answers the collector's questions from a table keyed by kind,
// recording the windows it was asked about so a test can assert on them.
type fixedEvents struct {
	counts   map[eventlog.Kind]int
	distinct map[eventlog.Kind]int
	started  time.Time
	hasStart bool
	err      error
	asked    map[eventlog.Kind]time.Time
}

func (f *fixedEvents) CountSince(_ context.Context, kind eventlog.Kind, since time.Time) (int, error) {
	if f.err != nil {
		return 0, f.err
	}
	f.remember(kind, since)
	return f.counts[kind], nil
}

func (f *fixedEvents) DistinctUsersSince(_ context.Context, kind eventlog.Kind, since time.Time) (int, error) {
	if f.err != nil {
		return 0, f.err
	}
	f.remember(kind, since)
	return f.distinct[kind], nil
}

func (f *fixedEvents) LatestAt(_ context.Context, kind eventlog.Kind) (time.Time, error) {
	if f.err != nil {
		return time.Time{}, f.err
	}
	if kind == eventlog.KindGridStart && f.hasStart {
		return f.started, nil
	}
	return time.Time{}, eventlog.ErrNoEvent
}

func (f *fixedEvents) remember(kind eventlog.Kind, since time.Time) {
	if f.asked == nil {
		f.asked = map[eventlog.Kind]time.Time{}
	}
	// Distinct-user and plain counts share a kind; the widest window asked
	// about is the one worth remembering.
	if previous, seen := f.asked[kind]; !seen || since.Before(previous) {
		f.asked[kind] = since
	}
}

func newTestCollector(t *testing.T, sources Sources) *Collector {
	t.Helper()
	if sources.Users == nil {
		sources.Users = fixedCounts{}
	}
	if sources.Provisioned == nil {
		sources.Provisioned = fixedRegions{}
	}
	if sources.Leases == nil {
		sources.Leases = fixedLeases{}
	}
	if sources.Presence == nil {
		sources.Presence = fixedPresence{}
	}
	if sources.Events == nil {
		sources.Events = &fixedEvents{}
	}
	collector, err := NewCollector(sources)
	if err != nil {
		t.Fatal(err)
	}
	return collector
}

func newTestRecorder(t *testing.T, sources Sources) *Recorder {
	t.Helper()
	recorder, err := New(filepath.Join(t.TempDir(), "stats.csv"),
		newTestCollector(t, sources), nil)
	if err != nil {
		t.Fatal(err)
	}
	return recorder
}

// easternTime builds an instant whose US-Eastern wall-clock reading is the
// given values, so the schedule is tested in the zone it is defined in.
func easternTime(t *testing.T, year int, month time.Month, day, hour, minute int) time.Time {
	t.Helper()
	location, err := time.LoadLocation("America/New_York")
	if err != nil {
		t.Fatal(err)
	}
	return time.Date(year, month, day, hour, minute, 0, 0, location)
}

// at moves both clocks together: the recorder decides whether a row is due,
// the collector stamps the snapshot and measures every window.
func at(recorder *Recorder, moment time.Time) {
	recorder.now = func() time.Time { return moment }
	recorder.collector.now = func() time.Time { return moment }
}

func testSources() Sources {
	return Sources{
		Users: fixedCounts{users: 7},
		Provisioned: fixedRegions{regions: []provisioning.Region{
			{ID: "a", Name: "Welcome", SizeX: 1, SizeY: 1, Enabled: true},
			{ID: "b", Name: "Sandbox", SizeX: 1, SizeY: 2, Enabled: true},
			{ID: "c", Name: "Delta", SizeX: 4, SizeY: 2, Enabled: true},
			{ID: "d", Name: "Mothballed", SizeX: 4, SizeY: 4, Enabled: false},
		}},
		Leases:   fixedLeases{regions: []regions.Region{{ID: "a"}, {ID: "c"}}},
		Presence: fixedPresence{presences: []presence.Presence{{UserID: "1"}, {UserID: "2"}}},
		Events: &fixedEvents{
			counts: map[eventlog.Kind]int{
				eventlog.KindLogin: 12, eventlog.KindRegistration: 3,
				eventlog.KindTeleport: 40, eventlog.KindCrossing: 900,
			},
			distinct: map[eventlog.Kind]int{eventlog.KindLogin: 5},
		},
	}
}

func TestRecordsOnceDailyAfterSix(t *testing.T) {
	recorder := newTestRecorder(t, testSources())

	// Before six: nothing is due.
	at(recorder, easternTime(t, 2026, time.August, 21, 5, 59))
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(recorder.path); !os.IsNotExist(err) {
		t.Fatalf("row recorded before six: %v", err)
	}

	// After six: header plus the day's row. Disabled regions count only as
	// undeployed, and with no recorded grid start the uptime field is empty
	// rather than zero.
	at(recorder, easternTime(t, 2026, time.August, 21, 6, 0))
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	want := header + "\n260821-0600,7,3,11,2,5,5,12,12,3,40,900,2,1,,720896\n"
	if string(content) != want {
		t.Fatalf("recorded %q, want %q", content, want)
	}

	// Later the same day: still one row.
	at(recorder, easternTime(t, 2026, time.August, 21, 18, 30))
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	// The next day appends; a late start records at the actual time.
	at(recorder, easternTime(t, 2026, time.August, 22, 9, 15))
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	content, err = os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	want += "260822-0915,7,3,11,2,5,5,12,12,3,40,900,2,1,,720896\n"
	if string(content) != want {
		t.Fatalf("after two days %q, want %q", content, want)
	}
}

func TestFailedCountsRecordNothing(t *testing.T) {
	for name, sources := range map[string]Sources{
		"users":    {Users: fixedCounts{err: errors.New("database is down")}},
		"presence": {Presence: fixedPresence{err: errors.New("database is down")}},
		"leases":   {Leases: fixedLeases{err: errors.New("database is down")}},
		"events":   {Events: &fixedEvents{err: errors.New("database is down")}},
	} {
		t.Run(name, func(t *testing.T) {
			recorder := newTestRecorder(t, sources)
			at(recorder, easternTime(t, 2026, time.August, 21, 6, 5))
			if err := recorder.RecordIfDue(context.Background()); err == nil {
				t.Fatal("expected the failed count to surface")
			}
			if _, err := os.Stat(recorder.path); !os.IsNotExist(err) {
				t.Fatal("a failed count must not record a row")
			}
		})
	}
}

// TestWindowsAreMeasuredFromTheReading pins the trailing windows the public
// figures are quoted over, which are the whole meaning of the columns.
func TestWindowsAreMeasuredFromTheReading(t *testing.T) {
	sources := testSources()
	events := sources.Events.(*fixedEvents)
	collector := newTestCollector(t, sources)
	moment := time.Date(2026, time.August, 21, 12, 0, 0, 0, time.UTC)
	collector.now = func() time.Time { return moment }
	if _, err := collector.Collect(context.Background()); err != nil {
		t.Fatal(err)
	}
	for kind, want := range map[eventlog.Kind]time.Duration{
		eventlog.KindLogin:        60 * 24 * time.Hour,
		eventlog.KindRegistration: 30 * 24 * time.Hour,
		eventlog.KindTeleport:     24 * time.Hour,
		eventlog.KindCrossing:     24 * time.Hour,
	} {
		if got := moment.Sub(events.asked[kind]); got != want {
			t.Fatalf("%s window = %s, want %s", kind, got, want)
		}
	}
}

// TestUptimeComesFromTheLastRecordedStart covers the one figure that is not a
// count: the grid start the log remembers, and the negative interval a clock
// adjustment could otherwise publish.
func TestUptimeComesFromTheLastRecordedStart(t *testing.T) {
	moment := time.Date(2026, time.August, 21, 12, 0, 0, 0, time.UTC)
	sources := testSources()
	events := sources.Events.(*fixedEvents)
	events.hasStart = true
	events.started = moment.Add(-90 * time.Minute)
	collector := newTestCollector(t, sources)
	collector.now = func() time.Time { return moment }

	snapshot, err := collector.Collect(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if snapshot.UptimeSeconds == nil || *snapshot.UptimeSeconds != 5400 {
		t.Fatalf("uptime = %v, want 5400 seconds", snapshot.UptimeSeconds)
	}
	if snapshot.GridStartedAt == nil || !snapshot.GridStartedAt.Equal(events.started) {
		t.Fatalf("grid start = %v, want %s", snapshot.GridStartedAt, events.started)
	}

	// A start in the future reports the start and no duration.
	events.started = moment.Add(time.Hour)
	snapshot, err = collector.Collect(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if snapshot.UptimeSeconds != nil {
		t.Fatalf("uptime = %d, want none", *snapshot.UptimeSeconds)
	}
	if snapshot.GridStartedAt == nil {
		t.Fatal("a future start must still be reported")
	}
}

// TestUpgradesAnOlderFile covers a stats.csv written before these columns
// existed: the header becomes the current one and the old rows are padded
// rather than zero-filled.
func TestUpgradesAnOlderFile(t *testing.T) {
	recorder := newTestRecorder(t, testSources())
	legacy := "datetime,users,regions,region_equivalents\n" +
		"260819-0600,5,3,11\n260820-0600,6,3,11\n"
	if err := os.WriteFile(recorder.path, []byte(legacy), 0o644); err != nil {
		t.Fatal(err)
	}
	at(recorder, easternTime(t, 2026, time.August, 21, 6, 0))
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	want := header + "\n" +
		"260819-0600,5,3,11,,,,,,,,,,,,\n" +
		"260820-0600,6,3,11,,,,,,,,,,,,\n" +
		"260821-0600,7,3,11,2,5,5,12,12,3,40,900,2,1,,720896\n"
	if string(content) != want {
		t.Fatalf("upgraded file %q, want %q", content, want)
	}
}

// TestUpgradesWithoutARowBeingDue covers the case that made this worth doing:
// the day's row is already written, so nothing is due for another day, and the
// file is still short a column this build measures. Before UpgradeColumns, the
// live grid served a stats.csv missing land_square_metres and would have kept
// serving it until six the next morning.
func TestUpgradesWithoutARowBeingDue(t *testing.T) {
	recorder := newTestRecorder(t, testSources())
	legacy := "datetime,users,regions,region_equivalents\n" +
		"260819-0600,5,3,11\n260820-0600,6,3,11\n"
	if err := os.WriteFile(recorder.path, []byte(legacy), 0o644); err != nil {
		t.Fatal(err)
	}
	// Before the recording hour, so RecordIfDue would return without writing
	// anything and the old upgrade-on-append path would never run.
	at(recorder, easternTime(t, 2026, time.August, 21, 5, 0))
	if err := recorder.UpgradeColumns(); err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	want := header + "\n" +
		"260819-0600,5,3,11,,,,,,,,,,,,\n" +
		"260820-0600,6,3,11,,,,,,,,,,,,\n"
	if string(content) != want {
		t.Fatalf("upgraded file %q, want %q", content, want)
	}
	// No row was invented. An upgrade changes what the columns are called and
	// how wide the rows are; it does not measure anything.
	if last, err := recorder.lastRecordedDay(); err != nil || last != "260820" {
		t.Fatalf("last recorded day = %q (%v), want 260820", last, err)
	}
	// And it is idempotent, which matters because it now runs at every start.
	if err := recorder.UpgradeColumns(); err != nil {
		t.Fatal(err)
	}
	again, err := os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	if string(again) != want {
		t.Fatalf("second upgrade changed the file: %q", again)
	}
}

// TestUpgradeLeavesAMissingFileAlone: a grid that has never recorded has no
// file, and starting one at boot would create a headers-only stats.csv for a
// grid that may never record. Absent stays absent.
func TestUpgradeLeavesAMissingFileAlone(t *testing.T) {
	recorder := newTestRecorder(t, testSources())
	if err := recorder.UpgradeColumns(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(recorder.path); !os.IsNotExist(err) {
		t.Fatalf("a file was created: %v", err)
	}
}

// TestRefusesUnrecognizedColumns: a file whose header is not an older prefix
// of this build's columns is left alone. Appending to it would produce a CSV
// whose columns mean two different things down its length.
func TestRefusesUnrecognizedColumns(t *testing.T) {
	recorder := newTestRecorder(t, testSources())
	foreign := "datetime,users,visitors\n260820-0600,6,3\n"
	if err := os.WriteFile(recorder.path, []byte(foreign), 0o644); err != nil {
		t.Fatal(err)
	}
	at(recorder, easternTime(t, 2026, time.August, 21, 6, 0))
	if err := recorder.RecordIfDue(context.Background()); err == nil {
		t.Fatal("expected unrecognized columns to be refused")
	}
	content, err := os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	if string(content) != foreign {
		t.Fatalf("file was modified: %q", content)
	}
}

func TestServeStats(t *testing.T) {
	recorder := newTestRecorder(t, testSources())

	// Before any row: the header alone, as plain text a browser shows.
	response := httptest.NewRecorder()
	recorder.ServeHTTP(response, httptest.NewRequest("GET", "/stats.csv", nil))
	if response.Code != 200 || response.Body.String() != header+"\n" {
		t.Fatalf("empty stats response %d %q", response.Code, response.Body.String())
	}
	if kind := response.Header().Get("Content-Type"); !strings.HasPrefix(kind, "text/plain") {
		t.Fatalf("content type %q", kind)
	}

	at(recorder, easternTime(t, 2026, time.August, 21, 7, 45))
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	response = httptest.NewRecorder()
	recorder.ServeHTTP(response, httptest.NewRequest("GET", "/stats.csv", nil))
	if response.Code != 200 ||
		response.Body.String() != header+"\n260821-0745,7,3,11,2,5,5,12,12,3,40,900,2,1,,720896\n" {
		t.Fatalf("stats response %d %q", response.Code, response.Body.String())
	}

	response = httptest.NewRecorder()
	recorder.ServeHTTP(response, httptest.NewRequest("POST", "/stats.csv", nil))
	if response.Code != 405 {
		t.Fatalf("POST answered %d, want 405", response.Code)
	}
}
