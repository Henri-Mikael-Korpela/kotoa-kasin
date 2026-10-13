// Image gallery for the admin pages: uploading, the picker dialog used by the
// recipe editor, and the gallery page (/hallinta/kuvat).
//
// Images are scaled down in the browser before upload: at most 1600 pixels on
// the longer side, saved as JPEG. That keeps uploads small and drops the
// metadata phones put in photos, such as where they were taken.

const KasinGallery = (function () {
    const MAX_SIDE = 1600;
    const JPEG_QUALITY = 0.85;
    const LONG_PRESS_MS = 500;
    const DOUBLE_TAP_MS = 350;

    // Small helpers.

    function displayName(image) {
        try {
            return decodeURIComponent(image.original_name || '') || 'Nimetön kuva';
        } catch (e) {
            return image.original_name || 'Nimetön kuva';
        }
    }

    function formatSize(bytes) {
        return bytes >= 1024 * 1024
            ? (bytes / (1024 * 1024)).toFixed(1).replace('.', ',') + ' Mt'
            : Math.max(1, Math.round(bytes / 1024)) + ' kt';
    }

    function extensionOf(image) {
        const match = /\.([a-z0-9]+)$/i.exec(image.url);
        return match ? match[1].toUpperCase() : '';
    }

    function formatDate(iso) {
        const date = new Date(iso);
        return isNaN(date.getTime()) ? '' : date.toLocaleDateString('fi-FI', {
            day: 'numeric',
            month: 'numeric',
            year: 'numeric',
        });
    }

    function findById(images, id) {
        for (const image of images) {
            if (image.id === id) {
                return image;
            }
        }
        return null;
    }

    function totalSize(images) {
        let total = 0;
        for (const image of images) {
            total += image.size;
        }
        return total;
    }

    // Uploading.

    async function loadBitmap(file) {
        if (window.createImageBitmap) {
            try {
                return await createImageBitmap(file, { imageOrientation: 'from-image' });
            } catch (error) {
                return createImageBitmap(file);
            }
        }
        return new Promise(function (resolve, reject) {
            const url = URL.createObjectURL(file);
            const img = new Image();
            img.onload = function () {
                URL.revokeObjectURL(url);
                resolve(img);
            };
            img.onerror = function () {
                URL.revokeObjectURL(url);
                reject(new Error('not an image'));
            };
            img.src = url;
        });
    }

    // Scales an image file down and re-encodes it as JPEG.
    async function prepareImage(file) {
        const bitmap = await loadBitmap(file);
        const scale = Math.min(1, MAX_SIDE / Math.max(bitmap.width, bitmap.height));
        const canvas = document.createElement('canvas');
        canvas.width = Math.max(1, Math.round(bitmap.width * scale));
        canvas.height = Math.max(1, Math.round(bitmap.height * scale));
        const context = canvas.getContext('2d');
        context.fillStyle = '#ffffff';  // Transparent areas become white in a JPEG.
        context.fillRect(0, 0, canvas.width, canvas.height);
        context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
        return new Promise(function (resolve, reject) {
            canvas.toBlob(function (blob) {
                if (blob) {
                    resolve(blob);
                } else {
                    reject(new Error('encoding failed'));
                }
            }, 'image/jpeg', JPEG_QUALITY);
        });
    }

    // Uploads one file; resolves with {id, url}.
    async function upload(file) {
        let blob;
        try {
            blob = await prepareImage(file);
        } catch (error) {
            throw apiError(415, 'Tiedostoa "' + file.name + '" ei voi lukea kuvana.');
        }
        const name = file.name.replace(/\.[^.]*$/, '') + '.jpg';
        return apiFetch('/api/images', {
            method: 'POST',
            headers: {
                'Accept': 'application/json',
                'Authorization': 'Bearer ' + (getToken() || ''),
                'Content-Type': 'image/jpeg',
                'X-File-Name': encodeURIComponent(name),
            },
            body: blob,
        });
    }

    function uploadErrorMessage(error, file) {
        if (error.status === 413) {
            return '"' + file.name + '" on liian suuri.';
        }
        if (error.status === 415) {
            return error.message.includes('"') ? error.message : '"' + file.name + '" ei ole kuva.';
        }
        if (error.status === 503) {
            return 'Palvelin on kiireinen, "' + file.name + '" jäi lataamatta. Yritä uudelleen.';
        }
        return messageFor(error, '"' + file.name + '" jäi lataamatta.');
    }

    // Uploads files one after another, reporting progress. Throws when signed out.
    async function uploadAll(files, onProgress) {
        const uploaded = [];
        const failures = [];
        for (let index = 0; index < files.length; index++) {
            onProgress(index + 1, files.length);
            try {
                uploaded.push(await upload(files[index]));
            } catch (error) {
                if (error.status === 401) {
                    throw error;
                }
                failures.push(uploadErrorMessage(error, files[index]));
            }
        }
        return { uploaded, failures };
    }

    // The details panel.

    function infoRow(label, value) {
        const row = document.createElement('div');
        const term = document.createElement('dt');
        term.textContent = label;
        const detail = document.createElement('dd');
        detail.textContent = value;
        row.append(term, detail);
        return { row, value: detail };
    }

    /*
     * The details of one image: a preview, its facts and the caller's buttons.
     * With no image the same layout is shown empty (a hint in the preview,
     * dashes for the facts, the buttons disabled), so nothing moves when an
     * image is chosen. extra, if given, goes below the buttons.
     */
    function renderInfo(container, image, actions, hint, extra) {
        const preview = document.createElement('div');
        preview.className = 'gallery-info-preview';
        let img = null;
        if (image) {
            img = document.createElement('img');
            img.src = image.url;
            img.alt = '';
            preview.appendChild(img);
        } else {
            const message = document.createElement('p');
            message.className = 'gallery-info-hint';
            message.textContent = hint;
            preview.appendChild(message);
        }

        const list = document.createElement('dl');
        list.className = 'gallery-info-list';
        let usedText = '';
        if (image) {
            usedText = image.used > 0 ? image.used + ' reseptissä' : 'Ei käytössä';
        }
        const facts = [
            ['Nimi', image ? displayName(image) : ''],
            ['Tiedostotyyppi', image ? extensionOf(image) : ''],
            ['Koko', image ? formatSize(image.size) : ''],
            ['Mitat', image ? '…' : ''],
            ['Ladattu', image ? formatDate(image.created_at) : ''],
            ['Käytössä', usedText],
        ];
        let dimensions = null;
        for (const [label, value] of facts) {
            const row = infoRow(label, value || '–');
            row.value.title = value;
            row.value.classList.toggle('is-empty', !value);
            if (label === 'Mitat') {
                dimensions = row.value;
            }
            list.appendChild(row.row);
        }
        if (img) {
            const showDimensions = function () {
                dimensions.textContent = img.naturalWidth + ' × ' + img.naturalHeight + ' px';
            };
            if (img.complete && img.naturalWidth) {
                showDimensions();
            } else {
                img.addEventListener('load', showDimensions);
            }
        }

        const buttons = document.createElement('div');
        buttons.className = 'gallery-info-actions';
        buttons.append(...actions);
        container.replaceChildren(preview, list, buttons);
        if (extra) {
            container.appendChild(extra);
        }
    }

    // The delete button of a details panel; disabled when there is nothing to delete.
    function deleteButton(image, onClick) {
        const remove = createCategoryButton('Poista kuva', 'is-danger');
        if (!image) {
            remove.disabled = true;
        } else if (image.used > 0) {
            remove.disabled = true;
            remove.title = 'Kuva on käytössä reseptissä, joten sitä ei voi poistaa.';
        } else {
            remove.addEventListener('click', onClick);
        }
        return remove;
    }

    function useButton(image, onClick) {
        const use = document.createElement('button');
        use.type = 'button';
        use.className = 'btn btn-primary';
        use.textContent = 'Käytä kuvaa';
        if (image) {
            use.addEventListener('click', onClick);
        } else {
            use.disabled = true;
        }
        return use;
    }

    /*
     * Picker dialog: resolves with the chosen image ({id, url, ...}) or null.
     *
     * A click (or tap) shows an image's details in the info panel, which is
     * always visible on the right on wide screens. A double click (or double tap)
     * chooses the image. On a device with a mouse, an X in the corner of a
     * hovered image deletes it; on touch screens, holding an image opens a menu
     * with Use, Details and Delete.
     */
    function pick() {
        return new Promise(function (resolve) {
            const dialog = document.createElement('dialog');
            dialog.className = 'gallery-dialog';
            dialog.innerHTML =
                '<div class="gallery-dialog-header">' +
                '  <h2>Valitse kuva</h2>' +
                '  <button type="button" class="category-button" data-close>Sulje</button>' +
                '</div>' +
                '<div class="gallery-dialog-actions">' +
                '  <label class="btn btn-primary gallery-upload-button">Lataa uusi kuva' +
                '    <input type="file" accept="image/*" multiple hidden>' +
                '  </label>' +
                '  <span class="gallery-status" aria-live="polite"></span>' +
                '</div>' +
                '<div class="gallery-dialog-body">' +
                '  <div class="gallery-grid gallery-dialog-grid" role="listbox" aria-label="Kuvat"></div>' +
                '  <aside class="gallery-info" aria-live="polite">' +
                '    <div class="gallery-info-header">' +
                '      <h3>Kuvan tiedot</h3>' +
                '      <button type="button" class="block-icon gallery-info-close" aria-label="Sulje tiedot">✕</button>' +
                '    </div>' +
                '    <div class="gallery-info-body"></div>' +
                '  </aside>' +
                '</div>' +
                '<div class="gallery-menu" role="menu" hidden></div>';
            document.body.appendChild(dialog);

            const grid = dialog.querySelector('.gallery-grid');
            const status = dialog.querySelector('.gallery-status');
            const input = dialog.querySelector('input[type="file"]');
            const info = dialog.querySelector('.gallery-info');
            const infoBody = dialog.querySelector('.gallery-info-body');
            const menu = dialog.querySelector('.gallery-menu');
            let images = [];
            let selectedId = null;
            let finished = false;

            // Resolves once, whether a picture, the close button or Esc ends the dialog.
            function finish(image) {
                if (finished) {
                    return;
                }
                finished = true;
                if (dialog.open) {
                    dialog.close();
                }
                dialog.remove();
                resolve(image);
            }
            dialog.addEventListener('close', function () {
                finish(null);  // Esc closes the dialog natively.
            });
            dialog.querySelector('[data-close]').addEventListener('click', function () {
                finish(null);
            });
            dialog.addEventListener('click', function (event) {
                if (event.target === dialog) {
                    finish(null);  // A click on the backdrop.
                }
            });
            dialog.querySelector('.gallery-info-close').addEventListener('click', function () {
                info.classList.remove('is-open');
            });

            function showEmptyInfo() {
                renderInfo(infoBody, null, [useButton(null), deleteButton(null)], images.length > 0
                    ? 'Napsauta kuvaa nähdäksesi sen tiedot. Kaksoisnapsautus valitsee kuvan.'
                    : 'Lataa kuva painikkeesta yllä.');
            }

            function showInfo(image) {
                const use = useButton(image, function () {
                    finish(image);
                });
                const remove = deleteButton(image, function () {
                    deleteImage(image);
                });
                renderInfo(infoBody, image, [use, remove]);
            }

            function select(image, openPanel) {
                selectedId = image.id;
                for (const thumb of grid.querySelectorAll('.gallery-thumb')) {
                    thumb.setAttribute('aria-selected', String(Number(thumb.dataset.id) === image.id));
                }
                showInfo(image);
                if (openPanel) {
                    info.classList.add('is-open');  // On narrow screens the panel slides in.
                }
            }

            async function deleteImage(image) {
                closeMenu();
                if (image.used > 0) {
                    status.textContent = 'Kuva "' + displayName(image) + '" on käytössä reseptissä, joten sitä ei voi poistaa.';
                    return;
                }
                if (!window.confirm('Poistetaanko kuva "' + displayName(image) + '"?\n\nPoistoa ei voi perua.')) {
                    return;
                }
                try {
                    await api('DELETE', '/api/images/' + encodeURIComponent(image.id));
                } catch (error) {
                    if (error.status === 409) {
                        status.textContent = 'Kuva "' + displayName(image) + '" on käytössä reseptissä, joten sitä ei voi poistaa.';
                    } else if (error.status === 404) {
                        load();
                    } else if (error.status !== 401) {
                        status.textContent = messageFor(error, 'Kuvan poistaminen epäonnistui.');
                    }
                    return;
                }
                if (selectedId === image.id) {
                    selectedId = null;
                    info.classList.remove('is-open');
                }
                await load();
                status.textContent = 'Kuva poistettu.';
            }

            function closeMenu() {
                menu.hidden = true;
            }
            menu.addEventListener('click', function (event) {
                if (event.target === menu) {
                    closeMenu();  // A tap outside the options.
                }
            });

            // Touch: holding an image opens this menu.
            function openMenu(image) {
                const title = document.createElement('p');
                title.className = 'gallery-menu-title';
                title.textContent = displayName(image);
                const options = [
                    { text: 'Käytä kuvaa', danger: false, action: function () { finish(image); } },
                    { text: 'Kuvan tiedot', danger: false, action: function () { closeMenu(); select(image, true); } },
                    { text: 'Poista', danger: true, action: function () { deleteImage(image); } },
                    { text: 'Peruuta', danger: false, action: closeMenu },
                ];
                const items = [title];
                for (const option of options) {
                    const item = document.createElement('button');
                    item.type = 'button';
                    item.setAttribute('role', 'menuitem');
                    item.className = 'gallery-menu-item' + (option.danger ? ' is-danger' : '');
                    item.textContent = option.text;
                    if (option.danger && image.used > 0) {
                        item.disabled = true;
                        item.textContent = 'Poista (käytössä reseptissä)';
                    }
                    item.addEventListener('click', option.action);
                    items.push(item);
                }
                menu.replaceChildren(...items);
                menu.hidden = false;
                items[1].focus();
            }

            function createTile(image) {
                const tile = document.createElement('div');
                tile.className = 'gallery-tile';

                const thumb = document.createElement('button');
                thumb.type = 'button';
                thumb.className = 'gallery-thumb';
                thumb.dataset.id = String(image.id);
                thumb.setAttribute('role', 'option');
                thumb.setAttribute('aria-selected', String(image.id === selectedId));
                thumb.setAttribute('aria-label', displayName(image) + '. Kaksoisnapsautus valitsee.');
                const img = document.createElement('img');
                img.src = image.url;
                img.alt = '';
                img.loading = 'lazy';
                img.draggable = false;
                thumb.appendChild(img);

                const remove = document.createElement('button');
                remove.type = 'button';
                remove.className = 'gallery-thumb-delete';
                remove.textContent = '✕';
                remove.tabIndex = -1;  // Keyboard users delete from the info panel or with Delete.
                remove.setAttribute('aria-label', 'Poista ' + displayName(image));
                remove.title = image.used > 0 ? 'Käytössä reseptissä, ei voi poistaa' : 'Poista kuva';
                if (image.used > 0) {
                    remove.classList.add('is-disabled');
                }
                remove.addEventListener('click', function (event) {
                    event.stopPropagation();
                    deleteImage(image);
                });

                // Touch state: tap shows details, double tap chooses, holding opens the menu.
                let pressTimer = null;
                let lastTap = 0;
                let startX = 0;
                let startY = 0;
                let suppressClick = false;

                // Mouse: click shows details, double click chooses.
                thumb.addEventListener('click', function () {
                    if (suppressClick) {
                        suppressClick = false;
                        return;
                    }
                    select(image, false);
                });
                thumb.addEventListener('dblclick', function () {
                    finish(image);
                });
                thumb.addEventListener('keydown', function (event) {
                    if (event.key === 'Delete') {
                        deleteImage(image);
                    }
                });

                thumb.addEventListener('pointerdown', function (event) {
                    if (event.pointerType !== 'touch') {
                        return;
                    }
                    startX = event.clientX;
                    startY = event.clientY;
                    pressTimer = window.setTimeout(function () {
                        pressTimer = null;
                        suppressClick = true;
                        openMenu(image);
                    }, LONG_PRESS_MS);
                });
                thumb.addEventListener('pointermove', function (event) {
                    if (pressTimer && Math.abs(event.clientX - startX) + Math.abs(event.clientY - startY) > 12) {
                        window.clearTimeout(pressTimer);  // Scrolling, not holding.
                        pressTimer = null;
                    }
                });
                thumb.addEventListener('pointerup', function (event) {
                    if (event.pointerType !== 'touch' || !pressTimer) {
                        return;
                    }
                    window.clearTimeout(pressTimer);
                    pressTimer = null;
                    const now = Date.now();
                    if (now - lastTap < DOUBLE_TAP_MS) {
                        suppressClick = true;
                        finish(image);
                        return;
                    }
                    lastTap = now;
                });
                thumb.addEventListener('pointercancel', function () {
                    window.clearTimeout(pressTimer);
                    pressTimer = null;
                });
                thumb.addEventListener('contextmenu', function (event) {
                    event.preventDefault();  // The hold menu replaces the browser's own.
                });

                tile.append(thumb, remove);
                return tile;
            }

            async function load() {
                status.textContent = 'Ladataan kuvia…';
                let list;
                try {
                    list = await api('GET', '/api/images');
                } catch (error) {
                    if (error.status !== 401) {
                        status.textContent = messageFor(error, 'Kuvien lataaminen epäonnistui.');
                    }
                    return;
                }
                images = list;
                status.textContent = images.length === 0 ? 'Ei vielä kuvia. Lataa ensimmäinen.' : '';
                const tiles = [];
                for (const image of images) {
                    tiles.push(createTile(image));
                }
                grid.replaceChildren(...tiles);
                const selected = selectedId !== null ? findById(images, selectedId) : null;
                if (selected) {
                    showInfo(selected);
                } else {
                    selectedId = null;
                    showEmptyInfo();
                }
            }

            input.addEventListener('change', async function () {
                const files = input.files;
                if (!files.length) {
                    return;
                }
                let outcome;
                try {
                    outcome = await uploadAll(files, function (index, count) {
                        status.textContent = count > 1 ? 'Ladataan kuvaa ' + index + '/' + count + '…' : 'Ladataan kuvaa…';
                    });
                } catch (error) {
                    return;  // Signed out: goToLogin() is already on its way.
                }
                input.value = '';
                if (outcome.uploaded.length > 0) {
                    selectedId = outcome.uploaded[outcome.uploaded.length - 1].id;  // Show the newest upload.
                }
                await load();
                if (outcome.failures.length > 0) {
                    status.textContent = outcome.failures.join(' ');
                } else {
                    status.textContent = outcome.uploaded.length === 1
                        ? 'Kuva ladattu. Kaksoisnapsauta sitä valitaksesi.'
                        : 'Kuvat ladattu.';
                }
            });

            dialog.showModal();
            showEmptyInfo();
            load();
        });
    }

    // The gallery page.

    // The recipes that show an image, each linking to its edit page.
    function usedInRecipes(image) {
        const section = document.createElement('div');
        section.className = 'gallery-info-recipes';
        const title = document.createElement('p');
        title.className = 'gallery-info-recipes-title';
        title.textContent = 'Käytössä resepteissä';
        const list = document.createElement('ul');
        list.className = 'gallery-info-recipe-list';
        const recipes = image && Array.isArray(image.recipes) ? image.recipes : [];
        if (recipes.length === 0) {
            const none = document.createElement('li');
            none.className = 'is-empty';
            none.textContent = image ? 'Ei yhdessäkään reseptissä.' : '–';
            list.appendChild(none);
        }
        for (const recipe of recipes) {
            const item = document.createElement('li');
            const link = document.createElement('a');
            link.href = '/hallinta/resepti?id=' + encodeURIComponent(recipe.id);
            link.target = '_blank';
            link.textContent = recipe.name;
            link.title = 'Muokkaa reseptiä ' + recipe.name;
            item.appendChild(link);
            list.appendChild(item);
        }
        section.append(title, list);
        return section;
    }

    function createGalleryCard(image, onSelect, onDelete) {
        const card = document.createElement('li');
        card.className = 'gallery-card';
        card.dataset.id = String(image.id);

        const show = document.createElement('button');
        show.type = 'button';
        show.className = 'gallery-card-image';
        show.setAttribute('aria-label', 'Näytä tiedot: ' + displayName(image));
        const img = document.createElement('img');
        img.src = image.url;
        img.alt = '';
        img.loading = 'lazy';
        show.appendChild(img);
        show.addEventListener('click', function () {
            onSelect(image);
        });

        const info = document.createElement('div');
        info.className = 'gallery-card-info';
        const name = document.createElement('p');
        name.className = 'gallery-card-name';
        name.textContent = displayName(image);
        const meta = document.createElement('p');
        meta.className = 'gallery-card-meta';
        meta.textContent = formatSize(image.size) + ' · ' +
            (image.used > 0 ? 'Käytössä ' + image.used + ' reseptissä' : 'Ei käytössä');
        info.append(name, meta);

        const actions = document.createElement('div');
        actions.className = 'gallery-card-actions';
        const remove = createCategoryButton('Poista', 'is-danger');
        if (image.used > 0) {
            remove.disabled = true;
            remove.title = 'Kuva on käytössä reseptissä. Poista se ensin resepteistä.';
        }
        remove.addEventListener('click', function () {
            onDelete(image);
        });
        actions.appendChild(remove);

        card.append(show, info, actions);
        return card;
    }

    function initPage() {
        const grid = document.getElementById('galleryGrid');
        const status = document.getElementById('galleryStatus');
        const errorEl = document.getElementById('galleryError');
        const input = document.getElementById('galleryUpload');
        const dropZone = document.getElementById('galleryDropZone');
        const summary = document.getElementById('gallerySummary');
        const infoBody = document.getElementById('galleryInfo');
        let images = [];
        let selectedId = null;
        setupLogout();

        function showInfo() {
            const image = findById(images, selectedId);
            selectedId = image ? image.id : null;
            for (const card of grid.children) {
                card.classList.toggle('is-selected', Number(card.dataset.id) === selectedId);
            }

            const open = document.createElement('a');
            open.className = 'btn btn-ghost';
            open.textContent = 'Avaa täysikokoisena';
            open.target = '_blank';
            open.rel = 'noopener';
            if (image) {
                open.href = image.url;
            } else {
                open.setAttribute('aria-disabled', 'true');
                open.classList.add('is-disabled');
            }
            const remove = deleteButton(image, function () {
                deleteImage(image);
            });
            renderInfo(infoBody, image, [open, remove], images.length > 0
                ? 'Napsauta kuvaa nähdäksesi sen tiedot.'
                : 'Ei vielä kuvia.', usedInRecipes(image));
        }

        function select(image) {
            selectedId = image.id;
            showInfo();
            // On narrow screens the panel is above the images; bring it into view.
            const box = infoBody.getBoundingClientRect();
            if (box.bottom < 0 || box.top > window.innerHeight) {
                infoBody.parentNode.scrollIntoView({ behavior: 'smooth', block: 'start' });
            }
        }

        function showSummary() {
            const total = totalSize(images);
            const count = document.createElement('strong');
            count.textContent = images.length + (images.length === 1 ? ' kuva' : ' kuvaa');
            const size = document.createElement('strong');
            size.textContent = formatSize(total);
            summary.replaceChildren(count, document.createTextNode(', levytilaa käytetty yhteensä '), size);
            summary.title = total.toLocaleString('fi-FI') + ' tavua';
        }

        async function load() {
            let list;
            try {
                list = await api('GET', '/api/images');
            } catch (error) {
                if (error.status !== 401) {
                    status.hidden = false;
                    status.textContent = messageFor(error, 'Kuvien lataaminen epäonnistui.');
                }
                return;
            }
            images = list;
            status.hidden = images.length > 0;
            status.textContent = 'Kuvia ei ole vielä. Lataa ensimmäinen yllä olevasta painikkeesta tai vedä kuvat tähän.';
            const cards = [];
            for (const image of images) {
                cards.push(createGalleryCard(image, select, deleteImage));
            }
            grid.replaceChildren(...cards);
            showSummary();
            showInfo();
        }

        async function deleteImage(image) {
            if (!window.confirm('Poistetaanko kuva "' + displayName(image) + '"?\n\nPoistoa ei voi perua.')) {
                return;
            }
            errorEl.textContent = '';
            try {
                await api('DELETE', '/api/images/' + encodeURIComponent(image.id));
            } catch (error) {
                if (error.status === 409) {
                    errorEl.textContent = 'Kuva "' + displayName(image) + '" on käytössä reseptissä, joten sitä ei voi poistaa.';
                    await load();
                } else if (error.status === 404) {
                    await load();
                } else if (error.status !== 401) {
                    errorEl.textContent = messageFor(error, 'Kuvan poistaminen epäonnistui.');
                }
                return;
            }
            await load();
        }

        async function uploadFiles(files) {
            const pictures = [];
            for (const file of files) {
                if (/^image\//.test(file.type)) {
                    pictures.push(file);
                }
            }
            if (pictures.length === 0) {
                errorEl.textContent = 'Valitse kuvatiedostoja.';
                return;
            }
            errorEl.textContent = '';
            let outcome;
            try {
                outcome = await uploadAll(pictures, function (index, count) {
                    status.hidden = false;
                    status.textContent = 'Ladataan kuvaa ' + index + '/' + count + '…';
                });
            } catch (error) {
                return;  // Signed out: goToLogin() is already on its way.
            }
            errorEl.textContent = outcome.failures.join(' ');
            await load();
        }

        input.addEventListener('change', function () {
            uploadFiles(input.files);
            input.value = '';
        });

        // Files dropped anywhere on the page are uploaded.
        let dragDepth = 0;
        document.addEventListener('dragenter', function (event) {
            if (event.dataTransfer && Array.from(event.dataTransfer.types).includes('Files')) {
                dragDepth++;
                dropZone.classList.add('is-active');
            }
        });
        document.addEventListener('dragleave', function () {
            dragDepth = Math.max(0, dragDepth - 1);
            if (dragDepth === 0) {
                dropZone.classList.remove('is-active');
            }
        });
        document.addEventListener('dragover', function (event) {
            event.preventDefault();
        });
        document.addEventListener('drop', function (event) {
            event.preventDefault();
            dragDepth = 0;
            dropZone.classList.remove('is-active');
            if (event.dataTransfer && event.dataTransfer.files.length) {
                uploadFiles(event.dataTransfer.files);
            }
        });

        async function start() {
            await requireSession();
            await load();
        }
        start();
    }

    return {
        pick,
        upload,
        initPage,
    };
})();
