#include "network/connection_pool.hpp"

// This file contains explicit template instantiations for common connection types
// The connection pool is a header-only template library, but we provide
// common instantiations here to improve compilation times

namespace hft {
namespace network {

// Most functionality is in the header as templates
// This file exists for potential future non-template code and
// to ensure the header compiles correctly

// Example explicit instantiations would go here if needed:
// template class ConnectionPool<SomeConnectionType>;
// template class PooledConnection<SomeConnectionType>;
// template class ConnectionGuard<SomeConnectionType>;

} // namespace network
} // namespace hft
