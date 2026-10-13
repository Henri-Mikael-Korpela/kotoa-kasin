# Kotoa käsin

Website for homemade cosmetics and cooking recipes, served by a small HTTP
server written in C. Recipes and categories are stored in SQLite and managed
from an admin view at `/hallinta`.

- **Server:** C, Linux only. epoll, one worker per thread, all memory reserved
  at startup, no `malloc` in the request path. Uses SQLite as its only library.
- **Site:** static pages in `public/`; the front page loads recipes from the API.
- **Recipe pages:** every recipe has a page at `/recipes/<slug>`, built from
  the blocks of its description. The slug is the recipe's unique, editable
  address (for example `/recipes/laventelisaippua`); an unknown slug answers
  404. Old `/resepti?id=<id>` links still work and switch to the slug address.
- **Admin:** log in at `/hallinta/kirjaudu`, then add, edit and delete recipes
  and categories.

## Run locally

Requires Docker (Docker Desktop with the WSL2 backend on Windows).

```sh
docker compose -f dev.docker-compose.yml up --build
```

Then open <http://localhost:8080>. The admin view is at
<http://localhost:8080/hallinta>; the login is `hanna` / `koti`.

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

```sh
docker compose -f dev.docker-compose.yml up -d
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

- **Change the admin password.** The login is still the hard-coded
  `hanna` / `koti`, set as `ADMIN_USERNAME` and `ADMIN_PASSWORD` in
  `src/main.c`. Change them and deploy again before sharing the address.
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
| Image | `image` | `src` (image address), `alt` (description for screen readers), `caption` (optional) |
| Layout | `layout` | two columns side by side; each holds a paragraph, title or image |

Add blocks from the bar under the list, drag them by the ⠿ handle (mouse or
touch) or move them with the arrow buttons, and remove them with ✕. In a layout
block, ⇄ swaps the two columns.

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
- Parsing, writing and rendering live in `public/js/blocks.js`, shared by the
  editor and the public recipe page. Unknown blocks are skipped, and text is
  always inserted as text, never as HTML.

**Slugs.** Each recipe has a slug, the last part of its page address
`/recipes/<slug>`: lowercase letters a-z, digits and single dashes, at most 100
characters, unique among recipes (the database enforces all of this). In the
editor the slug follows the name (ä becomes a, spaces become dashes) until you
edit it yourself. Changing a slug changes the page address, so links to the
old address stop working.

Descriptions may be up to 50,000 characters. Lists (`GET /api/recipes`) carry
only an `excerpt`, the text of the first paragraph block, to stay small.

**Images.** Upload images on the **Kuvat** page in the admin view (button, or
drag files onto the page), or straight from the image picker in the editor.

- The browser scales each image down to at most 1600 pixels on the longer side
  and saves it as JPEG before uploading. That keeps files small and removes the
  metadata phones store in photos, such as location.
- Files are stored in the uploads directory (`-u`, `/data/uploads` in Docker
  and on Azure) as `<id>.jpg`, served at `/uploads/<id>.jpg` with permanent
  caching (ids are never reused), and listed in the `images` table.
- An image that a recipe shows (as its card image or in a block) cannot be
  deleted; the gallery shows how many recipes use each image.

**Large requests.** Requests normally use each connection's fixed 8 KiB
buffer. Uploads and long descriptions borrow one of a few 2 MiB buffers that
each worker reserves at startup, so nothing is allocated while the server
runs. Only signed-in requests may borrow one, and the connection closes after
such a request so the buffer is free again at once. When every buffer is busy,
the server answers 503 and the browser can retry.

## Project layout

| Path | Contents |
|---|---|
| `src/` | The server: `main.c` (HTTP, routing, API), `db.c` (SQLite), `migrate.c` (migration runner), `sql.c` (SQLite helpers), `jwt.c`, `sha256.c`, `str.c`, `arena.c`, `buddy.c` |
| `migrations/` | Database migrations, `<number>__<name>.sql` |
| `public/` | The site: pages, `css/`, `js/`, `assets/`; `hallinta/` holds the admin pages. `js/blocks.js` is the block format, `js/editor.js` the editor, `js/gallery.js` the image gallery |
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
| `GET /api/images` | List uploaded images with how many recipes use each (login required) |
| `POST /api/images` | Upload: the raw JPEG, PNG, GIF or WebP file as the body, original name in `X-File-Name` (login required) |
| `DELETE /api/images/{id}` | Delete (login required; 409 while a recipe shows it) |
