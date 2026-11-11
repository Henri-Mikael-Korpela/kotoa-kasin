[Documentation index](../README.md)

# Deploy to Azure

## Rationale

This website was intended to run on a small Linux server with minimal resources. I wanted to see how this would work out and to also learn more about cloud services via Azure.

The usual way to build a site like this is with a framework like NodeJS, Spring Boot (Java
or Kotlin), ASP.NET Core (.NET) or Go's standard library. This project uses a
small HTTP server written in C instead, with SQLite as its only library, on
three Azure services. There are some benefits to this:

- **One library.** The only code from elsewhere is SQLite and the C library;
  a minimal Spring Boot web service brings 34 libraries (Tomcat, Jackson,
  Logback, SnakeYAML, Micrometer and Spring's own), each one more thing to
  track for security fixes. Spring Framework itself had a critical remote code
  execution flaw in 2022
  ([CVE-2022-22965](https://spring.io/security/cve-2022-22965), "Spring4Shell"),
  exploitable when deployed as a WAR file on Tomcat, which Spring Boot's
  default packaging is not.
- **A well-tested dependency.** SQLite has 590 times as much test code as
  library code, with 100% branch coverage of its core
  ([How SQLite Is Tested](https://www.sqlite.org/testing.html)).
- **No database server.** The database is a file in the same process: no
  separate server to run, pay for, patch or connect to. Since regular visitors of the site don't need to update anything and the traffic is low, this is a very good trade-off.

The aforementioned benefits are not without their downsides, though. Here are some trade-offs:

- **Memory safety.** C does not prevent buffer overflows or use of freed
  memory. About 70% of the security bugs Microsoft assigns a CVE each year
  ([MSRC, 2019](https://www.microsoft.com/en-us/msrc/blog/2019/07/a-proactive-approach-to-more-secure-code))
  and about 70% of Chromium's high-severity security bugs
  ([Chromium](https://www.chromium.org/Home/chromium-security/memory-safety/))
  are memory-safety problems. This server limits the risk (fixed buffers
  reserved at startup, strings that carry their length, unit tests), but Go,
  C#, Java and Kotlin rule out that whole class of bug.
- **Everything is written "by hand".** Not really, since I used Claude Code a lot, but about 8,500 lines of C plus 700 lines of
  tests cover what a framework provides: HTTP parsing and routing, sessions
  (JWT), SHA-256 and password hashing, the JavaScript and CSS minifiers, the
  HTML template engine, the country lookup. Handwritten code can be efficient and solve particular problems but better but now I am responsible for bugs many other framworks would have solved already.
- **Native compilation narrows the gap.** Spring Boot with GraalVM native
  images and .NET with Native AOT start in a fraction of a second and use less
  memory, at the cost of longer builds and some restrictions.
- **Fixed limits.** For now, memory (about 80 MiB with two workers), connections and
  request sizes are set when the server is built; changing them means
  rebuilding.
- **Features and scaling.** One replica only (SQLite on a file share allows
  one writer), HTTP/1.1 only (Azure's ingress provides HTTPS and HTTP/2),
  Linux only, and no ready-made metrics, tracing or single sign-on. In Spring
  or .NET each of these is a library away; here each would be work of its own.
- **People.** Far more developers know NodeJS, Spring, .NET or Go than would be
  comfortable maintaining a web server in C, so handing the project over is
  harder, even though AI tools are available.

## Services

The site is deployed to **Azure Container Apps** (0.25 vCPU, 0.5 GiB, one replica
kept running from 7 A.M. to 11 P.M. Finnish time and scaled to zero when idle at
night, HTTPS included). Deployment process is handled by a tooling container that runs the Azure CLI. The fact that deployment tooling is inside a container also reduces the number of user configuration changes needed in the development environment. The server image is built in Azure, and the database lives on an Azure Files share, so it survives restarts and new deployments.

In a nutshell, the following Azure services are used:

- Container Apps (the server)
- Container Registry (builds and stores the server image)
- Storage account + file share (the SQLite database)

## What you need before the first deployment

- **An Azure account with an active subscription (pay-as-you-go, or a plan with credits)**: You sign in with it when `deploy` asks (see below).
- **The Owner or Contributor role on that subscription**: Needed to register resource providers and create resources.
- **Subscription name or id**: Only if the account has more than one subscription. `AZURE_SUBSCRIPTION` in `deploy/config.local.env` is where you can set this.
- **Directory (tenant) of the subscription**: Only if the subscription is not in the account's default directory. `AZURE_TENANT` in `deploy/config.local.env` is where you can set this.
- **Region**: `swedencentral` by default. `AZURE_LOCATION` in `deploy/config.env` is where you can set this.
- **Resource names**: Default values work, but you can set the other settings in `deploy/config.env`.

You do not need to create anything in Azure by hand, and there are no keys or
connection strings to copy: the script creates all resources and the secrets
the app needs.

## Settings in `deploy/config.env`

Edit the file on your machine; the tooling container sees the change
immediately.

`deploy/config.env` is committed, so it must not hold anything that identifies
your Azure account. Put `AZURE_SUBSCRIPTION` and `AZURE_TENANT` in
`deploy/config.local.env` instead (not in version control). `deploy` reads it
after `config.env`, so any setting in it overrides the one in `config.env`. Create
it with just the lines you need:

```
AZURE_SUBSCRIPTION=<name or id>
AZURE_TENANT=<example.onmicrosoft.com or tenant id>
```

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
| `AZURE_FILE_SHARE_QUOTA_GB` | `1` | Size limit of the file share in GiB, for the database and the uploaded images together. The server reads the free space from the share, shows it in the image gallery and refuses uploads that do not fit (HTTP 507). Changing it applies on the next `deploy`. |

The defaults work as they are, so for most people the file needs no changes.
Set the two globally unique names yourself only if the defaults are already
taken, which Azure reports with a "name not available" error. Names and region
identify existing resources: changing them after the first deployment makes the
script create a second set rather than rename the first.

## Deploy

Before the first deployment, make the live site's admin password hash and save
it in `deploy/secrets.env` (never committed; see [Admin password](admin-password.md)):

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

Run the same command again whenever you want to publish changes to `src/`,
`templates/` or `public/`.

## What gets created

All in the resource group from `deploy/config.env` (default `kotoa-kasin`):

| Resource | Purpose |
|---|---|
| Container registry (Basic) | Holds the server images; also builds them (`az acr build`) |
| Storage account + file share `kotoa-data` | The SQLite database, mounted at `/data` |
| Container Apps environment | Hosting platform for the app (no Log Analytics) |
| Container app | The server, at most 1 replica, with the `daytime` scale rule |

## After the first deployment

- **Admin password:** the live site uses the hash in `deploy/secrets.env`
  (see [Admin password](admin-password.md)). To change it, put a new hash there and
  deploy again.
- **The database starts from the seeded recipes** (migration `0002`). Recipes from your local
  database are not copied. It is stored on the file share with `-s share`
  (see [Server options](server-options.md)).
- **Sessions:** a random signing secret (`KASIN_JWT_SECRET`) is created on the
  first deployment and stored as an app secret, so logins survive restarts.
- **Cold starts:** see [Cold starts and the daytime window](#cold-starts-and-the-daytime-window).

## Cold starts and the daytime window

When no replica is running, the first visitor waits for a *cold start*: Azure
finds a server, pulls the image and mounts the database's file share before the
server can answer. On the live app this took about 30 s (the server's own part
is under half a second), and the requests after it were fast. Nothing in the
server or the image changes that.

Keeping a replica always on removes the wait but costs money around the clock.
Since the site's visitors are in Finland, the deployment instead keeps one
replica running during the day and lets the app scale to zero at night:

| Time (Finnish time) | Replicas | First visitor waits? |
|---|---|---|
| 07:00 - 23:00 | 1, kept running by a cron scale rule | No |
| 23:00 - 07:00 | 0 when idle, 1 while visitors come | Yes, about 30 s |

This is a [KEDA cron scale rule](https://keda.sh/docs/latest/scalers/cron/)
named `daytime`. `deploy` sets it up on every run, both when it creates the app
and when it rolls out a new image, so an app made before the rule existed gets
it on its next deploy. The maximum stays at one replica, because SQLite on a file
share allows only one writer.

The window is set in `deploy/config.env`:

| Setting | Default | Notes |
|---|---|---|
| `AZURE_DAYTIME_TIMEZONE` | `Europe/Helsinki` | IANA time zone name. Daylight saving time is followed, so the window stays at 07:00-23:00 local time all year. |
| `AZURE_DAYTIME_START` | `0 7 * * *` | Cron expression (minute hour day month weekday) for when the replica is started. |
| `AZURE_DAYTIME_END` | `0 23 * * *` | Cron expression for when the replica is no longer kept running. |

Things to know:

- **Cost.** 16 hours a day is about 490 hours of running replica a month. The
  free monthly allowance covers about 200 hours at 0.25 vCPU and 0.5 GiB, and
  the rest is billed, at the lower idle rate while there are no requests. Check
  current prices in the Azure portal.
- **After 23:00.** The rule only stops keeping the replica up. A replica that a
  night visitor started stays until Azure scales it down after a quiet period.
- **At 07:00.** The replica takes a short while to start, which happens before
  most visitors arrive.
- **Always on, or never.** To keep a replica running all the time, run
  `az containerapp update --name kotoa-kasin --resource-group kotoa-kasin --min-replicas 1`
  inside the tooling container. The next `deploy` sets the minimum back to 0.
- **Check it.** `az containerapp show --name kotoa-kasin --resource-group kotoa-kasin --query properties.template.scale`
  shows the minimum, the maximum and the `daytime` rule.

## Custom domain and `www`

The site's main address is `kotoakasin.fi`. The server answers any request whose
`Host` starts with `www.` with a `301` to the same path and query on the address
without `www.` (always `https`), so search engines see only one address. For
visitors to get that far, `www.kotoakasin.fi` must also be added to the Container
App as a custom domain, with its own certificate (DNS record `CNAME www` pointing
to the app's address).

## Useful commands

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

## Back up the live database

```sh
docker exec -it kotoa-kasin-deploy backup-db-prod
```

This downloads the database from the Azure Files share and stores it as
`backups/kotoa-<UTC date>-<UTC time>.db` in the project root (not in version
control). It signs in to Azure first if needed. The tooling container mounts
`backups/` read-write, so recreate the container once after pulling this change:
`docker compose -f dev.docker-compose.yml up -d deploy`.

## Cost

The container registry (Basic) is the only fixed cost, about 5 €/month. Storage
for a small database costs cents. The app itself is billed for the hours of the
daytime window beyond the free monthly allowance (see
[Cold starts and the daytime window](#cold-starts-and-the-daytime-window)); to
pay nothing while idle, take the rule out of `deploy/bin/deploy`. Check current prices in the Azure
portal before deploying.

## Troubleshooting

| Problem | Cause and fix |
|---|---|
| An error saying the registry or storage account name is already in use or not available | Set `AZURE_ACR_NAME` / `AZURE_STORAGE_ACCOUNT` in `deploy/config.env` to something unique and run `deploy` again. |
| An error from `az acr build` saying that tasks are not permitted or are paused | Free and trial subscriptions can block building images in Azure. Build the image locally and push it to the registry instead. |
| `AuthorizationFailed` | Your account lacks the Owner or Contributor role on the subscription. |
| `The subscription of '...' doesn't exist in cloud 'AzureCloud'` | The subscription is in another directory than the one `az login` used. Set `AZURE_TENANT` in `deploy/config.env` to that directory's domain (shown in the portal under Directories + subscriptions) and run `deploy` again. |
| The site hangs and `az containerapp logs show` prints `db: database is locked` | The database is on the file share but the server was not started with `-s share`, so SQLite used POSIX locks the share does not support. The production `Dockerfile` already passes `-s share`; redeploy. |
| `deploy` reports no healthy answer at the end | Look at the log with `az containerapp logs show ... --follow`; the app may still be starting. |
| `docker exec` says the container is not running | The tooling container is stopped. Start it with `docker compose -f dev.docker-compose.yml up -d`. |
