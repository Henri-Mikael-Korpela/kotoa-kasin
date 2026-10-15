[Documentation index](../README.md)

# Recipe editor and images

Recipe descriptions are written in a block editor on the recipe's edit page in
the admin view, in the spirit of WordPress Gutenberg:

| Block | Name in storage | Options |
|---|---|---|
| Paragraph | `paragraph` | none; line breaks are kept |
| Title | `title` | `level`: 2, 3 or 4 (H2-H4; H1 is the recipe name) |
| Image | `image` | `src` (image address), `alt` (description for screen readers), `caption` (optional); how it is shown: `crop` (`x`, `y`, `w`, `h` as fractions of the picture), `width` and `height` (the picture's pixel size, needed with a crop), `scale` (width on the page, 25-100 % of the text column) |
| Layout | `layout` | two columns side by side; each holds a paragraph, title or image |

Add blocks from the bar under the list, drag them by the ⠿ handle (mouse or
touch) or move them with the arrow buttons, and remove them with ✕. In a layout
block, ⇄ swaps the two columns.

**Muokkaa kuvan esitystapaa** on an image block opens the image display editor: drag the
frame's corners to crop, drag inside the frame (or use the arrow keys) to
choose which part of the picture is centred, and set the image's size on the
page with the slider. The uploaded file is never changed; the settings are
stored as the block's options, and **Palauta alkuperäinen** removes them.

A live preview shows the recipe as its public page will look, updated with
every change to the name, category, cover image or blocks. On screens wider
than 1100 pixels the form is on the left and the preview on the right; on
narrower screens the **Muokkaa** and **Esikatselu** tabs switch between them.

**Storage format.** The description stays plain text in the `recipes` table.
Each block is marked with HTML comments, so blocks stay distinct from ordinary
text:

```
<!-- block:title {"level":2} -->
Ainekset
<!-- /block:title -->

<!-- block:paragraph -->
Sulata mehiläisvaha vesihauteessa.
<!-- /block:paragraph -->

<!-- block:image {"src":"/uploads/3.jpg","alt":"Valmis voide"} /-->

<!-- block:layout -->
<!-- block:image {"src":"/uploads/4.jpg","alt":""} /-->
<!-- block:paragraph -->
Teksti kuvan oikealla puolella.
<!-- /block:paragraph -->
<!-- /block:layout -->
```

- Options are a JSON object after the block name, with `<` and `>` written as
  `\u003c` and `\u003e` so they can never end the comment.
- In block text, `\` is stored as `\\` and `<!--` as `<\!--`, so text can never
  start a marker.
- An unused layout column is `<!-- block:empty /-->`.
- Text outside any marker (descriptions written before the editor) reads as
  paragraphs, split at blank lines.
- Parsing, writing and rendering live in `js/shared/blocks.js`, shared by the
  editor and the public recipe page. Unknown blocks are skipped, and text is
  always inserted as text, never as HTML.

**Slugs.** Each recipe has a slug, the last part of its page address
`/reseptit/<slug>`: lowercase letters a-z, digits and single dashes, at most 100
characters, unique among recipes (the database enforces all of this). In the
editor the slug follows the name (ä becomes a, spaces become dashes) until you
edit it yourself. Changing a slug changes the page address, so links to the
old address stop working.

Descriptions may be up to 50,000 characters. Lists (`GET /api/recipes`) carry
only an `excerpt`, the text of the first paragraph block, to stay small.

**Images.** Upload images on the **Kuvat** page in the admin view (button, or
drag files onto the page), or straight from the image picker in the editor. The
Kuvat page shows how many images there are and how much disk space their files
take in total; clicking an image shows its details (name, file type, size,
dimensions, upload date, use) in the panel beside the images.

- The browser scales each image down to at most 1600 pixels on the longer side
  and saves it as JPEG before uploading. That keeps files small and removes the
  metadata phones store in photos, such as location.
- Files are stored in the uploads directory (`-u`, `/data/uploads` in Docker
  and on Azure) as `<id>.jpg`, served at `/uploads/<id>.jpg` with permanent
  caching (ids are never reused), and listed in the `images` table.
- An image that a recipe shows (as its card image or in a block) cannot be
  deleted; the gallery shows how many recipes use each image.
- In the image picker, a click shows the image's details (name, file type,
  size, dimensions, upload date, use) in the panel on the right, and a double
  click chooses it. Hovering over an image shows an X that deletes it. On a
  touch screen a tap shows the details, a double tap chooses, and holding an
  image opens a menu: Käytä kuvaa, Kuvan tiedot, Poista.

**Large requests.** Requests normally use each connection's fixed 8 KiB
buffer. Uploads and long descriptions borrow one of a few 2 MiB buffers that
each worker reserves at startup, so nothing is allocated while the server
runs. Only signed-in requests may borrow one, and the connection closes after
such a request so the buffer is free again at once. When every buffer is busy,
the server answers 503 and the browser can retry.
