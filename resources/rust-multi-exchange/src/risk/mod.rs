//! Cross-exchange risk management

use crate::core::types::*;
use hashbrown::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};

/// Cross-exchange risk limits
#[derive(Debug, Clone)]
pub struct CrossExchangeRiskLimits {
    pub max_position_per_exchange: Quantity,
    pub max_total_position: Quantity,
    pub max_daily_loss: f64,
    pub max_drawdown: f64,
    pub kill_switch_enabled: bool,
}

impl Default for CrossExchangeRiskLimits {
    fn default() -> Self {
        CrossExchangeRiskLimits {
            max_position_per_exchange: to_qty(0.1),
            max_total_position: to_qty(0.2),
            max_daily_loss: 500.0,
            max_drawdown: 1000.0,
            kill_switch_enabled: true,
        }
    }
}

/// Cross-exchange risk manager
pub struct RiskManager {
    limits: CrossExchangeRiskLimits,
    positions: HashMap<ExchangeId, Quantity>,
    pnl_by_exchange: HashMap<ExchangeId, f64>,
    daily_pnl: f64,
    kill_switch: AtomicBool,
}

impl RiskManager {
    pub fn new(limits: CrossExchangeRiskLimits) -> Self {
        RiskManager {
            limits,
            positions: HashMap::new(),
            pnl_by_exchange: HashMap::new(),
            daily_pnl: 0.0,
            kill_switch: AtomicBool::new(false),
        }
    }

    /// Check if order is allowed
    pub fn check_order(&self, exchange: ExchangeId, side: Side, qty: Quantity) -> bool {
        if self.kill_switch.load(Ordering::Relaxed) {
            return false;
        }

        let current_pos = *self.positions.get(&exchange).unwrap_or(&0);
        let new_pos = match side {
            Side::Buy => current_pos + qty,
            Side::Sell => current_pos - qty,
        };

        // Check per-exchange limit
        if new_pos.abs() > self.limits.max_position_per_exchange {
            return false;
        }

        // Check total position limit
        let total: Quantity = self.positions.values().sum();
        let new_total = match side {
            Side::Buy => total + qty,
            Side::Sell => total - qty,
        };

        if new_total.abs() > self.limits.max_total_position {
            return false;
        }

        true
    }

    /// Update position after fill
    pub fn on_fill(&mut self, exchange: ExchangeId, side: Side, qty: Quantity, pnl: f64) {
        let pos = self.positions.entry(exchange).or_insert(0);
        *pos = match side {
            Side::Buy => *pos + qty,
            Side::Sell => *pos - qty,
        };

        *self.pnl_by_exchange.entry(exchange).or_insert(0.0) += pnl;
        self.daily_pnl += pnl;

        // Check loss limits
        if self.limits.kill_switch_enabled && self.daily_pnl < -self.limits.max_daily_loss {
            self.activate_kill_switch("Daily loss limit exceeded");
        }
    }

    pub fn activate_kill_switch(&self, reason: &str) {
        tracing::error!("KILL SWITCH ACTIVATED: {}", reason);
        self.kill_switch.store(true, Ordering::Relaxed);
    }

    pub fn deactivate_kill_switch(&self) {
        self.kill_switch.store(false, Ordering::Relaxed);
    }

    pub fn is_kill_switch_active(&self) -> bool {
        self.kill_switch.load(Ordering::Relaxed)
    }

    pub fn get_position(&self, exchange: ExchangeId) -> Quantity {
        *self.positions.get(&exchange).unwrap_or(&0)
    }

    pub fn get_total_position(&self) -> Quantity {
        self.positions.values().sum()
    }

    pub fn get_daily_pnl(&self) -> f64 {
        self.daily_pnl
    }

    pub fn reset_daily(&mut self) {
        self.daily_pnl = 0.0;
        self.pnl_by_exchange.clear();
    }
}
