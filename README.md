# Kotoa käsin

Website for homemade cosmetics and cooking recipes, served by a small HTTP
server written in C. Recipes and categories are stored in SQLite and managed
from an admin view at `/hallinta`.

- **Server:** C, Linux only. epoll, one worker per thread, all memory reserved
  at startup, no `malloc` in the request path. Uses SQLite as its only library.
- **Site:** static pages generated from the templates in `templates/` into `public/`; the front
  page loads recipes from the API.
- **Admin:** log in at `/hallinta/kirjaudu`, then add, edit and delete recipes
  and categories, manage images and see login attempts.

## Quick start

Requires Docker (Docker Desktop with the WSL2 backend on Windows).

```sh
docker compose -f dev.docker-compose.yml up --build
```

Open <http://localhost:8080>. The admin view at `/hallinta` needs an admin
password first; see [Running locally](docs/local-development.md).

## Documentation

### Getting started

| Document | Contents |
|---|---|
| [Running locally](docs/local-development.md) | Starting the dev container, setting the admin password, working on the code |
| [Deploying to Azure](docs/deployment.md) | What you need, settings, deploying, cost and troubleshooting |

### Security

| Document | Contents |
|---|---|
| [Admin password](docs/admin-password.md) | The salted, slow password hash and where it is stored |
| [Login protection](docs/login-protection.md) | Waits and bans, client addresses, logs, the Kirjautumiset view and countries |
| [Security headers](docs/security-headers.md) | Content Security Policy and the other headers sent with every response |
| [Fonts](docs/fonts.md) | The self-hosted fonts and their licences |

### The site and its content

| Document | Contents |
|---|---|
| [Recipe editor and images](docs/recipe-editor.md) | Blocks, the storage format, slugs, images and large requests |
| [Database migrations](docs/database-migrations.md) | How the schema is defined and changed |
| [HTML templates](docs/html-templating.md) | Generating the pages from templates: variables, includes, conditions and the layout of `templates/` |
| [JavaScript and CSS bundles](docs/bundles.md) | Building, minifying and separating the public and admin code |

### Reference

| Document | Contents |
|---|---|
| [Continuous integration](docs/continuous-integration.md) | The GitHub Actions checks: build, tests, static analysis and the production image |
| [Project layout](docs/project-layout.md) | What each directory and main file holds |
| [Server options](docs/server-options.md) | Command line, environment variables and build-time limits |
| [API](docs/api.md) | The JSON endpoints |

### Code style

| Document | Contents |
|---|---|
| [Programming mindset](docs/programming-mindset.md) | How to approach a change: simplicity, dependencies, performance and memory |
| [C programming style guide](docs/c-programming-style-guide.md) | Layout, naming, strings, assertions, error handling and the other C rules |

The short, checkable form of the C and JavaScript rules is in [CLAUDE.md](CLAUDE.md).
