# Applying ZIP patches and regenerating source snapshots

This repository accepts **file-replacement ZIP patches**, applied with
`tools/apply_patch.sh`. A patch contains only new or modified files, arranged
under the repository's top-level directory name. It is not a Git patch and
must not be confused with the larger source snapshot ZIPs (`gui.zip`,
`model.zip`, and `epanet.zip`).

## Apply a patch

From the root of an `AOWIS-SERVER-GUI` checkout:

```bash
./tools/apply_patch.sh ~/Downloads/aowis-server-gui-example-change.zip
```

The script takes **one** ZIP filename argument. Its basename must match
`aowis-server-gui-*.zip`; a MODEL or EPANET ZIP with a different filename
prefix is intentionally rejected. The ZIP's member paths must begin with the
**checkout folder's name**: `AOWIS-SERVER-GUI/` for a default clone,
`AOWIS-GUI/` for a checkout in a folder of that name. The examples below use
`AOWIS-SERVER-GUI/`. Files are extracted relative to the checkout's **parent**
directory, independent of the caller's current working directory. Existing
files with matching paths are overwritten.

For example, a valid patch could contain:

```text
AOWIS-SERVER-GUI/
AOWIS-SERVER-GUI/src/tabs/tab_projects_widget.cpp
AOWIS-SERVER-GUI/tests/projects_inspector_dock_test.cpp
AOWIS-SERVER-GUI/docs/apply-patches.md
```

To preview paths or verify ZIP integrity before applying:

```bash
unzip -Z -1 ~/Downloads/aowis-server-gui-example-change.zip
unzip -tq   ~/Downloads/aowis-server-gui-example-change.zip
```

**Apply patches only from a trusted source.** `apply_patch.sh` checks ZIP
integrity and some path rules, but it is not a general-purpose security sandbox
for hostile archives.

## What happens when applying

1. Check the patch filename, test the archive, and validate its entries,
   including the optional deletion list `<folder>/.aowis-patch-delete.txt`
   (one repository-relative file path per line; no absolute paths, `..`,
   `.git`, directories, symlinks, or paths the patch also adds).
2. Extract all files, overwriting files at the same relative paths, then
   delete the listed files. The deletion list itself never stays in the
   checkout.
3. Set modification times of the extracted files and directories to the
   **current application time**, instead of keeping the old ZIP timestamps.
   This is important for CMake/Ninja incremental builds.
4. Classify the **files** in the patch by repository path, ignoring
   directory-only ZIP entries, and regenerate the affected source snapshots.
5. Exit successfully if applying and all required snapshot regenerations work.

Snapshot routing:

| Changed path within `AOWIS-SERVER-GUI/` | Script called | Archive generated |
| --- | --- | --- |
| Ordinary GUI files, including `src/`, `docs/`, `tests/`, `tools/`, `README.md` | `tools/zip_gui_project.sh` | `../gui.zip` |
| `external/AOWIS-SERVER-MODEL/` | `tools/zip_model_project.sh` | `../model.zip` |
| `external/AOWIS-SERVER-EPANET/` | `tools/zip_epanet_project.sh` | `../epanet.zip` |

Each affected script runs **once**. A mixed patch can regenerate all three
archives. Unaffected archives remain unchanged; directory-only patches do not
regenerate any archive. The scripts execute from the **updated checkout**, so
a patch containing changes to the ZIP snapshot scripts uses their newly
installed versions.

`gui.zip`, `model.zip`, and `epanet.zip` are **full source snapshots**, not
incremental patch archives. They are written in the directory containing the
`AOWIS-SERVER-GUI` checkout, *not* inside the checkout. Each snapshot script
applies its own filters: for instance, `zip_gui_project.sh` excludes `assets/`,
build products, `.git/`, and separately snapshotted MODEL and EPANET trees.
An asset changed by a patch is still extracted, but do **not** assume it will
appear in `gui.zip`: check the snapshot script's filters before distributing a
snapshot. The patch ZIP itself must carry any changed asset files.

The snapshot scripts **do not compile the application or run tests**. Do that
separately after patching, for example:

```bash
./compile_linux_tests.sh --both
```

## Create a patch ZIP

Build the patch in a staging directory so it contains **only added or changed
files**, never an entire checkout. Keep the normal repository-relative paths:

```bash
stage="$(mktemp -d)"
mkdir -p "$stage/AOWIS-SERVER-GUI/src/tabs"
cp src/tabs/tab_projects_widget.cpp \
   "$stage/AOWIS-SERVER-GUI/src/tabs/tab_projects_widget.cpp"
(
    cd "$stage"
    zip -qr ~/Downloads/aowis-server-gui-example-change.zip AOWIS-SERVER-GUI
)
rm -rf -- "$stage"
```

Add other modified files in the same way. Preserve executable permissions for
scripts. Always inspect the final ZIP member list and check ZIP integrity.
Do not include unrelated files, builds, cache directories, or old snapshots.
New and modified files have fresh application-time modification timestamps
when the recipient applies the patch, even if their archived timestamps differ.

**Important limits:** These ZIPs replace, create and (with the deletion
list) remove files; they cannot express Git renames or three-way merges. The script does not compare
with Git, check for uncommitted changes, or create a backup. Review local
changes or commit/stash work before applying a patch that may overwrite it.
Patches touching nested Git submodules may also require separate Git bookkeeping
and commits, because raw ZIP entries do not update Git submodule gitlinks.

## Failure handling and troubleshooting

- **Invalid filename / path / corrupt ZIP:** Validation fails before extraction.
- **A snapshot script fails:** The patch files are **already applied**; the
  script reports the failed snapshot and exits nonzero. It still tries other
  affected snapshot scripts. Fix the cause and rerun the relevant `zip_*`
  script manually. Do not interpret an error as a rollback.
- **Missing `zip` or `unzip`:** Install the standard ZIP command-line tools.
  The script also uses Unix shell utilities (`bash`, `realpath`, `touch`, etc.).
- **Out-of-date build after extracting:** Confirm the patch was applied with
  `apply_patch.sh` (which refreshes mtimes), then run the regular build/test
  command. A snapshot refresh alone is not a build.
- **New `apply_patch.sh` delivered by a patch:** The invocation already running
  began with the older installed script; new apply behavior is available on
  subsequent invocations. If the old script predates automatic snapshot
  generation, regenerate needed snapshots manually for that first upgrade.

## Regression tests

The snapshot-routing behavior can be tested without Qt or CMake:

```bash
bash tests/apply_patch_script_test.sh
```

The test uses a disposable checkout and exercises GUI-only, MODEL-only,
EPANET-only, mixed and directory-only ZIPs, refreshed timestamps, and snapshot
failure reporting. It does not alter your working tree or project snapshots.
