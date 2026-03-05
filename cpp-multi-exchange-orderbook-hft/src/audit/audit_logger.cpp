#include "audit/audit_logger.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace hft {
namespace audit {

// ============================================================================
// AuditEntry Serialization
// ============================================================================

std::vector<uint8_t> AuditEntry::toBinary() const {
    std::vector<uint8_t> data;
    data.reserve(512);  // Pre-allocate

    auto writeU8 = [&](uint8_t v) { data.push_back(v); };
    auto writeU16 = [&](uint16_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
    };
    auto writeU32 = [&](uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };
    auto writeU64 = [&](uint64_t v) {
        writeU32(v & 0xFFFFFFFF);
        writeU32((v >> 32) & 0xFFFFFFFF);
    };
    auto writeDouble = [&](double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        writeU64(bits);
    };
    auto writeString = [&](const std::string& s) {
        writeU16(static_cast<uint16_t>(s.size()));
        data.insert(data.end(), s.begin(), s.end());
    };

    // Header
    writeU32(MAGIC);
    writeU16(VERSION);

    // Fixed fields
    writeU64(sequence_id);
    writeU64(timestamp_ns);
    writeU8(static_cast<uint8_t>(event_type));
    writeU8(static_cast<uint8_t>(side));
    writeU8(static_cast<uint8_t>(type));

    // Variable strings
    writeString(order_id);
    writeString(client_order_id);
    writeString(exchange_order_id);
    writeString(exchange);
    writeString(symbol);

    // Numeric fields
    writeDouble(price);
    writeDouble(quantity);
    writeDouble(filled_qty);
    writeDouble(avg_fill_price);
    writeDouble(commission);

    // Timestamps
    writeU64(exchange_timestamp_ns);
    writeU64(local_send_time_ns);
    writeU64(local_receive_time_ns);

    // Variable strings
    writeString(reason);
    writeString(raw_request);
    writeString(raw_response);
    writeString(metadata);

    // Prepend total length
    uint32_t total_len = static_cast<uint32_t>(data.size());
    std::vector<uint8_t> result;
    result.reserve(data.size() + 4);
    result.push_back(total_len & 0xFF);
    result.push_back((total_len >> 8) & 0xFF);
    result.push_back((total_len >> 16) & 0xFF);
    result.push_back((total_len >> 24) & 0xFF);
    result.insert(result.end(), data.begin(), data.end());

    return result;
}

std::string AuditEntry::toJson() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"seq\":" << sequence_id << ",";
    oss << "\"ts\":" << timestamp_ns << ",";
    oss << "\"event\":" << static_cast<int>(event_type) << ",";
    oss << "\"order_id\":\"" << order_id << "\",";
    oss << "\"client_order_id\":\"" << client_order_id << "\",";
    oss << "\"exchange_order_id\":\"" << exchange_order_id << "\",";
    oss << "\"exchange\":\"" << exchange << "\",";
    oss << "\"symbol\":\"" << symbol << "\",";
    oss << "\"side\":" << static_cast<int>(side) << ",";
    oss << "\"type\":" << static_cast<int>(type) << ",";
    oss << "\"price\":" << std::fixed << std::setprecision(8) << price << ",";
    oss << "\"qty\":" << quantity << ",";
    oss << "\"filled\":" << filled_qty << ",";
    oss << "\"avg_price\":" << avg_fill_price << ",";
    oss << "\"commission\":" << commission << ",";
    oss << "\"exch_ts\":" << exchange_timestamp_ns << ",";
    oss << "\"send_ts\":" << local_send_time_ns << ",";
    oss << "\"recv_ts\":" << local_receive_time_ns << ",";
    oss << "\"reason\":\"" << reason << "\"";
    if (!metadata.empty()) {
        oss << ",\"meta\":\"" << metadata << "\"";
    }
    oss << "}\n";
    return oss.str();
}

AuditEntry AuditEntry::fromBinary(const uint8_t* data, size_t length, size_t& bytes_read) {
    AuditEntry entry;
    size_t pos = 0;

    auto readU8 = [&]() -> uint8_t {
        return data[pos++];
    };
    auto readU16 = [&]() -> uint16_t {
        uint16_t v = data[pos] | (data[pos + 1] << 8);
        pos += 2;
        return v;
    };
    auto readU32 = [&]() -> uint32_t {
        uint32_t v = data[pos] | (data[pos + 1] << 8) |
                     (data[pos + 2] << 16) | (data[pos + 3] << 24);
        pos += 4;
        return v;
    };
    auto readU64 = [&]() -> uint64_t {
        uint64_t low = readU32();
        uint64_t high = readU32();
        return low | (high << 32);
    };
    auto readDouble = [&]() -> double {
        uint64_t bits = readU64();
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    auto readString = [&]() -> std::string {
        uint16_t len = readU16();
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    };

    // Verify header
    uint32_t magic = readU32();
    if (magic != MAGIC) {
        throw std::runtime_error("Invalid audit entry magic");
    }

    uint16_t version = readU16();
    (void)version;  // For future compatibility

    // Fixed fields
    entry.sequence_id = readU64();
    entry.timestamp_ns = readU64();
    entry.event_type = static_cast<AuditEventType>(readU8());
    entry.side = static_cast<OrderSideAudit>(readU8());
    entry.type = static_cast<OrderTypeAudit>(readU8());

    // Variable strings
    entry.order_id = readString();
    entry.client_order_id = readString();
    entry.exchange_order_id = readString();
    entry.exchange = readString();
    entry.symbol = readString();

    // Numeric fields
    entry.price = readDouble();
    entry.quantity = readDouble();
    entry.filled_qty = readDouble();
    entry.avg_fill_price = readDouble();
    entry.commission = readDouble();

    // Timestamps
    entry.exchange_timestamp_ns = readU64();
    entry.local_send_time_ns = readU64();
    entry.local_receive_time_ns = readU64();

    // Variable strings
    entry.reason = readString();
    entry.raw_request = readString();
    entry.raw_response = readString();
    entry.metadata = readString();

    bytes_read = pos;
    return entry;
}

// ============================================================================
// AuditLogger Implementation
// ============================================================================

AuditLogger::AuditLogger() {
    std::filesystem::create_directories(config_.output_dir);
}

AuditLogger::~AuditLogger() {
    stop();
}

void AuditLogger::setConfig(const AuditConfig& config) {
    config_ = config;
    std::filesystem::create_directories(config_.output_dir);
}

void AuditLogger::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    std::filesystem::create_directories(config_.output_dir);

    running_.store(true, std::memory_order_release);
    writer_thread_ = std::thread(&AuditLogger::writerLoop, this);
}

void AuditLogger::stop() {
    running_.store(false, std::memory_order_release);
    flush_requested_.store(true, std::memory_order_release);

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    // Close files
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (binary_file_.is_open()) {
        binary_file_.close();
    }
    if (json_file_.is_open()) {
        json_file_.close();
    }
}

void AuditLogger::flush() {
    flush_requested_.store(true, std::memory_order_release);
}

std::string AuditLogger::getCurrentFilePath() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&time, &tm);

    std::ostringstream oss;
    oss << config_.output_dir << "/" << config_.file_prefix << "_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_"
        << sequence_counter_.load(std::memory_order_relaxed) << ".bin";
    return oss.str();
}

uint64_t AuditLogger::getNextSequenceId() {
    return sequence_counter_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AuditLogger::getCurrentTimestampNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

void AuditLogger::log(AuditEntry entry) {
    entry.sequence_id = getNextSequenceId();
    if (entry.timestamp_ns == 0) {
        entry.timestamp_ns = getCurrentTimestampNs();
    }

    entries_logged_.fetch_add(1, std::memory_order_relaxed);

    if (!queue_.try_push(std::move(entry))) {
        queue_overflows_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AuditLogger::writerLoop() {
    std::vector<AuditEntry> batch;
    batch.reserve(config_.batch_size);

    while (running_.load(std::memory_order_acquire) ||
           flush_requested_.load(std::memory_order_acquire)) {

        // Collect batch
        batch.clear();
        AuditEntry entry;
        while (batch.size() < config_.batch_size && queue_.try_pop(entry)) {
            batch.push_back(std::move(entry));
        }

        if (batch.empty()) {
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Write batch
        std::lock_guard<std::mutex> lock(file_mutex_);

        // Open file if needed
        if (!binary_file_.is_open() || current_file_size_ >= config_.rotation_size_bytes) {
            rotateFile();
        }

        for (const auto& e : batch) {
            // Write binary
            auto binary_data = e.toBinary();
            binary_file_.write(reinterpret_cast<const char*>(binary_data.data()),
                               binary_data.size());
            current_file_size_ += binary_data.size();
            bytes_written_ += binary_data.size();

            // Write JSON if enabled
            if (config_.enable_json && json_file_.is_open()) {
                std::string json = e.toJson();
                json_file_ << json;
            }

            entries_written_.fetch_add(1, std::memory_order_relaxed);
        }

        if (config_.sync_to_disk) {
            binary_file_.flush();
            if (json_file_.is_open()) {
                json_file_.flush();
            }
        }

        flush_requested_.store(false, std::memory_order_release);
    }
}

void AuditLogger::rotateFile() {
    // Close current files
    if (binary_file_.is_open()) {
        binary_file_.close();
    }
    if (json_file_.is_open()) {
        json_file_.close();
    }

    // Open new files
    current_file_path_ = getCurrentFilePath();
    binary_file_.open(current_file_path_, std::ios::binary | std::ios::app);

    if (config_.enable_json) {
        std::string json_path = current_file_path_;
        json_path.replace(json_path.find(".bin"), 4, ".json");
        json_file_.open(json_path, std::ios::app);
    }

    current_file_size_ = 0;
    files_rotated_.fetch_add(1, std::memory_order_relaxed);

    // Cleanup old files
    cleanupOldFiles();
}

void AuditLogger::cleanupOldFiles() {
    if (config_.retention_days <= 0) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * config_.retention_days);
    auto cutoff_time = std::chrono::system_clock::to_time_t(cutoff);

    try {
        for (const auto& entry : std::filesystem::directory_iterator(config_.output_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            auto filename = entry.path().filename().string();
            if (filename.find(config_.file_prefix) != 0) {
                continue;
            }

            auto file_time = std::filesystem::last_write_time(entry);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                file_time - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            auto file_time_t = std::chrono::system_clock::to_time_t(sctp);

            if (file_time_t < cutoff_time) {
                std::filesystem::remove(entry.path());
            }
        }
    } catch (...) {
        // Ignore cleanup errors
    }
}

AuditLogger::Statistics AuditLogger::getStatistics() const {
    Statistics stats;
    stats.entries_logged = entries_logged_.load(std::memory_order_relaxed);
    stats.entries_written = entries_written_.load(std::memory_order_relaxed);
    stats.bytes_written = bytes_written_.load(std::memory_order_relaxed);
    stats.files_rotated = files_rotated_.load(std::memory_order_relaxed);
    stats.queue_overflows = queue_overflows_.load(std::memory_order_relaxed);
    stats.current_file_size = current_file_size_.load(std::memory_order_relaxed);
    return stats;
}

// ============================================================================
// Logging Methods
// ============================================================================

void AuditLogger::logOrderSent(const std::string& order_id,
                                const std::string& client_order_id,
                                const std::string& exchange,
                                const std::string& symbol,
                                OrderSideAudit side,
                                OrderTypeAudit type,
                                double price,
                                double quantity,
                                const std::string& raw_request) {
    AuditEntry entry;
    entry.event_type = AuditEventType::ORDER_SENT;
    entry.order_id = order_id;
    entry.client_order_id = client_order_id;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.side = side;
    entry.type = type;
    entry.price = price;
    entry.quantity = quantity;
    entry.raw_request = raw_request;
    entry.local_send_time_ns = getCurrentTimestampNs();
    log(std::move(entry));
}

void AuditLogger::logOrderAck(const std::string& order_id,
                               const std::string& exchange_order_id,
                               const std::string& exchange,
                               const std::string& symbol,
                               uint64_t exchange_timestamp_ns,
                               const std::string& raw_response) {
    AuditEntry entry;
    entry.event_type = AuditEventType::ORDER_ACK;
    entry.order_id = order_id;
    entry.exchange_order_id = exchange_order_id;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.exchange_timestamp_ns = exchange_timestamp_ns;
    entry.local_receive_time_ns = getCurrentTimestampNs();
    entry.raw_response = raw_response;
    log(std::move(entry));
}

void AuditLogger::logOrderReject(const std::string& order_id,
                                  const std::string& exchange,
                                  const std::string& symbol,
                                  const std::string& reason,
                                  const std::string& raw_response) {
    AuditEntry entry;
    entry.event_type = AuditEventType::ORDER_REJECT;
    entry.order_id = order_id;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.reason = reason;
    entry.raw_response = raw_response;
    entry.local_receive_time_ns = getCurrentTimestampNs();
    log(std::move(entry));
}

void AuditLogger::logOrderFill(const std::string& order_id,
                                const std::string& exchange_order_id,
                                const std::string& exchange,
                                const std::string& symbol,
                                OrderSideAudit side,
                                double fill_price,
                                double fill_qty,
                                double total_filled,
                                double commission,
                                bool is_partial,
                                const std::string& raw_message) {
    AuditEntry entry;
    entry.event_type = is_partial ? AuditEventType::ORDER_PARTIAL_FILL
                                  : AuditEventType::ORDER_FILL;
    entry.order_id = order_id;
    entry.exchange_order_id = exchange_order_id;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.side = side;
    entry.avg_fill_price = fill_price;
    entry.quantity = fill_qty;
    entry.filled_qty = total_filled;
    entry.commission = commission;
    entry.raw_response = raw_message;
    entry.local_receive_time_ns = getCurrentTimestampNs();
    log(std::move(entry));
}

void AuditLogger::logOrderCancel(const std::string& order_id,
                                  const std::string& exchange,
                                  const std::string& symbol,
                                  const std::string& raw_request) {
    AuditEntry entry;
    entry.event_type = AuditEventType::ORDER_CANCEL_SENT;
    entry.order_id = order_id;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.raw_request = raw_request;
    entry.local_send_time_ns = getCurrentTimestampNs();
    log(std::move(entry));
}

void AuditLogger::logOrderCancelAck(const std::string& order_id,
                                     const std::string& exchange,
                                     const std::string& symbol,
                                     const std::string& raw_response) {
    AuditEntry entry;
    entry.event_type = AuditEventType::ORDER_CANCEL_ACK;
    entry.order_id = order_id;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.raw_response = raw_response;
    entry.local_receive_time_ns = getCurrentTimestampNs();
    log(std::move(entry));
}

void AuditLogger::logRiskReject(const std::string& order_id,
                                 const std::string& exchange,
                                 const std::string& symbol,
                                 const std::string& reason) {
    AuditEntry entry;
    entry.event_type = AuditEventType::RISK_REJECT;
    entry.order_id = order_id;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.reason = reason;
    log(std::move(entry));
}

void AuditLogger::logCircuitBreaker(bool triggered,
                                     const std::string& reason,
                                     const std::string& details) {
    AuditEntry entry;
    entry.event_type = triggered ? AuditEventType::CIRCUIT_BREAKER_TRIGGERED
                                 : AuditEventType::CIRCUIT_BREAKER_RESET;
    entry.reason = reason;
    entry.metadata = details;
    log(std::move(entry));
}

void AuditLogger::logPositionChange(const std::string& exchange,
                                     const std::string& symbol,
                                     double old_qty,
                                     double new_qty,
                                     double avg_entry_price,
                                     double realized_pnl) {
    AuditEntry entry;
    entry.event_type = AuditEventType::POSITION_UPDATED;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.quantity = new_qty;
    entry.avg_fill_price = avg_entry_price;
    entry.commission = realized_pnl;  // Using commission field for realized PnL
    entry.metadata = "old_qty=" + std::to_string(old_qty);
    log(std::move(entry));
}

void AuditLogger::logConnectionUp(const std::string& exchange,
                                   const std::string& connection_type) {
    AuditEntry entry;
    entry.event_type = AuditEventType::CONNECTION_UP;
    entry.exchange = exchange;
    entry.metadata = connection_type;
    log(std::move(entry));
}

void AuditLogger::logConnectionDown(const std::string& exchange,
                                     const std::string& connection_type,
                                     const std::string& reason) {
    AuditEntry entry;
    entry.event_type = AuditEventType::CONNECTION_DOWN;
    entry.exchange = exchange;
    entry.reason = reason;
    entry.metadata = connection_type;
    log(std::move(entry));
}

void AuditLogger::logReconnectAttempt(const std::string& exchange,
                                       int attempt_number) {
    AuditEntry entry;
    entry.event_type = AuditEventType::RECONNECT_ATTEMPT;
    entry.exchange = exchange;
    entry.metadata = "attempt=" + std::to_string(attempt_number);
    log(std::move(entry));
}

void AuditLogger::logMarketDataGap(const std::string& exchange,
                                    const std::string& symbol,
                                    uint64_t expected_seq,
                                    uint64_t received_seq) {
    AuditEntry entry;
    entry.event_type = AuditEventType::MARKET_DATA_GAP;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.metadata = "expected=" + std::to_string(expected_seq) +
                     ",received=" + std::to_string(received_seq);
    log(std::move(entry));
}

void AuditLogger::logMarketDataStale(const std::string& exchange,
                                      const std::string& symbol,
                                      uint64_t staleness_ms) {
    AuditEntry entry;
    entry.event_type = AuditEventType::MARKET_DATA_STALE;
    entry.exchange = exchange;
    entry.symbol = symbol;
    entry.metadata = "staleness_ms=" + std::to_string(staleness_ms);
    log(std::move(entry));
}

void AuditLogger::logSystemStart(const std::string& version,
                                  const std::string& config_summary) {
    AuditEntry entry;
    entry.event_type = AuditEventType::SYSTEM_START;
    entry.reason = version;
    entry.metadata = config_summary;
    log(std::move(entry));
}

void AuditLogger::logSystemStop(const std::string& reason) {
    AuditEntry entry;
    entry.event_type = AuditEventType::SYSTEM_STOP;
    entry.reason = reason;
    log(std::move(entry));
}

void AuditLogger::logSystemError(const std::string& error,
                                  const std::string& details) {
    AuditEntry entry;
    entry.event_type = AuditEventType::SYSTEM_ERROR;
    entry.reason = error;
    entry.metadata = details;
    log(std::move(entry));
}

void AuditLogger::logCheckpoint(bool created, uint64_t checkpoint_id) {
    AuditEntry entry;
    entry.event_type = created ? AuditEventType::CHECKPOINT_CREATED
                               : AuditEventType::CHECKPOINT_RESTORED;
    entry.sequence_id = checkpoint_id;
    log(std::move(entry));
}

void AuditLogger::logStrategySignal(const std::string& strategy_name,
                                     const std::string& symbol,
                                     const std::string& signal_type,
                                     double confidence,
                                     const std::string& details) {
    AuditEntry entry;
    entry.event_type = AuditEventType::STRATEGY_SIGNAL;
    entry.symbol = symbol;
    entry.price = confidence;
    entry.reason = signal_type;
    entry.metadata = "strategy=" + strategy_name + "," + details;
    log(std::move(entry));
}

// ============================================================================
// AuditReader Implementation
// ============================================================================

AuditReader::AuditReader(const std::string& audit_dir)
    : audit_dir_(audit_dir) {}

std::vector<std::string> AuditReader::listLogFiles() const {
    std::vector<std::string> files;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(audit_dir_)) {
            if (entry.is_regular_file() &&
                entry.path().extension() == ".bin") {
                files.push_back(entry.path().string());
            }
        }
    } catch (...) {}

    std::sort(files.begin(), files.end());
    return files;
}

std::vector<AuditEntry> AuditReader::readFile(const std::string& path) {
    std::vector<AuditEntry> entries;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return entries;
    }

    size_t file_size = file.tellg();
    file.seekg(0);

    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);

    size_t pos = 0;
    while (pos + 4 < file_size) {
        // Read entry length
        uint32_t entry_len = data[pos] | (data[pos + 1] << 8) |
                             (data[pos + 2] << 16) | (data[pos + 3] << 24);
        pos += 4;

        if (pos + entry_len > file_size) {
            break;
        }

        try {
            size_t bytes_read;
            auto entry = AuditEntry::fromBinary(data.data() + pos, entry_len, bytes_read);
            entries.push_back(std::move(entry));
        } catch (...) {
            // Skip corrupted entries
        }

        pos += entry_len;
    }

    return entries;
}

std::vector<AuditEntry> AuditReader::queryByOrderId(const std::string& order_id) {
    std::vector<AuditEntry> results;
    for (const auto& file : listLogFiles()) {
        for (auto& entry : readFile(file)) {
            if (entry.order_id == order_id || entry.client_order_id == order_id) {
                results.push_back(std::move(entry));
            }
        }
    }
    return results;
}

std::vector<AuditEntry> AuditReader::queryByTimeRange(uint64_t start_ns, uint64_t end_ns) {
    std::vector<AuditEntry> results;
    for (const auto& file : listLogFiles()) {
        for (auto& entry : readFile(file)) {
            if (entry.timestamp_ns >= start_ns && entry.timestamp_ns <= end_ns) {
                results.push_back(std::move(entry));
            }
        }
    }
    return results;
}

std::vector<AuditEntry> AuditReader::queryByEventType(AuditEventType type) {
    std::vector<AuditEntry> results;
    for (const auto& file : listLogFiles()) {
        for (auto& entry : readFile(file)) {
            if (entry.event_type == type) {
                results.push_back(std::move(entry));
            }
        }
    }
    return results;
}

std::vector<AuditEntry> AuditReader::queryByExchange(const std::string& exchange) {
    std::vector<AuditEntry> results;
    for (const auto& file : listLogFiles()) {
        for (auto& entry : readFile(file)) {
            if (entry.exchange == exchange) {
                results.push_back(std::move(entry));
            }
        }
    }
    return results;
}

std::vector<AuditEntry> AuditReader::queryBySymbol(const std::string& symbol) {
    std::vector<AuditEntry> results;
    for (const auto& file : listLogFiles()) {
        for (auto& entry : readFile(file)) {
            if (entry.symbol == symbol) {
                results.push_back(std::move(entry));
            }
        }
    }
    return results;
}

void AuditReader::exportToCsv(const std::vector<AuditEntry>& entries,
                               const std::string& output_path) {
    std::ofstream file(output_path);
    if (!file) {
        return;
    }

    // Header
    file << "sequence_id,timestamp_ns,event_type,order_id,client_order_id,"
         << "exchange_order_id,exchange,symbol,side,type,price,quantity,"
         << "filled_qty,avg_fill_price,commission,exchange_ts,send_ts,"
         << "receive_ts,reason,metadata\n";

    for (const auto& e : entries) {
        file << e.sequence_id << ","
             << e.timestamp_ns << ","
             << static_cast<int>(e.event_type) << ","
             << "\"" << e.order_id << "\","
             << "\"" << e.client_order_id << "\","
             << "\"" << e.exchange_order_id << "\","
             << "\"" << e.exchange << "\","
             << "\"" << e.symbol << "\","
             << static_cast<int>(e.side) << ","
             << static_cast<int>(e.type) << ","
             << std::fixed << std::setprecision(8) << e.price << ","
             << e.quantity << ","
             << e.filled_qty << ","
             << e.avg_fill_price << ","
             << e.commission << ","
             << e.exchange_timestamp_ns << ","
             << e.local_send_time_ns << ","
             << e.local_receive_time_ns << ","
             << "\"" << e.reason << "\","
             << "\"" << e.metadata << "\"\n";
    }
}

} // namespace audit
} // namespace hft
