"""OKX exchange client."""

import asyncio
import base64
import hashlib
import hmac
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional, List, Dict, Any

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
class OKXConfig(ExchangeConfig):
    """OKX-specific configuration."""
    passphrase: str = ""
    rest_url: str = field(default="")
    ws_url: str = field(default="")

    def __post_init__(self):
        if self.testnet:
            self.rest_url = self.rest_url or "https://www.okx.com"  # OKX uses same URL with demo flag
            self.ws_url = self.ws_url or "wss://wspap.okx.com:8443/ws/v5/public?brokerId=9999"
        else:
            self.rest_url = self.rest_url or "https://www.okx.com"
            self.ws_url = self.ws_url or "wss://ws.okx.com:8443/ws/v5/public"


class OKXClient(ExchangeClient):
    """OKX exchange client implementation."""

    def __init__(self, config: OKXConfig):
        self._config = config
        self._callbacks: Optional[ExchangeCallbacks] = None
        self._session: Optional[aiohttp.ClientSession] = None
        self._ws: Optional[WebSocketClientProtocol] = None
        self._connected = False
        self._running = False
        self._subscriptions: List[Dict[str, str]] = []
        self._latency_ns = 0
        self._ws_task: Optional[asyncio.Task] = None

    @property
    def exchange_id(self) -> ExchangeId:
        return ExchangeId.OKX

    @property
    def is_connected(self) -> bool:
        return self._connected

    def set_callbacks(self, callbacks: ExchangeCallbacks) -> None:
        self._callbacks = callbacks

    def get_latency_ns(self) -> int:
        return self._latency_ns

    async def connect(self) -> None:
        """Connect to OKX."""
        self._session = aiohttp.ClientSession()

        # Test REST connectivity and measure latency
        start = now_ns()
        async with self._session.get(f"{self._config.rest_url}/api/v5/public/time") as resp:
            if resp.status != 200:
                raise ConnectionError("Failed to connect to OKX REST API")
            await resp.json()
        self._latency_ns = now_ns() - start

        self._connected = True
        self._running = True

        if self._callbacks and self._callbacks.on_connected:
            self._callbacks.on_connected(self.exchange_id)

    async def disconnect(self) -> None:
        """Disconnect from OKX."""
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
        inst_id = f"{symbol.base}-{symbol.quote}"
        channel = {"channel": "tickers", "instId": inst_id}
        await self._subscribe(channel)

    async def subscribe_orderbook(self, symbol: Symbol, depth: int = 20) -> None:
        """Subscribe to order book stream."""
        inst_id = f"{symbol.base}-{symbol.quote}"
        channel = {"channel": "books5", "instId": inst_id}  # Top 5 levels
        await self._subscribe(channel)

    async def _subscribe(self, channel: Dict[str, str]) -> None:
        """Subscribe to a channel."""
        self._subscriptions.append(channel)

        if not self._ws:
            self._ws = await websockets.connect(self._config.ws_url)
            self._ws_task = asyncio.create_task(self._ws_handler())

        msg = {
            "op": "subscribe",
            "args": [channel]
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
                    await self._ws.send("ping")
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
        if "data" not in data:
            return

        arg = data.get("arg", {})
        channel = arg.get("channel", "")

        for item in data.get("data", []):
            if channel == "tickers":
                inst_id = item.get("instId", "")
                parts = inst_id.split("-")
                if len(parts) >= 2:
                    tick = Tick(
                        symbol=Symbol(parts[0], parts[1]),
                        bid=to_price(float(item.get("bidPx", 0))),
                        bid_qty=to_qty(float(item.get("bidSz", 0))),
                        ask=to_price(float(item.get("askPx", 0))),
                        ask_qty=to_qty(float(item.get("askSz", 0))),
                        last_price=to_price(float(item.get("last", 0))),
                        last_qty=to_qty(float(item.get("lastSz", 0))),
                        timestamp=now_ns(),
                    )
                    if self._callbacks and self._callbacks.on_tick:
                        self._callbacks.on_tick(self.exchange_id, tick)

            elif channel == "books5":
                inst_id = arg.get("instId", "")
                parts = inst_id.split("-")
                bids = item.get("bids", [])
                asks = item.get("asks", [])
                if len(parts) >= 2 and bids and asks:
                    tick = Tick(
                        symbol=Symbol(parts[0], parts[1]),
                        bid=to_price(float(bids[0][0])) if bids else 0,
                        bid_qty=to_qty(float(bids[0][1])) if bids else 0,
                        ask=to_price(float(asks[0][0])) if asks else 0,
                        ask_qty=to_qty(float(asks[0][1])) if asks else 0,
                        timestamp=now_ns(),
                    )
                    if self._callbacks and self._callbacks.on_tick:
                        self._callbacks.on_tick(self.exchange_id, tick)

    def _sign_request(self, timestamp: str, method: str, path: str, body: str = "") -> str:
        """Sign request with HMAC-SHA256."""
        message = f"{timestamp}{method}{path}{body}"
        signature = hmac.new(
            self._config.api_secret.encode(),
            message.encode(),
            hashlib.sha256
        ).digest()
        return base64.b64encode(signature).decode()

    def _get_headers(self, method: str, path: str, body: str = "") -> Dict[str, str]:
        """Get request headers."""
        timestamp = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"
        sign = self._sign_request(timestamp, method, path, body)

        headers = {
            "OK-ACCESS-KEY": self._config.api_key,
            "OK-ACCESS-SIGN": sign,
            "OK-ACCESS-TIMESTAMP": timestamp,
            "OK-ACCESS-PASSPHRASE": self._config.passphrase,
            "Content-Type": "application/json"
        }

        if self._config.testnet:
            headers["x-simulated-trading"] = "1"

        return headers

    async def send_order(self, request: OrderRequest) -> OrderResponse:
        """Send order to OKX."""
        if not self._session:
            return OrderResponse(success=False, error_message="Not connected")

        inst_id = f"{request.symbol.base}-{request.symbol.quote}"
        path = "/api/v5/trade/order"
        params = {
            "instId": inst_id,
            "tdMode": "cash",
            "side": "buy" if request.side == Side.BUY else "sell",
            "ordType": self._order_type_str(request.order_type),
            "sz": str(request.quantity / 10**8),
        }

        if request.order_type != OrderType.MARKET:
            params["px"] = str(request.price / 10**8)

        body = orjson.dumps(params).decode()

        try:
            async with self._session.post(
                f"{self._config.rest_url}{path}",
                headers=self._get_headers("POST", path, body),
                data=body
            ) as resp:
                data = await resp.json()

                if data.get("code") == "0":
                    result = data.get("data", [{}])[0]
                    return OrderResponse(
                        success=True,
                        exchange_order_id=int(result.get("ordId", 0)),
                    )
                else:
                    return OrderResponse(
                        success=False,
                        error_message=data.get("msg", "Unknown error"),
                    )
        except Exception as e:
            return OrderResponse(success=False, error_message=str(e))

    async def cancel_order(self, symbol: Symbol, order_id: int) -> bool:
        """Cancel order on OKX."""
        if not self._session:
            return False

        inst_id = f"{symbol.base}-{symbol.quote}"
        path = "/api/v5/trade/cancel-order"
        params = {
            "instId": inst_id,
            "ordId": str(order_id),
        }

        body = orjson.dumps(params).decode()

        try:
            async with self._session.post(
                f"{self._config.rest_url}{path}",
                headers=self._get_headers("POST", path, body),
                data=body
            ) as resp:
                data = await resp.json()
                return data.get("code") == "0"
        except Exception:
            return False

    async def cancel_all_orders(self, symbol: Symbol) -> int:
        """Cancel all orders for symbol."""
        # OKX requires cancelling orders individually or by batch
        orders = await self.get_open_orders(symbol)
        cancelled = 0
        for order in orders:
            if await self.cancel_order(symbol, order.exchange_id):
                cancelled += 1
        return cancelled

    async def get_open_orders(self, symbol: Symbol) -> List[Order]:
        """Get open orders."""
        if not self._session:
            return []

        inst_id = f"{symbol.base}-{symbol.quote}"
        path = f"/api/v5/trade/orders-pending?instId={inst_id}"

        try:
            async with self._session.get(
                f"{self._config.rest_url}{path}",
                headers=self._get_headers("GET", path)
            ) as resp:
                data = await resp.json()
                if data.get("code") == "0":
                    return [self._parse_order(o) for o in data.get("data", [])]
                return []
        except Exception:
            return []

    def _parse_order(self, data: Dict[str, Any]) -> Order:
        """Parse order from API response."""
        inst_id = data.get("instId", "")
        parts = inst_id.split("-")
        symbol = Symbol(parts[0], parts[1]) if len(parts) >= 2 else Symbol("", "")

        return Order(
            id=0,
            exchange_id=int(data.get("ordId", 0)),
            exchange=ExchangeId.OKX,
            symbol=symbol,
            side=Side.BUY if data.get("side") == "buy" else Side.SELL,
            order_type=self._parse_order_type(data.get("ordType", "")),
            price=to_price(float(data.get("px", 0) or 0)),
            quantity=to_qty(float(data.get("sz", 0))),
            filled_qty=to_qty(float(data.get("fillSz", 0) or 0)),
            status=self._parse_status(data.get("state", "")),
        )

    @staticmethod
    def _order_type_str(order_type: OrderType) -> str:
        return {
            OrderType.LIMIT: "limit",
            OrderType.MARKET: "market",
            OrderType.LIMIT_MAKER: "post_only",
        }.get(order_type, "limit")

    @staticmethod
    def _parse_order_type(s: str) -> OrderType:
        return {
            "limit": OrderType.LIMIT,
            "market": OrderType.MARKET,
            "post_only": OrderType.LIMIT_MAKER,
        }.get(s, OrderType.LIMIT)

    @staticmethod
    def _parse_status(s: str) -> OrderStatus:
        return {
            "live": OrderStatus.NEW,
            "partially_filled": OrderStatus.PARTIALLY_FILLED,
            "filled": OrderStatus.FILLED,
            "canceled": OrderStatus.CANCELED,
        }.get(s, OrderStatus.PENDING)
