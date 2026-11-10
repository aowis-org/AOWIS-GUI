#pragma once

#include <aowis/config/object_store.h>

#include <QString>

// The GUI's AOWIS-CONFIG owner store ("gui"), opened once per process.
//
// Native: <QStandardPaths::GenericConfigLocation>/aowis/gui, i.e.
// $XDG_CONFIG_HOME/aowis/gui on Linux and %LOCALAPPDATA%\aowis\gui on
// Windows. QStandardPaths (not raw environment variables) is used so tests
// isolate the store by setting XDG_CONFIG_HOME before QApplication exists.
// Browser: /aowis/gui in the origin's localStorage; only the tab holding the
// writer lease can write (see AOWIS-CONFIG docs/api.md).
//
// Returns nullptr when the store cannot be opened (e.g. unsupported platform
// backend, insecure permissions); error, if given, receives a message that
// never contains stored data.
const aowis::config::OwnerStore *guiConfigStore(QString *error = nullptr);
