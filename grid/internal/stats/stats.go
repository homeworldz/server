// Package stats records a once-daily summary of the grid — how many user
// accounts exist, how many regions are provisioned and enabled, and how much
// land they cover in 256 m x 256 m standard-region equivalents — appended to
// a plain CSV so the numbers can be charted over time, and serves that file
// at /stats.csv (/stats is reserved for the human-facing page to come).
//
// One row per day, taken at 06:00 US Eastern time. The recorder checks every
// minute and writes the row the first time it finds one due, so a grid that
// was down at six records the day's row at startup instead — the timestamp
// column keeps the actual time so a late row is visible as one. A failed
// count skips the attempt entirely rather than recording zeros: a zero that
// means "the database was unreachable" would chart as an exodus.
package stats

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
	// The Eastern-time schedule must resolve on hosts without a system zone
	// database (Windows development machines); the embedded copy is the
	// fallback, the system database wins where present.
	_ "time/tzdata"

	"github.com/homeworldz/server/grid/internal/provisioning"
)

const header = "datetime,users,regions,region_equivalents"

// recordHour is the local hour after which the day's row is due.
const recordHour = 6

// UserCounter answers how many user accounts exist. Satisfied by the
// identity store's Postgres implementation; narrow so tests need not build
// the whole store.
type UserCounter interface {
	CountUsers(context.Context) (int, error)
}

// RegionLister is the slice of the provisioning store the recorder needs.
type RegionLister interface {
	List(context.Context) ([]provisioning.Region, error)
}

type Recorder struct {
	path        string
	users       UserCounter
	provisioned RegionLister
	logger      *slog.Logger
	location    *time.Location
	// now is replaceable by tests; everything time-dependent goes through it.
	now func() time.Time
	mu  sync.Mutex
}

func New(path string, users UserCounter, provisioned RegionLister, logger *slog.Logger) (*Recorder, error) {
	if strings.TrimSpace(path) == "" {
		return nil, fmt.Errorf("stats path is empty")
	}
	location, err := time.LoadLocation("America/New_York")
	if err != nil {
		return nil, fmt.Errorf("load Eastern time zone: %w", err)
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &Recorder{path: path, users: users, provisioned: provisioned,
		logger: logger, location: location, now: time.Now}, nil
}

// Run records whenever a row becomes due, once a minute, until the context
// ends. It checks immediately so a restart never loses a day.
func (r *Recorder) Run(ctx context.Context) {
	ticker := time.NewTicker(time.Minute)
	defer ticker.Stop()
	for {
		if err := r.RecordIfDue(ctx); err != nil {
			r.logger.Error("record grid stats", "error", err)
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

// RecordIfDue appends the day's row when the local time has passed the
// recording hour and no row exists for today. Nil when nothing was due.
func (r *Recorder) RecordIfDue(ctx context.Context) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	local := r.now().In(r.location)
	if local.Hour() < recordHour {
		return nil
	}
	today := local.Format("060102")
	last, err := r.lastRecordedDay()
	if err != nil {
		return err
	}
	if last >= today {
		return nil
	}
	users, err := r.users.CountUsers(ctx)
	if err != nil {
		return fmt.Errorf("count users: %w", err)
	}
	regions, equivalents, err := r.countRegions(ctx)
	if err != nil {
		return fmt.Errorf("count regions: %w", err)
	}
	line := fmt.Sprintf("%s,%d,%d,%d", local.Format("060102-1504"), users, regions, equivalents)
	if err := r.append(line); err != nil {
		return err
	}
	r.logger.Info("grid stats recorded", "users", users,
		"regions", regions, "regionEquivalents", equivalents)
	return nil
}

// countRegions counts enabled provisioned regions and their total land in
// standard-region chunks: a provisioned size is already in 256 m units, so a
// 4x2 rectangle is eight equivalents.
func (r *Recorder) countRegions(ctx context.Context) (int, int, error) {
	all, err := r.provisioned.List(ctx)
	if err != nil {
		return 0, 0, err
	}
	regions, equivalents := 0, 0
	for _, region := range all {
		if !region.Enabled {
			continue
		}
		regions++
		equivalents += region.SizeX * region.SizeY
	}
	return regions, equivalents, nil
}

// lastRecordedDay returns the YYMMDD of the newest row, or "" for none. The
// file is small for decades (a line a day), so reading it whole is fine.
func (r *Recorder) lastRecordedDay() (string, error) {
	content, err := os.ReadFile(r.path)
	if os.IsNotExist(err) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("read stats file: %w", err)
	}
	lines := strings.Split(strings.TrimSpace(string(content)), "\n")
	for index := len(lines) - 1; index >= 0; index-- {
		line := strings.TrimSpace(lines[index])
		if line == "" || strings.HasPrefix(line, "datetime") {
			continue
		}
		day, _, found := strings.Cut(line, "-")
		if !found || len(day) != 6 {
			return "", fmt.Errorf("malformed stats row %q", line)
		}
		return day, nil
	}
	return "", nil
}

func (r *Recorder) append(line string) error {
	if directory := filepath.Dir(r.path); directory != "." {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			return fmt.Errorf("create stats directory: %w", err)
		}
	}
	file, err := os.OpenFile(r.path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return fmt.Errorf("open stats file: %w", err)
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return fmt.Errorf("stat stats file: %w", err)
	}
	content := line + "\n"
	if info.Size() == 0 {
		content = header + "\n" + content
	}
	if _, err := file.WriteString(content); err != nil {
		return fmt.Errorf("append stats row: %w", err)
	}
	return nil
}

// ServeHTTP answers GET /stats.csv with the CSV as browsable plain text —
// the header line alone when no row has been recorded yet.
func (r *Recorder) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	if request.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "stats endpoint requires GET", http.StatusMethodNotAllowed)
		return
	}
	r.mu.Lock()
	content, err := os.ReadFile(r.path)
	r.mu.Unlock()
	if os.IsNotExist(err) {
		content = []byte(header + "\n")
	} else if err != nil {
		http.Error(w, "stats are unavailable", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = w.Write(content)
}
