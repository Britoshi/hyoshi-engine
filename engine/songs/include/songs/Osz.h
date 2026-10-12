#pragma once

#include "core/Result.h"

#include <optional>
#include <string>
#include <string_view>

namespace hyoshi::songs
{

struct UnpackedOsz
{
    std::string Folder;
    // The folder was already there, so nothing was written.
    bool WasAlreadyThere = false;
};

// Unpacks an osu! map set (.osz, a zip archive) into its own folder in songsFolder, named after the
// archive ("123 Artist - Title.osz" unpacks to "123 Artist - Title"); an archive whose files are all
// in one folder (a StepMania pack's) unpacks that folder's contents. If that folder is already
// there, it's left alone: the set was added before, or the folder is the player's own, and either
// way its files may have been edited. An archive with an entry that would land outside its folder
// (an absolute path, or "..") is refused before anything is written, and a folder left half
// written by a failed unpack is removed again. Safe to run on a worker.
Result<UnpackedOsz> UnpackOsz(const std::string& archivePath, const std::string& songsFolder);

// True for a path ending in .osz, in any case.
bool IsOszPath(std::string_view path);

// A map set UnpackOsz can add: an osu! set (.osz), a Phigros chart package (.pez, Phira's), a zip
// (.zip, as StepMania packs and songs come), or a file without an extension that is a zip archive,
// as Phira names its downloads. Reads the file's first bytes for the last.
bool IsMapArchive(const std::string& path);

// An entry's path inside its map folder, with backslashes as slashes and "." segments dropped, or
// nothing if it would leave the folder. Exposed for tests.
std::optional<std::string> ToMapRelativePath(std::string_view entry);

} // namespace hyoshi::songs
