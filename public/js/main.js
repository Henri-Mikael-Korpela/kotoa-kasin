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

  navLinks.querySelectorAll('a').forEach(function (link) {
    link.addEventListener('click', function () {
      navLinks.classList.remove('is-open');
      menuToggle.classList.remove('is-open');
      menuToggle.setAttribute('aria-expanded', 'false');
    });
  });

  var yearEl = document.getElementById('year');
  if (yearEl) {
    yearEl.textContent = new Date().getFullYear();
  }

  var recipeGrid = document.getElementById('recipeGrid');
  if (recipeGrid) {
    loadRecipes(recipeGrid);
  }
});

function showRecipeStatus(grid, message) {
  var status = document.createElement('p');
  status.className = 'recipe-grid-status';
  status.textContent = message;
  grid.replaceChildren(status);
}

function createRecipeCard(recipe) {
  var card = document.createElement('article');
  card.className = 'recipe-card';

  var thumb = document.createElement('a');
  thumb.href = '#';
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
  var title = document.createElement('h3');
  var titleLink = document.createElement('a');
  titleLink.href = '#';
  titleLink.textContent = recipe.name;
  title.appendChild(titleLink);
  var description = document.createElement('p');
  description.textContent = recipe.description;
  info.append(title, description);

  card.append(thumb, info);
  return card;
}

function loadRecipes(grid) {
  fetch('/api/recipes', { headers: { 'Accept': 'application/json' } })
    .then(function (response) {
      if (!response.ok) {
        throw new Error('HTTP ' + response.status);
      }
      return response.json();
    })
    .then(function (recipes) {
      if (recipes.length === 0) {
        showRecipeStatus(grid, 'Reseptejä ei ole vielä julkaistu.');
        return;
      }
      grid.replaceChildren.apply(grid, recipes.map(createRecipeCard));
    })
    .catch(function () {
      showRecipeStatus(grid, 'Reseptien lataaminen epäonnistui. Yritä hetken kuluttua uudelleen.');
    });
}
