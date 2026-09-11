/**
 * Framed operational journal: durability, replay, checksum rejection and tail recovery.
 *
 * Contract under test (ADR 0012): the journal is append-only and framed, a sequence gap or a
 * corrupt frame is refused rather than replayed, an interrupted write leaves a torn tail that
 * is recovered by truncation rather than by guessing, and a reopened journal continues the
 * sequence it recovered.
 *
 * The distinction that matters is between a torn tail and a corrupt frame. A torn tail is an
 * interrupted write and is recoverable. A complete frame that fails its checksum is damage,
 * and replaying it would put invented history in front of an operator.
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "context_hmi/operational_store.hpp"
#include "support/check.hpp"

using context_hmi::storage::Journal;
using context_hmi::storage::JournalConfig;
using context_hmi::storage::JournalRecord;
using context_hmi_test::check;
using Json = nlohmann::json;

namespace {

/** A unique scratch directory that removes itself, so tests never share a file. */
class ScratchDirectory {
  public:
    ScratchDirectory() {
        static std::uint64_t counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("context-hmi-journal-" + std::to_string(++counter) + "-" +
                 std::to_string(static_cast<std::uint64_t>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(path_);
    }
    ScratchDirectory(const ScratchDirectory &) = delete;
    ScratchDirectory &operator=(const ScratchDirectory &) = delete;
    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::filesystem::path file(const std::string &name) const { return path_ / name; }

  private:
    std::filesystem::path path_;
};

JournalConfig config_for(const std::filesystem::path &path) {
    JournalConfig config;
    config.path = path;
    config.recent_record_limit = 8;
    return config;
}

std::string read_all(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void write_all(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

template <typename Fn> std::string throws_message(Fn &&fn) {
    try {
        fn();
    } catch (const std::exception &error) {
        return error.what();
    }
    return {};
}

void appends_are_sequenced_and_replayable() {
    ScratchDirectory scratch;
    const auto path = scratch.file("journal.bin");
    {
        Journal journal(config_for(path));
        check(journal.append("command_intent", Json{{"key", "one"}}) == 1,
              "the first record takes sequence one");
        check(journal.append("command_result", Json{{"key", "one"}}) == 2,
              "sequences increase by one");
        check(journal.append("command_intent", Json{{"key", "two"}}) == 3,
              "sequences keep increasing across kinds");

        const auto snapshot = journal.snapshot();
        check(snapshot.at("records") == 3, "the snapshot counts every appended record");
        check(snapshot.at("recovered_tail_bytes") == 0, "a clean journal recovers no tail");
        check(snapshot.at("format") == "context-hmi-journal/1", "the framing format is declared");

        const auto recent = journal.recent(2);
        check(recent.size() == 2, "recent honours its limit");
        check(recent.back().sequence == 3, "recent ends at the newest record");
        check(recent.back().kind == "command_intent", "recent preserves the record kind");
        check(recent.back().payload.at("key") == "two", "recent preserves the payload");
    }

    Journal reopened(config_for(path));
    check(reopened.snapshot().at("records") == 3, "a reopened journal replays every record");
    check(reopened.append("command_result", Json{{"key", "two"}}) == 4,
          "a reopened journal continues the sequence it recovered");
    const auto recent = reopened.recent(8);
    check(recent.size() == 4, "the replayed records remain available");
    check(recent.front().sequence == 1, "replay starts at the first record");
}

void invalid_records_are_refused_before_they_reach_the_file() {
    ScratchDirectory scratch;
    Journal journal(config_for(scratch.file("journal.bin")));

    for (const char *kind : {"", "Command", "command intent", "command/intent"}) {
        const auto message =
            throws_message([&] { (void)journal.append(kind, Json{{"a", 1}}); });
        check(!message.empty(), std::string("kind '") + kind + "' is refused");
    }

    JournalConfig small = config_for(scratch.file("small.bin"));
    small.maximum_payload_bytes = 32;
    Journal bounded(small);
    const auto message = throws_message(
        [&] { (void)bounded.append("diagnostic", Json{{"padding", std::string(256, 'x')}}); });
    check(message.find("payload limit") != std::string::npos,
          "a payload above the declared limit is refused");
    check(bounded.snapshot().at("records") == 0,
          "a refused record never reaches the file");
}

void an_invalid_configuration_is_refused() {
    ScratchDirectory scratch;
    JournalConfig config = config_for(scratch.file("journal.bin"));
    config.recent_record_limit = 0;
    check(!throws_message([&] { Journal journal(config); }).empty(),
          "a zero recent-record limit is refused");

    config = config_for(scratch.file("journal.bin"));
    config.maximum_payload_bytes = 0;
    check(!throws_message([&] { Journal journal(config); }).empty(),
          "a zero payload bound is refused");

    config = config_for(scratch.file("journal.bin"));
    config.maximum_file_bytes = 8;
    check(!throws_message([&] { Journal journal(config); }).empty(),
          "a file bound smaller than one frame is refused");

    config = config_for("");
    check(!throws_message([&] { Journal journal(config); }).empty(),
          "an empty path is refused");
}

void a_foreign_file_is_not_adopted() {
    ScratchDirectory scratch;
    const auto path = scratch.file("foreign.bin");
    write_all(path, "this file was not written by the journal");
    const auto message = throws_message([&] { Journal journal(config_for(path)); });
    check(message.find("header is invalid") != std::string::npos,
          "a file without the journal magic is refused rather than overwritten");
}

/** A complete frame that fails its checksum is damage, and must never be replayed. */
void a_corrupt_frame_is_refused() {
    ScratchDirectory scratch;
    const auto path = scratch.file("journal.bin");
    {
        Journal journal(config_for(path));
        (void)journal.append("command_intent", Json{{"key", "one"}});
        (void)journal.append("command_result", Json{{"key", "one"}});
    }

    std::string bytes = read_all(path);
    check(bytes.size() > 48, "the journal wrote a framed file");
    /* Flip one bit inside the first frame's payload, leaving every length intact. */
    bytes[bytes.size() - 2] = static_cast<char>(bytes[bytes.size() - 2] ^ 0x20);
    write_all(path, bytes);

    const auto message = throws_message([&] { Journal journal(config_for(path)); });
    check(message.find("checksum is invalid") != std::string::npos,
          "a frame whose checksum fails is refused, not replayed: got '" + message + "'");
}

void a_corrupt_frame_header_is_refused() {
    ScratchDirectory scratch;
    const auto path = scratch.file("journal.bin");
    {
        Journal journal(config_for(path));
        (void)journal.append("command_intent", Json{{"key", "one"}});
    }

    std::string bytes = read_all(path);
    /* The frame magic sits immediately after the eight byte file magic. */
    bytes[8] = static_cast<char>(bytes[8] ^ 0xff);
    write_all(path, bytes);

    const auto message = throws_message([&] { Journal journal(config_for(path)); });
    check(message.find("frame header is corrupt") != std::string::npos,
          "a frame with a broken header is refused: got '" + message + "'");
}

/** An interrupted write leaves a torn tail. That is recoverable by truncation. */
void a_torn_tail_is_recovered_by_truncation() {
    ScratchDirectory scratch;
    const auto path = scratch.file("journal.bin");
    std::uintmax_t complete_size = 0;
    {
        Journal journal(config_for(path));
        (void)journal.append("command_intent", Json{{"key", "one"}});
        (void)journal.append("command_result", Json{{"key", "one"}});
        complete_size = std::filesystem::file_size(path);
        (void)journal.append("command_intent", Json{{"key", "two"}});
    }

    const std::string bytes = read_all(path);
    check(bytes.size() > complete_size, "the third record extended the file");

    /* Cut the final frame in half, as an interrupted write would. */
    const auto torn = complete_size + (bytes.size() - complete_size) / 2;
    write_all(path, bytes.substr(0, static_cast<std::size_t>(torn)));

    Journal recovered(config_for(path));
    const auto snapshot = recovered.snapshot();
    check(snapshot.at("records") == 2, "only the complete records are replayed");
    check(snapshot.at("recovered_tail_bytes").get<std::uintmax_t>() > 0,
          "the recovered tail is reported rather than hidden");
    check(std::filesystem::file_size(path) == complete_size,
          "the torn tail is truncated back to the last complete frame");

    check(recovered.append("command_result", Json{{"key", "two"}}) == 3,
          "the journal resumes at the sequence after the last complete record");
    const auto records = recovered.recent(8);
    check(records.size() == 3, "the recovered journal holds its complete history");
    check(records.back().payload.at("key") == "two", "the newly appended record is readable");
}

/** A truncated header, shorter than one frame header, is also a torn tail. */
void a_tail_shorter_than_a_frame_header_is_recovered() {
    ScratchDirectory scratch;
    const auto path = scratch.file("journal.bin");
    std::uintmax_t complete_size = 0;
    {
        Journal journal(config_for(path));
        (void)journal.append("command_intent", Json{{"key", "one"}});
        complete_size = std::filesystem::file_size(path);
    }

    std::string bytes = read_all(path);
    bytes.append(8, '\0');
    write_all(path, bytes);

    Journal recovered(config_for(path));
    check(recovered.snapshot().at("records") == 1,
          "a partial frame header does not add a record");
    check(recovered.snapshot().at("recovered_tail_bytes").get<std::uintmax_t>() == 8,
          "the exact torn byte count is reported");
    check(std::filesystem::file_size(path) == complete_size,
          "the partial header is truncated away");
    check(recovered.append("command_result", Json{{"key", "one"}}) == 2,
          "the journal continues cleanly after truncation");
}

void the_recent_window_is_bounded() {
    ScratchDirectory scratch;
    JournalConfig config = config_for(scratch.file("journal.bin"));
    config.recent_record_limit = 4;
    Journal journal(config);

    for (int index = 0; index < 20; ++index)
        (void)journal.append("diagnostic", Json{{"index", index}});

    const auto snapshot = journal.snapshot();
    check(snapshot.at("records") == 20, "every record is counted");
    check(snapshot.at("recent_records") == 4, "the in-memory window honours its bound");

    const auto recent = journal.recent(100);
    check(recent.size() == 4, "recent cannot exceed the retained window");
    check(recent.back().payload.at("index") == 19, "the newest record is retained");
    check(recent.front().payload.at("index") == 16, "the window holds the newest records");

    std::uint64_t previous = 0;
    bool ascending = true;
    for (const JournalRecord &record : recent) {
        if (record.sequence <= previous)
            ascending = false;
        previous = record.sequence;
    }
    check(ascending, "the retained window stays in sequence order");
}

}  /* namespace */

int main() {
    appends_are_sequenced_and_replayable();
    invalid_records_are_refused_before_they_reach_the_file();
    an_invalid_configuration_is_refused();
    a_foreign_file_is_not_adopted();
    a_corrupt_frame_is_refused();
    a_corrupt_frame_header_is_refused();
    a_torn_tail_is_recovered_by_truncation();
    a_tail_shorter_than_a_frame_header_is_recovered();
    the_recent_window_is_bounded();
    return context_hmi_test::report("journal tests");
}
