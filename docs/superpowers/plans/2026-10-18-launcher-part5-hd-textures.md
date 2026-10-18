# Launcher part 5 — HD texture pack download — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Execution chosen by the user:** a NEW session runs this plan, like part 4. Each of Tasks 1–3 is a brief for
> **DeepSeek v4.1 flash at max effort** (`python ~/.claude/skills/albanil-grok/grok-build.py <brief> --repo
> C:/Users/torso/Documents/deco --model "deepseek/deepseek-flash#max" --timeout 3600 --msg "<no-commit message>"`);
> Claude reads every diff, re-runs every gate, commits and decides; Task 4 is Claude's. Briefs live in
> `DECO/build/briefs_p5/` (same template as `DECO/build/briefs_p4/T4.md`: plan path, line range, "verbatim",
> no commit, gates, report). **Before every run: `Get-Process opencode` must be empty** (the user's own
> OpenCode TUI and its `serve --service` share the local DB; on 2026-10-18 a run hung 5 h with that open — ask
> the user to close it, never kill it yourself). One OpenCode session at a time; arm a Monitor that reports
> every file change and "STALL" after 20 minutes without one (`DECO/build/briefs_p4` session shows the loop).

**Goal:** The launcher downloads the community HD texture pack from its GitHub repository into the game's
texture folder, with a progress bar, credits, and a check of what is installed, missing or out of date.

**Architecture:** A Rust module `textures.rs` (launcher) lists the pack at the latest commit of `main` (two
GitHub API calls), compares it with the folder `settings.ini [display] texture_pack` names (sizes, plus the
SHA-1 recorded in a manifest, or a full re-hash), and downloads the missing files from
`raw.githubusercontent.com` four at a time, each checked against its git blob SHA-1. Tauri commands expose
status / download / cancel / open page; a new React screen shows it, reached from Options › Gráficos.

**Tech Stack:** Rust 1.96 + Tauri 2 (launcher), new crates `sha1 0.10`, `ureq 2` (rustls TLS), `fs2 0.4`;
React 19 + TSX + Vitest.

**Spec:** `docs/superpowers/specs/2026-10-18-launcher-part5-hd-textures-design.md` (read it first).

## Global Constraints

- Source, fixed: repository `Lin-zl522/Patapon-3-HD-Texture-Pack`, branch `main`; page
  `https://github.com/Lin-zl522/Patapon-3-HD-Texture-Pack`. The pack has no licence: never add it (or any of its
  files) to a repository or to the portable package; the launcher only downloads it on the player's PC.
- API calls: `GET https://api.github.com/repos/<repo>/commits/main` and
  `GET https://api.github.com/repos/<repo>/git/trees/<commit>?recursive=1` (≤ 2 per check). Files:
  `https://raw.githubusercontent.com/<repo>/<commit>/<percent-encoded path>`. Every request sends a
  `User-Agent`. 4 parallel downloads, 3 attempts per file.
- Files kept: `*.png` (any case), `textures.ini`, `README.md`; skipped: `new/`, `_custom_textures/`, everything
  else (`desktop.ini`, `.idea/`, `.gitignore`, `*.py`, `.nomedia`). Paths from the network must be relative,
  `/`-separated, without `..`, `\` or `:`.
- Destination: `display.texture_pack` (default `textures/INFN00001`) resolved against the portable root
  (`settings::root_dir()`); absolute values are used as they are. A folder that is (or sits under) a symlink /
  junction below the root is never written.
- Every file is verified with its git blob SHA-1 = `sha1("blob <len>\0" + bytes)` before it replaces anything
  (`<file>.part` then rename). `textures.ini` is written after every other file.
- Manifest `<pack>/.p3-launcher-textures.json` = `{"commit", "date", "files": [{"path","size","sha"}]}`.
- States: `linked`, `none`, `partial`, `update`, `complete`, `unknown`. Progress event `texture_progress`
  `{phase: "verify" | "download", done_bytes, total_bytes, files_done, files_total}`.
- Strings shown to the player only in `lang/es.json` / `lang/en.json`; code, comments and commits in English.
- Commits: launcher on `main` with `git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com
  commit`, body ends with `Verified: …` and `Agent: …`, **no `Co-Authored-By`**. The launcher has a remote now
  (`origin` = github.com/Montick1560/p3-launcher): push only on the user's explicit OK. This plan and its spec
  are psprecomp docs (`patapon3-windows`); nothing in psprecomp code changes.

## Review Focus

1. A network cut / GitHub limit in the middle: the error is shown, every verified file stays, the next
   "Download what is missing" fetches only the rest (Task 2 test `a_failed_file_is_the_only_one_fetched_again`).
2. A damaged file with the right size (or a hand-copied pack with no manifest) must be found and re-fetched —
   the quick check cannot see it, "Check every file" and the first download over an unmanaged copy must
   (Task 1 `what_needs_a_download`, Task 2 `verify_repairs_a_corrupt_file_and_adopts_a_copied_pack`).
3. Hostile or odd paths from the network (`../`, `\`, spaces like `dragon puppies`) are refused or encoded
   (Task 1 `tree_keeps_pack_files_only`, `urls`).
4. The developer `textures` junction to the PPSSPP memstick (make-portable `--textures`) is never written
   (Task 1 `a_linked_folder_is_reported`, Task 2 `never_writes_through_a_link`).
5. An update deletes only files the launcher wrote that upstream dropped; the player's `_custom_textures/`
   and any other file stay (Task 2 `an_update_fetches_changes_and_drops_removed_files_only`).

## File map

launcher (`DECO/launcher`):
- `src-tauri/Cargo.toml` (T1 `sha1`, T2 `ureq`, `fs2`), `src-tauri/src/textures.rs` (new: T1 pure part, T2
  engine + HTTP), `src-tauri/src/lib.rs` (T1 `mod textures;`, T2 commands).
- `src/types.ts`, `src/api.ts`, `src/textures.ts` + `src/textures.test.ts` (new), `src/screens/Textures.tsx` +
  `src/screens/Textures.test.tsx` (new), `src/screens/Options.tsx`, `src/screens/Options.test.tsx`,
  `src/App.tsx`, `src/styles.css`, `lang/es.json`, `lang/en.json`, `README.md` (T3).

Gates (exact commands):
- Rust: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q 2>&1 | grep -E "^test result|FAILED|panicked"`
  and `cargo build -q 2>&1 | grep -c warning`
- UI: `cd /c/Users/torso/Documents/deco/launcher && npx vitest run 2>&1 | tail -5` and `npx tsc --noEmit`

Baseline (2026-10-18, launcher 3b2164f): cargo 73 passed + 1 ignored, Vitest 43/43, tsc clean.

---

### Task 1: Pack listing, hashes, what is missing, states (launcher Rust, pure)

**Files:**
- Create: `src-tauri/src/textures.rs`
- Modify: `src-tauri/Cargo.toml` (`sha1 = "0.10"` under `[dependencies]`), `src-tauri/src/lib.rs` (`mod textures;`
  in the alphabetical `mod` list, after `mod sfo;`)

**Interfaces:**
- Produces (all `pub` in `textures`): consts `REPO`, `BRANCH`, `PAGE_URL`, `MANIFEST`; structs `RemoteFile {path,
  size, sha}`, `Remote {commit, date, files}`, `Manifest {commit, date, files}`, `Status {state, online,
  local_date, remote_date, missing_files, missing_bytes, total_bytes, error}`; fns `wanted(&str) -> bool`,
  `safe_rel(&str) -> bool`, `encode_path(&str) -> String`, `raw_url(commit, path) -> String`,
  `commit_url() -> String`, `tree_url(commit) -> String`, `parse_commit(&str) -> Result<(String, String), String>`,
  `parse_tree(&str) -> Result<Vec<RemoteFile>, String>`, `blob_sha1(&[u8]) -> String`,
  `load_manifest(&Path) -> Option<Manifest>`, `save_manifest(&Path, &Manifest) -> io::Result<()>`,
  `pack_dir(root: &Path, setting: &str) -> PathBuf`, `is_linked(root: &Path, pack: &Path) -> bool`,
  `needed(pack, want: &[RemoteFile], manifest: Option<&Manifest>, deep: bool, tick: &mut dyn FnMut(usize)) -> Vec<RemoteFile>`,
  `status(root, pack, remote: Option<&Remote>, error: Option<String>) -> Status`.

- [ ] **Step 1: Failing tests.** Create `src-tauri/src/textures.rs` with only the test module below (plus
  `use` lines the compiler asks for), add `mod textures;` to `lib.rs` and `sha1 = "0.10"` to `Cargo.toml`.

```rust
#[cfg(test)]
mod tests {
    use super::*;

    fn rf(path: &str, data: &[u8]) -> RemoteFile {
        RemoteFile { path: path.into(), size: data.len() as u64, sha: blob_sha1(data) }
    }
    fn put(pack: &Path, path: &str, data: &[u8]) {
        let p = pack.join(path);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, data).unwrap();
    }

    #[test] fn git_blob_hashes() {
        // `git hash-object` of an empty file and of "hello\n".
        assert_eq!(blob_sha1(b""), "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391");
        assert_eq!(blob_sha1(b"hello\n"), "ce013625030ba8dba906f756967f9e9ca394464a");
    }

    #[test] fn tree_keeps_pack_files_only() {
        let json = r#"{"sha":"x","truncated":false,"tree":[
          {"path":"textures.ini","type":"blob","size":3,"sha":"a"},
          {"path":"README.md","type":"blob","size":1,"sha":"b"},
          {"path":"ui","type":"tree","sha":"c"},
          {"path":"ui/logo/0000000011112222aaaabbbb.png","type":"blob","size":5,"sha":"d"},
          {"path":"monsters/dragon puppies/00000000aaaabbbbccccdddd.PNG","type":"blob","size":6,"sha":"e"},
          {"path":"ui/desktop.ini","type":"blob","size":1,"sha":"f"},
          {"path":".idea/workspace.xml","type":"blob","size":1,"sha":"g"},
          {"path":"textures.ini generator.py","type":"blob","size":1,"sha":"h"},
          {"path":"new/x.png","type":"blob","size":1,"sha":"i"},
          {"path":"_custom_textures/mine.png","type":"blob","size":1,"sha":"j"},
          {"path":"../evil.png","type":"blob","size":1,"sha":"k"},
          {"path":"ui\\evil.png","type":"blob","size":1,"sha":"l"},
          {"path":"C:/evil.png","type":"blob","size":1,"sha":"m"}]}"#;
        let files = parse_tree(json).unwrap();
        let paths: Vec<&str> = files.iter().map(|f| f.path.as_str()).collect();
        assert_eq!(paths, ["README.md", "monsters/dragon puppies/00000000aaaabbbbccccdddd.PNG", "textures.ini",
                           "ui/logo/0000000011112222aaaabbbb.png"]);
        assert_eq!((files[2].size, files[2].sha.as_str()), (3, "a"));
        assert!(parse_tree(r#"{"truncated":true,"tree":[]}"#).is_err(), "a cut listing is refused");
        assert!(parse_tree("not json").is_err());
    }

    #[test] fn urls() {
        assert_eq!(encode_path("monsters/dragon puppies/a b.png"), "monsters/dragon%20puppies/a%20b.png");
        assert_eq!(raw_url("abc", "ui/x.png"),
                   "https://raw.githubusercontent.com/Lin-zl522/Patapon-3-HD-Texture-Pack/abc/ui/x.png");
        assert_eq!(commit_url(), "https://api.github.com/repos/Lin-zl522/Patapon-3-HD-Texture-Pack/commits/main");
        assert_eq!(tree_url("abc"),
                   "https://api.github.com/repos/Lin-zl522/Patapon-3-HD-Texture-Pack/git/trees/abc?recursive=1");
        let sha = "68ef73e".repeat(5) + "12345";
        let c = format!(r#"{{"sha":"{sha}","commit":{{"committer":{{"date":"2026-08-26T23:05:25Z"}}}}}}"#);
        assert_eq!(parse_commit(&c).unwrap(), (sha, "2026-08-26T23:05:25Z".to_string()));
        assert!(parse_commit(r#"{"sha":"../../x"}"#).is_err(), "the sha goes into URLs");
        assert!(parse_commit(r#"{"message":"API rate limit exceeded"}"#).is_err());
    }

    #[test] fn what_needs_a_download() {
        let d = tempfile::tempdir().unwrap();
        let (a, b, c) = (rf("ui/a.png", b"aaaa"), rf("ui/b.png", b"bbbb"), rf("c.png", b"cc"));
        put(d.path(), "ui/a.png", b"aaaa");
        put(d.path(), "ui/b.png", b"bxbb");  // same size, other content
        put(d.path(), "c.png", b"c");        // wrong size (like the 0-byte file found on 2026-10-18)
        let want = vec![a.clone(), b.clone(), c.clone()];
        let paths = |v: Vec<RemoteFile>| v.into_iter().map(|f| f.path).collect::<Vec<_>>();
        assert_eq!(paths(needed(d.path(), &want, None, false, &mut |_| {})), ["c.png"], "quick: size only");
        assert_eq!(paths(needed(d.path(), &want, None, true, &mut |_| {})), ["ui/b.png", "c.png"], "deep: content too");
        let m = Manifest { commit: "old".into(), date: String::new(),
                           files: vec![a.clone(), RemoteFile { sha: "other".into(), ..b.clone() }] };
        assert_eq!(paths(needed(d.path(), &want, Some(&m), false, &mut |_| {})), ["ui/b.png", "c.png"],
                   "with a manifest: a SHA-1 changed upstream needs the file again");
        let mut n = 0;
        needed(d.path(), &want, None, false, &mut |i| n = i);
        assert_eq!(n, 3, "tick reports every file checked");
    }

    #[test] fn states() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path();
        let pack = root.join("textures/INFN00001");
        let files = vec![rf("textures.ini", b"[options]\n"), rf("ui/a.png", b"aaaa")];
        let remote = Remote { commit: "c2".into(), date: "2026-08-26T23:05:25Z".into(), files: files.clone() };
        let st = |r: Option<&Remote>| status(root, &pack, r, None);
        assert_eq!(st(None).state, "none");
        let s = st(Some(&remote));
        assert_eq!((s.state, s.missing_files, s.missing_bytes, s.total_bytes, s.online), ("none", 2, 14, 14, true));
        assert_eq!(s.remote_date.as_deref(), Some("2026-08-26T23:05:25Z"));
        put(&pack, "ui/a.png", b"aaaa");
        assert_eq!(st(Some(&remote)).state, "partial");
        assert_eq!(st(None).state, "none", "offline, no manifest, no textures.ini");
        put(&pack, "textures.ini", b"[options]\n");
        assert_eq!(st(None).state, "unknown", "offline and no manifest: cannot tell");
        assert_eq!(st(Some(&remote)).state, "complete");
        save_manifest(&pack, &Manifest { commit: "c1".into(), date: "2026-03-08T15:23:59Z".into(),
            files: vec![files[0].clone(), RemoteFile { sha: "old".into(), ..files[1].clone() }] }).unwrap();
        let s = st(Some(&remote));
        assert_eq!((s.state, s.missing_files, s.local_date.as_deref()), ("update", 1, Some("2026-03-08T15:23:59Z")));
        assert_eq!(st(None).state, "complete", "offline: every file the manifest lists is there");
        let s = status(root, &pack, None, Some("HTTP 403".into()));
        assert_eq!((s.online, s.error.as_deref()), (false, Some("HTTP 403")));
    }

    #[cfg(windows)]
    #[test] fn a_linked_folder_is_reported() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path().join("root");
        let real = d.path().join("ppsspp_pack");
        std::fs::create_dir_all(&real).unwrap();
        std::fs::create_dir_all(&root).unwrap();
        let ok = std::process::Command::new("cmd").args(["/C", "mklink", "/J"])
            .arg(root.join("textures")).arg(&real).status().unwrap();
        assert!(ok.success());
        let pack = root.join("textures").join("INFN00001");
        assert!(is_linked(&root, &pack));
        assert_eq!(status(&root, &pack, None, None).state, "linked");
        assert!(!is_linked(&root, &root.join("other").join("INFN00001")));
    }

    #[test] fn pack_folder_from_the_setting() {
        assert_eq!(pack_dir(Path::new("C:/P"), "textures/INFN00001"), Path::new("C:/P").join("textures/INFN00001"));
        let abs = if cfg!(windows) { "D:/packs/INFN00001" } else { "/packs/INFN00001" };
        assert_eq!(pack_dir(Path::new("C:/P"), abs), PathBuf::from(abs));
    }
}
```
  Run the Rust gate → compile errors (functions missing).

- [ ] **Step 2: Implementation** — above the tests in `textures.rs`:

```rust
//! The community HD texture pack (Patapon-3-HD-Texture-Pack, PPSSPP format), downloaded from its GitHub
//! repository into the folder settings.ini [display] texture_pack names
//! (psprecomp docs/superpowers/specs/2026-10-18-launcher-part5-hd-textures-design.md). The pack has no
//! licence: the launcher never ships it, it fetches it from the original repository. Every file is checked
//! against its git blob SHA-1; MANIFEST records what the launcher wrote.
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};

pub const REPO: &str = "Lin-zl522/Patapon-3-HD-Texture-Pack";
pub const BRANCH: &str = "main";
pub const PAGE_URL: &str = "https://github.com/Lin-zl522/Patapon-3-HD-Texture-Pack";
pub const MANIFEST: &str = ".p3-launcher-textures.json";

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct RemoteFile { pub path: String, pub size: u64, pub sha: String }

/// The pack at one commit of the repository.
#[derive(Clone, Debug, PartialEq)]
pub struct Remote { pub commit: String, pub date: String, pub files: Vec<RemoteFile> }

/// What the launcher wrote: the commit of the last complete download (empty before one finished) and
/// every file known good on disk.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Manifest { pub commit: String, pub date: String, pub files: Vec<RemoteFile> }

/// "linked" | "none" | "partial" | "update" | "complete" | "unknown" (spec §2.5).
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Status {
    pub state: &'static str,
    pub online: bool,
    pub local_date: Option<String>,
    pub remote_date: Option<String>,
    pub missing_files: usize,
    pub missing_bytes: u64,
    pub total_bytes: u64,
    pub error: Option<String>,
}

/// The files of the repository that belong to the pack.
pub fn wanted(path: &str) -> bool {
    if path.starts_with("new/") || path.starts_with("_custom_textures/") { return false; }
    path == "textures.ini" || path == "README.md" || path.to_ascii_lowercase().ends_with(".png")
}

/// A relative `/` path without "..", "\" or ":" (the list comes from the network).
pub fn safe_rel(p: &str) -> bool {
    !p.is_empty() && !p.starts_with('/') && !p.contains('\\') && !p.contains(':')
        && p.split('/').all(|s| !s.is_empty() && s != "." && s != "..")
}

pub fn encode_path(p: &str) -> String {
    let mut out = String::new();
    for b in p.bytes() {
        if b.is_ascii_alphanumeric() || b"-._~/".contains(&b) { out.push(b as char); }
        else { out.push_str(&format!("%{b:02X}")); }
    }
    out
}

pub fn raw_url(commit: &str, path: &str) -> String {
    format!("https://raw.githubusercontent.com/{REPO}/{commit}/{}", encode_path(path))
}
pub fn commit_url() -> String { format!("https://api.github.com/repos/{REPO}/commits/{BRANCH}") }
pub fn tree_url(commit: &str) -> String { format!("https://api.github.com/repos/{REPO}/git/trees/{commit}?recursive=1") }

/// (sha, committer date) of GET commits/main; the sha must be 40 hex digits (it goes into URLs).
pub fn parse_commit(json: &str) -> Result<(String, String), String> {
    let v: serde_json::Value = serde_json::from_str(json).map_err(|e| format!("GitHub answer: {e}"))?;
    let sha = v["sha"].as_str().filter(|s| s.len() == 40 && s.bytes().all(|b| b.is_ascii_hexdigit()))
        .ok_or_else(|| format!("GitHub answer has no commit: {}", v["message"].as_str().unwrap_or("?")))?;
    Ok((sha.to_string(), v["commit"]["committer"]["date"].as_str().unwrap_or("").to_string()))
}

/// The pack files of a recursive git tree listing, sorted by path.
pub fn parse_tree(json: &str) -> Result<Vec<RemoteFile>, String> {
    let v: serde_json::Value = serde_json::from_str(json).map_err(|e| format!("GitHub answer: {e}"))?;
    if v["truncated"].as_bool() == Some(true) { return Err("GitHub returned a cut file list".into()); }
    let tree = v["tree"].as_array().ok_or("GitHub answer has no file list")?;
    let mut out = Vec::new();
    for e in tree {
        if e["type"].as_str() != Some("blob") { continue; }
        let (Some(path), Some(size), Some(sha)) = (e["path"].as_str(), e["size"].as_u64(), e["sha"].as_str()) else { continue };
        if wanted(path) && safe_rel(path) { out.push(RemoteFile { path: path.into(), size, sha: sha.into() }); }
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

/// git's blob id: SHA-1 of "blob <len>\0" + the bytes (what the tree listing gives per file).
pub fn blob_sha1(data: &[u8]) -> String {
    use sha1::{Digest, Sha1};
    let mut h = Sha1::new();
    h.update(format!("blob {}\0", data.len()).as_bytes());
    h.update(data);
    h.finalize().iter().map(|b| format!("{b:02x}")).collect()
}

pub fn load_manifest(pack: &Path) -> Option<Manifest> {
    serde_json::from_str(&std::fs::read_to_string(pack.join(MANIFEST)).ok()?).ok()
}

pub fn save_manifest(pack: &Path, m: &Manifest) -> io::Result<()> {
    let tmp = pack.join(format!("{MANIFEST}.tmp"));
    std::fs::write(&tmp, serde_json::to_string_pretty(m).map_err(io::Error::other)?)?;
    std::fs::rename(&tmp, pack.join(MANIFEST))
}

/// settings.ini display.texture_pack: relative to the portable root, or absolute.
pub fn pack_dir(root: &Path, setting: &str) -> PathBuf {
    let p = Path::new(setting);
    if p.is_absolute() { p.to_path_buf() } else { root.join(p) }
}

/// True when `pack`, or a folder between it and `root`, is a symlink or junction (managed elsewhere).
pub fn is_linked(root: &Path, pack: &Path) -> bool {
    if std::fs::read_link(pack).is_ok() { return true; }
    let mut p = pack.to_path_buf();
    while p.pop() && p.starts_with(root) && p.as_path() != root {
        if std::fs::read_link(&p).is_ok() { return true; }
    }
    false
}

/// The files of `want` to fetch: absent or another size; with `deep`, also another content; with a
/// manifest (and not deep), also a SHA-1 other than the one the launcher recorded. `tick(n)` after each file.
pub fn needed(pack: &Path, want: &[RemoteFile], manifest: Option<&Manifest>, deep: bool,
              tick: &mut dyn FnMut(usize)) -> Vec<RemoteFile> {
    let known: HashMap<&str, &str> = manifest
        .map(|m| m.files.iter().map(|f| (f.path.as_str(), f.sha.as_str())).collect())
        .unwrap_or_default();
    let mut out = Vec::new();
    for (i, f) in want.iter().enumerate() {
        let p = pack.join(&f.path);
        let ok = match std::fs::metadata(&p) {
            Ok(md) if md.is_file() && md.len() == f.size => {
                if deep { std::fs::read(&p).map(|d| blob_sha1(&d) == f.sha).unwrap_or(false) }
                else if manifest.is_some() { known.get(f.path.as_str()) == Some(&f.sha.as_str()) }
                else { true }
            }
            _ => false,
        };
        if !ok { out.push(f.clone()); }
        tick(i + 1);
    }
    out
}

/// What the screens show. `remote` = the repository listing when online; `error` = why it is missing.
pub fn status(root: &Path, pack: &Path, remote: Option<&Remote>, error: Option<String>) -> Status {
    let mut s = Status { state: "none", online: remote.is_some(), local_date: None,
                         remote_date: remote.map(|r| r.date.clone()), missing_files: 0, missing_bytes: 0,
                         total_bytes: 0, error };
    if is_linked(root, pack) { s.state = "linked"; return s; }
    let manifest = load_manifest(pack);
    s.local_date = manifest.as_ref().filter(|m| !m.commit.is_empty()).map(|m| m.date.clone());
    let Some(want) = remote.map(|r| &r.files).or(manifest.as_ref().map(|m| &m.files)) else {
        if pack.join("textures.ini").is_file() { s.state = "unknown"; }
        return s;
    };
    let need = needed(pack, want, manifest.as_ref(), false, &mut |_| {});
    s.missing_files = need.len();
    s.missing_bytes = need.iter().map(|f| f.size).sum();
    s.total_bytes = want.iter().map(|f| f.size).sum();
    let present = want.iter().any(|f| pack.join(&f.path).is_file());
    let older = match (remote, &manifest) {
        (Some(r), Some(m)) => !m.commit.is_empty() && m.commit != r.commit,
        _ => false,
    };
    s.state = if need.is_empty() { "complete" } else if !present { "none" } else if older { "update" } else { "partial" };
    s
}
```
  Expected warnings: `dead_code` for items Task 2 uses (`REPO` users, `raw_url`, …) — do not silence them.

- [ ] **Step 3: Gate.** Rust gate → **80 passed** (73 + 7; on Windows the junction test runs), 1 ignored. Report
  (no commit). Claude commits `feat(launcher): HD texture pack listing, hashes and install state`.

---

### Task 2: Download engine, HTTP, Tauri commands (launcher Rust)

**Files:**
- Modify: `src-tauri/src/textures.rs` (engine + HTTP above the tests; tests appended to the same `mod tests`),
  `src-tauri/Cargo.toml` (`ureq = "2"`, `fs2 = "0.4"`), `src-tauri/src/lib.rs` (state, 4 commands, handler list)

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces: `pub trait Fetch: Sync { fn get(&self, url: &str) -> Result<Vec<u8>, String>; }`, `pub struct Http`
  (`Http::new()`), `pub fn fetch_remote(&dyn Fetch) -> Result<Remote, String>`, `#[derive(Clone, Serialize)] pub
  struct Progress {phase, done_bytes, total_bytes, files_done, files_total}`, `pub fn download(root, pack, remote:
  &Remote, verify: bool, fetch: &dyn Fetch, cancel: &AtomicBool, progress: &(dyn Fn(Progress) + Sync)) ->
  Result<(), String>` (Err `"cancelled"` on cancel). Commands: `texture_status(online: bool) -> Result<Status,
  String>`, `download_textures(verify: bool) -> Result<(), String>` (emits `texture_progress`),
  `cancel_textures()`, `open_texture_page()`.

- [ ] **Step 1: Failing tests** — append inside `mod tests` of `textures.rs` (add
  `use std::collections::{HashMap, HashSet}; use std::sync::{atomic::AtomicBool, Mutex};` at the top of the
  test module):

```rust
    const C1: &str = "1111111111111111111111111111111111111111";
    const C2: &str = "2222222222222222222222222222222222222222";
    const DATA: &[(&str, &[u8])] = &[("textures.ini", b"[options]\n"), ("ui/a.png", b"aaaa"),
                                     ("monsters/dragon puppies/b.png", b"bbbbbb"), ("c.png", b"cc")];

    fn pack_of(data: &[(&str, &[u8])], commit: &str) -> Remote {
        let mut files: Vec<RemoteFile> = data.iter().map(|(p, d)| rf(p, d)).collect();
        files.sort_by(|a, b| a.path.cmp(&b.path));
        Remote { commit: commit.into(), date: "2026-08-26T23:05:25Z".into(), files }
    }

    /// In-memory repository: raw URL -> bytes; `broken` URLs fail like a cut connection; `calls` keeps the
    /// last path segment of every request.
    struct Fake { files: HashMap<String, Vec<u8>>, broken: Mutex<HashSet<String>>, calls: Mutex<Vec<String>> }
    impl Fake {
        fn new(remote: &Remote, data: &[(&str, &[u8])]) -> Fake {
            Fake { files: data.iter().map(|(p, d)| (raw_url(&remote.commit, p), d.to_vec())).collect(),
                   broken: Mutex::default(), calls: Mutex::default() }
        }
        fn fetched(&self) -> Vec<String> {
            let mut v = self.calls.lock().unwrap().clone();
            v.sort();
            v.dedup();
            v
        }
    }
    impl Fetch for Fake {
        fn get(&self, url: &str) -> Result<Vec<u8>, String> {
            self.calls.lock().unwrap().push(url.rsplit('/').next().unwrap().to_string());
            if self.broken.lock().unwrap().contains(url) { return Err("connection reset".into()); }
            self.files.get(url).cloned().ok_or_else(|| "HTTP 404".to_string())
        }
    }

    fn run(root: &Path, remote: &Remote, fake: &Fake, verify: bool, cancel: bool) -> Result<(), String> {
        download(root, &root.join("textures/INFN00001"), remote, verify, fake, &AtomicBool::new(cancel), &|_| {})
    }

    #[test] fn downloads_everything_and_records_it() {
        let d = tempfile::tempdir().unwrap();
        let remote = pack_of(DATA, C2);
        let fake = Fake::new(&remote, DATA);
        let seen = Mutex::new(Vec::new());
        let pack = d.path().join("textures/INFN00001");
        download(d.path(), &pack, &remote, false, &fake, &AtomicBool::new(false), &|p| seen.lock().unwrap().push(p))
            .unwrap();
        for (p, data) in DATA { assert_eq!(std::fs::read(pack.join(p)).unwrap(), *data); }
        let m = load_manifest(&pack).unwrap();
        assert_eq!((m.commit.as_str(), m.files.len()), (C2, 4));
        let last = seen.lock().unwrap().last().cloned().unwrap();
        assert_eq!((last.phase, last.files_done, last.files_total, last.done_bytes, last.total_bytes),
                   ("download", 4, 4, 22, 22));
        assert_eq!(fake.calls.lock().unwrap().last().map(String::as_str), Some("textures.ini"), "textures.ini last");
        assert_eq!(status(d.path(), &pack, Some(&remote), None).state, "complete");
        assert!(!pack.join("ui/a.png.part").exists());
    }

    #[test] fn a_failed_file_is_the_only_one_fetched_again() {
        let d = tempfile::tempdir().unwrap();
        let pack = d.path().join("textures/INFN00001");
        let remote = pack_of(DATA, C2);
        let fake = Fake::new(&remote, DATA);
        fake.broken.lock().unwrap().insert(raw_url(C2, "ui/a.png"));
        let err = run(d.path(), &remote, &fake, false, false).unwrap_err();
        assert!(err.contains("ui/a.png") && err.contains("connection reset"), "{err}");
        assert!(!pack.join("textures.ini").exists(), "a cut download leaves the pack off");
        let s = status(d.path(), &pack, Some(&remote), None);
        assert_eq!((s.state, s.missing_files), ("partial", 2));
        fake.broken.lock().unwrap().clear();
        fake.calls.lock().unwrap().clear();
        run(d.path(), &remote, &fake, false, false).unwrap();
        assert_eq!(fake.fetched(), ["a.png", "textures.ini"]);
        assert_eq!(load_manifest(&pack).unwrap().commit, C2);
    }

    #[test] fn cancel_keeps_what_is_there_and_no_commit() {
        let d = tempfile::tempdir().unwrap();
        let remote = pack_of(DATA, C2);
        let fake = Fake::new(&remote, DATA);
        assert_eq!(run(d.path(), &remote, &fake, false, true).unwrap_err(), "cancelled");
        assert!(fake.calls.lock().unwrap().is_empty());
        assert_eq!(load_manifest(&d.path().join("textures/INFN00001")).unwrap().commit, "");
    }

    #[test] fn an_update_fetches_changes_and_drops_removed_files_only() {
        let d = tempfile::tempdir().unwrap();
        let pack = d.path().join("textures/INFN00001");
        let v1 = pack_of(DATA, C1);
        run(d.path(), &v1, &Fake::new(&v1, DATA), false, false).unwrap();
        put(&pack, "_custom_textures/mine.png", b"mine");
        put(&pack, "notes.txt", b"the player's");
        let v2data: &[(&str, &[u8])] = &[("textures.ini", b"[options]\nv2\n"), ("ui/a.png", b"AAAA"),
                                         ("monsters/dragon puppies/b.png", b"bbbbbb")];
        let v2 = pack_of(v2data, C2);
        let fake = Fake::new(&v2, v2data);
        assert_eq!(status(d.path(), &pack, Some(&v2), None).state, "update");
        run(d.path(), &v2, &fake, false, false).unwrap();
        assert_eq!(fake.fetched(), ["a.png", "textures.ini"]);
        assert!(!pack.join("c.png").exists(), "dropped upstream, written by the launcher");
        assert_eq!(std::fs::read(pack.join("_custom_textures/mine.png")).unwrap(), b"mine");
        assert_eq!(std::fs::read(pack.join("notes.txt")).unwrap(), b"the player's");
        assert_eq!(std::fs::read(pack.join("ui/a.png")).unwrap(), b"AAAA");
        assert_eq!(status(d.path(), &pack, Some(&v2), None).state, "complete");
    }

    #[test] fn verify_repairs_a_corrupt_file_and_adopts_a_copied_pack() {
        let d = tempfile::tempdir().unwrap();
        let pack = d.path().join("textures/INFN00001");
        let remote = pack_of(DATA, C2);
        for (p, data) in DATA { put(&pack, p, data); }  // copied by hand: no manifest
        put(&pack, "ui/a.png", b"aXaa");                 // damaged, same size
        let fake = Fake::new(&remote, DATA);
        run(d.path(), &remote, &fake, false, false).unwrap();
        assert_eq!(fake.fetched(), ["a.png"], "no manifest: content checked, only the bad file fetched");
        put(&pack, "c.png", b"cX");
        fake.calls.lock().unwrap().clear();
        assert_eq!(status(d.path(), &pack, Some(&remote), None).state, "complete", "the quick check cannot see it");
        run(d.path(), &remote, &fake, true, false).unwrap();
        assert_eq!(fake.fetched(), ["c.png"]);
        assert_eq!(std::fs::read(pack.join("c.png")).unwrap(), b"cc");
    }

    #[cfg(windows)]
    #[test] fn never_writes_through_a_link() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path().join("root");
        let real = d.path().join("ppsspp_pack");
        std::fs::create_dir_all(&real).unwrap();
        std::fs::create_dir_all(&root).unwrap();
        let ok = std::process::Command::new("cmd").args(["/C", "mklink", "/J"])
            .arg(root.join("textures")).arg(&real).status().unwrap();
        assert!(ok.success());
        let remote = pack_of(DATA, C2);
        let fake = Fake::new(&remote, DATA);
        assert!(run(&root, &remote, &fake, false, false).unwrap_err().contains("link"));
        assert!(fake.calls.lock().unwrap().is_empty());
        assert!(std::fs::read_dir(&real).unwrap().next().is_none(), "link target untouched");
    }

    /// Lists the real repository (2 API calls, no file download). cargo test -- --ignored
    #[test] #[ignore] fn real_repository_listing() {
        let r = fetch_remote(&Http::new()).unwrap();
        assert!(r.files.len() > 3000 && r.files.iter().any(|f| f.path == "textures.ini"), "{}", r.files.len());
        assert!(r.files.iter().map(|f| f.size).sum::<u64>() > 800_000_000);
        assert!(r.files.iter().all(|f| wanted(&f.path) && safe_rel(&f.path)));
    }
```
  Run the Rust gate → compile errors.

- [ ] **Step 2: Engine and HTTP** — in `textures.rs`, extend the `use` lines to
  `use std::collections::{HashMap, HashSet}; use std::io::{self, Read}; use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering::SeqCst}; use std::sync::Mutex;`
  and add after `status`:

```rust
/// One HTTP GET returning the whole body (API answers and pack files are at most a few MB).
pub trait Fetch: Sync {
    fn get(&self, url: &str) -> Result<Vec<u8>, String>;
}

/// ureq with rustls; GitHub wants a User-Agent.
pub struct Http(ureq::Agent);

impl Http {
    pub fn new() -> Http {
        Http(ureq::AgentBuilder::new()
            .timeout_connect(std::time::Duration::from_secs(15))
            .timeout_read(std::time::Duration::from_secs(60))
            .user_agent("p3-launcher (Patapon 3 recompiled; HD texture download)")
            .build())
    }
}

impl Fetch for Http {
    fn get(&self, url: &str) -> Result<Vec<u8>, String> {
        match self.0.get(url).call() {
            Ok(r) => {
                let mut v = Vec::new();
                r.into_reader().take(64 << 20).read_to_end(&mut v).map_err(|e| e.to_string())?;
                Ok(v)
            }
            Err(ureq::Error::Status(code, _)) if (code == 403 || code == 429) && url.contains("api.github.com") =>
                Err(format!("GitHub limit reached (HTTP {code}); try again in an hour")),
            Err(ureq::Error::Status(code, _)) => Err(format!("HTTP {code}")),
            Err(e) => Err(e.to_string()),
        }
    }
}

/// The pack at the latest commit of BRANCH (2 API calls).
pub fn fetch_remote(f: &dyn Fetch) -> Result<Remote, String> {
    let (commit, date) = parse_commit(&String::from_utf8_lossy(&f.get(&commit_url())?))?;
    let files = parse_tree(&String::from_utf8_lossy(&f.get(&tree_url(&commit))?))?;
    Ok(Remote { commit, date, files })
}

/// `texture_progress`: phase "verify" (re-hashing, files only) or "download".
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Progress {
    pub phase: &'static str,
    pub done_bytes: u64,
    pub total_bytes: u64,
    pub files_done: usize,
    pub files_total: usize,
}

const WORKERS: usize = 4;
const ATTEMPTS: u64 = 3;
const SPARE_BYTES: u64 = 64 << 20;

/// Fetches one file (3 attempts), checks size and SHA-1, writes `<file>.part` and renames it over the file.
fn fetch_one(pack: &Path, commit: &str, f: &RemoteFile, fetch: &dyn Fetch) -> Result<(), String> {
    let target = pack.join(&f.path);
    let mut last = String::new();
    for attempt in 1..=ATTEMPTS {
        match fetch.get(&raw_url(commit, &f.path)) {
            Ok(data) if data.len() as u64 == f.size && blob_sha1(&data) == f.sha => {
                let mut part = target.clone().into_os_string();
                part.push(".part");
                let part = PathBuf::from(part);
                let write = || -> io::Result<()> {
                    if let Some(dir) = target.parent() { std::fs::create_dir_all(dir)?; }
                    std::fs::write(&part, &data)?;
                    std::fs::rename(&part, &target)
                };
                return write().map_err(|e| format!("{}: {e}", f.path));
            }
            Ok(_) => last = format!("{}: the download does not match the repository", f.path),
            Err(e) => last = format!("{}: {e}", f.path),
        }
        if attempt < ATTEMPTS { std::thread::sleep(std::time::Duration::from_millis(300 * attempt)); }
    }
    Err(last)
}

/// Brings `pack` to `remote` (spec §2.2-2.7): checks what is on disk — by content when `verify`, or when files
/// are there but no manifest says the launcher wrote them —, fetches the rest WORKERS at a time with
/// textures.ini last, deletes files a previous download wrote that the repository dropped, and records the
/// result in the manifest, also after a cancel or a failure (every verified file is kept).
pub fn download(root: &Path, pack: &Path, remote: &Remote, verify: bool, fetch: &dyn Fetch, cancel: &AtomicBool,
                progress: &(dyn Fn(Progress) + Sync)) -> Result<(), String> {
    if is_linked(root, pack) {
        return Err(format!("{} is a link to another folder; the launcher does not change it", pack.display()));
    }
    std::fs::create_dir_all(pack).map_err(|e| e.to_string())?;
    let old = load_manifest(pack);
    let present = remote.files.iter().any(|f| pack.join(&f.path).is_file());
    let deep = verify || (old.is_none() && present);
    let count = remote.files.len();
    let need = needed(pack, &remote.files, old.as_ref(), deep, &mut |i| {
        if deep && (i % 50 == 0 || i == count) {
            progress(Progress { phase: "verify", done_bytes: 0, total_bytes: 0, files_done: i, files_total: count });
        }
    });
    let need_bytes: u64 = need.iter().map(|f| f.size).sum();
    if let Ok(free) = fs2::available_space(pack) {
        if !need.is_empty() && free < need_bytes + SPARE_BYTES {
            return Err(format!("not enough disk space: {} MB needed, {} MB free",
                               (need_bytes + SPARE_BYTES) >> 20, free >> 20));
        }
    }
    let (ini, rest): (Vec<&RemoteFile>, Vec<&RemoteFile>) = need.iter().partition(|f| f.path == "textures.ini");
    let done_bytes = AtomicU64::new(0);
    let files_done = AtomicUsize::new(0);
    let next = AtomicUsize::new(0);
    let good: Mutex<HashSet<String>> = Mutex::new(HashSet::new());
    let errors: Mutex<Vec<String>> = Mutex::new(Vec::new());
    let report = || progress(Progress { phase: "download", done_bytes: done_bytes.load(SeqCst), total_bytes: need_bytes,
                                        files_done: files_done.load(SeqCst), files_total: need.len() });
    let one = |f: &RemoteFile| {
        match fetch_one(pack, &remote.commit, f, fetch) {
            Ok(()) => { good.lock().unwrap().insert(f.path.clone()); }
            Err(e) => errors.lock().unwrap().push(e),
        }
        done_bytes.fetch_add(f.size, SeqCst);
        files_done.fetch_add(1, SeqCst);
        report();
    };
    report();
    std::thread::scope(|s| {
        for _ in 0..WORKERS {
            s.spawn(|| loop {
                if cancel.load(SeqCst) { break; }
                let Some(&f) = rest.get(next.fetch_add(1, SeqCst)) else { break };
                one(f);
            });
        }
    });
    let cancelled = cancel.load(SeqCst);
    if !cancelled && errors.lock().unwrap().is_empty() {
        for &f in &ini { one(f); }
    }
    let errors = errors.into_inner().unwrap();
    let good = good.into_inner().unwrap();
    let missing: HashSet<&str> = need.iter().map(|f| f.path.as_str()).filter(|p| !good.contains(*p)).collect();
    let complete = missing.is_empty();
    if complete {
        let keep: HashSet<&str> = remote.files.iter().map(|f| f.path.as_str()).collect();
        for f in old.iter().flat_map(|m| &m.files) {
            if !keep.contains(f.path.as_str()) && safe_rel(&f.path) { std::fs::remove_file(pack.join(&f.path)).ok(); }
        }
    }
    let (commit, date) = if complete { (remote.commit.clone(), remote.date.clone()) }
                         else { old.as_ref().map(|m| (m.commit.clone(), m.date.clone())).unwrap_or_default() };
    let files = remote.files.iter().filter(|f| !missing.contains(f.path.as_str())).cloned().collect();
    save_manifest(pack, &Manifest { commit, date, files }).map_err(|e| e.to_string())?;
    if !complete && cancelled { return Err("cancelled".into()); }
    if let Some(first) = errors.first() { return Err(format!("{} file(s) failed; first: {first}", errors.len())); }
    Ok(())
}
```
  `Cargo.toml` `[dependencies]`: `ureq = "2"` and `fs2 = "0.4"` (ureq 2's default features bring rustls TLS).
  Remove `HashMap` from the top `use` if the compiler reports it unused only when it is (it is used by `needed`).

- [ ] **Step 3: Commands** (`lib.rs`), next to the install commands:

```rust
#[derive(Default)]
struct TextureState { cancel: Arc<AtomicBool>, running: AtomicBool }

/// settings.ini display.texture_pack resolved against the portable root.
fn texture_pack(root: &std::path::Path) -> std::path::PathBuf {
    let v = settings::load(root).ok().and_then(|m| m.get("display.texture_pack").cloned())
        .unwrap_or_else(|| "textures/INFN00001".into());
    textures::pack_dir(root, &v)
}

/// online = false: the quick local check only (Options); true: also the repository (the Textures screen).
#[tauri::command]
async fn texture_status(online: bool) -> Result<textures::Status, String> {
    let root = settings::root_dir();
    tauri::async_runtime::spawn_blocking(move || {
        let pack = texture_pack(&root);
        if !online { return textures::status(&root, &pack, None, None); }
        match textures::fetch_remote(&textures::Http::new()) {
            Ok(r) => textures::status(&root, &pack, Some(&r), None),
            Err(e) => textures::status(&root, &pack, None, Some(e)),
        }
    }).await.map_err(|e| e.to_string())
}

#[tauri::command]
async fn download_textures(app: AppHandle, state: State<'_, TextureState>, verify: bool) -> Result<(), String> {
    if state.running.swap(true, Ordering::SeqCst) { return Err("already downloading".into()); }
    state.cancel.store(false, Ordering::SeqCst);
    let (cancel, root) = (state.cancel.clone(), settings::root_dir());
    let r = tauri::async_runtime::spawn_blocking(move || {
        let pack = texture_pack(&root);
        let http = textures::Http::new();
        let remote = textures::fetch_remote(&http)?;
        let last = std::sync::Mutex::new(std::time::Instant::now() - std::time::Duration::from_secs(1));
        textures::download(&root, &pack, &remote, verify, &http, &cancel, &|p| {
            let mut l = last.lock().unwrap();
            if p.files_done == p.files_total || l.elapsed() >= std::time::Duration::from_millis(100) {
                *l = std::time::Instant::now();
                app.emit("texture_progress", p).ok();
            }
        })
    }).await;
    state.running.store(false, Ordering::SeqCst);
    r.map_err(|e| e.to_string())?
}

#[tauri::command]
fn cancel_textures(state: State<'_, TextureState>) { state.cancel.store(true, Ordering::SeqCst); }

#[tauri::command]
fn open_texture_page() -> Result<(), String> { open_path(std::path::Path::new(textures::PAGE_URL)) }
```
  `.manage(TextureState::default())` next to `.manage(InstallState::default())`, and the four commands in
  `generate_handler![…]`.

- [ ] **Step 4: Gate.** Rust gate → **86 passed, 2 ignored** (80 + 6; paste the real line); `cargo build -q` with
  no warnings; then `cargo test -q real_repository_listing -- --ignored` → 1 passed (needs the network; paste
  it). Report (no commit). Claude commits `feat(launcher): download the HD texture pack file by file, verified`.

---

### Task 3: Textures screen, Options entry, credits (launcher UI)

**Files:**
- Create: `src/textures.ts`, `src/textures.test.ts`, `src/screens/Textures.tsx`, `src/screens/Textures.test.tsx`
- Modify: `src/types.ts`, `src/api.ts`, `src/screens/Options.tsx`, `src/screens/Options.test.tsx`, `src/App.tsx`,
  `src/styles.css`, `lang/es.json`, `lang/en.json`, `README.md`

**Interfaces:**
- Consumes: Task 2 commands `texture_status`, `download_textures`, `cancel_textures`, `open_texture_page`,
  event `texture_progress`.
- Produces: `TextureStatus`, `TextureProgress` (types.ts); `api.textureStatus(online)`, `api.downloadTextures(verify)`,
  `api.cancelTextures()`, `api.openTexturePage()`; `textures.ts`: `PACK_URL`, `CREDITS`, `formatSize`,
  `statusLine`, `entries`, `shortState`; screen `Textures`; Options `actions[].tab`.

- [ ] **Step 1: Strings.** `lang/es.json` adds (en in brackets — `lang/en.json` gets the same keys):

```json
  "options.textures": "Texturas HD: {state}",
  "textures.short.none": "no instaladas",
  "textures.short.partial": "incompletas",
  "textures.short.update": "hay actualización",
  "textures.short.complete": "instaladas ✓",
  "textures.short.unknown": "sin comprobar",
  "textures.short.linked": "carpeta enlazada",
  "textures.title": "TEXTURAS HD",
  "textures.checking": "Comprobando…",
  "textures.none": "No instaladas. El paquete ocupa {size}.",
  "textures.none_offline": "No instaladas.",
  "textures.partial": "Archivos que faltan: {files} ({size}).",
  "textures.update": "Hay una actualización del {date}: {files} archivos ({size}).",
  "textures.complete": "Instaladas ✓",
  "textures.complete_date": "Instaladas ✓ (versión del {date})",
  "textures.unknown": "Hay texturas en la carpeta, sin comprobar.",
  "textures.linked": "La carpeta de texturas es un enlace a otra carpeta: el launcher no la cambia.",
  "textures.offline": "Sin conexión con GitHub ({error}).",
  "textures.download": "Descargar ({size})",
  "textures.download_missing": "Descargar lo que falta ({size})",
  "textures.update_btn": "Actualizar ({size})",
  "textures.verify": "Comprobar todos los archivos",
  "textures.retry": "Reintentar",
  "textures.page": "Página del paquete en GitHub",
  "textures.cancel": "Cancelar",
  "textures.progress": "{done} de {total} · {files} de {count} archivos",
  "textures.verifying": "Comprobando archivos… {files} de {count}",
  "textures.done": "Texturas instaladas.",
  "textures.cancelled": "Descarga cancelada; lo descargado se conserva.",
  "textures.error": "Error: {message}",
  "textures.credits": "Créditos del paquete",
  "textures.license": "Paquete HD de la comunidad (Patapon-3-HD-Texture-Pack, basado en Patapon3Textures). El launcher lo descarga de su repositorio original y no lo redistribuye."
```
  en: "HD textures: {state}", "not installed", "incomplete", "update available", "installed ✓", "not checked",
  "linked folder", "HD TEXTURES", "Checking…", "Not installed. The pack takes {size}.", "Not installed.",
  "Missing files: {files} ({size}).", "Update from {date}: {files} files ({size}).", "Installed ✓",
  "Installed ✓ (version from {date})", "There are textures in the folder, not checked.", "The texture folder
  links to another folder: the launcher does not change it.", "Cannot reach GitHub ({error}).", "Download
  ({size})", "Download what is missing ({size})", "Update ({size})", "Check every file", "Retry", "The pack's
  GitHub page", "Cancel", "{done} of {total} · {files} of {count} files", "Checking files… {files} of {count}",
  "Textures installed.", "Download cancelled; what was downloaded is kept.", "Error: {message}", "Pack credits",
  "Community HD pack (Patapon-3-HD-Texture-Pack, based on Patapon3Textures). The launcher downloads it from its
  original repository and does not redistribute it."

- [ ] **Step 2: Types, api, `src/textures.ts` + tests.** `types.ts` adds:

```ts
export type TextureState = "linked" | "none" | "partial" | "update" | "complete" | "unknown";
export type TextureStatus = { state: TextureState; online: boolean; local_date: string | null; remote_date: string | null;
  missing_files: number; missing_bytes: number; total_bytes: number; error: string | null };
export type TextureProgress = { phase: "verify" | "download"; done_bytes: number; total_bytes: number;
  files_done: number; files_total: number };
```
  `api.ts` adds (import the two types):

```ts
  textureStatus: (online: boolean) => invoke<TextureStatus>("texture_status", { online }),
  downloadTextures: (verify: boolean) => invoke<void>("download_textures", { verify }),
  cancelTextures: () => invoke<void>("cancel_textures"),
  openTexturePage: () => invoke<void>("open_texture_page"),
```
  `src/textures.ts`:

```ts
// HD texture pack screen helpers (src-tauri/src/textures.rs computes the status): sizes, the status line,
// the entries each state offers and the pack's credits (its README, "Textures Remastered by").
import { t } from "./i18n";
import type { TextureStatus } from "./types";

type Strings = Record<string, string>;

export const PACK_URL = "https://github.com/Lin-zl522/Patapon-3-HD-Texture-Pack";

export const CREDITS: { name: string; role?: string }[] = [
  { name: "WallSoGB", role: "Leader of the Patapon3Textures project" },
  { name: "Shockturtle", role: "Leader of the Patapon2HD project" },
  { name: "owocek" },
  { name: "efonte", role: "AI Upscaling" },
  { name: "Keamble", role: "HD Equipments, HD Masks, HD UI, HD Sprites, HD Maps, HD effects" },
  { name: "Hozzzd", role: "Tech Support" },
  { name: "KnotSora", role: "HD monsters, HD effects" },
  { name: "Lin", role: "HD Equipments, HD UI, HD Sprites, HD Maps" },
  { name: "Olimp666", role: "Russian Localization" },
  { name: "wondaoxigen", role: "HD Masks, HD Hero Sprites" },
  { name: "Rin Casi" }, { name: "Fabierex" }, { name: "Heavenartist2006" }, { name: "Zica Supercharger" },
  { name: "bl4zerp" }, { name: "thiago34x" }, { name: "mamba" },
];

export function formatSize(bytes: number): string {
  if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(2)} GB`;
  if (bytes >= 1e6) return `${(bytes / 1e6).toFixed(1)} MB`;
  return `${Math.max(1, Math.round(bytes / 1e3))} KB`;
}

const day = (iso: string | null) => (iso ?? "").slice(0, 10);

export function statusLine(s: Strings, st: TextureStatus): string {
  const files = String(st.missing_files), size = formatSize(st.missing_bytes);
  switch (st.state) {
    case "linked": return t(s, "textures.linked");
    case "none": return st.total_bytes > 0 ? t(s, "textures.none", { size: formatSize(st.total_bytes) }) : t(s, "textures.none_offline");
    case "partial": return t(s, "textures.partial", { files, size });
    case "update": return t(s, "textures.update", { date: day(st.remote_date), files, size });
    case "complete": {
      const date = day(st.local_date ?? st.remote_date);
      return date ? t(s, "textures.complete_date", { date }) : t(s, "textures.complete");
    }
    case "unknown": return t(s, "textures.unknown");
  }
}

export type TextureEntry = { id: "download" | "verify" | "retry" | "page" | "back" | "cancel"; label: string };

/// The menu of the Textures screen for a status (null = still checking).
export function entries(s: Strings, st: TextureStatus | null): TextureEntry[] {
  const out: TextureEntry[] = [];
  if (st && st.state !== "linked") {
    if (st.online && st.missing_files > 0) {
      const key = st.state === "update" ? "textures.update_btn" : st.state === "none" ? "textures.download" : "textures.download_missing";
      out.push({ id: "download", label: t(s, key, { size: formatSize(st.missing_bytes) }) });
    }
    if (st.online && st.state !== "none") out.push({ id: "verify", label: t(s, "textures.verify") });
    if (!st.online) out.push({ id: "retry", label: t(s, "textures.retry") });
  }
  out.push({ id: "page", label: t(s, "textures.page") }, { id: "back", label: t(s, "prompt.back") });
  return out;
}

export function shortState(s: Strings, st: TextureStatus | null): string {
  return st ? t(s, `textures.short.${st.state}`) : "…";
}
```
  `src/textures.test.ts`:

```ts
import { entries, formatSize, shortState, statusLine } from "./textures";
import type { TextureStatus } from "./types";

const s: Record<string, string> = {
  "textures.none": "No instaladas. El paquete ocupa {size}.", "textures.none_offline": "No instaladas.",
  "textures.partial": "Archivos que faltan: {files} ({size}).", "textures.update": "Hay una actualización del {date}: {files} archivos ({size}).",
  "textures.complete": "Instaladas ✓", "textures.complete_date": "Instaladas ✓ (versión del {date})",
  "textures.unknown": "Sin comprobar.", "textures.linked": "Enlace.", "textures.download": "Descargar ({size})",
  "textures.download_missing": "Descargar lo que falta ({size})", "textures.update_btn": "Actualizar ({size})",
  "textures.verify": "Comprobar todos los archivos", "textures.retry": "Reintentar", "textures.page": "Página",
  "prompt.back": "Volver", "textures.short.partial": "incompletas" };
const st = (o: Partial<TextureStatus>): TextureStatus => ({ state: "none", online: true, local_date: null, remote_date: null,
  missing_files: 0, missing_bytes: 0, total_bytes: 856_059_410, error: null, ...o });

test("sizes", () => {
  expect(formatSize(856_059_410)).toBe("856.1 MB");
  expect(formatSize(60_878)).toBe("61 KB");
  expect(formatSize(1_500_000_000)).toBe("1.50 GB");
});

test("status lines", () => {
  expect(statusLine(s, st({ state: "none" }))).toBe("No instaladas. El paquete ocupa 856.1 MB.");
  expect(statusLine(s, st({ state: "none", online: false, total_bytes: 0 }))).toBe("No instaladas.");
  expect(statusLine(s, st({ state: "partial", missing_files: 1, missing_bytes: 60_878 }))).toBe("Archivos que faltan: 1 (61 KB).");
  expect(statusLine(s, st({ state: "update", remote_date: "2026-08-26T23:05:25Z", missing_files: 2, missing_bytes: 2e6 })))
    .toBe("Hay una actualización del 2026-08-26: 2 archivos (2.0 MB).");
  expect(statusLine(s, st({ state: "complete", local_date: "2026-08-26T23:05:25Z" }))).toBe("Instaladas ✓ (versión del 2026-08-26)");
  expect(statusLine(s, st({ state: "complete" }))).toBe("Instaladas ✓");
});

test("entries per state", () => {
  const ids = (x: TextureStatus | null) => entries(s, x).map((e) => e.id);
  expect(ids(null)).toEqual(["page", "back"]);
  expect(entries(s, st({ state: "none", missing_files: 3055, missing_bytes: 856_059_410 }))[0].label).toBe("Descargar (856.1 MB)");
  expect(ids(st({ state: "none", missing_files: 3055, missing_bytes: 856_059_410 }))).toEqual(["download", "page", "back"]);
  expect(entries(s, st({ state: "partial", missing_files: 1, missing_bytes: 60_878 }))[0].label).toBe("Descargar lo que falta (61 KB)");
  expect(entries(s, st({ state: "update", missing_files: 2, missing_bytes: 2e6 }))[0].label).toBe("Actualizar (2.0 MB)");
  expect(ids(st({ state: "complete" }))).toEqual(["verify", "page", "back"]);
  expect(ids(st({ state: "partial", online: false, missing_files: 1 }))).toEqual(["retry", "page", "back"]);
  expect(ids(st({ state: "linked" }))).toEqual(["page", "back"]);
  expect(shortState(s, st({ state: "partial" }))).toBe("incompletas");
  expect(shortState(s, null)).toBe("…");
});
```

- [ ] **Step 3: `src/screens/Textures.tsx`:**

```tsx
import { useEffect, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { api } from "../api";
import { PromptBar } from "../components/PromptBar";
import { t } from "../i18n";
import { useListNav } from "../nav/useNav";
import { CREDITS, entries, formatSize, PACK_URL, statusLine, type TextureEntry } from "../textures";
import type { TextureProgress, TextureStatus } from "../types";

type Props = { strings: Record<string, string>; onBack(): void };

// Options › Gráficos › Texturas HD: what is installed (checked against the repository), download / complete /
// update with a progress bar and Cancel (Back cancels too), a full check, and the pack's credits.
export function Textures(p: Props) {
  const [status, setStatus] = useState<TextureStatus | null>(null);
  const [busy, setBusy] = useState(false);
  const [prog, setProg] = useState<TextureProgress | null>(null);
  const [message, setMessage] = useState("");
  const alive = useRef(true);
  useEffect(() => () => { alive.current = false; }, []);

  const check = () => {
    setStatus(null);
    api.textureStatus(true).then((s) => { if (alive.current) setStatus(s); })
      .catch((e) => { if (alive.current) setMessage(t(p.strings, "textures.error", { message: String(e) })); });
  };
  useEffect(check, []);  // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    let un: (() => void) | null = null;
    let closed = false;
    listen<TextureProgress>("texture_progress", (e) => setProg(e.payload))
      .then((u) => { if (closed) u(); else un = u; }).catch(() => {});
    return () => { closed = true; un?.(); };
  }, []);

  const run = async (verify: boolean) => {
    setBusy(true);
    setMessage("");
    setProg(null);
    try {
      await api.downloadTextures(verify);
      if (alive.current) setMessage(t(p.strings, "textures.done"));
    } catch (e) {
      if (alive.current) setMessage(String(e) === "cancelled" ? t(p.strings, "textures.cancelled")
        : t(p.strings, "textures.error", { message: String(e) }));
    }
    if (!alive.current) return;
    setBusy(false);
    check();
  };

  const list: TextureEntry[] = busy ? [{ id: "cancel", label: t(p.strings, "textures.cancel") }] : entries(p.strings, status);
  const pick = (i: number) => {
    const e = list[Math.min(i, list.length - 1)];
    if (e.id === "cancel") void api.cancelTextures();
    else if (e.id === "download") void run(false);
    else if (e.id === "verify") void run(true);
    else if (e.id === "retry") { setMessage(""); check(); }
    else if (e.id === "page") void api.openTexturePage();
    else p.onBack();
  };
  const nav = useListNav(list.length, pick, () => { if (busy) void api.cancelTextures(); else p.onBack(); });
  useEffect(() => nav.setIndex(0), [busy, status]);  // eslint-disable-line react-hooks/exhaustive-deps
  const sel = Math.min(nav.index, list.length - 1);

  const pct = !prog ? 0 : prog.phase === "download" ? (prog.total_bytes > 0 ? Math.floor((100 * prog.done_bytes) / prog.total_bytes) : 0)
    : (prog.files_total > 0 ? Math.floor((100 * prog.files_done) / prog.files_total) : 0);
  const progText = prog?.phase === "verify"
    ? t(p.strings, "textures.verifying", { files: String(prog.files_done), count: String(prog.files_total) })
    : t(p.strings, "textures.progress", { done: formatSize(prog?.done_bytes ?? 0), total: formatSize(prog?.total_bytes ?? 0),
        files: String(prog?.files_done ?? 0), count: String(prog?.files_total ?? 0) });
  return (
    <div className="screen textures" data-testid="textures">
      <h1 className="title">{t(p.strings, "textures.title")}</h1>
      <div className="panel textures-panel">
        <p role="status">{status ? statusLine(p.strings, status) : t(p.strings, "textures.checking")}</p>
        {status?.error && <p className="muted">{t(p.strings, "textures.offline", { error: status.error })}</p>}
        {busy && <progress className="install-progress" max={100} value={pct} />}
        {busy && <p data-testid="texture-progress">{progText}</p>}
        {message && <p className="textures-message">{message}</p>}
      </div>
      <ul className="menu" role="listbox">
        {list.map((e, i) => (
          <li key={e.id} role="option" aria-selected={i === sel} className={i === sel ? "selected" : ""}
              onMouseEnter={() => nav.setIndex(i)} onClick={() => pick(i)}>{e.label}</li>
        ))}
      </ul>
      <section className="panel credits" aria-label={t(p.strings, "textures.credits")}>
        <h2>{t(p.strings, "textures.credits")}</h2>
        <p className="muted">{t(p.strings, "textures.license")}</p>
        <ul>{CREDITS.map((c) => <li key={c.name}><b>{c.name}</b>{c.role ? ` — ${c.role}` : ""}</li>)}</ul>
        <p className="muted">{PACK_URL}</p>
      </section>
      <PromptBar device={nav.device} items={[
        { id: "select", label: t(p.strings, "prompt.select") },
        { id: "confirm", label: t(p.strings, "prompt.confirm") },
        { id: "back", label: t(p.strings, busy ? "textures.cancel" : "prompt.back") },
      ]} />
    </div>
  );
}
```
  `styles.css`: `.textures .textures-panel` (like `.notice-text`, centred, `min-height: 6em`),
  `.textures .credits { max-height: 28vh; overflow-y: auto; font-size: clamp(12px, 1.8vh, 20px); }`,
  `.textures .credits ul { columns: 2; list-style: none; padding: 0; }`, `.textures .credits h2 { margin: 0 0 0.3em; font-size: 1.2em; }`.

- [ ] **Step 4: `src/screens/Textures.test.tsx`:**

```tsx
import { act, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";

const handlers = vi.hoisted(() => new Map<string, (ev: { payload: unknown }) => void>());
vi.mock("@tauri-apps/api/event", () => ({
  listen: vi.fn(async (name: string, cb: (ev: { payload: unknown }) => void) => {
    handlers.set(name, cb);
    return () => { handlers.delete(name); };
  }),
}));
const api = vi.hoisted(() => ({ textureStatus: vi.fn(), downloadTextures: vi.fn(), cancelTextures: vi.fn(async () => {}),
  openTexturePage: vi.fn(async () => {}) }));
vi.mock("../api", () => ({ api }));
import { Textures } from "./Textures";

const strings: Record<string, string> = {
  "textures.title": "TEXTURAS HD", "textures.checking": "Comprobando…", "textures.partial": "Archivos que faltan: {files} ({size}).",
  "textures.complete_date": "Instaladas ✓ (versión del {date})", "textures.linked": "Es un enlace.",
  "textures.offline": "Sin conexión con GitHub ({error}).", "textures.download_missing": "Descargar lo que falta ({size})",
  "textures.verify": "Comprobar todos los archivos", "textures.retry": "Reintentar", "textures.page": "Página del paquete en GitHub",
  "textures.cancel": "Cancelar", "textures.progress": "{done} de {total} · {files} de {count} archivos",
  "textures.done": "Texturas instaladas.", "textures.cancelled": "Descarga cancelada; lo descargado se conserva.",
  "textures.credits": "Créditos del paquete", "textures.license": "Se descarga del repositorio original.", "prompt.back": "Volver" };
const base = { state: "partial", online: true, local_date: null, remote_date: "2026-08-26T23:05:25Z", missing_files: 2,
  missing_bytes: 4_000_000, total_bytes: 856_059_410, error: null };

beforeEach(() => { vi.clearAllMocks(); handlers.clear(); api.textureStatus.mockResolvedValue(base); });

test("shows what is missing, the entries and the credits", async () => {
  render(<Textures strings={strings} onBack={vi.fn()} />);
  expect(await screen.findByText("Archivos que faltan: 2 (4.0 MB).")).toBeInTheDocument();
  expect(api.textureStatus).toHaveBeenCalledWith(true);
  expect(screen.getAllByRole("option").map((o) => o.textContent))
    .toEqual(["Descargar lo que falta (4.0 MB)", "Comprobar todos los archivos", "Página del paquete en GitHub", "Volver"]);
  expect(screen.getByText("WallSoGB")).toBeInTheDocument();
  expect(screen.getByText(/github\.com\/Lin-zl522\/Patapon-3-HD-Texture-Pack/)).toBeInTheDocument();
});

test("download shows progress, then the new status", async () => {
  let finish: () => void = () => {};
  api.downloadTextures.mockImplementation(() => new Promise<void>((r) => { finish = r; }));
  render(<Textures strings={strings} onBack={vi.fn()} />);
  await screen.findByText("Archivos que faltan: 2 (4.0 MB).");
  await userEvent.keyboard("{Enter}");
  expect(api.downloadTextures).toHaveBeenCalledWith(false);
  expect(screen.getAllByRole("option").map((o) => o.textContent)).toEqual(["Cancelar"]);
  act(() => handlers.get("texture_progress")!({ payload: { phase: "download", done_bytes: 2e6, total_bytes: 4e6, files_done: 1, files_total: 2 } }));
  expect(screen.getByTestId("texture-progress")).toHaveTextContent("2.0 MB de 4.0 MB · 1 de 2 archivos");
  expect(screen.getByRole("progressbar")).toHaveAttribute("value", "50");
  api.textureStatus.mockResolvedValue({ ...base, state: "complete", missing_files: 0, missing_bytes: 0, local_date: "2026-08-26T23:05:25Z" });
  await act(async () => { finish(); });
  expect(await screen.findByText("Instaladas ✓ (versión del 2026-08-26)")).toBeInTheDocument();
  expect(screen.getByText("Texturas instaladas.")).toBeInTheDocument();
  expect(api.textureStatus).toHaveBeenCalledTimes(2);
});

test("back during a download cancels it and stays", async () => {
  api.downloadTextures.mockImplementation(() => new Promise<void>(() => {}));
  const onBack = vi.fn();
  render(<Textures strings={strings} onBack={onBack} />);
  await screen.findByText("Archivos que faltan: 2 (4.0 MB).");
  await userEvent.keyboard("{Enter}");
  await userEvent.keyboard("{Escape}");
  expect(api.cancelTextures).toHaveBeenCalledOnce();
  expect(onBack).not.toHaveBeenCalled();
});

test("a cancelled download says what was kept", async () => {
  api.downloadTextures.mockRejectedValue("cancelled");
  render(<Textures strings={strings} onBack={vi.fn()} />);
  await screen.findByText("Archivos que faltan: 2 (4.0 MB).");
  await userEvent.keyboard("{Enter}");
  expect(await screen.findByText("Descarga cancelada; lo descargado se conserva.")).toBeInTheDocument();
});

test("offline offers retry; a linked folder offers no download", async () => {
  api.textureStatus.mockResolvedValue({ ...base, online: false, remote_date: null, error: "HTTP 403" });
  const { unmount } = render(<Textures strings={strings} onBack={vi.fn()} />);
  expect(await screen.findByText("Sin conexión con GitHub (HTTP 403).")).toBeInTheDocument();
  expect(screen.getAllByRole("option")[0]).toHaveTextContent("Reintentar");
  unmount();
  api.textureStatus.mockResolvedValue({ ...base, state: "linked" });
  render(<Textures strings={strings} onBack={vi.fn()} />);
  expect(await screen.findByText("Es un enlace.")).toBeInTheDocument();
  expect(screen.getAllByRole("option").map((o) => o.textContent)).toEqual(["Página del paquete en GitHub", "Volver"]);
});
```

- [ ] **Step 5: Options + App.** `Options.tsx`: `actions?: { id: string; label: string; run(): void; tab?: string }[]`
  and `const actions = (p.actions ?? []).filter((x) => (x.tab ?? "general") === TABS[tab]);` (replaces the
  `TABS[tab] === "general" ? … : []` line). `Options.test.tsx` adds:

```tsx
test("an action with a tab shows on that tab only", async () => {
  const run = vi.fn();
  render(<Options strings={strings} lang="es" defs={defs} values={values} onSaved={vi.fn()} onBack={vi.fn()}
    actions={[{ id: "textures", label: "Texturas HD: incompletas", run, tab: "graphics" }]} />);
  expect(screen.queryByText("Texturas HD: incompletas")).not.toBeInTheDocument();
  await userEvent.keyboard("e");
  await userEvent.keyboard("{ArrowDown}{ArrowDown}{ArrowDown}");
  await userEvent.keyboard("{Enter}");
  expect(run).toHaveBeenCalledOnce();
});
```
  `App.tsx`: `Screen` gains `"textures"`; state `const [texStatus, setTexStatus] = useState<TextureStatus | null>(null);`;
  `useEffect(() => { if (screen === "options") api.textureStatus(false).then(setTexStatus).catch(() => setTexStatus(null)); }, [screen]);`;
  the Options `actions` gain
  `{ id: "textures", tab: "graphics", label: t(strings, "options.textures", { state: shortState(strings, texStatus) }), run: () => setScreen("textures") }`;
  and a branch `else if (screen === "textures") { body = <Textures strings={strings} onBack={() => setScreen("options")} />; }`
  next to the `options` one. If `App.test.tsx` mocks `../api` without `textureStatus`, add
  `textureStatus: vi.fn(async () => ({ state: "none", online: false, local_date: null, remote_date: null, missing_files: 0, missing_bytes: 0, total_bytes: 0, error: null }))`
  to its mock (only if a test fails for that reason; say so).

- [ ] **Step 6: README.** Launcher `README.md` (es + en): section "Texturas HD / HD textures": Options › Gráficos ›
  Texturas HD; downloads Lin-zl522/Patapon-3-HD-Texture-Pack (856 MB, every language) from GitHub file by file
  into `textures/INFN00001`, verified; resume / "Check every file" / updates; the pack's credits and the
  no-redistribution note; a folder that is a link is never changed.

- [ ] **Step 7: Gates.** UI gate: Vitest **52 passed** (43 + 3 textures + 5 Textures + 1 Options; paste the real
  count), `tsc --noEmit` clean; Rust gate unchanged (86 + 2 ignored). Report (no commit). Claude commits
  `feat(launcher): HD textures screen with progress, check and credits`.

---

### Task 4: Package, end to end, Findings (Claude)

- [ ] **Step 1:** `npm run tauri build -- --no-bundle`; make-portable into `build/e2e_portable3` and
  `build/e2e_portable` exactly as in part 4's Findings (no `--textures`). Check `e2e_portable3/textures` does not
  exist yet (or is not a link).
- [ ] **Step 2 (CDP driver `build/e2e3/cdp.mjs`, launcher with
  `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222`):** Options › Gráficos shows
  "Texturas HD: no instaladas" → open it → "No instaladas. El paquete ocupa 856.1 MB." → Descargar → the bar
  moves (screenshot) → Cancelar at ~20 % → "Descarga cancelada…" and "Archivos que faltan: N" → "Descargar lo
  que falta" → complete; time it; "Instaladas ✓ (versión del 2026-08-26)"; Options shows "instaladas ✓".
- [ ] **Step 3:** delete one PNG → "Archivos que faltan: 1 (…)" → download fetches one file. Overwrite another PNG
  with same-size garbage → status still complete → "Comprobar todos los archivos" → repaired (`git hash-object`
  equals the tree SHA).
- [ ] **Step 4:** Start the 1.1.4 game from the launcher (debug port 9998), `e2e3.py hideout`; `logs/game.log` has
  `[TEXREP] loaded …` lines and no `[TEXREP] disabled`. Screenshot of the hideout with HD textures.
- [ ] **Step 5:** `## Findings` at the end of this plan (commits, E2E table, plan defects, download time), commit
  `docs(plan): launcher part 5 findings` in psprecomp; ask the user to try it on their PC (and later on the Steam
  Deck); push launcher + psprecomp docs only on their OK.
