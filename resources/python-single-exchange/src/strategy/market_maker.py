"""
Market making strategy implementations.
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Callable, Optional

from ..core import (
    Price, Quantity, OrderId, Timestamp, Side, Order, Symbol,
    to_price, to_qty, from_price, from_qty, now_ns
)
from ..orderbook import OrderBook


@dataclass
class Signal:
    """Market signal for strategy decisions."""
    fair_value: float = 0.0
    volatility: float = 0.0
    momentum: float = 0.0
    inventory_pressure: float = 0.0
    timestamp: Timestamp = 0


@dataclass
class QuoteDecision:
    """Strategy quote decision."""
    should_quote: bool = False
    bid_price: Price = 0
    ask_price: Price = 0
    bid_size: Quantity = 0
    ask_size: Quantity = 0
    reason: str = ""


@dataclass
class MarketMakerParams:
    """Market making parameters."""
    min_spread_bps: float = 5.0
    max_spread_bps: float = 50.0
    target_spread_bps: float = 10.0
    max_position: Quantity = 0
    inventory_skew: float = 0.5
    default_order_size: Quantity = 0
    min_order_size: Quantity = 0
    max_order_size: Quantity = 0
    quote_refresh_us: int = 100_000
    min_quote_life_us: int = 50_000

    @classmethod
    def default(cls) -> "MarketMakerParams":
        return cls(
            min_spread_bps=5.0,
            max_spread_bps=50.0,
            target_spread_bps=10.0,
            max_position=to_qty(1.0),
            inventory_skew=0.5,
            default_order_size=to_qty(0.001),
            min_order_size=to_qty(0.0001),
            max_order_size=to_qty(0.1),
            quote_refresh_us=100_000,
            min_quote_life_us=50_000,
        )


class MarketMaker(ABC):
    """Base market making strategy."""

    def __init__(self, params: MarketMakerParams):
        self.params = params
        self.enabled = False
        self._active_bid_id: OrderId = 0
        self._active_ask_id: OrderId = 0
        self._active_bid_price: Price = 0
        self._active_ask_price: Price = 0
        self._last_quote_time: Timestamp = 0
        self._quotes_sent = 0
        self._fills = 0

    @abstractmethod
    def compute_quotes(
        self,
        book: OrderBook,
        position: Quantity,
        signal: Signal
    ) -> QuoteDecision:
        """Compute quotes based on market state."""
        pass

    def on_fill(self, order: Order, filled_qty: Quantity, fill_price: Price) -> None:
        """Handle fill event."""
        self._fills += 1

    def on_cancel(self, order_id: OrderId) -> None:
        """Handle cancel event."""
        if order_id == self._active_bid_id:
            self._active_bid_id = 0
            self._active_bid_price = 0
        elif order_id == self._active_ask_id:
            self._active_ask_id = 0
            self._active_ask_price = 0

    @property
    def quotes_sent(self) -> int:
        return self._quotes_sent

    @property
    def fills(self) -> int:
        return self._fills


class BasicMarketMaker(MarketMaker):
    """Basic market making strategy."""

    def compute_quotes(
        self,
        book: OrderBook,
        position: Quantity,
        signal: Signal
    ) -> QuoteDecision:
        decision = QuoteDecision()

        if not self.enabled:
            decision.reason = "Strategy disabled"
            return decision

        if not book.is_valid:
            decision.reason = "Invalid orderbook"
            return decision

        fair_value = book.mid_price
        if fair_value is None:
            decision.reason = "Cannot determine fair value"
            return decision

        # Calculate spread
        spread_bps = self._calculate_spread(signal)
        half_spread = int(fair_value * spread_bps / 20000.0)

        # Calculate inventory skew
        skew = self._calculate_inventory_skew(position)
        skew_adj = int(fair_value * skew * self.params.inventory_skew / 10000.0)

        decision.bid_price = fair_value - half_spread - skew_adj
        decision.ask_price = fair_value + half_spread - skew_adj

        # Ensure no crossing
        if decision.bid_price >= decision.ask_price:
            decision.reason = "Prices would cross"
            return decision

        # Calculate sizes
        decision.bid_size = self._calculate_order_size(Side.BUY, position)
        decision.ask_size = self._calculate_order_size(Side.SELL, position)

        if decision.bid_size == 0 and decision.ask_size == 0:
            decision.reason = "Order sizes are zero"
            return decision

        # Check if we should skip quoting
        now = now_ns()
        if now - self._last_quote_time < self.params.min_quote_life_us * 1000:
            bid_diff = abs(decision.bid_price - self._active_bid_price)
            ask_diff = abs(decision.ask_price - self._active_ask_price)
            threshold = fair_value // 10000  # 1 bps

            if bid_diff < threshold and ask_diff < threshold:
                decision.reason = "Prices unchanged"
                return decision

        decision.should_quote = True
        self._last_quote_time = now
        self._quotes_sent += 1

        return decision

    def _calculate_spread(self, signal: Signal) -> float:
        spread = self.params.target_spread_bps

        # Adjust for volatility
        if signal.volatility > 0:
            spread *= (1.0 + signal.volatility)

        return max(self.params.min_spread_bps, min(spread, self.params.max_spread_bps))

    def _calculate_inventory_skew(self, position: Quantity) -> float:
        if self.params.max_position == 0:
            return 0.0
        return position / self.params.max_position

    def _calculate_order_size(self, side: Side, position: Quantity) -> Quantity:
        size = self.params.default_order_size

        if self.params.max_position > 0:
            if side == Side.BUY and position > 0:
                ratio = 1.0 - position / self.params.max_position
                size = int(size * max(0.0, ratio))
            elif side == Side.SELL and position < 0:
                ratio = 1.0 + position / self.params.max_position
                size = int(size * max(0.0, ratio))

        return max(self.params.min_order_size, min(size, self.params.max_order_size))


class AvellanedaStoikovMM(MarketMaker):
    """Avellaneda-Stoikov optimal market making strategy."""

    def __init__(
        self,
        params: MarketMakerParams,
        gamma: float = 0.1,  # Risk aversion
        sigma: float = 0.01,  # Volatility
        k: float = 1.5,  # Order arrival intensity
        t_horizon: float = 1.0  # Time horizon in seconds
    ):
        super().__init__(params)
        self.gamma = gamma
        self.sigma = sigma
        self.k = k
        self.t_horizon = t_horizon
        self._start_time: Timestamp = 0

    def compute_quotes(
        self,
        book: OrderBook,
        position: Quantity,
        signal: Signal
    ) -> QuoteDecision:
        import math

        decision = QuoteDecision()

        if not self.enabled or not book.is_valid:
            decision.reason = "Disabled or invalid book"
            return decision

        # Initialize start time
        if self._start_time == 0:
            self._start_time = signal.timestamp

        # Calculate time remaining
        elapsed_s = (signal.timestamp - self._start_time) / 1e9
        t_elapsed = elapsed_s / self.t_horizon
        t_remaining = max(0.01, 1.0 - (t_elapsed % 1.0))

        mid = book.mid_price
        if mid is None:
            decision.reason = "No mid price"
            return decision

        # Calculate reservation price
        reservation = self._reservation_price(mid, position, t_remaining)

        # Calculate optimal spread
        spread_bps = self._optimal_spread(t_remaining)
        spread_bps = max(self.params.min_spread_bps, min(spread_bps, self.params.max_spread_bps))
        half_spread = int(mid * spread_bps / 20000.0)

        decision.bid_price = reservation - half_spread
        decision.ask_price = reservation + half_spread

        if decision.bid_price >= decision.ask_price:
            decision.reason = "Prices would cross"
            return decision

        decision.bid_size = self.params.default_order_size
        decision.ask_size = self.params.default_order_size

        if decision.bid_size > 0 or decision.ask_size > 0:
            decision.should_quote = True

        return decision

    def _reservation_price(self, mid: Price, position: Quantity, t_remaining: float) -> Price:
        """r(s,q,t) = s - q * gamma * sigma^2 * (T - t)"""
        adjustment = position * self.gamma * (self.sigma ** 2) * t_remaining
        return mid - int(mid * adjustment)

    def _optimal_spread(self, t_remaining: float) -> float:
        """delta = gamma * sigma^2 * (T-t) + (2/gamma) * ln(1 + gamma/k)"""
        import math
        term1 = self.gamma * (self.sigma ** 2) * t_remaining
        term2 = (2.0 / self.gamma) * math.log(1.0 + self.gamma / self.k)
        return (term1 + term2) * 10000.0  # Convert to bps
