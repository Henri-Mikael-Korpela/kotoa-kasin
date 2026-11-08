[Documentation index](../README.md)

# Project layout

| Path | Contents |
|---|---|
| `src/` | The server: `main.c` (HTTP, routing, API), `db.c` (SQLite), `migrate.c` (migration runner), `sql.c` (SQLite helpers), `jwt.c`, `sha256.c`, `password.c` (password hashes), `login_guard.c` (login protection), `ip.c` (client addresses), `geoip.c` (countries), `gzip.c` (compresses public pages for browsers that accept it), `str.c`, `arena.c`, `buddy.c`. Tools: `passwd.c` (`kasin-passwd`), `geoip_tool.c` and `geoip_build.c` (`kasin-geoip`), and the bundlers: `bundler.c` (shared: manifest, files, watch mode), `jsbundle.c` and `jsmin.c` (JavaScript), `cssbundle.c` and `cssmin.c` (CSS). `htmlgen.c` (`kasin-htmlgen`) and `template.c` make the HTML pages from `templates/` |
| `tests/` | Unit tests, run with `ctest`: minifiers, the template engine, gzip, password hashes, login protection, addresses and countries |
| `scripts/` | `fetch-geoip`, which downloads the country data |
| `js/` | JavaScript sources and `bundles.txt`: `shared/blocks.js` is the block format, `site/main.js` the public site, `admin/admin.js` the admin pages, `admin/editor.js` the editor, `admin/gallery.js` the image gallery |
| `migrations/` | Database migrations, `<number>__<name>.sql` |
| `css/` | CSS sources and `bundles.txt`: `shared/style.css` is the site's style, `admin/admin.css` the admin view's |
| `templates/` | HTML sources: `pages/` has one template per page (`hallinta/` for the admin pages) and `fragments/` the parts they include, such as the head, header and footer |
| `public/` | The site: `assets/` and the generated pages and bundles (`*.html`, `js/` and `css/`); `hallinta/` holds the admin pages |
| `Dockerfile` | Production image (used for Azure) |
| `Dockerfile.dev`, `dev.docker-compose.yml` | Local development and the deployment tooling container |
| `deploy/` | `bin/deploy` (the deployment script), `config.env` (its settings), `config.local.env` (your subscription and tenant; not in version control) and `secrets.env` (the live admin password hash; not in version control) |
| `.github/workflows/ci.yml`, `.clang-tidy` | The GitHub Actions checks and the static analysis rules (see [Continuous integration](continuous-integration.md)) |
| `docs/` | This documentation; the index is `README.md` |
| `CLAUDE.md` | Pointer to the C style guide and the JavaScript code style rules for this project |
