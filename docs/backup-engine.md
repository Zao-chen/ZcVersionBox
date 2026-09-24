# Backup engine

The desktop application is the only writer for one data directory. File arguments and subsequent launches are forwarded over a user-only local socket. `BackupService` schedules at most two workers and serializes overlapping source paths, including reads during restore, import, and pull. The GUI never runs Git or copies backup content.

## Storage

The default root is `QStandardPaths::AppLocalDataLocation/backup-engine`. `--data-dir <directory>` selects an isolated profile, including its settings and instance lock. The old Documents directory is neither read nor migrated; users add their sources again.

Each `objects/<uuid>/` contains `config.json`, `repository.git`, optional `transaction.json`, and AI task records. A configuration has format `1`, an absolute local source path, remote URL, and a pending-import flag. Unknown formats fail explicitly. Absolute source paths never enter Git history.

The repository is bare, uses SHA-1 object IDs, and publishes snapshots at `refs/heads/main`. A snapshot tree contains `.zcversionbox.json` and `payload/`. The version-1 manifest records the root kind/name, file object IDs, byte lengths, executable bits, and directory paths. Single-file snapshots use `payload/file`; directory snapshots use relative paths. Metadata is validated against the Git tree before export. Notes use `refs/notes/zcversionbox` and contain separate manual and AI fields.

Files are hashed and streamed in bounded chunks. `fast-import` constructs a candidate at a private pending ref; `update-ref` publishes it only if the previous `main` still matches. Unchanged blobs are reused. There is no persistent checkout, working copy, `git add`, clean filter, or newline conversion.

All regular files and empty directories are included, regardless of ignore files. Git metadata and application data are excluded. Symbolic links, reparse points, special files, unsafe paths, and names incompatible with the target filesystem are rejected. ACLs and extended attributes are outside this format.

## Monitoring and scheduling

macOS uses FSEvents with a dispatch queue; Windows uses overlapped ReadDirectoryChangesW. Monitoring starts before the initial scan, so changes during the scan remain queued. Event paths update the affected portion of the content index. Debounce is two seconds with a thirty-second maximum scheduling delay; that delay is not a promise to snapshot an actively changing file.

There is no periodic polling or alternate monitor. A reported event gap invalidates the index and schedules a complete reconciliation. Monitor failure is visible and requires a successful restart. Missing source roots retain their history and can resume when the parent monitor reports their return.

Transient errors retry after 5, 15, then 60 seconds. Permission/configuration errors and conflicts wait for user action. Cancellation is checked during scans, streaming, and Git waits. Shutdown stops monitors and cancels work at safe points.

## Transactions

The journal is written atomically before repository publication or source replacement. The Git `main` reference is the commit point.

* Snapshot: construct candidate, record its ID, publish with compare-and-swap, remove the private ref and journal.
* Restore: protect current source changes as a snapshot, export the chosen tree into a sibling staging path, recheck the source, rename the source into a rollback path, install the staged tree, then publish a new restore commit.
* Pull: protect local changes, fetch the remote main and notes, reject divergence, and use the same replacement transaction for a fast-forward update of the source and `main`.
* Rebuild: prepare a new repository, record both heads, rename the old repository aside and install the new one. Rebuilding never implicitly overwrites a remote.
* Remove: move the complete object directory into application trash before cleanup. Source files and remote repositories are untouched.

On restart, an unpublished transaction rolls back; a published transaction finishes cleanup. If a rollback copy, source, or Git ref has changed unexpectedly, recovery stops and retains the materials. This protects against process interruption; it does not provide an application-consistent snapshot of live databases or a filesystem-wide point-in-time snapshot.

Publication and cleanup are separate outcomes: a cleanup failure reports the published revision and schedules only recovery, so retrying cannot create duplicate restore commits. Existing `.git` entries move with the source replacement transaction and are never deleted with the rollback tree. A target that cannot contain their parent directories is a conflict. In-place replacement of an ancestor of the application's data directory is refused; export remains available, avoiding movement of the live repository or legacy data.

## Sync, annotations, and interfaces

Normal pushes are non-forcing and explicitly transfer main and notes atomically. An explicit remote reset uses an observed main ID with `force-with-lease`. Unsupported remote capabilities fail rather than selecting another transfer protocol. `ConflictInfo` identifies the operation, category, base/local/remote IDs, and relevant source paths. `resolveConflict` currently returns Unsupported; there is no automatic merge or conflict editor.

All UI operations go through `BackupService::submit(Request)` and receive `TaskResult`, with a request ID, tracking ID, completion state, error category, retryability, revision, statistics, and operation data. Query results are bound to their original selection. Snapshots remain immutable; editing notes never amends them.

AI jobs are separate from snapshot success. Their queue and failures are stored outside Git, requests have a fifteen-second timeout, and completed descriptions are written as notes. Provider configuration uses only the new profile's current provider fields; there is no legacy settings migration or fallback commit message.

Notes require version 1 with string-valued manual/AI fields and are limited to 1 MiB. Fetch validates the remote note tree before changing local history or source files. Malformed or unknown notes fail explicitly and are never replaced with an empty annotation. Removing or rebuilding an object cancels its outstanding AI work.

## Development and checks

Core tests link Qt Core, Network, and Test only:

```sh
cmake -S . -B build/core -G Ninja -DZCVERSIONBOX_BUILD_APP=OFF -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/6.6.3/kit
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

The fault hook on `Engine` and injected monitor/engine factories make publication, source replacement, event gaps, retries, and UI responsiveness testable without touching user data. Tests cover local bare remotes, notes divergence, stale leases, a 10,000-file incremental fixture, raw byte preservation, and interruption at transaction boundaries. `backup-checks.yml` builds and tests both macOS and Windows.
