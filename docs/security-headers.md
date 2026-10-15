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

The policy matters because the admin session token is kept in the browser's
`localStorage`: if a script were ever injected into a page, the policy keeps it
from running.

**When changing the site:** keep scripts in `js/` and styles in `css/`; a
`<script>` or `style="..."` written into the HTML is blocked (setting styles from
JavaScript, `element.style.x = ...`, is fine). Anything loaded from another site,
such as an embedded map or video, analytics, or an image by its full address,
needs its address added to the policy in `src/main.c`, or the browser will
refuse it. Blocked items show as "Content Security Policy" errors in the
browser's developer console.
