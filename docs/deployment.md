[Documentation index](../README.md)

# Deploy to Azure

## Rationale

This website was intended to run on a small Linux server with minimal resources. I wanted to see how this would work out and to also learn more about cloud services via Azure.

The usual way to build a site like this is with a framework: Spring Boot (Java
or Kotlin), ASP.NET Core (.NET) or Go's standard library. This project uses a
small HTTP server written in C instead, with SQLite as its only library, on
three Azure services. This section makes the case for that choice with
measurements, and is just as clear about what it costs.

### What the site needs

A handful of pages, a JSON API, one admin and image uploads, visited now and
then with long quiet periods in between. On Azure Container Apps such a site
scales to zero: nothing is billed while no replica runs, and the first visitor
after a quiet period waits for a *cold start*. What matters most is therefore
how quickly and lightly the service starts, how little it needs while it runs,
and how little there is to keep patched. Raw speed matters least.

### Measured side by side

The same minimal service, one endpoint that answers `ok`, was built in each
stack and packaged the way its own documentation suggests. This project's
server ran as its production image, with its database, migrations and country
data. Every service ran with this deployment's limits (0.25 vCPU, 512 MB) on a
developer PC with Docker Desktop, in October 2026.

| | This server (C) | Go 1.23 | ASP.NET Core 8 | Spring Boot 3.3 (Java 21) |
|---|---|---|---|---|
| Container image (uncompressed) | 26.2 MB, of which 8.4 MB is country data | 7.3 MB | 167 MB | 324 MB |
| What the image carries | 112 KB program, musl libc, SQLite (1.4 MB) | One static program | .NET runtime (93 MB) | Java runtime (157 MB), 20 MB application |
| Third-party libraries | 1 (SQLite) | 0 | 0 beyond the runtime | 34 |
| Start until the first answer | 1.18 s | 0.77 s | 3.44 s | 34.6 s |
| Same, with 1 vCPU and 2 GiB | | | 1.39 s | 10.8 s |
| Memory when idle | 82 MiB, reserved at startup by design | 2.2 MiB | 29 MiB | 101 MiB |
| Memory under load | 81 MiB | 8.7 MiB | 37 MiB | 119 MiB |
| Requests per second | 13,336 | 4,533 | 1,072 | 275 |
| Median response time under load | 0.45 ms | 1.6 ms | 32 ms | 101 ms |

- **Startup** is the median of three cold starts, from `docker run` until the
  first successful answer, so it includes about 0.75 s of Docker's own work
  for every stack: Go's figure is almost entirely that. Measured inside the
  container, this server is ready in 0.14–0.23 s, including opening SQLite.
  Kotlin with Spring runs on the same Java runtime and libraries, so it
  behaves like the Java column.
- **Load** was generated with `wrk` (2 threads, 32 connections, 20 seconds)
  after a 15-second warm-up, so that the Java and .NET just-in-time compilers
  had reached full speed. At this CPU limit, the slowest responses (99th
  percentile) take 85–100 ms for every stack except Spring Boot (600 ms),
  because the limit hands out processor time in 100 ms slices.

### What a cold start costs on Azure

On the live app, on 14 October 2026, the first request after a quiet period
took **29.8 s**; the requests after it took 0.16 s. Azure's own event log shows
where the time went:

| Step | Time |
|---|---|
| Azure finds a server for the app and starts pulling the image | about 8 s |
| Pulling the 26 MB image | 3.0 s |
| Creating the container and mounting the database's file share | 12 s |
| This server starting, opening SQLite on the share and listening | 0.4 s |

Most of the wait is Azure's and no choice of language changes that. What the
server can do is add almost nothing to it, and this one adds under half a
second. Going by the measurements above, a Spring Boot service at this size
would add about 35 seconds more (about 11 at a full vCPU), and Azure would pull
an image twelve times larger first. That is an estimate from these
measurements, not something measured on Azure.

### What it costs to run

Container Apps' consumption plan gives each subscription 180,000 vCPU-seconds,
360,000 GiB-seconds and 2 million requests free every month
([Azure pricing](https://azure.microsoft.com/en-us/pricing/details/container-apps/)).
At 0.25 vCPU and 0.5 GiB that covers 200 hours of running replica a month. At
1 vCPU and 2 GiB, a common size for a JVM service, it covers 50 hours. A
small, quickly started service that sleeps most of the time stays within the
free allowance; the container registry, about 5 € a month, remains the only
fixed cost. With over 13,000 requests per second in the test above, at this
deployment's CPU limit (on a PC, not Azure's own hardware), the site will not
need a larger size for traffic.

### Fewer moving parts

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
  separate server to run, pay for, patch or connect to.

### What it costs in other ways

- **Memory safety.** C does not prevent buffer overflows or use of freed
  memory. About 70% of the security bugs Microsoft assigns a CVE each year
  ([MSRC, 2019](https://www.microsoft.com/en-us/msrc/blog/2019/07/a-proactive-approach-to-more-secure-code))
  and about 70% of Chromium's high-severity security bugs
  ([Chromium](https://www.chromium.org/Home/chromium-security/memory-safety/))
  are memory-safety problems. This server limits the risk (fixed buffers
  reserved at startup, strings that carry their length, unit tests), but Go,
  C#, Java and Kotlin rule out that whole class of bug.
- **Everything is written by hand.** About 8,500 lines of C plus 700 lines of
  tests cover what a framework provides: HTTP parsing and routing, sessions
  (JWT), SHA-256 and password hashing, the JavaScript and CSS minifiers, the
  country lookup. Hand-written code has hand-written bugs: a security review
  found that addresses starting with `//` could read any file on the server,
  something a framework's file serving would have prevented. It is fixed, and
  the kernel now keeps file access inside the served directories.
- **Go is a close rival.** In these tests Go started faster, idled on a
  fraction of the memory (this server reserves all of its memory up front, by
  design), shipped a smaller image and is memory-safe. A third of this
  server's throughput is still far more than the site needs. A Go version
  would have been a reasonable choice too.
- **Native compilation narrows the gap.** Spring Boot with GraalVM native
  images and .NET with Native AOT start in a fraction of a second and use less
  memory, at the cost of longer builds and some restrictions. They were not
  measured here.
- **Fixed limits.** Memory (about 80 MiB with two workers), connections and
  request sizes are set when the server is built; changing them means
  rebuilding.
- **Features and scaling.** One replica only (SQLite on a file share allows
  one writer), HTTP/1.1 only (Azure's ingress provides HTTPS and HTTP/2),
  Linux only, and no ready-made metrics, tracing or single sign-on. In Spring
  or .NET each of these is a library away; here each would be work of its own.
- **People.** Far more developers know Spring, .NET or Go than would be
  comfortable maintaining a web server in C, so handing the project over is
  harder.

### In short

For a small site with one admin, long quiet periods and a pay-per-use
platform, the minimal setup fits well. It adds almost nothing to Azure's cold
start, runs within the smallest size with plenty of headroom, and depends on
one thoroughly tested library. The price is owning code that a framework would
otherwise provide, including its security. If the site grows a team, many
features or more than one replica, Go or ASP.NET Core would be the natural
next step, and Spring Boot where the people around it already work on the JVM.

## Services

The site is deployed to **Azure Container Apps** (0.25 vCPU, 0.5 GiB, scaled to
zero when idle, HTTPS included). Deployment process is handled by a tooling container that runs the Azure CLI. The fact that deployment tooling is inside a container also reduces the number of user configuration changes needed in the development environment. The server image is built in Azure, and the database lives on an Azure Files share, so it survives restarts and new deployments.

In a nutshell, the following Azure services are used:

- Container Apps (the server)
- Container Registry (builds and stores the server image)
- Storage account + file share (the SQLite database)

## What you need before the first deployment

- **An Azure account with an active subscription (pay-as-you-go, or a plan with credits)**: You sign in with it when `deploy` asks (see below).
- **The Owner or Contributor role on that subscription**: Needed to register resource providers and create resources.
- **Subscription name or id**: Only if the account has more than one subscription. `AZURE_SUBSCRIPTION` in `deploy/config.env` is where you can set this.
- **Directory (tenant) of the subscription**: Only if the subscription is not in the account's default directory. `AZURE_TENANT` in `deploy/config.env` is where you can set this.
- **Region**: `swedencentral` by default. `AZURE_LOCATION` in `deploy/config.env` is where you can set this.
- **Resource names**: Default values work, but you can set the other settings in `deploy/config.env`.

You do not need to create anything in Azure by hand, and there are no keys or
connection strings to copy: the script creates all resources and the secrets
the app needs.

## Settings in `deploy/config.env`

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

Run the same command again whenever you want to publish changes to `src/` or
`public/`.

## What gets created

All in the resource group from `deploy/config.env` (default `kotoa-kasin`):

| Resource | Purpose |
|---|---|
| Container registry (Basic) | Holds the server images; also builds them (`az acr build`) |
| Storage account + file share `kotoa-data` | The SQLite database, mounted at `/data` |
| Container Apps environment | Hosting platform for the app (no Log Analytics) |
| Container app | The server, at most 1 replica |

## After the first deployment

- **Admin password:** the live site uses the hash in `deploy/secrets.env`
  (see [Admin password](admin-password.md)). To change it, put a new hash there and
  deploy again.
- **The database starts from the seeded recipes** (migration `0002`). Recipes from your local
  database are not copied. It is stored on the file share with `-s share`
  (see [Server options](server-options.md)).
- **Sessions:** a random signing secret (`KASIN_JWT_SECRET`) is created on the
  first deployment and stored as an app secret, so logins survive restarts.
- **Cold starts:** when idle, the app scales to zero and the first request
  after a quiet period takes a few seconds. To keep it always on:
  `az containerapp update --name kotoa-kasin --resource-group kotoa-kasin --min-replicas 1`
  (run it inside the tooling container; this costs an always-on replica).

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

## Cost

The container registry (Basic) is the only fixed cost, about 5 €/month. Storage
for a small database costs cents, and a scaled-to-zero app usually stays within
the Container Apps free monthly allowance. Check current prices in the Azure
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
