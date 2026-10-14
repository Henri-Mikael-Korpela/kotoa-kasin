// Admin pages (hallinta). The session is a JWT kept in localStorage and sent
// as "Authorization: Bearer <token>" on API calls that need it.

const TOKEN_KEY = 'kotoaKasinToken';
const DESCRIPTION_MAX_LENGTH = 50000;  // The limit in the recipes table.
const LOGIN_PAGE = '/hallinta/kirjaudu';
const LIST_PAGE = '/hallinta';
const FINNISH_ORDER = new Intl.Collator('fi', { sensitivity: 'base' });

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

/*
 * What api() throws: the HTTP status (0 when the server was not reached) and a
 * message. For a refused or failed login also the wait before the next attempt,
 * in seconds, and why ("delayed", "banned" or "busy").
 */
function apiError(status, message, data) {
    const details = data || {};
    return {
        status,
        message,
        retryAfter: Number(details.retry_after) || 0,
        reason: typeof details.reason === 'string' ? details.reason : '',
    };
}

/*
 * Sends a request and returns the JSON answer. Throws apiError() when the server
 * cannot be reached or answers with an error. A 401 on anything but the login
 * itself ends the session.
 */
async function apiFetch(path, options) {
    let response;
    try {
        response = await fetch(path, options);
    } catch (error) {
        throw apiError(0, 'Yhteysvirhe. Tarkista verkkoyhteys ja yritä uudelleen.');
    }
    let data = {};
    try {
        data = await response.json();
    } catch (error) {
        data = {};
    }
    if (response.status === 401 && path !== '/api/login') {
        goToLogin();
        throw apiError(401, 'Istunto on vanhentunut.');
    }
    if (!response.ok) {
        throw apiError(response.status, data.error || 'HTTP ' + response.status, data);
    }
    return data;
}

// Calls the JSON API, signed in when there is a session.
async function api(method, path, body) {
    const headers = { 'Accept': 'application/json' };
    const token = getToken();
    if (token) {
        headers['Authorization'] = 'Bearer ' + token;
    }
    const options = { method, headers };
    if (body !== undefined) {
        headers['Content-Type'] = 'application/json';
        options.body = JSON.stringify(body);
    }
    return apiFetch(path, options);
}

// Resolves with the session, or sends the user to the login page.
async function requireSession() {
    if (!getToken()) {
        goToLogin();
        await new Promise(function () {});  // Never resolves: the page is on its way elsewhere.
    }
    const session = await api('GET', '/api/session');
    const userEl = document.getElementById('adminUser');
    if (userEl) {
        userEl.textContent = session.username;
    }
    return session;
}

function setupLogout() {
    const button = document.getElementById('logoutButton');
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
    const thumb = document.createElement('span');
    thumb.className = 'mini-thumb';
    if (image) {
        const img = document.createElement('img');
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

/*
 * A recipe address from free text, by the same rules the database enforces:
 * a-z, 0-9 and single dashes, no dash at either end. Letters with marks lose
 * them (ä -> a, é -> e).
 */
function slugify(text) {
    return text.toLowerCase()
        .normalize('NFD').replace(/[̀-ͯ]/g, '')
        .replace(/[^a-z0-9]+/g, '-')
        .replace(/^-+|-+$/g, '')
        .slice(0, 80)
        .replace(/-+$/, '');
}

// The items in Finnish alphabetical order by name, as a new array.
function sortByName(items) {
    const sorted = [...items];
    sorted.sort(function (a, b) {
        return FINNISH_ORDER.compare(a.name, b.name);
    });
    return sorted;
}

function recipeCountText(count) {
    if (count === 0) {
        return 'Ei reseptejä';
    }
    return count === 1 ? '1 resepti' : count + ' reseptiä';
}

// Login page.

// "45 sekunnin" or "15 minuutin", for "... kuluttua".
function waitText(seconds) {
    if (seconds < 60) {
        return seconds + ' sekunnin';
    }
    const minutes = Math.ceil(seconds / 60);
    return minutes + ' minuutin';
}

// What to say when the server refused a login or asked to wait after a wrong password.
function loginErrorText(error, seconds) {
    if (error.status === 401) {
        return seconds > 0
            ? 'Väärä käyttäjätunnus tai salasana. Voit yrittää uudelleen ' + waitText(seconds) + ' kuluttua.'
            : 'Väärä käyttäjätunnus tai salasana.';
    }
    if (error.status === 429 && error.reason === 'banned') {
        return 'Liian monta epäonnistunutta yritystä. Kirjautuminen tästä osoitteesta on estetty. ' +
            'Yritä uudelleen ' + waitText(seconds) + ' kuluttua.';
    }
    if (error.status === 429 && error.reason === 'busy') {
        return 'Kirjautumisyrityksiä on juuri nyt liikaa. Yritä uudelleen ' + waitText(seconds) + ' kuluttua.';
    }
    if (error.status === 429) {
        return 'Liian monta epäonnistunutta yritystä. Yritä uudelleen ' + waitText(seconds) + ' kuluttua.';
    }
    return messageFor(error, 'Kirjautuminen epäonnistui. Yritä uudelleen.');
}

function initLoginPage() {
    const form = document.getElementById('loginForm');
    const errorEl = document.getElementById('loginError');
    const button = form.querySelector('button[type="submit"]');
    let countdown = null;

    // Keeps the button disabled while the server asks to wait, counting down in the message.
    function showWait(error) {
        window.clearInterval(countdown);
        const until = Date.now() + error.retryAfter * 1000;
        function tick() {
            const seconds = Math.max(0, Math.ceil((until - Date.now()) / 1000));
            errorEl.textContent = loginErrorText(error, seconds);
            button.disabled = seconds > 0;
            if (seconds === 0) {
                window.clearInterval(countdown);
            }
        }
        tick();
        countdown = window.setInterval(tick, 1000);
    }

    // Already logged in: skip the form.
    async function skipIfLoggedIn() {
        if (!getToken()) {
            return;
        }
        try {
            await api('GET', '/api/session');
        } catch (error) {
            clearToken();
            return;
        }
        window.location.replace(LIST_PAGE);
    }
    skipIfLoggedIn();

    form.addEventListener('submit', async function (event) {
        event.preventDefault();
        errorEl.textContent = '';
        button.disabled = true;

        let data;
        try {
            data = await api('POST', '/api/login', {
                username: form.username.value.trim(),
                password: form.password.value,
            });
        } catch (error) {
            button.disabled = false;
            if (error.retryAfter > 0) {
                showWait(error);
            } else {
                errorEl.textContent = loginErrorText(error, 0);
            }
            form.password.select();
            return;
        }
        setToken(data.token);
        window.location.assign(LIST_PAGE);
    });
}

// Recipe list page: recipes and category management.

async function deleteRecipe(row, recipe, button, errorEl, onDeleted) {
    const confirmed = window.confirm('Poistetaanko resepti "' + recipe.name + '"?\n\nPoistoa ei voi perua.');
    if (!confirmed) {
        return;
    }
    errorEl.textContent = '';
    button.disabled = true;

    try {
        await api('DELETE', '/api/recipes/' + encodeURIComponent(recipe.id));
    } catch (error) {
        // Already gone (404): drop the row all the same.
        if (error.status !== 404) {
            button.disabled = false;
            if (error.status !== 401) {
                errorEl.textContent = messageFor(error, 'Reseptin "' + recipe.name + '" poistaminen epäonnistui. Yritä uudelleen.');
            }
            return;
        }
    }

    const list = row.parentNode;
    row.classList.add('is-removing');
    window.setTimeout(function () {
        row.remove();
        if (list.children.length === 0) {
            const status = document.getElementById('listStatus');
            status.textContent = 'Reseptejä ei ole vielä. Lisää ensimmäinen resepti yllä olevasta painikkeesta.';
            status.hidden = false;
        }
    }, 250);
    onDeleted();
}

function createRecipeRow(recipe, errorEl, onDeleted) {
    const row = document.createElement('li');
    row.className = 'recipe-row';

    const link = document.createElement('a');
    link.className = 'recipe-row-link';
    link.href = '/hallinta/resepti?id=' + encodeURIComponent(recipe.id);

    const info = document.createElement('span');
    info.className = 'recipe-row-info';
    const tag = document.createElement('span');
    tag.className = 'recipe-tag';
    tag.textContent = recipe.category;
    const name = document.createElement('h2');
    name.textContent = recipe.name;
    const description = document.createElement('p');
    description.textContent = recipe.excerpt || 'Ei kuvausta.';
    info.append(tag, name, description);

    const edit = document.createElement('span');
    edit.className = 'recipe-row-edit';
    edit.textContent = 'Muokkaa';

    link.append(createThumb(recipe.image, ''), info, edit);

    const remove = document.createElement('button');
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
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'category-button' + (extraClass ? ' ' + extraClass : '');
    button.textContent = text;
    return button;
}

// Builds a category row that can switch between display and rename modes.
function createCategoryRow(category, errorEl, onChanged) {
    const row = document.createElement('li');
    row.className = 'category-row';

    async function deleteCategory(remove) {
        if (!window.confirm('Poistetaanko kategoria "' + category.name + '"?')) {
            return;
        }
        errorEl.textContent = '';
        remove.disabled = true;
        try {
            await api('DELETE', '/api/categories/' + encodeURIComponent(category.id));
        } catch (error) {
            remove.disabled = false;
            if (error.status === 409) {
                errorEl.textContent = 'Kategoriassa "' + category.name + '" on reseptejä. Siirrä ne ensin toiseen kategoriaan.';
                onChanged();
            } else if (error.status === 404) {
                onChanged();
            } else if (error.status !== 401) {
                errorEl.textContent = messageFor(error, 'Kategorian poistaminen epäonnistui.');
            }
            return;
        }
        onChanged();
    }

    function showDisplay() {
        const name = document.createElement('span');
        name.className = 'category-name';
        name.textContent = category.name;

        const count = document.createElement('span');
        count.className = 'category-count';
        count.textContent = recipeCountText(category.recipe_count);

        const rename = createCategoryButton('Nimeä uudelleen');
        rename.setAttribute('aria-label', 'Nimeä kategoria ' + category.name + ' uudelleen');
        rename.addEventListener('click', showRename);

        const remove = createCategoryButton('Poista', 'is-danger');
        remove.setAttribute('aria-label', 'Poista kategoria ' + category.name);
        if (category.recipe_count > 0) {
            remove.disabled = true;
            remove.title = 'Kategoriassa on reseptejä. Siirrä ne ensin toiseen kategoriaan.';
        }
        remove.addEventListener('click', function () {
            deleteCategory(remove);
        });

        const actions = document.createElement('span');
        actions.className = 'category-actions';
        actions.append(rename, remove);
        row.replaceChildren(name, count, actions);
    }

    function showRename() {
        const form = document.createElement('form');
        form.className = 'category-rename';
        form.noValidate = true;

        const input = document.createElement('input');
        input.type = 'text';
        input.maxLength = 60;
        input.value = category.name;
        input.setAttribute('aria-label', 'Kategorian uusi nimi');

        const save = createCategoryButton('Tallenna', 'is-primary');
        save.type = 'submit';
        const cancel = createCategoryButton('Peruuta');
        cancel.addEventListener('click', showDisplay);
        input.addEventListener('keydown', function (event) {
            if (event.key === 'Escape') {
                showDisplay();
            }
        });

        form.addEventListener('submit', async function (event) {
            event.preventDefault();
            const name = input.value.trim();
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
            try {
                await api('PUT', '/api/categories/' + encodeURIComponent(category.id), { name });
            } catch (error) {
                save.disabled = false;
                if (error.status !== 401) {
                    errorEl.textContent = categoryErrorMessage(error, 'Kategorian nimeäminen epäonnistui.');
                    input.focus();
                }
                return;
            }
            onChanged();
        });

        const actions = document.createElement('span');
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
    const list = document.getElementById('recipeList');
    const status = document.getElementById('listStatus');
    const errorEl = document.getElementById('listError');
    const categoryList = document.getElementById('categoryList');
    const categoryError = document.getElementById('categoryError');
    const categoryForm = document.getElementById('categoryForm');
    const newCategoryName = document.getElementById('newCategoryName');
    setupLogout();

    async function loadRecipes() {
        const recipes = await api('GET', '/api/recipes');
        if (recipes.length === 0) {
            list.replaceChildren();
            status.textContent = 'Reseptejä ei ole vielä. Lisää ensimmäinen resepti yllä olevasta painikkeesta.';
            status.hidden = false;
            return;
        }
        status.hidden = true;
        const rows = [];
        for (const recipe of recipes) {
            rows.push(createRecipeRow(recipe, errorEl, loadCategories));
        }
        list.replaceChildren(...rows);
    }

    async function loadCategories() {
        const categories = await api('GET', '/api/categories');
        if (categories.length === 0) {
            const empty = document.createElement('li');
            empty.className = 'category-empty';
            empty.textContent = 'Kategorioita ei ole vielä. Reseptin tallentaminen vaatii kategorian.';
            categoryList.replaceChildren(empty);
            return;
        }
        const rows = [];
        for (const category of sortByName(categories)) {
            rows.push(createCategoryRow(category, categoryError, reloadAll));
        }
        categoryList.replaceChildren(...rows);
    }

    // Renames change the category shown on recipes, so both lists refresh together.
    async function reloadAll() {
        try {
            await Promise.all([loadRecipes(), loadCategories()]);
        } catch (error) {
            if (error.status !== 401) {
                status.hidden = false;
                status.textContent = messageFor(error, 'Tietojen lataaminen epäonnistui.');
            }
        }
    }

    categoryForm.addEventListener('submit', async function (event) {
        event.preventDefault();
        const name = newCategoryName.value.trim();
        if (!name) {
            categoryError.textContent = 'Anna uudelle kategorialle nimi.';
            newCategoryName.focus();
            return;
        }
        const button = categoryForm.querySelector('button[type="submit"]');
        categoryError.textContent = '';
        button.disabled = true;
        try {
            await api('POST', '/api/categories', { name });
            newCategoryName.value = '';
            await loadCategories();
        } catch (error) {
            if (error.status !== 401) {
                categoryError.textContent = categoryErrorMessage(error, 'Kategorian lisääminen epäonnistui.');
                newCategoryName.focus();
            }
        }
        button.disabled = false;
    });

    async function start() {
        await requireSession();
        await reloadAll();
    }
    start();
}

// Recipe edit page, for both new and existing recipes.

function initEditPage() {
    const form = document.getElementById('recipeForm');
    const title = document.getElementById('editTitle');
    const errorEl = document.getElementById('formError');
    const saveButton = document.getElementById('saveButton');
    let preview = document.getElementById('imagePreview');
    const fields = {
        name: document.getElementById('recipeName'),
        slug: document.getElementById('recipeSlug'),
        category: document.getElementById('recipeCategory'),
        image: document.getElementById('recipeImage'),
    };
    const pickImage = document.getElementById('pickRecipeImage');
    const pageLink = document.getElementById('recipePageLink');
    const id = new URLSearchParams(window.location.search).get('id');
    let editor = KasinEditor.create(document.getElementById('descriptionEditor'), '');
    setupLogout();

    // The slug follows the name until it is edited by hand.
    let slugEdited = false;
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

    function updatePreview() {
        preview.replaceWith(createThumb(fields.image.value.trim(), 'Kuvan esikatselu'));
        preview = form.querySelector('.image-field .mini-thumb');
    }
    fields.image.addEventListener('input', updatePreview);
    pickImage.addEventListener('click', async function () {
        const image = await KasinGallery.pick();
        if (image) {
            fields.image.value = image.url;
            updatePreview();
            scheduleLive();
        }
    });

    function fillCategories(categories) {
        const placeholder = document.createElement('option');
        placeholder.value = '';
        placeholder.textContent = categories.length > 0 ? 'Valitse kategoria' : 'Ei kategorioita';
        const options = [placeholder];
        for (const category of sortByName(categories)) {
            const option = document.createElement('option');
            option.value = String(category.id);
            option.textContent = category.name;
            options.push(option);
        }
        fields.category.replaceChildren(...options);
        if (categories.length === 0) {
            saveButton.disabled = true;
            errorEl.textContent = 'Kategorioita ei ole vielä. Lisää ensin kategoria reseptilistan alta.';
        }
    }

    // Live preview: the recipe as its public page will show it, updated on every
    // change. Wide screens show it beside the form; narrow ones switch with tabs.
    const live = {
        layout: document.getElementById('editLayout'),
        address: document.getElementById('previewAddress'),
        category: document.getElementById('previewCategory'),
        name: document.getElementById('previewName'),
        cover: document.getElementById('previewCover'),
        content: document.getElementById('previewContent'),
        editTab: document.getElementById('editTab'),
        previewTab: document.getElementById('previewTab'),
        queued: false,
    };

    function renderLiveCover() {
        const src = KasinBlocks.safeSrc(fields.image.value.trim());
        if (live.cover.dataset.src === src) {
            return;
        }
        live.cover.dataset.src = src;
        if (!src) {
            live.cover.replaceChildren();
            live.cover.hidden = true;
            return;
        }
        const img = document.createElement('img');
        img.src = src;
        img.alt = '';
        img.addEventListener('error', function () {
            live.cover.hidden = true;
        });
        live.cover.replaceChildren(img);
        live.cover.hidden = false;
    }

    function renderLiveContent() {
        const blocks = KasinBlocks.parse(editor.serialize());
        if (blocks.length === 0) {
            const empty = document.createElement('p');
            empty.className = 'preview-empty';
            empty.textContent = 'Kuvaus näkyy tässä, kun lisäät lohkoja.';
            live.content.replaceChildren(empty);
            return;
        }
        // Images already shown are moved into the new content instead of being
        // created again, so they do not flicker while text is typed.
        const shown = new Map();
        for (const img of live.content.querySelectorAll('img')) {
            const src = img.getAttribute('src');
            if (!shown.has(src)) {
                shown.set(src, []);
            }
            shown.get(src).push(img);
        }
        const fresh = document.createElement('div');
        KasinBlocks.render(blocks, fresh);
        for (const img of fresh.querySelectorAll('img')) {
            const reusable = shown.get(img.getAttribute('src'));
            if (reusable && reusable.length > 0) {
                const old = reusable.shift();
                old.alt = img.alt;
                old.style.cssText = img.style.cssText;  // The crop may have changed.
                img.replaceWith(old);
            }
        }
        live.content.replaceChildren(...fresh.childNodes);
    }

    function renderLive() {
        live.queued = false;
        const option = fields.category.selectedOptions[0];
        live.category.textContent = option && option.value ? option.textContent : 'Kategoria';
        const name = fields.name.value.trim();
        live.name.textContent = name || 'Reseptin nimi';
        live.name.classList.toggle('is-placeholder', !name);
        live.address.textContent = '/reseptit/' + (slugify(fields.slug.value) || '…');
        renderLiveCover();
        renderLiveContent();
    }

    // Several changes can arrive at once (a re-render, a pick); draw once after them.
    function scheduleLive() {
        if (!live.queued) {
            live.queued = true;
            queueMicrotask(renderLive);
        }
    }
    form.addEventListener('input', scheduleLive);
    form.addEventListener('change', scheduleLive);

    function showView(view) {
        live.layout.dataset.view = view;
        for (const tab of [live.editTab, live.previewTab]) {
            const selected = (tab === live.previewTab) === (view === 'preview');
            tab.setAttribute('aria-selected', String(selected));
            tab.tabIndex = selected ? 0 : -1;
        }
        if (view === 'preview') {
            renderLive();
        }
    }
    live.editTab.addEventListener('click', function () {
        showView('edit');
    });
    live.previewTab.addEventListener('click', function () {
        showView('preview');
    });
    for (const tab of [live.editTab, live.previewTab]) {
        tab.addEventListener('keydown', function (event) {
            if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
                const next = tab === live.editTab ? live.previewTab : live.editTab;
                next.focus();
                next.click();
            }
        });
    }

    if (id) {
        title.textContent = 'Muokkaa reseptiä';
        document.title = 'Muokkaa reseptiä — Kotoa käsin';
        form.hidden = true;
    }

    async function loadForm() {
        try {
            await requireSession();
            const categories = await api('GET', '/api/categories');
            const recipe = id ? await api('GET', '/api/recipes/' + encodeURIComponent(id)) : null;
            fillCategories(categories);
            scheduleLive();
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
                scheduleLive();
                form.hidden = false;
            }
            fields.name.focus();
        } catch (error) {
            if (error.status === 401) {
                return;
            }
            form.hidden = false;
            saveButton.disabled = true;
            errorEl.textContent = error.status === 404
                ? 'Reseptiä ei löytynyt. Se on ehkä poistettu.'
                : messageFor(error, 'Reseptin lataaminen epäonnistui.');
        }
    }
    loadForm();

    function saveErrorMessage(error, slug) {
        if (error.status === 400) {
            return 'Tarkista kentät: nimi, osoite ja kategoria ovat pakollisia. Nimi saa olla enintään ' +
                '200 merkkiä ja kuvan osoite 500 merkkiä. Jos kategoria poistettiin juuri, lataa sivu uudelleen.';
        }
        if (error.status === 409) {
            return 'Osoite /reseptit/' + slug + ' on jo toisen reseptin käytössä. Valitse toinen osoite.';
        }
        if (error.status === 413) {
            return 'Resepti on liian pitkä tallennettavaksi. Lyhennä kuvausta.';
        }
        if (error.status === 404) {
            return 'Reseptiä ei löytynyt. Se on ehkä poistettu.';
        }
        return messageFor(error, 'Tallentaminen epäonnistui. Yritä uudelleen.');
    }

    form.addEventListener('submit', async function (event) {
        event.preventDefault();
        errorEl.textContent = '';
        const recipe = {
            name: fields.name.value.trim(),
            slug: slugify(fields.slug.value),
            category_id: Number(fields.category.value),
            image: fields.image.value.trim(),
            description: editor.serialize(),
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
        try {
            if (id) {
                await api('PUT', '/api/recipes/' + encodeURIComponent(id), recipe);
            } else {
                await api('POST', '/api/recipes', recipe);
            }
        } catch (error) {
            saveButton.disabled = false;
            if (error.status === 409) {
                fields.slug.focus();
            }
            if (error.status !== 401) {
                errorEl.textContent = saveErrorMessage(error, recipe.slug);
            }
            return;
        }
        window.location.assign(LIST_PAGE);
    });
}

// Login attempts page (Kirjautumiset).

const DAY_SEC = 24 * 60 * 60;
const REFRESH_MS = 30 * 1000;
const COUNTRY_NAMES = window.Intl && Intl.DisplayNames ? new Intl.DisplayNames(['fi'], { type: 'region' }) : null;

// The flag of a country as an emoji: its two letters as regional indicator symbols.
function flagEmoji(code) {
    const base = 0x1F1E6 - 'A'.charCodeAt(0);
    return String.fromCodePoint(base + code.charCodeAt(0), base + code.charCodeAt(1));
}

function countryName(code) {
    try {
        return COUNTRY_NAMES ? COUNTRY_NAMES.of(code) : code;
    } catch (e) {
        return code;
    }
}

// A country cell's contents: flag and Finnish name, or "Tuntematon" (private networks and unknown ranges).
function countryElement(code) {
    const cell = document.createElement('span');
    cell.className = 'logins-country';
    if (typeof code !== 'string' || !/^[A-Z]{2}$/.test(code)) {
        cell.textContent = 'Tuntematon';
        cell.classList.add('is-unknown');
        return cell;
    }
    const flag = document.createElement('span');
    flag.className = 'logins-flag';
    flag.setAttribute('aria-hidden', 'true');
    flag.textContent = flagEmoji(code);
    cell.append(flag, countryName(code));
    cell.title = code;
    return cell;
}

function formatTime(seconds) {
    return new Date(seconds * 1000).toLocaleString('fi-FI', {
        day: 'numeric',
        month: 'numeric',
        year: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
    });
}

// "3 min 12 s" or "45 s".
function formatRemaining(seconds) {
    const left = Math.max(0, seconds);
    const minutes = Math.floor(left / 60);
    return minutes > 0 ? minutes + ' min ' + (left % 60) + ' s' : left + ' s';
}

function cell(content) {
    const td = document.createElement('td');
    if (content instanceof Node) {
        td.appendChild(content);
    } else {
        td.textContent = content;
    }
    return td;
}

function row(cells) {
    const tr = document.createElement('tr');
    tr.append(...cells);
    return tr;
}

// Shows a table with its rows, or its "nothing here" note when there are none.
function fillTable(table, body, empty, rows) {
    body.replaceChildren(...rows);
    table.hidden = rows.length === 0;
    empty.hidden = rows.length > 0;
}

// The numbers above the tables, from the latest attempts and current blocks.
function loginSummary(data) {
    const since = data.now - DAY_SEC;
    let failed = 0;
    const addresses = new Set();
    for (const attempt of data.report.attempts) {
        if (!attempt.succeeded && attempt.at >= since) {
            failed++;
            addresses.add(attempt.ip);
        }
    }
    return { failed, addresses: addresses.size, blocked: data.blocked.length };
}

function initLoginsPage() {
    const status = document.getElementById('loginsStatus');
    const errorEl = document.getElementById('loginsError');
    const content = document.getElementById('loginsContent');
    const showSucceeded = document.getElementById('showSucceeded');
    const refreshButton = document.getElementById('loginsRefresh');
    const elements = {
        failedDay: document.getElementById('loginsFailedDay'),
        addressesDay: document.getElementById('loginsAddressesDay'),
        blockedNow: document.getElementById('loginsBlockedNow'),
        blockedTable: document.getElementById('blockedTable'),
        blockedRows: document.getElementById('blockedRows'),
        blockedEmpty: document.getElementById('blockedEmpty'),
        attemptsTitle: document.getElementById('attemptsTitle'),
        attemptsTable: document.getElementById('attemptsTable'),
        attemptsRows: document.getElementById('attemptsRows'),
        attemptsEmpty: document.getElementById('attemptsEmpty'),
        clientsTable: document.getElementById('clientsTable'),
        clientsRows: document.getElementById('clientsRows'),
        clientsEmpty: document.getElementById('clientsEmpty'),
    };
    let latest = null;
    setupLogout();

    async function unblock(client, button) {
        if (!window.confirm('Poistetaanko osoitteen ' + client.ip + ' esto?')) {
            return;
        }
        errorEl.textContent = '';
        button.disabled = true;
        try {
            await api('DELETE', '/api/login-blocks/' + encodeURIComponent(client.id));
        } catch (error) {
            button.disabled = false;
            if (error.status !== 401) {
                errorEl.textContent = messageFor(error, 'Eston poistaminen epäonnistui.');
            }
            return;
        }
        await load();
    }

    function blockedRow(block, clientsByIp) {
        const client = clientsByIp.get(block.ip);
        const until = document.createElement('span');
        until.textContent = formatTime(block.until) + ' (' + formatRemaining(block.until - latest.now) + ')';
        const actions = document.createElement('span');
        if (client) {
            const button = createCategoryButton('Poista esto');
            button.addEventListener('click', function () {
                unblock(client, button);
            });
            actions.appendChild(button);
        }
        const failures = block.failures + (block.banned ? ' (estetty)' : ' (odottaa)');
        return row([cell(block.ip), cell(countryElement(client ? client.country : null)), cell(failures),
            cell(until), cell(actions)]);
    }

    function attemptRow(attempt) {
        const agent = document.createElement('span');
        agent.className = 'logins-agent';
        agent.textContent = attempt.user_agent || '–';
        agent.title = attempt.user_agent;
        const name = document.createElement('span');
        name.textContent = attempt.username === '' ? '(tyhjä)' : attempt.username;
        if (attempt.succeeded) {
            const badge = document.createElement('span');
            badge.className = 'logins-badge';
            badge.textContent = 'Onnistui';
            name.append(' ', badge);
        }
        const tr = row([cell(formatTime(attempt.at)), cell(attempt.ip), cell(countryElement(attempt.country)),
            cell(name), cell(agent)]);
        if (attempt.succeeded) {
            tr.className = 'is-success';
        }
        return tr;
    }

    function clientRow(client) {
        return row([cell(client.ip), cell(countryElement(client.country)), cell(String(client.failures)),
            cell(String(client.successes)), cell(formatTime(client.last_seen)), cell(formatTime(client.first_seen))]);
    }

    function render() {
        const data = latest;
        const summary = loginSummary(data);
        elements.failedDay.textContent = String(summary.failed);
        elements.addressesDay.textContent = String(summary.addresses);
        elements.blockedNow.textContent = String(summary.blocked);

        const clientsByIp = new Map();
        for (const client of data.report.clients) {
            clientsByIp.set(client.ip, client);
        }
        const blockedRows = [];
        for (const block of data.blocked) {
            blockedRows.push(blockedRow(block, clientsByIp));
        }
        fillTable(elements.blockedTable, elements.blockedRows, elements.blockedEmpty, blockedRows);

        const attemptRows = [];
        for (const attempt of data.report.attempts) {
            if (showSucceeded.checked || !attempt.succeeded) {
                attemptRows.push(attemptRow(attempt));
            }
        }
        elements.attemptsTitle.textContent = showSucceeded.checked ? 'Kirjautumisyritykset' : 'Epäonnistuneet yritykset';
        elements.attemptsEmpty.textContent = showSucceeded.checked
            ? 'Ei kirjautumisyrityksiä.'
            : 'Ei epäonnistuneita kirjautumisyrityksiä.';
        fillTable(elements.attemptsTable, elements.attemptsRows, elements.attemptsEmpty, attemptRows);

        const clientRows = [];
        for (const client of data.report.clients) {
            clientRows.push(clientRow(client));
        }
        fillTable(elements.clientsTable, elements.clientsRows, elements.clientsEmpty, clientRows);
    }

    async function load() {
        try {
            latest = await api('GET', '/api/login-attempts');
        } catch (error) {
            if (error.status !== 401) {
                status.hidden = false;
                status.textContent = messageFor(error, 'Kirjautumistietojen lataaminen epäonnistui.');
            }
            return;
        }
        status.hidden = true;
        content.hidden = false;
        render();
    }

    showSucceeded.addEventListener('change', function () {
        if (latest) {
            render();
        }
    });
    refreshButton.addEventListener('click', load);
    window.setInterval(function () {
        if (!document.hidden) {
            load();
        }
    }, REFRESH_MS);

    async function start() {
        await requireSession();
        await load();
    }
    start();
}

document.addEventListener('DOMContentLoaded', function () {
    const page = document.body.getAttribute('data-page');
    if (page === 'login') {
        initLoginPage();
    } else if (page === 'list') {
        initListPage();
    } else if (page === 'edit') {
        initEditPage();
    } else if (page === 'images') {
        KasinGallery.initPage();
    } else if (page === 'logins') {
        initLoginsPage();
    }
});
