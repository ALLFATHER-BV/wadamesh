// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Files between the UI and the visitor (CAP_FILE_HANDOFF): a device writes a backup to
// its own storage and the user copies it off the card; in a browser the file is handed
// over (the browser downloads it) and taken in (the browser's file picker).
//
//   browser  download = a Blob + <a download> from the page (the UI's button press is
//            the user gesture); picker = <input type=file>, the chosen file is written
//            into this backend's SPIFFS (web/shell/app.js wadaPickFile)
//   native   a download is copied to $WADA_DATA_DIR/downloads/; there is no picker
#include <FS.h>
#include <stddef.h>

// Hand `path` on `fs` to the visitor under `download_name`.
void boardHandOffFile(fs::FS& fs, const char* path, const char* download_name);
// Ask the visitor for a file (`accept`: the picker's filter, e.g. ".json"); it lands in
// SPIFFS at "/<its name>" and boardTakePickedFile then reports it, once.
void boardPickFile(const char* accept);
bool boardTakePickedFile(char* path, size_t cap);
