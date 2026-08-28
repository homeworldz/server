package main

import (
	"bufio"
	"context"
	"database/sql"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"golang.org/x/term"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/attachments"
	"github.com/homeworldz/server/grid/internal/config"
	"github.com/homeworldz/server/grid/internal/estate"
	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/gestures"
	"github.com/homeworldz/server/grid/internal/httpapi"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/locations"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/renditions"
	"github.com/homeworldz/server/grid/internal/schema"
	"github.com/homeworldz/server/grid/internal/stats"
	"github.com/homeworldz/server/grid/internal/tasktransfer"
	"github.com/homeworldz/server/grid/internal/transit"
	"github.com/homeworldz/server/grid/internal/vault"
	"github.com/homeworldz/server/grid/internal/webtoken"
	_ "github.com/jackc/pgx/v5/stdlib"
)

// version is stamped at build time from the repository VERSION file by
// scripts/build-grid.sh or grid/cmd/package-release. An unstamped binary says
// so rather than naming a plausible version.
var version = "unstamped"

func main() {
	configDirectory := flag.String("config", "config", "directory containing grid.ini and db.ini")
	var showVersion, showHelp bool
	flag.BoolVar(&showVersion, "v", false, "print version and exit")
	flag.BoolVar(&showVersion, "version", false, "print version and exit")
	flag.BoolVar(&showHelp, "h", false, "show this help and exit")
	flag.BoolVar(&showHelp, "?", false, "show this help and exit")
	flag.BoolVar(&showHelp, "help", false, "show this help and exit")
	createUser := flag.String("u", "", "create or update a user (prompts for a password) and exit")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Homeworldz grid server %s\n\n", version)
		fmt.Fprintf(os.Stderr, "Usage: %s [options]\n\n", filepath.Base(os.Args[0]))
		fmt.Fprintln(os.Stderr, "Options:")
		fmt.Fprintln(os.Stderr, "  -config <dir>    directory containing grid.ini and db.ini (default \"config\")")
		fmt.Fprintln(os.Stderr, "  -u <username>    create or update a user, prompting for a password, then exit")
		fmt.Fprintln(os.Stderr, "  -v, --version    print version and exit")
		fmt.Fprintln(os.Stderr, "  -h, -?, --help   show this help and exit")
		fmt.Fprintln(os.Stderr, "\nWith no options the grid server starts normally.")
	}
	flag.Parse()
	if showHelp {
		flag.Usage()
		return
	}
	if showVersion {
		fmt.Println(version)
		return
	}
	logger := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	settings, err := config.LoadGrid(*configDirectory)
	if err != nil {
		logger.Error("load configuration", "error", err)
		os.Exit(1)
	}

	var db *sql.DB
	if settings.DatabaseURL != "" {
		db, err = sql.Open("pgx", settings.DatabaseURL)
		if err != nil {
			logger.Error("open database", "error", err)
			os.Exit(1)
		}
		defer db.Close()

		// Before the server or any admin mode runs: this build's queries reference
		// the schema it was written against, and running without it fails one
		// request at a time in handlers rather than here (see the schema package).
		if err := schema.Verify(context.Background(), db, logger); err != nil {
			logger.Error("database schema check", "error", err)
			os.Exit(1)
		}
	}

	// User administration: create or update one user, then exit without starting
	// the server.
	if strings.TrimSpace(*createUser) != "" {
		if db == nil {
			fmt.Fprintln(os.Stderr, "create user requires a configured database (set database.url in db.ini)")
			os.Exit(1)
		}
		if err := runCreateUser(context.Background(), db, strings.TrimSpace(*createUser)); err != nil {
			fmt.Fprintln(os.Stderr, "create user failed:", err)
			os.Exit(1)
		}
		return
	}

	provisionedRegions, err := provisioning.Load(filepath.Join(settings.Directory, "regions.json"))
	if err != nil {
		logger.Error("load provisioned regions", "error", err)
		os.Exit(1)
	}
	var provisionedStore provisioning.Store = provisionedRegions
	if db != nil {
		items, listErr := provisionedRegions.List(context.Background())
		if listErr != nil {
			logger.Error("list provisioned region seeds", "error", listErr)
			os.Exit(1)
		}
		persistent := provisioning.NewPostgresStore(db)
		if err := persistent.Import(context.Background(), items); err != nil {
			logger.Error("import provisioned region seeds", "error", err)
			os.Exit(1)
		}
		provisionedStore = persistent
	}

	assetVault := vaultStore(db, settings.VaultPath, logger)
	renditionStore := renditionsStore(db, settings.VaultPath, logger)

	welcome, err := arrival.ParseList(settings.WelcomeLocations)
	if err != nil {
		logger.Error("parse [grid] welcome_locations", "error", err)
		os.Exit(1)
	}

	// The event log records what happened for the statistics the grid
	// publishes (ADR 0039). Without a database there is nowhere to record and
	// nothing to count, so both it and the daily recorder stay absent rather
	// than counting into the void.
	var events *eventlog.PostgresStore
	var statsRecorder *stats.Recorder
	if db != nil {
		events = eventlog.NewPostgresStore(db)
		// Uptime is measured from this row, which is why it is written before
		// the server listens: an uptime that started when the first login
		// arrived would flatter every restart.
		if err := events.Record(context.Background(), eventlog.Event{
			Kind: eventlog.KindGridStart, Detail: version,
		}); err != nil {
			logger.Error("record grid start", "error", err)
		}
		// The collector needs the concrete identity store (CountUsers is
		// deliberately not on the Store interface).
		collector, collectErr := stats.NewCollector(stats.Sources{
			Users:       identity.NewPostgresStore(db),
			Provisioned: provisionedStore,
			Leases:      regions.NewPostgresStore(db),
			Presence:    presence.NewPostgresStore(db),
			Events:      events,
		})
		if collectErr != nil {
			logger.Error("configure grid stats", "error", collectErr)
			os.Exit(1)
		}
		statsRecorder, err = stats.New(settings.StatsPath, collector, logger)
		if err != nil {
			logger.Error("configure grid stats", "error", err)
			os.Exit(1)
		}
	}

	// The region-ticket verifier answers regions' validate-ticket calls; the
	// signing secret stays here. Absent a configured secret the route reports
	// itself unavailable rather than the binary failing to start.
	var ticketVerifier *webtoken.Signer
	if settings.WebsiteJWTSecret != "" {
		ticketVerifier, err = webtoken.NewSigner([]byte(settings.WebsiteJWTSecret),
			settings.WebsiteJWTIssuer, webtoken.RegionTicketAudience, settings.RegionTicketTTL)
		if err != nil {
			logger.Error("configure region ticket verifier", "error", err)
			os.Exit(1)
		}
	}

	server := &http.Server{
		Addr: settings.Address,
		Handler: httpapi.New(db, version, httpapi.Options{
			ServiceToken:      settings.ServiceToken,
			GridPublicURL:     settings.PublicURL,
			GridName:          settings.Name,
			GridNick:          settings.Nick,
			AboutURL:          settings.AboutURL,
			SupportURL:        settings.SupportURL,
			RegisterURL:       settings.RegisterURL,
			PasswordURL:       settings.PasswordURL,
			WelcomeMessage:    settings.WelcomeMessage,
			Logger:            logger,
			Regions:           regionStore(db),
			Identity:          identityStore(db),
			Presence:          presenceStore(db),
			Inventory:         inventoryStore(db),
			Assets:            assetStore(db),
			Vault:             assetVault,
			Renditions:        renditionStore,
			WorkerToken:       settings.WorkerToken,
			Provisioned:       provisionedStore,
			TerrainHTTPClient: &http.Client{Timeout: 2 * time.Second},
			Transits:          transitStore(db),
			TaskTransfers:     taskTransferStore(db),
			Locations:         locationStore(db),
			Gestures:          gestureStore(db),
			Attachments:       attachmentStore(db),
			Estates:           estate.NewPostgresStore(db),
			Welcome:           welcome,
			TicketVerifier:    ticketVerifier,
			Stats:             statsHandler(statsRecorder),
			Events:            eventRecorder(events),
			WebsiteAPIURL:     settings.WebsitePublicURL,
		}),
		ReadHeaderTimeout: 5 * time.Second,
	}
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	recording, stopRecording := context.WithCancel(context.Background())
	defer stopRecording()
	if statsRecorder != nil {
		go statsRecorder.Run(recording)
	}
	go func() {
		logger.Info("grid service listening", "address", settings.Address,
			"version", version, "configDirectory", settings.Directory)
		if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("serve", "error", err)
			os.Exit(1)
		}
	}()

	<-stop
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := server.Shutdown(ctx); err != nil {
		logger.Error("shutdown", "error", err)
	}
}

// runCreateUser prompts for a password (with confirmation) and creates or updates
// the named user's web (bcrypt) and viewer (MD5) password hashes.
func runCreateUser(ctx context.Context, db *sql.DB, username string) error {
	if username == "" {
		return errors.New("username cannot be empty")
	}
	password, err := readPassword("Password for '" + username + "': ")
	if err != nil {
		return err
	}
	if password == "" {
		return errors.New("password cannot be empty")
	}
	confirm, err := readPassword("Confirm password: ")
	if err != nil {
		return err
	}
	if password != confirm {
		return errors.New("passwords do not match")
	}
	user, created, err := identity.NewPostgresStore(db).UpsertUser(ctx, username, password)
	if err != nil {
		return err
	}
	action := "updated password hashes for existing user"
	if created {
		action = "created user"
	}
	fmt.Printf("%s %q (id %s).\n", action, user.Username, user.ID)
	return nil
}

func readPassword(prompt string) (string, error) {
	fmt.Print(prompt)
	if term.IsTerminal(int(syscall.Stdin)) {
		value, err := term.ReadPassword(int(syscall.Stdin))
		fmt.Println()
		if err != nil {
			return "", fmt.Errorf("read password: %w", err)
		}
		return string(value), nil
	}
	value, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil {
		return "", fmt.Errorf("read password: %w", err)
	}
	return strings.TrimSpace(value), nil
}

func regionStore(db *sql.DB) regions.Store {
	if db == nil {
		return nil
	}
	return regions.NewPostgresStore(db)
}

func identityStore(db *sql.DB) identity.Store {
	if db == nil {
		return nil
	}
	return identity.NewPostgresStore(db)
}

// statsHandler keeps a missing recorder out of the mux: an http.Handler
// interface holding a nil *stats.Recorder is not itself nil, and would route
// /stats into a nil-pointer call instead of leaving it unrouted.
func statsHandler(recorder *stats.Recorder) http.Handler {
	if recorder == nil {
		return nil
	}
	return recorder
}

// eventRecorder keeps a missing event store out of the API for the same
// reason statsHandler keeps a missing recorder out of the mux: an interface
// holding a nil pointer is not itself nil, and every best-effort Note would
// call through it into a nil dereference.
func eventRecorder(store *eventlog.PostgresStore) eventlog.Recorder {
	if store == nil {
		return nil
	}
	return store
}

func presenceStore(db *sql.DB) presence.Store {
	if db == nil {
		return nil
	}
	return presence.NewPostgresStore(db)
}

func locationStore(db *sql.DB) locations.Store {
	if db == nil {
		return nil
	}
	return locations.NewPostgresStore(db)
}

func gestureStore(db *sql.DB) gestures.Store {
	if db == nil {
		return nil
	}
	return gestures.NewPostgresStore(db)
}

func attachmentStore(db *sql.DB) attachments.Store {
	if db == nil {
		return nil
	}
	return attachments.NewPostgresStore(db)
}

func inventoryStore(db *sql.DB) inventory.Store {
	if db == nil {
		return nil
	}
	return inventory.NewPostgresStore(db)
}

func assetStore(db *sql.DB) assetmeta.Store {
	if db == nil {
		return nil
	}
	return assetmeta.NewPostgresStore(db)
}

// vaultStore opens the asset vault (ADR 0026). A configured database with an
// unusable vault directory is fatal rather than degraded: the vault's whole
// purpose is that inventory durability never depends on something optional, and
// once inventory commits enforce it, a grid that silently ran without one would
// have accepted items whose bytes it never stored.
// renditionsStore roots the derived-encoding tree beside the vault's shards
// (shard names are two hex characters, so "renditions" cannot collide).
func renditionsStore(db *sql.DB, vaultDirectory string, logger *slog.Logger) renditions.Store {
	if db == nil {
		return nil
	}
	store, err := renditions.NewPostgresStore(db, filepath.Join(vaultDirectory, "renditions"))
	if err != nil {
		logger.Error("open rendition store", "error", err, "path", vaultDirectory)
		os.Exit(1)
	}
	logger.Info("rendition store ready", "path", store.Directory())
	return store
}

func vaultStore(db *sql.DB, directory string, logger *slog.Logger) vault.Store {
	if db == nil {
		return nil
	}
	store, err := vault.NewPostgresStore(db, directory)
	if err != nil {
		logger.Error("open asset vault", "error", err, "path", directory)
		os.Exit(1)
	}
	logger.Info("asset vault ready", "path", store.Directory())
	return store
}

func transitStore(db *sql.DB) transit.Store {
	if db == nil {
		return nil
	}
	return transit.NewPostgresStore(db)
}

func taskTransferStore(db *sql.DB) tasktransfer.Store {
	if db == nil {
		return nil
	}
	return tasktransfer.NewPostgresStore(db)
}
