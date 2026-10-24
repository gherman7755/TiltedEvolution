#include <Services/MusicTools.h>

#include <TiltedCore/Filesystem.hpp>

namespace
{
constexpr wchar_t kYtDlpUrl[] = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
constexpr wchar_t kDenoUrl[] = L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip";
constexpr size_t kMaxSearchResults = 20;
constexpr uintmax_t kMaxCacheBytes = 2ull * 1024 * 1024 * 1024;

std::filesystem::path GetMusicDirectory() noexcept
{
    return TiltedPhoques::GetPath() / L"music";
}

std::filesystem::path GetToolsDirectory() noexcept
{
    return GetMusicDirectory() / L"tools";
}

std::filesystem::path GetYtDlpPath() noexcept
{
    return GetToolsDirectory() / L"yt-dlp.exe";
}

std::filesystem::path GetDenoPath() noexcept
{
    return GetToolsDirectory() / L"deno.exe";
}

std::filesystem::path GetSystemTool(const wchar_t* apName) noexcept
{
    wchar_t systemDirectory[MAX_PATH]{};
    GetSystemDirectoryW(systemDirectory, MAX_PATH);
    return std::filesystem::path(systemDirectory) / apName;
}

std::wstring ToWide(const std::string& acText) noexcept
{
    if (acText.empty())
        return {};

    const int cSize = MultiByteToWideChar(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), nullptr, 0);
    std::wstring result(cSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), result.data(), cSize);
    return result;
}

std::string ToUtf8(const std::wstring& acText) noexcept
{
    if (acText.empty())
        return {};

    const int cSize = WideCharToMultiByte(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), nullptr, 0, nullptr, nullptr);
    std::string result(cSize, '\0');
    WideCharToMultiByte(CP_UTF8, 0, acText.data(), static_cast<int>(acText.size()), result.data(), cSize, nullptr, nullptr);
    return result;
}

/**
 * @brief Quotes a command line argument the way CommandLineToArgvW and the C runtime read it back.
 */
std::wstring Quote(const std::wstring& acArgument) noexcept
{
    std::wstring result = L"\"";
    size_t backslashes = 0;

    for (const wchar_t c : acArgument)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (c == L'"')
            result.append(backslashes * 2 + 1, L'\\');
        else
            result.append(backslashes, L'\\');

        backslashes = 0;
        result.push_back(c);
    }

    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring YtDlpCommand() noexcept
{
    return Quote(GetYtDlpPath().wstring()) + L" --ignore-config --no-warnings --encoding utf-8 --color never --js-runtimes " + Quote(L"deno:" + GetDenoPath().wstring());
}

/**
 * @brief Remembers the last error yt-dlp printed, to show it to the player.
 */
void CaptureError(const std::string& acLine, std::string& aError) noexcept
{
    if (acLine.starts_with("ERROR:"))
        aError = acLine.substr(6);
}

std::vector<std::string> Split(const std::string& acLine, char aSeparator) noexcept
{
    std::vector<std::string> parts;
    size_t start = 0;

    while (true)
    {
        const size_t cEnd = acLine.find(aSeparator, start);
        parts.push_back(acLine.substr(start, cEnd == std::string::npos ? std::string::npos : cEnd - start));
        if (cEnd == std::string::npos)
            break;
        start = cEnd + 1;
    }

    return parts;
}

bool IsCachedFile(const std::filesystem::path& acPath) noexcept
{
    const auto cExtension = acPath.extension();
    return cExtension != L".part" && cExtension != L".ytdl" && cExtension != L".tmp";
}
} // namespace

MusicWorker::MusicWorker() noexcept
    : m_thread(&MusicWorker::Loop, this)
{
}

MusicWorker::~MusicWorker() noexcept
{
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
        m_jobs.clear();

        // The process runs in a job object, so this takes down everything it started too.
        if (m_process)
            TerminateJobObject(m_process, 1);
    }

    m_condition.notify_all();

    if (m_thread.joinable())
        m_thread.join();
}

void MusicWorker::Post(std::function<void()> aJob) noexcept
{
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping)
            return;
        m_jobs.push_back(std::move(aJob));
    }

    m_condition.notify_one();
}

void MusicWorker::ClearPending() noexcept
{
    std::scoped_lock lock(m_mutex);
    m_jobs.clear();
}

bool MusicWorker::IsStopping() noexcept
{
    std::scoped_lock lock(m_mutex);
    return m_stopping;
}

void MusicWorker::Loop() noexcept
{
    while (true)
    {
        std::function<void()> job;

        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });

            if (m_stopping)
                return;

            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        job();
    }
}

int MusicWorker::Run(const std::wstring& acCommandLine, const std::function<void(const std::string&)>& acOnLine) noexcept
{
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0))
        return -1;

    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // yt-dlp.exe starts a second process for the actual Python interpreter, a job object lets us kill both.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nullptr;
    startupInfo.hStdOutput = writePipe;
    startupInfo.hStdError = writePipe;

    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = acCommandLine;
    const std::wstring cWorkingDirectory = GetMusicDirectory().wstring();

    {
        std::scoped_lock lock(m_mutex);

        const bool cStarted = !m_stopping && CreateProcessW(
                                                 nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cWorkingDirectory.c_str(),
                                                 &startupInfo, &processInfo);

        CloseHandle(writePipe);

        if (!cStarted)
        {
            CloseHandle(readPipe);
            CloseHandle(job);
            return -1;
        }

        AssignProcessToJobObject(job, processInfo.hProcess);
        m_process = job;
    }

    ResumeThread(processInfo.hThread);
    CloseHandle(processInfo.hThread);

    std::string pending;
    char buffer[4096];
    DWORD read = 0;

    // Progress bars rewrite their line with \r, treat that as a line break too.
    const auto cFlush = [&](bool aAll) {
        size_t start = 0;
        while (true)
        {
            const size_t cEnd = pending.find_first_of("\r\n", start);
            if (cEnd == std::string::npos)
                break;
            if (cEnd > start)
                acOnLine(pending.substr(start, cEnd - start));
            start = cEnd + 1;
        }
        pending.erase(0, start);

        if (aAll && !pending.empty())
        {
            acOnLine(pending);
            pending.clear();
        }
    };

    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        pending.append(buffer, read);
        cFlush(false);
    }
    cFlush(true);

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(processInfo.hProcess, &exitCode);

    bool stopping = false;
    {
        std::scoped_lock lock(m_mutex);
        m_process = nullptr;
        stopping = m_stopping;
    }

    CloseHandle(processInfo.hProcess);
    CloseHandle(readPipe);
    CloseHandle(job);

    return stopping ? -1 : static_cast<int>(exitCode);
}

namespace MusicTools
{
std::filesystem::path GetCacheDirectory() noexcept
{
    return GetMusicDirectory() / L"cache";
}

bool AreToolsInstalled() noexcept
{
    std::error_code error;
    return std::filesystem::exists(GetYtDlpPath(), error) && std::filesystem::exists(GetDenoPath(), error);
}

bool InstallTools(MusicWorker& aWorker, std::string& aError) noexcept
{
    std::error_code error;
    std::filesystem::create_directories(GetToolsDirectory(), error);
    std::filesystem::create_directories(GetCacheDirectory(), error);

    const auto cDownload = [&aWorker, &aError](const wchar_t* apUrl, const std::filesystem::path& acTarget) {
        std::string output;
        const std::wstring cCommand = Quote(GetSystemTool(L"curl.exe").wstring()) + L" -L -f -s -S --retry 2 -o " + Quote(acTarget.wstring()) + L" " + Quote(apUrl);

        if (aWorker.Run(cCommand, [&output](const std::string& acLine) { output = acLine; }) != 0)
        {
            aError = output.empty() ? "Download failed" : output;
            return false;
        }
        return true;
    };

    if (!std::filesystem::exists(GetYtDlpPath(), error))
    {
        const auto cTemporary = GetToolsDirectory() / L"yt-dlp.exe.tmp";
        if (!cDownload(kYtDlpUrl, cTemporary))
            return false;

        std::filesystem::rename(cTemporary, GetYtDlpPath(), error);
        if (error)
        {
            aError = error.message();
            return false;
        }
    }

    if (!std::filesystem::exists(GetDenoPath(), error))
    {
        const auto cArchive = GetToolsDirectory() / L"deno.zip";
        if (!cDownload(kDenoUrl, cArchive))
            return false;

        std::string output;
        const std::wstring cCommand = Quote(GetSystemTool(L"tar.exe").wstring()) + L" -xf " + Quote(cArchive.wstring()) + L" -C " + Quote(GetToolsDirectory().wstring());
        const int cResult = aWorker.Run(cCommand, [&output](const std::string& acLine) { output = acLine; });

        std::filesystem::remove(cArchive, error);

        if (cResult != 0 || !std::filesystem::exists(GetDenoPath(), error))
        {
            aError = output.empty() ? "Could not unpack Deno" : output;
            return false;
        }
    }

    return true;
}

void UpdateTools(MusicWorker& aWorker) noexcept
{
    std::string lastLine;
    aWorker.Run(Quote(GetYtDlpPath().wstring()) + L" -U", [&lastLine](const std::string& acLine) { lastLine = acLine; });
    spdlog::info("[Music] yt-dlp update: {}", lastLine);
}

bool Search(MusicWorker& aWorker, const std::string& acQuery, std::vector<SearchResult>& aResults, std::string& aError) noexcept
{
    const bool cIsLink = acQuery.starts_with("http://") || acQuery.starts_with("https://") || acQuery.starts_with("www.") || acQuery.starts_with("youtu");
    const std::string cTarget = cIsLink ? acQuery : fmt::format("ytsearch{}:{}", kMaxSearchResults, acQuery);

    const std::wstring cCommand = YtDlpCommand() + L" --flat-playlist --playlist-end " + std::to_wstring(kMaxSearchResults) + L" --print " +
                                  Quote(L"%(id)s\t%(title)s\t%(channel,uploader)s\t%(duration)s\t%(live_status)s") + L" -- " + Quote(ToWide(cTarget));

    const int cResult = aWorker.Run(cCommand, [&aResults, &aError](const std::string& acLine) {
        CaptureError(acLine, aError);

        const auto cParts = Split(acLine, '\t');
        if (cParts.size() != 5 || cParts[0].size() != 11)
            return;

        // Streams have no end, they cannot be played in sync.
        if (cParts[4] == "is_live" || cParts[4] == "is_upcoming")
            return;

        SearchResult result{};
        result.VideoId = cParts[0];
        result.Title = cParts[1];
        result.Author = cParts[2] == "NA" ? "" : cParts[2];

        char* pEnd = nullptr;
        const double cSeconds = std::strtod(cParts[3].c_str(), &pEnd);
        if (pEnd != cParts[3].c_str() && cSeconds > 0.0)
            result.DurationMs = static_cast<uint32_t>(cSeconds * 1000.0);

        aResults.push_back(std::move(result));
    });

    if (cResult != 0 && aResults.empty())
    {
        if (aError.empty())
            aError = "yt-dlp failed";
        return false;
    }

    aError.clear();
    return true;
}

bool Download(MusicWorker& aWorker, const std::string& acVideoId, const std::function<void(double)>& acOnProgress, std::filesystem::path& aPath, std::string& aError) noexcept
{
    std::error_code error;
    std::filesystem::create_directories(GetCacheDirectory(), error);

    // Opus in WebM plays in the UI's Chromium, unlike AAC.
    const std::wstring cCommand = YtDlpCommand() + L" --no-playlist --no-mtime --newline --progress -f " + Quote(L"bestaudio[ext=webm]/bestaudio[acodec^=opus]/bestaudio") +
                                  L" --progress-template " + Quote(L"download:[progress]%(progress._percent_str)s") + L" --print after_move:filepath --no-simulate -o " +
                                  Quote((GetCacheDirectory() / L"%(id)s.%(ext)s").wstring()) + L" " + Quote(ToWide("https://www.youtube.com/watch?v=" + acVideoId));

    std::string printedPath;

    const int cResult = aWorker.Run(cCommand, [&](const std::string& acLine) {
        CaptureError(acLine, aError);

        if (acLine.starts_with("[progress]"))
        {
            const double cPercent = std::strtod(acLine.c_str() + 10, nullptr);
            acOnProgress(std::clamp(cPercent, 0.0, 100.0));
            return;
        }

        if (acLine.find(':') != std::string::npos && !acLine.starts_with("ERROR") && !acLine.starts_with("WARNING") && !acLine.starts_with("["))
            printedPath = acLine;
    });

    if (cResult == 0)
    {
        aPath = std::filesystem::path(ToWide(printedPath));
        if (printedPath.empty() || !std::filesystem::exists(aPath, error))
            aPath = FindCached(acVideoId);

        if (!aPath.empty())
        {
            aError.clear();
            return true;
        }
    }

    if (aError.empty())
        aError = "yt-dlp failed";
    return false;
}

std::filesystem::path FindCached(const std::string& acVideoId) noexcept
{
    std::error_code error;
    const std::wstring cStem = ToWide(acVideoId);

    for (const auto& entry : std::filesystem::directory_iterator(GetCacheDirectory(), error))
    {
        if (entry.is_regular_file(error) && entry.path().stem() == cStem && IsCachedFile(entry.path()))
            return entry.path();
    }

    return {};
}

void TrimCache(const std::vector<std::string>& acKeep) noexcept
{
    std::error_code error;

    struct CachedFile
    {
        std::filesystem::path Path;
        std::filesystem::file_time_type Time;
        uintmax_t Size;
    };

    std::vector<CachedFile> files;
    uintmax_t total = 0;

    for (const auto& entry : std::filesystem::directory_iterator(GetCacheDirectory(), error))
    {
        if (!entry.is_regular_file(error))
            continue;

        CachedFile file{entry.path(), entry.last_write_time(error), entry.file_size(error)};
        total += file.Size;
        files.push_back(std::move(file));
    }

    if (total <= kMaxCacheBytes)
        return;

    std::sort(files.begin(), files.end(), [](const CachedFile& acLhs, const CachedFile& acRhs) { return acLhs.Time < acRhs.Time; });

    for (const auto& file : files)
    {
        if (total <= kMaxCacheBytes)
            break;

        const std::string cStem = ToUtf8(file.Path.stem().wstring());
        if (std::find(acKeep.begin(), acKeep.end(), cStem) != acKeep.end())
            continue;

        if (std::filesystem::remove(file.Path, error))
            total -= file.Size;
    }
}
} // namespace MusicTools
