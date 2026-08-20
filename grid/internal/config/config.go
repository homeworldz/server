package config

import (
	"fmt"
	"net/url"
	"path/filepath"
	"strings"
	"time"

	ini "gopkg.in/ini.v1"
)

type Grid struct {
	Address      string
	PublicURL    string
	Name         string
	DatabaseURL  string
	ServiceToken string
	// WorkerToken authenticates the grid's own conversion workers (ADR 0033).
	// It is deliberately not the service token: regions hold that one, regions
	// are untrusted (ADR 0028), and a rendition upload cannot be
	// checksum-verified the way region-served asset bytes can — the grid
	// cannot verify a conversion without redoing it. Only holders of this
	// token may claim conversion jobs or write rendition bytes. Empty
	// disables those endpoints.
	WorkerToken string
	Directory   string
	// WelcomeLocations is the ordered new-arrival list ([grid]
	// welcome_locations): comma-separated Region/x/y/z entries, first entry
	// preferred. Parsing into structured points happens at the consumer so a
	// malformed entry fails that binary's startup with a specific error.
	WelcomeLocations []string
	// WelcomeMessage is the grid-wide greeting ([grid] welcome_message),
	// delivered once per login — the viewer login reply's message field and
	// the grid channel hello — never per region entered, so a border
	// crossing does not re-welcome anyone to the grid. {grid} and {user}
	// resolve to the grid name and the avatar's display name; configure it
	// empty to disable. Region-specific greetings are the region's own
	// region.welcome_message.
	WelcomeMessage string
	// AboutURL, SupportURL, RegisterURL and PasswordURL are the human-facing
	// destinations published in get_grid_info ([grid] about_url, support_url,
	// register_url, password_url). A viewer's grid manager offers them as Grid
	// Website, Grid Support, Grid Registration and Grid Password URI, so each
	// must be a page a person can open — not a service endpoint. Set one empty
	// to omit it from the document rather than publish a link that goes
	// nowhere. Grid Search and Grid Message URI stay unpublished: nothing
	// serves them, and advertising them makes a viewer fail rather than skip.
	AboutURL    string
	SupportURL  string
	RegisterURL string
	PasswordURL string
	// VaultPath is the filesystem root of the asset vault (ADR 0026), holding the
	// durable bytes behind inventory-referenced assets. A relative value resolves
	// against the process working directory, as the region's data_path does.
	VaultPath string
	// StatsPath is the daily-summary CSV the grid appends one row to each day
	// and serves at /stats.csv ([grid] stats_path). Relative values resolve
	// against the working directory, like the vault.
	StatsPath string

	// Website API ([website] and [mail] sections). These configure the
	// separate browser-facing homeworldz-api binary; the grid binary ignores them.
	WebsiteAddress string
	// WebsitePublicURL is the public https:// base of the website API
	// ([website] public_url), used to derive absolute URLs such as the grid
	// channel's wss:// endpoint. Empty when the deployment has no public name.
	WebsitePublicURL      string
	WebsiteAllowedOrigins []string
	WebsiteJWTSecret      string
	WebsiteJWTIssuer      string
	WebsiteJWTAudience    string
	WebsiteTokenTTL       time.Duration
	// RegionTicketTTL is the short lifetime of region-scoped tickets minted at
	// world entry ([website] region_ticket_ttl_seconds, default 300).
	RegionTicketTTL      time.Duration
	WebsiteRatePerMinute int
	WebsiteRateBurst     int
	MailTransport        string
	MailFrom             string
	MailVerificationURL  string
	MailResetURL         string
	SMTPHost             string
	SMTPPort             int
	SMTPUsername         string
	SMTPPassword         string
	SMTPImplicitTLS      bool
}

func LoadGrid(directory string) (Grid, error) {
	resolved, err := filepath.Abs(directory)
	if err != nil {
		return Grid{}, fmt.Errorf("resolve configuration directory: %w", err)
	}

	files := []string{
		filepath.Join(resolved, "grid.ini"),
		filepath.Join(resolved, "db.ini"),
	}
	parsed, err := ini.LoadSources(ini.LoadOptions{IgnoreInlineComment: true}, files[0], files[1])
	if err != nil {
		return Grid{}, fmt.Errorf("load configuration: %w", err)
	}

	result := Grid{
		Address:      parsed.Section("server").Key("address").MustString("127.0.0.1:42000"),
		PublicURL:    parsed.Section("server").Key("public_url").MustString("http://127.0.0.1:42000"),
		Name:         strings.TrimSpace(parsed.Section("grid").Key("name").MustString("Homeworldz")),
		DatabaseURL:  parsed.Section("database").Key("url").String(),
		ServiceToken: parsed.Section("auth").Key("service_token").String(),
		WorkerToken:  parsed.Section("auth").Key("worker_token").String(),
		Directory:    resolved,
		VaultPath: strings.TrimSpace(parsed.Section("vault").Key("path").
			MustString(filepath.Join("var", "vault"))),
		StatsPath: strings.TrimSpace(parsed.Section("grid").Key("stats_path").
			MustString(filepath.Join("var", "stats.csv"))),
		WelcomeLocations: splitList(parsed.Section("grid").Key("welcome_locations").String()),
		WelcomeMessage: parsed.Section("grid").Key("welcome_message").
			MustString("Welcome to {grid}, {user}!"),
		// The four human-facing destinations a viewer's grid manager offers.
		// They are not derived from the grid's own public URL: the grid serves
		// the protocol, while these are pages on the public site and the
		// account site, which are separate deployments.
		AboutURL: strings.TrimSpace(parsed.Section("grid").Key("about_url").
			MustString("https://homeworldz.com/")),
		SupportURL: strings.TrimSpace(parsed.Section("grid").Key("support_url").
			MustString("https://homeworldz.com/faq")),
		RegisterURL: strings.TrimSpace(parsed.Section("grid").Key("register_url").
			MustString("https://my.homeworldz.com/register")),
		PasswordURL: strings.TrimSpace(parsed.Section("grid").Key("password_url").
			MustString("https://my.homeworldz.com/forgot")),
	}
	if result.VaultPath == "" {
		return Grid{}, fmt.Errorf("invalid asset vault path %q", result.VaultPath)
	}
	if result.Name == "" || len(result.Name) > 128 {
		return Grid{}, fmt.Errorf("invalid grid name %q", result.Name)
	}
	result.PublicURL = strings.TrimRight(result.PublicURL, "/")
	publicURL, err := url.Parse(result.PublicURL)
	if err != nil || (publicURL.Scheme != "http" && publicURL.Scheme != "https") || publicURL.Host == "" ||
		publicURL.User != nil || publicURL.RawQuery != "" || publicURL.Fragment != "" {
		return Grid{}, fmt.Errorf("invalid grid public URL %q", result.PublicURL)
	}

	website := parsed.Section("website")
	result.WebsiteAddress = website.Key("address").MustString("127.0.0.1:42010")
	result.WebsitePublicURL = strings.TrimRight(strings.TrimSpace(website.Key("public_url").String()), "/")
	result.WebsiteAllowedOrigins = splitList(website.Key("allowed_origins").
		MustString("https://homeworldz.com,https://www.homeworldz.com"))
	result.WebsiteJWTSecret = website.Key("jwt_secret").String()
	result.WebsiteJWTIssuer = website.Key("jwt_issuer").MustString("https://api.homeworldz.com")
	result.WebsiteJWTAudience = website.Key("jwt_audience").MustString("https://homeworldz.com")
	result.WebsiteTokenTTL = time.Duration(website.Key("token_ttl_seconds").MustInt(3600)) * time.Second
	result.RegionTicketTTL = time.Duration(website.Key("region_ticket_ttl_seconds").MustInt(300)) * time.Second
	result.WebsiteRatePerMinute = website.Key("rate_per_minute").MustInt(30)
	result.WebsiteRateBurst = website.Key("rate_burst").MustInt(10)

	mail := parsed.Section("mail")
	result.MailTransport = strings.ToLower(strings.TrimSpace(mail.Key("transport").MustString("log")))
	result.MailFrom = mail.Key("from").MustString("no-reply@homeworldz.com")
	result.MailVerificationURL = mail.Key("verification_url").MustString("https://my.homeworldz.com/verify")

	// The management origin in code, not only in a deployed grid.ini. verification_url
	// once defaulted to the website origin, where public/_redirects 302s it onward, so
	// a deployment missing the setting kept working on a redirect in another
	// repository - invisible because nothing failed (fixed in baadf82; ADR 0034).
	result.MailResetURL = mail.Key("reset_url").MustString("https://my.homeworldz.com/reset")
	result.SMTPHost = mail.Key("smtp_host").String()
	result.SMTPPort = mail.Key("smtp_port").MustInt(587)
	result.SMTPUsername = mail.Key("smtp_username").String()
	result.SMTPPassword = mail.Key("smtp_password").String()
	// Implicit TLS (SMTPS) defaults on for the conventional port 465; a relay on
	// another port can force it with smtp_implicit_tls = true.
	result.SMTPImplicitTLS = mail.Key("smtp_implicit_tls").MustBool(result.SMTPPort == 465)

	return result, nil
}

// splitList parses a comma-separated INI value into trimmed, non-empty entries.
func splitList(value string) []string {
	parts := strings.Split(value, ",")
	items := make([]string, 0, len(parts))
	for _, part := range parts {
		if trimmed := strings.TrimSpace(part); trimmed != "" {
			items = append(items, trimmed)
		}
	}
	return items
}
