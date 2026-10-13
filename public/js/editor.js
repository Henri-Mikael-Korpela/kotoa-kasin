// Block editor for recipe descriptions, in the spirit of WordPress Gutenberg.
//
// The description is a list of blocks (see blocks.js for the stored format).
// Blocks are added from the bar at the end of the list, dragged by their handle
// (mouse or touch) or moved with the arrow buttons, and removed with the cross.
// A layout block puts two blocks side by side; its columns hold a paragraph,
// title or image each.
//
// The blocks are never edited in place: every change produces a new block (or
// list) that replaces the old one.
//
// Like a native form field, the editor fires a bubbling "input" event on its
// container whenever its content changes, so a live preview can follow it.

const KasinEditor = (function () {
    const LABELS = {
        paragraph: 'Kappale',
        title: 'Otsikko',
        image: 'Kuva',
        layout: 'Asettelu',
    };
    const COLUMN_TYPES = ['paragraph', 'title', 'image'];
    const BLOCK_TYPES = ['paragraph', 'title', 'image', 'layout'];
    const MIN_CROP = 0.05;  // Smallest crop side while dragging, as a fraction of the picture.
    const FULL_CROP = { x: 0, y: 0, w: 1, h: 1 };

    // Small helpers.

    function button(text, className, label) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = className;
        b.textContent = text;
        if (label) {
            b.setAttribute('aria-label', label);
            b.title = label;
        }
        return b;
    }

    function clamp(value, low, high) {
        return Math.min(high, Math.max(low, value));
    }

    // The list with the item at index replaced, as a new list.
    function replaceAt(items, index, item) {
        const copy = [...items];
        copy[index] = item;
        return copy;
    }

    // The list with one item moved from one position to another, as a new list.
    function withMoved(items, from, to) {
        const copy = [...items];
        const [item] = copy.splice(from, 1);
        copy.splice(to, 0, item);
        return copy;
    }

    // The list without the item at index, as a new list.
    function withoutIndex(items, index) {
        const copy = [];
        for (let i = 0; i < items.length; i++) {
            if (i !== index) {
                copy.push(items[i]);
            }
        }
        return copy;
    }

    // The children of an element that are blocks, except one.
    function blockItemsExcept(list, except) {
        const items = [];
        for (const child of list.children) {
            if (child !== except && child.classList.contains('block-item')) {
                items.push(child);
            }
        }
        return items;
    }

    // The nearest element that scrolls the given one, or null for the window.
    function scrollingAncestor(element) {
        for (let node = element.parentElement; node && node !== document.body; node = node.parentElement) {
            const overflow = window.getComputedStyle(node).overflowY;
            if ((overflow === 'auto' || overflow === 'scroll') && node.scrollHeight > node.clientHeight) {
                return node;
            }
        }
        return null;
    }

    function scrollTop(scroller) {
        return scroller ? scroller.scrollTop : window.scrollY;
    }

    function scrollBy(scroller, amount) {
        if (scroller) {
            scroller.scrollTop += amount;
        } else {
            window.scrollBy(0, amount);
        }
    }

    function autosize(textarea) {
        textarea.style.height = 'auto';
        textarea.style.height = textarea.scrollHeight + 2 + 'px';
    }

    /*
     * Image display editor. Shows the picture with a crop frame: drag a corner
     * to crop, drag inside the frame to choose which part is centred (the arrow
     * keys move it too), and set the size the image takes on the page. The file
     * itself is never changed.
     *
     * Resolves with the new display settings ({crop, width, height, scale}), or
     * null when cancelled.
     */
    function editDisplay(block) {
        return new Promise(function (resolve) {
            const dialog = document.createElement('dialog');
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

            const area = dialog.querySelector('.crop-area');
            const stage = dialog.querySelector('.crop-stage');
            const picture = dialog.querySelector('.crop-image');
            const box = dialog.querySelector('.crop-box');
            const loading = dialog.querySelector('.crop-loading');
            const scaleInput = dialog.querySelector('.crop-scale');
            const scaleValue = dialog.querySelector('.crop-scale-value');
            const dimensions = dialog.querySelector('.crop-dimensions');
            const result = dialog.querySelector('.crop-result');
            let crop = block.crop || FULL_CROP;
            let scale = block.scale;
            let finished = false;

            /*
             * The dialog has a fixed size, so the picture is scaled to fit the area
             * left for it (up or down), whatever its own size or shape.
             */
            function fitPicture() {
                if (!picture.naturalWidth) {
                    return;
                }
                const style = window.getComputedStyle(area);
                const width = area.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
                const height = area.clientHeight - parseFloat(style.paddingTop) - parseFloat(style.paddingBottom);
                const fit = Math.min(width / picture.naturalWidth, height / picture.naturalHeight);
                if (fit > 0) {
                    picture.style.width = Math.floor(picture.naturalWidth * fit) + 'px';
                    picture.style.height = Math.floor(picture.naturalHeight * fit) + 'px';
                }
            }
            const resizeWatcher = window.ResizeObserver ? new ResizeObserver(fitPicture) : null;
            if (resizeWatcher) {
                resizeWatcher.observe(area);
            }

            function finish(save) {
                if (finished) {
                    return;
                }
                finished = true;
                if (resizeWatcher) {
                    resizeWatcher.disconnect();
                }
                let settings = null;
                if (save && picture.naturalWidth) {
                    settings = {
                        crop: KasinBlocks.cropOption(KasinBlocks.roundCrop(crop)),
                        width: picture.naturalWidth,
                        height: picture.naturalHeight,
                        scale,
                    };
                }
                if (dialog.open) {
                    dialog.close();
                }
                dialog.remove();
                resolve(settings);
            }

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
                const figure = document.createElement('figure');
                figure.className = 'block-image';
                figure.style.width = scale + '%';
                const shown = KasinBlocks.imageElement({
                    src: block.src,
                    alt: '',
                    crop: KasinBlocks.cropOption(crop),
                    width: picture.naturalWidth,
                    height: picture.naturalHeight,
                });
                if (shown) {
                    figure.appendChild(shown);
                }
                result.replaceChildren(figure);
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
                crop = FULL_CROP;
                scale = 100;
                update();
            });
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
                const corner = event.target.dataset ? event.target.dataset.corner : null;
                const pictureBox = picture.getBoundingClientRect();
                const start = crop;
                const startX = event.clientX;
                const startY = event.clientY;
                const target = event.currentTarget;
                try {
                    target.setPointerCapture(event.pointerId);
                } catch (e) {
                    // Without capture the drag still follows the pointer over the frame.
                }
                box.classList.add('is-dragging');

                function onMove(moveEvent) {
                    const dx = (moveEvent.clientX - startX) / pictureBox.width;
                    const dy = (moveEvent.clientY - startY) / pictureBox.height;
                    if (!corner) {
                        crop = {
                            x: clamp(start.x + dx, 0, 1 - start.w),
                            y: clamp(start.y + dy, 0, 1 - start.h),
                            w: start.w,
                            h: start.h,
                        };
                    } else {
                        let left = start.x;
                        let top = start.y;
                        let right = start.x + start.w;
                        let bottom = start.y + start.h;
                        if (corner.includes('w')) {
                            left = clamp(left + dx, 0, right - MIN_CROP);
                        } else {
                            right = clamp(right + dx, left + MIN_CROP, 1);
                        }
                        if (corner.includes('n')) {
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
                const step = event.shiftKey ? 0.05 : 0.01;
                let dx = 0;
                let dy = 0;
                if (event.key === 'ArrowLeft') {
                    dx = -step;
                } else if (event.key === 'ArrowRight') {
                    dx = step;
                } else if (event.key === 'ArrowUp') {
                    dy = -step;
                } else if (event.key === 'ArrowDown') {
                    dy = step;
                } else {
                    return;
                }
                event.preventDefault();
                crop = {
                    x: clamp(crop.x + dx, 0, 1 - crop.w),
                    y: clamp(crop.y + dy, 0, 1 - crop.h),
                    w: crop.w,
                    h: crop.h,
                };
                update();
            });

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

    // The editing controls of a paragraph block. commit(next) is given each new
    // version of the block.
    function createParagraphBody(initial, commit) {
        let block = initial;
        const body = document.createElement('div');
        body.className = 'block-body block-body-paragraph';
        const text = document.createElement('textarea');
        text.className = 'block-text';
        text.rows = 3;
        text.placeholder = 'Kirjoita kappale…';
        text.value = block.text;
        text.setAttribute('aria-label', 'Kappaleen teksti');
        text.addEventListener('input', function () {
            block = { ...block, text: text.value };
            commit(block);
            autosize(text);
        });
        body.appendChild(text);
        window.requestAnimationFrame(function () {
            autosize(text);
        });
        return body;
    }

    function createTitleBody(initial, commit) {
        let block = initial;
        const body = document.createElement('div');
        body.className = 'block-body block-body-title';
        const level = document.createElement('select');
        level.className = 'block-title-level';
        level.setAttribute('aria-label', 'Otsikon taso');
        for (const value of KasinBlocks.TITLE_LEVELS) {
            const option = document.createElement('option');
            option.value = String(value);
            option.textContent = 'H' + value;
            level.appendChild(option);
        }
        level.value = String(block.level);
        const heading = document.createElement('input');
        heading.type = 'text';
        heading.className = 'block-title-text block-title-text-' + block.level;
        heading.placeholder = 'Otsikko';
        heading.value = block.text;
        heading.setAttribute('aria-label', 'Otsikon teksti');
        level.addEventListener('change', function () {
            block = { ...block, level: Number(level.value) };
            commit(block);
            heading.className = 'block-title-text block-title-text-' + block.level;
        });
        heading.addEventListener('input', function () {
            block = { ...block, text: heading.value };
            commit(block);
        });
        body.append(level, heading);
        return body;
    }

    function createImageBody(initial, commit) {
        let block = initial;
        const body = document.createElement('div');
        body.className = 'block-body block-body-image';
        const preview = document.createElement('div');
        preview.className = 'block-image-preview';
        const choose = button(block.src ? 'Vaihda kuva' : 'Valitse kuva', 'btn btn-ghost block-image-choose');
        const adjust = button('Muokkaa kuvan esitystapaa', 'btn btn-ghost block-image-choose block-image-adjust');
        const summary = document.createElement('span');
        summary.className = 'block-image-summary';
        const buttons = document.createElement('div');
        buttons.className = 'block-image-buttons';
        buttons.append(choose, adjust, summary);
        const fields = document.createElement('div');
        fields.className = 'block-image-fields';
        const alt = document.createElement('input');
        alt.type = 'text';
        alt.placeholder = 'Kuvan kuvaus näkövammaisille (alt-teksti)';
        alt.maxLength = 300;
        alt.value = block.alt;
        alt.setAttribute('aria-label', 'Kuvan kuvaus');
        const caption = document.createElement('input');
        caption.type = 'text';
        caption.placeholder = 'Kuvateksti (valinnainen)';
        caption.maxLength = 300;
        caption.value = block.caption;
        caption.setAttribute('aria-label', 'Kuvateksti');

        function showPreview() {
            const picture = KasinBlocks.imageElement({
                src: block.src,
                alt: '',
                crop: block.crop,
                width: block.width,
                height: block.height,
            });
            if (picture) {
                preview.replaceChildren(picture);
            } else {
                const empty = document.createElement('span');
                empty.textContent = 'Ei kuvaa';
                preview.replaceChildren(empty);
            }
            preview.classList.toggle('is-cropped', !!(picture && block.crop));
            choose.textContent = picture ? 'Vaihda kuva' : 'Valitse kuva';
            adjust.hidden = !picture;
            const notes = [];
            if (block.crop) {
                notes.push('rajattu');
            }
            if (block.scale < 100) {
                notes.push(block.scale + ' %');
            }
            summary.textContent = notes.join(' · ');
        }

        // Takes the changes into the block, shows them and lets the page know.
        function change(changes) {
            block = { ...block, ...changes };
            commit(block);
            showPreview();
            body.dispatchEvent(new Event('input', { bubbles: true }));
        }

        choose.addEventListener('click', async function () {
            const image = await KasinGallery.pick();
            if (image) {
                // A new picture starts uncropped.
                change({ src: image.url, crop: null, width: 0, height: 0 });
                alt.focus();
            }
        });
        adjust.addEventListener('click', async function () {
            const display = await editDisplay(block);
            if (display) {
                change(display);
            }
            adjust.focus();
        });
        alt.addEventListener('input', function () {
            block = { ...block, alt: alt.value };
            commit(block);
        });
        caption.addEventListener('input', function () {
            block = { ...block, caption: caption.value };
            commit(block);
        });
        showPreview();
        fields.append(buttons, alt, caption);
        body.append(preview, fields);
        return body;
    }

    function createBody(block, commit) {
        if (block.type === 'title') {
            return createTitleBody(block, commit);
        }
        if (block.type === 'image') {
            return createImageBody(block, commit);
        }
        return createParagraphBody(block, commit);
    }

    function create(container, initialText) {
        let blocks = KasinBlocks.parse(initialText);
        const list = document.createElement('div');
        list.className = 'block-list';
        const inserter = document.createElement('div');
        inserter.className = 'block-inserter';
        const status = document.createElement('p');
        status.className = 'visually-hidden';
        status.setAttribute('aria-live', 'polite');

        const label = document.createElement('span');
        label.className = 'block-inserter-label';
        label.textContent = 'Lisää lohko:';
        inserter.appendChild(label);
        for (const type of BLOCK_TYPES) {
            const add = button('+ ' + LABELS[type], 'block-add');
            add.addEventListener('click', function () {
                blocks = [...blocks, KasinBlocks.create(type)];
                render();
                focusBlock(blocks.length - 1);
            });
            inserter.appendChild(add);
        }

        container.replaceChildren(list, inserter, status);

        function focusBlock(index) {
            const item = list.children[index];
            const field = item && item.querySelector('textarea, input, button.block-image-choose, .layout-column button');
            if (field) {
                field.focus();
            }
        }

        function move(from, to) {
            if (to < 0 || to >= blocks.length || from === to) {
                return;
            }
            const block = blocks[from];
            blocks = withMoved(blocks, from, to);
            render();
            status.textContent = LABELS[block.type] + ' siirretty kohtaan ' + (to + 1) + '.';
        }

        function remove(index) {
            const block = blocks[index];
            blocks = withoutIndex(blocks, index);
            render();
            status.textContent = LABELS[block.type] + ' poistettu.';
            focusBlock(Math.min(index, blocks.length - 1));
        }

        // Typing does not redraw the editor: the new block just takes the old one's place.
        function commitAt(index) {
            return function (next) {
                blocks = replaceAt(blocks, index, next);
            };
        }

        function commitColumn(index, side) {
            return function (next) {
                const layout = blocks[index];
                blocks = replaceAt(blocks, index, { ...layout, columns: replaceAt(layout.columns, side, next) });
            };
        }

        // Replaces one column of the layout at index and redraws.
        function setColumn(index, side, column) {
            const layout = blocks[index];
            blocks = replaceAt(blocks, index, { ...layout, columns: replaceAt(layout.columns, side, column) });
            render();
        }

        function createLayoutBody(block, index) {
            const body = document.createElement('div');
            body.className = 'block-body block-body-layout';
            for (let side = 0; side < block.columns.length; side++) {
                const column = block.columns[side];
                const cell = document.createElement('div');
                cell.className = 'layout-column';
                const heading = document.createElement('div');
                heading.className = 'layout-column-header';
                const name = document.createElement('span');
                name.textContent = (side === 0 ? 'Vasen' : 'Oikea') + (column ? ': ' + LABELS[column.type].toLowerCase() : '');
                heading.appendChild(name);

                if (column) {
                    const clear = button('✕', 'block-icon', 'Tyhjennä ' + (side === 0 ? 'vasen' : 'oikea') + ' sarake');
                    clear.addEventListener('click', function () {
                        setColumn(index, side, null);
                    });
                    heading.appendChild(clear);
                    cell.append(heading, createBody(column, commitColumn(index, side)));
                } else {
                    const choices = document.createElement('div');
                    choices.className = 'layout-column-choices';
                    for (const type of COLUMN_TYPES) {
                        const add = button('+ ' + LABELS[type], 'block-add');
                        add.addEventListener('click', function () {
                            setColumn(index, side, KasinBlocks.create(type));
                        });
                        choices.appendChild(add);
                    }
                    cell.append(heading, choices);
                }
                body.appendChild(cell);
            }
            return body;
        }

        function createItem(block, index) {
            const item = document.createElement('div');
            item.className = 'block-item block-item-' + block.type;

            const toolbar = document.createElement('div');
            toolbar.className = 'block-toolbar';
            const handle = button('⠿', 'block-handle', 'Siirrä vetämällä: ' + LABELS[block.type]);
            const name = document.createElement('span');
            name.className = 'block-name';
            name.textContent = LABELS[block.type];
            const up = button('↑', 'block-icon', 'Siirrä ylös');
            const down = button('↓', 'block-icon', 'Siirrä alas');
            const close = button('✕', 'block-icon block-remove', 'Poista ' + LABELS[block.type].toLowerCase());
            up.disabled = index === 0;
            down.disabled = index === blocks.length - 1;
            up.addEventListener('click', function () {
                move(index, index - 1);
                list.children[index - 1].querySelector('.block-icon').focus();
            });
            down.addEventListener('click', function () {
                move(index, index + 1);
                const moved = list.children[index + 1];
                const target = moved.querySelectorAll('.block-icon')[1];
                (target.disabled ? moved.querySelector('.block-icon') : target).focus();
            });
            close.addEventListener('click', function () {
                remove(index);
            });
            toolbar.append(handle, name);
            if (block.type === 'layout') {
                const swap = button('⇄', 'block-icon', 'Vaihda sarakkeiden paikkaa');
                swap.addEventListener('click', function () {
                    const layout = blocks[index];
                    blocks = replaceAt(blocks, index, { ...layout, columns: [layout.columns[1], layout.columns[0]] });
                    render();
                });
                toolbar.appendChild(swap);
            }
            toolbar.append(up, down, close);

            const body = block.type === 'layout' ? createLayoutBody(block, index) : createBody(block, commitAt(index));
            item.append(toolbar, body);
            handle.addEventListener('pointerdown', function (event) {
                startDrag(event, item, index);
            });
            return item;
        }

        function render() {
            const nodes = [];
            for (let index = 0; index < blocks.length; index++) {
                nodes.push(createItem(blocks[index], index));
            }
            if (nodes.length === 0) {
                const empty = document.createElement('p');
                empty.className = 'block-empty';
                empty.textContent = 'Kuvaus on tyhjä. Lisää ensimmäinen lohko alta.';
                nodes.push(empty);
            }
            list.replaceChildren(...nodes);
            // Blocks were added, moved or removed.
            container.dispatchEvent(new Event('input', { bubbles: true }));
        }

        // Dragging: the block follows the pointer and a line shows where it lands.
        function startDrag(event, item, from) {
            if (event.button !== 0) {
                return;
            }
            event.preventDefault();
            const handle = event.currentTarget;
            try {
                handle.setPointerCapture(event.pointerId);  // Keeps the drag going outside the handle.
            } catch (e) {
                // Not every pointer can be captured; the drag still works over the handle.
            }
            const startY = event.clientY;
            const scroller = scrollingAncestor(list);
            const startScroll = scrollTop(scroller);
            let target = from;
            const marker = document.createElement('div');
            marker.className = 'block-drop-marker';
            item.classList.add('is-dragging');
            document.body.classList.add('is-dragging-block');

            // Where the block would land if dropped at this height.
            function targetFor(y) {
                let index = 0;
                for (const child of blockItemsExcept(list, item)) {
                    const childBox = child.getBoundingClientRect();
                    if (y > childBox.top + childBox.height / 2) {
                        index++;
                    }
                }
                return index;
            }

            function placeMarker(index) {
                const others = blockItemsExcept(list, item);
                if (index < others.length) {
                    list.insertBefore(marker, others[index]);
                } else {
                    list.appendChild(marker);
                }
            }

            function onMove(moveEvent) {
                // Scroll when dragging close to the top or bottom of what scrolls.
                const view = scroller ? scroller.getBoundingClientRect() : { top: 0, bottom: window.innerHeight };
                if (moveEvent.clientY < view.top + 60) {
                    scrollBy(scroller, -12);
                } else if (moveEvent.clientY > view.bottom - 60) {
                    scrollBy(scroller, 12);
                }
                const offset = moveEvent.clientY - startY + (scrollTop(scroller) - startScroll);
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
            },
        };
    }

    return {
        create,
    };
})();
