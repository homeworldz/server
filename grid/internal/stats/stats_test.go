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

	"github.com/homeworldz/server/grid/internal/provisioning"
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

func newTestRecorder(t *testing.T, users UserCounter, regions RegionLister) *Recorder {
	t.Helper()
	recorder, err := New(filepath.Join(t.TempDir(), "stats.csv"), users, regions, nil)
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

func TestRecordsOnceDailyAfterSix(t *testing.T) {
	regions := fixedRegions{regions: []provisioning.Region{
		{Name: "Welcome", SizeX: 1, SizeY: 1, Enabled: true},
		{Name: "Sandbox", SizeX: 1, SizeY: 2, Enabled: true},
		{Name: "Delta", SizeX: 4, SizeY: 2, Enabled: true},
		{Name: "Mothballed", SizeX: 4, SizeY: 4, Enabled: false},
	}}
	recorder := newTestRecorder(t, fixedCounts{users: 7}, regions)

	// Before six: nothing is due.
	recorder.now = func() time.Time { return easternTime(t, 2026, time.August, 21, 5, 59) }
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(recorder.path); !os.IsNotExist(err) {
		t.Fatalf("row recorded before six: %v", err)
	}

	// After six: header plus the day's row; disabled regions count nowhere.
	recorder.now = func() time.Time { return easternTime(t, 2026, time.August, 21, 6, 0) }
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	want := header + "\n260821-0600,7,3,11\n"
	if string(content) != want {
		t.Fatalf("recorded %q, want %q", content, want)
	}

	// Later the same day: still one row.
	recorder.now = func() time.Time { return easternTime(t, 2026, time.August, 21, 18, 30) }
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	// The next day appends; a late start records at the actual time.
	recorder.now = func() time.Time { return easternTime(t, 2026, time.August, 22, 9, 15) }
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	content, err = os.ReadFile(recorder.path)
	if err != nil {
		t.Fatal(err)
	}
	want += "260822-0915,7,3,11\n"
	if string(content) != want {
		t.Fatalf("after two days %q, want %q", content, want)
	}
}

func TestFailedCountsRecordNothing(t *testing.T) {
	recorder := newTestRecorder(t, fixedCounts{err: errors.New("database is down")},
		fixedRegions{})
	recorder.now = func() time.Time { return easternTime(t, 2026, time.August, 21, 6, 5) }
	if err := recorder.RecordIfDue(context.Background()); err == nil {
		t.Fatal("expected the failed count to surface")
	}
	if _, err := os.Stat(recorder.path); !os.IsNotExist(err) {
		t.Fatal("a failed count must not record a row")
	}
}

func TestServeStats(t *testing.T) {
	recorder := newTestRecorder(t, fixedCounts{users: 2},
		fixedRegions{regions: []provisioning.Region{{Name: "Welcome", SizeX: 1, SizeY: 1, Enabled: true}}})

	// Before any row: the header alone, as plain text a browser shows.
	response := httptest.NewRecorder()
	recorder.ServeHTTP(response, httptest.NewRequest("GET", "/stats", nil))
	if response.Code != 200 || response.Body.String() != header+"\n" {
		t.Fatalf("empty stats response %d %q", response.Code, response.Body.String())
	}
	if kind := response.Header().Get("Content-Type"); !strings.HasPrefix(kind, "text/plain") {
		t.Fatalf("content type %q", kind)
	}

	recorder.now = func() time.Time { return easternTime(t, 2026, time.August, 21, 7, 45) }
	if err := recorder.RecordIfDue(context.Background()); err != nil {
		t.Fatal(err)
	}
	response = httptest.NewRecorder()
	recorder.ServeHTTP(response, httptest.NewRequest("GET", "/stats", nil))
	if response.Code != 200 ||
		response.Body.String() != header+"\n260821-0745,2,1,1\n" {
		t.Fatalf("stats response %d %q", response.Code, response.Body.String())
	}

	response = httptest.NewRecorder()
	recorder.ServeHTTP(response, httptest.NewRequest("POST", "/stats", nil))
	if response.Code != 405 {
		t.Fatalf("POST answered %d, want 405", response.Code)
	}
}
