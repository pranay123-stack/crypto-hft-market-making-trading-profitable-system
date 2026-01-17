"""Arbitrage detection and execution."""

import asyncio
from dataclasses import dataclass, field
from typing import Optional, List, Dict, Callable
import structlog

from ..core import (
    ExchangeId, Symbol, ArbitrageOpportunity, Side, OrderType,
    TimeInForce, from_price, from_qty, now_ns
)
from ..orderbook import ConsolidatedBook
from ..exchange import ExchangeManager
from ..exchange.base import OrderRequest

log = structlog.get_logger()


@dataclass
class ArbitrageConfig:
    """Arbitrage detector configuration."""
    min_profit_bps: float = 2.0  # Minimum profit in basis points
    max_position_usd: float = 10000.0  # Maximum position size in USD
    min_quantity: float = 0.001  # Minimum trade quantity
    execution_timeout_ms: int = 1000  # Timeout for execution
    fee_bps: float = 0.2  # Assumed fee per side in basis points


@dataclass
class ArbitrageStats:
    """Arbitrage statistics."""
    opportunities_detected: int = 0
    opportunities_executed: int = 0
    total_profit_bps: float = 0.0
    total_volume: float = 0.0
    failed_executions: int = 0


class ArbitrageDetector:
    """Detects cross-exchange arbitrage opportunities."""

    def __init__(self, config: ArbitrageConfig):
        self._config = config
        self._stats = ArbitrageStats()
        self._last_opportunity: Optional[ArbitrageOpportunity] = None
        self._on_opportunity: Optional[Callable[[ArbitrageOpportunity], None]] = None

    @property
    def stats(self) -> ArbitrageStats:
        return self._stats

    def set_opportunity_callback(
        self, callback: Callable[[ArbitrageOpportunity], None]
    ) -> None:
        """Set callback for when opportunity is detected."""
        self._on_opportunity = callback

    def check(self, book: ConsolidatedBook) -> Optional[ArbitrageOpportunity]:
        """Check for arbitrage opportunity.

        Returns opportunity if found and meets criteria.
        """
        # Account for fees: need profit > 2 * fee_bps
        min_profit = self._config.min_profit_bps + (2 * self._config.fee_bps)

        opportunity = book.detect_arbitrage(min_profit_bps=min_profit)

        if opportunity:
            self._stats.opportunities_detected += 1
            self._last_opportunity = opportunity

            log.info(
                "Arbitrage opportunity detected",
                symbol=str(opportunity.symbol),
                buy_exchange=str(opportunity.buy_exchange),
                sell_exchange=str(opportunity.sell_exchange),
                buy_price=from_price(opportunity.buy_price),
                sell_price=from_price(opportunity.sell_price),
                profit_bps=f"{opportunity.expected_profit_bps:.2f}",
            )

            if self._on_opportunity:
                self._on_opportunity(opportunity)

        return opportunity


class ArbitrageExecutor:
    """Executes cross-exchange arbitrage."""

    def __init__(
        self,
        exchange_manager: ExchangeManager,
        config: ArbitrageConfig,
    ):
        self._exchange_manager = exchange_manager
        self._config = config
        self._stats = ArbitrageStats()
        self._executing = False

    @property
    def stats(self) -> ArbitrageStats:
        return self._stats

    @property
    def is_executing(self) -> bool:
        return self._executing

    async def execute(self, opportunity: ArbitrageOpportunity) -> bool:
        """Execute arbitrage opportunity.

        Sends simultaneous buy and sell orders to capture the spread.
        Returns True if both orders were successfully placed.
        """
        if self._executing:
            log.warning("Already executing arbitrage, skipping")
            return False

        self._executing = True

        try:
            # Calculate quantity (respect limits)
            quantity = min(
                opportunity.quantity,
                int(self._config.max_position_usd / from_price(opportunity.buy_price) * 10**8),
            )

            if from_qty(quantity) < self._config.min_quantity:
                log.warning("Quantity too small for arbitrage", qty=from_qty(quantity))
                return False

            log.info(
                "Executing arbitrage",
                buy_exchange=str(opportunity.buy_exchange),
                sell_exchange=str(opportunity.sell_exchange),
                quantity=from_qty(quantity),
                expected_profit_bps=f"{opportunity.expected_profit_bps:.2f}",
            )

            # Create order requests
            buy_request = OrderRequest(
                symbol=opportunity.symbol,
                side=Side.BUY,
                order_type=OrderType.LIMIT,
                price=opportunity.buy_price,
                quantity=quantity,
                tif=TimeInForce.IOC,  # Immediate or cancel
            )

            sell_request = OrderRequest(
                symbol=opportunity.symbol,
                side=Side.SELL,
                order_type=OrderType.LIMIT,
                price=opportunity.sell_price,
                quantity=quantity,
                tif=TimeInForce.IOC,
            )

            # Execute both legs simultaneously
            buy_task = self._exchange_manager.send_order(
                opportunity.buy_exchange, buy_request
            )
            sell_task = self._exchange_manager.send_order(
                opportunity.sell_exchange, sell_request
            )

            try:
                buy_result, sell_result = await asyncio.wait_for(
                    asyncio.gather(buy_task, sell_task),
                    timeout=self._config.execution_timeout_ms / 1000,
                )
            except asyncio.TimeoutError:
                log.error("Arbitrage execution timeout")
                self._stats.failed_executions += 1
                return False

            # Check results
            if buy_result.success and sell_result.success:
                self._stats.opportunities_executed += 1
                self._stats.total_profit_bps += opportunity.expected_profit_bps
                self._stats.total_volume += from_qty(quantity) * 2

                log.info(
                    "Arbitrage executed successfully",
                    buy_order_id=buy_result.exchange_order_id,
                    sell_order_id=sell_result.exchange_order_id,
                )
                return True
            else:
                self._stats.failed_executions += 1

                if not buy_result.success:
                    log.error("Buy leg failed", error=buy_result.error_message)
                if not sell_result.success:
                    log.error("Sell leg failed", error=sell_result.error_message)

                # Cancel any successful leg
                if buy_result.success:
                    await self._exchange_manager.cancel_order(
                        opportunity.buy_exchange,
                        opportunity.symbol,
                        buy_result.exchange_order_id,
                    )
                if sell_result.success:
                    await self._exchange_manager.cancel_order(
                        opportunity.sell_exchange,
                        opportunity.symbol,
                        sell_result.exchange_order_id,
                    )

                return False

        finally:
            self._executing = False
