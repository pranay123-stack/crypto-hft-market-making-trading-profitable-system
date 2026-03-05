#include "core/checkpoint_manager.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace hft {
namespace core {

// ============================================================================
// CRC32 Implementation (IEEE 802.3)
// ============================================================================

static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd706b3,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

uint32_t Checkpoint::calculateCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// Checkpoint Serialization
// ============================================================================

// Helper to write to buffer
class BufferWriter {
public:
    void writeU8(uint8_t v) { data_.push_back(v); }
    void writeU16(uint16_t v) {
        data_.push_back(v & 0xFF);
        data_.push_back((v >> 8) & 0xFF);
    }
    void writeU32(uint32_t v) {
        data_.push_back(v & 0xFF);
        data_.push_back((v >> 8) & 0xFF);
        data_.push_back((v >> 16) & 0xFF);
        data_.push_back((v >> 24) & 0xFF);
    }
    void writeU64(uint64_t v) {
        writeU32(v & 0xFFFFFFFF);
        writeU32((v >> 32) & 0xFFFFFFFF);
    }
    void writeDouble(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        writeU64(bits);
    }
    void writeString(const std::string& s) {
        writeU32(static_cast<uint32_t>(s.size()));
        data_.insert(data_.end(), s.begin(), s.end());
    }
    void writeBytes(const std::vector<uint8_t>& bytes) {
        writeU32(static_cast<uint32_t>(bytes.size()));
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }
    std::vector<uint8_t>& data() { return data_; }

private:
    std::vector<uint8_t> data_;
};

// Helper to read from buffer
class BufferReader {
public:
    explicit BufferReader(const std::vector<uint8_t>& data) : data_(data), pos_(0) {}

    bool hasRemaining(size_t n) const { return pos_ + n <= data_.size(); }
    uint8_t readU8() { return data_[pos_++]; }
    uint16_t readU16() {
        uint16_t v = data_[pos_] | (data_[pos_ + 1] << 8);
        pos_ += 2;
        return v;
    }
    uint32_t readU32() {
        uint32_t v = data_[pos_] | (data_[pos_ + 1] << 8) |
                     (data_[pos_ + 2] << 16) | (data_[pos_ + 3] << 24);
        pos_ += 4;
        return v;
    }
    uint64_t readU64() {
        uint64_t low = readU32();
        uint64_t high = readU32();
        return low | (high << 32);
    }
    double readDouble() {
        uint64_t bits = readU64();
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string readString() {
        uint32_t len = readU32();
        std::string s(data_.begin() + pos_, data_.begin() + pos_ + len);
        pos_ += len;
        return s;
    }
    std::vector<uint8_t> readBytes() {
        uint32_t len = readU32();
        std::vector<uint8_t> bytes(data_.begin() + pos_, data_.begin() + pos_ + len);
        pos_ += len;
        return bytes;
    }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_;
};

std::vector<uint8_t> Checkpoint::serialize() const {
    BufferWriter writer;

    // Header
    writer.writeU32(MAGIC);
    writer.writeU16(version);
    writer.writeU64(checkpoint_id);
    writer.writeU64(created_at);
    writer.writeString(system_version);

    // Open orders
    writer.writeU32(static_cast<uint32_t>(open_orders.size()));
    for (const auto& order : open_orders) {
        writer.writeString(order.order_id);
        writer.writeString(order.client_order_id);
        writer.writeString(order.exchange_order_id);
        writer.writeString(order.exchange);
        writer.writeString(order.symbol);
        writer.writeU8(static_cast<uint8_t>(order.side));
        writer.writeU8(static_cast<uint8_t>(order.type));
        writer.writeU8(static_cast<uint8_t>(order.status));
        writer.writeDouble(order.price);
        writer.writeDouble(order.quantity);
        writer.writeDouble(order.filled_qty);
        writer.writeDouble(order.avg_fill_price);
        writer.writeU64(order.create_time);
        writer.writeU64(order.update_time);
    }

    // Positions
    writer.writeU32(static_cast<uint32_t>(positions.size()));
    for (const auto& pos : positions) {
        writer.writeString(pos.symbol);
        writer.writeString(pos.exchange);
        writer.writeDouble(pos.quantity);
        writer.writeDouble(pos.avg_entry_price);
        writer.writeDouble(pos.realized_pnl);
        writer.writeDouble(pos.unrealized_pnl);
        writer.writeU64(pos.last_update_time);
    }

    // PnL state
    writer.writeDouble(realized_pnl);
    writer.writeDouble(unrealized_pnl);
    writer.writeDouble(daily_pnl);
    writer.writeDouble(high_water_mark);

    // Sequences
    writer.writeU32(static_cast<uint32_t>(last_sequences.size()));
    for (const auto& [exchange, seq] : last_sequences) {
        writer.writeString(exchange);
        writer.writeU64(seq);
    }

    // Risk state
    writer.writeDouble(current_exposure);
    writer.writeDouble(max_drawdown);
    writer.writeU32(static_cast<uint32_t>(orders_today));
    writer.writeU32(static_cast<uint32_t>(fills_today));

    // Provider data
    writer.writeU32(static_cast<uint32_t>(provider_data.size()));
    for (const auto& [name, data] : provider_data) {
        writer.writeString(name);
        writer.writeBytes(data);
    }

    // Calculate and append CRC32
    uint32_t crc = calculateCrc32(writer.data().data(), writer.data().size());
    writer.writeU32(crc);

    return writer.data();
}

Checkpoint Checkpoint::deserialize(const std::vector<uint8_t>& data) {
    Checkpoint cp;
    BufferReader reader(data);

    // Verify CRC first
    if (data.size() < 4) {
        throw std::runtime_error("Checkpoint data too small");
    }
    uint32_t stored_crc;
    std::memcpy(&stored_crc, data.data() + data.size() - 4, sizeof(stored_crc));
    uint32_t calc_crc = calculateCrc32(data.data(), data.size() - 4);
    if (stored_crc != calc_crc) {
        throw std::runtime_error("Checkpoint CRC mismatch");
    }

    // Header
    uint32_t magic = reader.readU32();
    if (magic != MAGIC) {
        throw std::runtime_error("Invalid checkpoint magic number");
    }
    cp.version = reader.readU16();
    cp.checkpoint_id = reader.readU64();
    cp.created_at = reader.readU64();
    cp.system_version = reader.readString();

    // Open orders
    uint32_t num_orders = reader.readU32();
    cp.open_orders.reserve(num_orders);
    for (uint32_t i = 0; i < num_orders; ++i) {
        OrderState order;
        order.order_id = reader.readString();
        order.client_order_id = reader.readString();
        order.exchange_order_id = reader.readString();
        order.exchange = reader.readString();
        order.symbol = reader.readString();
        order.side = reader.readU8();
        order.type = reader.readU8();
        order.status = reader.readU8();
        order.price = reader.readDouble();
        order.quantity = reader.readDouble();
        order.filled_qty = reader.readDouble();
        order.avg_fill_price = reader.readDouble();
        order.create_time = reader.readU64();
        order.update_time = reader.readU64();
        cp.open_orders.push_back(std::move(order));
    }

    // Positions
    uint32_t num_positions = reader.readU32();
    cp.positions.reserve(num_positions);
    for (uint32_t i = 0; i < num_positions; ++i) {
        PositionState pos;
        pos.symbol = reader.readString();
        pos.exchange = reader.readString();
        pos.quantity = reader.readDouble();
        pos.avg_entry_price = reader.readDouble();
        pos.realized_pnl = reader.readDouble();
        pos.unrealized_pnl = reader.readDouble();
        pos.last_update_time = reader.readU64();
        cp.positions.push_back(std::move(pos));
    }

    // PnL state
    cp.realized_pnl = reader.readDouble();
    cp.unrealized_pnl = reader.readDouble();
    cp.daily_pnl = reader.readDouble();
    cp.high_water_mark = reader.readDouble();

    // Sequences
    uint32_t num_sequences = reader.readU32();
    for (uint32_t i = 0; i < num_sequences; ++i) {
        std::string exchange = reader.readString();
        uint64_t seq = reader.readU64();
        cp.last_sequences[exchange] = seq;
    }

    // Risk state
    cp.current_exposure = reader.readDouble();
    cp.max_drawdown = reader.readDouble();
    cp.orders_today = static_cast<int>(reader.readU32());
    cp.fills_today = static_cast<int>(reader.readU32());

    // Provider data
    uint32_t num_providers = reader.readU32();
    for (uint32_t i = 0; i < num_providers; ++i) {
        std::string name = reader.readString();
        std::vector<uint8_t> pdata = reader.readBytes();
        cp.provider_data[name] = std::move(pdata);
    }

    return cp;
}

bool Checkpoint::validate(const std::vector<uint8_t>& data) {
    if (data.size() < 10) {  // Minimum: magic(4) + version(2) + crc(4)
        return false;
    }

    // Check magic
    uint32_t magic;
    std::memcpy(&magic, data.data(), sizeof(magic));
    if (magic != MAGIC) {
        return false;
    }

    // Check CRC
    uint32_t stored_crc;
    std::memcpy(&stored_crc, data.data() + data.size() - 4, sizeof(stored_crc));
    uint32_t calc_crc = calculateCrc32(data.data(), data.size() - 4);
    return stored_crc == calc_crc;
}

// ============================================================================
// CheckpointManager Implementation
// ============================================================================

CheckpointManager::CheckpointManager(const CheckpointConfig& config)
    : config_(config) {
    // Create checkpoint directory if it doesn't exist
    std::filesystem::create_directories(config_.checkpoint_dir);
}

CheckpointManager::~CheckpointManager() {
    stop();
}

void CheckpointManager::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    checkpoint_thread_ = std::thread(&CheckpointManager::checkpointLoop, this);
}

void CheckpointManager::stop() {
    running_.store(false, std::memory_order_release);
    if (checkpoint_thread_.joinable()) {
        checkpoint_thread_.join();
    }
}

void CheckpointManager::setConfig(const CheckpointConfig& config) {
    config_ = config;
    std::filesystem::create_directories(config_.checkpoint_dir);
}

void CheckpointManager::registerProvider(const std::string& name,
                                          std::shared_ptr<ICheckpointProvider> provider) {
    std::unique_lock<std::shared_mutex> lock(providers_mutex_);
    providers_[name] = std::move(provider);
}

void CheckpointManager::unregisterProvider(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(providers_mutex_);
    providers_.erase(name);
}

Checkpoint CheckpointManager::buildCheckpoint() {
    Checkpoint cp;

    auto now = std::chrono::high_resolution_clock::now();
    cp.checkpoint_id = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    cp.created_at = cp.checkpoint_id;
    cp.system_version = "1.0.0";

    // Copy current state
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cp.open_orders = current_orders_;
        cp.positions = current_positions_;
        cp.realized_pnl = realized_pnl_;
        cp.unrealized_pnl = unrealized_pnl_;
        cp.daily_pnl = daily_pnl_;
        cp.high_water_mark = high_water_mark_;
        cp.current_exposure = current_exposure_;
        cp.max_drawdown = max_drawdown_;
        cp.orders_today = orders_today_;
        cp.fills_today = fills_today_;
        cp.last_sequences = sequences_;
    }

    // Get provider data
    {
        std::shared_lock<std::shared_mutex> lock(providers_mutex_);
        for (const auto& [name, provider] : providers_) {
            try {
                cp.provider_data[name] = provider->getState();
            } catch (const std::exception& e) {
                notifyError("Provider " + name + " failed: " + e.what());
            }
        }
    }

    return cp;
}

bool CheckpointManager::createCheckpoint() {
    auto start = std::chrono::high_resolution_clock::now();

    try {
        Checkpoint cp = buildCheckpoint();

        if (!writeCheckpoint(cp)) {
            return false;
        }

        last_checkpoint_id_.store(cp.checkpoint_id, std::memory_order_release);
        checkpoints_created_.fetch_add(1, std::memory_order_relaxed);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        last_checkpoint_duration_us_.store(duration.count(), std::memory_order_relaxed);
        last_checkpoint_time_.store(cp.created_at, std::memory_order_relaxed);

        notifyCreated(cp.checkpoint_id);

        if (config_.auto_cleanup) {
            cleanupOldCheckpoints();
        }

        return true;
    } catch (const std::exception& e) {
        checkpoint_errors_.fetch_add(1, std::memory_order_relaxed);
        notifyError("Checkpoint creation failed: " + std::string(e.what()));
        return false;
    }
}

bool CheckpointManager::writeCheckpoint(const Checkpoint& checkpoint) {
    std::string temp_path = getCheckpointPath(checkpoint.checkpoint_id) + ".tmp";
    std::string final_path = getCheckpointPath(checkpoint.checkpoint_id);

    // Serialize
    std::vector<uint8_t> data = checkpoint.serialize();
    last_checkpoint_size_.store(data.size(), std::memory_order_relaxed);

    // Write to temp file
    std::ofstream file(temp_path, std::ios::binary);
    if (!file) {
        notifyError("Failed to open checkpoint file: " + temp_path);
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());

    if (config_.sync_to_disk) {
        file.flush();
        // Note: fsync would require platform-specific code
    }

    file.close();

    // Atomic rename
    try {
        std::filesystem::rename(temp_path, final_path);
    } catch (const std::exception& e) {
        std::filesystem::remove(temp_path);
        notifyError("Failed to rename checkpoint: " + std::string(e.what()));
        return false;
    }

    return true;
}

std::string CheckpointManager::getCheckpointPath(uint64_t checkpoint_id) const {
    std::ostringstream oss;
    oss << config_.checkpoint_dir << "/" << config_.file_prefix << "_" << checkpoint_id << ".bin";
    return oss.str();
}

bool CheckpointManager::hasCheckpoint() const {
    return !listCheckpointIds().empty();
}

std::vector<uint64_t> CheckpointManager::listCheckpointIds() const {
    std::vector<uint64_t> ids;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(config_.checkpoint_dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find(config_.file_prefix) == 0 && filename.ends_with(".bin")) {
                    // Extract ID from filename
                    size_t start = config_.file_prefix.length() + 1;
                    size_t end = filename.find(".bin");
                    if (end > start) {
                        std::string id_str = filename.substr(start, end - start);
                        try {
                            ids.push_back(std::stoull(id_str));
                        } catch (...) {
                            // Ignore invalid filenames
                        }
                    }
                }
            }
        }
    } catch (...) {
        // Directory might not exist
    }

    std::sort(ids.begin(), ids.end(), std::greater<uint64_t>());
    return ids;
}

std::optional<Checkpoint> CheckpointManager::loadLatestCheckpoint() const {
    auto ids = listCheckpointIds();
    if (ids.empty()) {
        return std::nullopt;
    }
    return loadCheckpoint(ids[0]);
}

std::optional<Checkpoint> CheckpointManager::loadCheckpoint(uint64_t checkpoint_id) const {
    return readCheckpoint(getCheckpointPath(checkpoint_id));
}

std::optional<Checkpoint> CheckpointManager::readCheckpoint(const std::string& path) const {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    size_t size = file.tellg();
    file.seekg(0);

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (!Checkpoint::validate(data)) {
        return std::nullopt;
    }

    try {
        return Checkpoint::deserialize(data);
    } catch (...) {
        return std::nullopt;
    }
}

bool CheckpointManager::restoreFromCheckpoint(const Checkpoint& checkpoint) {
    try {
        // Restore to state
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            current_orders_ = checkpoint.open_orders;
            current_positions_ = checkpoint.positions;
            realized_pnl_ = checkpoint.realized_pnl;
            unrealized_pnl_ = checkpoint.unrealized_pnl;
            daily_pnl_ = checkpoint.daily_pnl;
            high_water_mark_ = checkpoint.high_water_mark;
            current_exposure_ = checkpoint.current_exposure;
            max_drawdown_ = checkpoint.max_drawdown;
            orders_today_ = checkpoint.orders_today;
            fills_today_ = checkpoint.fills_today;
            sequences_ = checkpoint.last_sequences;
        }

        // Restore providers (sorted by priority)
        std::vector<std::pair<int, std::string>> providers_sorted;
        {
            std::shared_lock<std::shared_mutex> lock(providers_mutex_);
            for (const auto& [name, provider] : providers_) {
                providers_sorted.emplace_back(provider->getRestorePriority(), name);
            }
        }
        std::sort(providers_sorted.begin(), providers_sorted.end());

        for (const auto& [priority, name] : providers_sorted) {
            auto it = checkpoint.provider_data.find(name);
            if (it != checkpoint.provider_data.end()) {
                std::shared_lock<std::shared_mutex> lock(providers_mutex_);
                auto provider_it = providers_.find(name);
                if (provider_it != providers_.end()) {
                    provider_it->second->setState(it->second);
                }
            }
        }

        checkpoints_restored_.fetch_add(1, std::memory_order_relaxed);
        notifyRestored(checkpoint.checkpoint_id);
        return true;
    } catch (const std::exception& e) {
        notifyError("Restore failed: " + std::string(e.what()));
        return false;
    }
}

bool CheckpointManager::restoreFromLatest() {
    auto cp = loadLatestCheckpoint();
    if (!cp) {
        return false;
    }
    return restoreFromCheckpoint(*cp);
}

void CheckpointManager::setOpenOrders(const std::vector<OrderState>& orders) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_orders_ = orders;
}

void CheckpointManager::setPositions(const std::vector<PositionState>& positions) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_positions_ = positions;
}

void CheckpointManager::setPnLState(double realized, double unrealized, double daily, double hwm) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    realized_pnl_ = realized;
    unrealized_pnl_ = unrealized;
    daily_pnl_ = daily;
    high_water_mark_ = hwm;
}

void CheckpointManager::setRiskState(double exposure, double drawdown, int orders, int fills) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_exposure_ = exposure;
    max_drawdown_ = drawdown;
    orders_today_ = orders;
    fills_today_ = fills;
}

void CheckpointManager::setSequence(const std::string& exchange, uint64_t sequence) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    sequences_[exchange] = sequence;
}

void CheckpointManager::setOnCheckpointCreated(OnCheckpointCreated callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_created_ = std::move(callback);
}

void CheckpointManager::setOnCheckpointRestored(OnCheckpointRestored callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_restored_ = std::move(callback);
}

void CheckpointManager::setOnCheckpointError(OnCheckpointError callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_error_ = std::move(callback);
}

CheckpointManager::Statistics CheckpointManager::getStatistics() const {
    Statistics stats;
    stats.checkpoints_created = checkpoints_created_.load(std::memory_order_relaxed);
    stats.checkpoints_restored = checkpoints_restored_.load(std::memory_order_relaxed);
    stats.checkpoint_errors = checkpoint_errors_.load(std::memory_order_relaxed);
    stats.last_checkpoint_time = last_checkpoint_time_.load(std::memory_order_relaxed);
    stats.last_checkpoint_size = last_checkpoint_size_.load(std::memory_order_relaxed);
    stats.last_checkpoint_duration_us = last_checkpoint_duration_us_.load(std::memory_order_relaxed);
    return stats;
}

void CheckpointManager::cleanupOldCheckpoints() {
    auto ids = listCheckpointIds();
    if (ids.size() <= config_.max_checkpoints) {
        return;
    }

    // Delete oldest checkpoints
    for (size_t i = config_.max_checkpoints; i < ids.size(); ++i) {
        try {
            std::filesystem::remove(getCheckpointPath(ids[i]));
        } catch (...) {
            // Ignore deletion errors
        }
    }
}

void CheckpointManager::checkpointLoop() {
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(config_.checkpoint_interval);

        if (running_.load(std::memory_order_acquire)) {
            createCheckpoint();
        }
    }
}

void CheckpointManager::notifyCreated(uint64_t id) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (on_created_) {
        on_created_(id);
    }
}

void CheckpointManager::notifyRestored(uint64_t id) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (on_restored_) {
        on_restored_(id);
    }
}

void CheckpointManager::notifyError(const std::string& error) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (on_error_) {
        on_error_(error);
    }
}

} // namespace core
} // namespace hft
