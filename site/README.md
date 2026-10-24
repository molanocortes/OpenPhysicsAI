# The website

The project's public front door, with one job: someone arrives, sees what the lab computes, and leaves with it
downloaded. Live since 2026-10-07 at <https://openphysicsai.org> and <https://openphysicsai.com> (the owner's
decision; see the end of this page for how it is published). Up: [docs/map/INDEX.md](../docs/map/INDEX.md).

## Build and look at it

```bash
python3 tools/site_build.py                 # writes build/site/, or lists what is wrong and writes nothing
python3 tools/site_build.py --brand "NAME"  # the same site under another name
```

On the development machine the output is registered with the local router as
<http://openphysicsai-site.localhost:8080>; anywhere else, any static file server on `build/site/` shows it.

| File | What it is |
|---|---|
| `index.html` | the page, a template: `%BRAND%` and `%REPO%` are filled in from `site.json` |
| `style.css` | the studio look of [docs/app/DESIGN.md](../docs/app/DESIGN.md): its tokens, its type, the house colour map as the only gradient; light only |
| `site.js` | names the visitor's system on the download button, adds copy buttons, plays a film only while it is on screen; makes no network request and stores nothing |
| `site.json` | the name, the repository, the branch the links must exist on, and the films copied in from `docs/media` and `flags/` |
| `media/posters/` | one still per film, so the page paints from about 190 kB before any film loads; made with `tools/site_poster.swift` |
| `media/films/` | the films as small H.264 videos (4.6 MB for all fifteen, against 45 MB of GIF and WebP), made with `tools/site_video.swift`; each plays only while on screen |
| `media/card.jpg`, `apple-touch-icon.png` | the share card shown when a link to the site is posted, and the iPhone home-screen icon; made with `tools/site_card.swift` |
| `robots.txt`, `sitemap.xml`, `llms.txt` | for search engines and for AI agents that visit the site; templates like the page |
| `_worker.js`, `_routes.json` | Cloudflare Pages answers a byte-range request with the whole file, and Safari (every iPhone) plays no video unless ranges get 206 Partial Content; this worker, routed to `media/films/` only, slices the film. Everything else is served by Pages directly, with `_headers` |

## The lab page (`/lab/`)

A second page, added 2026-10-07 on the owner's word, where a visitor runs the lab itself: the solvers and the renderer
compiled to WebAssembly ([src/web/README.md](../src/web/README.md)) and run in a background worker on the visitor's own
phone or computer. Nothing is installed, and nothing is sent anywhere: the scenario, the run and the result stay in
the tab's memory.

| File | What it is |
|---|---|
| `lab/index.html`, `lab/lab.css` | the page: a row of scenarios, the viewer (a film of the result that plays, turns under a finger or the mouse, and zooms), the scenario's own controls, the run, and the solver's record |
| `lab/lab.js` | asks the worker for runs and pictures and shows them: the film of the current view kept within a memory budget, quick half-size pictures while the view turns, a sharp one when paused |
| `lab/worker.js` | the lab's module in a worker: run, open, view, render, one message at a time; Stop ends the worker and starts another |
| `lab/wasi.js` | the thirteen WASI calls the module makes, over files kept in memory, written for the lab (a file grows by doubling, not by a copy per write); also run by `tools/web_run.mjs` under Node |
| `lab/catalogue.json` | the scenarios: each names its source in `examples/lab/`, what is changed so that a phone runs it in seconds, in plain words for the page, and its first view |
| `lab/NOTICES.txt` | the notices of the libraries linked into the module (wasi-libc's musl and cloudlibc parts) |

The module, the scenarios and their card pictures are not in `site/`: `python3 tools/web_build.py --check` writes them
to `build/web/site/lab/` from the branch the site describes, after running every scenario in WebAssembly and natively
and comparing the two, and `tools/site_build.py` copies them in only if they were built and checked from that very
commit with the current glue and catalogue. The card pictures are drawn by the module itself from its own results, so
a card shows what the page computes, not the app's full-size film.

The security policy in `_headers` allows exactly what the page needs beyond the rest of the site: fetching the site's
own files (`connect-src 'self'`) and compiling WebAssembly (`'wasm-unsafe-eval'`, which permits no JavaScript `eval`).

A poster is a frame from the middle of its film, because the first frame of a simulation is often the empty state
before anything happens. The videos, posters and card are committed, so the site builds anywhere; remaking them
needs macOS:

```bash
swiftc -O tools/site_poster.swift -o /tmp/site_poster
swiftc -O tools/site_video.swift -o /tmp/site_video
/tmp/site_poster docs/media/tile-motor.gif site/media/posters/tile-motor.jpg 0.6
/tmp/site_video docs/media/tile-motor.gif site/media/films/tile-motor.mp4 450      # kbps; 1300 for the hero
```

## The rules it keeps, and the build checks them

- **Every claim already stands in the public README.** The page says nothing about the lab that the README on the
  public branch does not say; when the README changes, the page follows it, not the other way round.
- **Every link into the repository exists on the branch the site describes** (`publish_ref` in `site.json`). The
  build checks each one against that branch.
- **Nothing is sold and nobody is asked to sign up**: no prices, no payment, no newsletter, no form, no e-mail
  address, no request for stars. Questions go to GitHub issues.
- **Nothing is loaded from another site and nothing is tracked**: no web fonts (the system's own font is SF Pro on
  Apple devices), no scripts, styles or images from elsewhere, no cookies, no analytics. The footer says so, and the
  build refuses a page that would make it untrue.
- **The internal program name is not used as a name** on the public site.

Patterns that must not appear even in this repository are read from a private file outside it
(`~/.openphysicsai/site-never-public.txt`) when it is present.

## Publishing

The site is served by **Cloudflare Pages**, project `openphysicsai` in the owner's Cloudflare account (also reachable at
<https://openphysicsai.pages.dev>), deployed by direct upload. Both domains are custom domains of that project:
Cloudflare holds their DNS (the apex CNAME to the project is flattened) and their certificates, and plain http is
redirected to https. The www names are attached too. A redirect rule in the .org zone (Rules, "Redirect from WWW
to root") sends www.openphysicsai.org to openphysicsai.org, and one in the .com zone sends both openphysicsai.com
and www.openphysicsai.com to openphysicsai.org, all with a 301 that keeps the path and the query string, so the
site has one address. `_headers` sets the security policy that makes the footer's promise binding in the browser:
nothing loaded from another site, no framing, no form posts. Cloudflare's Web Analytics stays off. The page names
openphysicsai.org as its canonical address (`url` in `site.json`).

To publish a change:

```bash
python3 tools/site_build.py                                   # writes and checks build/site/
cd build/site && zip -q -r -X ../openphysicsai-site.zip . -x .site-build
```

then, in the Cloudflare dashboard, Workers & Pages, `openphysicsai`, **Create deployment**, choose **Preview** with a
name first, check the preview address it gives, and deploy the same zip to **Production**. The zip is about 5 MB, so
a session's browser tool (10 MB at most) can do the upload itself. A muted video in a hidden browser tab neither
loads nor plays, so a check of the films needs a visible tab or a person.

What to watch afterwards, none of it collected by the page: download counts of the release assets once packaged
builds exist (the GitHub API keeps them), stars and forks, and the repository's traffic (GitHub keeps only 14 days,
so it has to be copied out weekly).
