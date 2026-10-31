[Documentation index](../README.md)

# Project layout

| Path | Contents |
|---|---|
| `src/` | The server: `main.c` (HTTP, routing, API), `db.c` (SQLite), `migrate.c` (migration runner), `sql.c` (SQLite helpers), `jwt.c`, `sha256.c`, `password.c` (password hashes), `login_guard.c` (login protection), `ip.c` (client addresses), `geoip.c` (countries), `str.c`, `arena.c`, `buddy.c`. Tools: `passwd.c` (`kasin-passwd`), `geoip_tool.c` and `geoip_build.c` (`kasin-geoip`), and the bundlers: `bundler.c` (shared: manifest, files, watch mode), `jsbundle.c` and `jsmin.c` (JavaScript), `cssbundle.c` and `cssmin.c` (CSS) |
| `tests/` | Unit tests, run with `ctest`: minifiers, password hashes, login protection, addresses and countries |
| `scripts/` | `fetch-geoip`, which downloads the country data |
| `js/` | JavaScript sources and `bundles.txt`: `shared/blocks.js` is the block format, `site/main.js` the public site, `admin/admin.js` the admin pages, `admin/editor.js` the editor, `admin/gallery.js` the image gallery |
| `migrations/` | Database migrations, `<number>__<name>.sql` |
| `css/` | CSS sources and `bundles.txt`: `shared/style.css` is the site's style, `admin/admin.css` the admin view's |
| `public/` | The site: pages, `assets/`, and the generated bundles in `js/` and `css/`; `hallinta/` holds the admin pages |
| `Dockerfile` | Production image (used for Azure) |
| `Dockerfile.dev`, `dev.docker-compose.yml` | Local development and the deployment tooling container |
| `deploy/` | `bin/deploy` (the deployment script), `config.env` (its settings), `config.local.env` (your subscription and tenant; not in version control) and `secrets.env` (the live admin password hash; not in version control) |
| `docs/` | This documentation; the index is `README.md` |
| `CLAUDE.md` | C and JavaScript code style rules for this project |
