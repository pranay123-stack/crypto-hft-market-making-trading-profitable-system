"""Binance exchange client."""

import asyncio
import hashlib
import hmac
import time
from dataclasses import dataclass, field
from typing import Optional, List, Dict, Any
from urllib.parse import urlencode

import aiohttp
import orjson
import websockets
from websockets.client import WebSocketClientProtocol

from ..core import (
    ExchangeId, Symbol, Order, Tick, Trade, Side, OrderStatus,
    OrderType, TimeInForce, to_price, to_qty, now_ns
)
from .base import ExchangeClient, ExchangeConfig, ExchangeCallbacks, OrderRequest, OrderResponse


@dataclass
class BinanceConfig(ExchangeConfig):
    """Binance-specific configuration."""
    rest_url: str = field(default="")
    ws_url: str = field(default="")

    def __post_init__(self):
        if self.testnet:
            self.rest_url = self.rest_url or "https://testnet.binance.vision/api/v3"
            self.ws_url = self.ws_url or "wss://testnet.binance.vision/ws"
        else:
            self.rest_url = self.rest_url or "https://api.binance.com/api/v3"
            self.ws_url = self.ws_url or "wss://stream.binance.com:9443/ws"


class BinanceClient(ExchangeClient):
    """Binance exchange client implementation."""

    def __init__(self, config: BinanceConfig):
        self._config = config
        self._callbacks: Optional[ExchangeCallbacks] = None
        self._session: Optional[aiohttp.ClientSession] = None
        self._ws: Optional[WebSocketClientProtocol] = None
        self._connected = False
        self._running = False
        self._subscriptions: Dict[str, Symbol] = {}
        self._latency_ns = 0
        self._ws_task: Optional[asyncio.Task] = None

    @property
    def exchange_id(self) -> ExchangeId:
        return ExchangeId.BINANCE

    @property
    def is_connected(self) -> bool:
        return self._connected

    def set_callbacks(self, callbacks: ExchangeCallbacks) -> None:
        self._callbacks = callbacks

    def get_latency_ns(self) -> int:
        return self._latency_ns

    async def connect(self) -> None:
        """Connect to Binance."""
        self._session = aiohttp.ClientSession()

        # Test REST connectivity
        async with self._session.get(f"{self._config.rest_url}/ping") as resp:
            if resp.status != 200:
                raise ConnectionError("Failed to connect to Binance REST API")

        # Measure latency
        start = now_ns()
        async with self._session.get(f"{self._config.rest_url}/time") as resp:
            await resp.json()
        self._latency_ns = now_ns() - start

        self._connected = True
        self._running = True

        if self._callbacks and self._callbacks.on_connected:
            self._callbacks.on_connected(self.exchange_id)

    async def disconnect(self) -> None:
        """Disconnect from Binance."""
        self._running = False
        self._connected = False

        if self._ws_task:
            self._ws_task.cancel()
            try:
                await self._ws_task
            except asyncio.CancelledError:
                pass

        if self._ws:
            await self._ws.close()
            self._ws = None

        if self._session:
            await self._session.close()
            self._session = None

        if self._callbacks and self._callbacks.on_disconnected:
            self._callbacks.on_disconnected(self.exchange_id)

    async def subscribe_ticker(self, symbol: Symbol) -> None:
        """Subscribe to ticker stream."""
        stream = f"{str(symbol).lower()}@ticker"
        await self._subscribe_stream(stream, symbol)

    async def subscribe_orderbook(self, symbol: Symbol, depth: int = 20) -> None:
        """Subscribe to order book stream."""
        stream = f"{str(symbol).lower()}@depth{depth}@100ms"
        await self._subscribe_stream(stream, symbol)

    async def _subscribe_stream(self, stream: str, symbol: Symbol) -> None:
        """Subscribe to a WebSocket stream."""
        self._subscriptions[stream] = symbol

        if not self._ws:
            self._ws = await websockets.connect(f"{self._config.ws_url}/{stream}")
            self._ws_task = asyncio.create_task(self._ws_handler())
        else:
            # Send subscription message
            msg = {
                "method": "SUBSCRIBE",
                "params": [stream],
                "id": int(time.time() * 1000)
            }
            await self._ws.send(orjson.dumps(msg).decode())

    async def _ws_handler(self) -> None:
        """Handle WebSocket messages."""
        while self._running and self._ws:
            try:
                msg = await asyncio.wait_for(self._ws.recv(), timeout=30)
                data = orjson.loads(msg)
                await self._process_ws_message(data)
            except asyncio.TimeoutError:
                # Send ping
                if self._ws:
                    pong = await self._ws.ping()
                    await pong
            except websockets.ConnectionClosed:
                self._connected = False
                if self._callbacks and self._callbacks.on_disconnected:
                    self._callbacks.on_disconnected(self.exchange_id)
                break
            except Exception as e:
                if self._callbacks and self._callbacks.on_error:
                    self._callbacks.on_error(self.exchange_id, str(e))

    async def _process_ws_message(self, data: Dict[str, Any]) -> None:
        """Process WebSocket message."""
        if "e" not in data:
            return

        event_type = data["e"]

        if event_type == "24hrTicker":
            tick = Tick(
                symbol=Symbol.from_str(data["s"]),
                bid=to_price(float(data["b"])),
                bid_qty=to_qty(float(data["B"])),
                ask=to_price(float(data["a"])),
                ask_qty=to_qty(float(data["A"])),
                last_price=to_price(float(data["c"])),
                last_qty=to_qty(float(data["Q"])),
                timestamp=now_ns(),
            )
            if self._callbacks and self._callbacks.on_tick:
                self._callbacks.on_tick(self.exchange_id, tick)

        elif event_type == "depthUpdate":
            # Process order book update
            if data["b"] and data["a"]:
                best_bid = data["b"][0] if data["b"] else ["0", "0"]
                best_ask = data["a"][0] if data["a"] else ["0", "0"]
                tick = Tick(
                    symbol=Symbol.from_str(data["s"]),
                    bid=to_price(float(best_bid[0])),
                    bid_qty=to_qty(float(best_bid[1])),
                    ask=to_price(float(best_ask[0])),
                    ask_qty=to_qty(float(best_ask[1])),
                    timestamp=now_ns(),
                )
                if self._callbacks and self._callbacks.on_tick:
                    self._callbacks.on_tick(self.exchange_id, tick)

    def _sign_request(self, params: Dict[str, Any]) -> str:
        """Sign request with HMAC-SHA256."""
        query_string = urlencode(params)
        signature = hmac.new(
            self._config.api_secret.encode(),
            query_string.encode(),
            hashlib.sha256
        ).hexdigest()
        return f"{query_string}&signature={signature}"

    def _get_headers(self) -> Dict[str, str]:
        """Get request headers."""
        return {"X-MBX-APIKEY": self._config.api_key}

    async def send_order(self, request: OrderRequest) -> OrderResponse:
        """Send order to Binance."""
        if not self._session:
            return OrderResponse(success=False, error_message="Not connected")

        params = {
            "symbol": str(request.symbol),
            "side": "BUY" if request.side == Side.BUY else "SELL",
            "type": self._order_type_str(request.order_type),
            "quantity": request.quantity / 10**8,
            "timestamp": int(time.time() * 1000),
        }

        if request.order_type != OrderType.MARKET:
            params["price"] = request.price / 10**8
            params["timeInForce"] = self._tif_str(request.tif)

        if request.client_order_id:
            params["newClientOrderId"] = str(request.client_order_id)

        try:
            url = f"{self._config.rest_url}/order?{self._sign_request(params)}"
            async with self._session.post(url, headers=self._get_headers()) as resp:
                data = await resp.json()

                if resp.status == 200:
                    return OrderResponse(
                        success=True,
                        exchange_order_id=data["orderId"],
                        client_order_id=request.client_order_id or 0,
                    )
                else:
                    return OrderResponse(
                        success=False,
                        error_message=data.get("msg", "Unknown error"),
                    )
        except Exception as e:
            return OrderResponse(success=False, error_message=str(e))

    async def cancel_order(self, symbol: Symbol, order_id: int) -> bool:
        """Cancel order on Binance."""
        if not self._session:
            return False

        params = {
            "symbol": str(symbol),
            "orderId": order_id,
            "timestamp": int(time.time() * 1000),
        }

        try:
            url = f"{self._config.rest_url}/order?{self._sign_request(params)}"
            async with self._session.delete(url, headers=self._get_headers()) as resp:
                return resp.status == 200
        except Exception:
            return False

    async def cancel_all_orders(self, symbol: Symbol) -> int:
        """Cancel all orders for symbol."""
        if not self._session:
            return 0

        params = {
            "symbol": str(symbol),
            "timestamp": int(time.time() * 1000),
        }

        try:
            url = f"{self._config.rest_url}/openOrders?{self._sign_request(params)}"
            async with self._session.delete(url, headers=self._get_headers()) as resp:
                if resp.status == 200:
                    data = await resp.json()
                    return len(data)
                return 0
        except Exception:
            return 0

    async def get_open_orders(self, symbol: Symbol) -> List[Order]:
        """Get open orders."""
        if not self._session:
            return []

        params = {
            "symbol": str(symbol),
            "timestamp": int(time.time() * 1000),
        }

        try:
            url = f"{self._config.rest_url}/openOrders?{self._sign_request(params)}"
            async with self._session.get(url, headers=self._get_headers()) as resp:
                if resp.status == 200:
                    data = await resp.json()
                    return [self._parse_order(o) for o in data]
                return []
        except Exception:
            return []

    def _parse_order(self, data: Dict[str, Any]) -> Order:
        """Parse order from API response."""
        return Order(
            id=data.get("clientOrderId", 0),
            exchange_id=data["orderId"],
            exchange=ExchangeId.BINANCE,
            symbol=Symbol.from_str(data["symbol"]),
            side=Side.BUY if data["side"] == "BUY" else Side.SELL,
            order_type=self._parse_order_type(data["type"]),
            price=to_price(float(data["price"])),
            quantity=to_qty(float(data["origQty"])),
            filled_qty=to_qty(float(data["executedQty"])),
            status=self._parse_status(data["status"]),
        )

    @staticmethod
    def _order_type_str(order_type: OrderType) -> str:
        return {
            OrderType.LIMIT: "LIMIT",
            OrderType.MARKET: "MARKET",
            OrderType.LIMIT_MAKER: "LIMIT_MAKER",
        }.get(order_type, "LIMIT")

    @staticmethod
    def _tif_str(tif: TimeInForce) -> str:
        return {
            TimeInForce.GTC: "GTC",
            TimeInForce.IOC: "IOC",
            TimeInForce.FOK: "FOK",
            TimeInForce.GTX: "GTX",
        }.get(tif, "GTC")

    @staticmethod
    def _parse_order_type(s: str) -> OrderType:
        return {
            "LIMIT": OrderType.LIMIT,
            "MARKET": OrderType.MARKET,
            "LIMIT_MAKER": OrderType.LIMIT_MAKER,
        }.get(s, OrderType.LIMIT)

    @staticmethod
    def _parse_status(s: str) -> OrderStatus:
        return {
            "NEW": OrderStatus.NEW,
            "PARTIALLY_FILLED": OrderStatus.PARTIALLY_FILLED,
            "FILLED": OrderStatus.FILLED,
            "CANCELED": OrderStatus.CANCELED,
            "REJECTED": OrderStatus.REJECTED,
            "EXPIRED": OrderStatus.EXPIRED,
        }.get(s, OrderStatus.PENDING)
