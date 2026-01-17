#!/usr/bin/env python3
"""
Crypto HFT Market Making Bot - Single Exchange (Python)
"""

import asyncio
import signal
import sys
from typing import Optional

import structlog

from .core import Symbol, Tick, Order, from_price, from_qty, now_ns
from .orderbook import OrderBook
from .strategy import BasicMarketMaker, MarketMakerParams, Signal
from .risk import RiskManager, RiskLimits
from .exchange import BinanceClient, BinanceConfig
from .exchange.binance import ExchangeCallbacks

# Configure structured logging
structlog.configure(
    processors=[
        structlog.stdlib.filter_by_level,
        structlog.stdlib.add_logger_name,
        structlog.stdlib.add_log_level,
        structlog.stdlib.PositionalArgumentsFormatter(),
        structlog.processors.TimeStamper(fmt="iso"),
        structlog.processors.StackInfoRenderer(),
        structlog.processors.format_exc_info,
        structlog.dev.ConsoleRenderer()
    ],
    wrapper_class=structlog.stdlib.BoundLogger,
    context_class=dict,
    logger_factory=structlog.PrintLoggerFactory(),
    cache_logger_on_first_use=True,
)

log = structlog.get_logger()


class TradingEngine:
    """Main trading engine orchestrating all components."""

    def __init__(
        self,
        symbol: str,
        config: BinanceConfig,
        strategy_params: MarketMakerParams,
        risk_limits: RiskLimits,
    ):
        self.symbol = Symbol(symbol)
        self.orderbook = OrderBook(self.symbol)
        self.strategy = BasicMarketMaker(strategy_params)
        self.risk_manager = RiskManager(risk_limits)
        self.exchange = BinanceClient(config)

        self._running = False
        self._trading_enabled = False
        self._ticks_processed = 0
        self._orders_sent = 0

    async def start(self) -> None:
        """Start the trading engine."""
        log.info("Starting trading engine", symbol=str(self.symbol))

        # Setup callbacks
        self.exchange.set_callbacks(ExchangeCallbacks(
            on_tick=self._on_tick,
            on_order_update=self._on_order_update,
            on_error=self._on_error,
            on_connected=self._on_connected,
            on_disconnected=self._on_disconnected,
        ))

        # Connect to exchange
        await self.exchange.connect()

        # Subscribe to market data
        await self.exchange.subscribe_ticker(self.symbol)
        await self.exchange.subscribe_orderbook(self.symbol)

        self._running = True
        self.strategy.enabled = True

        log.info("Trading engine started")

    async def stop(self) -> None:
        """Stop the trading engine."""
        log.info("Stopping trading engine")

        self._running = False
        self._trading_enabled = False
        self.strategy.enabled = False

        # Cancel all orders
        await self.exchange.cancel_all_orders(self.symbol)

        # Disconnect
        await self.exchange.disconnect()

        log.info(
            "Trading engine stopped",
            ticks_processed=self._ticks_processed,
            orders_sent=self._orders_sent,
            quotes_sent=self.strategy.quotes_sent,
            fills=self.strategy.fills,
        )

    def _on_tick(self, tick: Tick) -> None:
        """Handle market data tick."""
        self._ticks_processed += 1

        # Update orderbook
        self.orderbook.update_bid(tick.bid, tick.bid_qty)
        self.orderbook.update_ask(tick.ask, tick.ask_qty)

        # Run strategy
        if self._trading_enabled and self.strategy.enabled:
            position = self.risk_manager.position

            signal = Signal(
                fair_value=from_price(self.orderbook.mid_price or 0),
                timestamp=now_ns(),
            )

            decision = self.strategy.compute_quotes(self.orderbook, position, signal)

            if decision.should_quote:
                log.debug(
                    "Quote decision",
                    bid=from_price(decision.bid_price),
                    ask=from_price(decision.ask_price),
                )
                # Would send orders here in production

    def _on_order_update(self, order: Order) -> None:
        """Handle order update."""
        log.info(
            "Order update",
            order_id=order.id,
            status=order.status.name,
            filled=from_qty(order.filled_qty),
        )

    def _on_error(self, error: str) -> None:
        """Handle error."""
        log.error("Exchange error", error=error)

    def _on_connected(self) -> None:
        """Handle connection."""
        log.info("Connected to exchange")
        self._trading_enabled = True

    def _on_disconnected(self) -> None:
        """Handle disconnection."""
        log.warning("Disconnected from exchange")
        self._trading_enabled = False

    async def run(self) -> None:
        """Main run loop."""
        await self.start()

        try:
            while self._running:
                await asyncio.sleep(0.1)

                # Print periodic stats
                if self._ticks_processed > 0 and self._ticks_processed % 100 == 0:
                    mid = self.orderbook.mid_price
                    spread_bps = self.orderbook.spread_bps
                    log.info(
                        "Stats",
                        ticks=self._ticks_processed,
                        mid_price=from_price(mid) if mid else None,
                        spread_bps=f"{spread_bps:.2f}" if spread_bps else None,
                        quotes=self.strategy.quotes_sent,
                        fills=self.strategy.fills,
                    )
        finally:
            await self.stop()


def main():
    """Main entry point."""
    print("""
╔═══════════════════════════════════════════════════════════════╗
║          Crypto HFT Market Making System v1.0.0               ║
║          High-Frequency Trading Bot - Single Exchange         ║
║                      (Python Edition)                         ║
╚═══════════════════════════════════════════════════════════════╝
""")

    # Configuration
    config = BinanceConfig(
        api_key="",  # Set via environment
        api_secret="",
        testnet=True,
    )

    strategy_params = MarketMakerParams.default()
    risk_limits = RiskLimits.default()

    # Create engine
    engine = TradingEngine(
        symbol="BTCUSDT",
        config=config,
        strategy_params=strategy_params,
        risk_limits=risk_limits,
    )

    # Handle shutdown
    loop = asyncio.get_event_loop()

    def shutdown_handler():
        log.info("Shutdown signal received")
        loop.create_task(engine.stop())

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, shutdown_handler)

    # Run
    try:
        loop.run_until_complete(engine.run())
    except KeyboardInterrupt:
        pass
    finally:
        loop.close()


if __name__ == "__main__":
    main()
