// Recipes can be filtered by category with ?kategoria=<name> on the front page.
var CATEGORY_PARAM = 'kategoria';
var FINNISH_ORDER = new Intl.Collator('fi', { sensitivity: 'base' });

document.addEventListener('DOMContentLoaded', function () {
  var header = document.getElementById('siteHeader');
  var menuToggle = document.getElementById('menuToggle');
  var navLinks = document.getElementById('navLinks');

  function updateHeaderState() {
    // Pages without a cover photo keep the solid header at all scroll positions.
    if (window.scrollY > 40 || header.hasAttribute('data-solid')) {
      header.classList.add('is-scrolled');
    } else {
      header.classList.remove('is-scrolled');
    }
  }

  updateHeaderState();
  window.addEventListener('scroll', updateHeaderState, { passive: true });

  menuToggle.addEventListener('click', function () {
    var isOpen = navLinks.classList.toggle('is-open');
    menuToggle.classList.toggle('is-open', isOpen);
    menuToggle.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
  });

  // Delegated, so links added to the category submenu later close the menu too.
  navLinks.addEventListener('click', function (event) {
    if (event.target.closest('a')) {
      navLinks.classList.remove('is-open');
      menuToggle.classList.remove('is-open');
      menuToggle.setAttribute('aria-expanded', 'false');
    }
  });

  var yearEl = document.getElementById('year');
  if (yearEl) {
    yearEl.textContent = new Date().getFullYear();
  }

  var categoryFilter = new URLSearchParams(window.location.search).get(CATEGORY_PARAM);

  var categoryMenu = document.getElementById('categoryMenu');
  if (categoryMenu) {
    loadCategoryMenu(categoryMenu, categoryFilter);
  }

  var recipeGrid = document.getElementById('recipeGrid');
  if (recipeGrid) {
    loadRecipes(recipeGrid, categoryFilter);
  }

  var recipePage = document.getElementById('recipePage');
  if (recipePage) {
    loadRecipePage(recipePage);
  }
});

function fetchJson(path) {
  return fetch(path, { headers: { 'Accept': 'application/json' } }).then(function (response) {
    if (!response.ok) {
      throw new Error('HTTP ' + response.status);
    }
    return response.json();
  });
}

function sameCategory(a, b) {
  return FINNISH_ORDER.compare(a, b) === 0;
}

function categoryUrl(name) {
  var params = new URLSearchParams();
  params.set(CATEGORY_PARAM, name);
  return '/?' + params.toString() + '#reseptit';
}

// Category submenu under "Reseptit", alphabetically.

function loadCategoryMenu(menu, currentCategory) {
  fetchJson('/api/categories')
    .then(function (categories) {
      if (categories.length === 0) {
        return;
      }
      categories.sort(function (a, b) {
        return FINNISH_ORDER.compare(a.name, b.name);
      });
      var items = categories.map(function (category) {
        var item = document.createElement('li');
        var link = document.createElement('a');
        link.href = categoryUrl(category.name);
        link.textContent = category.name;
        if (currentCategory && sameCategory(category.name, currentCategory)) {
          link.setAttribute('aria-current', 'page');
        }
        item.appendChild(link);
        return item;
      });
      menu.replaceChildren.apply(menu, items);
      menu.hidden = false;
      menu.parentNode.classList.add('has-submenu');
    })
    .catch(function () {
      // Without categories the plain "Reseptit" link still works.
    });
}

// Recipe grid on the front page.

function showRecipeStatus(grid, message) {
  var status = document.createElement('p');
  status.className = 'recipe-grid-status';
  status.textContent = message;
  grid.replaceChildren(status);
}

function showCategoryFilter(category) {
  var filter = document.getElementById('recipeFilter');
  if (!filter) {
    return;
  }
  var label = document.createElement('span');
  label.textContent = 'Kategoria: ';
  var name = document.createElement('strong');
  name.textContent = category;
  var reset = document.createElement('a');
  reset.href = '/#reseptit';
  reset.textContent = 'Näytä kaikki reseptit';
  filter.replaceChildren(label, name, reset);
  filter.hidden = false;
}

function recipeUrl(recipe) {
  return '/resepti?id=' + encodeURIComponent(recipe.id);
}

function createRecipeCard(recipe) {
  var card = document.createElement('article');
  card.className = 'recipe-card';

  var thumb = document.createElement('a');
  thumb.href = recipeUrl(recipe);
  thumb.setAttribute('aria-hidden', 'true');
  thumb.tabIndex = -1;
  thumb.className = 'recipe-thumb';
  thumb.setAttribute('data-emoji', '🌿');
  if (recipe.image) {
    var img = document.createElement('img');
    img.src = recipe.image;
    img.alt = recipe.name;
    img.loading = 'lazy';
    img.addEventListener('error', function () {
      img.remove();
    });
    thumb.appendChild(img);
  }

  var info = document.createElement('div');
  info.className = 'recipe-info';
  var tag = document.createElement('a');
  tag.className = 'recipe-tag';
  tag.href = categoryUrl(recipe.category);
  tag.textContent = recipe.category;
  var title = document.createElement('h3');
  var titleLink = document.createElement('a');
  titleLink.href = recipeUrl(recipe);
  titleLink.textContent = recipe.name;
  title.appendChild(titleLink);
  var description = document.createElement('p');
  description.textContent = recipe.excerpt;
  info.append(tag, title, description);

  card.append(thumb, info);
  return card;
}

function loadRecipes(grid, categoryFilter) {
  if (categoryFilter) {
    showCategoryFilter(categoryFilter);
  }
  fetchJson('/api/recipes')
    .then(function (recipes) {
      if (categoryFilter) {
        recipes = recipes.filter(function (recipe) {
          return sameCategory(recipe.category, categoryFilter);
        });
      }
      if (recipes.length === 0) {
        showRecipeStatus(grid, categoryFilter
          ? 'Tässä kategoriassa ei ole vielä reseptejä.'
          : 'Reseptejä ei ole vielä julkaistu.');
        return;
      }
      grid.replaceChildren.apply(grid, recipes.map(createRecipeCard));
    })
    .catch(function () {
      showRecipeStatus(grid, 'Reseptien lataaminen epäonnistui. Yritä hetken kuluttua uudelleen.');
    });
}

// Recipe page (/resepti?id=...).

function loadRecipePage(page) {
  var id = new URLSearchParams(window.location.search).get('id');
  var status = document.getElementById('recipeStatus');
  if (!id) {
    status.textContent = 'Reseptiä ei löytynyt.';
    return;
  }
  fetchJson('/api/recipes/' + encodeURIComponent(id))
    .then(function (recipe) {
      document.title = recipe.name + ' — Kotoa käsin';
      var tag = document.getElementById('recipeCategory');
      tag.href = categoryUrl(recipe.category);
      tag.textContent = recipe.category;
      document.getElementById('recipeName').textContent = recipe.name;

      var cover = document.getElementById('recipeCover');
      var src = KasinBlocks.safeSrc(recipe.image);
      if (src) {
        var img = document.createElement('img');
        img.src = src;
        img.alt = '';
        img.addEventListener('error', function () {
          cover.hidden = true;
        });
        cover.replaceChildren(img);
        cover.hidden = false;
      }
      KasinBlocks.render(KasinBlocks.parse(recipe.description), document.getElementById('recipeContent'));
      status.hidden = true;
      page.hidden = false;
    })
    .catch(function (error) {
      status.textContent = error.message === 'HTTP 404'
        ? 'Reseptiä ei löytynyt. Se on ehkä poistettu.'
        : 'Reseptin lataaminen epäonnistui. Yritä hetken kuluttua uudelleen.';
    });
}
