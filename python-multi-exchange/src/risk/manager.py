"""Risk manager for multi-exchange trading."""

from dataclasses import dataclass, field
from typing import Dict, Optional
from enum import Enum, auto
import structlog

from ..core import ExchangeId, Symbol, Price, Quantity, Side, from_price, from_qty

log = structlog.get_logger()


class RiskStatus(Enum):
    """Risk status."""
    OK = auto()
    WARNING = auto()
    BREACH = auto()
    KILL_SWITCH = auto()


@dataclass
class RiskLimits:
    """Risk limits configuration."""
    # Position limits
    max_position_per_exchange: float = 1.0  # Max position per exchange
    max_total_position: float = 3.0  # Max total position across exchanges

    # Loss limits
    max_loss_per_trade: float = 100.0  # USD
    max_daily_loss: float = 1000.0  # USD
    max_drawdown: float = 2000.0  # USD

    # Order limits
    max_order_size: float = 0.5  # Base currency
    max_order_value: float = 10000.0  # USD
    max_orders_per_second: int = 10

    # Price limits
    max_price_deviation_bps: float = 100.0  # Max deviation from mid

    @classmethod
    def default(cls) -> "RiskLimits":
        return cls()

    @classmethod
    def conservative(cls) -> "RiskLimits":
        return cls(
            max_position_per_exchange=0.5,
            max_total_position=1.0,
            max_loss_per_trade=50.0,
            max_daily_loss=500.0,
            max_order_size=0.1,
        )


@dataclass
class ExchangePosition:
    """Position tracking per exchange."""
    exchange: ExchangeId
    position: float = 0.0
    realized_pnl: float = 0.0
    unrealized_pnl: float = 0.0
    avg_entry_price: float = 0.0


@dataclass
class RiskMetrics:
    """Current risk metrics."""
    total_position: float = 0.0
    total_realized_pnl: float = 0.0
    total_unrealized_pnl: float = 0.0
    daily_pnl: float = 0.0
    peak_pnl: float = 0.0
    drawdown: float = 0.0
    orders_this_second: int = 0
    status: RiskStatus = RiskStatus.OK


class RiskManager:
    """Multi-exchange risk manager."""

    def __init__(self, limits: RiskLimits):
        self._limits = limits
        self._positions: Dict[ExchangeId, ExchangePosition] = {}
        self._metrics = RiskMetrics()
        self._kill_switch_active = False

    @property
    def limits(self) -> RiskLimits:
        return self._limits

    @property
    def metrics(self) -> RiskMetrics:
        return self._metrics

    @property
    def is_kill_switch_active(self) -> bool:
        return self._kill_switch_active

    @property
    def total_position(self) -> float:
        return sum(p.position for p in self._positions.values())

    def get_position(self, exchange: ExchangeId) -> float:
        """Get position for specific exchange."""
        if exchange in self._positions:
            return self._positions[exchange].position
        return 0.0

    def check_order(
        self,
        exchange: ExchangeId,
        side: Side,
        quantity: float,
        price: float,
        mid_price: Optional[float] = None,
    ) -> tuple[bool, str]:
        """Check if order passes risk checks.

        Returns (allowed, reason).
        """
        if self._kill_switch_active:
            return False, "Kill switch active"

        # Check order size
        if quantity > self._limits.max_order_size:
            return False, f"Order size {quantity} exceeds limit {self._limits.max_order_size}"

        # Check order value
        order_value = quantity * price
        if order_value > self._limits.max_order_value:
            return False, f"Order value ${order_value:.2f} exceeds limit ${self._limits.max_order_value}"

        # Check position limits
        current_position = self.get_position(exchange)
        new_position = current_position + (quantity if side == Side.BUY else -quantity)

        if abs(new_position) > self._limits.max_position_per_exchange:
            return False, f"Would exceed per-exchange position limit"

        total_after = self.total_position + (quantity if side == Side.BUY else -quantity)
        if abs(total_after) > self._limits.max_total_position:
            return False, f"Would exceed total position limit"

        # Check price deviation
        if mid_price and mid_price > 0:
            deviation_bps = abs(price - mid_price) / mid_price * 10000
            if deviation_bps > self._limits.max_price_deviation_bps:
                return False, f"Price deviation {deviation_bps:.1f} bps exceeds limit"

        # Check rate limit
        if self._metrics.orders_this_second >= self._limits.max_orders_per_second:
            return False, "Rate limit exceeded"

        return True, "OK"

    def record_fill(
        self,
        exchange: ExchangeId,
        side: Side,
        quantity: float,
        price: float,
    ) -> None:
        """Record a fill and update positions."""
        if exchange not in self._positions:
            self._positions[exchange] = ExchangePosition(exchange=exchange)

        pos = self._positions[exchange]

        # Update position
        if side == Side.BUY:
            # Buying: update average entry price
            if pos.position >= 0:
                # Adding to long or opening long
                total_value = pos.position * pos.avg_entry_price + quantity * price
                pos.position += quantity
                if pos.position > 0:
                    pos.avg_entry_price = total_value / pos.position
            else:
                # Closing short
                realized = (pos.avg_entry_price - price) * quantity
                pos.realized_pnl += realized
                pos.position += quantity
        else:
            # Selling: similar logic
            if pos.position <= 0:
                # Adding to short or opening short
                total_value = abs(pos.position) * pos.avg_entry_price + quantity * price
                pos.position -= quantity
                if pos.position < 0:
                    pos.avg_entry_price = total_value / abs(pos.position)
            else:
                # Closing long
                realized = (price - pos.avg_entry_price) * quantity
                pos.realized_pnl += realized
                pos.position -= quantity

        self._update_metrics()

    def update_mark_price(self, exchange: ExchangeId, mark_price: float) -> None:
        """Update unrealized PnL with current mark price."""
        if exchange not in self._positions:
            return

        pos = self._positions[exchange]
        if pos.position > 0:
            pos.unrealized_pnl = (mark_price - pos.avg_entry_price) * pos.position
        elif pos.position < 0:
            pos.unrealized_pnl = (pos.avg_entry_price - mark_price) * abs(pos.position)
        else:
            pos.unrealized_pnl = 0.0

        self._update_metrics()

    def _update_metrics(self) -> None:
        """Update risk metrics."""
        self._metrics.total_position = sum(p.position for p in self._positions.values())
        self._metrics.total_realized_pnl = sum(p.realized_pnl for p in self._positions.values())
        self._metrics.total_unrealized_pnl = sum(p.unrealized_pnl for p in self._positions.values())

        total_pnl = self._metrics.total_realized_pnl + self._metrics.total_unrealized_pnl
        self._metrics.daily_pnl = total_pnl

        # Update peak and drawdown
        if total_pnl > self._metrics.peak_pnl:
            self._metrics.peak_pnl = total_pnl
        self._metrics.drawdown = self._metrics.peak_pnl - total_pnl

        # Check risk status
        self._check_risk_status()

    def _check_risk_status(self) -> None:
        """Check and update risk status."""
        # Check daily loss
        if self._metrics.daily_pnl < -self._limits.max_daily_loss:
            self._metrics.status = RiskStatus.KILL_SWITCH
            self._kill_switch_active = True
            log.error("KILL SWITCH: Daily loss limit breached", loss=self._metrics.daily_pnl)
            return

        # Check drawdown
        if self._metrics.drawdown > self._limits.max_drawdown:
            self._metrics.status = RiskStatus.KILL_SWITCH
            self._kill_switch_active = True
            log.error("KILL SWITCH: Drawdown limit breached", drawdown=self._metrics.drawdown)
            return

        # Check for warnings
        if self._metrics.daily_pnl < -self._limits.max_daily_loss * 0.8:
            self._metrics.status = RiskStatus.WARNING
            log.warning("Approaching daily loss limit", loss=self._metrics.daily_pnl)
        elif self._metrics.drawdown > self._limits.max_drawdown * 0.8:
            self._metrics.status = RiskStatus.WARNING
            log.warning("Approaching drawdown limit", drawdown=self._metrics.drawdown)
        else:
            self._metrics.status = RiskStatus.OK

    def trigger_kill_switch(self, reason: str) -> None:
        """Manually trigger kill switch."""
        self._kill_switch_active = True
        self._metrics.status = RiskStatus.KILL_SWITCH
        log.error("KILL SWITCH triggered", reason=reason)

    def reset_kill_switch(self) -> None:
        """Reset kill switch (use with caution)."""
        self._kill_switch_active = False
        self._metrics.status = RiskStatus.OK
        log.warning("Kill switch reset")

    def reset_daily_metrics(self) -> None:
        """Reset daily metrics (call at start of trading day)."""
        self._metrics.daily_pnl = 0.0
        self._metrics.peak_pnl = 0.0
        self._metrics.drawdown = 0.0
        self._metrics.orders_this_second = 0

        for pos in self._positions.values():
            pos.realized_pnl = 0.0

        log.info("Daily metrics reset")

    def increment_order_count(self) -> None:
        """Increment order count for rate limiting."""
        self._metrics.orders_this_second += 1

    def reset_order_count(self) -> None:
        """Reset order count (call every second)."""
        self._metrics.orders_this_second = 0
