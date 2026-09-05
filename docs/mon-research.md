# Japanese Mon Project — Research Report

Compiled from mindmeld (local conversation index, queried live via its REST API at
`localhost:3847/api/search` and `/api/sessions/{id}`) and the GitHub repo `loqwai/paper-cranes`.
Facts marked **[verified]** were confirmed by reading a file on disk or a commit in this session.
Facts marked **[per transcript]** are only attested in a past Claude Code conversation and were not
independently re-verified.

## 1. What the project is, in the user's terms

The user is building a family of **3D-printed, glow-in-the-dark NFC "kandi" beads** shaped like
Japanese family crests (*mon*, 家紋), worn at festivals. Each bead has an NFC chip; tapping it is
meant to show the tapper's own bead motif live on a wall display, so a stranger can recognize
"their" bead in the dark from across a room. Per the handoff doc **[per transcript, HANDOFF.md,
verified on disk]**:

> "Let's give people something to gasp about, as they understand the bead on their kandi represents
> the visual on the nfc chip." — quoted from the user in `shaders/redaphid/wip/lattice-bead/HANDOFF.md`

Two tracks exist and were deliberately kept independent:
- **The physical bead/print track** — repo `nfc-bead` (**[verified]** exists at
  `D:\Projects\nfc-bead`).
- **The shader/wall-visual track** — repo `paper-cranes` (**[verified]** exists at
  `D:\Projects\paper-cranes`, matches the Paper Cranes project referenced by the user).

**[per transcript]** Mon were not chosen for aesthetic reasons first — they were the answer to a
manufacturing constraint. The user rejected an earlier shape set ("Those are pretty bad. I want the
bead SHAPE to change — not just disks") and asked for "an existing design language with the
constraints I'm describing... from history." Mon won because they are circular, strictly symmetric,
abstract-geometric, and legible in one colour at small size — exactly what a glowing single-filament
bead needs, and (per the more recent lattice work) plausibly what a folded shader lattice cell needs
too.

A live deadline is attached: **MOGEE FEST, 2026-09-06** (Mogollon Rim) **[per transcript]**.

## 2. The "initial image" and the shapes it uses

There is **no single static "initial image" file for mon on disk** that this research could locate —
`D:\Projects\nfc-bead\tmp_mon_ref.png` **[verified]** exists but is a corrupt/empty PNG (1200×0
pixels, 33 bytes), so it is not usable reference art. A mindmeld session (39030) also states
explicitly that at the point the shape work started, "no reference images were processed" — the mon
were built from measurement, not traced art.

What *is* verified as the working "initial image" mechanism, in the newer shader work, is Paper
Cranes' `?image=` texture pipeline: a baked PNG is loaded as the WebGL `initialFrame` texture
(`src/Visualizer.js`) and sampled by the shader as the source of the mon shape. That baked PNG *is*
the initial image the shaders draw from.

The eleven mon shapes chosen (same list appears in both the bead code and the shader work)
**[verified, `D:\Projects\nfc-bead\beads\glow-set\japanese.py` line 529, `MON = { ... }`]**:

```
mokko, kikko, kiku, ume, kikyo, suhama, matsukawa, katabami, hakkaku, ogi, tomoe
```

(mallow tie-ring, tortoise-shell hexagon, chrysanthemum, plum blossom, bellflower, sandbar, pine
bark, wood-sorrel, octagon, folding fan, comma-swirl — standard mon families.)

**[per transcript]** These are not traced SVGs — they were built as **signed distance fields**
composed in Python from `sd_circle` / `sd_capsule` / `sd_polygon` primitives with smooth
`op_union`/`op_sub`/`op_inter` operators, "measured off polar radius profiles" of 504 public-domain
Wikimedia crest SVGs, then quality-gated (stroke ≥1.6mm, interior angle ≥26°, symmetric-or-clearly-
asymmetric only, never near-symmetric). A separate session mentioned generating high-resolution
(1385×1385 px) reference images including files named "Japanese Crest Mokkou.svg" and similar — but
those reference SVGs were not found on disk in this session's search and were likely fetched into an
ephemeral scratch location during that conversation, not committed to a repo.

## 3. Asset inventory (paths verified on disk)

**Bead / print side — `D:\Projects\nfc-bead`** (branch `japanese-mon`, tracking `origin/glow-set`)
**[verified: directory and file exist]**:
- `beads/glow-set/japanese.py` — the `MON` dict (SDF definitions for all 11 crests), 801 lines.
- `beads/glow-set/shapes.py` — geometric fit solver (`fit_report`): NTAG215 pocket, peg, string-hole
  and wall-thickness constraints for the printable snap-fit pendant.
- `beads/glow-set/adinkra.py`, `chinese.py` — sibling motif families (Adinkra symbols, Chinese
  glyphs) that share the same pipeline.
- `beads/glow-set/motif_outline.py` — exports one motif to a JSON polygon for Blender.
- `.venv` present (numpy/scikit-image) for the trace/contour step Blender 5.0 lacks natively.

**Shader / wall-visual side — `D:\Projects\pc-lab-tile`** (a `paper-cranes` git worktree)
**[verified: files exist]**:
- `public/images/beads/mon-{hakkaku,katabami,kikko,kiku,kikyo,matsukawa,mokko,ogi,suhama,tomoe,ume}.png`
  — all 11 baked textures, one PNG per motif.
- Channel layout of these PNGs **[per transcript, corroborated by HANDOFF.md on disk]**: **A** =
  silhouette coverage, **R** = ink/interior detail, **G** = signed distance field (0.5 = boundary,
  the scale-invariant part), **B** = spare. 1024×1024, ~1.12× bleed margin baked in.
- `shaders/redaphid/wip/lattice-bead/{1,2,grid,tile,bright,nfold}.frag`, plus `HANDOFF.md`,
  `grid.md`, `tile.md` — the shader family that samples these textures.
- `journals/lab/shots/*.png` — contact-sheet screenshots from headless legibility tests
  (`kikyo-3x3.png`, `kikyo-8x8.png`, `tomoe-5x5.png`, etc.).
- `journals/lab/LEDGER.jsonl` — append-only hypothesis/verdict log from the parallel "lab" testing
  process described in the handoff.

Formats in play: Python (SDF composition + `skimage.measure.find_contours` to trace a print-ready
polygon), 3MF (print-ready output, per session 39585/40261), PNG (baked SDF texture for the shader),
GLSL fragment shaders (`.frag`), Markdown handoff/journal docs. No SVG or OpenSCAD/Blender-script
step was found for the mon specifically (Blender is mentioned only as the target of
`motif_outline.py`'s JSON export).

## 4. What the latest `paper-cranes` branch is doing, and what's reusable

The repo has ~100+ branches. Ranked by last-commit timestamp via the GitHub API, the most recently
updated is **`mogee`** (last commit **2026-09-04T22:46:37Z**), branched from `lab/substrate2` on
2026-09-04 **[verified via `gh api repos/loqwai/paper-cranes/branches/mogee` and recent commits]**.

`mogee` is explicitly a pre-show consolidation branch: "the collected visuals for MOGEE FEST
(2026-09-06)... gathers every working visual produced by the lattice-bead lab wave into one tree...
so nothing has to be recovered from a scratch branch during the show" (its own `MOGEE.md`,
**[verified, read from GitHub]**). It merges 16 parallel `lab/*` exploration branches down to one
tree of 24 shader files under `shaders/redaphid/wip/lattice-bead/`, screened headlessly at fixed
seeds. Recent commits on top of that also add an **`nfc-writer`** tool that writes `2cb.pw/mon-<shape>`
short links per bead and shows a live preview, and a new **`satellites`** shader with audio-reactive
knobs — i.e. active work at the exact intersection of "physical bead" and "on-screen visual."

Key reusable mechanics for a 240×240 round display (documented in
`shaders/redaphid/wip/lattice-bead/HANDOFF.md`, read in full from the `pc-lab-tile` worktree):
- **The bead shape *is* a distance field already** (from the SDF composition in `japanese.py`), so
  baking a texture for a shader is "~20 lines," not a re-vectorization job.
- **Bake an SDF, not a 1-bit silhouette.** `NEAREST` texture filtering plus a hard silhouette gives
  staircase edges with no gradient to `smoothstep` against; the SDF-in-green-channel encoding stays
  smooth at any zoom/scale, which matters a lot on a small round panel where the motif will be
  magnified.
- **`REPEAT` wrap + the existing fold math tiles the motif for free** — useful if a round-display
  visual wants a lattice/kaleidoscope treatment rather than one centered crest.
- Rotational symmetry order per motif was measured (kiku=12, ume=5, kikyo=5, tomoe=3, hakkaku=8,
  kikko=6, mokko=4, matsukawa=3, katabami=3, suhama=3, ogi=3) — directly useful for picking which
  motifs read cleanly at very small sizes; `kiku`, `tomoe`, and `kikyo` were called out as strongest
  for legibility (high radial symmetry, large negative space), though `kiku`'s shader render was
  later found to be a dead/black render bug, unresolved as of `mogee`.
- No commit or file in `mogee`'s diff against `main` mentions ESP32, "round display," "240," or
  "badge" — this shader work is currently browser/wall-projection only. Porting the mon textures or
  shader logic to Chorus's 240×240 badge firmware has not been started anywhere found.

## 5. Open questions and a proposed next step

Open questions, all explicitly flagged as unresolved in the source material:
- Which mon are actually being printed/worn first — the "kiku, tomoe, kikyo" ranking is a guess in
  the handoff, not a confirmed shortlist.
- `kiku.frag` renders solid black with no GLSL error (unexplained, excluded from `mogee`).
- `hero.frag`'s background flashes on the beat, a known-bad defect not yet fixed as of `mogee`.
- No mon reference SVGs or a canonical "initial image" file were found persisted anywhere on disk —
  worth asking the user directly where (or whether) they still have the original 504 Wikimedia crest
  SVGs or the 1385×1385 reference renders mentioned in one conversation.

Proposed next step for reusing the same shapes on both the 3D print and the Chorus badge: the
11 baked SDF PNGs in `D:/Projects/pc-lab-tile/public/images/beads/` are already the smallest, most
portable asset — a single 1024×1024 texture per motif, in the same A/R/G/B convention Paper Cranes
already uses. Downsampling one of these (e.g. `mon-kikyo.png` or `mon-tomoe.png`) to a size the
ESP32-S3's RAM/flash budget can hold (it has no GPU; a 240x240 8-bit SDF is 57 KB, a 128x128 one is 16 KB), and sampling its green-channel SDF the same way
`beadDist()` does in `HANDOFF.md` §6, would let the round badge draw the identical crest — with the
identical distance-field math — as both the wall shader and the physical bead's outline, without
re-deriving the shape a third time.
