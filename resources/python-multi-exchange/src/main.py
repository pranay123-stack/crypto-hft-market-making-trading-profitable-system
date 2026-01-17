#!/usr/bin/env python3
"""
Crypto HFT Market Making Bot - Multi Exchange (Python)
"""

import asyncio
import signal
import sys
from typing import Optional

import structlog

from .core import ExchangeId, Symbol, Tick, Order, from_price, from_qty, now_ns
from .orderbook import ConsolidatedBook
from .strategy import CrossExchangeMM, CrossExchangeMMParams
from .arbitrage import ArbitrageDetector, ArbitrageExecutor, ArbitrageConfig
from .risk import RiskManager, RiskLimits
from .exchange import (
    ExchangeManager,
    BinanceClient, BinanceConfig,
    BybitClient, BybitConfig,
    OKXClient, OKXConfig,
)
from .exchange.base import ExchangeCallbacks

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


class MultiExchangeEngine:
    """Main multi-exchange trading engine."""

    def __init__(
        self,
        symbol: str,
        strategy_params: CrossExchangeMMParams,
        arb_config: ArbitrageConfig,
        risk_limits: RiskLimits,
    ):
        self.symbol = Symbol.from_str(symbol)
        self.exchange_manager = ExchangeManager()
        self.consolidated_book = ConsolidatedBook(self.symbol)
        self.strategy = CrossExchangeMM(strategy_params)
        self.arb_detector = ArbitrageDetector(arb_config)
        self.arb_executor = ArbitrageExecutor(self.exchange_manager, arb_config)
        self.risk_manager = RiskManager(risk_limits)

        self._running = False
        self._trading_enabled = False
        self._ticks_processed = 0

    def add_exchange(
        self,
        exchange_id: ExchangeId,
        api_key: str = "",
        api_secret: str = "",
        passphrase: str = "",
        testnet: bool = True,
    ) -> None:
        """Add an exchange to the engine."""
        if exchange_id == ExchangeId.BINANCE:
            config = BinanceConfig(api_key=api_key, api_secret=api_secret, testnet=testnet)
            client = BinanceClient(config)
        elif exchange_id == ExchangeId.BYBIT:
            config = BybitConfig(api_key=api_key, api_secret=api_secret, testnet=testnet)
            client = BybitClient(config)
        elif exchange_id == ExchangeId.OKX:
            config = OKXConfig(
                api_key=api_key, api_secret=api_secret,
                passphrase=passphrase, testnet=testnet
            )
            client = OKXClient(config)
        else:
            raise ValueError(f"Unsupported exchange: {exchange_id}")

        self.exchange_manager.register_exchange(client)
        self.consolidated_book.add_exchange(exchange_id)
        log.info("Added exchange", exchange=str(exchange_id))

    async def start(self) -> None:
        """Start the trading engine."""
        log.info("Starting multi-exchange trading engine", symbol=str(self.symbol))

        # Setup callbacks
        self.exchange_manager.set_callbacks(ExchangeCallbacks(
            on_tick=self._on_tick,
            on_order_update=self._on_order_update,
            on_error=self._on_error,
            on_connected=self._on_connected,
            on_disconnected=self._on_disconnected,
        ))

        # Connect to all exchanges
        await self.exchange_manager.connect_all()

        # Subscribe to market data on all exchanges
        await self.exchange_manager.subscribe_ticker_all(self.symbol)
        await self.exchange_manager.subscribe_orderbook_all(self.symbol)

        self._running = True
        self.strategy.enabled = True

        log.info(
            "Trading engine started",
            exchanges=len(self.exchange_manager.get_connected_exchanges()),
        )

    async def stop(self) -> None:
        """Stop the trading engine."""
        log.info("Stopping trading engine")

        self._running = False
        self._trading_enabled = False
        self.strategy.enabled = False

        # Cancel all orders on all exchanges
        cancelled = await self.exchange_manager.cancel_all_orders_all_exchanges(self.symbol)
        for exchange, count in cancelled.items():
            log.info("Cancelled orders", exchange=str(exchange), count=count)

        # Disconnect from all exchanges
        await self.exchange_manager.disconnect_all()

        log.info(
            "Trading engine stopped",
            ticks_processed=self._ticks_processed,
            quotes_sent=self.strategy.stats.quotes_sent,
            fills=self.strategy.stats.fills,
            arb_opportunities=self.arb_detector.stats.opportunities_detected,
            arb_executed=self.arb_executor.stats.opportunities_executed,
        )

    def _on_tick(self, exchange: ExchangeId, tick: Tick) -> None:
        """Handle market data tick from any exchange."""
        self._ticks_processed += 1

        # Update consolidated book
        self.consolidated_book.update(exchange, tick)

        # Update risk manager with mark price
        mid = self.consolidated_book.mid_price
        if mid:
            self.risk_manager.update_mark_price(exchange, from_price(mid))

        # Check for arbitrage
        if self._trading_enabled and not self.risk_manager.is_kill_switch_active:
            arb_opp = self.arb_detector.check(self.consolidated_book)
            if arb_opp and not self.arb_executor.is_executing:
                asyncio.create_task(self.arb_executor.execute(arb_opp))

        # Run market making strategy
        if self._trading_enabled and self.strategy.enabled:
            if not self.risk_manager.is_kill_switch_active:
                decision = self.strategy.compute_quotes(
                    self.consolidated_book,
                    self.exchange_manager,
                )
                if decision.should_quote:
                    asyncio.create_task(
                        self.strategy.send_quotes(
                            decision, self.exchange_manager, self.symbol
                        )
                    )

    def _on_order_update(self, exchange: ExchangeId, order: Order) -> None:
        """Handle order update from any exchange."""
        log.info(
            "Order update",
            exchange=str(exchange),
            order_id=order.id,
            status=order.status.name,
            filled=from_qty(order.filled_qty),
        )

        # Record fill in risk manager
        if order.filled_qty > 0:
            self.risk_manager.record_fill(
                exchange,
                order.side,
                from_qty(order.filled_qty),
                from_price(order.price),
            )

            # Notify strategy of fill
            self.strategy.on_fill(order.side, order.filled_qty, order.price)

            # Hedge if configured
            if self.strategy._params.hedge_on_fill:
                asyncio.create_task(
                    self.strategy.hedge_fill(
                        exchange, order.side, order.filled_qty, order.price,
                        self.exchange_manager, self.consolidated_book, self.symbol
                    )
                )

    def _on_error(self, exchange: ExchangeId, error: str) -> None:
        """Handle error from any exchange."""
        log.error("Exchange error", exchange=str(exchange), error=error)

    def _on_connected(self, exchange: ExchangeId) -> None:
        """Handle exchange connection."""
        log.info("Exchange connected", exchange=str(exchange))

        # Enable trading when at least 2 exchanges connected
        connected = len(self.exchange_manager.get_connected_exchanges())
        if connected >= 2:
            self._trading_enabled = True
            log.info("Trading enabled", connected_exchanges=connected)

    def _on_disconnected(self, exchange: ExchangeId) -> None:
        """Handle exchange disconnection."""
        log.warning("Exchange disconnected", exchange=str(exchange))

        # Disable trading if less than 2 exchanges
        connected = len(self.exchange_manager.get_connected_exchanges())
        if connected < 2:
            self._trading_enabled = False
            log.warning("Trading disabled, insufficient exchanges", connected=connected)

    async def run(self) -> None:
        """Main run loop."""
        await self.start()

        try:
            while self._running:
                await asyncio.sleep(0.1)

                # Reset rate limit counter every second
                self.risk_manager.reset_order_count()

                # Print periodic stats
                if self._ticks_processed > 0 and self._ticks_processed % 100 == 0:
                    nbbo = self.consolidated_book.nbbo
                    log.info(
                        "Stats",
                        ticks=self._ticks_processed,
                        exchanges=len(self.exchange_manager.get_connected_exchanges()),
                        nbbo_bid=from_price(nbbo.best_bid) if nbbo.best_bid else None,
                        nbbo_ask=from_price(nbbo.best_ask) if nbbo.best_ask else None,
                        spread_bps=f"{nbbo.spread_bps:.2f}" if nbbo.spread_bps else None,
                        position=f"{self.strategy.position:.4f}",
                        quotes=self.strategy.stats.quotes_sent,
                        fills=self.strategy.stats.fills,
                        arb_opps=self.arb_detector.stats.opportunities_detected,
                    )
        finally:
            await self.stop()


def main():
    """Main entry point."""
    print("""
╔═══════════════════════════════════════════════════════════════╗
║       Crypto HFT Market Making System v1.0.0                  ║
║       Multi-Exchange High-Frequency Trading Bot               ║
║                    (Python Edition)                           ║
╚═══════════════════════════════════════════════════════════════╝
""")

    # Configuration
    strategy_params = CrossExchangeMMParams.default()
    arb_config = ArbitrageConfig()
    risk_limits = RiskLimits.default()

    # Create engine
    engine = MultiExchangeEngine(
        symbol="BTCUSDT",
        strategy_params=strategy_params,
        arb_config=arb_config,
        risk_limits=risk_limits,
    )

    # Add exchanges (all in testnet mode)
    engine.add_exchange(ExchangeId.BINANCE, testnet=True)
    engine.add_exchange(ExchangeId.BYBIT, testnet=True)
    engine.add_exchange(ExchangeId.OKX, testnet=True)

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
