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
// column). Options are a JSON object after the name; "<" and ">" inside it are
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

  function image(src, alt, caption) {
    return { type: 'image', src: src || '', alt: alt || '', caption: caption || '' };
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
        return image(asString(options.src), asString(options.alt), asString(options.caption));
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
        var img = document.createElement('img');
        img.src = src;
        img.alt = block.alt;
        img.loading = 'lazy';
        figure.appendChild(img);
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
    safeSrc: safeSrc
  };
})();
