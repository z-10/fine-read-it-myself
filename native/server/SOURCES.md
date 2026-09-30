# Site plugin format

Plugins are declarative YAML, loaded from compiled resources, then `<data>/plugins`, then the optional
`user_sources_dir`. Files are sorted within each folder; a later definition with the same `id` replaces
the earlier one. Restart after changing a user plugin. Rebuild after changing a built-in definition.
The Sites tab and Library discover all sites through `/api/sources`; neither contains a list of site names.

Required fields are `id`, `match`, `novel` and `chapter`. `name` defaults to `id`; `homepage` is the site link.

```yaml
id: example
name: Example
homepage: https://example.org
match:
  - '^https?://example\.org/books/(?P<slug>[^/?#]+)(?:[/?#]|$)'
novel:
  key: '{slug}'
  url: 'https://example.org/books/{slug}'
  title: 'h1'
  author: '.author'
  cover: ['.cover img@data-src', '.cover img@src']
  description: '.summary'
  chapters:
    item: '.chapter-list a'
    url: '@href'
    title: ''
chapter:
  title: 'h1'
  content: '#content'
  drop: ['script', 'style', '.advertisement']
fetch:
  delay_seconds: 1.5
  login: true
```

`match` patterns accept Python-style named capture groups. Captures expand `{name}` in the canonical
`novel.url` and optional `novel.key`. Identity is `id:key`, defaulting to `id:canonical_url`.
Only include book identity in the key, never a chapter number or a decorative slug for a numeric ID.
The same novel pasted as a chapter URL must resolve to the same URL and key. Existing records with an
older key are matched by source and canonical URL when added again, preserving their chapters and audio.

Field selectors use CSS; append `@attribute` to read an attribute, or use `@attribute` alone to read the
current chapter-list item. An empty selector reads the current item's text. An array tries selectors in
order and uses the first nonempty value. Attribute URL fields resolve against the novel page, including
`data-src` for lazy covers. Cover URLs read from metadata also resolve against the page.

For values embedded in text, use an object such as:

```yaml
count:
  select: '.header-stats'
  regex: '([0-9][0-9,]*)\s+[Cc]hapters'
```

The first regex capture is the value. Numbered sites can replace the `chapters.item` list with `count`,
`url: 'https://example.org/books/{slug}/chapter-{n}'` and `title: 'Chapter {n}'`.
The count must be numeric (commas allowed), at most 100,000. Missing/invalid counts fail visibly.
Optional `chapters.vars` selectors supply additional URL template variables; `{novel_url}` is also available.
Titles generated from counts remain generic until the chapter is fetched.

For embedded JSON, `novel.data` or `chapter.data` specifies the marker before the page's JSON object.
Selectors beginning with `$` walk the JSON, as in the Wuxiaworld definition. `chapter.locked` resolving
to `true` marks a paid preview and prevents it from becoming a full narration.

`chapter.content` is the CSS selector for the text container. `drop` removes matching descendants;
`drop_css_hidden: true` removes classes declared `display: none` in page style blocks.
`drop_tag_pattern` optionally removes elements whose tag names fully match a regex, such as NovelFire's
injected custom tags. Nested matches are removed together. Script/style/template text is never read.

`fetch.login` exposes an optional cookie field. `fetch.delay_seconds` is the default interval between
requests to the host. Users can override delays (0–60 seconds) and cookies in Sites without changing
YAML. The API saves one plugin at a time through `PUT /api/sources/{id}/config`:
`{"cookie":"session=...","delay_seconds":2}`. Omitted fields are preserved, `"***"` preserves a stored
cookie, `""` forgets it, and `null` resets the delay. Existing `site_logins` remain compatible;
delay overrides are stored in `site_delays` in settings.json.

Run the native regression checks with:

```sh
cmake -S native -B native/build -DRM_BUILD_TESTS=ON
cmake --build native/build --target rm-source-tests
ctest --test-dir native/build --output-on-failure
```

These use synthetic HTML and a loopback HTTP fixture; they require no external sites, accounts or models.
