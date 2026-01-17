"""Core types for multi-exchange HFT system."""

from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Optional
import time


# Type aliases
Price = int  # Fixed-point: actual_price * 10^8
Quantity = int  # Fixed-point: actual_qty * 10^8
OrderId = int
Timestamp = int  # Nanoseconds

# Constants
PRICE_DECIMALS = 8
QTY_DECIMALS = 8
PRICE_MULTIPLIER = 10**PRICE_DECIMALS
QTY_MULTIPLIER = 10**QTY_DECIMALS


class ExchangeId(Enum):
    """Supported exchanges."""
    BINANCE = auto()
    BYBIT = auto()
    OKX = auto()
    KRAKEN = auto()

    def __str__(self) -> str:
        return self.name.lower()


@dataclass(frozen=True)
class Symbol:
    """Trading symbol."""
    base: str
    quote: str

    def __str__(self) -> str:
        return f"{self.base}{self.quote}"

    @classmethod
    def from_str(cls, s: str) -> "Symbol":
        """Parse symbol from string like 'BTCUSDT'."""
        # Common quote currencies
        for quote in ["USDT", "USDC", "USD", "BTC", "ETH"]:
            if s.endswith(quote):
                return cls(s[:-len(quote)], quote)
        raise ValueError(f"Cannot parse symbol: {s}")


class Side(Enum):
    """Order side."""
    BUY = 1
    SELL = 2


class OrderStatus(Enum):
    """Order status."""
    PENDING = auto()
    NEW = auto()
    PARTIALLY_FILLED = auto()
    FILLED = auto()
    CANCELED = auto()
    REJECTED = auto()
    EXPIRED = auto()


class OrderType(Enum):
    """Order type."""
    LIMIT = auto()
    MARKET = auto()
    LIMIT_MAKER = auto()


class TimeInForce(Enum):
    """Time in force."""
    GTC = auto()  # Good Till Cancel
    IOC = auto()  # Immediate Or Cancel
    FOK = auto()  # Fill Or Kill
    GTX = auto()  # Good Till Crossing (Post Only)


@dataclass
class Order:
    """Order representation."""
    id: OrderId
    exchange_id: OrderId
    exchange: ExchangeId
    symbol: Symbol
    side: Side
    order_type: OrderType
    price: Price
    quantity: Quantity
    filled_qty: Quantity = 0
    status: OrderStatus = OrderStatus.PENDING
    tif: TimeInForce = TimeInForce.GTC
    create_time: Timestamp = 0
    update_time: Timestamp = 0


@dataclass
class Quote:
    """Quote (bid/ask pair)."""
    bid_price: Price
    bid_qty: Quantity
    ask_price: Price
    ask_qty: Quantity
    exchange: ExchangeId
    timestamp: Timestamp = 0


@dataclass
class Trade:
    """Trade execution."""
    id: int
    order_id: OrderId
    exchange: ExchangeId
    symbol: Symbol
    side: Side
    price: Price
    quantity: Quantity
    fee: Price = 0
    fee_asset: str = ""
    timestamp: Timestamp = 0


@dataclass
class Tick:
    """Market data tick."""
    symbol: Symbol
    bid: Price
    bid_qty: Quantity
    ask: Price
    ask_qty: Quantity
    last_price: Price = 0
    last_qty: Quantity = 0
    timestamp: Timestamp = 0


@dataclass
class ExchangeTick:
    """Market data tick with exchange info."""
    exchange: ExchangeId
    tick: Tick
    latency_ns: int = 0


@dataclass
class ArbitrageOpportunity:
    """Cross-exchange arbitrage opportunity."""
    symbol: Symbol
    buy_exchange: ExchangeId
    sell_exchange: ExchangeId
    buy_price: Price
    sell_price: Price
    quantity: Quantity
    expected_profit_bps: float
    timestamp: Timestamp = 0

    @property
    def spread_bps(self) -> float:
        """Calculate spread in basis points."""
        if self.buy_price == 0:
            return 0.0
        return (self.sell_price - self.buy_price) / self.buy_price * 10000


@dataclass
class NBBO:
    """National Best Bid and Offer across exchanges."""
    symbol: Symbol
    best_bid: Price = 0
    best_bid_qty: Quantity = 0
    best_bid_exchange: Optional[ExchangeId] = None
    best_ask: Price = 0
    best_ask_qty: Quantity = 0
    best_ask_exchange: Optional[ExchangeId] = None
    timestamp: Timestamp = 0

    @property
    def mid_price(self) -> Optional[Price]:
        """Calculate mid price."""
        if self.best_bid > 0 and self.best_ask > 0:
            return (self.best_bid + self.best_ask) // 2
        return None

    @property
    def spread_bps(self) -> Optional[float]:
        """Calculate spread in basis points."""
        mid = self.mid_price
        if mid and mid > 0:
            return (self.best_ask - self.best_bid) / mid * 10000
        return None

    @property
    def is_crossed(self) -> bool:
        """Check if market is crossed (arb opportunity)."""
        return self.best_bid > 0 and self.best_ask > 0 and self.best_bid >= self.best_ask


# Conversion functions
def to_price(value: float) -> Price:
    """Convert float to fixed-point price."""
    return int(value * PRICE_MULTIPLIER)


def to_qty(value: float) -> Quantity:
    """Convert float to fixed-point quantity."""
    return int(value * QTY_MULTIPLIER)


def from_price(value: Price) -> float:
    """Convert fixed-point price to float."""
    return value / PRICE_MULTIPLIER


def from_qty(value: Quantity) -> float:
    """Convert fixed-point quantity to float."""
    return value / QTY_MULTIPLIER


def now_ns() -> Timestamp:
    """Get current timestamp in nanoseconds."""
    return time.time_ns()
