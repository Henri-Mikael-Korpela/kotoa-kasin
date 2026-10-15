[Documentation index](../README.md)

# Login protection

Wrong passwords are slowed down and then refused, per client address, the same way locally and on Azure:

| Failures in a row | Before the next attempt |
|---|---|
| 1-3 | No wait |
| 4-9 | 2, 4, 8, 16, 32, then 60 seconds |
| 10 | Banned for 15 minutes |

- A successful login forgets the address's failures; so do 15 minutes without
  a new one.
- A refused attempt gets `429 Too Many Requests` with `Retry-After` at once,
  without checking the password; the login page shows how long to wait and
  counts down.
- At most 6 passwords are checked per minute in all, from every address
  together, because each check is slow on purpose (see
  [Admin password](admin-password.md)). Banned addresses do not use up checks.
  An attacker with very many addresses can therefore delay the admin's login
  by up to a minute, but cannot overload the server.
- The limits are kept in memory, shared by all the server's threads, and
  rebuilt from the database at startup, so a restart (or Azure scaling the app
  to zero) does not lift a ban.

**Client address.** Locally it is the address the connection comes from (in
Docker, the Docker network's gateway, such as `172.18.0.1`, for every browser).
On Azure the connection comes from the ingress proxy, which adds the client's
address to `X-Forwarded-For`; the deployment sets `KASIN_TRUST_PROXY=1`, and
the server then uses that header's last entry, the one the proxy added.
Without the setting the header is ignored, since any client can send one.

**Logs.** Every checked password is logged, and refusals at most once a minute
per address, so attacks show in `az containerapp logs show` (and locally in
`docker compose -f dev.docker-compose.yml logs -f httpd`):

```
login: FAILED for "admin" from 193.166.3.2 (FI); 4 recent failures, next attempt allowed in 2 s
login: refused attempt from 193.166.3.2 (FI): must wait after 4 failures, 2 s more (1 refused since the last such line)
login: success for "hanna" from 87.95.0.1 (FI)
```

**Kirjautumiset** (`/hallinta/kirjautumiset`) shows the addresses that must
wait or are banned (with a button to lift the block), the latest attempts with
time, address, country, user name tried and browser, and every address that
has tried to log in. Each address is stored once as a client (`login_clients`:
address, country, first and last seen) and each checked password as an attempt
of that client (`login_attempts`). Records older than 90 days are removed.

Locally every browser, phones included, comes in through the same Docker
gateway address, so a lockout applies to all of them. Wait 15 minutes, or lift
all blocks and restart:

```sh
docker compose -f dev.docker-compose.yml exec httpd sqlite3 /data/kotoa.db "UPDATE login_clients SET cleared_at = strftime('%s', 'now')"
docker compose -f dev.docker-compose.yml restart httpd
```

**Countries** come from DB-IP's free "IP to Country Lite" database (CC BY 4.0;
credited on the Kirjautumiset page). It is looked up on the server, so no
address is sent anywhere. `scripts/fetch-geoip` downloads the monthly CSV and
`kasin-geoip` turns it into a compact file (8 MB) that the server maps into
memory (`-g`). The production image fetches the latest data at every build;
the dev container fetches it once and again when it is over a month old (kept
in the `build-cache` volume). Without the data, attempts are recorded without
countries. Private and reserved addresses have no country.
