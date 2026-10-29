Monocypher 4.0.3 (Loup Vaillant et al.), BSD-2-Clause or CC0 (see LICENCE.md). Used by the direct P2P path
(`src/net/direct_crypto.cpp`): XChaCha20-Poly1305 (`crypto_aead_lock`/`unlock`), BLAKE2b, `crypto_wipe`.

Source: https://monocypher.org/download/monocypher-4.0.3.tar.gz, downloaded 29 Oct 2026; its SHA-512 matched the
published `monocypher-4.0.3.tar.gz.sha512`. Only `src/monocypher.c`, `src/monocypher.h` and `LICENCE.md` were copied,
unchanged. 4.0.3 rather than 4.0.2: the download page flags versions below 4.0.3 for a possible timing leak in
public-key code (unused here, but no reason to ship it).

| file | SHA-256 |
|---|---|
| monocypher-4.0.3.tar.gz | 8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66 |
| monocypher.c | 57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be |
| monocypher.h | c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd |
