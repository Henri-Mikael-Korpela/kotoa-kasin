[Documentation index](../README.md)

# JavaScript and CSS bundles

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
