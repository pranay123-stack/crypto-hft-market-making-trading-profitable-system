/**
 * @file lock_free_queue.cpp
 * @brief Implementation of lock-free queue utilities
 *
 * The main queue implementation is header-only (template classes).
 * This file contains explicit template instantiations for common types
 * to improve compilation times in large projects.
 */

#include "core/lock_free_queue.hpp"
#include "core/types.hpp"

namespace hft::core {

// Explicit template instantiations for common queue sizes and types
// This can reduce compilation times by avoiding redundant template instantiation

// Common message types that will be queued
struct OrderMessage {
    OrderId order_id;
    Price price;
    Quantity quantity;
    Side side;
    OrderType type;
    Exchange exchange;
    Symbol symbol;
    Timestamp timestamp;
};

struct MarketDataMessage {
    Price bid_price;
    Price ask_price;
    Quantity bid_qty;
    Quantity ask_qty;
    Exchange exchange;
    Symbol symbol;
    Timestamp timestamp;
};

// Explicit instantiations for SPSC queues
template class SPSCQueue<OrderMessage, 1024>;
template class SPSCQueue<OrderMessage, 4096>;
template class SPSCQueue<OrderMessage, 16384>;

template class SPSCQueue<MarketDataMessage, 1024>;
template class SPSCQueue<MarketDataMessage, 4096>;
template class SPSCQueue<MarketDataMessage, 16384>;
template class SPSCQueue<MarketDataMessage, 65536>;

// Simple types
template class SPSCQueue<uint64_t, 1024>;
template class SPSCQueue<uint64_t, 4096>;

// Explicit instantiations for MPMC queues
template class MPMCQueue<OrderMessage, 1024>;
template class MPMCQueue<OrderMessage, 4096>;

template class MPMCQueue<MarketDataMessage, 1024>;
template class MPMCQueue<MarketDataMessage, 4096>;

}  // namespace hft::core
