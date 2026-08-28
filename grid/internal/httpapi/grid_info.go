package httpapi

import (
	_ "embed"
	"encoding/base64"
	"encoding/xml"
	"html"
	"net/http"
)

//go:embed assets/homeworldz.svg
var homeworldzLogo []byte

var homeworldzLogoDataURL = "data:image/svg+xml;base64," +
	base64.StdEncoding.EncodeToString(homeworldzLogo)

// viewerGridInfo is the get_grid_info document. The first six fields are
// protocol endpoints served by this grid; the last four are pages a person
// opens, which is why they come from configuration rather than the grid's own
// public URL — the public site and the account site are separate deployments.
//
// Firestorm's grid manager shows them as Grid URI (login), Login Page
// (welcome), Helper URI (helperuri), Grid Website (about), Grid Support
// (help), Grid Registration (register) and Grid Password URI (password).
//
// login is the XML-RPC login_to_simulator endpoint, not a sign-in page. Its
// resemblance to the website's own login page is a trap: pointing it at the
// site would break viewer login entirely.
//
// Grid Search and Grid Message URI have no fields here on purpose. Nothing
// serves them, and a viewer told where to search fails against a dead URL
// instead of quietly doing without.
type viewerGridInfo struct {
	XMLName  xml.Name `xml:"gridinfo"`
	GridNick string   `xml:"gridnick"`
	GridName string   `xml:"gridname"`
	Platform string   `xml:"platform"`
	Login    string   `xml:"login"`
	Welcome  string   `xml:"welcome"`
	Helper   string   `xml:"helperuri"`
	About    string   `xml:"about,omitempty"`
	Help     string   `xml:"help,omitempty"`
	Register string   `xml:"register,omitempty"`
	Password string   `xml:"password,omitempty"`
}

func (a *API) gridInfo(w http.ResponseWriter, _ *http.Request) {
	contents, err := xml.Marshal(viewerGridInfo{
		GridNick: a.gridNick,
		GridName: a.gridName,
		Platform: "OpenSim",
		Login:    a.publicURL + "/login",
		Welcome:  a.publicURL + "/welcome",
		Helper:   a.publicURL + "/",
		About:    a.aboutURL,
		Help:     a.supportURL,
		Register: a.registerURL,
		Password: a.passwordURL,
	})
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "grid_info_error", Message: "grid information is unavailable"})
		return
	}
	w.Header().Set("Content-Type", "application/xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(append([]byte(xml.Header), contents...))
}

func (a *API) welcome(w http.ResponseWriter, _ *http.Request) {
	gridName := html.EscapeString(a.gridName)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; img-src data:; style-src 'unsafe-inline'")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>` + gridName + `</title>
<style>
html,body{height:100%;margin:0}body{display:grid;place-items:center;background:#071923;color:#eaf7fb;font:16px system-ui,sans-serif;text-align:center}.panel{padding:2rem}.logo{display:block;width:min(80vw,28rem);height:auto;margin:0 auto 1.5rem}p{color:#b8d6df}
</style>
</head>
<body><main class="panel"><img class="logo" src="` + homeworldzLogoDataURL + `" alt="` + gridName + `"><p>Welcome to ` + gridName + `.</p></main></body>
</html>`))
}

func (a *API) logo(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "image/svg+xml; charset=utf-8")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(homeworldzLogo)
}
