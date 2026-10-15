[Documentation index](../README.md)

# Server options

```
kasin-httpd [-p port] [-t threads] [-r docroot] [-d database] [-m migrations]
            [-u uploads] [-s local|share] [-g countries]
```

`-g` names the country file made by `kasin-geoip` (see
[Login protection](login-protection.md)).

Environment: `KASIN_ADMIN_PASSWORD_HASH` (see [Admin password](admin-password.md)),
`KASIN_JWT_SECRET` (signs sessions; random at each start when unset, which
logs everyone out on restart) and `KASIN_TRUST_PROXY=1` (take the client
address from `X-Forwarded-For`; only behind a proxy that sets it).

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
