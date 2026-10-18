# Launcher part 5 — HD texture pack download — Design

Date: 2026-10-18. Approved in chat by the user ("Todos los idiomas", download from the launcher with credits,
progress bar, a check of whether the pack is there or what is missing).

## 1. The pack (analysed 2026-10-18)

- Source: https://github.com/Lin-zl522/Patapon-3-HD-Texture-Pack, branch `main` (last commit 68ef73e,
  2026-08-26). No releases, no licence file, no Git LFS. 3053 PNG + `textures.ini` (PPSSPP format:
  `hash = xxh64`, `ignoreAddress`, `reduceHash`), 856 MB with every language (`ui/language/*`, ~280 MB).
- Junk in the repository the launcher skips: `new/`, `_custom_textures/` (the player's own folder), every
  `desktop.ini`, `.idea/`, `.gitignore`, `textures.ini generator.py`, `.nomedia`.
- Paths contain spaces (`monsters/dragon puppies/…`).
- The branch zip (`codeload…/zip/refs/heads/main`) has no Content-Length and no Range: no real progress and
  no resume. `raw.githubusercontent.com` answers with Content-Length and does not count against the API limit
  (60 calls / hour unauthenticated); the tree API lists every file with its size and git blob SHA-1.
- The user's own copy (`DECO/PPSSPP/memstick/PSP/TEXTURES/INFN00001`, 2026-10-10) has an identical
  `textures.ini` but one 0-byte PNG (`effects/hero_skill/buzzcrave/00000000f8a96d3151a03810.png`, 60 878 bytes
  upstream): checking by size and hash matters.

## 2. Decisions

1. **All languages**, the whole pack (user's choice). Files kept: `*.png`, `textures.ini`, `README.md`.
2. **Download file by file**: one API call for the latest commit of `main`, one for its recursive tree;
   every file from `raw.githubusercontent.com/<repo>/<commit>/<path>` (pinned to that commit, path
   percent-encoded), 4 at a time, 3 attempts each, checked against its git blob SHA-1
   (`sha1("blob <size>\0" + bytes)`), written as `<file>.part` then renamed. `textures.ini` is written last,
   so a cut download leaves the pack inactive instead of half active.
3. **Destination**: the folder `settings.ini [display] texture_pack` names (default `textures/INFN00001`,
   relative to the portable root) — what the game already reads (`PSPRECOMP_TEXTURES`). If that folder or a
   parent under the root is a symlink / junction (the developer `--textures` link of make-portable), the
   launcher reports "linked" and never writes there.
4. **Manifest** `<pack>/.p3-launcher-textures.json`: `{commit, date, files: [{path, size, sha}]}` = the commit
   of the last complete download (empty before one finished) and every file known good. Written after every
   download, also a cancelled or failed one, so the next one fetches only the rest.
5. **States** (computed in Rust): `linked`, `none` (nothing on disk), `partial` (some files missing or
   different), `update` (the manifest's commit is older than `main` and files changed), `complete`, `unknown`
   (offline and no manifest, but a `textures.ini` is there). The quick check (Options label, the screen's first
   line) compares sizes and, with a manifest, the recorded SHA-1; "Check every file" re-hashes everything
   (also done automatically when files exist but no manifest says who wrote them — a hand-copied pack).
6. **Updates** fetch only changed / missing files and delete only files a previous launcher download wrote that
   the repository dropped. Nothing else in the folder is touched (`_custom_textures/` stays the player's).
7. **Disk space**: refuse to start when free space < missing bytes + 64 MB (message with both numbers).
8. **Credits**: the screen lists the README's "Textures Remastered by" names and roles, the repository URL and
   a sentence that the launcher downloads the pack from its original repository and does not redistribute it.
   An entry opens the GitHub page.
9. **UI**: Options › Gráficos gains an entry "Texturas HD: <short state>" (quick offline check) that opens a
   new screen **Texturas HD** with the online status, Download / Download what is missing / Update (with the
   size), "Check every file", Retry (offline), the GitHub page, Back; while downloading: a progress bar with
   MB and file counts (or "Checking files… n of m") and Cancel (Back also cancels). The existing
   `display.hd_textures` switch stays as it is.

## 3. Out of scope

Importing a PPSSPP copy (not requested; "Check every file" after a manual copy covers it), picking languages,
GitHub tokens, mirrors, bundling the pack in the portable package.
