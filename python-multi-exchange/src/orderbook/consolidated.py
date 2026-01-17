"""Consolidated order book aggregating multiple exchanges."""

from dataclasses import dataclass, field
from typing import Dict, Optional, List, Tuple
from sortedcontainers import SortedDict

from ..core import (
    ExchangeId, Symbol, Price, Quantity, Timestamp, Tick,
    NBBO, ArbitrageOpportunity, now_ns
)


@dataclass
class PriceLevel:
    """Price level with quantity by exchange."""
    price: Price
    quantities: Dict[ExchangeId, Quantity] = field(default_factory=dict)

    @property
    def total_quantity(self) -> Quantity:
        return sum(self.quantities.values())

    def add_exchange(self, exchange: ExchangeId, qty: Quantity) -> None:
        if qty > 0:
            self.quantities[exchange] = qty
        elif exchange in self.quantities:
            del self.quantities[exchange]

    def remove_exchange(self, exchange: ExchangeId) -> None:
        self.quantities.pop(exchange, None)


class ExchangeBook:
    """Order book for a single exchange."""

    def __init__(self, exchange: ExchangeId, symbol: Symbol):
        self.exchange = exchange
        self.symbol = symbol
        self.best_bid: Price = 0
        self.best_bid_qty: Quantity = 0
        self.best_ask: Price = 0
        self.best_ask_qty: Quantity = 0
        self.last_update: Timestamp = 0

    def update(self, tick: Tick) -> None:
        """Update from tick."""
        self.best_bid = tick.bid
        self.best_bid_qty = tick.bid_qty
        self.best_ask = tick.ask
        self.best_ask_qty = tick.ask_qty
        self.last_update = now_ns()

    @property
    def mid_price(self) -> Optional[Price]:
        if self.best_bid > 0 and self.best_ask > 0:
            return (self.best_bid + self.best_ask) // 2
        return None

    @property
    def spread_bps(self) -> Optional[float]:
        mid = self.mid_price
        if mid and mid > 0:
            return (self.best_ask - self.best_bid) / mid * 10000
        return None


class ConsolidatedBook:
    """Consolidated order book across multiple exchanges."""

    def __init__(self, symbol: Symbol):
        self.symbol = symbol
        self._exchange_books: Dict[ExchangeId, ExchangeBook] = {}
        self._nbbo = NBBO(symbol=symbol)
        self._last_update: Timestamp = 0

    def add_exchange(self, exchange: ExchangeId) -> None:
        """Add an exchange to track."""
        if exchange not in self._exchange_books:
            self._exchange_books[exchange] = ExchangeBook(exchange, self.symbol)

    def remove_exchange(self, exchange: ExchangeId) -> None:
        """Remove an exchange."""
        self._exchange_books.pop(exchange, None)
        self._update_nbbo()

    def update(self, exchange: ExchangeId, tick: Tick) -> None:
        """Update from exchange tick."""
        if exchange not in self._exchange_books:
            self.add_exchange(exchange)

        self._exchange_books[exchange].update(tick)
        self._last_update = now_ns()
        self._update_nbbo()

    def _update_nbbo(self) -> None:
        """Recalculate NBBO."""
        best_bid = 0
        best_bid_qty = 0
        best_bid_exchange = None
        best_ask = 0
        best_ask_qty = 0
        best_ask_exchange = None

        for exchange, book in self._exchange_books.items():
            # Best bid (highest)
            if book.best_bid > best_bid:
                best_bid = book.best_bid
                best_bid_qty = book.best_bid_qty
                best_bid_exchange = exchange

            # Best ask (lowest)
            if book.best_ask > 0 and (best_ask == 0 or book.best_ask < best_ask):
                best_ask = book.best_ask
                best_ask_qty = book.best_ask_qty
                best_ask_exchange = exchange

        self._nbbo = NBBO(
            symbol=self.symbol,
            best_bid=best_bid,
            best_bid_qty=best_bid_qty,
            best_bid_exchange=best_bid_exchange,
            best_ask=best_ask,
            best_ask_qty=best_ask_qty,
            best_ask_exchange=best_ask_exchange,
            timestamp=now_ns(),
        )

    @property
    def nbbo(self) -> NBBO:
        """Get National Best Bid and Offer."""
        return self._nbbo

    @property
    def mid_price(self) -> Optional[Price]:
        """Get mid price from NBBO."""
        return self._nbbo.mid_price

    @property
    def spread_bps(self) -> Optional[float]:
        """Get spread in basis points from NBBO."""
        return self._nbbo.spread_bps

    def get_exchange_book(self, exchange: ExchangeId) -> Optional[ExchangeBook]:
        """Get order book for specific exchange."""
        return self._exchange_books.get(exchange)

    def get_all_exchange_books(self) -> Dict[ExchangeId, ExchangeBook]:
        """Get all exchange order books."""
        return self._exchange_books.copy()

    def detect_arbitrage(self, min_profit_bps: float = 1.0) -> Optional[ArbitrageOpportunity]:
        """Detect cross-exchange arbitrage opportunity.

        Returns an opportunity if we can buy on one exchange and sell on another
        for a profit greater than min_profit_bps.
        """
        if len(self._exchange_books) < 2:
            return None

        best_buy: Optional[Tuple[ExchangeId, Price, Quantity]] = None
        best_sell: Optional[Tuple[ExchangeId, Price, Quantity]] = None

        for exchange, book in self._exchange_books.items():
            # Find lowest ask (where we would buy)
            if book.best_ask > 0:
                if best_buy is None or book.best_ask < best_buy[1]:
                    best_buy = (exchange, book.best_ask, book.best_ask_qty)

            # Find highest bid (where we would sell)
            if book.best_bid > 0:
                if best_sell is None or book.best_bid > best_sell[1]:
                    best_sell = (exchange, book.best_bid, book.best_bid_qty)

        if not best_buy or not best_sell:
            return None

        # Check if different exchanges and profitable
        if best_buy[0] == best_sell[0]:
            return None

        buy_price = best_buy[1]
        sell_price = best_sell[1]

        if sell_price <= buy_price:
            return None

        profit_bps = (sell_price - buy_price) / buy_price * 10000

        if profit_bps < min_profit_bps:
            return None

        # Calculate executable quantity (min of both sides)
        quantity = min(best_buy[2], best_sell[2])

        return ArbitrageOpportunity(
            symbol=self.symbol,
            buy_exchange=best_buy[0],
            sell_exchange=best_sell[0],
            buy_price=buy_price,
            sell_price=sell_price,
            quantity=quantity,
            expected_profit_bps=profit_bps,
            timestamp=now_ns(),
        )

    def get_best_execution_venue(self, is_buy: bool) -> Optional[ExchangeId]:
        """Get best exchange for execution.

        Args:
            is_buy: True for buy order (find lowest ask), False for sell (find highest bid)
        """
        if is_buy:
            return self._nbbo.best_ask_exchange
        else:
            return self._nbbo.best_bid_exchange

    def get_venues_by_price(self, is_buy: bool) -> List[Tuple[ExchangeId, Price, Quantity]]:
        """Get venues sorted by price (best first).

        Args:
            is_buy: True for buy (ascending ask), False for sell (descending bid)
        """
        venues = []

        for exchange, book in self._exchange_books.items():
            if is_buy and book.best_ask > 0:
                venues.append((exchange, book.best_ask, book.best_ask_qty))
            elif not is_buy and book.best_bid > 0:
                venues.append((exchange, book.best_bid, book.best_bid_qty))

        # Sort: ascending for buys (lowest ask first), descending for sells (highest bid first)
        venues.sort(key=lambda x: x[1], reverse=not is_buy)
        return venues
