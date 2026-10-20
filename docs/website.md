# Project website

The project website is a dependency-free static site prepared for GitHub Pages
at [hugorosario.github.io/BrickOS](https://hugorosario.github.io/BrickOS/).
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
appearance and interactions. The displayed version is the latest published
release. The workflow reads its tag from GitHub and passes it as `SITE_VERSION`,
because the sources' `versions.json` may already name the next release. A local
build without `SITE_VERSION` shows the version in `versions.json`. Download links
lead to GitHub's latest release page, keeping release notes and the correct package
available to visitors.

## GitHub Pages

Select **GitHub Actions** as the publishing source in the repository's
**Settings → Pages**. The workflow in `.github/workflows/pages.yml` builds and
deploys the site after relevant changes are pushed to `main`. When a release is
published, it runs itself again on `main`, because the `github-pages` environment
deploys only from `main`. It can also be run manually from Actions. Only `dist/site/` is uploaded; repository sources, local
publication controls and firmware release ZIPs are not part of the website.

GitHub project sites inherit a custom domain configured on the account's
`hugorosario.github.io` Pages site. If that domain points to a different host,
the default project address redirects there even after a successful deployment.
Resolve the account-level Pages association or configure a dedicated project
subdomain and its DNS before treating the public URL as available.

The workflow uses read-only repository access for building. The deployment job
has the `pages: write` and `id-token: write` permissions required by
[GitHub's Pages workflow](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

The website is documentation and presentation only. This workflow does not
create firmware releases, tags or commits, and does not install anything on a device.
