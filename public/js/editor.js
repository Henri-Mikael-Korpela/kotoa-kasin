// Block editor for recipe descriptions, in the spirit of WordPress Gutenberg.
//
// The description is a list of blocks (see blocks.js for the stored format).
// Blocks are added from the bar at the end of the list, dragged by their handle
// (mouse or touch) or moved with the arrow buttons, and removed with the cross.
// A layout block puts two blocks side by side; its columns hold a paragraph,
// title or image each.
//
// Like a native form field, the editor fires a bubbling "input" event on its
// container whenever its content changes, so a live preview can follow it.

var KasinEditor = (function () {
  var LABELS = {
    paragraph: 'Kappale',
    title: 'Otsikko',
    image: 'Kuva',
    layout: 'Asettelu'
  };
  var COLUMN_TYPES = ['paragraph', 'title', 'image'];
  var BLOCK_TYPES = ['paragraph', 'title', 'image', 'layout'];

  function button(text, className, label) {
    var b = document.createElement('button');
    b.type = 'button';
    b.className = className;
    b.textContent = text;
    if (label) {
      b.setAttribute('aria-label', label);
      b.title = label;
    }
    return b;
  }

  function autosize(textarea) {
    textarea.style.height = 'auto';
    textarea.style.height = textarea.scrollHeight + 2 + 'px';
  }

  // Editing controls for one paragraph, title or image block. Inputs write
  // straight into the block object.
  function createBody(block) {
    var body = document.createElement('div');
    body.className = 'block-body block-body-' + block.type;

    if (block.type === 'paragraph') {
      var text = document.createElement('textarea');
      text.className = 'block-text';
      text.rows = 3;
      text.placeholder = 'Kirjoita kappale…';
      text.value = block.text;
      text.setAttribute('aria-label', 'Kappaleen teksti');
      text.addEventListener('input', function () {
        block.text = text.value;
        autosize(text);
      });
      body.appendChild(text);
      window.requestAnimationFrame(function () {
        autosize(text);
      });
    } else if (block.type === 'title') {
      var level = document.createElement('select');
      level.className = 'block-title-level';
      level.setAttribute('aria-label', 'Otsikon taso');
      KasinBlocks.TITLE_LEVELS.forEach(function (value) {
        var option = document.createElement('option');
        option.value = String(value);
        option.textContent = 'H' + value;
        level.appendChild(option);
      });
      level.value = String(block.level);
      var heading = document.createElement('input');
      heading.type = 'text';
      heading.className = 'block-title-text block-title-text-' + block.level;
      heading.placeholder = 'Otsikko';
      heading.value = block.text;
      heading.setAttribute('aria-label', 'Otsikon teksti');
      level.addEventListener('change', function () {
        block.level = Number(level.value);
        heading.className = 'block-title-text block-title-text-' + block.level;
      });
      heading.addEventListener('input', function () {
        block.text = heading.value;
      });
      body.append(level, heading);
    } else if (block.type === 'image') {
      var preview = document.createElement('div');
      preview.className = 'block-image-preview';
      var choose = button(block.src ? 'Vaihda kuva' : 'Valitse kuva', 'btn btn-ghost block-image-choose');
      var fields = document.createElement('div');
      fields.className = 'block-image-fields';
      var alt = document.createElement('input');
      alt.type = 'text';
      alt.placeholder = 'Kuvan kuvaus näkövammaisille (alt-teksti)';
      alt.maxLength = 300;
      alt.value = block.alt;
      alt.setAttribute('aria-label', 'Kuvan kuvaus');
      var caption = document.createElement('input');
      caption.type = 'text';
      caption.placeholder = 'Kuvateksti (valinnainen)';
      caption.maxLength = 300;
      caption.value = block.caption;
      caption.setAttribute('aria-label', 'Kuvateksti');

      function showPreview() {
        var src = KasinBlocks.safeSrc(block.src);
        if (src) {
          var img = document.createElement('img');
          img.src = src;
          img.alt = '';
          preview.replaceChildren(img);
        } else {
          var empty = document.createElement('span');
          empty.textContent = 'Ei kuvaa';
          preview.replaceChildren(empty);
        }
        choose.textContent = src ? 'Vaihda kuva' : 'Valitse kuva';
      }
      choose.addEventListener('click', function () {
        KasinGallery.pick().then(function (image) {
          if (image) {
            block.src = image.url;
            showPreview();
            alt.focus();
            body.dispatchEvent(new Event('input', { bubbles: true }));
          }
        });
      });
      alt.addEventListener('input', function () {
        block.alt = alt.value;
      });
      caption.addEventListener('input', function () {
        block.caption = caption.value;
      });
      showPreview();
      fields.append(choose, alt, caption);
      body.append(preview, fields);
    }
    return body;
  }

  function create(container, initialText) {
    var blocks = KasinBlocks.parse(initialText);
    var list = document.createElement('div');
    list.className = 'block-list';
    var inserter = document.createElement('div');
    inserter.className = 'block-inserter';
    var status = document.createElement('p');
    status.className = 'visually-hidden';
    status.setAttribute('aria-live', 'polite');

    var label = document.createElement('span');
    label.className = 'block-inserter-label';
    label.textContent = 'Lisää lohko:';
    inserter.appendChild(label);
    BLOCK_TYPES.forEach(function (type) {
      var add = button('+ ' + LABELS[type], 'block-add');
      add.addEventListener('click', function () {
        blocks.push(KasinBlocks.create(type));
        render();
        focusBlock(blocks.length - 1);
      });
      inserter.appendChild(add);
    });

    container.replaceChildren(list, inserter, status);

    function focusBlock(index) {
      var item = list.children[index];
      var field = item && item.querySelector('textarea, input, button.block-image-choose, .layout-column button');
      if (field) {
        field.focus();
      }
    }

    function move(from, to) {
      if (to < 0 || to >= blocks.length || from === to) {
        return;
      }
      var block = blocks.splice(from, 1)[0];
      blocks.splice(to, 0, block);
      render();
      status.textContent = LABELS[block.type] + ' siirretty kohtaan ' + (to + 1) + '.';
    }

    function remove(index) {
      var block = blocks.splice(index, 1)[0];
      render();
      status.textContent = LABELS[block.type] + ' poistettu.';
      focusBlock(Math.min(index, blocks.length - 1));
    }

    function createLayoutBody(block) {
      var body = document.createElement('div');
      body.className = 'block-body block-body-layout';
      block.columns.forEach(function (column, side) {
        var cell = document.createElement('div');
        cell.className = 'layout-column';
        var heading = document.createElement('div');
        heading.className = 'layout-column-header';
        var name = document.createElement('span');
        name.textContent = (side === 0 ? 'Vasen' : 'Oikea') + (column ? ': ' + LABELS[column.type].toLowerCase() : '');
        heading.appendChild(name);

        if (column) {
          var clear = button('✕', 'block-icon', 'Tyhjennä ' + (side === 0 ? 'vasen' : 'oikea') + ' sarake');
          clear.addEventListener('click', function () {
            block.columns[side] = null;
            render();
          });
          heading.appendChild(clear);
          cell.append(heading, createBody(column));
        } else {
          var choices = document.createElement('div');
          choices.className = 'layout-column-choices';
          COLUMN_TYPES.forEach(function (type) {
            var add = button('+ ' + LABELS[type], 'block-add');
            add.addEventListener('click', function () {
              block.columns[side] = KasinBlocks.create(type);
              render();
            });
            choices.appendChild(add);
          });
          cell.append(heading, choices);
        }
        body.appendChild(cell);
      });
      return body;
    }

    function createItem(block, index) {
      var item = document.createElement('div');
      item.className = 'block-item block-item-' + block.type;

      var toolbar = document.createElement('div');
      toolbar.className = 'block-toolbar';
      var handle = button('⠿', 'block-handle', 'Siirrä vetämällä: ' + LABELS[block.type]);
      var name = document.createElement('span');
      name.className = 'block-name';
      name.textContent = LABELS[block.type];
      var up = button('↑', 'block-icon', 'Siirrä ylös');
      var down = button('↓', 'block-icon', 'Siirrä alas');
      var close = button('✕', 'block-icon block-remove', 'Poista ' + LABELS[block.type].toLowerCase());
      up.disabled = index === 0;
      down.disabled = index === blocks.length - 1;
      up.addEventListener('click', function () {
        move(index, index - 1);
        list.children[index - 1].querySelector('.block-icon').focus();
      });
      down.addEventListener('click', function () {
        move(index, index + 1);
        var moved = list.children[index + 1];
        var target = moved.querySelectorAll('.block-icon')[1];
        (target.disabled ? moved.querySelector('.block-icon') : target).focus();
      });
      close.addEventListener('click', function () {
        remove(index);
      });
      toolbar.append(handle, name);
      if (block.type === 'layout') {
        var swap = button('⇄', 'block-icon', 'Vaihda sarakkeiden paikkaa');
        swap.addEventListener('click', function () {
          block.columns.reverse();
          render();
        });
        toolbar.appendChild(swap);
      }
      toolbar.append(up, down, close);

      item.append(toolbar, block.type === 'layout' ? createLayoutBody(block) : createBody(block));
      handle.addEventListener('pointerdown', function (event) {
        startDrag(event, item, index);
      });
      return item;
    }

    function render() {
      var nodes = blocks.map(createItem);
      if (nodes.length === 0) {
        var empty = document.createElement('p');
        empty.className = 'block-empty';
        empty.textContent = 'Kuvaus on tyhjä. Lisää ensimmäinen lohko alta.';
        nodes.push(empty);
      }
      list.replaceChildren.apply(list, nodes);
      // Blocks were added, moved or removed.
      container.dispatchEvent(new Event('input', { bubbles: true }));
    }

    // Dragging: the block follows the pointer and a line shows where it lands.
    function startDrag(event, item, from) {
      if (event.button !== 0) {
        return;
      }
      event.preventDefault();
      var handle = event.currentTarget;
      try {
        handle.setPointerCapture(event.pointerId);  // Keeps the drag going outside the handle.
      } catch (e) {
        // Not every pointer can be captured; the drag still works over the handle.
      }
      var startY = event.clientY;
      var startScroll = window.scrollY;
      var target = from;
      var marker = document.createElement('div');
      marker.className = 'block-drop-marker';
      item.classList.add('is-dragging');
      document.body.classList.add('is-dragging-block');

      function targetFor(y) {
        var items = Array.prototype.filter.call(list.children, function (child) {
          return child !== item && child.classList.contains('block-item');
        });
        var index = 0;
        items.forEach(function (child) {
          var box = child.getBoundingClientRect();
          if (y > box.top + box.height / 2) {
            index++;
          }
        });
        return index;
      }

      function placeMarker(index) {
        var others = Array.prototype.filter.call(list.children, function (child) {
          return child !== item && child.classList.contains('block-item');
        });
        if (index < others.length) {
          list.insertBefore(marker, others[index]);
        } else {
          list.appendChild(marker);
        }
      }

      function onMove(moveEvent) {
        // Scroll when dragging close to the top or bottom of the window.
        if (moveEvent.clientY < 60) {
          window.scrollBy(0, -12);
        } else if (moveEvent.clientY > window.innerHeight - 60) {
          window.scrollBy(0, 12);
        }
        var offset = moveEvent.clientY - startY + (window.scrollY - startScroll);
        item.style.transform = 'translateY(' + offset + 'px)';
        target = targetFor(moveEvent.clientY);
        placeMarker(target);
      }

      function onEnd() {
        handle.removeEventListener('pointermove', onMove);
        handle.removeEventListener('pointerup', onEnd);
        handle.removeEventListener('pointercancel', onEnd);
        document.body.classList.remove('is-dragging-block');
        marker.remove();
        item.style.transform = '';
        item.classList.remove('is-dragging');
        if (target !== from) {
          move(from, target);
        }
      }

      handle.addEventListener('pointermove', onMove);
      handle.addEventListener('pointerup', onEnd);
      handle.addEventListener('pointercancel', onEnd);
    }

    render();

    return {
      serialize: function () {
        return KasinBlocks.serialize(blocks);
      },
      isEmpty: function () {
        return blocks.length === 0;
      }
    };
  }

  return {
    create: create
  };
})();
