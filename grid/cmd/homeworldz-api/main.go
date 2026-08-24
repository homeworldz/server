// Command homeworldz-api serves the browser-facing Homeworldz website API
// (homeworldz.com/api/openapi.yaml): email-verified avatar registration,
// stateless website authentication, self-service account management, and
// privileged administration. It runs as its own binary on its own port,
// separate from the grid's service-token internal API.
package main

import (
	"context"
	"database/sql"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/homeworldz/server/grid/internal/api"
	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/config"
	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/locations"
	"github.com/homeworldz/server/grid/internal/mailer"
	"github.com/homeworldz/server/grid/internal/messages"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/schema"
	"github.com/homeworldz/server/grid/internal/stats"
	"github.com/homeworldz/server/grid/internal/webaccount"
	"github.com/homeworldz/server/grid/internal/webtoken"
	_ "github.com/jackc/pgx/v5/stdlib"
)

// version is stamped at build time from the repository VERSION file by
// scripts/build-grid.sh or grid/cmd/package-release. An unstamped binary says
// so rather than naming a plausible version.
var version = "unstamped"

func main() {
	configDirectory := flag.String("config", "config", "directory containing grid.ini and db.ini")
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	settings, err := config.LoadGrid(*configDirectory)
	if err != nil {
		logger.Error("load configuration", "error", err)
		os.Exit(1)
	}
	if settings.DatabaseURL == "" {
		logger.Error("website api requires a database url ([database] url in db.ini)")
		os.Exit(1)
	}

	db, err := sql.Open("pgx", settings.DatabaseURL)
	if err != nil {
		logger.Error("open database", "error", err)
		os.Exit(1)
	}
	defer db.Close()

	// Before anything is served: this build's queries reference the schema it was
	// written against, and running without it fails one request at a time in
	// handlers rather than here (see the schema package).
	if err := schema.Verify(context.Background(), db, logger); err != nil {
		logger.Error("database schema check", "error", err)
		os.Exit(1)
	}

	signer, err := webtoken.NewSigner([]byte(settings.WebsiteJWTSecret),
		settings.WebsiteJWTIssuer, settings.WebsiteJWTAudience, settings.WebsiteTokenTTL)
	if err != nil {
		logger.Error("configure token signer (set [website] jwt_secret)", "error", err)
		os.Exit(1)
	}
	// The region-ticket signer shares the secret and issuer but never the
	// audience: an account route refuses a ticket and a region refuses an
	// account token on the audience check alone.
	ticketSigner, err := webtoken.NewSigner([]byte(settings.WebsiteJWTSecret),
		settings.WebsiteJWTIssuer, webtoken.RegionTicketAudience, settings.RegionTicketTTL)
	if err != nil {
		logger.Error("configure region ticket signer", "error", err)
		os.Exit(1)
	}

	mail, err := buildMailer(settings, logger)
	if err != nil {
		logger.Error("configure mailer", "error", err)
		os.Exit(1)
	}

	welcome, err := arrival.ParseList(settings.WelcomeLocations)
	if err != nil {
		logger.Error("parse [grid] welcome_locations", "error", err)
		os.Exit(1)
	}

	// The public statistics endpoint counts from the same stores the grid's
	// daily CSV row does, so the login page and the chart can never disagree
	// about what a figure means.
	events := eventlog.NewPostgresStore(db)
	statistics, err := stats.NewCollector(stats.Sources{
		Users:       identity.NewPostgresStore(db),
		Provisioned: provisioning.NewPostgresStore(db),
		Leases:      regions.NewPostgresStore(db),
		Presence:    presence.NewPostgresStore(db),
		Events:      events,
	})
	if err != nil {
		logger.Error("configure grid statistics", "error", err)
		os.Exit(1)
	}

	handler, err := api.New(api.Options{
		Accounts:        webaccount.NewPostgresStore(db),
		Regions:         provisioning.NewPostgresStore(db),
		Leases:          regions.NewPostgresStore(db),
		Presence:        presence.NewPostgresStore(db),
		Signer:          signer,
		Mailer:          mail,
		Logger:          logger,
		AllowedOrigins:  settings.WebsiteAllowedOrigins,
		VerificationURL: settings.MailVerificationURL,
		ResetURL:        settings.MailResetURL,
		RatePerMinute:   settings.WebsiteRatePerMinute,
		RateBurst:       settings.WebsiteRateBurst,
		Version:         version,
		GridName:        settings.Name,
		WelcomeMessage:  settings.WelcomeMessage,
		Welcome:         welcome,
		Sessions:        identity.NewPostgresStore(db),
		Messages:        messages.NewPostgresStore(db),
		Locations:       locations.NewPostgresStore(db),
		Inventory:       inventory.NewPostgresStore(db),
		TicketSigner:    ticketSigner,
		ChannelURL:      channelURL(settings.WebsitePublicURL),
		Stats:           statistics,
		Events:          events,
	})
	if err != nil {
		logger.Error("build website api", "error", err)
		os.Exit(1)
	}

	server := &http.Server{
		Addr:              settings.WebsiteAddress,
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
	}
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		logger.Info("website api listening", "address", settings.WebsiteAddress,
			"version", version, "mailTransport", settings.MailTransport)
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

// channelURL derives the grid channel's absolute WebSocket URL from the
// website API's public URL, when one is configured.
func channelURL(publicURL string) string {
	switch {
	case strings.HasPrefix(publicURL, "https://"):
		return "wss://" + strings.TrimPrefix(publicURL, "https://") + "/v1/client/channel"
	case strings.HasPrefix(publicURL, "http://"):
		return "ws://" + strings.TrimPrefix(publicURL, "http://") + "/v1/client/channel"
	default:
		return ""
	}
}

// buildMailer selects the mail transport. "log" (the default) writes the
// verification link to the log instead of sending email, which is what local
// development uses; "smtp" sends through the configured relay.
func buildMailer(settings config.Grid, logger *slog.Logger) (mailer.Mailer, error) {
	switch settings.MailTransport {
	case "smtp":
		return mailer.NewSMTPMailer(mailer.SMTPConfig{
			Host:        settings.SMTPHost,
			Port:        settings.SMTPPort,
			Username:    settings.SMTPUsername,
			Password:    settings.SMTPPassword,
			From:        settings.MailFrom,
			ImplicitTLS: settings.SMTPImplicitTLS,
		})
	case "log", "":
		return mailer.NewLogMailer(logger, settings.MailFrom), nil
	default:
		return nil, fmt.Errorf("unknown mail transport %q", settings.MailTransport)
	}
}
