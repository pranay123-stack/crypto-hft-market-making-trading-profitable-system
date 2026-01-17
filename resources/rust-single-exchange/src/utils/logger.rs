//! Logging setup

use tracing_subscriber::{fmt, prelude::*, EnvFilter};

/// Initialize logging
pub fn init_logging(level: &str, log_to_file: bool, log_dir: &str) {
    let filter = EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| EnvFilter::new(level));

    let subscriber = tracing_subscriber::registry()
        .with(filter)
        .with(fmt::layer().with_target(true).with_thread_ids(true));

    if log_to_file {
        let file_appender = tracing_appender::rolling::daily(log_dir, "hft.log");
        let (non_blocking, _guard) = tracing_appender::non_blocking(file_appender);

        let file_layer = fmt::layer()
            .with_writer(non_blocking)
            .with_ansi(false)
            .json();

        subscriber.with(file_layer).init();
    } else {
        subscriber.init();
    }
}

/// Performance timing helper
pub struct PerfTimer {
    name: &'static str,
    start: std::time::Instant,
}

impl PerfTimer {
    pub fn new(name: &'static str) -> Self {
        PerfTimer {
            name,
            start: std::time::Instant::now(),
        }
    }
}

impl Drop for PerfTimer {
    fn drop(&mut self) {
        let elapsed = self.start.elapsed();
        tracing::debug!(
            target: "perf",
            "{}: {:?}",
            self.name,
            elapsed
        );
    }
}

/// Macro for timing code blocks
#[macro_export]
macro_rules! time_it {
    ($name:expr, $block:expr) => {{
        let _timer = $crate::utils::logger::PerfTimer::new($name);
        $block
    }};
}
