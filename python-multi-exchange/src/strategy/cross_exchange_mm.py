"""Cross-exchange market making strategy."""

import asyncio
from dataclasses import dataclass, field
from typing import Optional, Dict, List
import structlog

from ..core import (
    ExchangeId, Symbol, Price, Quantity, Side, OrderType, TimeInForce,
    Order, NBBO, from_price, from_qty, to_price, now_ns
)
from ..orderbook import ConsolidatedBook
from ..exchange import ExchangeManager
from ..exchange.base import OrderRequest, OrderResponse

log = structlog.get_logger()


@dataclass
class CrossExchangeMMParams:
    """Parameters for cross-exchange market making."""
    # Spread parameters
    min_spread_bps: float = 5.0
    target_spread_bps: float = 10.0
    max_spread_bps: float = 50.0

    # Position parameters
    max_position: float = 1.0  # Max position in base currency
    order_size: float = 0.1  # Order size in base currency

    # Inventory skew
    inventory_skew_factor: float = 0.5  # How much to skew based on inventory

    # Exchange preferences
    prefer_lowest_latency: bool = True

    # Quote distribution
    quote_on_all_exchanges: bool = False  # Quote on all vs best only

    # Hedging
    hedge_on_fill: bool = True
    hedge_exchange: Optional[ExchangeId] = None  # Prefer this exchange for hedging

    @classmethod
    def default(cls) -> "CrossExchangeMMParams":
        return cls()


@dataclass
class QuoteDecision:
    """Decision from strategy."""
    should_quote: bool = False
    quotes: Dict[ExchangeId, tuple] = field(default_factory=dict)  # exchange -> (bid, ask, size)


@dataclass
class CrossExchangeMMStats:
    """Strategy statistics."""
    quotes_sent: int = 0
    fills: int = 0
    hedges_sent: int = 0
    pnl_realized: float = 0.0


class CrossExchangeMM:
    """Cross-exchange market making strategy."""

    def __init__(self, params: CrossExchangeMMParams):
        self._params = params
        self._stats = CrossExchangeMMStats()
        self._position: float = 0.0  # Current position in base currency
        self._active_orders: Dict[ExchangeId, List[int]] = {}  # exchange -> order_ids
        self.enabled = False

    @property
    def stats(self) -> CrossExchangeMMStats:
        return self._stats

    @property
    def position(self) -> float:
        return self._position

    def update_position(self, delta: float) -> None:
        """Update position after fill."""
        self._position += delta

    def compute_quotes(
        self,
        book: ConsolidatedBook,
        exchange_manager: ExchangeManager,
    ) -> QuoteDecision:
        """Compute quotes based on consolidated book.

        Returns quotes to place on each exchange.
        """
        if not self.enabled:
            return QuoteDecision(should_quote=False)

        nbbo = book.nbbo
        if not nbbo.mid_price:
            return QuoteDecision(should_quote=False)

        mid_price = nbbo.mid_price
        fair_value = mid_price

        # Calculate spread based on NBBO spread
        nbbo_spread_bps = nbbo.spread_bps or self._params.target_spread_bps
        half_spread_bps = max(
            self._params.min_spread_bps / 2,
            min(nbbo_spread_bps / 2, self._params.max_spread_bps / 2)
        )

        # Inventory skew: if long, lower bid and raise ask
        inventory_skew_bps = self._position * self._params.inventory_skew_factor * 10  # 10 bps per unit

        # Calculate bid/ask prices
        bid_offset_bps = half_spread_bps + inventory_skew_bps
        ask_offset_bps = half_spread_bps - inventory_skew_bps

        bid_price = int(fair_value * (1 - bid_offset_bps / 10000))
        ask_price = int(fair_value * (1 + ask_offset_bps / 10000))

        # Ensure bid < ask
        if bid_price >= ask_price:
            bid_price = int(fair_value * 0.9999)
            ask_price = int(fair_value * 1.0001)

        # Check position limits
        can_buy = self._position < self._params.max_position
        can_sell = self._position > -self._params.max_position

        # Determine which exchanges to quote on
        quotes: Dict[ExchangeId, tuple] = {}
        order_size = to_price(self._params.order_size)

        if self._params.quote_on_all_exchanges:
            # Quote on all connected exchanges
            for exchange in exchange_manager.get_connected_exchanges():
                quotes[exchange] = (
                    bid_price if can_buy else 0,
                    ask_price if can_sell else 0,
                    order_size,
                )
        else:
            # Quote only on best exchange
            if self._params.prefer_lowest_latency:
                best_exchange = exchange_manager.get_fastest_exchange()
            else:
                best_exchange = nbbo.best_bid_exchange or nbbo.best_ask_exchange

            if best_exchange:
                quotes[best_exchange] = (
                    bid_price if can_buy else 0,
                    ask_price if can_sell else 0,
                    order_size,
                )

        return QuoteDecision(
            should_quote=len(quotes) > 0,
            quotes=quotes,
        )

    async def send_quotes(
        self,
        decision: QuoteDecision,
        exchange_manager: ExchangeManager,
        symbol: Symbol,
    ) -> None:
        """Send quotes to exchanges."""
        if not decision.should_quote:
            return

        tasks = []

        for exchange, (bid_price, ask_price, size) in decision.quotes.items():
            # Send bid
            if bid_price > 0:
                bid_request = OrderRequest(
                    symbol=symbol,
                    side=Side.BUY,
                    order_type=OrderType.LIMIT_MAKER,
                    price=bid_price,
                    quantity=size,
                    tif=TimeInForce.GTX,
                )
                tasks.append(exchange_manager.send_order(exchange, bid_request))

            # Send ask
            if ask_price > 0:
                ask_request = OrderRequest(
                    symbol=symbol,
                    side=Side.SELL,
                    order_type=OrderType.LIMIT_MAKER,
                    price=ask_price,
                    quantity=size,
                    tif=TimeInForce.GTX,
                )
                tasks.append(exchange_manager.send_order(exchange, ask_request))

        if tasks:
            results = await asyncio.gather(*tasks, return_exceptions=True)
            for result in results:
                if isinstance(result, OrderResponse) and result.success:
                    self._stats.quotes_sent += 1

    async def hedge_fill(
        self,
        fill_exchange: ExchangeId,
        fill_side: Side,
        fill_qty: Quantity,
        fill_price: Price,
        exchange_manager: ExchangeManager,
        book: ConsolidatedBook,
        symbol: Symbol,
    ) -> Optional[OrderResponse]:
        """Hedge a fill on another exchange.

        When we get filled on one exchange, immediately hedge on another.
        """
        if not self._params.hedge_on_fill:
            return None

        # Determine hedge exchange (opposite side, different exchange)
        hedge_exchange = self._params.hedge_exchange

        if not hedge_exchange or hedge_exchange == fill_exchange:
            # Find best exchange for hedge
            venues = book.get_venues_by_price(is_buy=(fill_side == Side.SELL))
            for venue_exchange, _, _ in venues:
                if venue_exchange != fill_exchange:
                    hedge_exchange = venue_exchange
                    break

        if not hedge_exchange or hedge_exchange == fill_exchange:
            log.warning("No hedge exchange available")
            return None

        # Hedge is opposite side
        hedge_side = Side.SELL if fill_side == Side.BUY else Side.BUY

        # Get aggressive price for hedge (cross the spread to ensure fill)
        venue_book = book.get_exchange_book(hedge_exchange)
        if not venue_book:
            return None

        if hedge_side == Side.BUY:
            hedge_price = int(venue_book.best_ask * 1.001)  # Pay up slightly
        else:
            hedge_price = int(venue_book.best_bid * 0.999)  # Accept slightly less

        hedge_request = OrderRequest(
            symbol=symbol,
            side=hedge_side,
            order_type=OrderType.LIMIT,
            price=hedge_price,
            quantity=fill_qty,
            tif=TimeInForce.IOC,
        )

        log.info(
            "Sending hedge order",
            exchange=str(hedge_exchange),
            side=hedge_side.name,
            price=from_price(hedge_price),
            qty=from_qty(fill_qty),
        )

        result = await exchange_manager.send_order(hedge_exchange, hedge_request)

        if result.success:
            self._stats.hedges_sent += 1

        return result

    def on_fill(self, side: Side, quantity: Quantity, price: Price) -> None:
        """Handle fill notification."""
        self._stats.fills += 1

        # Update position
        qty_float = from_qty(quantity)
        if side == Side.BUY:
            self._position += qty_float
        else:
            self._position -= qty_float

        log.info(
            "Fill received",
            side=side.name,
            qty=qty_float,
            price=from_price(price),
            new_position=self._position,
        )
