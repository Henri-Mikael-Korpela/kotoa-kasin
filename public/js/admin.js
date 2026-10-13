// Admin pages (hallinta). The session is a JWT kept in localStorage and sent
// as "Authorization: Bearer <token>" on API calls that need it.

var TOKEN_KEY = 'kotoaKasinToken';
var DESCRIPTION_MAX_LENGTH = 50000;  // The limit in the recipes table.
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

var FINNISH_ORDER = new Intl.Collator('fi', { sensitivity: 'base' });

/*
 * A recipe address from free text, by the same rules the database enforces:
 * a-z, 0-9 and single dashes, no dash at either end. Letters with marks lose
 * them (ä -> a, é -> e).
 */
function slugify(text) {
  return text.toLowerCase()
    .normalize('NFD').replace(/[\u0300-\u036f]/g, '')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 80)
    .replace(/-+$/, '');
}

function sortByName(items) {
  return items.sort(function (a, b) {
    return FINNISH_ORDER.compare(a.name, b.name);
  });
}

function recipeCountText(count) {
  if (count === 0) {
    return 'Ei reseptejä';
  }
  return count === 1 ? '1 resepti' : count + ' reseptiä';
}

// Recipe list page: recipes and category management.

function deleteRecipe(row, recipe, button, errorEl, onDeleted) {
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
      onDeleted();
    })
    .catch(function (error) {
      button.disabled = false;
      if (error.status !== 401) {
        errorEl.textContent = messageFor(error, 'Reseptin "' + recipe.name + '" poistaminen epäonnistui. Yritä uudelleen.');
      }
    });
}

function createRecipeRow(recipe, errorEl, onDeleted) {
  var row = document.createElement('li');
  row.className = 'recipe-row';

  var link = document.createElement('a');
  link.className = 'recipe-row-link';
  link.href = '/hallinta/resepti?id=' + encodeURIComponent(recipe.id);

  var info = document.createElement('span');
  info.className = 'recipe-row-info';
  var tag = document.createElement('span');
  tag.className = 'recipe-tag';
  tag.textContent = recipe.category;
  var name = document.createElement('h2');
  name.textContent = recipe.name;
  var description = document.createElement('p');
  description.textContent = recipe.excerpt || 'Ei kuvausta.';
  info.append(tag, name, description);

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
    deleteRecipe(row, recipe, remove, errorEl, onDeleted);
  });

  row.append(link, remove);
  return row;
}

function categoryErrorMessage(error, fallback) {
  if (error.status === 409) {
    return 'Samanniminen kategoria on jo olemassa.';
  }
  if (error.status === 400) {
    return 'Anna kategorialle nimi (enintään 60 merkkiä).';
  }
  return messageFor(error, fallback);
}

function createCategoryButton(text, extraClass) {
  var button = document.createElement('button');
  button.type = 'button';
  button.className = 'category-button' + (extraClass ? ' ' + extraClass : '');
  button.textContent = text;
  return button;
}

// Builds a category row that can switch between display and rename modes.
function createCategoryRow(category, errorEl, onChanged) {
  var row = document.createElement('li');
  row.className = 'category-row';

  function showDisplay() {
    var name = document.createElement('span');
    name.className = 'category-name';
    name.textContent = category.name;

    var count = document.createElement('span');
    count.className = 'category-count';
    count.textContent = recipeCountText(category.recipe_count);

    var rename = createCategoryButton('Nimeä uudelleen');
    rename.setAttribute('aria-label', 'Nimeä kategoria ' + category.name + ' uudelleen');
    rename.addEventListener('click', showRename);

    var remove = createCategoryButton('Poista', 'is-danger');
    remove.setAttribute('aria-label', 'Poista kategoria ' + category.name);
    if (category.recipe_count > 0) {
      remove.disabled = true;
      remove.title = 'Kategoriassa on reseptejä. Siirrä ne ensin toiseen kategoriaan.';
    }
    remove.addEventListener('click', function () {
      if (!window.confirm('Poistetaanko kategoria "' + category.name + '"?')) {
        return;
      }
      errorEl.textContent = '';
      remove.disabled = true;
      api('DELETE', '/api/categories/' + encodeURIComponent(category.id))
        .then(onChanged, function (error) {
          remove.disabled = false;
          if (error.status === 409) {
            errorEl.textContent = 'Kategoriassa "' + category.name + '" on reseptejä. Siirrä ne ensin toiseen kategoriaan.';
            onChanged();
          } else if (error.status === 404) {
            onChanged();
          } else if (error.status !== 401) {
            errorEl.textContent = messageFor(error, 'Kategorian poistaminen epäonnistui.');
          }
        });
    });

    var actions = document.createElement('span');
    actions.className = 'category-actions';
    actions.append(rename, remove);
    row.replaceChildren(name, count, actions);
  }

  function showRename() {
    var form = document.createElement('form');
    form.className = 'category-rename';
    form.noValidate = true;

    var input = document.createElement('input');
    input.type = 'text';
    input.maxLength = 60;
    input.value = category.name;
    input.setAttribute('aria-label', 'Kategorian uusi nimi');

    var save = createCategoryButton('Tallenna', 'is-primary');
    save.type = 'submit';
    var cancel = createCategoryButton('Peruuta');
    cancel.addEventListener('click', showDisplay);
    input.addEventListener('keydown', function (event) {
      if (event.key === 'Escape') {
        showDisplay();
      }
    });

    form.addEventListener('submit', function (event) {
      event.preventDefault();
      var name = input.value.trim();
      if (!name) {
        errorEl.textContent = 'Anna kategorialle nimi.';
        input.focus();
        return;
      }
      if (name === category.name) {
        showDisplay();
        return;
      }
      errorEl.textContent = '';
      save.disabled = true;
      api('PUT', '/api/categories/' + encodeURIComponent(category.id), { name: name })
        .then(onChanged, function (error) {
          save.disabled = false;
          if (error.status !== 401) {
            errorEl.textContent = categoryErrorMessage(error, 'Kategorian nimeäminen epäonnistui.');
            input.focus();
          }
        });
    });

    var actions = document.createElement('span');
    actions.className = 'category-actions';
    actions.append(save, cancel);
    form.append(input, actions);
    row.replaceChildren(form);
    input.focus();
    input.select();
  }

  showDisplay();
  return row;
}

function initListPage() {
  var list = document.getElementById('recipeList');
  var status = document.getElementById('listStatus');
  var errorEl = document.getElementById('listError');
  var categoryList = document.getElementById('categoryList');
  var categoryError = document.getElementById('categoryError');
  var categoryForm = document.getElementById('categoryForm');
  var newCategoryName = document.getElementById('newCategoryName');
  setupLogout();

  function loadRecipes() {
    return api('GET', '/api/recipes').then(function (recipes) {
      if (recipes.length === 0) {
        list.replaceChildren();
        status.textContent = 'Reseptejä ei ole vielä. Lisää ensimmäinen resepti yllä olevasta painikkeesta.';
        status.hidden = false;
        return;
      }
      status.hidden = true;
      list.replaceChildren.apply(list, recipes.map(function (recipe) {
        return createRecipeRow(recipe, errorEl, loadCategories);
      }));
    });
  }

  function loadCategories() {
    return api('GET', '/api/categories').then(function (categories) {
      if (categories.length === 0) {
        var empty = document.createElement('li');
        empty.className = 'category-empty';
        empty.textContent = 'Kategorioita ei ole vielä. Reseptin tallentaminen vaatii kategorian.';
        categoryList.replaceChildren(empty);
        return;
      }
      categoryList.replaceChildren.apply(categoryList, sortByName(categories).map(function (category) {
        return createCategoryRow(category, categoryError, reloadAll);
      }));
    });
  }

  // Renames change the category shown on recipes, so both lists refresh together.
  function reloadAll() {
    return Promise.all([loadRecipes(), loadCategories()]).catch(function (error) {
      if (error.status !== 401) {
        status.hidden = false;
        status.textContent = messageFor(error, 'Tietojen lataaminen epäonnistui.');
      }
    });
  }

  categoryForm.addEventListener('submit', function (event) {
    event.preventDefault();
    var name = newCategoryName.value.trim();
    if (!name) {
      categoryError.textContent = 'Anna uudelle kategorialle nimi.';
      newCategoryName.focus();
      return;
    }
    var button = categoryForm.querySelector('button[type="submit"]');
    categoryError.textContent = '';
    button.disabled = true;
    api('POST', '/api/categories', { name: name })
      .then(function () {
        newCategoryName.value = '';
        return loadCategories();
      })
      .catch(function (error) {
        if (error.status !== 401) {
          categoryError.textContent = categoryErrorMessage(error, 'Kategorian lisääminen epäonnistui.');
          newCategoryName.focus();
        }
      })
      .then(function () {
        button.disabled = false;
      });
  });

  requireSession().then(reloadAll);
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
    slug: document.getElementById('recipeSlug'),
    category: document.getElementById('recipeCategory'),
    image: document.getElementById('recipeImage')
  };
  var pickImage = document.getElementById('pickRecipeImage');
  var pageLink = document.getElementById('recipePageLink');
  // The slug follows the name until it is edited by hand.
  var slugEdited = false;
  fields.name.addEventListener('input', function () {
    if (!slugEdited) {
      fields.slug.value = slugify(fields.name.value);
    }
  });
  fields.slug.addEventListener('input', function () {
    slugEdited = fields.slug.value !== '';
  });
  fields.slug.addEventListener('change', function () {
    fields.slug.value = slugify(fields.slug.value);
  });
  var editor = KasinEditor.create(document.getElementById('descriptionEditor'), '');
  var id = new URLSearchParams(window.location.search).get('id');
  setupLogout();

  function updatePreview() {
    preview.replaceWith(createThumb(fields.image.value.trim(), 'Kuvan esikatselu'));
    preview = form.querySelector('.image-field .mini-thumb');
  }
  fields.image.addEventListener('input', updatePreview);
  pickImage.addEventListener('click', function () {
    KasinGallery.pick().then(function (image) {
      if (image) {
        fields.image.value = image.url;
        updatePreview();
      }
    });
  });

  function fillCategories(categories) {
    var placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = categories.length > 0 ? 'Valitse kategoria' : 'Ei kategorioita';
    var options = sortByName(categories).map(function (category) {
      var option = document.createElement('option');
      option.value = String(category.id);
      option.textContent = category.name;
      return option;
    });
    fields.category.replaceChildren.apply(fields.category, [placeholder].concat(options));
    if (categories.length === 0) {
      saveButton.disabled = true;
      errorEl.textContent = 'Kategorioita ei ole vielä. Lisää ensin kategoria reseptilistan alta.';
    }
  }

  if (id) {
    title.textContent = 'Muokkaa reseptiä';
    document.title = 'Muokkaa reseptiä — Kotoa käsin';
    form.hidden = true;
  }

  requireSession()
    .then(function () {
      return Promise.all([
        api('GET', '/api/categories'),
        id ? api('GET', '/api/recipes/' + encodeURIComponent(id)) : null
      ]);
    })
    .then(function (results) {
      fillCategories(results[0]);
      var recipe = results[1];
      if (recipe) {
        fields.name.value = recipe.name;
        fields.slug.value = recipe.slug;
        slugEdited = true;
        pageLink.href = '/reseptit/' + encodeURIComponent(recipe.slug);
        pageLink.hidden = false;
        fields.category.value = String(recipe.category_id);
        fields.image.value = recipe.image;
        editor = KasinEditor.create(document.getElementById('descriptionEditor'), recipe.description);
        updatePreview();
        form.hidden = false;
      }
      fields.name.focus();
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
      slug: slugify(fields.slug.value),
      category_id: Number(fields.category.value),
      image: fields.image.value.trim(),
      description: editor.serialize()
    };
    if (!recipe.name) {
      errorEl.textContent = 'Anna reseptille nimi.';
      fields.name.focus();
      return;
    }
    if (!recipe.slug) {
      errorEl.textContent = 'Anna reseptille osoite.';
      fields.slug.focus();
      return;
    }
    fields.slug.value = recipe.slug;
    if (!fields.category.value) {
      errorEl.textContent = 'Valitse reseptille kategoria.';
      fields.category.focus();
      return;
    }
    if (recipe.description.length > DESCRIPTION_MAX_LENGTH) {
      errorEl.textContent = 'Kuvaus on liian pitkä (' + recipe.description.length + ' merkkiä, enintään ' +
        DESCRIPTION_MAX_LENGTH + '). Lyhennä tekstiä tai poista lohkoja.';
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
          errorEl.textContent = 'Tarkista kentät: nimi, osoite ja kategoria ovat pakollisia. Nimi saa olla enintään ' +
            '200 merkkiä ja kuvan osoite 500 merkkiä. Jos kategoria poistettiin juuri, lataa sivu uudelleen.';
        } else if (error.status === 409) {
          errorEl.textContent = 'Osoite /reseptit/' + recipe.slug + ' on jo toisen reseptin käytössä. Valitse toinen osoite.';
          fields.slug.focus();
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
  } else if (page === 'images') {
    KasinGallery.initPage();
  }
});
