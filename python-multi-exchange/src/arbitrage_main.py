#!/usr/bin/env python3
"""
Crypto HFT Arbitrage Bot - Multi Exchange (Python)
Standalone arbitrage-only bot.
"""

import asyncio
import signal
from typing import Optional

import structlog

from .core import ExchangeId, Symbol, Tick, from_price, now_ns
from .orderbook import ConsolidatedBook
from .arbitrage import ArbitrageDetector, ArbitrageExecutor, ArbitrageConfig
from .risk import RiskManager, RiskLimits
from .exchange import (
    ExchangeManager,
    BinanceClient, BinanceConfig,
    BybitClient, BybitConfig,
    OKXClient, OKXConfig,
)
from .exchange.base import ExchangeCallbacks

# Configure logging
structlog.configure(
    processors=[
        structlog.stdlib.filter_by_level,
        structlog.stdlib.add_log_level,
        structlog.processors.TimeStamper(fmt="iso"),
        structlog.dev.ConsoleRenderer()
    ],
    wrapper_class=structlog.stdlib.BoundLogger,
    context_class=dict,
    logger_factory=structlog.PrintLoggerFactory(),
    cache_logger_on_first_use=True,
)

log = structlog.get_logger()


class ArbitrageBot:
    """Standalone arbitrage bot."""

    def __init__(
        self,
        symbol: str,
        arb_config: ArbitrageConfig,
        risk_limits: RiskLimits,
    ):
        self.symbol = Symbol.from_str(symbol)
        self.exchange_manager = ExchangeManager()
        self.consolidated_book = ConsolidatedBook(self.symbol)
        self.arb_detector = ArbitrageDetector(arb_config)
        self.arb_executor = ArbitrageExecutor(self.exchange_manager, arb_config)
        self.risk_manager = RiskManager(risk_limits)

        self._running = False
        self._ticks_processed = 0

    def add_exchange(
        self,
        exchange_id: ExchangeId,
        api_key: str = "",
        api_secret: str = "",
        passphrase: str = "",
        testnet: bool = True,
    ) -> None:
        """Add an exchange."""
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

    async def start(self) -> None:
        """Start the arbitrage bot."""
        log.info("Starting arbitrage bot", symbol=str(self.symbol))

        self.exchange_manager.set_callbacks(ExchangeCallbacks(
            on_tick=self._on_tick,
            on_error=self._on_error,
            on_connected=self._on_connected,
            on_disconnected=self._on_disconnected,
        ))

        await self.exchange_manager.connect_all()
        await self.exchange_manager.subscribe_ticker_all(self.symbol)
        await self.exchange_manager.subscribe_orderbook_all(self.symbol)

        self._running = True
        log.info("Arbitrage bot started")

    async def stop(self) -> None:
        """Stop the arbitrage bot."""
        log.info("Stopping arbitrage bot")
        self._running = False

        await self.exchange_manager.cancel_all_orders_all_exchanges(self.symbol)
        await self.exchange_manager.disconnect_all()

        log.info(
            "Arbitrage bot stopped",
            opportunities_detected=self.arb_detector.stats.opportunities_detected,
            opportunities_executed=self.arb_executor.stats.opportunities_executed,
            total_profit_bps=f"{self.arb_executor.stats.total_profit_bps:.2f}",
        )

    def _on_tick(self, exchange: ExchangeId, tick: Tick) -> None:
        """Handle tick."""
        self._ticks_processed += 1
        self.consolidated_book.update(exchange, tick)

        # Check for arbitrage
        if not self.risk_manager.is_kill_switch_active:
            arb_opp = self.arb_detector.check(self.consolidated_book)
            if arb_opp and not self.arb_executor.is_executing:
                asyncio.create_task(self.arb_executor.execute(arb_opp))

    def _on_error(self, exchange: ExchangeId, error: str) -> None:
        log.error("Exchange error", exchange=str(exchange), error=error)

    def _on_connected(self, exchange: ExchangeId) -> None:
        log.info("Exchange connected", exchange=str(exchange))

    def _on_disconnected(self, exchange: ExchangeId) -> None:
        log.warning("Exchange disconnected", exchange=str(exchange))

    async def run(self) -> None:
        """Main run loop."""
        await self.start()

        try:
            while self._running:
                await asyncio.sleep(1)

                nbbo = self.consolidated_book.nbbo
                if nbbo.is_crossed:
                    log.info(
                        "CROSSED MARKET",
                        bid=from_price(nbbo.best_bid),
                        ask=from_price(nbbo.best_ask),
                        bid_exchange=str(nbbo.best_bid_exchange),
                        ask_exchange=str(nbbo.best_ask_exchange),
                    )
        finally:
            await self.stop()


def main():
    """Main entry point."""
    print("""
╔═══════════════════════════════════════════════════════════════╗
║       Crypto HFT Arbitrage Bot v1.0.0                         ║
║       Cross-Exchange Arbitrage Scanner & Executor             ║
║                    (Python Edition)                           ║
╚═══════════════════════════════════════════════════════════════╝
""")

    arb_config = ArbitrageConfig(
        min_profit_bps=2.0,
        max_position_usd=5000.0,
        fee_bps=0.1,
    )
    risk_limits = RiskLimits.conservative()

    bot = ArbitrageBot(
        symbol="BTCUSDT",
        arb_config=arb_config,
        risk_limits=risk_limits,
    )

    bot.add_exchange(ExchangeId.BINANCE, testnet=True)
    bot.add_exchange(ExchangeId.BYBIT, testnet=True)
    bot.add_exchange(ExchangeId.OKX, testnet=True)

    loop = asyncio.get_event_loop()

    def shutdown_handler():
        log.info("Shutdown signal received")
        loop.create_task(bot.stop())

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, shutdown_handler)

    try:
        loop.run_until_complete(bot.run())
    except KeyboardInterrupt:
        pass
    finally:
        loop.close()


if __name__ == "__main__":
    main()
