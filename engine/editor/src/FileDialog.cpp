#include "editor/FileDialog.h"

#include "core/Log.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>

#include <memory>
#include <mutex>
#include <utility>

namespace hyoshi::editor
{

namespace
{

// One open dialog: SDL needs its filters until it answers, and may answer on another thread.
struct Request
{
    std::vector<FileFilter> Filters;
    std::vector<SDL_DialogFileFilter> SdlFilters;
    FileDialogDone Done;
};

struct Answer
{
    FileDialogDone Done;
    std::optional<std::string> Path;
};

std::mutex answersMutex;
std::vector<Answer> answers;

void SDLCALL OnDialogClosed(void* userData, const char* const* files, int /*filter*/)
{
    std::unique_ptr<Request> request(static_cast<Request*>(userData));
    std::optional<std::string> path;
    if (files == nullptr)
    {
        HYOSHI_LOG_WARN("File dialog: {}", SDL_GetError());
    }
    else if (files[0] != nullptr)
    {
        path = files[0];
    }
    const std::scoped_lock lock(answersMutex);
    answers.push_back({std::move(request->Done), std::move(path)});
}

std::unique_ptr<Request> MakeRequest(std::vector<FileFilter> filters, FileDialogDone done)
{
    auto request = std::make_unique<Request>();
    request->Filters = std::move(filters);
    request->Done = std::move(done);
    for (const FileFilter& filter : request->Filters)
    {
        request->SdlFilters.push_back({filter.Name.c_str(), filter.Extensions.c_str()});
    }
    return request;
}

} // namespace

void ShowOpenFileDialog(std::vector<FileFilter> filters, const std::string& start, FileDialogDone done)
{
    std::unique_ptr<Request> request = MakeRequest(std::move(filters), std::move(done));
    Request* owned = request.release();
    SDL_ShowOpenFileDialog(OnDialogClosed, owned, nullptr, owned->SdlFilters.data(),
                           static_cast<int>(owned->SdlFilters.size()), start.empty() ? nullptr : start.c_str(), false);
}

void ShowSaveFileDialog(std::vector<FileFilter> filters, const std::string& start, FileDialogDone done)
{
    std::unique_ptr<Request> request = MakeRequest(std::move(filters), std::move(done));
    Request* owned = request.release();
    SDL_ShowSaveFileDialog(OnDialogClosed, owned, nullptr, owned->SdlFilters.data(),
                           static_cast<int>(owned->SdlFilters.size()), start.empty() ? nullptr : start.c_str());
}

void DeliverFileDialogs()
{
    std::vector<Answer> ready;
    {
        const std::scoped_lock lock(answersMutex);
        ready.swap(answers);
    }
    for (Answer& answer : ready)
    {
        if (answer.Done)
        {
            answer.Done(std::move(answer.Path));
        }
    }
}

} // namespace hyoshi::editor
