[Documentation index](../README.md)

# Fonts

The site's fonts are served from `public/fonts/`, not from Google, so a visit
sends nothing to Google Fonts.

| Font | Used for | Licence |
|---|---|---|
| Fraunces | Headings | SIL Open Font License 1.1, `OFL-Fraunces.txt` |
| Work Sans | Body text | SIL Open Font License 1.1, `OFL-Work-Sans.txt` |

The files are the WOFF2 files Google Fonts serves, downloaded once. Both fonts
are variable fonts, so one file per character set (latin, latin-ext,
vietnamese) covers every weight; Fraunces also has an optical size axis. The
`@font-face` rules are at the top of `css/shared/style.css`; the browser loads
only the sets that a page's text needs, so Finnish pages load just the latin
files, which `head.html` preloads.

## Licence

Both fonts are under the SIL Open Font License 1.1, which allows using,
copying and serving them, also commercially and on a website. Conditions:

- **Keep the copyright notice and licence with the fonts.** The two `OFL-*.txt`
  files in `public/fonts/` do this and are served with the fonts. The licence
  does not require showing the fonts' copyright on the pages themselves.
- The fonts may not be sold on their own.
- A modified version must be under the same licence and may not use a name the
  licence reserves. Neither licence file here declares a reserved name, but the
  simplest rule is not to edit the font files.

Copyright: Fraunces, 2018 The Fraunces Project Authors
(<https://github.com/undercasetype/Fraunces>); Work Sans, 2019 The Work Sans
Project Authors (<https://github.com/weiweihuanghuang/Work-Sans>).

## Changing a font

Download the font's files, keep its `OFL.txt` next to them, add the
`@font-face` rules and update this page. Admin pages use the same fonts: their
Content Security Policy allows fonts only from this site.
