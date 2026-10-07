# azahar-3ds-accurate

A fork of [Azahar](https://github.com/azahar-emu/azahar) that behaves more like
real 3DS hardware where the
[New Love Plus+ English patch](https://github.com/czyrustuazon/NewLovePlusPlusEngPatcher)
needs it. Stock Azahar is forgiving in ways hardware is not, so a patch can work
in the emulator and crash on a console. This fork closes those gaps so crashes
show up on a PC first.

Base: upstream `7fbe541c0`, the repo's first commit. Branch `upstream` holds
pristine upstream snapshots; everything NLPP-specific is in the commits on
`main` above it.

## Changes

| Change | Why |
|--------|-----|
| `File::OpenLinkFile` clones the source session (offset, size, subfile) and keeps the shared backend open while a clone lives | Stock Azahar returned the whole extra-data blob for the clone. NLPP read it as a small record and heap-walked garbage (Communications / Game Start hang). Hardware clones the handle correctly. |
| Exception reports include `FAR` / `DFSR` | Same fields as a Luma3DS ARM11 dump, so emulator and hardware crashes compare directly. |
| `NLPP_EMU_NAME_WALK_ABORT=1` | Replays Luma dump 2026-10-01 18:36: forces the bad directory offset on the first layout name lookup, then data-aborts the vanilla `ldrh` at `0x644D78`. |
| `NLPP_EMU_MENU_VT=1` | Replays Luma dump 2026-10-01 19:58: one null object at `0x2EEDA4`. |
| `NLPP_EMU_PANE_FLAG=1` | Replays Luma dump 2026-10-03 00:50: one null `r6` at the `beq` `0x3C5FCC`, so the vanilla `ldrb` at `0x3C6070` faults with FAR `0x5F`. |

Each replay fires once and logs `NLPP <name>: one-shot ...`. A patched
`code.bin` with the guard in place keeps running; a build that lost the guard
logs `Exception Type: Data Abort`. Turn on **Debugging → Enable exception
handler** (or `enable_exception_handler=true` in `qt-config.ini`), or aborts are
not reported.

## Used by

- EngPatcher `ab_test/` (A/B instances; `make.ps1 build-azahar`, `make.ps1 smoke`)
- EngPatcher `tools/smoke_boot_azahar.py --inject name-walk,pane-flag,menu-vt`
- nlpp-gold-maker `smoke.yml` (boots every published gold bake on the runner)

EngPatcher keeps these changes as `ab_test/patches/azahar-*.patch`, exported
by its `tools/export_azahar_patches.py` from `git diff $(merge-base main
upstream)..main`. Its `tests/test_export_azahar_patches.py` fails when they
drift.

## Rules for agents (hard requirement)

Set by EngPatcher `CLAUDE.md` and `.cursor/rules/azahar-3ds-accurate.mdc`:

- Hardware-accuracy changes to Azahar land here first, as commits on `main`.
- **Commit only. Never push.** The owner pushes.
- Then run `python tools/export_azahar_patches.py` in EngPatcher and commit the
  regenerated patches there.
- Never commit to `upstream` except a pristine upstream import (below).
- Never open pull requests or issues on azahar-emu/azahar from this fork.
  Azahar's [AI-POLICY.md](AI-POLICY.md) bans AI-written contributions of any
  copyrightable size; code here is not eligible for upstreaming.

## Updating from upstream

History here starts fresh, so pull upstream in as a snapshot on `upstream`,
then merge it into `main`:

```bash
git remote add azahar https://github.com/azahar-emu/azahar.git   # once
git fetch azahar
git switch upstream
git read-tree -u --reset azahar/master   # exact upstream tree, submodules included
git commit -m "Import upstream $(git rev-parse --short azahar/master)"
git switch main
git merge upstream                        # resolve NLPP conflicts here
```

Then re-export the patches in EngPatcher and rebuild Azahar
(`make.ps1 build-azahar`, runner `setup-smoke-runner.sh`).
