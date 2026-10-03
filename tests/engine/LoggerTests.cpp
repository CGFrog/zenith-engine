#include "engine/application/Application.hpp"
#include "engine/diagnostics/Logger.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

LoggerConfig quietConfig(size_t capacity = 4096) {
    return {.capacity = capacity, .minimumLevel = LogLevel::Debug, .echoToStderr = false};
}

void testRingKeepsNewestEntries() {
    Logger logger(quietConfig(3));
    for (int index = 0; index < 5; ++index) {
        logger.log(LogLevel::Info, "test", "message " + std::to_string(index));
    }
    const std::vector<LogEntry> entries = logger.entries();
    expect(entries.size() == 3, "the ring should hold at most its capacity");
    expect(entries.front().message == "message 2" && entries.back().message == "message 4",
           "the oldest entries should be dropped first");
    expect(entries.front().sequence == 3 && entries.back().sequence == 5, "sequences should count every message");
    expect(logger.latestSequence() == 5 && logger.droppedCount() == 2, "drops and the newest sequence are counted");
    expect(entries.back().category == "test" && entries.back().level == LogLevel::Info,
           "entries should keep their category and level");
    expect(entries.back().wallTime.time_since_epoch().count() != 0, "entries should be timestamped");

    const std::vector<LogEntry> newer = logger.entriesAfter(3);
    expect(newer.size() == 2 && newer.front().sequence == 4, "entriesAfter should return only newer entries");
    expect(logger.entriesAfter(0).size() == 3, "entriesAfter older than the ring should return everything held");
    expect(logger.entriesAfter(5).empty(), "nothing is newer than the newest entry");

    logger.clear();
    expect(logger.entries().empty(), "clear should drop held entries");
    const LogSnapshot cleared = logger.snapshotAfter(2);
    expect(cleared.entries.empty() && cleared.latestSequence == 5,
           "a snapshot should report the newest sequence even when nothing is held");
    logger.log(LogLevel::Info, "test", "after clear");
    expect(logger.entries().size() == 1 && logger.entries().front().sequence == 6,
           "sequences should continue after clear");
}

void testMinimumLevel() {
    Logger logger({.minimumLevel = LogLevel::Warning, .echoToStderr = false});
    logger.log(LogLevel::Info, "test", "ignored");
    logger.log(LogLevel::Error, "test", "kept");
    expect(logger.entries().size() == 1 && logger.entries().front().message == "kept",
           "messages below the minimum level should be discarded");
}

void testSinks() {
    Logger logger(quietConfig());
    std::vector<std::string> received;
    const Logger::SinkId first =
        logger.subscribe([&received](const LogEntry& entry) { received.push_back("first:" + entry.message); });
    logger.subscribe([&received](const LogEntry& entry) { received.push_back("second:" + entry.message); });
    logger.log(LogLevel::Warning, "test", "a");
    expect(received == std::vector<std::string>{"first:a", "second:a"}, "every sink should receive each message");
    logger.unsubscribe(first);
    logger.log(LogLevel::Warning, "test", "b");
    expect(received.size() == 3 && received.back() == "second:b", "an unsubscribed sink should not be called");

    // A sink may log and unsubscribe itself without deadlocking.
    Logger reentrant(quietConfig());
    std::shared_ptr<Logger::SinkId> self = std::make_shared<Logger::SinkId>(0);
    int calls = 0;
    *self = reentrant.subscribe([&reentrant, &calls, self](const LogEntry& entry) {
        ++calls;
        if (entry.message == "outer") {
            reentrant.log(LogLevel::Info, "nested", "inner");
            reentrant.unsubscribe(*self);
        }
    });
    reentrant.log(LogLevel::Info, "test", "outer");
    reentrant.log(LogLevel::Info, "test", "after");
    expect(calls == 2, "a sink should see its nested message and nothing after unsubscribing");
    expect(reentrant.entries().size() == 3, "nested messages should be stored");
}

void testConcurrentLogging() {
    constexpr int threadCount = 4;
    constexpr int messagesPerThread = 500;
    Logger logger(quietConfig(256));
    std::atomic<int> delivered{0};
    logger.subscribe([&delivered](const LogEntry&) { delivered.fetch_add(1, std::memory_order_relaxed); });
    std::vector<std::thread> threads;
    for (int thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([&logger, thread] {
            for (int index = 0; index < messagesPerThread; ++index) {
                logger.log(LogLevel::Info, "thread", std::to_string(thread) + ":" + std::to_string(index));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::vector<LogEntry> entries = logger.entries();
    expect(logger.latestSequence() == threadCount * messagesPerThread, "no message should be lost");
    expect(delivered.load() == threadCount * messagesPerThread, "sinks should see every message");
    expect(entries.size() == 256 && logger.droppedCount() == threadCount * messagesPerThread - 256,
           "the ring should stay bounded under concurrent logging");
    bool ascending = true;
    for (size_t index = 1; index < entries.size(); ++index) {
        ascending = ascending && entries[index].sequence == entries[index - 1].sequence + 1;
    }
    expect(ascending, "held entries should have contiguous ascending sequences");
}

void testChannels() {
    Logger logger(quietConfig());
    const LogChannel graphics(&logger, "graphics");
    graphics.warning("device lost");
    graphics.withCategory("assets").error("missing file");
    const std::vector<LogEntry> entries = logger.entries();
    expect(entries.size() == 2 && entries[0].category == "graphics" && entries[0].level == LogLevel::Warning,
           "a channel should log under its category");
    expect(entries[1].category == "assets" && entries[1].level == LogLevel::Error,
           "withCategory should keep the logger");

    // Without a logger the message goes to stderr; it must not crash.
    const LogChannel standalone;
    standalone.info("Logger test: a channel without a logger writes this line to stderr");
    expect(standalone.logger() == nullptr, "a default channel has no logger");
}

class EmptyScene final : public ApplicationScene {
  public:
    SceneResult load(ApplicationContext& context) override {
        context.logger.log(LogLevel::Info, "game", "scene loaded");
        return {.success = true};
    }

    void update(ApplicationContext&, const InputState&, float) override {}

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}
};

class LoggingApplication final : public Application {
  public:
    SceneKey initialScene() const override {
        return {};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<EmptyScene>();
    }
};

void testApplicationContextCarriesLogger() {
    Logger logger(quietConfig());
    Scene scene;
    AssetManager assets(&logger);
    PhysicsWorld physics;
    AudioSystem audio(&logger);
    RenderRuntime rendering;
    GameUiContext gameUi;
    LoggingApplication application;
    {
        ApplicationSession session(application, scene, assets, physics, audio, rendering, gameUi, logger);
        expect(session.start().success, "the application should start");
        session.shutdown();
    }
    const std::vector<LogEntry> entries = logger.entries();
    expect(!entries.empty() && entries.front().category == "game" && entries.front().message == "scene loaded",
           "scenes should log through the engine logger in their context");
    expect(assets.log().logger() == &logger && assets.log().category() == "assets",
           "the asset manager should log through the injected logger");
}
} // namespace

int main() {
    testRingKeepsNewestEntries();
    testMinimumLevel();
    testSinks();
    testConcurrentLogging();
    testChannels();
    testApplicationContextCarriesLogger();
    if (failures == 0) {
        std::cout << "Logger tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
