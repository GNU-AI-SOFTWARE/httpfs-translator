# CHANGELOG — httpfs

Keep a Changelog format, semantic versioning. Frozen contracts never change
silently: every evolution goes through a `version` bump.

## [0.1.1] — 2026-11-22

## [0.1.0] — 2026-11-22

## [Unreleased]

### Modifié

- **Reset de version à 0.1.0** : la v1.0.0 avait été coupée par le tail d'un
  bootstrap raté (avant le merge des PRs de versioning). `VERSION` repasse à
  0.1.0, la section `## [1.0.0]` devient `## [0.1.0]` et le tag `v1.0.0` est
  supprimé ; le versionnage itératif repart de 0.1.0 → 0.1.1 → …

### Ajouté

- **Versionnage itératif** : `scripts/release.sh iterate` (bump patch auto à
  chaque itération : 0.1.0 → 0.1.1 → 0.1.2 …, `--all` pour toute la pile),
  helper `scripts/version.sh` (`--next`, `--check`), doc `docs/VERSIONING.md`
  (section « Versionnage itératif »).

### Fixed

- **`src/http.c` — `parse_ll` signed-overflow UB**: a digit run longer than
  `LLONG_MAX` in an attacker-controlled `Content-Length` / `Content-Range`
  header made the multiply overflow (confirmed under UBSan). Out-of-range
  values are now refused.
- **`src/http.c` — `size_total_of` read the wrong redirect hop**: with
  redirect following, the header buffer holds one block per hop; the search
  now starts at the *last* status line, so a 302's `Content-Length` can no
  longer mask the final response's size.
- **`src/netfs.c` — `fetch_block` index wraparound**: a seek near the top of
  the 64-bit address space made `idx * HTTPFS_BLOCK_SIZE` wrap and serve
  block 0 at a huge offset; it now returns an empty block (EOF).
- **Probe HEAD with GET fallback (`http_probe`)**: signed CDN URLs behind
  `huggingface.co/.../resolve/` (Xet bridge `cas-bridge.xethub.hf.co`) can
  refuse HEAD (403/405) or lose it on a redirect hop, which left
  `/page/status` unreadable forever. `ensure_metadata` now calls
  `http_probe`: HEAD first, then on transport failure or a 403/405/501
  answer a 1-byte ranged GET capped to 1 MB of body (`HTTP_MODE_META`;
  `CURLE_FILESIZE_EXCEEDED` in that mode publishes the headers instead of
  failing).
