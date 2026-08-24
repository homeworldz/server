// Package stats records a once-daily summary of the grid — how many user
// accounts exist and how many people used them, how many regions are
// provisioned, running and enabled, how much land they cover in 256 m x 256 m
// standard-region equivalents, and how long the grid has been up — appended to
// a plain CSV so the numbers can be charted over time, and serves that file at
// /stats.csv. The same figures are read live by the website API's public
// /v1/stats endpoint; both come from Collector, so the page and the chart can
// never disagree about what a column means.
//
// One row per day, taken at 06:00 US Eastern time. The recorder checks every
// minute and writes the row the first time it finds one due, so a grid that
// was down at six records the day's row at startup instead — the timestamp
// column keeps the actual time so a late row is visible as one. A failed
// count skips the attempt entirely rather than recording zeros: a zero that
// means "the database was unreachable" would chart as an exodus.
//
// Columns are only ever appended, never reordered or repurposed, so a chart
// built against an older file keeps working. A file written by an older build
// is upgraded in place the next time a row is due: its header becomes the
// current one and its rows are padded with empty fields, which says "this
// grid did not measure that yet" rather than the zero that would say it
// measured nothing.
package stats

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
	// The Eastern-time schedule must resolve on hosts without a system zone
	// database (Windows development machines); the embedded copy is the
	// fallback, the system database wins where present.
	_ "time/tzdata"
)

// columns names each CSV field in order. The first four are the original
// columns and keep their exact meaning: users is every account, regions is
// enabled regions, region_equivalents is enabled land in standard regions.
//
// A new column goes on the end and nowhere else. upgradeHeader rewrites an
// older file only when its header is a prefix of this one, so inserting a
// column in the middle would refuse every existing file rather than silently
// mismatching them — which is the right failure, but still a failure.
var columns = []string{
	"datetime", "users", "regions", "region_equivalents",
	"users_online", "active_30d", "active_60d",
	"logins_24h", "logins_30d", "registrations_30d",
	"teleports_24h", "crossings_24h",
	"regions_online", "regions_undeployed", "uptime_seconds",
	"land_square_metres",
}

var header = strings.Join(columns, ",")

// recordHour is the local hour after which the day's row is due.
const recordHour = 6

type Recorder struct {
	path      string
	collector *Collector
	logger    *slog.Logger
	location  *time.Location
	// now is replaceable by tests; everything time-dependent goes through it.
	now func() time.Time
	mu  sync.Mutex
}

func New(path string, collector *Collector, logger *slog.Logger) (*Recorder, error) {
	if strings.TrimSpace(path) == "" {
		return nil, fmt.Errorf("stats path is empty")
	}
	if collector == nil {
		return nil, fmt.Errorf("stats collector is required")
	}
	location, err := time.LoadLocation("America/New_York")
	if err != nil {
		return nil, fmt.Errorf("load Eastern time zone: %w", err)
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &Recorder{path: path, collector: collector,
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
	snapshot, err := r.collector.Collect(ctx)
	if err != nil {
		return err
	}
	if err := r.append(row(local, snapshot)); err != nil {
		return err
	}
	r.logger.Info("grid stats recorded", "users", snapshot.Users,
		"activeUsers30d", snapshot.ActiveUsers30d, "usersOnline", snapshot.UsersOnline,
		"regions", snapshot.Regions, "regionsOnline", snapshot.RegionsOnline,
		"regionEquivalents", snapshot.RegionEquivalents)
	return nil
}

// row renders a snapshot as one CSV line, stamped with the local wall-clock
// time the row was taken at rather than the snapshot's UTC instant, so a late
// row is visible as one in the operator's own time.
func row(local time.Time, snapshot Snapshot) string {
	uptime := ""
	if snapshot.UptimeSeconds != nil {
		uptime = strconv.FormatInt(*snapshot.UptimeSeconds, 10)
	}
	fields := []string{
		local.Format("060102-1504"),
		strconv.Itoa(snapshot.Users),
		strconv.Itoa(snapshot.Regions),
		strconv.Itoa(snapshot.RegionEquivalents),
		strconv.Itoa(snapshot.UsersOnline),
		strconv.Itoa(snapshot.ActiveUsers30d),
		strconv.Itoa(snapshot.ActiveUsers60d),
		strconv.Itoa(snapshot.Logins24h),
		strconv.Itoa(snapshot.Logins30d),
		strconv.Itoa(snapshot.Registrations30d),
		strconv.Itoa(snapshot.Teleports24h),
		strconv.Itoa(snapshot.Crossings24h),
		strconv.Itoa(snapshot.RegionsOnline),
		strconv.Itoa(snapshot.RegionsUndeployed),
		uptime,
		strconv.FormatInt(snapshot.LandSquareMetres, 10),
	}
	return strings.Join(fields, ",")
}

// lastRecordedDay returns the YYMMDD of the newest row, or "" for none. The
// file is small for decades (a line a day), so reading it whole is fine.
func (r *Recorder) lastRecordedDay() (string, error) {
	lines, err := r.lines()
	if err != nil {
		return "", err
	}
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

// lines reads the file as trimmed lines, or none when it does not exist yet.
func (r *Recorder) lines() ([]string, error) {
	content, err := os.ReadFile(r.path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("read stats file: %w", err)
	}
	trimmed := strings.TrimSpace(string(content))
	if trimmed == "" {
		return nil, nil
	}
	return strings.Split(trimmed, "\n"), nil
}

func (r *Recorder) append(line string) error {
	if directory := filepath.Dir(r.path); directory != "." {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			return fmt.Errorf("create stats directory: %w", err)
		}
	}
	if err := r.upgradeHeader(); err != nil {
		return err
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

// upgradeHeader rewrites a file written by a build with fewer columns: the
// header becomes the current one and every existing row is padded to the
// current width with empty fields. Padding rather than zero-filling is the
// point — nobody counted logins in those weeks, and a column of zeros would
// claim somebody had and found none.
//
// Only an older prefix of the current columns is upgraded. A header that is
// not one is a file this build does not understand, and appending to it would
// produce a CSV whose columns mean two different things down its length.
func (r *Recorder) upgradeHeader() error {
	lines, err := r.lines()
	if err != nil || len(lines) == 0 {
		return err
	}
	existing := strings.TrimSpace(lines[0])
	if existing == header {
		return nil
	}
	if !strings.HasPrefix(existing, "datetime") {
		return fmt.Errorf("stats file %s has no header row", r.path)
	}
	if !strings.HasPrefix(header, existing+",") {
		return fmt.Errorf("stats file %s has unrecognized columns %q", r.path, existing)
	}
	width := len(strings.Split(existing, ","))
	padding := strings.Repeat(",", len(columns)-width)
	upgraded := make([]string, 0, len(lines))
	upgraded = append(upgraded, header)
	for _, line := range lines[1:] {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" {
			continue
		}
		upgraded = append(upgraded, trimmed+padding)
	}
	if err := os.WriteFile(r.path, []byte(strings.Join(upgraded, "\n")+"\n"), 0o644); err != nil {
		return fmt.Errorf("upgrade stats file columns: %w", err)
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
