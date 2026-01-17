"""
Risk management implementation.
"""

from dataclasses import dataclass
from enum import Enum, auto
from typing import Optional, Callable
import time

from ..core import (
    Price, Quantity, OrderId, Timestamp, Side, Order, Symbol,
    to_qty, from_qty, from_price, now_ns
)


class RiskViolation(Enum):
    NONE = auto()
    POSITION_LIMIT = auto()
    ORDER_SIZE_LIMIT = auto()
    ORDER_VALUE_LIMIT = auto()
    RATE_LIMIT = auto()
    OPEN_ORDERS_LIMIT = auto()
    DAILY_LOSS_LIMIT = auto()
    DRAWDOWN_LIMIT = auto()
    PRICE_DEVIATION = auto()
    KILL_SWITCH_ACTIVE = auto()


@dataclass
class RiskCheckResult:
    passed: bool
    violation: RiskViolation = RiskViolation.NONE
    message: str = ""

    @classmethod
    def ok(cls) -> "RiskCheckResult":
        return cls(passed=True)

    @classmethod
    def fail(cls, violation: RiskViolation, message: str) -> "RiskCheckResult":
        return cls(passed=False, violation=violation, message=message)


@dataclass
class RiskLimits:
    max_position_qty: Quantity = 0
    max_position_value: float = 0.0
    max_order_qty: Quantity = 0
    max_order_value: float = 0.0
    max_orders_per_second: int = 10
    max_open_orders: int = 100
    max_daily_loss: float = 0.0
    max_drawdown: float = 0.0
    max_deviation_bps: float = 100.0
    kill_switch_enabled: bool = True

    @classmethod
    def default(cls) -> "RiskLimits":
        return cls(
            max_position_qty=to_qty(1.0),
            max_position_value=100000.0,
            max_order_qty=to_qty(0.1),
            max_order_value=10000.0,
            max_orders_per_second=10,
            max_open_orders=100,
            max_daily_loss=1000.0,
            max_drawdown=2000.0,
            max_deviation_bps=100.0,
            kill_switch_enabled=True,
        )


@dataclass
class Position:
    symbol: Symbol
    quantity: Quantity = 0
    avg_price: Price = 0
    unrealized_pnl: float = 0.0
    realized_pnl: float = 0.0
    last_update: Timestamp = 0

    def notional_value(self, current_price: Price) -> float:
        return abs(from_qty(self.quantity)) * from_price(current_price)

    @property
    def is_long(self) -> bool:
        return self.quantity > 0

    @property
    def is_short(self) -> bool:
        return self.quantity < 0

    @property
    def is_flat(self) -> bool:
        return self.quantity == 0


class RiskManager:
    """Risk management with real-time monitoring."""

    def __init__(self, limits: RiskLimits):
        self.limits = limits
        self._position = Position(Symbol(""))
        self._open_orders: dict[OrderId, Order] = {}

        # Rate limiting
        self._orders_this_second = 0
        self._current_second = 0

        # P&L tracking
        self._daily_realized_pnl = 0.0
        self._peak_equity = 0.0

        # Kill switch
        self._kill_switch_active = False
        self._kill_switch_callback: Optional[Callable[[str], None]] = None

        # Statistics
        self._orders_checked = 0
        self._orders_rejected = 0

    def check_order(self, order: Order, reference_price: Optional[Price] = None) -> RiskCheckResult:
        """Pre-trade risk check."""
        self._orders_checked += 1

        # Check kill switch
        if self._kill_switch_active:
            self._orders_rejected += 1
            return RiskCheckResult.fail(
                RiskViolation.KILL_SWITCH_ACTIVE,
                "Kill switch is active"
            )

        # Check position limit
        result = self._check_position_limit(order)
        if not result.passed:
            self._orders_rejected += 1
            return result

        # Check order size
        result = self._check_order_size(order)
        if not result.passed:
            self._orders_rejected += 1
            return result

        # Check rate limit
        result = self._check_rate_limit()
        if not result.passed:
            self._orders_rejected += 1
            return result

        # Check open orders
        result = self._check_open_orders()
        if not result.passed:
            self._orders_rejected += 1
            return result

        # Check daily loss
        result = self._check_daily_loss()
        if not result.passed:
            self._orders_rejected += 1
            return result

        # Check price deviation
        if reference_price is not None:
            result = self._check_price_deviation(order, reference_price)
            if not result.passed:
                self._orders_rejected += 1
                return result

        return RiskCheckResult.ok()

    def _check_position_limit(self, order: Order) -> RiskCheckResult:
        if self.limits.max_position_qty == 0:
            return RiskCheckResult.ok()

        if order.side == Side.BUY:
            potential_pos = self._position.quantity + order.quantity
        else:
            potential_pos = self._position.quantity - order.quantity

        if abs(potential_pos) > self.limits.max_position_qty:
            return RiskCheckResult.fail(
                RiskViolation.POSITION_LIMIT,
                f"Position limit exceeded: potential={from_qty(potential_pos)}"
            )

        return RiskCheckResult.ok()

    def _check_order_size(self, order: Order) -> RiskCheckResult:
        if self.limits.max_order_qty > 0 and order.quantity > self.limits.max_order_qty:
            return RiskCheckResult.fail(
                RiskViolation.ORDER_SIZE_LIMIT,
                f"Order size exceeds limit: qty={from_qty(order.quantity)}"
            )

        if self.limits.max_order_value > 0:
            value = from_qty(order.quantity) * from_price(order.price)
            if value > self.limits.max_order_value:
                return RiskCheckResult.fail(
                    RiskViolation.ORDER_VALUE_LIMIT,
                    f"Order value exceeds limit: value={value:.2f}"
                )

        return RiskCheckResult.ok()

    def _check_rate_limit(self) -> RiskCheckResult:
        if self.limits.max_orders_per_second == 0:
            return RiskCheckResult.ok()

        now = int(time.time())
        if now != self._current_second:
            self._current_second = now
            self._orders_this_second = 0

        self._orders_this_second += 1

        if self._orders_this_second > self.limits.max_orders_per_second:
            return RiskCheckResult.fail(
                RiskViolation.RATE_LIMIT,
                f"Rate limit exceeded: {self._orders_this_second} orders/second"
            )

        return RiskCheckResult.ok()

    def _check_open_orders(self) -> RiskCheckResult:
        if self.limits.max_open_orders == 0:
            return RiskCheckResult.ok()

        if len(self._open_orders) >= self.limits.max_open_orders:
            return RiskCheckResult.fail(
                RiskViolation.OPEN_ORDERS_LIMIT,
                f"Open orders limit reached: {len(self._open_orders)}"
            )

        return RiskCheckResult.ok()

    def _check_daily_loss(self) -> RiskCheckResult:
        if self.limits.max_daily_loss == 0:
            return RiskCheckResult.ok()

        if -self._daily_realized_pnl >= self.limits.max_daily_loss:
            self.activate_kill_switch("Daily loss limit reached")
            return RiskCheckResult.fail(
                RiskViolation.DAILY_LOSS_LIMIT,
                f"Daily loss limit reached: {-self._daily_realized_pnl:.2f}"
            )

        return RiskCheckResult.ok()

    def _check_price_deviation(self, order: Order, reference: Price) -> RiskCheckResult:
        if self.limits.max_deviation_bps == 0 or reference == 0:
            return RiskCheckResult.ok()

        deviation_bps = 10000.0 * abs(order.price - reference) / reference

        if deviation_bps > self.limits.max_deviation_bps:
            return RiskCheckResult.fail(
                RiskViolation.PRICE_DEVIATION,
                f"Price deviation too high: {deviation_bps:.1f} bps"
            )

        return RiskCheckResult.ok()

    def on_order_sent(self, order: Order) -> None:
        """Track sent order."""
        self._open_orders[order.id] = order

    def on_fill(self, order: Order, filled_qty: Quantity, fill_price: Price) -> None:
        """Update position after fill."""
        old_qty = self._position.quantity

        if order.side == Side.BUY:
            if self._position.quantity >= 0:
                # Adding to long
                new_qty = self._position.quantity + filled_qty
                if new_qty > 0:
                    self._position.avg_price = (
                        (self._position.avg_price * old_qty + fill_price * filled_qty) // new_qty
                    )
                self._position.quantity = new_qty
            else:
                # Covering short
                covered = min(filled_qty, -self._position.quantity)
                pnl = from_qty(covered) * (from_price(self._position.avg_price) - from_price(fill_price))
                self._position.realized_pnl += pnl
                self._daily_realized_pnl += pnl

                self._position.quantity += filled_qty
                if self._position.quantity > 0:
                    self._position.avg_price = fill_price
        else:
            if self._position.quantity <= 0:
                # Adding to short
                new_qty = self._position.quantity - filled_qty
                if new_qty < 0:
                    self._position.avg_price = (
                        (self._position.avg_price * (-old_qty) + fill_price * filled_qty) // (-new_qty)
                    )
                self._position.quantity = new_qty
            else:
                # Closing long
                closed = min(filled_qty, self._position.quantity)
                pnl = from_qty(closed) * (from_price(fill_price) - from_price(self._position.avg_price))
                self._position.realized_pnl += pnl
                self._daily_realized_pnl += pnl

                self._position.quantity -= filled_qty
                if self._position.quantity < 0:
                    self._position.avg_price = fill_price

        self._position.last_update = now_ns()

        # Update order tracking
        if order.id in self._open_orders:
            tracked = self._open_orders[order.id]
            tracked.filled_qty += filled_qty
            if tracked.filled_qty >= tracked.quantity:
                del self._open_orders[order.id]

    def on_order_canceled(self, order_id: OrderId) -> None:
        """Remove canceled order from tracking."""
        self._open_orders.pop(order_id, None)

    @property
    def position(self) -> Quantity:
        """Get current position."""
        return self._position.quantity

    @property
    def daily_pnl(self) -> float:
        """Get daily P&L."""
        return self._daily_realized_pnl + self._position.unrealized_pnl

    def activate_kill_switch(self, reason: str) -> None:
        """Activate kill switch."""
        if not self._kill_switch_active:
            self._kill_switch_active = True
            if self._kill_switch_callback:
                self._kill_switch_callback(reason)

    def deactivate_kill_switch(self) -> None:
        """Deactivate kill switch."""
        self._kill_switch_active = False

    @property
    def is_kill_switch_active(self) -> bool:
        return self._kill_switch_active

    def set_kill_switch_callback(self, callback: Callable[[str], None]) -> None:
        """Set callback for kill switch activation."""
        self._kill_switch_callback = callback

    def reset_daily_stats(self) -> None:
        """Reset daily statistics."""
        self._daily_realized_pnl = 0.0
        self._peak_equity = self._position.unrealized_pnl
