"""Exchange manager for multi-exchange coordination."""

import asyncio
from dataclasses import dataclass, field
from typing import Dict, Optional, List, Callable

from ..core import ExchangeId, Symbol, Order, Tick, Trade
from .base import ExchangeClient, ExchangeCallbacks, OrderRequest, OrderResponse


@dataclass
class ExchangeHealth:
    """Exchange health status."""
    exchange: ExchangeId
    is_connected: bool = False
    latency_ns: int = 0
    last_tick_time: int = 0
    error_count: int = 0
    is_healthy: bool = True


class ExchangeManager:
    """Manages multiple exchange connections."""

    def __init__(self):
        self._clients: Dict[ExchangeId, ExchangeClient] = {}
        self._health: Dict[ExchangeId, ExchangeHealth] = {}
        self._callbacks: Optional[ExchangeCallbacks] = None
        self._on_tick_aggregated: Optional[Callable[[ExchangeId, Tick], None]] = None

    def register_exchange(self, client: ExchangeClient) -> None:
        """Register an exchange client."""
        exchange_id = client.exchange_id
        self._clients[exchange_id] = client
        self._health[exchange_id] = ExchangeHealth(exchange=exchange_id)

        # Set up callbacks to route through manager
        client.set_callbacks(ExchangeCallbacks(
            on_tick=lambda eid, tick: self._on_tick(eid, tick),
            on_order_update=lambda eid, order: self._on_order_update(eid, order),
            on_trade=lambda eid, trade: self._on_trade(eid, trade),
            on_error=lambda eid, error: self._on_error(eid, error),
            on_connected=lambda eid: self._on_connected(eid),
            on_disconnected=lambda eid: self._on_disconnected(eid),
        ))

    def set_callbacks(self, callbacks: ExchangeCallbacks) -> None:
        """Set callbacks for exchange events."""
        self._callbacks = callbacks

    def get_client(self, exchange: ExchangeId) -> Optional[ExchangeClient]:
        """Get exchange client by ID."""
        return self._clients.get(exchange)

    def get_all_exchanges(self) -> List[ExchangeId]:
        """Get all registered exchange IDs."""
        return list(self._clients.keys())

    def get_connected_exchanges(self) -> List[ExchangeId]:
        """Get all connected exchange IDs."""
        return [eid for eid, health in self._health.items() if health.is_connected]

    def get_health(self, exchange: ExchangeId) -> Optional[ExchangeHealth]:
        """Get exchange health status."""
        return self._health.get(exchange)

    def get_fastest_exchange(self) -> Optional[ExchangeId]:
        """Get the exchange with lowest latency."""
        connected = [(eid, h) for eid, h in self._health.items()
                     if h.is_connected and h.is_healthy]
        if not connected:
            return None
        return min(connected, key=lambda x: x[1].latency_ns)[0]

    async def connect_all(self) -> None:
        """Connect to all registered exchanges."""
        tasks = [client.connect() for client in self._clients.values()]
        await asyncio.gather(*tasks, return_exceptions=True)

    async def disconnect_all(self) -> None:
        """Disconnect from all exchanges."""
        tasks = [client.disconnect() for client in self._clients.values()]
        await asyncio.gather(*tasks, return_exceptions=True)

    async def subscribe_ticker_all(self, symbol: Symbol) -> None:
        """Subscribe to ticker on all exchanges."""
        tasks = [client.subscribe_ticker(symbol)
                 for client in self._clients.values()
                 if client.is_connected]
        await asyncio.gather(*tasks, return_exceptions=True)

    async def subscribe_orderbook_all(self, symbol: Symbol, depth: int = 20) -> None:
        """Subscribe to order book on all exchanges."""
        tasks = [client.subscribe_orderbook(symbol, depth)
                 for client in self._clients.values()
                 if client.is_connected]
        await asyncio.gather(*tasks, return_exceptions=True)

    async def send_order(self, exchange: ExchangeId, request: OrderRequest) -> OrderResponse:
        """Send order to specific exchange."""
        client = self._clients.get(exchange)
        if not client:
            return OrderResponse(success=False, error_message=f"Exchange {exchange} not registered")
        if not client.is_connected:
            return OrderResponse(success=False, error_message=f"Exchange {exchange} not connected")
        return await client.send_order(request)

    async def cancel_order(self, exchange: ExchangeId, symbol: Symbol, order_id: int) -> bool:
        """Cancel order on specific exchange."""
        client = self._clients.get(exchange)
        if not client or not client.is_connected:
            return False
        return await client.cancel_order(symbol, order_id)

    async def cancel_all_orders_all_exchanges(self, symbol: Symbol) -> Dict[ExchangeId, int]:
        """Cancel all orders on all exchanges."""
        results = {}
        for exchange_id, client in self._clients.items():
            if client.is_connected:
                count = await client.cancel_all_orders(symbol)
                results[exchange_id] = count
        return results

    def _on_tick(self, exchange: ExchangeId, tick: Tick) -> None:
        """Handle tick from exchange."""
        if exchange in self._health:
            from ..core import now_ns
            self._health[exchange].last_tick_time = now_ns()

        if self._callbacks and self._callbacks.on_tick:
            self._callbacks.on_tick(exchange, tick)

    def _on_order_update(self, exchange: ExchangeId, order: Order) -> None:
        """Handle order update from exchange."""
        if self._callbacks and self._callbacks.on_order_update:
            self._callbacks.on_order_update(exchange, order)

    def _on_trade(self, exchange: ExchangeId, trade: Trade) -> None:
        """Handle trade from exchange."""
        if self._callbacks and self._callbacks.on_trade:
            self._callbacks.on_trade(exchange, trade)

    def _on_error(self, exchange: ExchangeId, error: str) -> None:
        """Handle error from exchange."""
        if exchange in self._health:
            self._health[exchange].error_count += 1
            if self._health[exchange].error_count > 10:
                self._health[exchange].is_healthy = False

        if self._callbacks and self._callbacks.on_error:
            self._callbacks.on_error(exchange, error)

    def _on_connected(self, exchange: ExchangeId) -> None:
        """Handle connection from exchange."""
        if exchange in self._health:
            self._health[exchange].is_connected = True
            client = self._clients.get(exchange)
            if client:
                self._health[exchange].latency_ns = client.get_latency_ns()

        if self._callbacks and self._callbacks.on_connected:
            self._callbacks.on_connected(exchange)

    def _on_disconnected(self, exchange: ExchangeId) -> None:
        """Handle disconnection from exchange."""
        if exchange in self._health:
            self._health[exchange].is_connected = False

        if self._callbacks and self._callbacks.on_disconnected:
            self._callbacks.on_disconnected(exchange)
