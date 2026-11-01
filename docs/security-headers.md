[Documentation index](../README.md)

# Security headers

Every response, pages, files and API alike, carries these headers
(`SECURITY_HEADERS` in `src/main.c`):

| Header | What it does |
|---|---|
| `Content-Security-Policy` | Scripts, styles, images and API calls only from this site, plus Google Fonts; no inline scripts or styles, no plugins, no `<base>`; forms only to this site |
| `X-Content-Type-Options: nosniff` | Browsers take files as their declared type, so an upload cannot be run as a script or page |
| `X-Frame-Options: DENY` and `frame-ancestors 'none'` | No other site can show the pages in a frame to trick clicks |
| `Referrer-Policy: strict-origin-when-cross-origin` | Links to other sites reveal only the site's address, not the page |
| `Strict-Transport-Security: max-age=31536000` | Browsers use only HTTPS for the site for a year (ignored over plain HTTP, such as the dev server) |

The policy matters because an injected script could call the API as the
logged-in admin; the policy keeps such a script from running.

**When changing the site:** keep scripts in `js/` and styles in `css/`; a
`<script>` or `style="..."` written into the HTML is blocked (setting styles from
JavaScript, `element.style.x = ...`, is fine). Anything loaded from another site,
such as an embedded map or video, analytics, or an image by its full address,
needs its address added to the policy in `src/main.c`, or the browser will
refuse it. Blocked items show as "Content Security Policy" errors in the
browser's developer console.

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
