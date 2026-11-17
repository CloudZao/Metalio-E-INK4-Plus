# App Store

[简体中文](app-store.md)

The App Store connects to the cloud app store and installs ELF apps in `metalio/e-ink/apps/` on the SD card. The home app grid lists App Store after built-in apps, followed by installed apps, continuing onto subsequent pages when necessary. The Store displays all apps in a single-column horizontal list with icons, names, versions, package sizes and Install or Open actions. Tap a row for details. Information stays above the fixed bottom actions. Pages have no on-screen back arrows. A short HOME or Back press closes one level at a time; holding HOME returns to the home screen. Returning from an app opened in details restores those details. Loading, refresh and operation results appear over the header without reserving list space or covering app rows. Download percentage appears inside the corresponding row action, which cancels on tap. Details use the bottom primary action for the same status.

## Usage

1. The platform must confirm that the device's fixed MAC has a valid binding, its board and chip are registered, and a published app supports the current firmware and ABI. The deployed API and storage service must be available.
2. The store URL is managed by `cloudzao_app_store_url` in `main/cloudzao_endpoints.c`.
3. The toolbar above the list shows the filter and app count on the left and an icon-only refresh action on the right. Entry and refresh first read installed apps from SD, then fetch the store catalog. Refresh retains existing apps, filters and the scroll position within the available content range. A failed request keeps local apps and the existing catalog; installed apps remain runnable. Empty states are hidden while loading, and result notices dismiss automatically. Filter opens a bordered floating panel with All / Not installed / Installed / Downloading without moving the app list. Selecting an option immediately filters and closes the panel. Check for updates in app details fetches the latest catalog. A newer compatible version opens a centered dialog showing its version number; installation starts only after confirmation. Cancel or tap outside the dialog to keep the installed version. Otherwise, the Store reports Already up to date. Tap anywhere outside the installation filter panel to dismiss it. Installed apps run offline, and unpublishing does not remove local copies.

The store and details hold the same foreground network requirement as the assistant. Ordinary idle power saving does not disconnect them, while normal inactivity standby remains available outside installation. Leaving or entering standby releases the page requirement and cancels in-flight requests with file cleanup; waking restores it. Manually disabled Wi-Fi remains disabled.

## Files and updates

Each installation contains `metalio/e-ink/apps/<app-name>/manifest.json` and `<app-name>.elf`, for example `metalio/e-ink/apps/计算器/计算器.elf`. The server-provided Chinese app name is used. Chinese characters and spaces are preserved; FAT-invalid characters, leading dots and trailing dots/spaces are handled, and length is limited at UTF-8 boundaries. Different app IDs cannot overwrite the same name. The local manifest field `installedName` fixes the directory and ELF name across updates and language changes. App IDs and SHA256 remain the identity and integrity checks. Download length, SHA256 and ELF structure must pass verification, and launch verifies the file again. The manifest changes only after the new ELF has been written and verified, using the existing file commit and backup recovery mechanism. Old ELF files are cleaned after the switch. The online catalog displays at most 16 apps per page, with Previous / Next navigation and one online page retained at a time. There is no fixed cumulative app-count limit; each ELF is limited to 32 MiB. SD access follows the existing lifecycle permissions during USB export and standby.

The server package `size` field must be a decimal integer string, such as `"1076"`; JSON numbers are rejected. Catalogs, details, update checks and download manifests use the same rule, with a range of 1 to 33554432 bytes. Chunk digest `offset` values also require decimal integer strings, including `"0"`; `index`, `length` and `chunkSize` remain JSON numbers. Local installation manifests and resume checkpoints continue to store numeric lengths.

### Temporary files and recovery

Store work files live in `metalio/e-ink/temp/app_store/<app-name>--<appId>/`. Names are sanitized for FAT and limited at UTF-8 boundaries; the app ID distinguishes duplicate names. Existing directories are reused by ID when an app is renamed.

- `download.part`: ELF download data.
- `download.part.json`: checkpoint of verified download blocks.
- `download.part.json.part`: checkpoint being written.
- `download.part.json.cloud-backup`: checkpoint replacement backup, restored or cleaned when downloading again.
- `manifest.json.part`: installation manifest awaiting commit.

Failure, cancellation and leaving the page retain resumable data. Retrying verifies complete blocks again. Successful installation or app removal cleans these work files and the empty directory, leaving unknown files untouched. Work directories can be deleted manually while no installation is active; subsequent installation downloads the data again.

`manifest.json.cloud-backup` provides manifest recovery. During an update, the previous ELF is retained as `<old-sha256>.elf` until the new manifest commits. Failure or cancellation before commit restores the previous version and retains the new package for resuming. Recovery after a power interruption uses the manifest digest to select the matching ELF rather than relying only on file size. Private state and recovery backups live in `metalio/e-ink/settings/apps/<app-name>--<appId>/`, with write staging files in the Store work directory. Updates retain persistent state. Uninstall deletes the app's state, recovery backups and write staging files, so reinstalling starts with fresh state. State cleanup failure reports a storage error and retains the installation manifest so removal can be retried. File formats and read, write, recovery and cleanup rules are documented in the [private storage interface](../tools/elf_app/DEVELOPMENT.md#private-storage-xz_app_cap_storage).

## App icons

Icons are uploaded, verified and updated with the ELF and shared by home, Store and details. Encoding, source requirements and builds are documented in the [ELF developer guide](../tools/elf_app/DEVELOPMENT.md#6-icons-and-content-assets).

## App ABI

App entry, compatibility checks, coding rules, builds, publishing, resources and runtime limits are maintained in [ELF app developer guide](../tools/elf_app/DEVELOPMENT.md). [app_api.h](../main/app_store/app_api.h) is authoritative for signatures.

### Foundation services

See [capability interfaces](../tools/elf_app/DEVELOPMENT.md#5-capability-interfaces) for time, timers, private storage, widgets, keyboard, exit callbacks, notifications and keep-awake, including results and limits.

## Installation queue and resources

Apps install in selection order, with one queue entry per app. Waiting apps show Queued. Cancel removes a waiting entry or stops the active download; the next app starts after cleanup. Download percentage appears inside the app action at most once per second. Queued, verifying and cancelling states use the same button. Tapping cancels a waiting or active download; the bottom primary action in details behaves identically. A failed item does not stop the queue and can be selected again. Active and queued installations pause inactivity standby. Leaving the Store clears waiting entries and cancels the active download, retaining verifiable partial files and checkpoints while preserving completed installations. Cancellation waits for the current network wait or SD block operation to return. Cancellation before manifest switching preserves the previous version; after the manifest commits, installation cleanup completes without rolling back the installed version. Downloads use 256 KiB HTTP ranges. After interruption, existing complete blocks are verified again and reused; unverified tails are downloaded again.

App content can be read on demand from the ELF. See [developer resource documentation](../tools/elf_app/DEVELOPMENT.md#6-icons-and-content-assets) for formats, builds and memory limits.

See [sample apps](../tools/elf_app/README.md) for sources and build instructions.

The Downloading filter includes active downloads, queued apps and verification. Completed or cancelled tasks leave the list automatically.

## App launch

Launch centers the ELF icon, app name and loading message, with 16px between the icon and name and 40px between the name and message. App content appears only after the loading page has reached the display and ELF initialization succeeds. On failure, the loading page remains with an error. Apps use the full area below the status bar. The host adds no app-name heading; apps decide whether to draw their own title.

## App information

The list displays the name, summary, then version and package size, with 12px between lines. Summaries use the next smaller font size and a single line with an ellipsis for overflow. Missing summaries do not add an empty line. Details use a scrollable information area for version, size, category, languages, provider, last updated time, description and release notes, while bottom actions occupy a separate fixed area. Vertical drags starting in the information or action area scroll the details without activating buttons. Description and release notes each expand or collapse independently by tapping their heading and are expanded by default. Headings match other information labels; body text uses the next smaller font size.

Summaries and categories can be edited on the server without uploading the ELF again. Details include bilingual descriptions and release notes, supported languages, provider and last updated date. Metadata is saved with the installation manifest for offline viewing. Older manifests remain readable; online information supplements them only when both version and SHA256 match. Missing values display Not provided.

The toolbar above the list contains filtering, the app count for the active filter and an icon-only refresh action. The toolbar, list and details use outlined surfaces with a subtle offset shadow; app icons remain 128×128. Details share one outer frame for information and fixed actions, separated by a single-pixel line inset on both sides.

The store retrieves the catalog, app details and updates, and downloads app packages. The server checks device binding and compatibility. Installation state stays local; installation lists and results are not synchronized.

## Diagnostic logging

`AppStore` logs local scans, catalog requests, installation, removal and ELF loading. `AppRuntime` logs app entry execution. INFO covers key starts, completion and normal cancellation; WARN covers recoverable conditions such as network unavailability or skipped damaged entries; ERROR covers invalid manifests, integrity, storage and launch failures. DEBUG adds details such as verification stages, is disabled by default and should be enabled per module for diagnosis in a build that permits DEBUG. Download progress is not logged per chunk.

Failures include `operation`, `stage`, HTTP status, numeric error codes and relevant byte counts, excluding URLs, query parameters, headers, bodies and credentials. For HTTP business errors, inspect `operation=http`, `http_status`, the allowlisted `code` and validated `request_id`. Transport failures include `transport_error`; `json`, `schema` and `item` identify invalid API data. App identifiers are logged only after format validation. Normal cancellation is not an error.

Read-only requests and individual chunks allow up to three additional attempts with 1/2/4-second backoff and jitter. HTTP 429/503 honor a longer `Retry-After`. Delays over 60 seconds stop automatic retries for a later manual retry. An expired catalog snapshot restarts at page one; 412 refreshes the same release manifest; 416 checks the total size and discards the checkpoint without treating the download as complete. Checkpoints store package identity, size, digest, ETag, chunk size and verified prefix; existing blocks are verified again on resume.
