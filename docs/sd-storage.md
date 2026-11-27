# SD storage and data management

[中文](sd-storage_zn.md) | **English**

Product root: `/sdcard/metalio/e-ink/` ([sd_paths.h](../main/infra/storage/sd_paths.h))

| Path | Use |
|:---|:---|
| `.../books` | Book content, with optional category subdirectories; legacy sidecars remain supported |
| `.../wallpaper` | Wallpapers and shutdown art |
| `.../fonts`, `.../fonts_ttf` | Reader `.ef` fonts, extension `.fontpack` files and `.ttf` / `.otf` fonts |
| `.../fonts_ime/zh-CN` | Chinese input dictionary and licenses |
| `.../settings` | Wi-Fi configuration; `books/` stores reading positions, time and bookmarks; `sysmgmt/logs/` stores runtime logs and crash dumps |
| `.../temp/books` | `cache/` stores rebuildable covers, page counts and TXT indexes; `work/` stores pagination work files |
| `.../temp/bwchat` | `images/` stores chat images; `messages/` stores session JSON for the current boot, cleared on startup |
| `.../temp/wallpaper` | `work/` stores image processing and preview work files; `meta/` stores wallpaper names |
| `.../temp/transfer/work` | Download and preview work files owned by the transfer app |

SD mounting ensures the application layout, cleans pagination work files and recovers wallpaper backups left by interrupted saves. HTTP downloading and JPEG decoding clean their own temporary files on first use; failed cleanup prevents new work files from being created and can be retried. Wallpaper saves and deletions clean their own work files beforehand. It does not scan book libraries, wallpaper libraries or auxiliary cache libraries, move existing auxiliary data, or read or update a migration-complete flag. Existing auxiliary files remain at their current locations for both reading and writing. Book-side legacy files take precedence, followed by grouped and flat files in the new directories; files are created in the new layout only when no existing data is found. Wallpaper names support adjacent `.meta.json` and `.meta` files as well as existing files in the new metadata directory. New book auxiliary files and wallpaper metadata use `0`–`f` subdirectories based on the first hash character to limit directory size. Old files are not reorganized automatically; back up existing data before organizing it on a computer. Clearing `temp/` preserves reading positions and bookmarks. Covers and indexes can be rebuilt; missing wallpaper metadata falls back to the filename. New Baiwen sessions use the new directories directly. Old session caches and log directories are not moved automatically. Explicit book and wallpaper renames, moves and deletions maintain the corresponding auxiliary data. Existing `ui/` and `recordings/` directories remain untouched; the recordings directory is no longer created by default. Both upload instruction files remain at the application root.

The bookshelf, cloud library and file transfer page share book and wallpaper deletion handling. Deleting a book removes associated progress, bookmarks, covers, indexes, their fixed-name backups and recent-reading entries, and subtracts its contribution to the home-page reading-time and finished-book totals. Dated reading history is retained. Legacy covers shared by another format of the same title are kept while that book exists. Books being opened or read cannot be deleted; successful deletion cancels pending progress writes. Individual-book deletion cleans known paths without enumerating the book directory. Legacy pagination work files with unknown session IDs are left for removal with their containing directory. Recursive directory deletion processes books before sidecar files. Deleting a wallpaper also removes its name metadata, save backup and corresponding selections. Direct deletion through a computer USB drive does not run this application cleanup.

See [App Store](app-store_en.md) for ELF installation, resume and private-data transactions. Stop device file activity before USB export and safely eject after host writes. Direct deletion on a computer bypasses firmware business cleanup.
