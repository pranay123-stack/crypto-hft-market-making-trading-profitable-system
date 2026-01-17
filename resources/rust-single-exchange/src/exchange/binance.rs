//! Binance exchange client implementation

use super::*;
use crate::core::types::*;
use async_trait::async_trait;
use futures_util::{SinkExt, StreamExt};
use hmac::{Hmac, Mac};
use reqwest::Client;
use sha2::Sha256;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tokio::sync::RwLock;
use tokio_tungstenite::{connect_async, tungstenite::Message};
use tracing::{debug, error, info, warn};

type HmacSha256 = Hmac<Sha256>;

/// Binance-specific configuration
#[derive(Debug, Clone)]
pub struct BinanceConfig {
    pub base: ExchangeConfig,
    pub use_futures: bool,
    pub recv_window: u64,
}

impl Default for BinanceConfig {
    fn default() -> Self {
        BinanceConfig {
            base: ExchangeConfig::default(),
            use_futures: false,
            recv_window: 5000,
        }
    }
}

impl BinanceConfig {
    pub fn testnet() -> Self {
        let mut config = Self::default();
        config.base.testnet = true;
        config.base.rest_url = "https://testnet.binance.vision".to_string();
        config.base.ws_url = "wss://testnet.binance.vision/ws".to_string();
        config
    }

    pub fn futures_testnet() -> Self {
        let mut config = Self::testnet();
        config.use_futures = true;
        config.base.rest_url = "https://testnet.binancefuture.com".to_string();
        config.base.ws_url = "wss://stream.binancefuture.com/ws".to_string();
        config
    }
}

/// Binance exchange client
pub struct BinanceClient {
    config: BinanceConfig,
    http_client: Client,
    callbacks: Arc<RwLock<ExchangeCallbacks>>,
    connected: Arc<AtomicBool>,
    ws_sender: Arc<RwLock<Option<futures_util::stream::SplitSink<
        tokio_tungstenite::WebSocketStream<
            tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>
        >,
        Message
    >>>>,
}

impl BinanceClient {
    pub fn new(config: BinanceConfig) -> Self {
        let http_client = Client::builder()
            .timeout(std::time::Duration::from_millis(config.base.connect_timeout_ms))
            .build()
            .expect("Failed to create HTTP client");

        BinanceClient {
            config,
            http_client,
            callbacks: Arc::new(RwLock::new(ExchangeCallbacks::default())),
            connected: Arc::new(AtomicBool::new(false)),
            ws_sender: Arc::new(RwLock::new(None)),
        }
    }

    fn sign(&self, query: &str) -> String {
        let mut mac = HmacSha256::new_from_slice(self.config.base.api_secret.as_bytes())
            .expect("HMAC can take key of any size");
        mac.update(query.as_bytes());
        hex::encode(mac.finalize().into_bytes())
    }

    async fn signed_request(
        &self,
        method: reqwest::Method,
        endpoint: &str,
        params: &[(&str, &str)],
    ) -> Result<serde_json::Value, ExchangeError> {
        let timestamp = now_millis().to_string();
        let mut query_params: Vec<(&str, &str)> = params.to_vec();
        query_params.push(("timestamp", &timestamp));
        query_params.push(("recvWindow", &self.config.recv_window.to_string()));

        let query_string: String = query_params
            .iter()
            .map(|(k, v)| format!("{}={}", k, v))
            .collect::<Vec<_>>()
            .join("&");

        let signature = self.sign(&query_string);
        let full_query = format!("{}&signature={}", query_string, signature);

        let url = format!("{}{}?{}", self.config.base.rest_url, endpoint, full_query);

        let response = self
            .http_client
            .request(method, &url)
            .header("X-MBX-APIKEY", &self.config.base.api_key)
            .send()
            .await
            .map_err(|e| ExchangeError::RequestFailed(e.to_string()))?;

        if !response.status().is_success() {
            let status = response.status();
            let body = response.text().await.unwrap_or_default();
            return Err(ExchangeError::RequestFailed(format!(
                "HTTP {}: {}",
                status, body
            )));
        }

        response
            .json()
            .await
            .map_err(|e| ExchangeError::ParseError(e.to_string()))
    }

    async fn public_request(&self, endpoint: &str) -> Result<serde_json::Value, ExchangeError> {
        let url = format!("{}{}", self.config.base.rest_url, endpoint);

        let response = self
            .http_client
            .get(&url)
            .send()
            .await
            .map_err(|e| ExchangeError::RequestFailed(e.to_string()))?;

        response
            .json()
            .await
            .map_err(|e| ExchangeError::ParseError(e.to_string()))
    }

    fn parse_ticker(data: &serde_json::Value) -> Option<Tick> {
        Some(Tick {
            bid: to_price(data["b"].as_str()?.parse().ok()?),
            ask: to_price(data["a"].as_str()?.parse().ok()?),
            bid_qty: to_qty(data["B"].as_str()?.parse().ok()?),
            ask_qty: to_qty(data["A"].as_str()?.parse().ok()?),
            last_price: to_price(data["c"].as_str().unwrap_or("0").parse().unwrap_or(0.0)),
            last_qty: 0,
            exchange_ts: data["E"].as_u64().unwrap_or(0) * 1_000_000,
            local_ts: now_nanos(),
            sequence: 0,
        })
    }

    fn parse_order_update(data: &serde_json::Value) -> Option<Order> {
        let status_str = data["X"].as_str()?;
        let status = match status_str {
            "NEW" => OrderStatus::New,
            "PARTIALLY_FILLED" => OrderStatus::PartiallyFilled,
            "FILLED" => OrderStatus::Filled,
            "CANCELED" => OrderStatus::Canceled,
            "REJECTED" => OrderStatus::Rejected,
            "EXPIRED" => OrderStatus::Expired,
            _ => return None,
        };

        let side_str = data["S"].as_str()?;
        let side = match side_str {
            "BUY" => Side::Buy,
            "SELL" => Side::Sell,
            _ => return None,
        };

        Some(Order {
            id: data["i"].as_u64()?,
            client_id: data["c"].as_str()?.parse().unwrap_or(0),
            symbol: Symbol::new(data["s"].as_str()?),
            side,
            order_type: OrderType::Limit,
            time_in_force: TimeInForce::Gtc,
            price: to_price(data["p"].as_str()?.parse().ok()?),
            quantity: to_qty(data["q"].as_str()?.parse().ok()?),
            filled_qty: to_qty(data["z"].as_str()?.parse().ok()?),
            status,
            timestamp: data["T"].as_u64()? * 1_000_000,
        })
    }
}

#[async_trait]
impl ExchangeClient for BinanceClient {
    fn name(&self) -> &str {
        "binance"
    }

    async fn connect(&mut self) -> Result<(), ExchangeError> {
        info!("Connecting to Binance WebSocket: {}", self.config.base.ws_url);

        let (ws_stream, _) = connect_async(&self.config.base.ws_url)
            .await
            .map_err(|e| ExchangeError::ConnectionFailed(e.to_string()))?;

        let (sender, mut receiver) = ws_stream.split();

        *self.ws_sender.write().await = Some(sender);
        self.connected.store(true, Ordering::Relaxed);

        // Spawn message handler
        let callbacks = self.callbacks.clone();
        let connected = self.connected.clone();

        tokio::spawn(async move {
            while let Some(msg) = receiver.next().await {
                match msg {
                    Ok(Message::Text(text)) => {
                        if let Ok(data) = serde_json::from_str::<serde_json::Value>(&text) {
                            let event_type = data["e"].as_str().unwrap_or("");

                            let cbs = callbacks.read().await;

                            match event_type {
                                "bookTicker" => {
                                    if let Some(tick) = Self::parse_ticker(&data) {
                                        if let Some(ref cb) = cbs.on_tick {
                                            cb(tick);
                                        }
                                    }
                                }
                                "executionReport" => {
                                    if let Some(order) = Self::parse_order_update(&data) {
                                        if let Some(ref cb) = cbs.on_order_update {
                                            cb(order);
                                        }
                                    }
                                }
                                _ => {
                                    debug!("Unknown event type: {}", event_type);
                                }
                            }
                        }
                    }
                    Ok(Message::Ping(data)) => {
                        debug!("Received ping");
                        // Pong is handled automatically by tungstenite
                    }
                    Ok(Message::Close(_)) => {
                        warn!("WebSocket closed");
                        connected.store(false, Ordering::Relaxed);
                        let cbs = callbacks.read().await;
                        if let Some(ref cb) = cbs.on_disconnected {
                            cb();
                        }
                        break;
                    }
                    Err(e) => {
                        error!("WebSocket error: {}", e);
                        let cbs = callbacks.read().await;
                        if let Some(ref cb) = cbs.on_error {
                            cb(e.to_string());
                        }
                    }
                    _ => {}
                }
            }
        });

        // Notify connected
        let cbs = self.callbacks.read().await;
        if let Some(ref cb) = cbs.on_connected {
            cb();
        }

        info!("Connected to Binance");
        Ok(())
    }

    async fn disconnect(&mut self) -> Result<(), ExchangeError> {
        self.connected.store(false, Ordering::Relaxed);

        if let Some(mut sender) = self.ws_sender.write().await.take() {
            let _ = sender.close().await;
        }

        info!("Disconnected from Binance");
        Ok(())
    }

    fn is_connected(&self) -> bool {
        self.connected.load(Ordering::Relaxed)
    }

    async fn subscribe_ticker(&mut self, symbol: &Symbol) -> Result<(), ExchangeError> {
        let stream = format!("{}@bookTicker", symbol.as_str().to_lowercase());
        let subscribe_msg = serde_json::json!({
            "method": "SUBSCRIBE",
            "params": [stream],
            "id": 1
        });

        if let Some(ref mut sender) = *self.ws_sender.write().await {
            sender
                .send(Message::Text(subscribe_msg.to_string()))
                .await
                .map_err(|e| ExchangeError::WebSocketError(e.to_string()))?;
        }

        info!("Subscribed to ticker: {}", symbol);
        Ok(())
    }

    async fn subscribe_orderbook(&mut self, symbol: &Symbol, depth: u32) -> Result<(), ExchangeError> {
        let stream = format!("{}@depth{}@100ms", symbol.as_str().to_lowercase(), depth);
        let subscribe_msg = serde_json::json!({
            "method": "SUBSCRIBE",
            "params": [stream],
            "id": 2
        });

        if let Some(ref mut sender) = *self.ws_sender.write().await {
            sender
                .send(Message::Text(subscribe_msg.to_string()))
                .await
                .map_err(|e| ExchangeError::WebSocketError(e.to_string()))?;
        }

        info!("Subscribed to orderbook: {} depth={}", symbol, depth);
        Ok(())
    }

    async fn subscribe_trades(&mut self, symbol: &Symbol) -> Result<(), ExchangeError> {
        let stream = format!("{}@trade", symbol.as_str().to_lowercase());
        let subscribe_msg = serde_json::json!({
            "method": "SUBSCRIBE",
            "params": [stream],
            "id": 3
        });

        if let Some(ref mut sender) = *self.ws_sender.write().await {
            sender
                .send(Message::Text(subscribe_msg.to_string()))
                .await
                .map_err(|e| ExchangeError::WebSocketError(e.to_string()))?;
        }

        info!("Subscribed to trades: {}", symbol);
        Ok(())
    }

    async fn send_order(&self, request: OrderRequest) -> Result<OrderResponse, ExchangeError> {
        let side = match request.side {
            Side::Buy => "BUY",
            Side::Sell => "SELL",
        };

        let order_type = match request.order_type {
            OrderType::Limit => "LIMIT",
            OrderType::Market => "MARKET",
            OrderType::LimitMaker => "LIMIT_MAKER",
            _ => "LIMIT",
        };

        let tif = match request.time_in_force {
            TimeInForce::Gtc => "GTC",
            TimeInForce::Ioc => "IOC",
            TimeInForce::Fok => "FOK",
            TimeInForce::Gtx => "GTX",
        };

        let price_str = from_price(request.price).to_string();
        let qty_str = from_qty(request.quantity).to_string();

        let params = vec![
            ("symbol", request.symbol.as_str()),
            ("side", side),
            ("type", order_type),
            ("timeInForce", tif),
            ("price", &price_str),
            ("quantity", &qty_str),
        ];

        let result = self
            .signed_request(reqwest::Method::POST, "/api/v3/order", &params)
            .await?;

        Ok(OrderResponse {
            success: true,
            order_id: result["orderId"].as_u64().unwrap_or(0),
            client_order_id: result["clientOrderId"].as_str().unwrap_or("").to_string(),
            status: OrderStatus::New,
            error_message: None,
        })
    }

    async fn cancel_order(&self, symbol: &Symbol, order_id: OrderId) -> Result<CancelResponse, ExchangeError> {
        let order_id_str = order_id.to_string();
        let params = vec![
            ("symbol", symbol.as_str()),
            ("orderId", &order_id_str),
        ];

        let _ = self
            .signed_request(reqwest::Method::DELETE, "/api/v3/order", &params)
            .await?;

        Ok(CancelResponse {
            success: true,
            order_id,
            error_message: None,
        })
    }

    async fn cancel_all_orders(&self, symbol: &Symbol) -> Result<(), ExchangeError> {
        let params = vec![("symbol", symbol.as_str())];

        self.signed_request(reqwest::Method::DELETE, "/api/v3/openOrders", &params)
            .await?;

        Ok(())
    }

    async fn get_balance(&self, asset: &str) -> Result<f64, ExchangeError> {
        let result = self
            .signed_request(reqwest::Method::GET, "/api/v3/account", &[])
            .await?;

        if let Some(balances) = result["balances"].as_array() {
            for balance in balances {
                if balance["asset"].as_str() == Some(asset) {
                    return balance["free"]
                        .as_str()
                        .and_then(|s| s.parse().ok())
                        .ok_or_else(|| ExchangeError::ParseError("Invalid balance".to_string()));
                }
            }
        }

        Ok(0.0)
    }

    async fn get_open_orders(&self, symbol: &Symbol) -> Result<Vec<Order>, ExchangeError> {
        let params = vec![("symbol", symbol.as_str())];

        let result = self
            .signed_request(reqwest::Method::GET, "/api/v3/openOrders", &params)
            .await?;

        let orders = result
            .as_array()
            .ok_or_else(|| ExchangeError::ParseError("Expected array".to_string()))?
            .iter()
            .filter_map(Self::parse_order_update)
            .collect();

        Ok(orders)
    }

    fn set_callbacks(&mut self, callbacks: ExchangeCallbacks) {
        let cbs = self.callbacks.clone();
        tokio::spawn(async move {
            *cbs.write().await = callbacks;
        });
    }

    async fn server_time(&self) -> Result<Timestamp, ExchangeError> {
        let result = self.public_request("/api/v3/time").await?;
        result["serverTime"]
            .as_u64()
            .map(|t| t * 1_000_000)
            .ok_or_else(|| ExchangeError::ParseError("Invalid server time".to_string()))
    }
}
