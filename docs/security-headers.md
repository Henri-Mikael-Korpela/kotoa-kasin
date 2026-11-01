[Documentation index](../README.md)

# Security headers

Every response, pages, files and API alike, carries these headers
(`SECURITY_HEADERS` and the policies in `src/main.c`):

| Header | What it does |
|---|---|
| `Content-Security-Policy` | Depends on the response; see [the two policies](#the-two-policies) |
| `X-Content-Type-Options: nosniff` | Browsers take files as their declared type, so an upload cannot be run as a script or page |
| `X-Frame-Options: DENY` and `frame-ancestors 'none'` | No other site can show the pages in a frame to trick clicks |
| `Referrer-Policy: strict-origin-when-cross-origin` | Links to other sites reveal only the site's address, not the page |
| `Strict-Transport-Security: max-age=31536000` | Browsers use only HTTPS for the site for a year (ignored over plain HTTP, such as the dev server) |

## The two policies

**Strict: admin pages, the API and files.** Scripts, styles, images and API calls
only from this site, plus Google Fonts; no inline scripts or styles, no plugins, no
`<base>`; forms only to this site. This matters because an injected script could
call the API as the logged-in admin; the policy keeps such a script from running.

**Public pages** (HTML outside `hallinta`): the [nonce-based policy Google AdSense
supports](https://support.google.com/adsense/answer/16283098). AdSense's addresses
change over time, so they cannot be listed.

```
script-src 'nonce-<new for each response>' 'strict-dynamic' 'unsafe-inline' 'unsafe-eval' https: http:;
object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'
```

The server reads each public page, adds the nonce to its `<script` tags and sends
it with `Cache-Control: no-cache`. A script without the nonce does not run, unless a
script that has it loads that script, which is how AdSense loads its own. Inline
event handlers do not run either. Browsers that understand nonces ignore
`'unsafe-inline'` and the schemes, which are for older browsers.

This is looser than the strict policy: there is no limit on styles, images, frames
or connections, and scripts that run may use `eval`. Injected styles and images
would load, and a script allowed to run could send data anywhere. Everything on
public pages must therefore stay safe with that in mind: recipes are shown through
`textContent`-style code, never as raw HTML with scripts.

**When changing the site:**

- Admin pages: keep scripts in `js/` and styles in `css/`; a `<script>` or
  `style="..."` written into the HTML is blocked (setting styles from JavaScript,
  `element.style.x = ...`, is fine). Anything loaded from another site, such as an
  embedded map or video, needs its address added to `STRICT_CSP` in `src/main.c`.
- Public pages: write `<script src="...">` as usual, in lowercase, and the server
  adds the nonce. A public page may be at most 24 KiB (`MAX_PUBLIC_PAGE_SIZE`); a
  larger one fails with status 500.

Blocked items show as "Content Security Policy" errors in the browser's developer
console.

## Session cookie

Logging in sets one cookie, `kasin_session`, holding a signed token that is valid
for 8 hours:

| Attribute | What it does |
|---|---|
| `HttpOnly` | Scripts cannot read it, so an injected or third-party script cannot copy the session elsewhere |
| `Secure` | Sent only over HTTPS. The plain HTTP dev server turns this off with `KASIN_INSECURE_COOKIE=1`; never set that in production |
| `SameSite=Strict` | Browsers do not send it with requests another site starts, which stops cross-site request forgery |
| `Path=/api` | Sent only to the API, not with pages and files |

The server also refuses a session on a request whose `Sec-Fetch-Site` says
`cross-site` or `same-site`.

Limits: a script running on this site's own pages can still make API calls while
the admin is logged in, even though it cannot read the token. Logging out clears
the cookie but does not revoke the token, since sessions are not stored; a token
copied earlier stays valid until it expires. Changing `KASIN_JWT_SECRET` ends every
session.
