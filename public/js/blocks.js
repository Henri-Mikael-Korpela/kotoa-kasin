// Recipe description blocks: parsing, serializing and rendering.
//
// A description is plain text in which every block is marked with HTML comments,
// so blocks stay distinct from ordinary text:
//
//   <!-- block:title {"level":2} -->
//   Ainekset
//   <!-- /block:title -->
//
//   <!-- block:paragraph -->
//   Sulata mehiläisvaha vesihauteessa.
//   <!-- /block:paragraph -->
//
//   <!-- block:image {"src":"/uploads/3.jpg","alt":"Valmis voide"} /-->
//
//   <!-- block:layout -->
//   <!-- block:image {"src":"/uploads/4.jpg","alt":""} /-->
//   <!-- block:paragraph -->
//   Teksti kuvan oikealla puolella.
//   <!-- /block:paragraph -->
//   <!-- /block:layout -->
//
// Block names: paragraph, title, image, layout, and empty (an unused layout
// column). An image may also say how it is shown, without touching the file:
//
//   <!-- block:image {"src":"/uploads/3.jpg","alt":"","crop":{"x":0.1,"y":0,
//        "w":0.8,"h":0.75},"width":1600,"height":1200,"scale":60} /-->
//
// crop is the part shown, as fractions of the picture (left, top, width,
// height); its centre is the point the picture is centred on. width and height
// are the picture's pixel size, which the crop needs to keep its shape. scale
// is the image's width on the page as a percentage of the text column. Options are a JSON object after the name; "<" and ">" inside it are
// written as \u003c and \u003e so it can never end the comment early. In block
// text, "\" is written as "\\" and "<!--" as "<\!--", so text cannot start a
// marker either. Text outside any block (descriptions written before the
// editor existed) is read as paragraphs, split at blank lines.

const KasinBlocks = (function () {
    const TITLE_LEVELS = [2, 3, 4];
    const MARKER = /<!-- (\/?)block:([a-z][a-z0-9_]*)(?: (\{[\s\S]*?\}))? (\/?)-->/g;
    const MIN_CROP = 0.02;  // Smallest crop side, as a fraction of the picture.

    function escapeText(text) {
        return text.replace(/\\/g, '\\\\').replace(/<!--/g, '<\\!--');
    }

    function unescapeText(text) {
        return text.replace(/\\\\|<\\!--/g, function (match) {
            return match === '\\\\' ? '\\' : '<!--';
        });
    }

    function optionsJson(options) {
        return JSON.stringify(options).replace(/</g, '\\u003c').replace(/>/g, '\\u003e');
    }

    function parseOptions(json) {
        if (!json) {
            return {};
        }
        try {
            const value = JSON.parse(json);
            return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
        } catch (e) {
            return {};
        }
    }

    function asString(value) {
        return typeof value === 'string' ? value : '';
    }

    function number(value) {
        return typeof value === 'number' && isFinite(value) ? value : NaN;
    }

    // Constructors for the in-memory form of each block. Each returns a new object.

    function paragraph(text) {
        return { type: 'paragraph', text: text || '' };
    }

    function title(level, text) {
        return { type: 'title', level: TITLE_LEVELS.includes(level) ? level : 2, text: text || '' };
    }

    // A crop that stays inside the picture, or null for the whole picture.
    function cropOption(value) {
        if (!value || typeof value !== 'object') {
            return null;
        }
        const x = number(value.x);
        const y = number(value.y);
        const w = number(value.w);
        const h = number(value.h);
        if (!(x >= 0 && y >= 0 && w >= MIN_CROP && h >= MIN_CROP && x + w <= 1.0001 && y + h <= 1.0001)) {
            return null;
        }
        if (x < 0.0001 && y < 0.0001 && w > 0.9999 && h > 0.9999) {
            return null;
        }
        return { x, y, w, h };
    }

    function scaleOption(value) {
        const scale = number(value);
        return scale >= 10 && scale <= 100 ? Math.round(scale) : 100;
    }

    function sizeOption(value) {
        const size = number(value);
        return size >= 1 && size <= 100000 ? Math.round(size) : 0;
    }

    function round(value) {
        return Math.round(value * 10000) / 10000;
    }

    // Rounds a crop for storage, so a stored crop reads back exactly.
    function roundCrop(crop) {
        if (!crop) {
            return null;
        }
        return { x: round(crop.x), y: round(crop.y), w: round(crop.w), h: round(crop.h) };
    }

    // display: optional {crop, scale, width, height}.
    function image(src, alt, caption, display) {
        const shown = display || {};
        const width = sizeOption(shown.width);
        const height = sizeOption(shown.height);
        return {
            type: 'image',
            src: src || '',
            alt: alt || '',
            caption: caption || '',
            crop: width && height ? cropOption(shown.crop) : null,
            scale: scaleOption(shown.scale),
            width,
            height,
        };
    }

    function layout(left, right) {
        return { type: 'layout', columns: [left || null, right || null] };
    }

    function create(type) {
        if (type === 'title') {
            return title(2, '');
        }
        if (type === 'image') {
            return image('', '', '');
        }
        if (type === 'layout') {
            return layout(null, null);
        }
        return paragraph('');
    }

    // Serializing.

    function serializeBlock(block) {
        if (!block) {
            return '<!-- block:empty /-->';
        }
        switch (block.type) {
            case 'paragraph':
                return '<!-- block:paragraph -->\n' + escapeText(block.text) + '\n<!-- /block:paragraph -->';
            case 'title':
                return '<!-- block:title ' + optionsJson({ level: block.level }) + ' -->\n' +
                    escapeText(block.text) + '\n<!-- /block:title -->';
            case 'image': {
                const options = { src: block.src, alt: block.alt };
                if (block.caption) {
                    options.caption = block.caption;
                }
                if (block.crop && block.width && block.height) {
                    options.crop = roundCrop(block.crop);
                    options.width = block.width;
                    options.height = block.height;
                }
                if (block.scale !== 100) {
                    options.scale = block.scale;
                }
                return '<!-- block:image ' + optionsJson(options) + ' /-->';
            }
            case 'layout':
                return '<!-- block:layout -->\n' + serializeBlock(block.columns[0]) + '\n' +
                    serializeBlock(block.columns[1]) + '\n<!-- /block:layout -->';
            default:
                return '';
        }
    }

    function serialize(blocks) {
        const parts = [];
        for (const block of blocks) {
            const text = serializeBlock(block);
            if (text) {
                parts.push(text);
            }
        }
        return parts.join('\n\n');
    }

    // Parsing.

    function looseParagraphs(text) {
        const paragraphs = [];
        for (const part of text.split(/\n[ \t]*\n/)) {
            const trimmed = part.trim();
            if (trimmed) {
                paragraphs.push(paragraph(trimmed));
            }
        }
        return paragraphs;
    }

    function layoutFromChildren(children) {
        const columns = [];
        for (const child of children) {
            if (child.type === 'layout') {
                continue;  // Layouts do not nest.
            }
            columns.push(child.type === 'empty' ? null : child);
        }
        return layout(columns[0], columns[1]);
    }

    function makeBlock(name, options, content, children) {
        switch (name) {
            case 'paragraph':
                return paragraph(content);
            case 'title':
                return title(options.level, content);
            case 'image':
                return image(asString(options.src), asString(options.alt), asString(options.caption), options);
            case 'layout':
                return layoutFromChildren(children);
            case 'empty':
                return { type: 'empty' };
            default:
                return null;  // Unknown block: dropped.
        }
    }

    // Text between a block's markers, without the newlines that frame it.
    function blockText(raw) {
        return unescapeText(raw.replace(/^\n/, '').replace(/\n$/, ''));
    }

    function parse(input) {
        const text = typeof input === 'string' ? input : '';
        const root = { name: null, children: [] };
        const stack = [root];
        let position = 0;
        MARKER.lastIndex = 0;

        function closeOpen(frame, end) {
            const block = makeBlock(frame.name, frame.options, blockText(text.slice(frame.start, end)), frame.children);
            if (block) {
                stack[stack.length - 1].children.push(block);
            }
        }

        let match = MARKER.exec(text);
        while (match !== null) {
            const before = text.slice(position, match.index);
            position = MARKER.lastIndex;
            const closing = match[1] === '/';
            const name = match[2];
            const selfClosing = match[4] === '/';
            const current = stack[stack.length - 1];

            if (closing) {
                if (current.name === name) {
                    stack.pop();
                    closeOpen(current, match.index);
                }
                // A stray closing marker is ignored.
            } else {
                if (current === root) {
                    root.children.push(...looseParagraphs(before));
                }
                if (current.name === 'paragraph' || current.name === 'title') {
                    // Markers cannot appear inside text; ignore a malformed one.
                } else if (selfClosing) {
                    const block = makeBlock(name, parseOptions(match[3]), '', []);
                    if (block) {
                        current.children.push(block);
                    }
                } else {
                    stack.push({ name, options: parseOptions(match[3]), start: position, children: [] });
                }
            }
            match = MARKER.exec(text);
        }

        if (stack.length > 1) {
            // Blocks left open take the rest of the text and are closed at its end.
            while (stack.length > 1) {
                closeOpen(stack.pop(), text.length);
            }
        } else if (position < text.length) {
            root.children.push(...looseParagraphs(text.slice(position)));
        }

        const blocks = [];
        for (const block of root.children) {
            if (block.type !== 'empty') {
                blocks.push(block);
            }
        }
        return blocks;
    }

    // Rendering for the public site. All text goes through textContent.

    // Only images on this site: a path starting with one slash.
    function safeSrc(src) {
        return typeof src === 'string' && /^\/(?!\/)[^\\\s]*$/.test(src) ? src : '';
    }

    /*
     * The picture of an image block, cropped as its options say: a frame with
     * the crop's shape, and the whole picture inside it, scaled and shifted so
     * only the crop shows.
     */
    function imageElement(block) {
        const src = safeSrc(block.src);
        if (!src) {
            return null;
        }
        const img = document.createElement('img');
        img.src = src;
        img.alt = block.alt;
        img.loading = 'lazy';
        const crop = block.crop;
        if (!crop || !block.width || !block.height) {
            return img;
        }
        const frame = document.createElement('div');
        frame.className = 'block-image-frame';
        frame.style.aspectRatio = (crop.w * block.width).toFixed(2) + ' / ' + (crop.h * block.height).toFixed(2);
        img.style.width = (100 / crop.w).toFixed(4) + '%';
        img.style.height = (100 / crop.h).toFixed(4) + '%';
        img.style.left = (-100 * crop.x / crop.w).toFixed(4) + '%';
        img.style.top = (-100 * crop.y / crop.h).toFixed(4) + '%';
        frame.appendChild(img);
        return frame;
    }

    function renderBlock(block) {
        if (!block) {
            return null;
        }
        switch (block.type) {
            case 'paragraph': {
                const p = document.createElement('p');
                p.className = 'block-paragraph';
                p.textContent = block.text;
                return p;
            }
            case 'title': {
                const heading = document.createElement('h' + block.level);
                heading.className = 'block-title';
                heading.textContent = block.text;
                return heading;
            }
            case 'image': {
                const src = safeSrc(block.src);
                if (!src) {
                    return null;
                }
                const figure = document.createElement('figure');
                figure.className = 'block-image';
                if (block.scale < 100) {
                    figure.style.width = block.scale + '%';
                }
                figure.appendChild(imageElement(block));
                if (block.caption) {
                    const caption = document.createElement('figcaption');
                    caption.textContent = block.caption;
                    figure.appendChild(caption);
                }
                return figure;
            }
            case 'layout': {
                const row = document.createElement('div');
                row.className = 'block-layout';
                for (const column of block.columns) {
                    const cell = document.createElement('div');
                    cell.className = 'block-column';
                    const content = renderBlock(column);
                    if (content) {
                        cell.appendChild(content);
                    }
                    row.appendChild(cell);
                }
                return row;
            }
            default:
                return null;
        }
    }

    function render(blocks, container) {
        const nodes = [];
        for (const block of blocks) {
            const node = renderBlock(block);
            if (node) {
                nodes.push(node);
            }
        }
        container.replaceChildren(...nodes);
    }

    return {
        TITLE_LEVELS,
        create,
        parse,
        serialize,
        render,
        imageElement,
        cropOption,
        roundCrop,
        safeSrc,
    };
})();
