/**
 * @file data_feed.cpp
 * @brief Historical market data feed implementation
 */

#include "backtesting/data_feed.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <random>

#ifdef __linux__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hft::backtesting {

// ============================================================================
// CSVParser Implementation
// ============================================================================

CSVParser::CSVParser(char delimiter)
    : delimiter_(delimiter)
    , timestamp_format_("%Y-%m-%d %H:%M:%S") {
}

std::vector<std::string_view> CSVParser::splitLine(std::string_view line) const {
    std::vector<std::string_view> fields;
    size_t start = 0;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            in_quotes = !in_quotes;
        } else if (line[i] == delimiter_ && !in_quotes) {
            fields.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }

    // Add the last field
    if (start <= line.size()) {
        auto field = line.substr(start);
        // Remove trailing whitespace/newlines
        while (!field.empty() && (field.back() == '\n' || field.back() == '\r' ||
                                   field.back() == ' ')) {
            field.remove_suffix(1);
        }
        fields.push_back(field);
    }

    return fields;
}

core::Timestamp CSVParser::parseTimestamp(std::string_view ts) const {
    // Try parsing as Unix timestamp (seconds or milliseconds)
    if (ts.find('-') == std::string_view::npos && ts.find(':') == std::string_view::npos) {
        uint64_t value = 0;
        auto result = std::from_chars(ts.data(), ts.data() + ts.size(), value);
        if (result.ec == std::errc{}) {
            // Determine if seconds, milliseconds, or microseconds
            if (value < 10000000000ULL) {
                // Seconds (10 digits max for reasonable dates)
                return core::Timestamp::from_seconds(value);
            } else if (value < 10000000000000ULL) {
                // Milliseconds
                return core::Timestamp::from_millis(value);
            } else {
                // Microseconds
                return core::Timestamp::from_micros(value);
            }
        }
    }

    // Parse ISO 8601 format: YYYY-MM-DDTHH:MM:SS.ffffff
    std::tm tm{};
    int year, month, day, hour, minute, second;
    uint64_t microseconds = 0;

    // Find decimal point for fractional seconds
    auto dot_pos = ts.find('.');
    std::string_view datetime_part = ts;
    std::string_view frac_part;

    if (dot_pos != std::string_view::npos) {
        datetime_part = ts.substr(0, dot_pos);
        frac_part = ts.substr(dot_pos + 1);
    }

    // Parse datetime part
    if (datetime_part.size() >= 19) {
        // Format: YYYY-MM-DDTHH:MM:SS or YYYY-MM-DD HH:MM:SS
        year = (datetime_part[0] - '0') * 1000 + (datetime_part[1] - '0') * 100 +
               (datetime_part[2] - '0') * 10 + (datetime_part[3] - '0');
        month = (datetime_part[5] - '0') * 10 + (datetime_part[6] - '0');
        day = (datetime_part[8] - '0') * 10 + (datetime_part[9] - '0');
        hour = (datetime_part[11] - '0') * 10 + (datetime_part[12] - '0');
        minute = (datetime_part[14] - '0') * 10 + (datetime_part[15] - '0');
        second = (datetime_part[17] - '0') * 10 + (datetime_part[18] - '0');

        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
    }

    // Parse fractional seconds
    if (!frac_part.empty()) {
        // Remove timezone suffix if present
        auto z_pos = frac_part.find('Z');
        if (z_pos != std::string_view::npos) {
            frac_part = frac_part.substr(0, z_pos);
        }

        // Pad or truncate to 6 digits (microseconds)
        std::string frac_str(frac_part);
        while (frac_str.size() < 6) frac_str += '0';
        frac_str = frac_str.substr(0, 6);

        std::from_chars(frac_str.data(), frac_str.data() + frac_str.size(), microseconds);
    }

    // Convert to Unix timestamp
    std::time_t unix_time = timegm(&tm);
    return core::Timestamp::from_micros(
        static_cast<uint64_t>(unix_time) * 1000000ULL + microseconds);
}

core::Price CSVParser::parsePrice(std::string_view price) const {
    double value = 0.0;

    // Remove any quotes
    while (!price.empty() && price.front() == '"') price.remove_prefix(1);
    while (!price.empty() && price.back() == '"') price.remove_suffix(1);

    // Fast path for simple numbers
    auto result = std::from_chars(price.data(), price.data() + price.size(), value);
    if (result.ec != std::errc{}) {
        // Fallback to stod for complex formats
        try {
            value = std::stod(std::string(price));
        } catch (...) {
            value = 0.0;
        }
    }

    return core::Price::from_double(value);
}

core::Quantity CSVParser::parseQuantity(std::string_view qty) const {
    double value = 0.0;

    // Remove any quotes
    while (!qty.empty() && qty.front() == '"') qty.remove_prefix(1);
    while (!qty.empty() && qty.back() == '"') qty.remove_suffix(1);

    auto result = std::from_chars(qty.data(), qty.data() + qty.size(), value);
    if (result.ec != std::errc{}) {
        try {
            value = std::stod(std::string(qty));
        } catch (...) {
            value = 0.0;
        }
    }

    return core::Quantity::from_double(value);
}

std::optional<Trade> CSVParser::parseTrade(std::string_view line) const {
    auto fields = splitLine(line);

    // Expected format: timestamp,price,quantity,side[,trade_id]
    // or: timestamp,symbol,price,quantity,side[,trade_id]
    if (fields.size() < 4) {
        return std::nullopt;
    }

    Trade trade;
    trade.exchange = default_exchange_;
    trade.symbol = default_symbol_;

    size_t idx = 0;

    // Parse timestamp
    trade.timestamp = parseTimestamp(fields[idx++]);

    // Check if second field is a symbol (non-numeric)
    if (fields.size() >= 5 && !std::isdigit(fields[idx][0])) {
        trade.symbol = core::Symbol(fields[idx++]);
    }

    // Parse price and quantity
    trade.price = parsePrice(fields[idx++]);
    trade.quantity = parseQuantity(fields[idx++]);

    // Parse side
    std::string_view side = fields[idx++];
    if (side == "buy" || side == "BUY" || side == "b" || side == "B" || side == "1") {
        trade.aggressor_side = core::Side::Buy;
    } else {
        trade.aggressor_side = core::Side::Sell;
    }

    // Optional trade ID
    if (idx < fields.size()) {
        uint64_t trade_id = 0;
        std::from_chars(fields[idx].data(), fields[idx].data() + fields[idx].size(), trade_id);
        trade.trade_id = core::TradeId{trade_id};
    }

    return trade;
}

std::optional<OHLCVBar> CSVParser::parseOHLCV(std::string_view line) const {
    auto fields = splitLine(line);

    // Expected format: timestamp,open,high,low,close,volume[,quote_volume,trades]
    // or: timestamp,symbol,open,high,low,close,volume[,quote_volume,trades]
    if (fields.size() < 6) {
        return std::nullopt;
    }

    OHLCVBar bar;
    bar.exchange = default_exchange_;
    bar.symbol = default_symbol_;

    size_t idx = 0;

    // Parse timestamp
    bar.timestamp = parseTimestamp(fields[idx++]);

    // Check if second field is a symbol
    if (fields.size() >= 7 && !std::isdigit(fields[idx][0])) {
        bar.symbol = core::Symbol(fields[idx++]);
    }

    // Parse OHLCV
    bar.open = parsePrice(fields[idx++]);
    bar.high = parsePrice(fields[idx++]);
    bar.low = parsePrice(fields[idx++]);
    bar.close = parsePrice(fields[idx++]);
    bar.volume = parseQuantity(fields[idx++]);

    // Optional fields
    if (idx < fields.size()) {
        bar.quote_volume = parseQuantity(fields[idx++]);
    }
    if (idx < fields.size()) {
        uint32_t count = 0;
        std::from_chars(fields[idx].data(), fields[idx].data() + fields[idx].size(), count);
        bar.trade_count = count;
    }

    return bar;
}

std::optional<OrderBookSnapshot> CSVParser::parseOrderBookSnapshot(std::string_view line) const {
    auto fields = splitLine(line);

    // Expected format: timestamp,bid1_price,bid1_qty,ask1_price,ask1_qty,...
    if (fields.size() < 5) {
        return std::nullopt;
    }

    OrderBookSnapshot snapshot;
    snapshot.exchange = default_exchange_;
    snapshot.symbol = default_symbol_;
    snapshot.timestamp = parseTimestamp(fields[0]);

    size_t idx = 1;

    // Parse bid levels
    while (idx + 1 < fields.size() && snapshot.bid_count < OrderBookSnapshot::MAX_DEPTH) {
        auto& level = snapshot.bids[snapshot.bid_count];
        level.price = parsePrice(fields[idx++]);
        level.quantity = parseQuantity(fields[idx++]);

        if (!level.price.is_zero()) {
            snapshot.bid_count++;
        }

        // Check if we've reached asks section
        if (idx < fields.size()) {
            auto price = parsePrice(fields[idx]);
            if (snapshot.bid_count > 0 && price > snapshot.bids[0].price) {
                break;  // Price higher than best bid, must be asks
            }
        }
    }

    // Parse ask levels
    while (idx + 1 < fields.size() && snapshot.ask_count < OrderBookSnapshot::MAX_DEPTH) {
        auto& level = snapshot.asks[snapshot.ask_count];
        level.price = parsePrice(fields[idx++]);
        level.quantity = parseQuantity(fields[idx++]);

        if (!level.price.is_zero()) {
            snapshot.ask_count++;
        }
    }

    return snapshot;
}

std::optional<OrderBookUpdate> CSVParser::parseOrderBookUpdate(std::string_view line) const {
    auto fields = splitLine(line);

    // Expected format: timestamp,side,price,quantity[,update_type]
    if (fields.size() < 4) {
        return std::nullopt;
    }

    OrderBookUpdate update;
    update.exchange = default_exchange_;
    update.symbol = default_symbol_;
    update.timestamp = parseTimestamp(fields[0]);

    // Parse side
    std::string_view side = fields[1];
    if (side == "bid" || side == "BID" || side == "b" || side == "B" || side == "buy") {
        update.side = core::Side::Buy;
    } else {
        update.side = core::Side::Sell;
    }

    update.price = parsePrice(fields[2]);
    update.quantity = parseQuantity(fields[3]);

    // Determine update type
    if (update.quantity.is_zero()) {
        update.type = OrderBookUpdate::UpdateType::Delete;
    } else {
        update.type = OrderBookUpdate::UpdateType::Set;
    }

    return update;
}

std::vector<Trade> CSVParser::parseTrades(std::istream& input, size_t max_count) const {
    std::vector<Trade> trades;
    std::string line;

    // Skip header if present
    if (std::getline(input, line)) {
        // Check if this looks like a header
        if (line.find("timestamp") != std::string::npos ||
            line.find("time") != std::string::npos ||
            line.find("price") == std::string::npos) {
            // This is a header, continue to next line
        } else {
            // Not a header, parse it
            auto trade = parseTrade(line);
            if (trade) {
                trades.push_back(*trade);
            }
        }
    }

    // Parse remaining lines
    while (std::getline(input, line)) {
        if (max_count > 0 && trades.size() >= max_count) break;

        auto trade = parseTrade(line);
        if (trade) {
            trades.push_back(*trade);
        }
    }

    return trades;
}

std::vector<OHLCVBar> CSVParser::parseOHLCVBars(std::istream& input, size_t max_count) const {
    std::vector<OHLCVBar> bars;
    std::string line;

    // Skip header
    if (std::getline(input, line)) {
        if (line.find("open") == std::string::npos) {
            auto bar = parseOHLCV(line);
            if (bar) {
                bars.push_back(*bar);
            }
        }
    }

    while (std::getline(input, line)) {
        if (max_count > 0 && bars.size() >= max_count) break;

        auto bar = parseOHLCV(line);
        if (bar) {
            bars.push_back(*bar);
        }
    }

    return bars;
}

DataEventType CSVParser::detectFormat(std::string_view header) const {
    std::string lower_header(header);
    std::transform(lower_header.begin(), lower_header.end(), lower_header.begin(), ::tolower);

    if (lower_header.find("open") != std::string::npos &&
        lower_header.find("high") != std::string::npos &&
        lower_header.find("low") != std::string::npos &&
        lower_header.find("close") != std::string::npos) {
        return DataEventType::OHLCVBar;
    }

    if (lower_header.find("bid") != std::string::npos &&
        lower_header.find("ask") != std::string::npos) {
        return DataEventType::OrderBookSnapshot;
    }

    if (lower_header.find("side") != std::string::npos &&
        lower_header.find("price") != std::string::npos &&
        lower_header.find("open") == std::string::npos) {
        // Could be trade or order book update
        if (lower_header.find("trade") != std::string::npos ||
            lower_header.find("qty") != std::string::npos ||
            lower_header.find("quantity") != std::string::npos) {
            return DataEventType::Trade;
        }
        return DataEventType::OrderBookUpdate;
    }

    // Default to trade
    return DataEventType::Trade;
}

// ============================================================================
// BinaryParser Implementation
// ============================================================================

std::optional<Trade> BinaryParser::parseTrade(const uint8_t* data, size_t size) const {
    if (size < TRADE_RECORD_SIZE) {
        return std::nullopt;
    }

    Trade trade;

    // Layout: timestamp(8) + exchange(1) + pad(3) + symbol(24) + trade_id(8) +
    //         price(8) + quantity(8) + side(1) + pad(3) = 64 bytes
    size_t offset = 0;

    std::memcpy(&trade.timestamp.nanos, data + offset, 8);
    offset += 8;

    trade.exchange = static_cast<core::Exchange>(data[offset]);
    offset += 4;  // 1 byte + 3 padding

    std::memcpy(trade.symbol.data.data(), data + offset, 24);
    trade.symbol.length = static_cast<uint8_t>(std::strlen(trade.symbol.c_str()));
    offset += 24;

    std::memcpy(&trade.trade_id.value, data + offset, 8);
    offset += 8;

    std::memcpy(&trade.price.value, data + offset, 8);
    offset += 8;

    std::memcpy(&trade.quantity.value, data + offset, 8);
    offset += 8;

    trade.aggressor_side = static_cast<core::Side>(data[offset]);

    return trade;
}

std::optional<OHLCVBar> BinaryParser::parseOHLCV(const uint8_t* data, size_t size) const {
    if (size < OHLCV_RECORD_SIZE) {
        return std::nullopt;
    }

    OHLCVBar bar;
    size_t offset = 0;

    // Layout: timestamp(8) + close_time(8) + exchange(1) + pad(3) + symbol(24) +
    //         open(8) + high(8) + low(8) + close(8) + volume(8) + quote_volume(8) +
    //         trade_count(4) + pad(4) = 104 bytes

    std::memcpy(&bar.timestamp.nanos, data + offset, 8);
    offset += 8;

    std::memcpy(&bar.close_time.nanos, data + offset, 8);
    offset += 8;

    bar.exchange = static_cast<core::Exchange>(data[offset]);
    offset += 4;

    std::memcpy(bar.symbol.data.data(), data + offset, 24);
    bar.symbol.length = static_cast<uint8_t>(std::strlen(bar.symbol.c_str()));
    offset += 24;

    std::memcpy(&bar.open.value, data + offset, 8);
    offset += 8;

    std::memcpy(&bar.high.value, data + offset, 8);
    offset += 8;

    std::memcpy(&bar.low.value, data + offset, 8);
    offset += 8;

    std::memcpy(&bar.close.value, data + offset, 8);
    offset += 8;

    std::memcpy(&bar.volume.value, data + offset, 8);
    offset += 8;

    std::memcpy(&bar.quote_volume.value, data + offset, 8);
    offset += 8;

    std::memcpy(&bar.trade_count, data + offset, 4);

    return bar;
}

std::optional<OrderBookSnapshot> BinaryParser::parseOrderBookSnapshot(
    const uint8_t* data, size_t size) const {

    // Header: magic(4) + timestamp(8) + exchange(1) + pad(3) + symbol(24) +
    //         sequence(8) + bid_count(1) + ask_count(1) + pad(2) = 52 bytes
    // Then: levels * (price(8) + quantity(8) + order_count(4) + pad(4)) = 24 bytes each

    if (size < 52) {
        return std::nullopt;
    }

    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != ORDERBOOK_MAGIC) {
        return std::nullopt;
    }

    OrderBookSnapshot snapshot;
    size_t offset = 4;

    std::memcpy(&snapshot.timestamp.nanos, data + offset, 8);
    offset += 8;

    snapshot.exchange = static_cast<core::Exchange>(data[offset]);
    offset += 4;

    std::memcpy(snapshot.symbol.data.data(), data + offset, 24);
    snapshot.symbol.length = static_cast<uint8_t>(std::strlen(snapshot.symbol.c_str()));
    offset += 24;

    std::memcpy(&snapshot.sequence_number, data + offset, 8);
    offset += 8;

    snapshot.bid_count = data[offset++];
    snapshot.ask_count = data[offset++];
    offset += 2;  // padding

    // Parse bid levels
    for (uint8_t i = 0; i < snapshot.bid_count && offset + 24 <= size; ++i) {
        std::memcpy(&snapshot.bids[i].price.value, data + offset, 8);
        offset += 8;
        std::memcpy(&snapshot.bids[i].quantity.value, data + offset, 8);
        offset += 8;
        std::memcpy(&snapshot.bids[i].order_count, data + offset, 4);
        offset += 8;  // 4 bytes + 4 padding
    }

    // Parse ask levels
    for (uint8_t i = 0; i < snapshot.ask_count && offset + 24 <= size; ++i) {
        std::memcpy(&snapshot.asks[i].price.value, data + offset, 8);
        offset += 8;
        std::memcpy(&snapshot.asks[i].quantity.value, data + offset, 8);
        offset += 8;
        std::memcpy(&snapshot.asks[i].order_count, data + offset, 4);
        offset += 8;
    }

    return snapshot;
}

std::vector<Trade> BinaryParser::loadTrades(const uint8_t* data, size_t size) const {
    std::vector<Trade> trades;

    // Check header
    if (size < 8) return trades;

    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != TRADE_MAGIC) return trades;

    uint32_t count;
    std::memcpy(&count, data + 4, 4);

    trades.reserve(count);

    size_t offset = 8;
    while (offset + TRADE_RECORD_SIZE <= size && trades.size() < count) {
        auto trade = parseTrade(data + offset, size - offset);
        if (trade) {
            trades.push_back(*trade);
        }
        offset += TRADE_RECORD_SIZE;
    }

    return trades;
}

std::vector<OHLCVBar> BinaryParser::loadOHLCVBars(const uint8_t* data, size_t size) const {
    std::vector<OHLCVBar> bars;

    if (size < 8) return bars;

    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != OHLCV_MAGIC) return bars;

    uint32_t count;
    std::memcpy(&count, data + 4, 4);

    bars.reserve(count);

    size_t offset = 8;
    while (offset + OHLCV_RECORD_SIZE <= size && bars.size() < count) {
        auto bar = parseOHLCV(data + offset, size - offset);
        if (bar) {
            bars.push_back(*bar);
        }
        offset += OHLCV_RECORD_SIZE;
    }

    return bars;
}

std::vector<uint8_t> BinaryParser::serializeTrade(const Trade& trade) const {
    std::vector<uint8_t> buffer(TRADE_RECORD_SIZE, 0);
    size_t offset = 0;

    std::memcpy(buffer.data() + offset, &trade.timestamp.nanos, 8);
    offset += 8;

    buffer[offset] = static_cast<uint8_t>(trade.exchange);
    offset += 4;

    std::memcpy(buffer.data() + offset, trade.symbol.data.data(), 24);
    offset += 24;

    std::memcpy(buffer.data() + offset, &trade.trade_id.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &trade.price.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &trade.quantity.value, 8);
    offset += 8;

    buffer[offset] = static_cast<uint8_t>(trade.aggressor_side);

    return buffer;
}

std::vector<uint8_t> BinaryParser::serializeOHLCV(const OHLCVBar& bar) const {
    std::vector<uint8_t> buffer(OHLCV_RECORD_SIZE, 0);
    size_t offset = 0;

    std::memcpy(buffer.data() + offset, &bar.timestamp.nanos, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &bar.close_time.nanos, 8);
    offset += 8;

    buffer[offset] = static_cast<uint8_t>(bar.exchange);
    offset += 4;

    std::memcpy(buffer.data() + offset, bar.symbol.data.data(), 24);
    offset += 24;

    std::memcpy(buffer.data() + offset, &bar.open.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &bar.high.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &bar.low.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &bar.close.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &bar.volume.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &bar.quote_volume.value, 8);
    offset += 8;

    std::memcpy(buffer.data() + offset, &bar.trade_count, 4);

    return buffer;
}

std::vector<uint8_t> BinaryParser::serializeOrderBookSnapshot(
    const OrderBookSnapshot& snapshot) const {

    size_t level_size = 24;  // price(8) + qty(8) + count(4) + pad(4)
    size_t total_size = 52 + (snapshot.bid_count + snapshot.ask_count) * level_size;
    std::vector<uint8_t> buffer(total_size, 0);

    size_t offset = 0;

    uint32_t magic = ORDERBOOK_MAGIC;
    std::memcpy(buffer.data() + offset, &magic, 4);
    offset += 4;

    std::memcpy(buffer.data() + offset, &snapshot.timestamp.nanos, 8);
    offset += 8;

    buffer[offset] = static_cast<uint8_t>(snapshot.exchange);
    offset += 4;

    std::memcpy(buffer.data() + offset, snapshot.symbol.data.data(), 24);
    offset += 24;

    std::memcpy(buffer.data() + offset, &snapshot.sequence_number, 8);
    offset += 8;

    buffer[offset++] = snapshot.bid_count;
    buffer[offset++] = snapshot.ask_count;
    offset += 2;

    for (uint8_t i = 0; i < snapshot.bid_count; ++i) {
        std::memcpy(buffer.data() + offset, &snapshot.bids[i].price.value, 8);
        offset += 8;
        std::memcpy(buffer.data() + offset, &snapshot.bids[i].quantity.value, 8);
        offset += 8;
        std::memcpy(buffer.data() + offset, &snapshot.bids[i].order_count, 4);
        offset += 8;
    }

    for (uint8_t i = 0; i < snapshot.ask_count; ++i) {
        std::memcpy(buffer.data() + offset, &snapshot.asks[i].price.value, 8);
        offset += 8;
        std::memcpy(buffer.data() + offset, &snapshot.asks[i].quantity.value, 8);
        offset += 8;
        std::memcpy(buffer.data() + offset, &snapshot.asks[i].order_count, 4);
        offset += 8;
    }

    return buffer;
}

// ============================================================================
// MemoryMappedFile Implementation
// ============================================================================

MemoryMappedFile::~MemoryMappedFile() {
    close();
}

MemoryMappedFile::MemoryMappedFile(MemoryMappedFile&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
    , fd_(other.fd_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

MemoryMappedFile& MemoryMappedFile::operator=(MemoryMappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
        fd_ = other.fd_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    return *this;
}

bool MemoryMappedFile::open(const std::filesystem::path& path) {
#ifdef __linux__
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ == -1) {
        return false;
    }

    struct stat st;
    if (fstat(fd_, &st) == -1) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    size_ = static_cast<size_t>(st.st_size);

    if (size_ == 0) {
        ::close(fd_);
        fd_ = -1;
        data_ = nullptr;
        return true;  // Empty file is valid
    }

    data_ = static_cast<uint8_t*>(
        mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));

    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Advise the kernel about sequential access
    madvise(data_, size_, MADV_SEQUENTIAL);

    return true;
#else
    // Fallback for non-Linux systems: read entire file into memory
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;

    size_ = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    data_ = new uint8_t[size_];
    file.read(reinterpret_cast<char*>(data_), size_);

    return true;
#endif
}

void MemoryMappedFile::close() {
#ifdef __linux__
    if (data_ != nullptr) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
#else
    delete[] data_;
    data_ = nullptr;
#endif
    size_ = 0;
}

// ============================================================================
// DataStream Iterator Implementation
// ============================================================================

template<typename T>
DataStream<T>::Iterator::Iterator(DataStream* stream, bool end)
    : stream_(stream), at_end_(end) {
    if (!at_end_ && stream_ && stream_->hasNext()) {
        auto value = stream_->next();
        if (value) {
            current_ = *value;
        } else {
            at_end_ = true;
        }
    } else {
        at_end_ = true;
    }
}

template<typename T>
typename DataStream<T>::Iterator& DataStream<T>::Iterator::operator++() {
    if (stream_ && stream_->hasNext()) {
        auto value = stream_->next();
        if (value) {
            current_ = *value;
        } else {
            at_end_ = true;
        }
    } else {
        at_end_ = true;
    }
    return *this;
}

template<typename T>
typename DataStream<T>::Iterator DataStream<T>::Iterator::operator++(int) {
    Iterator tmp = *this;
    ++(*this);
    return tmp;
}

template<typename T>
bool DataStream<T>::Iterator::operator==(const Iterator& other) const {
    return at_end_ == other.at_end_;
}

template<typename T>
bool DataStream<T>::Iterator::operator!=(const Iterator& other) const {
    return !(*this == other);
}

// Explicit template instantiations
template class DataStream<Trade>;
template class DataStream<OHLCVBar>;
template class DataStream<OrderBookSnapshot>;
template class DataStream<DataEvent>;

// ============================================================================
// DataFeed Implementation
// ============================================================================

DataFeed::DataFeed(const DataFeedConfig& config)
    : config_(config)
    , current_time_(config.start_time) {
}

DataFeed::~DataFeed() {
    shutdown();
}

bool DataFeed::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_.load()) {
        return true;
    }

    // Scan and load data from all configured paths
    for (const auto& path : config_.data_paths) {
        if (std::filesystem::is_directory(path)) {
            if (!scanDirectory(path)) {
                return false;
            }
        } else if (std::filesystem::is_regular_file(path)) {
            if (config_.format == DataFormat::CSV) {
                if (!loadCSVFile(path)) {
                    return false;
                }
            } else if (config_.format == DataFormat::Binary) {
                if (!loadBinaryFile(path)) {
                    return false;
                }
            }
        }
    }

    // Sort all data by timestamp
    std::sort(trades_.begin(), trades_.end(),
        [](const Trade& a, const Trade& b) {
            return a.timestamp < b.timestamp;
        });

    std::sort(ohlcv_bars_.begin(), ohlcv_bars_.end(),
        [](const OHLCVBar& a, const OHLCVBar& b) {
            return a.timestamp < b.timestamp;
        });

    std::sort(order_book_snapshots_.begin(), order_book_snapshots_.end(),
        [](const OrderBookSnapshot& a, const OrderBookSnapshot& b) {
            return a.timestamp < b.timestamp;
        });

    std::sort(order_book_updates_.begin(), order_book_updates_.end(),
        [](const OrderBookUpdate& a, const OrderBookUpdate& b) {
            return a.timestamp < b.timestamp;
        });

    // Count total events
    total_events_ = trades_.size() + ohlcv_bars_.size() +
                    order_book_snapshots_.size() + order_book_updates_.size();

    // Calculate memory usage
    memory_usage_ = trades_.size() * sizeof(Trade) +
                    ohlcv_bars_.size() * sizeof(OHLCVBar) +
                    order_book_snapshots_.size() * sizeof(OrderBookSnapshot) +
                    order_book_updates_.size() * sizeof(OrderBookUpdate);

    // Build list of loaded exchanges and symbols
    std::set<core::Exchange> exchanges_set;
    std::set<std::string> symbols_set;

    for (const auto& trade : trades_) {
        exchanges_set.insert(trade.exchange);
        symbols_set.insert(std::string(trade.symbol.view()));
    }

    for (auto ex : exchanges_set) {
        loaded_exchanges_.push_back(ex);
    }

    for (const auto& sym : symbols_set) {
        loaded_symbols_.push_back(core::Symbol(sym));
    }

    // Initialize event queue with first events from each data type
    refillEventQueue();

    initialized_.store(true);
    return true;
}

void DataFeed::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    trades_.clear();
    ohlcv_bars_.clear();
    order_book_snapshots_.clear();
    order_book_updates_.clear();
    current_order_books_.clear();

    while (!event_queue_.empty()) {
        event_queue_.pop();
    }

    sources_.clear();
    initialized_.store(false);
}

bool DataFeed::hasNext() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !event_queue_.empty() ||
           current_trade_idx_ < trades_.size() ||
           current_ohlcv_idx_ < ohlcv_bars_.size() ||
           current_snapshot_idx_ < order_book_snapshots_.size() ||
           current_update_idx_ < order_book_updates_.size();
}

std::optional<DataEvent> DataFeed::next() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (event_queue_.empty()) {
        refillEventQueue();
    }

    if (event_queue_.empty()) {
        return std::nullopt;
    }

    auto event = event_queue_.top();
    event_queue_.pop();

    current_time_ = event.event.timestamp;
    events_processed_++;

    // Refill queue if running low
    if (event_queue_.size() < config_.prefetch_events / 2) {
        refillEventQueue();
    }

    return event.event;
}

void DataFeed::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    current_trade_idx_ = 0;
    current_ohlcv_idx_ = 0;
    current_snapshot_idx_ = 0;
    current_update_idx_ = 0;
    current_time_ = config_.start_time;
    events_processed_ = 0;

    while (!event_queue_.empty()) {
        event_queue_.pop();
    }

    current_order_books_.clear();

    refillEventQueue();
}

void DataFeed::forEach(EventCallback callback) {
    reset();

    while (hasNext()) {
        auto event = next();
        if (event) {
            callback(*event);
        }
    }
}

std::optional<DataEvent> DataFeed::seekTo(core::Timestamp timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Binary search to find positions in each data array
    current_trade_idx_ = std::lower_bound(trades_.begin(), trades_.end(), timestamp,
        [](const Trade& t, core::Timestamp ts) {
            return t.timestamp < ts;
        }) - trades_.begin();

    current_ohlcv_idx_ = std::lower_bound(ohlcv_bars_.begin(), ohlcv_bars_.end(), timestamp,
        [](const OHLCVBar& b, core::Timestamp ts) {
            return b.timestamp < ts;
        }) - ohlcv_bars_.begin();

    current_snapshot_idx_ = std::lower_bound(
        order_book_snapshots_.begin(), order_book_snapshots_.end(), timestamp,
        [](const OrderBookSnapshot& s, core::Timestamp ts) {
            return s.timestamp < ts;
        }) - order_book_snapshots_.begin();

    current_update_idx_ = std::lower_bound(
        order_book_updates_.begin(), order_book_updates_.end(), timestamp,
        [](const OrderBookUpdate& u, core::Timestamp ts) {
            return u.timestamp < ts;
        }) - order_book_updates_.begin();

    current_time_ = timestamp;

    while (!event_queue_.empty()) {
        event_queue_.pop();
    }

    refillEventQueue();

    if (!event_queue_.empty()) {
        return event_queue_.top().event;
    }

    return std::nullopt;
}

std::optional<OrderBookSnapshot> DataFeed::getOrderBook(
    core::Exchange exchange, const core::Symbol& symbol) const {

    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = std::to_string(static_cast<int>(exchange)) + "_" +
                      std::string(symbol.view());

    auto it = current_order_books_.find(key);
    if (it != current_order_books_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<Trade> DataFeed::getTrades(
    core::Exchange exchange, const core::Symbol& symbol,
    core::Timestamp start, core::Timestamp end) const {

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Trade> result;

    auto start_it = std::lower_bound(trades_.begin(), trades_.end(), start,
        [](const Trade& t, core::Timestamp ts) {
            return t.timestamp < ts;
        });

    auto end_it = std::upper_bound(trades_.begin(), trades_.end(), end,
        [](core::Timestamp ts, const Trade& t) {
            return ts < t.timestamp;
        });

    for (auto it = start_it; it != end_it; ++it) {
        if (it->exchange == exchange && it->symbol == symbol) {
            result.push_back(*it);
        }
    }

    return result;
}

std::vector<OHLCVBar> DataFeed::getOHLCV(
    core::Exchange exchange, const core::Symbol& symbol,
    core::Timestamp start, core::Timestamp end) const {

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<OHLCVBar> result;

    auto start_it = std::lower_bound(ohlcv_bars_.begin(), ohlcv_bars_.end(), start,
        [](const OHLCVBar& b, core::Timestamp ts) {
            return b.timestamp < ts;
        });

    auto end_it = std::upper_bound(ohlcv_bars_.begin(), ohlcv_bars_.end(), end,
        [](core::Timestamp ts, const OHLCVBar& b) {
            return ts < b.timestamp;
        });

    for (auto it = start_it; it != end_it; ++it) {
        if (it->exchange == exchange && it->symbol == symbol) {
            result.push_back(*it);
        }
    }

    return result;
}

double DataFeed::progress() const noexcept {
    if (total_events_ == 0) return 1.0;
    return static_cast<double>(events_processed_) / static_cast<double>(total_events_);
}

bool DataFeed::loadCSVFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    // Determine data type from filename or header
    std::string filename = path.filename().string();
    std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);

    // Extract exchange and symbol from filename pattern: EXCHANGE_SYMBOL_datatype.csv
    core::Exchange exchange = core::Exchange::Unknown;
    core::Symbol symbol;

    // Try to parse filename pattern
    auto underscore1 = filename.find('_');
    if (underscore1 != std::string::npos) {
        std::string exchange_str = filename.substr(0, underscore1);
        if (exchange_str == "binance") exchange = core::Exchange::Binance;
        else if (exchange_str == "coinbase") exchange = core::Exchange::Coinbase;
        else if (exchange_str == "kraken") exchange = core::Exchange::Kraken;
        else if (exchange_str == "bybit") exchange = core::Exchange::Bybit;
        else if (exchange_str == "okx") exchange = core::Exchange::OKX;

        auto underscore2 = filename.find('_', underscore1 + 1);
        if (underscore2 != std::string::npos) {
            symbol = core::Symbol(filename.substr(underscore1 + 1, underscore2 - underscore1 - 1));
        }
    }

    CSVParser parser(config_.csv_delimiter);
    parser.setExchange(exchange);
    parser.setSymbol(symbol);
    parser.setTimestampFormat(config_.timestamp_format);

    // Read header to detect format
    std::string header;
    if (!std::getline(file, header)) {
        return false;
    }

    DataEventType format = parser.detectFormat(header);

    // Reset to beginning
    file.clear();
    file.seekg(0);

    if (format == DataEventType::Trade && config_.include_trades) {
        auto loaded_trades = parser.parseTrades(file);
        trades_.insert(trades_.end(), loaded_trades.begin(), loaded_trades.end());
    } else if (format == DataEventType::OHLCVBar && config_.include_ohlcv) {
        auto loaded_bars = parser.parseOHLCVBars(file);
        ohlcv_bars_.insert(ohlcv_bars_.end(), loaded_bars.begin(), loaded_bars.end());
    }

    return true;
}

bool DataFeed::loadBinaryFile(const std::filesystem::path& path) {
    MemoryMappedFile mmap;
    if (!mmap.open(path)) {
        return false;
    }

    if (mmap.size() < 4) {
        return false;
    }

    uint32_t magic;
    std::memcpy(&magic, mmap.data(), 4);

    BinaryParser parser;

    if (magic == BinaryParser::TRADE_MAGIC && config_.include_trades) {
        auto loaded_trades = parser.loadTrades(mmap.data(), mmap.size());
        trades_.insert(trades_.end(), loaded_trades.begin(), loaded_trades.end());
    } else if (magic == BinaryParser::OHLCV_MAGIC && config_.include_ohlcv) {
        auto loaded_bars = parser.loadOHLCVBars(mmap.data(), mmap.size());
        ohlcv_bars_.insert(ohlcv_bars_.end(), loaded_bars.begin(), loaded_bars.end());
    }

    return true;
}

bool DataFeed::scanDirectory(const std::filesystem::path& dir) {
    if (!std::filesystem::exists(dir)) {
        return false;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;

        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".csv" && config_.format == DataFormat::CSV) {
            loadCSVFile(entry.path());
        } else if ((ext == ".bin" || ext == ".dat") && config_.format == DataFormat::Binary) {
            loadBinaryFile(entry.path());
        }
    }

    return true;
}

void DataFeed::refillEventQueue() {
    // Add events from each data type up to prefetch limit
    size_t to_add = config_.prefetch_events - event_queue_.size();
    size_t added = 0;

    // Add trades
    while (current_trade_idx_ < trades_.size() && added < to_add) {
        const auto& trade = trades_[current_trade_idx_];

        // Filter by time range
        if (trade.timestamp < config_.start_time) {
            current_trade_idx_++;
            continue;
        }
        if (config_.end_time.nanos > 0 && trade.timestamp > config_.end_time) {
            current_trade_idx_ = trades_.size();
            break;
        }

        DataEvent event;
        event.type = DataEventType::Trade;
        event.timestamp = trade.timestamp;
        event.exchange = trade.exchange;
        event.symbol = trade.symbol;
        event.data.trade.trade = &trades_[current_trade_idx_];

        event_queue_.push({event, 0});
        current_trade_idx_++;
        added++;
    }

    // Add OHLCV bars
    while (current_ohlcv_idx_ < ohlcv_bars_.size() && added < to_add) {
        const auto& bar = ohlcv_bars_[current_ohlcv_idx_];

        if (bar.timestamp < config_.start_time) {
            current_ohlcv_idx_++;
            continue;
        }
        if (config_.end_time.nanos > 0 && bar.timestamp > config_.end_time) {
            current_ohlcv_idx_ = ohlcv_bars_.size();
            break;
        }

        DataEvent event;
        event.type = DataEventType::OHLCVBar;
        event.timestamp = bar.timestamp;
        event.exchange = bar.exchange;
        event.symbol = bar.symbol;
        event.data.ohlcv.bar = &ohlcv_bars_[current_ohlcv_idx_];

        event_queue_.push({event, 1});
        current_ohlcv_idx_++;
        added++;
    }

    // Add order book snapshots
    while (current_snapshot_idx_ < order_book_snapshots_.size() && added < to_add) {
        const auto& snapshot = order_book_snapshots_[current_snapshot_idx_];

        if (snapshot.timestamp < config_.start_time) {
            current_snapshot_idx_++;
            continue;
        }
        if (config_.end_time.nanos > 0 && snapshot.timestamp > config_.end_time) {
            current_snapshot_idx_ = order_book_snapshots_.size();
            break;
        }

        DataEvent event;
        event.type = DataEventType::OrderBookSnapshot;
        event.timestamp = snapshot.timestamp;
        event.exchange = snapshot.exchange;
        event.symbol = snapshot.symbol;
        event.data.order_book.snapshot = &order_book_snapshots_[current_snapshot_idx_];

        // Update current order book state
        std::string key = std::to_string(static_cast<int>(snapshot.exchange)) + "_" +
                          std::string(snapshot.symbol.view());
        current_order_books_[key] = snapshot;

        event_queue_.push({event, 2});
        current_snapshot_idx_++;
        added++;
    }

    // Add order book updates
    while (current_update_idx_ < order_book_updates_.size() && added < to_add) {
        const auto& update = order_book_updates_[current_update_idx_];

        if (update.timestamp < config_.start_time) {
            current_update_idx_++;
            continue;
        }
        if (config_.end_time.nanos > 0 && update.timestamp > config_.end_time) {
            current_update_idx_ = order_book_updates_.size();
            break;
        }

        DataEvent event;
        event.type = DataEventType::OrderBookUpdate;
        event.timestamp = update.timestamp;
        event.exchange = update.exchange;
        event.symbol = update.symbol;
        event.data.book_update.update = &order_book_updates_[current_update_idx_];

        event_queue_.push({event, 3});
        current_update_idx_++;
        added++;
    }
}

// ============================================================================
// MultiExchangeDataSynchronizer Implementation
// ============================================================================

MultiExchangeDataSynchronizer::MultiExchangeDataSynchronizer(const SyncConfig& config)
    : config_(config) {
}

void MultiExchangeDataSynchronizer::addFeed(core::Exchange exchange,
                                             std::shared_ptr<DataFeed> feed) {
    ExchangeFeed ef;
    ef.exchange = exchange;
    ef.feed = std::move(feed);
    ef.clock_offset = 0;
    feeds_.push_back(std::move(ef));
}

bool MultiExchangeDataSynchronizer::hasNext() const {
    for (const auto& feed : feeds_) {
        if (feed.buffered_event.has_value() || feed.feed->hasNext()) {
            return true;
        }
    }
    return false;
}

std::optional<DataEvent> MultiExchangeDataSynchronizer::next() {
    // Ensure all feeds have a buffered event
    for (auto& feed : feeds_) {
        if (!feed.buffered_event.has_value() && feed.feed->hasNext()) {
            feed.buffered_event = feed.feed->next();

            // Apply clock offset correction if configured
            if (config_.adjust_timestamps && feed.buffered_event.has_value()) {
                auto& event = *feed.buffered_event;
                int64_t adjusted = static_cast<int64_t>(event.timestamp.nanos) + feed.clock_offset;
                event.timestamp = core::Timestamp{static_cast<uint64_t>(std::max(int64_t{0}, adjusted))};
            }
        }
    }

    // Find the event with the earliest timestamp
    size_t best_idx = feeds_.size();
    core::Timestamp best_time = core::Timestamp::max();

    for (size_t i = 0; i < feeds_.size(); ++i) {
        if (feeds_[i].buffered_event.has_value()) {
            if (feeds_[i].buffered_event->timestamp < best_time) {
                best_time = feeds_[i].buffered_event->timestamp;
                best_idx = i;
            }
        }
    }

    if (best_idx == feeds_.size()) {
        return std::nullopt;
    }

    auto result = feeds_[best_idx].buffered_event;
    feeds_[best_idx].buffered_event.reset();
    current_time_ = best_time;

    return result;
}

void MultiExchangeDataSynchronizer::reset() {
    for (auto& feed : feeds_) {
        feed.feed->reset();
        feed.buffered_event.reset();
    }
    current_time_ = core::Timestamp::zero();
}

void MultiExchangeDataSynchronizer::estimateClockOffsets() {
    // Implementation would analyze overlapping periods to estimate clock differences
    // This is a placeholder for future implementation
    for (auto& feed : feeds_) {
        feed.clock_offset = 0;
    }
}

// ============================================================================
// DataFeedFactory Implementation
// ============================================================================

std::unique_ptr<DataFeed> DataFeedFactory::create(const DataFeedConfig& config) {
    auto feed = std::make_unique<DataFeed>(config);
    if (feed->initialize()) {
        return feed;
    }
    return nullptr;
}

std::unique_ptr<DataFeed> DataFeedFactory::fromFile(
    const std::filesystem::path& path,
    core::Exchange exchange,
    const core::Symbol& symbol) {

    DataFeedConfig config;
    config.data_paths.push_back(path);

    // Detect format from extension
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".csv") {
        config.format = DataFormat::CSV;
    } else {
        config.format = DataFormat::Binary;
    }

    config.exchanges.push_back(exchange);
    config.symbols.push_back(symbol);

    return create(config);
}

std::unique_ptr<DataFeed> DataFeedFactory::fromDirectory(
    const std::filesystem::path& dir,
    core::Timestamp start,
    core::Timestamp end) {

    DataFeedConfig config;
    config.data_paths.push_back(dir);
    config.start_time = start;
    config.end_time = end;

    // Detect format from files in directory
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".csv") {
                config.format = DataFormat::CSV;
                break;
            } else if (ext == ".bin" || ext == ".dat") {
                config.format = DataFormat::Binary;
                break;
            }
        }
    }

    return create(config);
}

std::unique_ptr<DataFeed> DataFeedFactory::createSynthetic(
    core::Exchange exchange,
    const core::Symbol& symbol,
    core::Timestamp start,
    core::Timestamp end,
    uint64_t events_per_second) {

    // Create a synthetic data feed for testing
    DataFeedConfig config;
    config.start_time = start;
    config.end_time = end;
    config.exchanges.push_back(exchange);
    config.symbols.push_back(symbol);

    auto feed = std::make_unique<DataFeed>(config);

    // Generate synthetic trade data
    std::mt19937_64 rng(42);  // Fixed seed for reproducibility
    std::normal_distribution<double> price_dist(0.0, 0.0001);  // 0.01% volatility
    std::exponential_distribution<double> size_dist(1.0);

    double base_price = 50000.0;  // Starting price
    uint64_t trade_id = 1;

    uint64_t interval_ns = 1'000'000'000ULL / events_per_second;
    core::Timestamp current = start;

    while (current < end) {
        // Random walk price
        base_price *= (1.0 + price_dist(rng));

        Trade trade;
        trade.timestamp = current;
        trade.exchange = exchange;
        trade.symbol = symbol;
        trade.trade_id = core::TradeId{trade_id++};
        trade.price = core::Price::from_double(base_price);
        trade.quantity = core::Quantity::from_double(0.1 + size_dist(rng) * 0.5);
        trade.aggressor_side = (rng() % 2 == 0) ? core::Side::Buy : core::Side::Sell;

        // Directly add to feed's internal storage (would need friend access or alternative)
        // For now, this is a placeholder - actual implementation would need proper injection

        current = core::Timestamp{current.nanos + interval_ns};
    }

    return feed;
}

}  // namespace hft::backtesting
