# Kotoa käsin

Website for homemade cosmetics and cooking recipes, served by a small HTTP
server written in C. Recipes and categories are stored in SQLite and managed
from an admin view at `/hallinta`.

- **Server:** C, Linux only. epoll, one worker per thread, all memory reserved
  at startup, no `malloc` in the request path. Uses SQLite as its only library.
- **Site:** static pages in `public/`; the front page loads recipes from the API.
- **Admin:** log in at `/hallinta/kirjaudu`, then add, edit and delete recipes
  and categories.

## Run locally

Requires Docker (Docker Desktop with the WSL2 backend on Windows).

```sh
docker compose -f dev.docker-compose.yml up --build
```

Then open <http://localhost:8080>. The admin view is at
<http://localhost:8080/hallinta>, with the user name `hanna` and the password
you set as follows. Until you do, the site runs but nobody can log in.

1. Make the password hash into a file named `.env` next to
   `dev.docker-compose.yml`. `kasin-passwd -g` makes a strong random password,
   shows it once and writes only its hash to the file (PowerShell):

   ```powershell
   docker compose -f dev.docker-compose.yml exec -T httpd /build/kasin-passwd -g | Out-File -Encoding ascii .env
   ```

   To choose the password yourself, run `docker compose -f dev.docker-compose.yml
   exec httpd /build/kasin-passwd` instead: it asks twice (at least 12
   characters; a few unrelated words work well) and prints a
   `KASIN_ADMIN_PASSWORD_HASH=...` line to paste into `.env`.
2. Restart: `docker compose -f dev.docker-compose.yml up -d`.

`.env` is in `.gitignore`; never commit it. See [Admin password](#admin-password).

- The server is compiled inside the container when it starts. After changing C
  code, run `docker compose -f dev.docker-compose.yml restart httpd`.
- Files in `public/` are served directly; changes show up on reload.
- The database lives in the `db-data` Docker volume (`/data/kotoa.db` in the
  container) and survives restarts. `docker compose down -v` deletes it.

To reach the dev server from a phone on the same network, allow TCP 8080 in
Windows Firewall and open `http://<PC address>:8080`.

## Deploy to Azure

The site is deployed to **Azure Container Apps** (0.25 vCPU, 0.5 GiB, scaled to
zero when idle, HTTPS included) by a tooling container that runs the Azure CLI.
The server image is built in Azure, and the database lives on an Azure Files
share, so it survives restarts and new deployments.

### What you need before the first deployment

| What | Required | Where it goes |
|---|---|---|
| An **Azure account with an active subscription** (pay-as-you-go, or a plan with credits) | Yes | You sign in with it when `deploy` asks (see below) |
| The **Owner** or **Contributor** role on that subscription | Yes | Needed to register resource providers and create resources |
| **Subscription name or id** | Only if the account has more than one subscription | `AZURE_SUBSCRIPTION` in `deploy/config.env` |
| **Directory (tenant)** of the subscription | Only if the subscription is not in the account's default directory | `AZURE_TENANT` in `deploy/config.env` |
| **Region** | No (default `swedencentral`) | `AZURE_LOCATION` in `deploy/config.env` |
| **Resource names** | No (defaults work) | The other settings in `deploy/config.env` |

You do not need to create anything in Azure by hand, and there are no keys or
connection strings to copy: the script creates all resources and the secrets
the app needs.

### Settings in `deploy/config.env`

Edit the file on your machine; the tooling container sees the change
immediately.

| Setting | Default | Notes |
|---|---|---|
| `AZURE_SUBSCRIPTION` | *(empty)* | Name or id. Empty uses the subscription `az login` selected, which is fine when you have just one. |
| `AZURE_TENANT` | *(empty)* | The directory that holds the subscription, as a domain (`example.onmicrosoft.com`) or tenant id. A Microsoft account can belong to several directories and `az login` signs in to only one; set this when `deploy` cannot find your subscription. |
| `AZURE_LOCATION` | `swedencentral` | Region for every resource. It must support Container Apps, for example `swedencentral`, `northeurope` or `westeurope`. |
| `AZURE_RESOURCE_GROUP` | `kotoa-kasin` | Everything is created inside this group. |
| `AZURE_CONTAINERAPP_ENV` | `kotoa-kasin-env` | Container Apps environment. |
| `AZURE_CONTAINERAPP_NAME` | `kotoa-kasin` | Lowercase letters, digits and `-`, up to 32 characters. Part of the site address. |
| `IMAGE_NAME` | `kotoa-kasin` | Name of the image in the registry. |
| `AZURE_ACR_NAME` | *(empty)* | Container registry. Globally unique, 5-50 letters and digits. Empty gives `kotoakasin` plus 8 characters of your subscription id. |
| `AZURE_STORAGE_ACCOUNT` | *(empty)* | Storage account. Globally unique, 3-24 lowercase letters and digits. Empty gives the same default as the registry. |
| `AZURE_FILE_SHARE` | `kotoa-data` | File share holding the database. 3-63 lowercase letters, digits and `-`. |

The defaults work as they are, so for most people the file needs no changes.
Set the two globally unique names yourself only if the defaults are already
taken, which Azure reports with a "name not available" error. Names and region
identify existing resources: changing them after the first deployment makes the
script create a second set rather than rename the first.

### Deploy

Before the first deployment, make the live site's admin password hash and save
it in `deploy/secrets.env` (never committed; see [Admin password](#admin-password)):

```powershell
docker compose -f dev.docker-compose.yml up -d
docker compose -f dev.docker-compose.yml exec -T httpd /build/kasin-passwd -g | Out-File -Encoding ascii deploy/secrets.env
```

`-g` shows the new random password once; keep it in a password manager. Then:

```sh
docker exec -it kotoa-kasin-deploy deploy
```

1. **Sign in.** The first run prints a link (`https://login.microsoft.com/device`)
   and a code. Open the link on any device, enter the code and sign in with the
   Azure account. The sign-in is stored in the `azure-login` volume, so later
   runs skip this.
2. **Wait.** The first deployment creates the resource group, container
   registry, storage account and file share, Container Apps environment and the
   app. It takes roughly 10 minutes; later deployments only build and roll out a
   new image.
3. **Open the site.** The script finishes by printing the address, like
   `https://kotoa-kasin.<random>.swedencentral.azurecontainerapps.io`, and the
   admin address (`/hallinta`).

Run the same command again whenever you want to publish changes to `src/` or
`public/`.

### What gets created

All in the resource group from `deploy/config.env` (default `kotoa-kasin`):

| Resource | Purpose |
|---|---|
| Container registry (Basic) | Holds the server images; also builds them (`az acr build`) |
| Storage account + file share `kotoa-data` | The SQLite database, mounted at `/data` |
| Container Apps environment | Hosting platform for the app (no Log Analytics) |
| Container app | The server, at most 1 replica |

### After the first deployment

- **Admin password:** the live site uses the hash in `deploy/secrets.env`
  (see [Admin password](#admin-password)). To change it, put a new hash there and
  deploy again.
- **The database starts from the seeded recipes** (migration `0002`). Recipes from your local
  database are not copied. It is stored on the file share with `-s share`
  (see Server options below).
- **Sessions:** a random signing secret (`KASIN_JWT_SECRET`) is created on the
  first deployment and stored as an app secret, so logins survive restarts.
- **Cold starts:** when idle, the app scales to zero and the first request
  after a quiet period takes a few seconds. To keep it always on:
  `az containerapp update --name kotoa-kasin --resource-group kotoa-kasin --min-replicas 1`
  (run it inside the tooling container; this costs an always-on replica).

### Useful commands

Run these inside the tooling container
(`docker exec -it kotoa-kasin-deploy sh`), or prefix them with
`docker exec -it kotoa-kasin-deploy`:

```sh
# Live server log
az containerapp logs show --name kotoa-kasin --resource-group kotoa-kasin --follow

# Site address
az containerapp show --name kotoa-kasin --resource-group kotoa-kasin \
  --query properties.configuration.ingress.fqdn --output tsv

# Remove everything that was created, including the database
az group delete --name kotoa-kasin
```

### Cost

The container registry (Basic) is the only fixed cost, about 5 €/month. Storage
for a small database costs cents, and a scaled-to-zero app usually stays within
the Container Apps free monthly allowance. Check current prices in the Azure
portal before deploying.

### Troubleshooting

| Problem | Cause and fix |
|---|---|
| An error saying the registry or storage account name is already in use or not available | Set `AZURE_ACR_NAME` / `AZURE_STORAGE_ACCOUNT` in `deploy/config.env` to something unique and run `deploy` again. |
| An error from `az acr build` saying that tasks are not permitted or are paused | Free and trial subscriptions can block building images in Azure. Build the image locally and push it to the registry instead. |
| `AuthorizationFailed` | Your account lacks the Owner or Contributor role on the subscription. |
| `The subscription of '...' doesn't exist in cloud 'AzureCloud'` | The subscription is in another directory than the one `az login` used. Set `AZURE_TENANT` in `deploy/config.env` to that directory's domain (shown in the portal under Directories + subscriptions) and run `deploy` again. |
| The site hangs and `az containerapp logs show` prints `db: database is locked` | The database is on the file share but the server was not started with `-s share`, so SQLite used POSIX locks the share does not support. The production `Dockerfile` already passes `-s share`; redeploy. |
| `deploy` reports no healthy answer at the end | Look at the log with `az containerapp logs show ... --follow`; the app may still be starting. |
| `docker exec` says the container is not running | The tooling container is stopped. Start it with `docker compose -f dev.docker-compose.yml up -d`. |

## Admin password

The admin logs in as `hanna` with a password that is never stored anywhere:
the server only knows a salted, slow hash of it (PBKDF2-HMAC-SHA256, 600,000
iterations, a random 16-byte salt), given in the environment variable
`KASIN_ADMIN_PASSWORD_HASH`:

```
KASIN_ADMIN_PASSWORD_HASH=pbkdf2-sha256:<iterations>:<salt>:<hash>
```

- **Making a hash:** `kasin-passwd` (built with the server) asks for the
  password twice and prints the line above; `kasin-passwd -g` makes a strong
  random password instead and shows it once. In the dev container:
  `docker compose -f dev.docker-compose.yml exec httpd /build/kasin-passwd`.
  When writing its output to a file, pass `-T` to `exec`: without it the shown
  password would end up in the file too. In Windows PowerShell 5.1 use
  `| Out-File -Encoding ascii <file>` rather than `>`, which writes UTF-16 that
  Compose cannot read. In Git Bash, prefix the command with `MSYS_NO_PATHCONV=1`
  so `/build/...` is not turned into a Windows path.
- **Local development:** the line goes in `.env` next to
  `dev.docker-compose.yml`; Compose passes it to the server.
- **Azure:** the line goes in `deploy/secrets.env`. `deploy` stores it as the
  Container Apps secret `admin-password-hash`, which the app reads as
  `KASIN_ADMIN_PASSWORD_HASH`. The first deployment needs it; later ones keep the
  stored secret unless the file holds a hash (then it is replaced). Use a
  different, strong password for the live site than for local development.
- **Never commit either file;** both are in `.gitignore`. A salted, slow hash
  is still open to guessing by anyone who has it, only slowly.
- **Without the variable** the site runs, logins answer 503 and the log says
  why. A malformed value stops the server at startup (exit code 78).

Each login takes about 0.7 seconds of processor time on a desktop computer (and
a few seconds on the smallest Azure size): that is what makes guessing slow.

## Database migrations

The database structure is defined by SQL files in `migrations/`, applied in
order when the server starts:

| File | Contents |
|---|---|
| `0001__create_recipe_tables.sql` | The `categories` and `recipes` tables |
| `0002__seed_initial_recipes.sql` | Starter content: two categories and the original recipes |
| `0003__create_images_table.sql` | The `images` table for uploaded images |
| `0004__allow_longer_recipe_descriptions.sql` | Descriptions up to 50,000 characters, for editor blocks |
| `0005__add_recipe_slugs.sql` | The unique `slug` of each recipe, made from the names of existing recipes |

**Naming:** `<number>__<name>.sql`, for example `0003__add_servings_to_recipes.sql`:
four digits, two underscores, then lowercase letters, digits and underscores.
Other files in the directory are ignored.

**How it works:**

- Each migration runs once, in its own transaction. If it fails, nothing from
  it is kept and the server does not start, so fix the file and restart.
- Applied migrations are recorded in the `schema_migrations` table
  (`version`, `name`, `applied_at`). The database's current version is the
  highest `version` there.
- Several servers starting at once (as during an Azure deployment) are safe:
  one applies the migrations, the others wait and find nothing left to do.

**Changing the schema:** add a new file with the next number. Do not edit,
rename or renumber a file that has been applied anywhere. The server refuses
to start if:

- an applied migration's file was renamed or renumbered;
- a new file has a number below the database's current version (two branches
  both adding `0003`, say): renumber the new file above the current version;
- the database is newer than the newest file (an older server, newer database).

Migration files must not contain `BEGIN` or `COMMIT`. Foreign key enforcement
is off while migrations run, so tables can be rebuilt; the server checks all
foreign keys once they have finished.

Locally, `docker compose -f dev.docker-compose.yml restart httpd` applies new
migrations. On Azure, `deploy` copies `migrations/` into the image, and they
run when the new version starts.

Databases created before `migrations/` existed (schema version 2) are adopted
on first start: `0001` and `0002` are recorded as applied without running.

## Recipe editor and images

Recipe descriptions are written in a block editor on the recipe's edit page in
the admin view, in the spirit of WordPress Gutenberg:

| Block | Name in storage | Options |
|---|---|---|
| Paragraph | `paragraph` | none; line breaks are kept |
| Title | `title` | `level`: 2, 3 or 4 (H2-H4; H1 is the recipe name) |
| Image | `image` | `src` (image address), `alt` (description for screen readers), `caption` (optional); how it is shown: `crop` (`x`, `y`, `w`, `h` as fractions of the picture), `width` and `height` (the picture's pixel size, needed with a crop), `scale` (width on the page, 25-100 % of the text column) |
| Layout | `layout` | two columns side by side; each holds a paragraph, title or image |

Add blocks from the bar under the list, drag them by the ⠿ handle (mouse or
touch) or move them with the arrow buttons, and remove them with ✕. In a layout
block, ⇄ swaps the two columns.

**Muokkaa kuvan esitystapaa** on an image block opens the image display editor: drag the
frame's corners to crop, drag inside the frame (or use the arrow keys) to
choose which part of the picture is centred, and set the image's size on the
page with the slider. The uploaded file is never changed; the settings are
stored as the block's options, and **Palauta alkuperäinen** removes them.

A live preview shows the recipe as its public page will look, updated with
every change to the name, category, cover image or blocks. On screens wider
than 1100 pixels the form is on the left and the preview on the right; on
narrower screens the **Muokkaa** and **Esikatselu** tabs switch between them.

**Storage format.** The description stays plain text in the `recipes` table.
Each block is marked with HTML comments, so blocks stay distinct from ordinary
text:

```
<!-- block:title {"level":2} -->
Ainekset
<!-- /block:title -->

<!-- block:paragraph -->
Sulata mehiläisvaha vesihauteessa.
<!-- /block:paragraph -->

<!-- block:image {"src":"/uploads/3.jpg","alt":"Valmis voide"} /-->

<!-- block:layout -->
<!-- block:image {"src":"/uploads/4.jpg","alt":""} /-->
<!-- block:paragraph -->
Teksti kuvan oikealla puolella.
<!-- /block:paragraph -->
<!-- /block:layout -->
```

- Options are a JSON object after the block name, with `<` and `>` written as
  `\u003c` and `\u003e` so they can never end the comment.
- In block text, `\` is stored as `\\` and `<!--` as `<\!--`, so text can never
  start a marker.
- An unused layout column is `<!-- block:empty /-->`.
- Text outside any marker (descriptions written before the editor) reads as
  paragraphs, split at blank lines.
- Parsing, writing and rendering live in `js/shared/blocks.js`, shared by the
  editor and the public recipe page. Unknown blocks are skipped, and text is
  always inserted as text, never as HTML.

**Slugs.** Each recipe has a slug, the last part of its page address
`/reseptit/<slug>`: lowercase letters a-z, digits and single dashes, at most 100
characters, unique among recipes (the database enforces all of this). In the
editor the slug follows the name (ä becomes a, spaces become dashes) until you
edit it yourself. Changing a slug changes the page address, so links to the
old address stop working.

Descriptions may be up to 50,000 characters. Lists (`GET /api/recipes`) carry
only an `excerpt`, the text of the first paragraph block, to stay small.

**Images.** Upload images on the **Kuvat** page in the admin view (button, or
drag files onto the page), or straight from the image picker in the editor. The
Kuvat page shows how many images there are and how much disk space their files
take in total; clicking an image shows its details (name, file type, size,
dimensions, upload date, use) in the panel beside the images.

- The browser scales each image down to at most 1600 pixels on the longer side
  and saves it as JPEG before uploading. That keeps files small and removes the
  metadata phones store in photos, such as location.
- Files are stored in the uploads directory (`-u`, `/data/uploads` in Docker
  and on Azure) as `<id>.jpg`, served at `/uploads/<id>.jpg` with permanent
  caching (ids are never reused), and listed in the `images` table.
- An image that a recipe shows (as its card image or in a block) cannot be
  deleted; the gallery shows how many recipes use each image.
- In the image picker, a click shows the image's details (name, file type,
  size, dimensions, upload date, use) in the panel on the right, and a double
  click chooses it. Hovering over an image shows an X that deletes it. On a
  touch screen a tap shows the details, a double tap chooses, and holding an
  image opens a menu: Käytä kuvaa, Kuvan tiedot, Poista.

**Large requests.** Requests normally use each connection's fixed 8 KiB
buffer. Uploads and long descriptions borrow one of a few 2 MiB buffers that
each worker reserves at startup, so nothing is allocated while the server
runs. Only signed-in requests may borrow one, and the connection closes after
such a request so the buffer is free again at once. When every buffer is busy,
the server answers 503 and the browser can retry.

## JavaScript and CSS bundles

The JavaScript and CSS sources live in `js/` and `css/`, outside the served
directory. The browser gets bundles built from them by two small C programs
built with the server, `kasin-jsbundle` and `kasin-cssbundle`:

| Bundle | Pages | Sources |
|---|---|---|
| `/js/site.js` | The public site | `js/shared/blocks.js`, `js/site/main.js` |
| `/js/admin.js` | The admin view (`/hallinta`) | `js/shared/blocks.js`, `js/admin/admin.js`, `gallery.js`, `editor.js` |
| `/css/site.css` | The public site | `css/shared/style.css` |
| `/css/admin.css` | The admin view (`/hallinta`) | `css/shared/style.css`, `css/admin/admin.css` |

`js/bundles.txt` and `css/bundles.txt` list the bundles and their sources in
order (for CSS that is the cascade order: later files win). The public and
admin code are kept apart:

- A bundle may only use files from `shared/` and from the directory named after
  it, so admin code or styles can never end up in a public bundle (or the other
  way round); the tools refuse such a manifest.
- Each JavaScript bundle runs in its own function scope in strict mode: nothing
  it declares becomes a global, so the two bundles cannot see or overwrite each
  other's names. All files of one bundle share that scope, so the tool refuses
  to build a bundle in which two files declare the same top-level name.
- A CSS source may not use `@import` or `@charset`, which only work at the
  start of a style sheet and would break once files are joined; list the file
  in the manifest instead. `url()` values are not rewritten: write them relative
  to `/css/`, where the bundles are served.

The bundles are minified without changing what they mean:

- **JavaScript:** comments and whitespace are removed, keeping a space or line
  break only where the code needs one, and optional semicolons and trailing
  commas are dropped. Names are never changed. About 40 % smaller.
- **CSS:** comments and whitespace are removed, keeping a space only where it
  matters (`a :hover`, `and (`, around `+` and `-` for `calc()`), the last `;`
  of each rule is dropped and `0.5` is written `.5`. Strings, `url()` values and
  custom property values (`--name: ...`) are kept as written. About 29 %
  smaller; every element of every page gets exactly the same computed styles.

Bundles are built automatically:

- **Dev container:** two watchers (`kasin-jsbundle -w`, `kasin-cssbundle -w`)
  build them at start and again whenever you save a file in `js/` or `css/` or
  change a `bundles.txt`. Reload the page to see the change; no restart is
  needed. A mistake such as an unclosed string or brace is reported in the
  container log (`docker compose -f dev.docker-compose.yml logs -f httpd`) and
  the previous bundles stay in place until the next successful build. The
  watchers poll file times, because file-change events do not cross the Windows
  bind mount.
- **Production image:** the `Dockerfile` builds them, after running the
  minifiers' tests (`tests/jsmin_test.c`, `tests/cssmin_test.c`).

The bundles in `public/js/` and `public/css/` are generated and not in version
control (see `.gitignore`); edit the sources in `js/` and `css/` instead. Both
tools take the same options:

```sh
kasin-cssbundle -o public/css css/bundles.txt      # minified
kasin-cssbundle -n -o public/css css/bundles.txt   # sources as they are, for debugging
kasin-cssbundle -w -o public/css css/bundles.txt   # keep running, rebuild on change
```

## Project layout

| Path | Contents |
|---|---|
| `src/` | The server: `main.c` (HTTP, routing, API), `db.c` (SQLite), `migrate.c` (migration runner), `sql.c` (SQLite helpers), `jwt.c`, `sha256.c`, `str.c`, `arena.c`, `buddy.c`. Also the bundlers: `bundler.c` (shared: manifest, files, watch mode), `jsbundle.c` and `jsmin.c` (JavaScript), `cssbundle.c` and `cssmin.c` (CSS) |
| `tests/` | `jsmin_test.c` and `cssmin_test.c`, tests for the minifiers (`ctest`) |
| `js/` | JavaScript sources and `bundles.txt`: `shared/blocks.js` is the block format, `site/main.js` the public site, `admin/admin.js` the admin pages, `admin/editor.js` the editor, `admin/gallery.js` the image gallery |
| `migrations/` | Database migrations, `<number>__<name>.sql` |
| `css/` | CSS sources and `bundles.txt`: `shared/style.css` is the site's style, `admin/admin.css` the admin view's |
| `public/` | The site: pages, `assets/`, and the generated bundles in `js/` and `css/`; `hallinta/` holds the admin pages |
| `Dockerfile` | Production image (used for Azure) |
| `Dockerfile.dev`, `dev.docker-compose.yml` | Local development and the deployment tooling container |
| `deploy/` | `bin/deploy` (the deployment script) and `config.env` (its settings) |
| `CLAUDE.md` | C code style rules for this project |

### Server options

```
kasin-httpd [-p port] [-t threads] [-r docroot] [-d database] [-m migrations]
            [-u uploads] [-s local|share]
```

`-u` names the directory uploaded images are stored in (default `uploads`,
created if missing).

`-m` names the migrations directory (default `migrations`, relative to the
working directory).

`-s share` is for a database on a network share such as Azure Files (used in
the Azure deployment). SQLite cannot rely on POSIX file locks or WAL there, so
it locks with a `<database>.lock` directory and keeps a rollback journal.
`-s local` (the default) uses normal locking and WAL. Memory limits are set at build time through the
CMake options `KASIN_MAX_CONNS_PER_WORKER`, `KASIN_REQ_BUF_SIZE`,
`KASIN_RESP_BUF_SIZE`, `KASIN_BIG_BUF_SIZE` (largest upload or response,
default 2 MiB), `KASIN_BIG_BUFS_PER_WORKER` (default 2) and `KASIN_DB_HEAP_SIZE`.

### API

Reading is public; changes need a login token (`Authorization: Bearer <token>`).

| Endpoint | Purpose |
|---|---|
| `POST /api/login` | `{username, password}` returns `{token, expires_at}` |
| `GET /api/session` | Checks the token (login required) |
| `GET`, `POST /api/recipes` | List (with `excerpt` instead of `description`), create (`slug`, `name`, `image`, `description`, `category_id`; 409 if another recipe has the slug) |
| `GET`, `PUT`, `DELETE /api/recipes/{id}` | Read, update (409 if another recipe has the slug), delete |
| `GET /api/recipes/slug/{slug}` | Read a recipe by its slug |
| `GET`, `POST /api/categories` | List (alphabetical), create (`name`) |
| `GET`, `PUT`, `DELETE /api/categories/{id}` | Read, rename, delete (409 while recipes use it) |
| `GET /api/images` | List uploaded images with how many recipes use each (`used`) and which ones (`recipes`: `{id, name}`, alphabetically) (login required) |
| `POST /api/images` | Upload: the raw JPEG, PNG, GIF or WebP file as the body, original name in `X-File-Name` (login required) |
| `DELETE /api/images/{id}` | Delete (login required; 409 while a recipe shows it) |
