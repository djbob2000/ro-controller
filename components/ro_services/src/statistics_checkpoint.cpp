#include "ro/services.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace ro::svc {
namespace {
constexpr char CHECKPOINT_PATH[] = "/spiffs/stats.current";
constexpr char CHECKPOINT_TMP[] = "/spiffs/stats.current.tmp";
constexpr char STATS_PATH[] = "/spiffs/stats.log";
constexpr char STATS_TMP[] = "/spiffs/stats.log.tmp";
constexpr uint32_t MAGIC = 0x53544F52U;
constexpr uint16_t VERSION = 1;
struct CheckpointRecord { uint32_t magic{MAGIC}; uint16_t version{VERSION}; uint16_t payload_size{sizeof(DailyStats)}; DailyStats stats{}; uint32_t checksum{0}; };
static_assert(std::is_trivially_copyable_v<DailyStats>);
static_assert(std::is_trivially_copyable_v<CheckpointRecord>);

uint32_t fnv1a(const uint8_t* data, size_t size) noexcept {
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) { hash ^= data[i]; hash *= 16777619U; }
    return hash;
}
uint32_t record_checksum(const CheckpointRecord& record) noexcept { return fnv1a(reinterpret_cast<const uint8_t*>(&record.stats), sizeof(record.stats)); }

esp_err_t compact_history(size_t keep_lines) noexcept {
    if (keep_lines == 0) return ESP_OK;
    std::ifstream in(STATS_PATH);
    if (!in) return ESP_OK;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
        if (lines.size() > keep_lines) lines.erase(lines.begin());
    }
    in.close();
    std::ofstream out(STATS_TMP, std::ios::trunc);
    if (!out) return ESP_FAIL;
    for (const auto& item : lines) out << item << '\n';
    out.flush();
    if (!out) return ESP_FAIL;
    out.close();
    std::remove(STATS_PATH);
    return std::rename(STATS_TMP, STATS_PATH) == 0 ? ESP_OK : ESP_FAIL;
}
}

void StatisticsService::on_manual_flush(int64_t epoch, bool from_standby) noexcept {
    rollover_if_needed(epoch);
    // Legacy transition accounting sees Standby -> StandbyFlush as preventive.
    // Correct it when the transition was explicitly caused by a manual command.
    if (from_standby && current_.standby_flushes > 0) --current_.standby_flushes;
    ++current_.manual_flushes;
}

esp_err_t StatisticsService::restore_durable_checkpoint() noexcept {
    std::ifstream in(CHECKPOINT_PATH, std::ios::binary);
    if (!in) return ESP_OK;
    CheckpointRecord record{};
    in.read(reinterpret_cast<char*>(&record), sizeof(record));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(record)) || record.magic != MAGIC || record.version != VERSION ||
        record.payload_size != sizeof(DailyStats) || record.checksum != record_checksum(record)) {
        std::remove(CHECKPOINT_PATH);
        return ESP_ERR_INVALID_CRC;
    }
    current_ = record.stats;
    last_observed_epoch_ = 0;
    return ESP_OK;
}

esp_err_t StatisticsService::persist_durable_checkpoint(size_t history_limit_days) noexcept {
    CheckpointRecord record{};
    record.stats = current_;
    record.checksum = record_checksum(record);
    std::ofstream out(CHECKPOINT_TMP, std::ios::binary | std::ios::trunc);
    if (!out) return ESP_FAIL;
    out.write(reinterpret_cast<const char*>(&record), sizeof(record));
    out.flush();
    if (!out) return ESP_FAIL;
    out.close();
    std::remove(CHECKPOINT_PATH);
    if (std::rename(CHECKPOINT_TMP, CHECKPOINT_PATH) != 0) return ESP_FAIL;
    return compact_history(history_limit_days);
}

} // namespace ro::svc
