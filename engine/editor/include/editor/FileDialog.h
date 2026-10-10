#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hyoshi::editor
{

// A file type a dialog offers: its name ("Charts") and extensions without dots, separated by
// semicolons ("json;osu"), or "*" for any file.
struct FileFilter
{
    std::string Name;
    std::string Extensions;
};

// The answer to a file dialog: the chosen path, or nothing when it was cancelled or failed.
using FileDialogDone = std::function<void(std::optional<std::string> path)>;

// The system's file dialogs (ADR 0002, milestone 1). They don't block: `done` runs on the main
// thread during a later frame of the editor, once the dialog closes. `start` is a folder or file
// to open at, or empty.
void ShowOpenFileDialog(std::vector<FileFilter> filters, const std::string& start, FileDialogDone done);
void ShowSaveFileDialog(std::vector<FileFilter> filters, const std::string& start, FileDialogDone done);

// Runs the answers of dialogs that have closed. The editor calls it every frame.
void DeliverFileDialogs();

} // namespace hyoshi::editor
