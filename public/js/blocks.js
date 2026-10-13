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

var KasinBlocks = (function () {
  var TITLE_LEVELS = [2, 3, 4];
  var MARKER = /<!-- (\/?)block:([a-z][a-z0-9_]*)(?: (\{[\s\S]*?\}))? (\/?)-->/g;

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
      var value = JSON.parse(json);
      return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
    } catch (e) {
      return {};
    }
  }

  function asString(value) {
    return typeof value === 'string' ? value : '';
  }

  // Constructors for the in-memory form of each block.

  function paragraph(text) {
    return { type: 'paragraph', text: text || '' };
  }

  function title(level, text) {
    return { type: 'title', level: TITLE_LEVELS.indexOf(level) >= 0 ? level : 2, text: text || '' };
  }

  var MIN_CROP = 0.02;  // Smallest crop side, as a fraction of the picture.

  function number(value) {
    return typeof value === 'number' && isFinite(value) ? value : NaN;
  }

  // A crop that stays inside the picture, or null for the whole picture.
  function cropOption(value) {
    if (!value || typeof value !== 'object') {
      return null;
    }
    var x = number(value.x);
    var y = number(value.y);
    var w = number(value.w);
    var h = number(value.h);
    if (!(x >= 0 && y >= 0 && w >= MIN_CROP && h >= MIN_CROP && x + w <= 1.0001 && y + h <= 1.0001)) {
      return null;
    }
    if (x < 0.0001 && y < 0.0001 && w > 0.9999 && h > 0.9999) {
      return null;
    }
    return { x: x, y: y, w: w, h: h };
  }

  function scaleOption(value) {
    var scale = number(value);
    return scale >= 10 && scale <= 100 ? Math.round(scale) : 100;
  }

  function sizeOption(value) {
    var size = number(value);
    return size >= 1 && size <= 100000 ? Math.round(size) : 0;
  }

  // Rounds a crop for storage, so a stored crop reads back exactly.
  function roundCrop(crop) {
    function round(value) {
      return Math.round(value * 10000) / 10000;
    }
    return crop ? { x: round(crop.x), y: round(crop.y), w: round(crop.w), h: round(crop.h) } : null;
  }

  // display: optional {crop, scale, width, height}.
  function image(src, alt, caption, display) {
    display = display || {};
    var width = sizeOption(display.width);
    var height = sizeOption(display.height);
    return {
      type: 'image',
      src: src || '',
      alt: alt || '',
      caption: caption || '',
      crop: width && height ? cropOption(display.crop) : null,
      scale: scaleOption(display.scale),
      width: width,
      height: height
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
        var options = { src: block.src, alt: block.alt };
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
    return blocks.map(serializeBlock).filter(Boolean).join('\n\n');
  }

  // Parsing.

  function looseParagraphs(text) {
    return text.split(/\n[ \t]*\n/)
      .map(function (part) { return part.trim(); })
      .filter(Boolean)
      .map(paragraph);
  }

  function makeBlock(name, options, content, children) {
    switch (name) {
      case 'paragraph':
        return paragraph(content);
      case 'title':
        return title(options.level, content);
      case 'image':
        return image(asString(options.src), asString(options.alt), asString(options.caption), options);
      case 'layout': {
        var columns = children.filter(function (child) {
          return child.type !== 'layout';  // Layouts do not nest.
        }).map(function (child) {
          return child.type === 'empty' ? null : child;
        });
        return layout(columns[0], columns[1]);
      }
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

  function parse(text) {
    text = typeof text === 'string' ? text : '';
    var root = { name: null, children: [] };
    var stack = [root];
    var position = 0;
    var match;
    MARKER.lastIndex = 0;

    function top() {
      return stack[stack.length - 1];
    }
    function closeOpen(frame, end) {
      var block = makeBlock(frame.name, frame.options, blockText(text.slice(frame.start, end)), frame.children);
      if (block) {
        stack[stack.length - 1].children.push(block);
      }
    }

    while ((match = MARKER.exec(text)) !== null) {
      var before = text.slice(position, match.index);
      position = MARKER.lastIndex;
      var closing = match[1] === '/';
      var name = match[2];
      var selfClosing = match[4] === '/';
      var current = top();

      if (closing) {
        if (current.name === name) {
          stack.pop();
          closeOpen(current, match.index);
        }
        continue;  // A stray closing marker is ignored.
      }
      if (current === root) {
        Array.prototype.push.apply(root.children, looseParagraphs(before));
      }
      if (current.name === 'paragraph' || current.name === 'title') {
        continue;  // Markers cannot appear inside text; ignore a malformed one.
      }
      if (selfClosing) {
        var block = makeBlock(name, parseOptions(match[3]), '', []);
        if (block) {
          current.children.push(block);
        }
      } else {
        stack.push({ name: name, options: parseOptions(match[3]), start: position, children: [] });
      }
    }
    if (stack.length > 1) {
      // Blocks left open take the rest of the text and are closed at its end.
      while (stack.length > 1) {
        closeOpen(stack.pop(), text.length);
      }
    } else if (position < text.length) {
      Array.prototype.push.apply(root.children, looseParagraphs(text.slice(position)));
    }
    return root.children.filter(function (block) {
      return block.type !== 'empty';
    });
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
    var src = safeSrc(block.src);
    if (!src) {
      return null;
    }
    var img = document.createElement('img');
    img.src = src;
    img.alt = block.alt;
    img.loading = 'lazy';
    var crop = block.crop;
    if (!crop || !block.width || !block.height) {
      return img;
    }
    var frame = document.createElement('div');
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
        var p = document.createElement('p');
        p.className = 'block-paragraph';
        p.textContent = block.text;
        return p;
      }
      case 'title': {
        var heading = document.createElement('h' + block.level);
        heading.className = 'block-title';
        heading.textContent = block.text;
        return heading;
      }
      case 'image': {
        var src = safeSrc(block.src);
        if (!src) {
          return null;
        }
        var figure = document.createElement('figure');
        figure.className = 'block-image';
        if (block.scale < 100) {
          figure.style.width = block.scale + '%';
        }
        figure.appendChild(imageElement(block));
        if (block.caption) {
          var caption = document.createElement('figcaption');
          caption.textContent = block.caption;
          figure.appendChild(caption);
        }
        return figure;
      }
      case 'layout': {
        var row = document.createElement('div');
        row.className = 'block-layout';
        block.columns.forEach(function (column) {
          var cell = document.createElement('div');
          cell.className = 'block-column';
          var content = renderBlock(column);
          if (content) {
            cell.appendChild(content);
          }
          row.appendChild(cell);
        });
        return row;
      }
      default:
        return null;
    }
  }

  function render(blocks, container) {
    var nodes = blocks.map(renderBlock).filter(Boolean);
    container.replaceChildren.apply(container, nodes);
  }

  return {
    TITLE_LEVELS: TITLE_LEVELS,
    create: create,
    parse: parse,
    serialize: serialize,
    render: render,
    imageElement: imageElement,
    cropOption: cropOption,
    roundCrop: roundCrop,
    safeSrc: safeSrc
  };
})();
