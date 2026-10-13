// Recipes can be filtered by category with ?kategoria=<name> on the front page.
const CATEGORY_PARAM = 'kategoria';
const FINNISH_ORDER = new Intl.Collator('fi', { sensitivity: 'base' });

document.addEventListener('DOMContentLoaded', function () {
    const header = document.getElementById('siteHeader');
    const menuToggle = document.getElementById('menuToggle');
    const navLinks = document.getElementById('navLinks');

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
        const isOpen = navLinks.classList.toggle('is-open');
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

    const yearEl = document.getElementById('year');
    if (yearEl) {
        yearEl.textContent = new Date().getFullYear();
    }

    const categoryFilter = new URLSearchParams(window.location.search).get(CATEGORY_PARAM);

    const categoryMenu = document.getElementById('categoryMenu');
    if (categoryMenu) {
        loadCategoryMenu(categoryMenu, categoryFilter);
    }

    const recipeGrid = document.getElementById('recipeGrid');
    if (recipeGrid) {
        loadRecipes(recipeGrid, categoryFilter);
    }

    const recipePage = document.getElementById('recipePage');
    if (recipePage) {
        loadRecipePage(recipePage);
    }
});

async function fetchJson(path) {
    const response = await fetch(path, { headers: { 'Accept': 'application/json' } });
    if (!response.ok) {
        throw new Error('HTTP ' + response.status);
    }
    return response.json();
}

function sameCategory(a, b) {
    return FINNISH_ORDER.compare(a, b) === 0;
}

function categoryUrl(name) {
    const params = new URLSearchParams();
    params.set(CATEGORY_PARAM, name);
    return '/?' + params.toString() + '#reseptit';
}

// The categories in Finnish alphabetical order, as a new array.
function sortedByName(categories) {
    const sorted = [...categories];
    sorted.sort(function (a, b) {
        return FINNISH_ORDER.compare(a.name, b.name);
    });
    return sorted;
}

// Category submenu under "Reseptit", alphabetically.

async function loadCategoryMenu(menu, currentCategory) {
    let categories;
    try {
        categories = await fetchJson('/api/categories');
    } catch (error) {
        return;  // Without categories the plain "Reseptit" link still works.
    }
    if (categories.length === 0) {
        return;
    }
    const items = [];
    for (const category of sortedByName(categories)) {
        const item = document.createElement('li');
        const link = document.createElement('a');
        link.href = categoryUrl(category.name);
        link.textContent = category.name;
        if (currentCategory && sameCategory(category.name, currentCategory)) {
            link.setAttribute('aria-current', 'page');
        }
        item.appendChild(link);
        items.push(item);
    }
    menu.replaceChildren(...items);
    menu.hidden = false;
    menu.parentNode.classList.add('has-submenu');
}

// Recipe grid on the front page.

function showRecipeStatus(grid, message) {
    const status = document.createElement('p');
    status.className = 'recipe-grid-status';
    status.textContent = message;
    grid.replaceChildren(status);
}

function showCategoryFilter(category) {
    const filter = document.getElementById('recipeFilter');
    if (!filter) {
        return;
    }
    const label = document.createElement('span');
    label.textContent = 'Kategoria: ';
    const name = document.createElement('strong');
    name.textContent = category;
    const reset = document.createElement('a');
    reset.href = '/#reseptit';
    reset.textContent = 'Näytä kaikki reseptit';
    filter.replaceChildren(label, name, reset);
    filter.hidden = false;
}

function recipeUrl(recipe) {
    return '/reseptit/' + encodeURIComponent(recipe.slug);
}

function createRecipeCard(recipe) {
    const card = document.createElement('article');
    card.className = 'recipe-card';

    const thumb = document.createElement('a');
    thumb.href = recipeUrl(recipe);
    thumb.setAttribute('aria-hidden', 'true');
    thumb.tabIndex = -1;
    thumb.className = 'recipe-thumb';
    thumb.setAttribute('data-emoji', '🌿');
    if (recipe.image) {
        const img = document.createElement('img');
        img.src = recipe.image;
        img.alt = recipe.name;
        img.loading = 'lazy';
        img.addEventListener('error', function () {
            img.remove();
        });
        thumb.appendChild(img);
    }

    const info = document.createElement('div');
    info.className = 'recipe-info';
    const tag = document.createElement('a');
    tag.className = 'recipe-tag';
    tag.href = categoryUrl(recipe.category);
    tag.textContent = recipe.category;
    const title = document.createElement('h3');
    const titleLink = document.createElement('a');
    titleLink.href = recipeUrl(recipe);
    titleLink.textContent = recipe.name;
    title.appendChild(titleLink);
    const description = document.createElement('p');
    description.textContent = recipe.excerpt;
    info.append(tag, title, description);

    card.append(thumb, info);
    return card;
}

async function loadRecipes(grid, categoryFilter) {
    if (categoryFilter) {
        showCategoryFilter(categoryFilter);
    }
    let recipes;
    try {
        recipes = await fetchJson('/api/recipes');
    } catch (error) {
        showRecipeStatus(grid, 'Reseptien lataaminen epäonnistui. Yritä hetken kuluttua uudelleen.');
        return;
    }

    const shown = [];
    for (const recipe of recipes) {
        if (!categoryFilter || sameCategory(recipe.category, categoryFilter)) {
            shown.push(recipe);
        }
    }
    if (shown.length === 0) {
        showRecipeStatus(grid, categoryFilter
            ? 'Tässä kategoriassa ei ole vielä reseptejä.'
            : 'Reseptejä ei ole vielä julkaistu.');
        return;
    }
    const cards = [];
    for (const recipe of shown) {
        cards.push(createRecipeCard(recipe));
    }
    grid.replaceChildren(...cards);
}

// Recipe page: /reseptit/<slug>. The older /resepti?id=<id> addresses still
// work and are switched to the slug address.

function recipeRequest() {
    const match = /^\/reseptit\/([^/]+)$/.exec(window.location.pathname);
    if (match) {
        return '/api/recipes/slug/' + match[1];
    }
    const id = new URLSearchParams(window.location.search).get('id');
    return id ? '/api/recipes/' + encodeURIComponent(id) : null;
}

function showRecipe(recipe, page, status) {
    if (window.location.pathname !== recipeUrl(recipe)) {
        window.history.replaceState(null, '', recipeUrl(recipe));
    }
    document.title = recipe.name + ' — Kotoa käsin';
    const tag = document.getElementById('recipeCategory');
    tag.href = categoryUrl(recipe.category);
    tag.textContent = recipe.category;
    document.getElementById('recipeName').textContent = recipe.name;

    const cover = document.getElementById('recipeCover');
    const src = KasinBlocks.safeSrc(recipe.image);
    if (src) {
        const img = document.createElement('img');
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
}

async function loadRecipePage(page) {
    const status = document.getElementById('recipeStatus');
    const request = recipeRequest();
    if (!request) {
        status.textContent = 'Reseptiä ei löytynyt.';
        return;
    }
    try {
        const recipe = await fetchJson(request);
        showRecipe(recipe, page, status);
    } catch (error) {
        status.textContent = error.message === 'HTTP 404'
            ? 'Reseptiä ei löytynyt. Se on ehkä poistettu.'
            : 'Reseptin lataaminen epäonnistui. Yritä hetken kuluttua uudelleen.';
    }
}
