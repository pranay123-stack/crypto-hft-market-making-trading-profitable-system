"""
Core types for the HFT system.

Uses integer arithmetic for prices/quantities to avoid floating-point issues.
"""

from dataclasses import dataclass, field
from enum import IntEnum
from typing import Optional
import time

# Type aliases
Price = int  # Fixed-point with 8 decimal places
Quantity = int  # Fixed-point with 8 decimal places
OrderId = int
Timestamp = int  # Nanoseconds since epoch

# Precision constant
PRECISION = 100_000_000  # 8 decimal places


class Side(IntEnum):
    BUY = 0
    SELL = 1

    def opposite(self) -> "Side":
        return Side.SELL if self == Side.BUY else Side.BUY

    def __str__(self) -> str:
        return "BUY" if self == Side.BUY else "SELL"


class OrderType(IntEnum):
    LIMIT = 0
    MARKET = 1
    LIMIT_MAKER = 2
    IOC = 3
    FOK = 4


class OrderStatus(IntEnum):
    NEW = 0
    PARTIALLY_FILLED = 1
    FILLED = 2
    CANCELED = 3
    REJECTED = 4
    EXPIRED = 5


class TimeInForce(IntEnum):
    GTC = 0  # Good till cancel
    IOC = 1  # Immediate or cancel
    FOK = 2  # Fill or kill
    GTX = 3  # Good till crossing (post-only)


@dataclass
class Symbol:
    """Trading symbol."""
    value: str

    def __str__(self) -> str:
        return self.value

    def __hash__(self) -> int:
        return hash(self.value)


@dataclass
class Order:
    """Order representation."""
    id: OrderId = 0
    client_id: OrderId = 0
    symbol: Symbol = field(default_factory=lambda: Symbol(""))
    side: Side = Side.BUY
    order_type: OrderType = OrderType.LIMIT
    time_in_force: TimeInForce = TimeInForce.GTC
    price: Price = 0
    quantity: Quantity = 0
    filled_qty: Quantity = 0
    status: OrderStatus = OrderStatus.NEW
    timestamp: Timestamp = 0

    @property
    def remaining(self) -> Quantity:
        return self.quantity - self.filled_qty

    @property
    def is_active(self) -> bool:
        return self.status in (OrderStatus.NEW, OrderStatus.PARTIALLY_FILLED)


@dataclass
class Quote:
    """Bid/ask quote."""
    bid_price: Price = 0
    ask_price: Price = 0
    bid_qty: Quantity = 0
    ask_qty: Quantity = 0
    timestamp: Timestamp = 0

    @property
    def spread(self) -> Price:
        return self.ask_price - self.bid_price

    @property
    def mid_price(self) -> Price:
        return (self.bid_price + self.ask_price) // 2

    @property
    def spread_bps(self) -> float:
        mid = self.mid_price
        if mid == 0:
            return 0.0
        return 10000.0 * self.spread / mid


@dataclass
class Trade:
    """Trade execution."""
    order_id: OrderId = 0
    trade_id: OrderId = 0
    symbol: Symbol = field(default_factory=lambda: Symbol(""))
    side: Side = Side.BUY
    price: Price = 0
    quantity: Quantity = 0
    timestamp: Timestamp = 0
    is_maker: bool = False


@dataclass
class Tick:
    """Market data tick."""
    bid: Price = 0
    ask: Price = 0
    bid_qty: Quantity = 0
    ask_qty: Quantity = 0
    last_price: Price = 0
    last_qty: Quantity = 0
    exchange_ts: Timestamp = 0
    local_ts: Timestamp = 0
    sequence: int = 0


# Conversion functions
def to_price(value: float) -> Price:
    """Convert float to fixed-point price."""
    return int(value * PRECISION)


def from_price(price: Price) -> float:
    """Convert fixed-point price to float."""
    return price / PRECISION


def to_qty(value: float) -> Quantity:
    """Convert float to fixed-point quantity."""
    return int(value * PRECISION)


def from_qty(qty: Quantity) -> float:
    """Convert fixed-point quantity to float."""
    return qty / PRECISION


def now_ns() -> Timestamp:
    """Get current timestamp in nanoseconds."""
    return int(time.time_ns())


def now_ms() -> int:
    """Get current timestamp in milliseconds."""
    return int(time.time() * 1000)
