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

  var MIN_CROP = 0.05;  // Smallest crop side while dragging, as a fraction of the picture.
  var FULL_CROP = { x: 0, y: 0, w: 1, h: 1 };

  function clamp(value, low, high) {
    return Math.min(high, Math.max(low, value));
  }

  /*
   * Image display editor. Shows the picture with a crop frame: drag a corner
   * to crop, drag inside the frame to choose which part is centred (the arrow
   * keys move it too), and set the size the image takes on the page. The file
   * itself is never changed; the settings are stored on the block.
   *
   * Resolves with true when the settings were saved, false when cancelled.
   */
  function editDisplay(block) {
    return new Promise(function (resolve) {
      var dialog = document.createElement('dialog');
      dialog.className = 'image-display-dialog';
      dialog.innerHTML =
        '<div class="gallery-dialog-header">' +
        '  <h2>Kuvan näkymä</h2>' +
        '  <button type="button" class="category-button" data-cancel>Peruuta</button>' +
        '</div>' +
        '<div class="image-display-body">' +
        '  <div class="crop-area">' +
        '    <div class="crop-stage">' +
        '      <img class="crop-image" alt="" draggable="false">' +
        '      <div class="crop-box" tabindex="0" role="group"' +
        '        aria-label="Rajaus. Vedä siirtääksesi keskikohtaa, nuolinäppäimet siirtävät.">' +
        '        <span class="crop-center" aria-hidden="true"></span>' +
        '        <span class="crop-handle" data-corner="nw" aria-hidden="true"></span>' +
        '        <span class="crop-handle" data-corner="ne" aria-hidden="true"></span>' +
        '        <span class="crop-handle" data-corner="sw" aria-hidden="true"></span>' +
        '        <span class="crop-handle" data-corner="se" aria-hidden="true"></span>' +
        '      </div>' +
        '    </div>' +
        '    <p class="crop-loading">Ladataan kuvaa…</p>' +
        '  </div>' +
        '  <div class="crop-side">' +
        '    <p class="form-hint">Rajaa vetämällä kulmista. Vedä kuvaa rajauksen sisällä valitaksesi, mikä kohta jää keskelle.</p>' +
        '    <div class="form-field">' +
        '      <label for="cropScale">Koko sivulla: <output class="crop-scale-value"></output></label>' +
        '      <input type="range" id="cropScale" class="crop-scale" min="25" max="100" step="5">' +
        '    </div>' +
        '    <p class="crop-dimensions"></p>' +
        '    <span class="form-label">Esikatselu</span>' +
        '    <div class="crop-result"></div>' +
        '  </div>' +
        '</div>' +
        '<div class="image-display-actions">' +
        '  <button type="button" class="btn btn-ghost" data-reset>Palauta alkuperäinen</button>' +
        '  <button type="button" class="btn btn-primary" data-done>Valmis</button>' +
        '</div>';
      document.body.appendChild(dialog);

      var area = dialog.querySelector('.crop-area');
      var stage = dialog.querySelector('.crop-stage');
      var picture = dialog.querySelector('.crop-image');
      var box = dialog.querySelector('.crop-box');
      var loading = dialog.querySelector('.crop-loading');
      var scaleInput = dialog.querySelector('.crop-scale');
      var scaleValue = dialog.querySelector('.crop-scale-value');
      var dimensions = dialog.querySelector('.crop-dimensions');
      var result = dialog.querySelector('.crop-result');
      var crop = Object.assign({}, block.crop || FULL_CROP);
      var scale = block.scale;
      var finished = false;

      function finish(save) {
        if (finished) {
          return;
        }
        finished = true;
        if (resizeWatcher) {
          resizeWatcher.disconnect();
        }
        if (save && picture.naturalWidth) {
          block.crop = KasinBlocks.cropOption(KasinBlocks.roundCrop(crop));
          block.width = picture.naturalWidth;
          block.height = picture.naturalHeight;
          block.scale = scale;
        }
        if (dialog.open) {
          dialog.close();
        }
        dialog.remove();
        resolve(save);
      }
      dialog.addEventListener('close', function () {
        finish(false);  // Esc.
      });
      dialog.querySelector('[data-cancel]').addEventListener('click', function () {
        finish(false);
      });
      dialog.querySelector('[data-done]').addEventListener('click', function () {
        finish(true);
      });
      dialog.querySelector('[data-reset]').addEventListener('click', function () {
        crop = Object.assign({}, FULL_CROP);
        scale = 100;
        update();
      });

      function update() {
        box.style.left = crop.x * 100 + '%';
        box.style.top = crop.y * 100 + '%';
        box.style.width = crop.w * 100 + '%';
        box.style.height = crop.h * 100 + '%';
        scaleInput.value = String(scale);
        scaleValue.textContent = scale + ' %';
        if (picture.naturalWidth) {
          dimensions.textContent = 'Näkyvä alue: ' + Math.round(crop.w * picture.naturalWidth) + ' × ' +
            Math.round(crop.h * picture.naturalHeight) + ' px (kuva ' + picture.naturalWidth + ' × ' +
            picture.naturalHeight + ' px)';
        }
        // The preview column stands for the page's text column.
        var figure = document.createElement('figure');
        figure.className = 'block-image';
        figure.style.width = scale + '%';
        var shown = KasinBlocks.imageElement({
          src: block.src,
          alt: '',
          crop: KasinBlocks.cropOption(crop),
          width: picture.naturalWidth,
          height: picture.naturalHeight
        });
        if (shown) {
          figure.appendChild(shown);
        }
        result.replaceChildren(figure);
      }

      scaleInput.addEventListener('input', function () {
        scale = Number(scaleInput.value);
        update();
      });

      // Dragging: a corner resizes the crop, the inside moves it.
      function startDrag(event) {
        if (event.button !== 0 || !picture.naturalWidth) {
          return;
        }
        event.preventDefault();
        box.focus();
        var corner = event.target.dataset ? event.target.dataset.corner : null;
        var area = picture.getBoundingClientRect();
        var start = Object.assign({}, crop);
        var startX = event.clientX;
        var startY = event.clientY;
        var target = event.currentTarget;
        try {
          target.setPointerCapture(event.pointerId);
        } catch (e) {
          // Without capture the drag still follows the pointer over the frame.
        }
        box.classList.add('is-dragging');

        function onMove(moveEvent) {
          var dx = (moveEvent.clientX - startX) / area.width;
          var dy = (moveEvent.clientY - startY) / area.height;
          if (!corner) {
            crop.x = clamp(start.x + dx, 0, 1 - start.w);
            crop.y = clamp(start.y + dy, 0, 1 - start.h);
          } else {
            var left = start.x;
            var top = start.y;
            var right = start.x + start.w;
            var bottom = start.y + start.h;
            if (corner.indexOf('w') >= 0) {
              left = clamp(left + dx, 0, right - MIN_CROP);
            } else {
              right = clamp(right + dx, left + MIN_CROP, 1);
            }
            if (corner.indexOf('n') >= 0) {
              top = clamp(top + dy, 0, bottom - MIN_CROP);
            } else {
              bottom = clamp(bottom + dy, top + MIN_CROP, 1);
            }
            crop = { x: left, y: top, w: right - left, h: bottom - top };
          }
          update();
        }
        function onEnd() {
          target.removeEventListener('pointermove', onMove);
          target.removeEventListener('pointerup', onEnd);
          target.removeEventListener('pointercancel', onEnd);
          box.classList.remove('is-dragging');
        }
        target.addEventListener('pointermove', onMove);
        target.addEventListener('pointerup', onEnd);
        target.addEventListener('pointercancel', onEnd);
      }
      box.addEventListener('pointerdown', startDrag);

      box.addEventListener('keydown', function (event) {
        var step = event.shiftKey ? 0.05 : 0.01;
        var moves = {
          ArrowLeft: [-step, 0],
          ArrowRight: [step, 0],
          ArrowUp: [0, -step],
          ArrowDown: [0, step]
        };
        var move = moves[event.key];
        if (!move) {
          return;
        }
        event.preventDefault();
        crop.x = clamp(crop.x + move[0], 0, 1 - crop.w);
        crop.y = clamp(crop.y + move[1], 0, 1 - crop.h);
        update();
      });

      /*
       * The dialog has a fixed size, so the picture is scaled to fit the area
       * left for it (up or down), whatever its own size or shape.
       */
      function fitPicture() {
        if (!picture.naturalWidth) {
          return;
        }
        var style = window.getComputedStyle(area);
        var width = area.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
        var height = area.clientHeight - parseFloat(style.paddingTop) - parseFloat(style.paddingBottom);
        var fit = Math.min(width / picture.naturalWidth, height / picture.naturalHeight);
        if (fit > 0) {
          picture.style.width = Math.floor(picture.naturalWidth * fit) + 'px';
          picture.style.height = Math.floor(picture.naturalHeight * fit) + 'px';
        }
      }
      var resizeWatcher = window.ResizeObserver ? new ResizeObserver(fitPicture) : null;
      if (resizeWatcher) {
        resizeWatcher.observe(area);
      }

      picture.addEventListener('load', function () {
        loading.hidden = true;
        fitPicture();
        stage.classList.add('is-ready');
        update();
      });
      picture.addEventListener('error', function () {
        loading.textContent = 'Kuvaa ei voitu ladata.';
      });
      picture.src = KasinBlocks.safeSrc(block.src);

      dialog.showModal();
      update();
    });
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
      var adjust = button('Muokkaa kuvan esitystapaa', 'btn btn-ghost block-image-choose block-image-adjust');
      var summary = document.createElement('span');
      summary.className = 'block-image-summary';
      var buttons = document.createElement('div');
      buttons.className = 'block-image-buttons';
      buttons.append(choose, adjust, summary);
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
        var picture = KasinBlocks.imageElement({
          src: block.src,
          alt: '',
          crop: block.crop,
          width: block.width,
          height: block.height
        });
        if (picture) {
          preview.replaceChildren(picture);
        } else {
          var empty = document.createElement('span');
          empty.textContent = 'Ei kuvaa';
          preview.replaceChildren(empty);
        }
        preview.classList.toggle('is-cropped', !!(picture && block.crop));
        choose.textContent = picture ? 'Vaihda kuva' : 'Valitse kuva';
        adjust.hidden = !picture;
        var notes = [];
        if (block.crop) {
          notes.push('rajattu');
        }
        if (block.scale < 100) {
          notes.push(block.scale + ' %');
        }
        summary.textContent = notes.join(' · ');
      }
      function changed() {
        showPreview();
        body.dispatchEvent(new Event('input', { bubbles: true }));
      }
      choose.addEventListener('click', function () {
        KasinGallery.pick().then(function (image) {
          if (image) {
            block.src = image.url;
            block.crop = null;  // A new picture starts uncropped.
            block.width = 0;
            block.height = 0;
            changed();
            alt.focus();
          }
        });
      });
      adjust.addEventListener('click', function () {
        editDisplay(block).then(function (done) {
          if (done) {
            changed();
          }
          adjust.focus();
        });
      });
      alt.addEventListener('input', function () {
        block.alt = alt.value;
      });
      caption.addEventListener('input', function () {
        block.caption = caption.value;
      });
      showPreview();
      fields.append(buttons, alt, caption);
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
