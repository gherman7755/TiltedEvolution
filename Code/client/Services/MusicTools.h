#pragma once

#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

/**
 * @brief Runs jobs one after the other on a background thread, and the processes they start.
 */
struct MusicWorker
{
    MusicWorker() noexcept;
    ~MusicWorker() noexcept;

    TP_NOCOPYMOVE(MusicWorker);

    void Post(std::function<void()> aJob) noexcept;
    /**
     * @brief Drops the jobs that did not start yet.
     */
    void ClearPending() noexcept;
    [[nodiscard]] bool IsStopping() noexcept;

    /**
     * @brief Runs a process without a window and hands over its output (stdout and stderr) line by line.
     * @return The exit code, or -1 if the process could not start or got killed.
     */
    int Run(const std::wstring& acCommandLine, const std::function<void(const std::string&)>& acOnLine) noexcept;

private:
    void Loop() noexcept;

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::function<void()>> m_jobs;
    bool m_stopping{false};
    HANDLE m_process{nullptr};
    std::thread m_thread;
};

/**
 * @brief yt-dlp (with Deno, which it needs for YouTube) and the track cache.
 *
 * The tools live in "music/tools" next to the client, downloaded tracks in "music/cache".
 * Everything here blocks, it is meant to run on a MusicWorker.
 */
namespace MusicTools
{
struct SearchResult
{
    std::string VideoId;
    std::string Title;
    std::string Author;
    uint32_t DurationMs{};
};

[[nodiscard]] std::filesystem::path GetCacheDirectory() noexcept;
[[nodiscard]] bool AreToolsInstalled() noexcept;
/**
 * @brief Downloads the latest yt-dlp and Deno from their GitHub releases.
 */
bool InstallTools(MusicWorker& aWorker, std::string& aError) noexcept;
/**
 * @brief Lets yt-dlp update itself, YouTube changes often enough that old versions stop working.
 */
void UpdateTools(MusicWorker& aWorker) noexcept;
/**
 * @brief Searches YouTube, or lists the videos of a YouTube video or playlist link.
 */
bool Search(MusicWorker& aWorker, const std::string& acQuery, std::vector<SearchResult>& aResults, std::string& aError) noexcept;
/**
 * @brief Downloads the audio of a video into the cache.
 * @param acOnProgress Gets the download progress in percent.
 */
bool Download(MusicWorker& aWorker, const std::string& acVideoId, const std::function<void(double)>& acOnProgress, std::filesystem::path& aPath, std::string& aError) noexcept;
/**
 * @brief The cached audio of a video, empty if it is not downloaded yet.
 */
[[nodiscard]] std::filesystem::path FindCached(const std::string& acVideoId) noexcept;
/**
 * @brief Deletes the oldest cached tracks while the cache is too big, except for the given videos.
 */
void TrimCache(const std::vector<std::string>& acKeep) noexcept;
} // namespace MusicTools
