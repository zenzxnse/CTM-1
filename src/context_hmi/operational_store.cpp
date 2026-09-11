#include "context_hmi/operational_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace context_hmi::storage {
namespace {

constexpr std::array<unsigned char, 8> kFileMagic{'C', 'H', 'M', 'I', 'J', 'N', 'L', '1'};
constexpr std::uint32_t kFrameMagic = 0x314d5246U;
constexpr std::uint16_t kFrameVersion = 1;
constexpr std::size_t kFrameHeaderSize = 32;
constexpr std::size_t kMaximumKindBytes = 64;
constexpr std::size_t kMaximumConfiguredPayloadBytes = 256U * 1024U * 1024U;

void put_u16(std::array<unsigned char, kFrameHeaderSize>& output, std::size_t offset,
             std::uint16_t value) {
  output[offset] = static_cast<unsigned char>(value & 0xffU);
  output[offset + 1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put_u32(std::array<unsigned char, kFrameHeaderSize>& output, std::size_t offset,
             std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

void put_u64(std::array<unsigned char, kFrameHeaderSize>& output, std::size_t offset,
             std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    output[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint16_t get_u16(const std::array<unsigned char, kFrameHeaderSize>& input,
                      std::size_t offset) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint32_t>(input[offset]) |
      (static_cast<std::uint32_t>(input[offset + 1]) << 8U));
}

std::uint32_t get_u32(const std::array<unsigned char, kFrameHeaderSize>& input,
                      std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(input[offset + index]) << (index * 8U);
  }
  return value;
}

std::uint64_t get_u64(const std::array<unsigned char, kFrameHeaderSize>& input,
                      std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8U);
  }
  return value;
}

std::uint32_t crc32c_update(std::uint32_t crc, std::string_view data) {
  for (const unsigned char byte : data) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return crc;
}

std::uint32_t frame_crc(std::uint64_t sequence, std::string_view kind,
                        std::string_view payload) {
  std::array<unsigned char, 8> sequence_bytes{};
  for (std::size_t index = 0; index < sequence_bytes.size(); ++index) {
    sequence_bytes[index] =
        static_cast<unsigned char>((sequence >> (index * 8U)) & 0xffU);
  }
  std::uint32_t crc = 0xffffffffU;
  crc = crc32c_update(crc, std::string_view(
                               reinterpret_cast<const char*>(sequence_bytes.data()),
                               sequence_bytes.size()));
  crc = crc32c_update(crc, kind);
  crc = crc32c_update(crc, payload);
  return ~crc;
}

bool valid_kind(const std::string& kind) {
  if (kind.empty() || kind.size() > kMaximumKindBytes) {
    return false;
  }
  return std::all_of(kind.begin(), kind.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '.' ||
           character == '-';
  });
}

void synchronize_file(std::FILE* file) {
  if (std::fflush(file) != 0) {
    throw std::runtime_error("operational store flush failed");
  }
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    throw std::runtime_error("operational store synchronization failed");
  }
#else
  if (::fsync(fileno(file)) != 0) {
    throw std::runtime_error("operational store synchronization failed");
  }
#endif
}

void truncate_file(const std::filesystem::path& path, std::uintmax_t size,
                   bool synchronize) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb+");
  if (file == nullptr) {
    throw std::runtime_error("could not open operational store for tail recovery");
  }
  try {
#if defined(_WIN32)
    if (_chsize_s(_fileno(file), static_cast<__int64>(size)) != 0) {
      throw std::runtime_error("operational store tail recovery failed");
    }
#else
    if (::ftruncate(fileno(file), static_cast<off_t>(size)) != 0) {
      throw std::runtime_error("operational store tail recovery failed");
    }
#endif
    if (synchronize) {
      synchronize_file(file);
    } else if (std::fflush(file) != 0) {
      throw std::runtime_error("operational store tail recovery flush failed");
    }
    if (std::fclose(file) != 0) {
      file = nullptr;
      throw std::runtime_error("operational store tail recovery close failed");
    }
    file = nullptr;
  } catch (...) {
    if (file != nullptr) {
      std::fclose(file);
      file = nullptr;
    }
    throw;
  }
}

void write_exact(std::FILE* file, const void* bytes, std::size_t size) {
  if (size != 0 && std::fwrite(bytes, 1, size, file) != size) {
    throw std::runtime_error("operational store write failed");
  }
}

nlohmann::json parse_payload(const std::string& payload) {
  return nlohmann::json::parse(payload, [](int depth, nlohmann::json::parse_event_t,
                                          nlohmann::json&) {
    if (depth > 32) {
      throw std::runtime_error("operational record exceeds JSON nesting limit");
    }
    return true;
  });
}

}  /* namespace */

Journal::Journal(JournalConfig config) : config_(std::move(config)) {
  if (config_.path.empty() || config_.maximum_payload_bytes == 0 ||
      config_.maximum_payload_bytes > std::numeric_limits<std::uint32_t>::max() ||
      config_.maximum_payload_bytes > kMaximumConfiguredPayloadBytes ||
      config_.maximum_file_bytes < kFileMagic.size() + kFrameHeaderSize ||
      config_.recent_record_limit == 0) {
    throw std::invalid_argument("operational store configuration is invalid");
  }
  open_and_recover();
}

void Journal::open_and_recover() {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(config_.path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("could not inspect operational store path");
  }
  if (!error && std::filesystem::is_symlink(status)) {
    throw std::runtime_error("operational store must not be a symbolic link");
  }
  const bool existed = !error && std::filesystem::exists(status);
  if (existed && !std::filesystem::is_regular_file(status)) {
    throw std::runtime_error("operational store path is not a regular file");
  }
  const auto parent = config_.path.parent_path();
  if (!parent.empty() && !std::filesystem::is_directory(parent, error)) {
    throw std::runtime_error("operational store parent directory does not exist");
  }

  if (!existed) {
    std::FILE* file = std::fopen(config_.path.string().c_str(), "wb");
    if (file == nullptr) {
      throw std::runtime_error("could not create operational store");
    }
    try {
      write_exact(file, kFileMagic.data(), kFileMagic.size());
      if (config_.synchronize) {
        synchronize_file(file);
      } else if (std::fflush(file) != 0) {
        throw std::runtime_error("operational store flush failed");
      }
      if (std::fclose(file) != 0) {
        file = nullptr;
        throw std::runtime_error("operational store close failed");
      }
      file = nullptr;
    } catch (...) {
      if (file != nullptr) {
        std::fclose(file);
      }
      throw;
    }
  } else if (std::filesystem::file_size(config_.path, error) == 0) {
    throw std::runtime_error("operational store header is invalid");
  }

  const auto file_size = std::filesystem::file_size(config_.path, error);
  if (error || file_size > config_.maximum_file_bytes) {
    throw std::runtime_error("operational store exceeds configured file limit");
  }
  std::ifstream input(config_.path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not open operational store for replay");
  }
  std::array<unsigned char, kFileMagic.size()> magic{};
  input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
  if (input.gcount() != static_cast<std::streamsize>(magic.size()) || magic != kFileMagic) {
    throw std::runtime_error("operational store header is invalid");
  }

  std::uintmax_t last_complete = kFileMagic.size();
  std::uint64_t expected_sequence = 1;
  for (;;) {
    std::array<unsigned char, kFrameHeaderSize> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto header_bytes = input.gcount();
    if (header_bytes == 0) {
      break;
    }
    if (header_bytes != static_cast<std::streamsize>(header.size())) {
      recovered_tail_bytes_ = file_size - last_complete;
      input.close();
      truncate_file(config_.path, last_complete, config_.synchronize);
      break;
    }
    if (get_u32(header, 0) != kFrameMagic || get_u16(header, 4) != kFrameVersion ||
        (get_u32(header, 12) != 0U && get_u32(header, 12) != 1U) ||
        get_u32(header, 28) != 0U) {
      throw std::runtime_error("operational store frame header is corrupt");
    }
    const auto kind_size = get_u16(header, 6);
    const auto payload_size = get_u32(header, 8);
    const auto sequence = get_u64(header, 16);
    const auto expected_crc = get_u32(header, 24);
    if (kind_size == 0 || kind_size > kMaximumKindBytes ||
        payload_size > config_.maximum_payload_bytes || sequence != expected_sequence) {
      throw std::runtime_error("operational store frame bounds or sequence are invalid");
    }
    std::string kind(kind_size, '\0');
    std::string payload(payload_size, '\0');
    input.read(kind.data(), static_cast<std::streamsize>(kind.size()));
    const auto read_kind = input.gcount();
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    const auto read_payload = input.gcount();
    if (read_kind != static_cast<std::streamsize>(kind.size()) ||
        read_payload != static_cast<std::streamsize>(payload.size())) {
      recovered_tail_bytes_ = file_size - last_complete;
      input.close();
      truncate_file(config_.path, last_complete, config_.synchronize);
      break;
    }
    if (!valid_kind(kind) || frame_crc(sequence, kind, payload) != expected_crc) {
      throw std::runtime_error("operational store frame checksum is invalid");
    }
    JournalRecord record{sequence, std::move(kind), parse_payload(payload)};
    if (recent_.size() >= config_.recent_record_limit) {
      recent_.erase(recent_.begin());
    }
    recent_.push_back(std::move(record));
    ++expected_sequence;
    last_complete += kFrameHeaderSize + kind_size + payload_size;
  }
  next_sequence_ = expected_sequence;
}

std::uint64_t Journal::append(const std::string& kind, const nlohmann::json& payload) {
  if (!valid_kind(kind)) {
    throw std::invalid_argument("operational record kind is invalid");
  }
  const auto encoded = payload.dump();
  if (encoded.size() > config_.maximum_payload_bytes) {
    throw std::runtime_error("operational record exceeds payload limit");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code error;
  const auto current_size = std::filesystem::file_size(config_.path, error);
  const auto frame_size = kFrameHeaderSize + kind.size() + encoded.size();
  if (error || current_size > config_.maximum_file_bytes ||
      frame_size > config_.maximum_file_bytes - current_size) {
    throw std::runtime_error("operational store file limit reached");
  }
  const auto status = std::filesystem::symlink_status(config_.path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw std::runtime_error("operational store path changed or is not a regular file");
  }
  std::array<unsigned char, kFrameHeaderSize> header{};
  put_u32(header, 0, kFrameMagic);
  put_u16(header, 4, kFrameVersion);
  put_u16(header, 6, static_cast<std::uint16_t>(kind.size()));
  put_u32(header, 8, static_cast<std::uint32_t>(encoded.size()));
  put_u32(header, 12, config_.synchronize ? 1U : 0U);
  put_u64(header, 16, next_sequence_);
  put_u32(header, 24, frame_crc(next_sequence_, kind, encoded));

  std::FILE* file = std::fopen(config_.path.string().c_str(), "ab");
  if (file == nullptr) {
    throw std::runtime_error("could not open operational store for append");
  }
  try {
    write_exact(file, header.data(), header.size());
    write_exact(file, kind.data(), kind.size());
    write_exact(file, encoded.data(), encoded.size());
    if (config_.synchronize) {
      synchronize_file(file);
    } else if (std::fflush(file) != 0) {
      throw std::runtime_error("operational store flush failed");
    }
    if (std::fclose(file) != 0) {
      file = nullptr;
      throw std::runtime_error("operational store close failed");
    }
    file = nullptr;
  } catch (...) {
    if (file != nullptr) {
      std::fclose(file);
    }
    throw;
  }

  const auto sequence = next_sequence_++;
  if (recent_.size() >= config_.recent_record_limit) {
    recent_.erase(recent_.begin());
  }
  recent_.push_back(JournalRecord{sequence, kind, payload});
  return sequence;
}

std::vector<JournalRecord> Journal::recent(std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  limit = std::min(limit, recent_.size());
  return std::vector<JournalRecord>(recent_.end() - static_cast<std::ptrdiff_t>(limit),
                                    recent_.end());
}

nlohmann::json Journal::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code error;
  const auto bytes = std::filesystem::file_size(config_.path, error);
  return nlohmann::json{{"enabled", true},
                        {"format", "context-hmi-journal/1"},
                        {"records", next_sequence_ - 1},
                        {"recent_records", recent_.size()},
                        {"recent_limit", config_.recent_record_limit},
                        {"bytes", error ? 0 : bytes},
                        {"maximum_bytes", config_.maximum_file_bytes},
                        {"synchronous", config_.synchronize},
                        {"recovered_tail_bytes", recovered_tail_bytes_}};
}

}  /* namespace context_hmi::storage */
