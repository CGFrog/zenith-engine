#include "RecentProjects.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>
#include <utility>

namespace {
using Json = nlohmann::json;

std::filesystem::path normalizedProjectPath(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

// The key two paths are compared by: Windows file names ignore case.
std::string comparisonKey(const std::filesystem::path& path) {
    std::string key = normalizedProjectPath(path).generic_string();
#if defined(_WIN32)
    std::ranges::transform(key, key.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
    return key;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace

bool recentProjectMissing(const RecentProject& project) {
    std::error_code error;
    return !std::filesystem::is_regular_file(project.projectFile, error);
}

RecentProjects::RecentProjects(std::filesystem::path file)
    : m_file(std::move(file)) {}

RecentProjectsResult RecentProjects::load() {
    m_entries.clear();
    std::error_code error;
    if (!std::filesystem::exists(m_file, error)) {
        return {.success = true};
    }
    std::ifstream input(m_file);
    if (!input) {
        return {.error = "Could not open the recent projects list: " + m_file.string()};
    }
    Json document;
    try {
        input >> document;
    } catch (const Json::exception& parseError) {
        return {.error = "Could not parse the recent projects list '" + m_file.string() + "': " + parseError.what()};
    }
    const auto version = document.is_object() ? document.find("version") : document.end();
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != currentVersion) {
        return {.error = "Unsupported recent projects list: " + m_file.string()};
    }
    const auto projects = document.find("projects");
    if (projects == document.end() || !projects->is_array()) {
        return {.error = "The recent projects list has no 'projects' array: " + m_file.string()};
    }
    for (const Json& entry : *projects) {
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string() ||
            entry["path"].get<std::string>().empty()) {
            continue;
        }
        RecentProject project{.projectFile = normalizedProjectPath(fromUtf8(entry["path"].get<std::string>()))};
        if (const auto name = entry.find("name"); name != entry.end() && name->is_string()) {
            project.displayName = name->get<std::string>();
        }
        if (const auto opened = entry.find("lastOpened"); opened != entry.end() && opened->is_number_integer()) {
            project.lastOpened = std::chrono::system_clock::time_point(std::chrono::seconds(opened->get<int64_t>()));
        }
        const std::string key = comparisonKey(project.projectFile);
        const bool duplicate = std::ranges::any_of(
            m_entries, [&](const RecentProject& existing) { return comparisonKey(existing.projectFile) == key; });
        if (!duplicate && m_entries.size() < maximumEntries) {
            m_entries.push_back(std::move(project));
        }
    }
    return {.success = true};
}

RecentProjectsResult RecentProjects::save() const {
    Json projects = Json::array();
    for (const RecentProject& project : m_entries) {
        projects.push_back(
            {{"path", utf8(project.projectFile)},
             {"name", project.displayName},
             {"lastOpened",
              std::chrono::duration_cast<std::chrono::seconds>(project.lastOpened.time_since_epoch()).count()}});
    }
    const Json document = {{"version", currentVersion}, {"projects", std::move(projects)}};

    std::error_code error;
    if (m_file.has_parent_path()) {
        std::filesystem::create_directories(m_file.parent_path(), error);
        if (error) {
            return {.error = "Could not create the editor settings directory: " + error.message()};
        }
    }
    // Written beside the list and renamed over it, so an interrupted write
    // never leaves a truncated list.
    std::filesystem::path temporary = m_file;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            return {.error = "Could not write the recent projects list: " + temporary.string()};
        }
        output << document.dump(2) << '\n';
        if (!output) {
            return {.error = "Could not finish writing the recent projects list: " + temporary.string()};
        }
    }
    std::filesystem::rename(temporary, m_file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return {.error = "Could not replace the recent projects list: " + m_file.string()};
    }
    return {.success = true};
}

void RecentProjects::recordOpened(const std::filesystem::path& projectFile, std::string displayName,
                                  std::chrono::system_clock::time_point openedAt) {
    remove(projectFile);
    m_entries.insert(m_entries.begin(), RecentProject{.projectFile = normalizedProjectPath(projectFile),
                                                      .displayName = std::move(displayName),
                                                      .lastOpened = openedAt});
    if (m_entries.size() > maximumEntries) {
        m_entries.resize(maximumEntries);
    }
}

bool RecentProjects::remove(const std::filesystem::path& projectFile) {
    const std::string key = comparisonKey(projectFile);
    return std::erase_if(m_entries,
                         [&](const RecentProject& project) { return comparisonKey(project.projectFile) == key; }) > 0;
}

std::span<const RecentProject> RecentProjects::entries() const {
    return m_entries;
}

const std::filesystem::path& RecentProjects::file() const {
    return m_file;
}
