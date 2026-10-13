-- Starter content for a new site: two categories and the original recipes.

INSERT INTO categories (name) VALUES ('Kosmetiikka'), ('Ruoanlaitto');

INSERT INTO recipes (category_id, name, image, description)
SELECT (SELECT id FROM categories WHERE name = 'Ruoanlaitto'), column1, column2, column3
FROM (VALUES
    ('Laventelisaippua', '/assets/recipes/laventelisaippua.jpg',
     'Rauhoittava käsintehty saippua kuivalle iholle.'),
    ('Yrttivoide kuivalle iholle', '/assets/recipes/yrttivoide.jpg',
     'Puutarhan yrteistä uutettu, hoitava perusvoide.'),
    ('Kotitekoinen marjahillo', '/assets/recipes/marjahillo.jpg',
     'Kesän marjasadosta keitetty, vähäsokerinen hillo.'),
    ('Ruusuvesitoner', '/assets/recipes/ruusuvesitoner.jpg',
     'Virkistävä kasvovesi tuoreista ruusunlehdistä.'),
    ('Hapanjuurileipä', '/assets/recipes/hapanjuurileipa.jpg',
     'Rapeakuorinen leipä omalla juuritaikinalla.'),
    ('Kalendulavoide', '/assets/recipes/kalendulavoide.jpg',
     'Ärsytystä lievittävä voide kehäkukan uutteesta.'),
    ('Omena-kanelihillo', '/assets/recipes/omenahillo.jpg',
     'Syksyn omenista, kanelista ja tähtianiksesta.'),
    ('Yrttisuola', '/assets/recipes/yrttisuola.jpg',
     'Kuivatuista yrteistä sekoitettu mauste-suola.'));

INSERT INTO recipes (category_id, name, image, description) VALUES (
    (SELECT id FROM categories WHERE name = 'Kosmetiikka'),
    'Mehiläisvahahuulivoide',
    '/assets/recipes/huulivoide.jpg',
    'Pehmentävä huulivoide mehiläisvahasta, sheavoista ja kylmäpuristetusta'
    || ' auringonkukkaöljystä. Valmistuu vartissa ja riittää koko talveksi.'
);
