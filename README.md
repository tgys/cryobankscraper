# ffscraper

C++ scraper for Fairfax Cryobank donor profile pages. It fetches donor URLs, extracts photos from each profile, and writes a JSON gallery you can browse in the Qt app.

## Build

Requires Qt 6 (Core, Gui, Network, Widgets, Concurrent) and CMake 3.20+.

```bash
cmake -S . -B build
cmake --build build
```

Binaries:

- `build/scrapeff` — default CLI scraper (Chromium-style HTTP, parallel workers)
- `build/scrape_requests` — same scraper with Python `requests`-compatible HTTP (single-threaded)
- `build/gallery_qt` — desktop gallery; runs a scrape on launch if `gallery_data.json` is missing

## How it scrapes

1. **Build a URL list** in the working directory:
   - Reads `base_urls.txt` (default: `https://fairfaxcryobank.com/search/donorprofile.aspx?number=`).
   - For Fairfax profiles, also harvests links from the “meet our newest donors” listing page.
   - Generates candidate URLs for donor IDs `0` … `9999` (override with `SCRAPEFF_FAIRFAX_MAX_ID`), trying both padded (`0428`) and unpadded (`428`) `number=` values.
   - Merges any extra URLs from `target_urls.txt`.
   - Writes the final list to `target_urls.txt`.

2. **Fetch each profile page** over HTTPS (retries, optional pacing via `SCRAPEFF_HTTP_SPACING_MS`).

3. **Parse HTML** with Gumbo: find images inside `div.main` → `div.foto` (or `#main` / `#foto`).

4. **Download each image** and verify it decodes as a real image. One gallery tile per donor profile ID is kept.

5. **Write outputs** to the working directory:
   - `gallery_data.json` — main gallery data (`page`, `src`, `did`, `profile_number`)
   - `gallery.html` — static HTML gallery
   - `image_urls.txt`, `fetch_failed.txt`, `profile_urls_before_image_extract.txt`
   - `childhood_photo_did_counts.json` — counts per `ChildhoodPhoto.ashx?did=` id

Donors with a real photo use `ChildhoodPhoto.ashx?did=…`. Donors without one show a shared placeholder (`search-temp-01.jpg`).

## Gallery

`gallery_qt` loads `gallery_data.json` (or scrapes first). By default it shows only donors **with profile pics** (real `ChildhoodPhoto` URLs), sorted by donor id low → high. Use **Filter → All donors** to include placeholders.

## Useful environment variables

| Variable | Purpose |
|---|---|
| `SCRAPEFF_THREADS` | Parallel workers for `scrapeff` (default 4) |
| `SCRAPEFF_FAIRFAX_MAX_ID` | Upper bound for brute-force donor IDs (default 10000) |
| `SCRAPEFF_FAIRFAX_SKIP_BRUTE_IDS=1` | Only scrape URLs found on the listing page |
| `SCRAPEFF_TARGET_URLS_ONLY=1` | Use `target_urls.txt` only, skip generation |
| `SCRAPEFF_HTTP_PROFILE=urllib` | Python-requests-style client (or use `scrape_requests`) |

Run the CLI from the directory where you want output files written (`target_urls.txt`, `gallery_data.json`, etc.).
