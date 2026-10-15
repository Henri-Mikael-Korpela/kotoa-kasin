[Documentation index](../README.md)

# API

Reading is public; changes need a login token (`Authorization: Bearer <token>`).

| Endpoint | Purpose |
|---|---|
| `POST /api/login` | `{username, password}` returns `{token, expires_at}`. A wrong password: 401 with `retry_after` (seconds before the next attempt). Refused by the [login protection](login-protection.md): 429 with `Retry-After` and `{error, reason: "delayed", "banned" or "busy", retry_after}` |
| `GET /api/session` | Checks the token (login required) |
| `GET`, `POST /api/recipes` | List (with `excerpt` instead of `description`), create (`slug`, `name`, `image`, `description`, `category_id`; 409 if another recipe has the slug) |
| `GET`, `PUT`, `DELETE /api/recipes/{id}` | Read, update (409 if another recipe has the slug), delete |
| `GET /api/recipes/slug/{slug}` | Read a recipe by its slug |
| `GET`, `POST /api/categories` | List (alphabetical), create (`name`) |
| `GET`, `PUT`, `DELETE /api/categories/{id}` | Read, rename, delete (409 while recipes use it) |
| `GET /api/images` | List uploaded images with how many recipes use each (`used`) and which ones (`recipes`: `{id, name}`, alphabetically) (login required) |
| `POST /api/images` | Upload: the raw JPEG, PNG, GIF or WebP file as the body, original name in `X-File-Name` (login required) |
| `DELETE /api/images/{id}` | Delete (login required; 409 while a recipe shows it) |
| `GET /api/login-attempts` | `{now, blocked, report: {clients, attempts}}`: addresses waiting or banned now, the 500 addresses seen last and the 300 latest attempts (login required) |
| `DELETE /api/login-blocks/{client id}` | Lifts an address's wait or ban (login required) |
