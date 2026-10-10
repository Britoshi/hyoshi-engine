#include "songs/Osz.h"

#include "platform/Platform.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

namespace hyoshi::songs
{

namespace
{

// Replaces what Windows doesn't allow in a file name, so a folder named after an archive works on
// every platform.
std::string ToFolderName(std::string_view archiveName)
{
    std::string name(archiveName);
    for (char& c : name)
    {
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32)
        {
            c = '_';
        }
    }
    // Windows also drops trailing dots and spaces.
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
    {
        name.pop_back();
    }
    return name.empty() ? std::string("Imported map") : name;
}

std::string_view GetStem(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string ZipError(mz_zip_archive& zip)
{
    return mz_zip_get_error_string(mz_zip_get_last_error(&zip));
}

} // namespace

bool IsOszPath(std::string_view path)
{
    constexpr std::string_view EXTENSION = ".osz";
    return path.size() > EXTENSION.size() &&
           std::equal(EXTENSION.begin(), EXTENSION.end(), path.end() - static_cast<std::ptrdiff_t>(EXTENSION.size()),
                      [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

std::optional<std::string> ToMapRelativePath(std::string_view entry)
{
    std::string path(entry);
    std::replace(path.begin(), path.end(), '\\', '/');
    // Absolute paths, and drive letters (C:...).
    if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos)
    {
        return std::nullopt;
    }

    std::string result;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
        {
            end = path.size();
        }
        const std::string_view segment(path.data() + start, end - start);
        if (segment == "..")
        {
            return std::nullopt;
        }
        if (!segment.empty() && segment != ".")
        {
            if (!result.empty())
            {
                result += '/';
            }
            result += segment;
        }
        start = end + 1;
    }
    if (result.empty())
    {
        return std::nullopt;
    }
    return result;
}

Result<UnpackedOsz> UnpackOsz(const std::string& archivePath, const std::string& songsFolder)
{
    std::string folder = songsFolder;
    if (!folder.empty() && folder.back() != '/' && folder.back() != '\\')
    {
        folder += '/';
    }
    folder += ToFolderName(GetStem(archivePath));
    std::error_code error;
    if (std::filesystem::exists(folder, error))
    {
        return UnpackedOsz{.Folder = folder, .WasAlreadyThere = true};
    }

    Result<std::vector<std::byte>> bytes = platform::LoadFile(archivePath);
    if (!bytes)
    {
        return bytes.GetError();
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.Value().data(), bytes.Value().size(), 0))
    {
        return Error{"'" + archivePath + "' isn't a zip archive (" + ZipError(zip) + ")"};
    }

    // Check every entry before writing any, so a bad archive leaves nothing behind.
    struct Entry
    {
        mz_uint Index = 0;
        std::string Path;
    };
    std::vector<Entry> entries;
    Result<void> result;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint index = 0; index < count && result; ++index)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat))
        {
            result = Error{"Could not read '" + archivePath + "' (" + ZipError(zip) + ")"};
        }
        else if (!mz_zip_reader_is_file_a_directory(&zip, index))
        {
            if (std::optional<std::string> path = ToMapRelativePath(stat.m_filename))
            {
                entries.push_back({index, std::move(*path)});
            }
            else
            {
                result = Error{"'" + archivePath + "' has a file outside its folder: '" + stat.m_filename + "'"};
            }
        }
    }
    if (result && entries.empty())
    {
        result = Error{"'" + archivePath + "' holds no files"};
    }

    for (const Entry& entry : entries)
    {
        if (!result)
        {
            break;
        }
        const std::string path = folder + "/" + entry.Path;
        result = platform::MakeDirectory(path.substr(0, path.rfind('/')));
        if (!result)
        {
            break;
        }
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, entry.Index, &size, 0);
        if (data == nullptr)
        {
            result = Error{"Could not unpack '" + entry.Path + "' from '" + archivePath + "' (" + ZipError(zip) + ")"};
            break;
        }
        result = platform::SaveFile(path, std::span(static_cast<const std::byte*>(data), size));
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    if (!result)
    {
        // The folder wasn't there before, so all it holds is this unpack's files. Left in place,
        // it would stop the set from being added again.
        std::filesystem::remove_all(folder, error);
        return result.GetError();
    }
    return UnpackedOsz{.Folder = folder};
}

} // namespace hyoshi::songs
