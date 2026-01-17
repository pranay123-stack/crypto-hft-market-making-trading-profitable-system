"""Base exchange client interface."""

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Callable, Optional, List
from ..core import (
    ExchangeId, Symbol, Order, Tick, Trade,
    Price, Quantity, OrderId, Side, OrderType, TimeInForce
)


@dataclass
class ExchangeConfig:
    """Base exchange configuration."""
    api_key: str = ""
    api_secret: str = ""
    testnet: bool = True
    rate_limit_per_second: int = 10


@dataclass
class OrderRequest:
    """Order request."""
    symbol: Symbol
    side: Side
    order_type: OrderType
    price: Price
    quantity: Quantity
    tif: TimeInForce = TimeInForce.GTC
    client_order_id: Optional[OrderId] = None


@dataclass
class OrderResponse:
    """Order response."""
    success: bool
    exchange_order_id: OrderId = 0
    client_order_id: OrderId = 0
    error_message: str = ""


@dataclass
class ExchangeCallbacks:
    """Callbacks for exchange events."""
    on_tick: Optional[Callable[[ExchangeId, Tick], None]] = None
    on_order_update: Optional[Callable[[ExchangeId, Order], None]] = None
    on_trade: Optional[Callable[[ExchangeId, Trade], None]] = None
    on_error: Optional[Callable[[ExchangeId, str], None]] = None
    on_connected: Optional[Callable[[ExchangeId], None]] = None
    on_disconnected: Optional[Callable[[ExchangeId], None]] = None


class ExchangeClient(ABC):
    """Abstract base class for exchange clients."""

    @property
    @abstractmethod
    def exchange_id(self) -> ExchangeId:
        """Get exchange identifier."""
        pass

    @property
    @abstractmethod
    def is_connected(self) -> bool:
        """Check if connected."""
        pass

    @abstractmethod
    async def connect(self) -> None:
        """Connect to exchange."""
        pass

    @abstractmethod
    async def disconnect(self) -> None:
        """Disconnect from exchange."""
        pass

    @abstractmethod
    async def subscribe_ticker(self, symbol: Symbol) -> None:
        """Subscribe to ticker updates."""
        pass

    @abstractmethod
    async def subscribe_orderbook(self, symbol: Symbol, depth: int = 20) -> None:
        """Subscribe to order book updates."""
        pass

    @abstractmethod
    async def send_order(self, request: OrderRequest) -> OrderResponse:
        """Send order to exchange."""
        pass

    @abstractmethod
    async def cancel_order(self, symbol: Symbol, order_id: OrderId) -> bool:
        """Cancel order."""
        pass

    @abstractmethod
    async def cancel_all_orders(self, symbol: Symbol) -> int:
        """Cancel all orders for symbol. Returns count cancelled."""
        pass

    @abstractmethod
    async def get_open_orders(self, symbol: Symbol) -> List[Order]:
        """Get open orders."""
        pass

    @abstractmethod
    def set_callbacks(self, callbacks: ExchangeCallbacks) -> None:
        """Set event callbacks."""
        pass

    @abstractmethod
    def get_latency_ns(self) -> int:
        """Get estimated latency in nanoseconds."""
        pass
