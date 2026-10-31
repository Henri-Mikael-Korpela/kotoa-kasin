[Documentation index](../README.md)

# Run locally

Local development requires Docker (Docker Desktop with the WSL2 backend on Windows). You can build and run the dev container with:

```sh
docker compose -f dev.docker-compose.yml up --build
```

Then open <http://localhost:8080>.

The admin view is at
<http://localhost:8080/hallinta>, with the admin user name (`ADMIN_USERNAME` in `src/main.c`). You need to set the password yourself. Until you do, the site runs but nobody can log in. Here are the steps to set the password:

1. Make the password hash into a file named `.env` next to
   `dev.docker-compose.yml` in project root. You can either create a password automatically or choose one yourself. To generate a password automatically, run the following Powershell command:

   ```powershell
   docker compose -f dev.docker-compose.yml exec -T httpd /build/kasin-passwd -g | Out-File -Encoding ascii .env
   ```

   `kasin-passwd -g` makes a strong random password, shows it once and writes only its hash to the file.

   To choose the password yourself, run `docker compose -f dev.docker-compose.yml
   exec httpd /build/kasin-passwd` instead: it asks twice (at least 12
   characters; a few unrelated words work well) and prints a
   `KASIN_ADMIN_PASSWORD_HASH=...` line to paste into `.env`.
2. Restart: `docker compose -f dev.docker-compose.yml up -d`.

Note: `.env` is in `.gitignore`; never commit it. See [Admin password](admin-password.md).

Server code interacts with the container you built. After changing C code, run `docker compose -f dev.docker-compose.yml restart httpd` to rebuild the server. Files in `public/` are served directly; changes show up on reload.

The database lives in the `db-data` Docker volume (`/data/kotoa.db` in the container) and survives restarts. `docker compose down -v` deletes it.

To reach the dev server from a mobile device on the same network, allow TCP 8080 in
Windows Firewall and open `http://<PC address>:8080`.
