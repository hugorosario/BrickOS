# Project website

The project website is a dependency-free static site hosted at
[hugorosario.pt/BrickOS](https://hugorosario.pt/BrickOS/).
The GitHub Pages address
[hugorosario.github.io/BrickOS](https://hugorosario.github.io/BrickOS/) redirects
to the domain inherited from the account's existing Pages configuration.
Its source is in `site/`. It uses the curated real-device images already in
`docs/screenshots/`; the build copies only the captures referenced by the page,
without editing or resizing them. Most images show Stock++.

## Build and preview

With Node.js available, run:

```sh
node tools/build-site.mjs
```

The generated site is in `dist/site/`, which is ignored by Git. Serve that folder
with any static HTTP server. All local asset URLs are relative, so the site works
under the GitHub Pages `/BrickOS/` project path and in a local preview. There are
no runtime dependencies, third-party fonts, trackers or API credentials.
The workflow passes the actual Pages base URL through `SITE_URL` for canonical
links, social metadata and the 404-page home link. Local builds default to the
GitHub Pages address; `SITE_URL` can override it when needed.

Edit `site/index.html`, `site/styles.css` and `site/app.js` to update the content,
appearance and interactions. The displayed version is read from `versions.json`
during the build. Download links lead to GitHub's latest release page, keeping
release notes and the correct package available to visitors.

## GitHub Pages

Select **GitHub Actions** as the publishing source in the repository's
**Settings → Pages**. The workflow in `.github/workflows/pages.yml` builds and
deploys the site after relevant changes are pushed to `main`. It can also be run
manually from Actions. Only `dist/site/` is uploaded; repository sources, local
publication controls and firmware release ZIPs are not part of the website.

The workflow uses read-only repository access for building. The deployment job
has the `pages: write` and `id-token: write` permissions required by
[GitHub's Pages workflow](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

The website is documentation and presentation only. This workflow does not
create firmware releases, tags or commits, and does not install anything on a device.
