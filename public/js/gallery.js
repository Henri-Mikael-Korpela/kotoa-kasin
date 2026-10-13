// Image gallery for the admin pages: uploading, the picker dialog used by the
// recipe editor, and the gallery page (/hallinta/kuvat).
//
// Images are scaled down in the browser before upload: at most 1600 pixels on
// the longer side, saved as JPEG. That keeps uploads small and drops the
// metadata phones put in photos, such as where they were taken.

var KasinGallery = (function () {
  var MAX_SIDE = 1600;
  var JPEG_QUALITY = 0.85;

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

  function loadBitmap(file) {
    if (window.createImageBitmap) {
      return createImageBitmap(file, { imageOrientation: 'from-image' }).catch(function () {
        return createImageBitmap(file);
      });
    }
    return new Promise(function (resolve, reject) {
      var url = URL.createObjectURL(file);
      var img = new Image();
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
  function prepareImage(file) {
    return loadBitmap(file).then(function (bitmap) {
      var width = bitmap.width;
      var height = bitmap.height;
      var scale = Math.min(1, MAX_SIDE / Math.max(width, height));
      var canvas = document.createElement('canvas');
      canvas.width = Math.max(1, Math.round(width * scale));
      canvas.height = Math.max(1, Math.round(height * scale));
      var context = canvas.getContext('2d');
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
    });
  }

  // Uploads one file; resolves with {id, url}.
  function upload(file) {
    return prepareImage(file)
      .catch(function () {
        throw new ApiError(415, 'Tiedostoa "' + file.name + '" ei voi lukea kuvana.');
      })
      .then(function (blob) {
        var name = file.name.replace(/\.[^.]*$/, '') + '.jpg';
        return fetch('/api/images', {
          method: 'POST',
          headers: {
            'Accept': 'application/json',
            'Authorization': 'Bearer ' + (getToken() || ''),
            'Content-Type': 'image/jpeg',
            'X-File-Name': encodeURIComponent(name)
          },
          body: blob
        }).catch(function () {
          throw new ApiError(0, 'Yhteysvirhe. Tarkista verkkoyhteys ja yritä uudelleen.');
        });
      })
      .then(function (response) {
        return response.json().catch(function () {
          return {};
        }).then(function (data) {
          if (response.status === 401) {
            goToLogin();
            throw new ApiError(401, 'Istunto on vanhentunut.');
          }
          if (!response.ok) {
            throw new ApiError(response.status, data.error || 'HTTP ' + response.status);
          }
          return data;
        });
      });
  }

  // Uploads files one after another, reporting progress.
  function uploadAll(files, onProgress) {
    var uploaded = [];
    var failures = [];
    var chain = Promise.resolve();
    Array.prototype.forEach.call(files, function (file, index) {
      chain = chain.then(function () {
        onProgress(index + 1, files.length);
        return upload(file).then(function (image) {
          uploaded.push(image);
        }, function (error) {
          if (error.status === 401) {
            throw error;
          }
          failures.push(uploadErrorMessage(error, file));
        });
      });
    });
    return chain.then(function () {
      return { uploaded: uploaded, failures: failures };
    });
  }

  function uploadErrorMessage(error, file) {
    if (error.status === 413) {
      return '"' + file.name + '" on liian suuri.';
    }
    if (error.status === 415) {
      return error.message.indexOf('"') >= 0 ? error.message : '"' + file.name + '" ei ole kuva.';
    }
    if (error.status === 503) {
      return 'Palvelin on kiireinen, "' + file.name + '" jäi lataamatta. Yritä uudelleen.';
    }
    return messageFor(error, '"' + file.name + '" jäi lataamatta.');
  }

  var LONG_PRESS_MS = 500;
  var DOUBLE_TAP_MS = 350;

  function extensionOf(image) {
    var match = /\.([a-z0-9]+)$/i.exec(image.url);
    return match ? match[1].toUpperCase() : '';
  }

  function formatDate(iso) {
    var date = new Date(iso);
    return isNaN(date.getTime()) ? '' : date.toLocaleDateString('fi-FI', {
      day: 'numeric',
      month: 'numeric',
      year: 'numeric'
    });
  }

  function infoRow(label, value) {
    var row = document.createElement('div');
    var term = document.createElement('dt');
    term.textContent = label;
    var detail = document.createElement('dd');
    detail.textContent = value;
    row.append(term, detail);
    return { row: row, value: detail };
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
      var dialog = document.createElement('dialog');
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

      var grid = dialog.querySelector('.gallery-grid');
      var status = dialog.querySelector('.gallery-status');
      var input = dialog.querySelector('input[type="file"]');
      var info = dialog.querySelector('.gallery-info');
      var infoBody = dialog.querySelector('.gallery-info-body');
      var menu = dialog.querySelector('.gallery-menu');
      var images = [];
      var selectedId = null;
      var finished = false;

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

      function findImage(id) {
        return images.filter(function (image) {
          return image.id === id;
        })[0] || null;
      }

      function showEmptyInfo() {
        var hint = document.createElement('p');
        hint.className = 'gallery-info-hint';
        hint.textContent = images.length > 0
          ? 'Napsauta kuvaa nähdäksesi sen tiedot. Kaksoisnapsautus valitsee kuvan.'
          : 'Lataa kuva painikkeesta yllä.';
        infoBody.replaceChildren(hint);
      }

      function showInfo(image) {
        var preview = document.createElement('div');
        preview.className = 'gallery-info-preview';
        var img = document.createElement('img');
        img.src = image.url;
        img.alt = '';
        preview.appendChild(img);

        var list = document.createElement('dl');
        list.className = 'gallery-info-list';
        var dimensions = infoRow('Mitat', '…');
        list.append(
          infoRow('Nimi', displayName(image)).row,
          infoRow('Tiedostotyyppi', extensionOf(image)).row,
          infoRow('Koko', formatSize(image.size)).row,
          dimensions.row,
          infoRow('Ladattu', formatDate(image.created_at)).row,
          infoRow('Käytössä', image.used > 0 ? image.used + ' reseptissä' : 'Ei käytössä').row
        );
        function showDimensions() {
          dimensions.value.textContent = img.naturalWidth + ' × ' + img.naturalHeight + ' px';
        }
        if (img.complete && img.naturalWidth) {
          showDimensions();
        } else {
          img.addEventListener('load', showDimensions);
        }

        var actions = document.createElement('div');
        actions.className = 'gallery-info-actions';
        var use = document.createElement('button');
        use.type = 'button';
        use.className = 'btn btn-primary';
        use.textContent = 'Käytä kuvaa';
        use.addEventListener('click', function () {
          finish(image);
        });
        var remove = createCategoryButton('Poista kuva', 'is-danger');
        if (image.used > 0) {
          remove.disabled = true;
          remove.title = 'Kuva on käytössä reseptissä, joten sitä ei voi poistaa.';
        }
        remove.addEventListener('click', function () {
          deleteImage(image);
        });
        actions.append(use, remove);

        infoBody.replaceChildren(preview, list, actions);
      }

      function select(image, openPanel) {
        selectedId = image.id;
        Array.prototype.forEach.call(grid.querySelectorAll('.gallery-thumb'), function (thumb) {
          thumb.setAttribute('aria-selected', String(Number(thumb.dataset.id) === image.id));
        });
        showInfo(image);
        if (openPanel) {
          info.classList.add('is-open');  // On narrow screens the panel slides in.
        }
      }

      function deleteImage(image) {
        closeMenu();
        if (image.used > 0) {
          status.textContent = 'Kuva "' + displayName(image) + '" on käytössä reseptissä, joten sitä ei voi poistaa.';
          return;
        }
        if (!window.confirm('Poistetaanko kuva "' + displayName(image) + '"?\n\nPoistoa ei voi perua.')) {
          return;
        }
        api('DELETE', '/api/images/' + encodeURIComponent(image.id)).then(function () {
          if (selectedId === image.id) {
            selectedId = null;
            info.classList.remove('is-open');
          }
          return load().then(function () {
            status.textContent = 'Kuva poistettu.';
          });
        }, function (error) {
          if (error.status === 409) {
            status.textContent = 'Kuva "' + displayName(image) + '" on käytössä reseptissä, joten sitä ei voi poistaa.';
          } else if (error.status === 404) {
            load();
          } else if (error.status !== 401) {
            status.textContent = messageFor(error, 'Kuvan poistaminen epäonnistui.');
          }
        });
      }

      // Touch: holding an image opens this menu.
      function openMenu(image) {
        var title = document.createElement('p');
        title.className = 'gallery-menu-title';
        title.textContent = displayName(image);
        var options = [
          { text: 'Käytä kuvaa', action: function () { finish(image); } },
          { text: 'Kuvan tiedot', action: function () { closeMenu(); select(image, true); } },
          { text: 'Poista', danger: true, action: function () { deleteImage(image); } },
          { text: 'Peruuta', action: closeMenu }
        ];
        var buttons = options.map(function (option) {
          var item = document.createElement('button');
          item.type = 'button';
          item.setAttribute('role', 'menuitem');
          item.className = 'gallery-menu-item' + (option.danger ? ' is-danger' : '');
          item.textContent = option.text;
          if (option.danger && image.used > 0) {
            item.disabled = true;
            item.textContent = 'Poista (käytössä reseptissä)';
          }
          item.addEventListener('click', option.action);
          return item;
        });
        menu.replaceChildren.apply(menu, [title].concat(buttons));
        menu.hidden = false;
        buttons[0].focus();
      }

      function closeMenu() {
        menu.hidden = true;
      }
      menu.addEventListener('click', function (event) {
        if (event.target === menu) {
          closeMenu();  // A tap outside the options.
        }
      });

      function createTile(image) {
        var tile = document.createElement('div');
        tile.className = 'gallery-tile';

        var thumb = document.createElement('button');
        thumb.type = 'button';
        thumb.className = 'gallery-thumb';
        thumb.dataset.id = String(image.id);
        thumb.setAttribute('role', 'option');
        thumb.setAttribute('aria-selected', String(image.id === selectedId));
        thumb.setAttribute('aria-label', displayName(image) + '. Kaksoisnapsautus valitsee.');
        var img = document.createElement('img');
        img.src = image.url;
        img.alt = '';
        img.loading = 'lazy';
        img.draggable = false;
        thumb.appendChild(img);

        var remove = document.createElement('button');
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

        // Touch: tap shows details, double tap chooses, holding opens the menu.
        var pressTimer = null;
        var lastTap = 0;
        var startX = 0;
        var startY = 0;
        var suppressClick = false;
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
          var now = Date.now();
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

      function load() {
        status.textContent = 'Ladataan kuvia…';
        return api('GET', '/api/images').then(function (list) {
          images = list;
          status.textContent = images.length === 0 ? 'Ei vielä kuvia. Lataa ensimmäinen.' : '';
          grid.replaceChildren.apply(grid, images.map(createTile));
          var selected = selectedId !== null ? findImage(selectedId) : null;
          if (selected) {
            showInfo(selected);
          } else {
            selectedId = null;
            showEmptyInfo();
          }
        }, function (error) {
          if (error.status !== 401) {
            status.textContent = messageFor(error, 'Kuvien lataaminen epäonnistui.');
          }
        });
      }

      input.addEventListener('change', function () {
        var files = input.files;
        if (!files.length) {
          return;
        }
        uploadAll(files, function (index, count) {
          status.textContent = count > 1 ? 'Ladataan kuvaa ' + index + '/' + count + '…' : 'Ladataan kuvaa…';
        }).then(function (outcome) {
          input.value = '';
          if (outcome.uploaded.length > 0) {
            selectedId = outcome.uploaded[outcome.uploaded.length - 1].id;  // Show the newest upload.
          }
          return load().then(function () {
            status.textContent = outcome.failures.length > 0
              ? outcome.failures.join(' ')
              : (outcome.uploaded.length === 1 ? 'Kuva ladattu. Kaksoisnapsauta sitä valitaksesi.' : 'Kuvat ladattu.');
          });
        }, function () {
          // Signed out: goToLogin() is already on its way.
        });
      });

      dialog.showModal();
      showEmptyInfo();
      load();
    });
  }

  // The gallery page.

  function createGalleryCard(image, onChanged, errorEl) {
    var card = document.createElement('li');
    card.className = 'gallery-card';

    var link = document.createElement('a');
    link.className = 'gallery-card-image';
    link.href = image.url;
    link.target = '_blank';
    link.rel = 'noopener';
    var img = document.createElement('img');
    img.src = image.url;
    img.alt = displayName(image);
    img.loading = 'lazy';
    link.appendChild(img);

    var info = document.createElement('div');
    info.className = 'gallery-card-info';
    var name = document.createElement('p');
    name.className = 'gallery-card-name';
    name.textContent = displayName(image);
    var meta = document.createElement('p');
    meta.className = 'gallery-card-meta';
    meta.textContent = formatSize(image.size) + ' · ' +
      (image.used > 0 ? 'Käytössä ' + image.used + ' reseptissä' : 'Ei käytössä');
    info.append(name, meta);

    var actions = document.createElement('div');
    actions.className = 'gallery-card-actions';
    var copy = createCategoryButton('Kopioi osoite');
    copy.addEventListener('click', function () {
      var done = function () {
        copy.textContent = 'Kopioitu';
        window.setTimeout(function () {
          copy.textContent = 'Kopioi osoite';
        }, 1500);
      };
      if (navigator.clipboard) {
        navigator.clipboard.writeText(image.url).then(done, function () {
          window.prompt('Kuvan osoite:', image.url);
        });
      } else {
        window.prompt('Kuvan osoite:', image.url);
      }
    });
    var remove = createCategoryButton('Poista', 'is-danger');
    if (image.used > 0) {
      remove.disabled = true;
      remove.title = 'Kuva on käytössä reseptissä. Poista se ensin resepteistä.';
    }
    remove.addEventListener('click', function () {
      if (!window.confirm('Poistetaanko kuva "' + displayName(image) + '"?\n\nPoistoa ei voi perua.')) {
        return;
      }
      errorEl.textContent = '';
      remove.disabled = true;
      api('DELETE', '/api/images/' + encodeURIComponent(image.id)).then(onChanged, function (error) {
        remove.disabled = false;
        if (error.status === 409) {
          errorEl.textContent = 'Kuva "' + displayName(image) + '" on käytössä reseptissä, joten sitä ei voi poistaa.';
          onChanged();
        } else if (error.status === 404) {
          onChanged();
        } else if (error.status !== 401) {
          errorEl.textContent = messageFor(error, 'Kuvan poistaminen epäonnistui.');
        }
      });
    });
    actions.append(copy, remove);

    card.append(link, info, actions);
    return card;
  }

  function initPage() {
    var grid = document.getElementById('galleryGrid');
    var status = document.getElementById('galleryStatus');
    var errorEl = document.getElementById('galleryError');
    var input = document.getElementById('galleryUpload');
    var dropZone = document.getElementById('galleryDropZone');
    setupLogout();

    function load() {
      return api('GET', '/api/images').then(function (images) {
        status.hidden = images.length > 0;
        status.textContent = 'Kuvia ei ole vielä. Lataa ensimmäinen yllä olevasta painikkeesta tai vedä kuvat tähän.';
        grid.replaceChildren.apply(grid, images.map(function (image) {
          return createGalleryCard(image, load, errorEl);
        }));
      }, function (error) {
        if (error.status !== 401) {
          status.hidden = false;
          status.textContent = messageFor(error, 'Kuvien lataaminen epäonnistui.');
        }
      });
    }

    function uploadFiles(files) {
      var images = Array.prototype.filter.call(files, function (file) {
        return /^image\//.test(file.type);
      });
      if (images.length === 0) {
        errorEl.textContent = 'Valitse kuvatiedostoja.';
        return;
      }
      errorEl.textContent = '';
      uploadAll(images, function (index, count) {
        status.hidden = false;
        status.textContent = 'Ladataan kuvaa ' + index + '/' + count + '…';
      }).then(function (outcome) {
        errorEl.textContent = outcome.failures.join(' ');
        return load();
      }, function () {
        // Signed out: goToLogin() is already on its way.
      });
    }

    input.addEventListener('change', function () {
      uploadFiles(input.files);
      input.value = '';
    });

    // Files dropped anywhere on the page are uploaded.
    var dragDepth = 0;
    document.addEventListener('dragenter', function (event) {
      if (event.dataTransfer && Array.prototype.indexOf.call(event.dataTransfer.types, 'Files') >= 0) {
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

    requireSession().then(load);
  }

  return {
    pick: pick,
    upload: upload,
    initPage: initPage
  };
})();
