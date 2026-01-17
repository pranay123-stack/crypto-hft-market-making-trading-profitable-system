//! Risk management module

use crate::core::types::*;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

/// Risk limits configuration
#[derive(Debug, Clone)]
pub struct RiskLimits {
    pub max_position_qty: Quantity,
    pub max_position_value: f64,
    pub max_order_qty: Quantity,
    pub max_order_value: f64,
    pub max_orders_per_second: u32,
    pub max_open_orders: u32,
    pub max_daily_loss: f64,
    pub max_drawdown: f64,
    pub max_deviation_bps: f64,
    pub kill_switch_enabled: bool,
}

impl Default for RiskLimits {
    fn default() -> Self {
        RiskLimits {
            max_position_qty: to_qty(1.0),
            max_position_value: 100000.0,
            max_order_qty: to_qty(0.1),
            max_order_value: 10000.0,
            max_orders_per_second: 10,
            max_open_orders: 100,
            max_daily_loss: 1000.0,
            max_drawdown: 2000.0,
            max_deviation_bps: 100.0,
            kill_switch_enabled: true,
        }
    }
}

/// Risk check result
#[derive(Debug, Clone)]
pub struct RiskCheckResult {
    pub passed: bool,
    pub violation: Option<RiskViolation>,
    pub message: String,
}

impl RiskCheckResult {
    pub fn pass() -> Self {
        RiskCheckResult {
            passed: true,
            violation: None,
            message: String::new(),
        }
    }

    pub fn fail(violation: RiskViolation, message: impl Into<String>) -> Self {
        RiskCheckResult {
            passed: false,
            violation: Some(violation),
            message: message.into(),
        }
    }
}

/// Types of risk violations
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RiskViolation {
    PositionLimit,
    OrderSizeLimit,
    OrderValueLimit,
    RateLimit,
    OpenOrdersLimit,
    DailyLossLimit,
    DrawdownLimit,
    PriceDeviation,
    KillSwitchActive,
}

/// Position tracking
#[derive(Debug, Clone, Default)]
pub struct Position {
    pub symbol: Symbol,
    pub quantity: Quantity,
    pub avg_price: Price,
    pub unrealized_pnl: f64,
    pub realized_pnl: f64,
    pub last_update: Timestamp,
}

impl Position {
    pub fn notional_value(&self, current_price: Price) -> f64 {
        from_qty(self.quantity.abs()) * from_price(current_price)
    }

    pub fn is_long(&self) -> bool {
        self.quantity > 0
    }

    pub fn is_short(&self) -> bool {
        self.quantity < 0
    }

    pub fn is_flat(&self) -> bool {
        self.quantity == 0
    }
}

/// Risk manager
pub struct RiskManager {
    limits: RiskLimits,
    position: Position,
    open_orders: HashMap<OrderId, Order>,

    // Rate limiting
    orders_this_second: AtomicU64,
    current_second: AtomicU64,

    // P&L tracking
    daily_realized_pnl: f64,
    peak_equity: f64,

    // Kill switch
    kill_switch_active: AtomicBool,

    // Statistics
    orders_checked: AtomicU64,
    orders_rejected: AtomicU64,
}

impl RiskManager {
    pub fn new(limits: RiskLimits) -> Self {
        RiskManager {
            limits,
            position: Position::default(),
            open_orders: HashMap::new(),
            orders_this_second: AtomicU64::new(0),
            current_second: AtomicU64::new(0),
            daily_realized_pnl: 0.0,
            peak_equity: 0.0,
            kill_switch_active: AtomicBool::new(false),
            orders_checked: AtomicU64::new(0),
            orders_rejected: AtomicU64::new(0),
        }
    }

    /// Check if order passes risk checks
    pub fn check_order(&self, order: &Order) -> RiskCheckResult {
        self.orders_checked.fetch_add(1, Ordering::Relaxed);

        // Check kill switch
        if self.kill_switch_active.load(Ordering::Relaxed) {
            self.orders_rejected.fetch_add(1, Ordering::Relaxed);
            return RiskCheckResult::fail(
                RiskViolation::KillSwitchActive,
                "Kill switch is active",
            );
        }

        // Check position limit
        if let Some(result) = self.check_position_limit(order) {
            self.orders_rejected.fetch_add(1, Ordering::Relaxed);
            return result;
        }

        // Check order size
        if let Some(result) = self.check_order_size(order) {
            self.orders_rejected.fetch_add(1, Ordering::Relaxed);
            return result;
        }

        // Check rate limit
        if let Some(result) = self.check_rate_limit() {
            self.orders_rejected.fetch_add(1, Ordering::Relaxed);
            return result;
        }

        // Check open orders
        if let Some(result) = self.check_open_orders() {
            self.orders_rejected.fetch_add(1, Ordering::Relaxed);
            return result;
        }

        // Check daily loss
        if let Some(result) = self.check_daily_loss() {
            self.orders_rejected.fetch_add(1, Ordering::Relaxed);
            return result;
        }

        RiskCheckResult::pass()
    }

    fn check_position_limit(&self, order: &Order) -> Option<RiskCheckResult> {
        if self.limits.max_position_qty == 0 {
            return None;
        }

        let potential_pos = match order.side {
            Side::Buy => self.position.quantity + order.quantity,
            Side::Sell => self.position.quantity - order.quantity,
        };

        if potential_pos.abs() > self.limits.max_position_qty {
            return Some(RiskCheckResult::fail(
                RiskViolation::PositionLimit,
                format!(
                    "Position limit exceeded: potential={} max={}",
                    from_qty(potential_pos),
                    from_qty(self.limits.max_position_qty)
                ),
            ));
        }

        None
    }

    fn check_order_size(&self, order: &Order) -> Option<RiskCheckResult> {
        if self.limits.max_order_qty > 0 && order.quantity > self.limits.max_order_qty {
            return Some(RiskCheckResult::fail(
                RiskViolation::OrderSizeLimit,
                format!(
                    "Order size exceeds limit: qty={} max={}",
                    from_qty(order.quantity),
                    from_qty(self.limits.max_order_qty)
                ),
            ));
        }

        if self.limits.max_order_value > 0.0 {
            let value = from_qty(order.quantity) * from_price(order.price);
            if value > self.limits.max_order_value {
                return Some(RiskCheckResult::fail(
                    RiskViolation::OrderValueLimit,
                    format!(
                        "Order value exceeds limit: value={:.2} max={:.2}",
                        value, self.limits.max_order_value
                    ),
                ));
            }
        }

        None
    }

    fn check_rate_limit(&self) -> Option<RiskCheckResult> {
        if self.limits.max_orders_per_second == 0 {
            return None;
        }

        let now = now_millis() / 1000;
        let prev = self.current_second.load(Ordering::Relaxed);

        if now != prev {
            if self.current_second.compare_exchange(
                prev, now, Ordering::Relaxed, Ordering::Relaxed
            ).is_ok() {
                self.orders_this_second.store(0, Ordering::Relaxed);
            }
        }

        let count = self.orders_this_second.fetch_add(1, Ordering::Relaxed);

        if count >= self.limits.max_orders_per_second as u64 {
            return Some(RiskCheckResult::fail(
                RiskViolation::RateLimit,
                format!("Rate limit exceeded: {} orders/second", count),
            ));
        }

        None
    }

    fn check_open_orders(&self) -> Option<RiskCheckResult> {
        if self.limits.max_open_orders == 0 {
            return None;
        }

        if self.open_orders.len() >= self.limits.max_open_orders as usize {
            return Some(RiskCheckResult::fail(
                RiskViolation::OpenOrdersLimit,
                format!("Open orders limit reached: {}", self.open_orders.len()),
            ));
        }

        None
    }

    fn check_daily_loss(&self) -> Option<RiskCheckResult> {
        if self.limits.max_daily_loss == 0.0 {
            return None;
        }

        let daily_loss = -self.daily_realized_pnl;

        if daily_loss >= self.limits.max_daily_loss {
            self.activate_kill_switch("Daily loss limit reached");
            return Some(RiskCheckResult::fail(
                RiskViolation::DailyLossLimit,
                format!("Daily loss limit reached: {:.2}", daily_loss),
            ));
        }

        None
    }

    /// Update position after fill
    pub fn on_fill(&mut self, order: &Order, filled_qty: Quantity, fill_price: Price) {
        let old_qty = self.position.quantity;

        match order.side {
            Side::Buy => {
                if self.position.quantity >= 0 {
                    // Adding to long
                    let new_qty = self.position.quantity + filled_qty;
                    if new_qty > 0 {
                        self.position.avg_price =
                            (self.position.avg_price * old_qty + fill_price * filled_qty) / new_qty;
                    }
                    self.position.quantity = new_qty;
                } else {
                    // Covering short
                    let covered = filled_qty.min(-self.position.quantity);
                    let pnl = from_qty(covered) *
                        (from_price(self.position.avg_price) - from_price(fill_price));
                    self.position.realized_pnl += pnl;
                    self.daily_realized_pnl += pnl;

                    self.position.quantity += filled_qty;
                    if self.position.quantity > 0 {
                        self.position.avg_price = fill_price;
                    }
                }
            }
            Side::Sell => {
                if self.position.quantity <= 0 {
                    // Adding to short
                    let new_qty = self.position.quantity - filled_qty;
                    if new_qty < 0 {
                        self.position.avg_price =
                            (self.position.avg_price * (-old_qty) + fill_price * filled_qty) / (-new_qty);
                    }
                    self.position.quantity = new_qty;
                } else {
                    // Closing long
                    let closed = filled_qty.min(self.position.quantity);
                    let pnl = from_qty(closed) *
                        (from_price(fill_price) - from_price(self.position.avg_price));
                    self.position.realized_pnl += pnl;
                    self.daily_realized_pnl += pnl;

                    self.position.quantity -= filled_qty;
                    if self.position.quantity < 0 {
                        self.position.avg_price = fill_price;
                    }
                }
            }
        }

        self.position.last_update = now_nanos();

        // Check drawdown
        let equity = self.daily_realized_pnl + self.position.unrealized_pnl;
        if equity > self.peak_equity {
            self.peak_equity = equity;
        } else if self.limits.max_drawdown > 0.0 {
            let drawdown = self.peak_equity - equity;
            if drawdown > self.limits.max_drawdown {
                self.activate_kill_switch("Drawdown limit exceeded");
            }
        }
    }

    /// Track sent order
    pub fn on_order_sent(&mut self, order: Order) {
        self.open_orders.insert(order.id, order);
    }

    /// Handle order cancel
    pub fn on_order_canceled(&mut self, order_id: OrderId) {
        self.open_orders.remove(&order_id);
    }

    /// Get current position
    pub fn get_position(&self) -> Quantity {
        self.position.quantity
    }

    /// Activate kill switch
    pub fn activate_kill_switch(&self, _reason: &str) {
        self.kill_switch_active.store(true, Ordering::Relaxed);
    }

    /// Deactivate kill switch
    pub fn deactivate_kill_switch(&self) {
        self.kill_switch_active.store(false, Ordering::Relaxed);
    }

    /// Check if kill switch is active
    pub fn is_kill_switch_active(&self) -> bool {
        self.kill_switch_active.load(Ordering::Relaxed)
    }

    /// Reset daily statistics
    pub fn reset_daily_stats(&mut self) {
        self.daily_realized_pnl = 0.0;
        self.peak_equity = self.position.unrealized_pnl;
    }

    /// Get orders checked count
    pub fn orders_checked(&self) -> u64 {
        self.orders_checked.load(Ordering::Relaxed)
    }

    /// Get orders rejected count
    pub fn orders_rejected(&self) -> u64 {
        self.orders_rejected.load(Ordering::Relaxed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_risk_check_pass() {
        let limits = RiskLimits::default();
        let rm = RiskManager::new(limits);

        let order = Order::new(
            Symbol::new("BTCUSDT"),
            Side::Buy,
            OrderType::Limit,
            to_price(50000.0),
            to_qty(0.01),
        );

        let result = rm.check_order(&order);
        assert!(result.passed);
    }

    #[test]
    fn test_position_limit() {
        let mut limits = RiskLimits::default();
        limits.max_position_qty = to_qty(0.1);
        let rm = RiskManager::new(limits);

        let order = Order::new(
            Symbol::new("BTCUSDT"),
            Side::Buy,
            OrderType::Limit,
            to_price(50000.0),
            to_qty(0.2),  // Exceeds limit
        );

        let result = rm.check_order(&order);
        assert!(!result.passed);
        assert_eq!(result.violation, Some(RiskViolation::PositionLimit));
    }
}
