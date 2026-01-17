"""
Order book implementation using sorted containers for performance.
"""

from dataclasses import dataclass
from typing import Optional, List, Tuple
from sortedcontainers import SortedDict

from ..core import (
    Price, Quantity, OrderId, Timestamp, Symbol, Order, Side, now_ns
)


@dataclass
class PriceLevel:
    """Aggregated price level."""
    price: Price
    quantity: Quantity
    order_count: int = 1
    last_update: Timestamp = 0


class OrderBook:
    """
    L2 Order Book with L3 support.

    Uses SortedDict for efficient best bid/ask access.
    """

    MAX_DEPTH = 100

    def __init__(self, symbol: Symbol):
        self.symbol = symbol
        # Bids: negative keys for descending order (best bid first)
        self._bids: SortedDict[int, PriceLevel] = SortedDict()
        # Asks: positive keys for ascending order (best ask first)
        self._asks: SortedDict[Price, PriceLevel] = SortedDict()
        # L3: Order tracking
        self._orders: dict[OrderId, Order] = {}
        self._last_update: Timestamp = 0
        self._sequence: int = 0

    # L2 Updates
    def update_bid(self, price: Price, quantity: Quantity) -> None:
        """Update bid at price level."""
        key = -price  # Negative for descending sort
        if quantity == 0:
            self._bids.pop(key, None)
        else:
            self._bids[key] = PriceLevel(price, quantity, 1, now_ns())
        self._last_update = now_ns()

    def update_ask(self, price: Price, quantity: Quantity) -> None:
        """Update ask at price level."""
        if quantity == 0:
            self._asks.pop(price, None)
        else:
            self._asks[price] = PriceLevel(price, quantity, 1, now_ns())
        self._last_update = now_ns()

    def clear(self) -> None:
        """Clear the order book."""
        self._bids.clear()
        self._asks.clear()
        self._orders.clear()

    def apply_snapshot(
        self,
        bids: List[Tuple[Price, Quantity]],
        asks: List[Tuple[Price, Quantity]]
    ) -> None:
        """Apply a full snapshot."""
        self._bids.clear()
        self._asks.clear()

        for price, qty in bids:
            self._bids[-price] = PriceLevel(price, qty, 1, now_ns())

        for price, qty in asks:
            self._asks[price] = PriceLevel(price, qty, 1, now_ns())

        self._last_update = now_ns()

    # L3 Updates
    def add_order(self, order: Order) -> None:
        """Add individual order."""
        self._orders[order.id] = order

        if order.side == Side.BUY:
            key = -order.price
            if key in self._bids:
                level = self._bids[key]
                level.quantity += order.quantity
                level.order_count += 1
            else:
                self._bids[key] = PriceLevel(order.price, order.quantity, 1, now_ns())
        else:
            if order.price in self._asks:
                level = self._asks[order.price]
                level.quantity += order.quantity
                level.order_count += 1
            else:
                self._asks[order.price] = PriceLevel(order.price, order.quantity, 1, now_ns())

    def remove_order(self, order_id: OrderId) -> Optional[Order]:
        """Remove individual order."""
        order = self._orders.pop(order_id, None)
        if order is None:
            return None

        if order.side == Side.BUY:
            key = -order.price
            if key in self._bids:
                level = self._bids[key]
                level.quantity -= order.remaining
                level.order_count -= 1
                if level.quantity <= 0 or level.order_count <= 0:
                    del self._bids[key]
        else:
            if order.price in self._asks:
                level = self._asks[order.price]
                level.quantity -= order.remaining
                level.order_count -= 1
                if level.quantity <= 0 or level.order_count <= 0:
                    del self._asks[order.price]

        return order

    # Queries
    @property
    def best_bid(self) -> Optional[Price]:
        """Get best bid price."""
        if not self._bids:
            return None
        return self._bids.peekitem(0)[1].price

    @property
    def best_ask(self) -> Optional[Price]:
        """Get best ask price."""
        if not self._asks:
            return None
        return self._asks.peekitem(0)[1].price

    @property
    def best_bid_qty(self) -> Optional[Quantity]:
        """Get best bid quantity."""
        if not self._bids:
            return None
        return self._bids.peekitem(0)[1].quantity

    @property
    def best_ask_qty(self) -> Optional[Quantity]:
        """Get best ask quantity."""
        if not self._asks:
            return None
        return self._asks.peekitem(0)[1].quantity

    @property
    def mid_price(self) -> Optional[Price]:
        """Get mid price."""
        bid = self.best_bid
        ask = self.best_ask
        if bid is None or ask is None:
            return None
        return (bid + ask) // 2

    @property
    def spread(self) -> Optional[Price]:
        """Get spread."""
        bid = self.best_bid
        ask = self.best_ask
        if bid is None or ask is None:
            return None
        return ask - bid

    @property
    def spread_bps(self) -> Optional[float]:
        """Get spread in basis points."""
        mid = self.mid_price
        spread = self.spread
        if mid is None or spread is None or mid == 0:
            return None
        return 10000.0 * spread / mid

    def bid_level(self, depth: int) -> Optional[PriceLevel]:
        """Get bid level at depth."""
        if depth >= len(self._bids):
            return None
        return list(self._bids.values())[depth]

    def ask_level(self, depth: int) -> Optional[PriceLevel]:
        """Get ask level at depth."""
        if depth >= len(self._asks):
            return None
        return list(self._asks.values())[depth]

    @property
    def bid_depth(self) -> int:
        """Get number of bid levels."""
        return len(self._bids)

    @property
    def ask_depth(self) -> int:
        """Get number of ask levels."""
        return len(self._asks)

    def vwap_bid(self, target_qty: Quantity) -> Optional[Price]:
        """Calculate VWAP for selling (hitting bids)."""
        remaining = target_qty
        total_value = 0
        total_qty = 0

        for level in self._bids.values():
            fill = min(remaining, level.quantity)
            total_value += level.price * fill
            total_qty += fill
            remaining -= fill
            if remaining <= 0:
                break

        if total_qty == 0:
            return None
        return total_value // total_qty

    def vwap_ask(self, target_qty: Quantity) -> Optional[Price]:
        """Calculate VWAP for buying (lifting asks)."""
        remaining = target_qty
        total_value = 0
        total_qty = 0

        for level in self._asks.values():
            fill = min(remaining, level.quantity)
            total_value += level.price * fill
            total_qty += fill
            remaining -= fill
            if remaining <= 0:
                break

        if total_qty == 0:
            return None
        return total_value // total_qty

    def imbalance(self, levels: int = 5) -> float:
        """Calculate book imbalance (positive = bid heavy)."""
        bid_vol = sum(
            level.quantity
            for _, level in zip(range(levels), self._bids.values())
        )
        ask_vol = sum(
            level.quantity
            for _, level in zip(range(levels), self._asks.values())
        )

        total = bid_vol + ask_vol
        if total == 0:
            return 0.0

        return (bid_vol - ask_vol) / total

    @property
    def is_valid(self) -> bool:
        """Check if book is valid (not crossed)."""
        bid = self.best_bid
        ask = self.best_ask
        if bid is None or ask is None:
            return True  # Empty book is valid
        return bid < ask

    @property
    def last_update(self) -> Timestamp:
        return self._last_update

    @property
    def sequence(self) -> int:
        return self._sequence

    @sequence.setter
    def sequence(self, value: int) -> None:
        self._sequence = value
