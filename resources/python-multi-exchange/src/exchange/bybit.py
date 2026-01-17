"""Bybit exchange client."""

import asyncio
import hashlib
import hmac
import time
from dataclasses import dataclass, field
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
class BybitConfig(ExchangeConfig):
    """Bybit-specific configuration."""
    rest_url: str = field(default="")
    ws_url: str = field(default="")

    def __post_init__(self):
        if self.testnet:
            self.rest_url = self.rest_url or "https://api-testnet.bybit.com"
            self.ws_url = self.ws_url or "wss://stream-testnet.bybit.com/v5/public/spot"
        else:
            self.rest_url = self.rest_url or "https://api.bybit.com"
            self.ws_url = self.ws_url or "wss://stream.bybit.com/v5/public/spot"


class BybitClient(ExchangeClient):
    """Bybit exchange client implementation."""

    def __init__(self, config: BybitConfig):
        self._config = config
        self._callbacks: Optional[ExchangeCallbacks] = None
        self._session: Optional[aiohttp.ClientSession] = None
        self._ws: Optional[WebSocketClientProtocol] = None
        self._connected = False
        self._running = False
        self._subscriptions: List[str] = []
        self._latency_ns = 0
        self._ws_task: Optional[asyncio.Task] = None

    @property
    def exchange_id(self) -> ExchangeId:
        return ExchangeId.BYBIT

    @property
    def is_connected(self) -> bool:
        return self._connected

    def set_callbacks(self, callbacks: ExchangeCallbacks) -> None:
        self._callbacks = callbacks

    def get_latency_ns(self) -> int:
        return self._latency_ns

    async def connect(self) -> None:
        """Connect to Bybit."""
        self._session = aiohttp.ClientSession()

        # Test REST connectivity and measure latency
        start = now_ns()
        async with self._session.get(f"{self._config.rest_url}/v5/market/time") as resp:
            if resp.status != 200:
                raise ConnectionError("Failed to connect to Bybit REST API")
            await resp.json()
        self._latency_ns = now_ns() - start

        self._connected = True
        self._running = True

        if self._callbacks and self._callbacks.on_connected:
            self._callbacks.on_connected(self.exchange_id)

    async def disconnect(self) -> None:
        """Disconnect from Bybit."""
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
        topic = f"tickers.{symbol}"
        await self._subscribe(topic)

    async def subscribe_orderbook(self, symbol: Symbol, depth: int = 20) -> None:
        """Subscribe to order book stream."""
        topic = f"orderbook.{depth}.{symbol}"
        await self._subscribe(topic)

    async def _subscribe(self, topic: str) -> None:
        """Subscribe to a topic."""
        self._subscriptions.append(topic)

        if not self._ws:
            self._ws = await websockets.connect(self._config.ws_url)
            self._ws_task = asyncio.create_task(self._ws_handler())

        msg = {
            "op": "subscribe",
            "args": [topic]
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
                ping_msg = {"op": "ping"}
                if self._ws:
                    await self._ws.send(orjson.dumps(ping_msg).decode())
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
        topic = data.get("topic", "")

        if topic.startswith("tickers."):
            ticker_data = data.get("data", {})
            tick = Tick(
                symbol=Symbol.from_str(ticker_data.get("symbol", "")),
                bid=to_price(float(ticker_data.get("bid1Price", 0))),
                bid_qty=to_qty(float(ticker_data.get("bid1Size", 0))),
                ask=to_price(float(ticker_data.get("ask1Price", 0))),
                ask_qty=to_qty(float(ticker_data.get("ask1Size", 0))),
                last_price=to_price(float(ticker_data.get("lastPrice", 0))),
                timestamp=now_ns(),
            )
            if self._callbacks and self._callbacks.on_tick:
                self._callbacks.on_tick(self.exchange_id, tick)

        elif topic.startswith("orderbook."):
            ob_data = data.get("data", {})
            bids = ob_data.get("b", [])
            asks = ob_data.get("a", [])
            if bids and asks:
                tick = Tick(
                    symbol=Symbol.from_str(ob_data.get("s", "")),
                    bid=to_price(float(bids[0][0])) if bids else 0,
                    bid_qty=to_qty(float(bids[0][1])) if bids else 0,
                    ask=to_price(float(asks[0][0])) if asks else 0,
                    ask_qty=to_qty(float(asks[0][1])) if asks else 0,
                    timestamp=now_ns(),
                )
                if self._callbacks and self._callbacks.on_tick:
                    self._callbacks.on_tick(self.exchange_id, tick)

    def _sign_request(self, timestamp: int, params: str) -> str:
        """Sign request with HMAC-SHA256."""
        sign_str = f"{timestamp}{self._config.api_key}{params}"
        return hmac.new(
            self._config.api_secret.encode(),
            sign_str.encode(),
            hashlib.sha256
        ).hexdigest()

    def _get_headers(self, timestamp: int, sign: str) -> Dict[str, str]:
        """Get request headers."""
        return {
            "X-BAPI-API-KEY": self._config.api_key,
            "X-BAPI-TIMESTAMP": str(timestamp),
            "X-BAPI-SIGN": sign,
            "Content-Type": "application/json"
        }

    async def send_order(self, request: OrderRequest) -> OrderResponse:
        """Send order to Bybit."""
        if not self._session:
            return OrderResponse(success=False, error_message="Not connected")

        timestamp = int(time.time() * 1000)
        params = {
            "category": "spot",
            "symbol": str(request.symbol),
            "side": "Buy" if request.side == Side.BUY else "Sell",
            "orderType": "Limit" if request.order_type == OrderType.LIMIT else "Market",
            "qty": str(request.quantity / 10**8),
        }

        if request.order_type != OrderType.MARKET:
            params["price"] = str(request.price / 10**8)
            params["timeInForce"] = self._tif_str(request.tif)

        body = orjson.dumps(params).decode()
        sign = self._sign_request(timestamp, body)

        try:
            async with self._session.post(
                f"{self._config.rest_url}/v5/order/create",
                headers=self._get_headers(timestamp, sign),
                data=body
            ) as resp:
                data = await resp.json()

                if data.get("retCode") == 0:
                    result = data.get("result", {})
                    return OrderResponse(
                        success=True,
                        exchange_order_id=int(result.get("orderId", 0)),
                    )
                else:
                    return OrderResponse(
                        success=False,
                        error_message=data.get("retMsg", "Unknown error"),
                    )
        except Exception as e:
            return OrderResponse(success=False, error_message=str(e))

    async def cancel_order(self, symbol: Symbol, order_id: int) -> bool:
        """Cancel order on Bybit."""
        if not self._session:
            return False

        timestamp = int(time.time() * 1000)
        params = {
            "category": "spot",
            "symbol": str(symbol),
            "orderId": str(order_id),
        }

        body = orjson.dumps(params).decode()
        sign = self._sign_request(timestamp, body)

        try:
            async with self._session.post(
                f"{self._config.rest_url}/v5/order/cancel",
                headers=self._get_headers(timestamp, sign),
                data=body
            ) as resp:
                data = await resp.json()
                return data.get("retCode") == 0
        except Exception:
            return False

    async def cancel_all_orders(self, symbol: Symbol) -> int:
        """Cancel all orders for symbol."""
        if not self._session:
            return 0

        timestamp = int(time.time() * 1000)
        params = {
            "category": "spot",
            "symbol": str(symbol),
        }

        body = orjson.dumps(params).decode()
        sign = self._sign_request(timestamp, body)

        try:
            async with self._session.post(
                f"{self._config.rest_url}/v5/order/cancel-all",
                headers=self._get_headers(timestamp, sign),
                data=body
            ) as resp:
                data = await resp.json()
                if data.get("retCode") == 0:
                    return len(data.get("result", {}).get("list", []))
                return 0
        except Exception:
            return 0

    async def get_open_orders(self, symbol: Symbol) -> List[Order]:
        """Get open orders."""
        if not self._session:
            return []

        timestamp = int(time.time() * 1000)
        params = f"category=spot&symbol={symbol}"
        sign = self._sign_request(timestamp, params)

        try:
            async with self._session.get(
                f"{self._config.rest_url}/v5/order/realtime?{params}",
                headers=self._get_headers(timestamp, sign)
            ) as resp:
                data = await resp.json()
                if data.get("retCode") == 0:
                    orders = data.get("result", {}).get("list", [])
                    return [self._parse_order(o) for o in orders]
                return []
        except Exception:
            return []

    def _parse_order(self, data: Dict[str, Any]) -> Order:
        """Parse order from API response."""
        return Order(
            id=0,
            exchange_id=int(data.get("orderId", 0)),
            exchange=ExchangeId.BYBIT,
            symbol=Symbol.from_str(data.get("symbol", "")),
            side=Side.BUY if data.get("side") == "Buy" else Side.SELL,
            order_type=OrderType.LIMIT if data.get("orderType") == "Limit" else OrderType.MARKET,
            price=to_price(float(data.get("price", 0))),
            quantity=to_qty(float(data.get("qty", 0))),
            filled_qty=to_qty(float(data.get("cumExecQty", 0))),
            status=self._parse_status(data.get("orderStatus", "")),
        )

    @staticmethod
    def _tif_str(tif: TimeInForce) -> str:
        return {
            TimeInForce.GTC: "GTC",
            TimeInForce.IOC: "IOC",
            TimeInForce.FOK: "FOK",
            TimeInForce.GTX: "PostOnly",
        }.get(tif, "GTC")

    @staticmethod
    def _parse_status(s: str) -> OrderStatus:
        return {
            "New": OrderStatus.NEW,
            "PartiallyFilled": OrderStatus.PARTIALLY_FILLED,
            "Filled": OrderStatus.FILLED,
            "Cancelled": OrderStatus.CANCELED,
            "Rejected": OrderStatus.REJECTED,
        }.get(s, OrderStatus.PENDING)
