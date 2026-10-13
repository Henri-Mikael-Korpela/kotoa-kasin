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

  function createThumbButton(image) {
    var button = document.createElement('button');
    button.type = 'button';
    button.className = 'gallery-thumb';
    var img = document.createElement('img');
    img.src = image.url;
    img.alt = displayName(image);
    img.loading = 'lazy';
    button.appendChild(img);
    button.setAttribute('aria-label', 'Valitse ' + displayName(image));
    return button;
  }

  // Picker dialog: resolves with the chosen image ({id, url, ...}) or null.
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
        '<div class="gallery-grid gallery-dialog-grid"></div>';
      document.body.appendChild(dialog);

      var grid = dialog.querySelector('.gallery-grid');
      var status = dialog.querySelector('.gallery-status');
      var input = dialog.querySelector('input[type="file"]');
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

      function load() {
        status.textContent = 'Ladataan kuvia…';
        return api('GET', '/api/images').then(function (images) {
          status.textContent = images.length === 0 ? 'Ei vielä kuvia. Lataa ensimmäinen.' : '';
          grid.replaceChildren.apply(grid, images.map(function (image) {
            var button = createThumbButton(image);
            button.addEventListener('click', function () {
              finish(image);
            });
            return button;
          }));
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
          if (outcome.uploaded.length === 1 && outcome.failures.length === 0) {
            finish(outcome.uploaded[0]);  // A single new image is the one wanted.
            return;
          }
          return load().then(function () {
            status.textContent = outcome.failures.join(' ');
          });
        }, function () {
          // Signed out: goToLogin() is already on its way.
        });
      });

      dialog.showModal();
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
