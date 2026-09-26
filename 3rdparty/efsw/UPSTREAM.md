efsw 1.7.2

Upstream: https://github.com/SpartanJ/efsw
Tag: https://github.com/SpartanJ/efsw/tree/1.7.2
Commit: 41ddf6822f2d0dec7e14fafa09c4cef391137b20 (2026-08-29)
License: MIT (see LICENSE).
Archive SHA-256: ccb91ea041223dc58513726ff8156eaa0190f75215dc00d7cba2daf3cd900173
Retrieved: 2026-09-26.

Vendored: include/, src/efsw/, CMakeLists.txt, efswConfig.cmake.in,
LICENSE, README.md.
Linked statically; upstream examples and installation are disabled.
Configuration/build does not download dependencies.

The application registers logical directories from its own background source
enumeration, shares registrations, and forwards events through a bounded Qt
adapter. Windows/macOS merge descendant registrations into recursive native
roots. Linux uses non-recursive watches to preserve selection rules and avoid
overlapping inotify registrations. Windows uses default FILE_SHARE_DELETE;
avoiding open child-directory handles also permits source ancestor renames.
Missed-event callbacks invalidate coverage and trigger a fresh check; independent
SHA-256 checks remain the correctness fallback.

Local patch: src/efsw/WatcherWin32.cpp forwards Modified events without the
upstream FileInfo-based size/time deduplication, in both Windows callback paths.
That query opens the changed file to obtain its identity; it caused concurrent
QSaveFile record replacements to fail with ERROR_ACCESS_DENIED in the isolated
200-save test. The application's bounded event adapter and debounce scheduler
already coalesce events. Removing the query also avoids suppressing same-size,
same-mtime content edits. No other upstream files are modified.
