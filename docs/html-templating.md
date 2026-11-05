[Documentation index](../README.md)

# HTML templates

The pages of the site share a head, a header and a footer. Instead of repeating
them in every page, the pages are written as templates in `templates/` and a
small C program built with the server, `kasin-htmlgen`, generates the HTML
files in `public/` from them. The templates are the source; the generated
pages are not in version control (see `.gitignore`), so edit the templates, not
the files in `public/`.

## Layout of `templates/`

| Path | Contents |
|---|---|
| `templates/pages/` | One template per page. A page's path here is its path in `public/`: `pages/meista.html` becomes `public/meista.html` and `pages/hallinta/kuvat.html` becomes `public/hallinta/kuvat.html`. Every `.html` file here is generated. |
| `templates/fragments/` | The parts pages include: they are not pages themselves. |

The fragments are:

| Fragment | Contents |
|---|---|
| `head.html` | From `<!DOCTYPE html>` to `</head>`, for the public and the admin pages. |
| `header.html` | The public site's header and menu. |
| `footer.html` | The public site's footer. |
| `admin-header.html` | The admin view's header and menu (not the login page's, which differs). |
| `contact-info.html` | Only defaults: the email address and the Instagram and Facebook addresses, so that they are written once. |

A page is its own content plus the parts it includes. This is `meista.html`
without the content of `<main>`:

```html
{{set title "Meistä &mdash; Kotoa käsin"}}
{{set description "Kotoa käsin &ndash; kolmen lapsen kotiäidin ruoanlaittoa."}}
{{set page "meista"}}
{{set solid "1"}}
{{include "head.html"}}

<body>

{{include "header.html"}}

  <main>
    ...
  </main>

{{include "footer.html"}}

  <script src="/js/{{bundle}}.js"></script>
</body>

</html>
```

## Syntax

Everything between `{{` and `}}` is a tag. All other text is copied as it is.
Spaces inside the braces do not matter: `{{ title }}` is `{{title}}`.

| Tag | Meaning |
|---|---|
| `{{name}}` | Inserts the value of the variable. It is an error if the variable was never set. |
| `{{set name "value"}}` | Sets a variable. A later `set` replaces the value. |
| `{{default name "value"}}` | Sets a variable only if it has no value (never set, or empty). |
| `{{include "file.html"}}` | Inserts the template `templates/fragments/file.html`, processed in turn. A subdirectory can be used: `"admin/x.html"`. |
| `{{if name}}` | The text up to `{{else}}` or `{{end}}` is used only if the variable has a non-empty value. A variable that was never set counts as empty. |
| `{{if name == "value"}}` | The same when the value equals the string; `!=` when it does not. |
| `{{else}}` | The other branch of the `{{if}}`. At most one per `{{if}}`. |
| `{{end}}` | Ends the `{{if}}`. |

Rules to know:

- **Variables are global to one page.** What a page or a fragment sets, all
  the others see, whether they come before or after. A fragment can therefore
  read what the page set (`title`, `page`) and the page can read what a fragment
  set (`{{include "contact-info.html"}}` makes `{{email}}` available). Each page
  is generated on its own: variables do not carry over from one page to another.
- **Names** use `a-z`, `0-9` and `_`. `set`, `default`, `include`, `if`,
  `else` and `end` are keywords and cannot be names.
- **Values are quoted and inserted as they are**, without escaping. They are
  HTML written by the site's author (`&mdash;` stays `&mdash;`), so a value
  cannot contain a double quote: write `&quot;`. There is no way to write a
  literal `{{` in a page; `{{` always starts a tag.
- **A statement alone on its line takes the line with it.** `set`, `default`,
  `include`, `if`, `else` and `end` tags that have nothing but spaces around them
  on their line leave no blank line behind, so a template can be laid out for
  reading:

  ```html
  {{if robots}}
    <meta name="robots" content="{{robots}}">
  {{end}}
  ```

  A tag with text around it, like `<a{{if solid}} class="on"{{end}}>`, is
  replaced in place and the line stays. `{{name}}` is never removed with its
  line.
- **An included template ends with its own line break**, like any text file; the
  tag that includes it, alone on its line, then adds nothing else.
- **A skipped branch is not run.** `set`, `include` and `{{name}}` inside the
  branch that is not used do nothing, so they cannot fail.

## Variables the templates use

| Variable | Set by | Used by | Meaning |
|---|---|---|---|
| `title` | every page | `head.html` | The page's `<title>`. Required. |
| `description` | public pages | `head.html` | The meta description. Left out if not set. |
| `robots` | admin pages | `head.html` | The value of `<meta name="robots">` (`noindex`). Left out if not set. |
| `bundle` | admin pages | `head.html`, pages | `admin` for the admin view; defaults to `site`. The style sheet and script are `/css/<bundle>.css` and `/js/<bundle>.js`, see [JavaScript and CSS bundles](bundles.md). |
| `page` | every page | `header.html`, `admin-header.html` | Which menu item has `aria-current="page"`: `meista` or `yhteystiedot` on the public site; `recipes`, `images` or `logins` in the admin view. |
| `solid` | public pages but the front page | `header.html` | When set, the header is solid from the start; otherwise it is transparent over the front page's picture until scrolled. |
| `home_url` | the front page (`#top`) | `header.html`, `footer.html` | Where the logo and the home links go; defaults to `/`. |
| `recipes_url` | the front page (`#reseptit`) | `footer.html` | Where the recipe collection link in the footer goes; defaults to `/#reseptit`. |
| `email`, `instagram_url`, `facebook_url` | `contact-info.html` | `footer.html`, `yhteystiedot.html` | The contact details; change them in `contact-info.html` only. |

## Adding or changing a page

1. Add the template `templates/pages/<path>.html` (or change an existing
   one). Start it with the `set` tags for `title` and the other variables it
   needs, then `{{include "head.html"}}`.
2. Open `/<path>` in the browser. In the dev container the page is generated as
   soon as you save; see below.

A part that two or more pages share belongs in `templates/fragments/`. When a
page needs a variation of it, give the fragment a variable and let the page set
it, as `page` and `solid` do, rather than copying the fragment.

## Building

`kasin-htmlgen` is built with the server:

```sh
kasin-htmlgen -o public templates       # generate every page once
kasin-htmlgen -w -o public templates    # keep running, generate again on change
```

- **Dev container:** a watcher (`kasin-htmlgen -w`) generates the pages when the
  container starts and again whenever a file in `templates/` is saved, added or
  removed. Reload the page to see the change; no restart is needed. A mistake is
  reported in the container log (`docker compose -f dev.docker-compose.yml logs -f httpd`),
  and the pages generated earlier stay in place until the next successful build. The
  watcher polls file times, because file-change events do not cross the Windows
  bind mount.
- **Production image:** the `Dockerfile` generates the pages in its build stage,
  after the unit tests have run, and copies them to the served directory. A
  template with a mistake therefore fails the build and the deployment instead
  of reaching the site. Nothing from `templates/` is served itself.

A page that fails does not stop the others from being generated, but the tool
exits with an error (status 65) after all pages have been tried.

## Errors

Errors are printed as `<template>:<line>: <message>`, naming the file and the line
where the mistake is, which for a mistake inside a fragment is the fragment:

```
templates/pages/meista.html:12: variable has no value: titel
templates/fragments/footer.html:3: cannot include contact-nfo.html
```

Typical mistakes: a variable that is never set (a typo in a name), an
`{{if}}` without `{{end}}`, a name that is not a template, a value that
contains a double quote, and templates including each other.

## Limits

Everything the tool does is bounded, and the limits are constants in
`src/template.h` and `src/htmlgen.c`:

| Limit | Value |
|---|---|
| Includes nested inside each other, counting the page | 8 |
| Variables in one page | 32 |
| `{{if}}`s open at once | 16 |
| Words and quoted strings in one tag | 4 |
| Size of a template or generated page | 1 MiB |
| Files under `templates/` | 256 |

Includes cannot loop: a template that includes itself, directly or through
others, stops at the depth limit with an error. An include name cannot start with
`/` or contain `..`, `.`, empty segments or backslashes, so it cannot reach a file
outside `templates/fragments/`.

The engine is `src/template.c` (tested by `tests/template_test.c`); `src/htmlgen.c`
reads and writes the files and implements watch mode.
