"""
Binance exchange client implementation.
"""

import asyncio
import hashlib
import hmac
import time
from dataclasses import dataclass, field
from typing import Callable, Optional, Any
import aiohttp
import websockets
import orjson

from ..core import (
    Price, Quantity, OrderId, Timestamp, Side, Order, OrderType, OrderStatus,
    TimeInForce, Symbol, Tick, Trade, to_price, to_qty, from_price, from_qty, now_ns
)


@dataclass
class BinanceConfig:
    api_key: str = ""
    api_secret: str = ""
    testnet: bool = True
    recv_window: int = 5000

    @property
    def rest_url(self) -> str:
        if self.testnet:
            return "https://testnet.binance.vision"
        return "https://api.binance.com"

    @property
    def ws_url(self) -> str:
        if self.testnet:
            return "wss://testnet.binance.vision/ws"
        return "wss://stream.binance.com:9443/ws"


@dataclass
class ExchangeCallbacks:
    on_tick: Optional[Callable[[Tick], None]] = None
    on_order_update: Optional[Callable[[Order], None]] = None
    on_trade: Optional[Callable[[Trade], None]] = None
    on_error: Optional[Callable[[str], None]] = None
    on_connected: Optional[Callable[[], None]] = None
    on_disconnected: Optional[Callable[[], None]] = None


class BinanceClient:
    """Binance exchange client."""

    def __init__(self, config: BinanceConfig):
        self.config = config
        self._http_session: Optional[aiohttp.ClientSession] = None
        self._ws: Optional[websockets.WebSocketClientProtocol] = None
        self._connected = False
        self._callbacks = ExchangeCallbacks()
        self._running = False

    async def connect(self) -> None:
        """Connect to exchange."""
        self._http_session = aiohttp.ClientSession()
        self._ws = await websockets.connect(self.config.ws_url)
        self._connected = True
        self._running = True

        if self._callbacks.on_connected:
            self._callbacks.on_connected()

        # Start message handler
        asyncio.create_task(self._message_handler())

    async def disconnect(self) -> None:
        """Disconnect from exchange."""
        self._running = False
        self._connected = False

        if self._ws:
            await self._ws.close()
            self._ws = None

        if self._http_session:
            await self._http_session.close()
            self._http_session = None

        if self._callbacks.on_disconnected:
            self._callbacks.on_disconnected()

    @property
    def is_connected(self) -> bool:
        return self._connected

    def set_callbacks(self, callbacks: ExchangeCallbacks) -> None:
        """Set event callbacks."""
        self._callbacks = callbacks

    async def subscribe_ticker(self, symbol: Symbol) -> None:
        """Subscribe to ticker updates."""
        if not self._ws:
            return

        stream = f"{symbol.value.lower()}@bookTicker"
        msg = {
            "method": "SUBSCRIBE",
            "params": [stream],
            "id": 1
        }
        await self._ws.send(orjson.dumps(msg).decode())

    async def subscribe_orderbook(self, symbol: Symbol, depth: int = 20) -> None:
        """Subscribe to order book updates."""
        if not self._ws:
            return

        stream = f"{symbol.value.lower()}@depth{depth}@100ms"
        msg = {
            "method": "SUBSCRIBE",
            "params": [stream],
            "id": 2
        }
        await self._ws.send(orjson.dumps(msg).decode())

    async def subscribe_trades(self, symbol: Symbol) -> None:
        """Subscribe to trade updates."""
        if not self._ws:
            return

        stream = f"{symbol.value.lower()}@trade"
        msg = {
            "method": "SUBSCRIBE",
            "params": [stream],
            "id": 3
        }
        await self._ws.send(orjson.dumps(msg).decode())

    async def send_order(
        self,
        symbol: Symbol,
        side: Side,
        order_type: OrderType,
        price: Price,
        quantity: Quantity,
        time_in_force: TimeInForce = TimeInForce.GTC
    ) -> Optional[OrderId]:
        """Send order to exchange."""
        if not self._http_session:
            return None

        params = {
            "symbol": symbol.value,
            "side": "BUY" if side == Side.BUY else "SELL",
            "type": self._order_type_str(order_type),
            "timeInForce": self._tif_str(time_in_force),
            "price": f"{from_price(price):.8f}",
            "quantity": f"{from_qty(quantity):.8f}",
            "timestamp": str(int(time.time() * 1000)),
            "recvWindow": str(self.config.recv_window),
        }

        signature = self._sign(params)
        params["signature"] = signature

        url = f"{self.config.rest_url}/api/v3/order"
        headers = {"X-MBX-APIKEY": self.config.api_key}

        async with self._http_session.post(url, params=params, headers=headers) as resp:
            if resp.status == 200:
                data = await resp.json()
                return int(data.get("orderId", 0))
            else:
                error = await resp.text()
                if self._callbacks.on_error:
                    self._callbacks.on_error(f"Order failed: {error}")
                return None

    async def cancel_order(self, symbol: Symbol, order_id: OrderId) -> bool:
        """Cancel order."""
        if not self._http_session:
            return False

        params = {
            "symbol": symbol.value,
            "orderId": str(order_id),
            "timestamp": str(int(time.time() * 1000)),
        }

        signature = self._sign(params)
        params["signature"] = signature

        url = f"{self.config.rest_url}/api/v3/order"
        headers = {"X-MBX-APIKEY": self.config.api_key}

        async with self._http_session.delete(url, params=params, headers=headers) as resp:
            return resp.status == 200

    async def cancel_all_orders(self, symbol: Symbol) -> bool:
        """Cancel all orders for symbol."""
        if not self._http_session:
            return False

        params = {
            "symbol": symbol.value,
            "timestamp": str(int(time.time() * 1000)),
        }

        signature = self._sign(params)
        params["signature"] = signature

        url = f"{self.config.rest_url}/api/v3/openOrders"
        headers = {"X-MBX-APIKEY": self.config.api_key}

        async with self._http_session.delete(url, params=params, headers=headers) as resp:
            return resp.status == 200

    async def get_balance(self, asset: str) -> float:
        """Get balance for asset."""
        if not self._http_session:
            return 0.0

        params = {
            "timestamp": str(int(time.time() * 1000)),
        }

        signature = self._sign(params)
        params["signature"] = signature

        url = f"{self.config.rest_url}/api/v3/account"
        headers = {"X-MBX-APIKEY": self.config.api_key}

        async with self._http_session.get(url, params=params, headers=headers) as resp:
            if resp.status == 200:
                data = await resp.json()
                for balance in data.get("balances", []):
                    if balance["asset"] == asset:
                        return float(balance["free"])
            return 0.0

    async def _message_handler(self) -> None:
        """Handle WebSocket messages."""
        while self._running and self._ws:
            try:
                msg = await asyncio.wait_for(self._ws.recv(), timeout=30)
                data = orjson.loads(msg)
                await self._process_message(data)
            except asyncio.TimeoutError:
                # Send ping to keep connection alive
                await self._ws.ping()
            except websockets.exceptions.ConnectionClosed:
                self._connected = False
                if self._callbacks.on_disconnected:
                    self._callbacks.on_disconnected()
                break
            except Exception as e:
                if self._callbacks.on_error:
                    self._callbacks.on_error(str(e))

    async def _process_message(self, data: dict) -> None:
        """Process WebSocket message."""
        event_type = data.get("e", "")

        if event_type == "bookTicker":
            tick = Tick(
                bid=to_price(float(data.get("b", 0))),
                ask=to_price(float(data.get("a", 0))),
                bid_qty=to_qty(float(data.get("B", 0))),
                ask_qty=to_qty(float(data.get("A", 0))),
                exchange_ts=int(data.get("E", 0)) * 1_000_000,
                local_ts=now_ns(),
            )
            if self._callbacks.on_tick:
                self._callbacks.on_tick(tick)

        elif event_type == "executionReport":
            order = self._parse_order_update(data)
            if order and self._callbacks.on_order_update:
                self._callbacks.on_order_update(order)

    def _parse_order_update(self, data: dict) -> Optional[Order]:
        """Parse execution report to Order."""
        status_map = {
            "NEW": OrderStatus.NEW,
            "PARTIALLY_FILLED": OrderStatus.PARTIALLY_FILLED,
            "FILLED": OrderStatus.FILLED,
            "CANCELED": OrderStatus.CANCELED,
            "REJECTED": OrderStatus.REJECTED,
            "EXPIRED": OrderStatus.EXPIRED,
        }

        status = status_map.get(data.get("X", ""))
        if status is None:
            return None

        return Order(
            id=int(data.get("i", 0)),
            client_id=int(data.get("c", "0") or "0"),
            symbol=Symbol(data.get("s", "")),
            side=Side.BUY if data.get("S") == "BUY" else Side.SELL,
            price=to_price(float(data.get("p", 0))),
            quantity=to_qty(float(data.get("q", 0))),
            filled_qty=to_qty(float(data.get("z", 0))),
            status=status,
            timestamp=int(data.get("T", 0)) * 1_000_000,
        )

    def _sign(self, params: dict) -> str:
        """Sign request parameters."""
        query_string = "&".join(f"{k}={v}" for k, v in params.items())
        return hmac.new(
            self.config.api_secret.encode(),
            query_string.encode(),
            hashlib.sha256
        ).hexdigest()

    def _order_type_str(self, order_type: OrderType) -> str:
        mapping = {
            OrderType.LIMIT: "LIMIT",
            OrderType.MARKET: "MARKET",
            OrderType.LIMIT_MAKER: "LIMIT_MAKER",
        }
        return mapping.get(order_type, "LIMIT")

    def _tif_str(self, tif: TimeInForce) -> str:
        mapping = {
            TimeInForce.GTC: "GTC",
            TimeInForce.IOC: "IOC",
            TimeInForce.FOK: "FOK",
            TimeInForce.GTX: "GTX",
        }
        return mapping.get(tif, "GTC")
