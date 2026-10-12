// Admin pages (hallinta). The session is a JWT kept in localStorage and sent
// as "Authorization: Bearer <token>" on API calls that need it.

var TOKEN_KEY = 'kotoaKasinToken';
var LOGIN_PAGE = '/hallinta/kirjaudu';
var LIST_PAGE = '/hallinta';

function getToken() {
  try {
    return localStorage.getItem(TOKEN_KEY);
  } catch (e) {
    return null;
  }
}

function setToken(token) {
  try {
    localStorage.setItem(TOKEN_KEY, token);
  } catch (e) {
    // Private mode without storage: the session simply ends with the page.
  }
}

function clearToken() {
  try {
    localStorage.removeItem(TOKEN_KEY);
  } catch (e) {
    // Nothing stored.
  }
}

function goToLogin() {
  clearToken();
  window.location.replace(LOGIN_PAGE);
}

function ApiError(status, message) {
  this.status = status;
  this.message = message;
}

// Calls the JSON API. A 401 on anything but the login itself ends the session.
function api(method, path, body) {
  var headers = { 'Accept': 'application/json' };
  var token = getToken();
  if (token) {
    headers['Authorization'] = 'Bearer ' + token;
  }
  var options = { method: method, headers: headers };
  if (body !== undefined) {
    headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  return fetch(path, options)
    .catch(function () {
      throw new ApiError(0, 'Yhteysvirhe. Tarkista verkkoyhteys ja yritä uudelleen.');
    })
    .then(function (response) {
      return response.json()
        .catch(function () {
          return {};
        })
        .then(function (data) {
          if (response.status === 401 && path !== '/api/login') {
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

// Resolves with the session, or sends the user to the login page.
function requireSession() {
  if (!getToken()) {
    goToLogin();
    return new Promise(function () {});
  }
  return api('GET', '/api/session').then(function (session) {
    var userEl = document.getElementById('adminUser');
    if (userEl) {
      userEl.textContent = session.username;
    }
    return session;
  });
}

function setupLogout() {
  var button = document.getElementById('logoutButton');
  if (button) {
    button.addEventListener('click', goToLogin);
  }
}

function messageFor(error, fallback) {
  if (error.status === 0) {
    return error.message;
  }
  if (error.status === 503) {
    return 'Tietokanta on hetkellisesti varattu. Yritä uudelleen.';
  }
  return fallback;
}

function createThumb(image, alt) {
  var thumb = document.createElement('span');
  thumb.className = 'mini-thumb';
  if (image) {
    var img = document.createElement('img');
    img.src = image;
    img.alt = alt;
    img.loading = 'lazy';
    img.addEventListener('error', function () {
      img.remove();
    });
    thumb.appendChild(img);
  }
  return thumb;
}

// Login page.

function initLoginPage() {
  var form = document.getElementById('loginForm');
  var errorEl = document.getElementById('loginError');
  var button = form.querySelector('button[type="submit"]');

  // Already logged in: skip the form.
  if (getToken()) {
    api('GET', '/api/session').then(function () {
      window.location.replace(LIST_PAGE);
    }, function () {
      clearToken();
    });
  }

  form.addEventListener('submit', function (event) {
    event.preventDefault();
    errorEl.textContent = '';
    button.disabled = true;

    api('POST', '/api/login', {
      username: form.username.value.trim(),
      password: form.password.value
    })
      .then(function (data) {
        setToken(data.token);
        window.location.assign(LIST_PAGE);
      })
      .catch(function (error) {
        button.disabled = false;
        errorEl.textContent = error.status === 401
          ? 'Väärä käyttäjätunnus tai salasana.'
          : messageFor(error, 'Kirjautuminen epäonnistui. Yritä uudelleen.');
        form.password.select();
      });
  });
}

// Recipe list page.

function deleteRecipe(row, recipe, button, errorEl) {
  var confirmed = window.confirm('Poistetaanko resepti "' + recipe.name + '"?\n\nPoistoa ei voi perua.');
  if (!confirmed) {
    return;
  }
  errorEl.textContent = '';
  button.disabled = true;

  api('DELETE', '/api/recipes/' + encodeURIComponent(recipe.id))
    .catch(function (error) {
      if (error.status === 404) {
        return;  // Already gone: drop the row all the same.
      }
      throw error;
    })
    .then(function () {
      var list = row.parentNode;
      row.classList.add('is-removing');
      window.setTimeout(function () {
        row.remove();
        if (list.children.length === 0) {
          var status = document.getElementById('listStatus');
          status.textContent = 'Reseptejä ei ole vielä. Lisää ensimmäinen resepti yllä olevasta painikkeesta.';
          status.hidden = false;
        }
      }, 250);
    })
    .catch(function (error) {
      button.disabled = false;
      if (error.status !== 401) {
        errorEl.textContent = messageFor(error, 'Reseptin "' + recipe.name + '" poistaminen epäonnistui. Yritä uudelleen.');
      }
    });
}

function createRecipeRow(recipe, errorEl) {
  var row = document.createElement('li');
  row.className = 'recipe-row';

  var link = document.createElement('a');
  link.className = 'recipe-row-link';
  link.href = '/hallinta/resepti?id=' + encodeURIComponent(recipe.id);

  var info = document.createElement('span');
  info.className = 'recipe-row-info';
  var name = document.createElement('h2');
  name.textContent = recipe.name;
  var description = document.createElement('p');
  description.textContent = recipe.description || 'Ei kuvausta.';
  info.append(name, description);

  var edit = document.createElement('span');
  edit.className = 'recipe-row-edit';
  edit.textContent = 'Muokkaa';

  link.append(createThumb(recipe.image, ''), info, edit);

  var remove = document.createElement('button');
  remove.type = 'button';
  remove.className = 'recipe-row-delete';
  remove.textContent = 'Poista';
  remove.setAttribute('aria-label', 'Poista resepti ' + recipe.name);
  remove.addEventListener('click', function () {
    deleteRecipe(row, recipe, remove, errorEl);
  });

  row.append(link, remove);
  return row;
}

function initListPage() {
  var list = document.getElementById('recipeList');
  var status = document.getElementById('listStatus');
  var errorEl = document.getElementById('listError');
  setupLogout();

  requireSession()
    .then(function () {
      return api('GET', '/api/recipes');
    })
    .then(function (recipes) {
      if (recipes.length === 0) {
        status.textContent = 'Reseptejä ei ole vielä. Lisää ensimmäinen resepti yllä olevasta painikkeesta.';
        return;
      }
      status.hidden = true;
      list.replaceChildren.apply(list, recipes.map(function (recipe) {
        return createRecipeRow(recipe, errorEl);
      }));
    })
    .catch(function (error) {
      if (error.status !== 401) {
        status.textContent = messageFor(error, 'Reseptien lataaminen epäonnistui.');
      }
    });
}

// Recipe edit page, for both new and existing recipes.

function initEditPage() {
  var form = document.getElementById('recipeForm');
  var title = document.getElementById('editTitle');
  var errorEl = document.getElementById('formError');
  var saveButton = document.getElementById('saveButton');
  var preview = document.getElementById('imagePreview');
  var fields = {
    name: document.getElementById('recipeName'),
    image: document.getElementById('recipeImage'),
    description: document.getElementById('recipeDescription')
  };
  var id = new URLSearchParams(window.location.search).get('id');
  setupLogout();

  function updatePreview() {
    preview.replaceWith(createThumb(fields.image.value.trim(), 'Kuvan esikatselu'));
    preview = form.querySelector('.image-field .mini-thumb');
  }
  fields.image.addEventListener('input', updatePreview);

  if (id) {
    title.textContent = 'Muokkaa reseptiä';
    document.title = 'Muokkaa reseptiä — Kotoa käsin';
    form.hidden = true;
  }

  requireSession()
    .then(function () {
      if (!id) {
        fields.name.focus();
        return;
      }
      return api('GET', '/api/recipes/' + encodeURIComponent(id)).then(function (recipe) {
        fields.name.value = recipe.name;
        fields.image.value = recipe.image;
        fields.description.value = recipe.description;
        updatePreview();
        form.hidden = false;
      });
    })
    .catch(function (error) {
      if (error.status === 401) {
        return;
      }
      form.hidden = false;
      saveButton.disabled = true;
      errorEl.textContent = error.status === 404
        ? 'Reseptiä ei löytynyt. Se on ehkä poistettu.'
        : messageFor(error, 'Reseptin lataaminen epäonnistui.');
    });

  form.addEventListener('submit', function (event) {
    event.preventDefault();
    errorEl.textContent = '';
    var recipe = {
      name: fields.name.value.trim(),
      image: fields.image.value.trim(),
      description: fields.description.value.trim()
    };
    if (!recipe.name) {
      errorEl.textContent = 'Anna reseptille nimi.';
      fields.name.focus();
      return;
    }

    saveButton.disabled = true;
    var request = id
      ? api('PUT', '/api/recipes/' + encodeURIComponent(id), recipe)
      : api('POST', '/api/recipes', recipe);
    request
      .then(function () {
        window.location.assign(LIST_PAGE);
      })
      .catch(function (error) {
        saveButton.disabled = false;
        if (error.status === 400) {
          errorEl.textContent = 'Tarkista kentät: nimi on pakollinen ja enintään 200 merkkiä, ' +
            'kuvan osoite enintään 500 ja kuvaus enintään 2000 merkkiä.';
        } else if (error.status === 413) {
          errorEl.textContent = 'Resepti on liian pitkä tallennettavaksi. Lyhennä kuvausta.';
        } else if (error.status === 404) {
          errorEl.textContent = 'Reseptiä ei löytynyt. Se on ehkä poistettu.';
        } else if (error.status !== 401) {
          errorEl.textContent = messageFor(error, 'Tallentaminen epäonnistui. Yritä uudelleen.');
        }
      });
  });
}

document.addEventListener('DOMContentLoaded', function () {
  var page = document.body.getAttribute('data-page');
  if (page === 'login') {
    initLoginPage();
  } else if (page === 'list') {
    initListPage();
  } else if (page === 'edit') {
    initEditPage();
  }
});
