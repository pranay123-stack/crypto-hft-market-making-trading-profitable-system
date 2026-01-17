================================================================================
INSTALLATION GUIDE - HIGH-FREQUENCY TRADING SYSTEM
================================================================================

OVERVIEW
--------
This directory contains comprehensive installation documentation for setting up
a complete C++ HFT (High-Frequency Trading) system from scratch. The guides are
designed for Ubuntu 22.04/24.04 LTS and RHEL 8/9, optimized for production
deployment in co-location environments.

CRITICAL REQUIREMENTS
---------------------
Before starting installation:

1. HARDWARE MINIMUMS (Production):
   - CPU: Intel Xeon or AMD EPYC with 16+ cores
   - RAM: 64GB DDR4-3200 minimum (128GB recommended)
   - Storage: 2TB NVMe SSD (PCIe 4.0)
   - Network: 10Gbps NIC (Mellanox/Intel preferred)
   - Latency: <500us to exchange (co-location)

2. OPERATING SYSTEM:
   - Ubuntu 22.04 LTS Server (recommended)
   - RHEL 8.x/9.x (enterprise environments)
   - Real-time kernel patches applied
   - Minimal installation (no GUI)

3. NETWORK ACCESS:
   - Direct internet connection (no proxy preferred)
   - Firewall configured for exchange APIs
   - NTP synchronized to atomic clock sources
   - DNS resolution working correctly

4. USER PERMISSIONS:
   - Root or sudo access required for installation
   - Dedicated 'hfttrader' user for runtime
   - Proper file permissions (covered in setup/)

INSTALLATION SEQUENCE
---------------------
Follow these guides IN ORDER to ensure proper dependency resolution:

PHASE 1: PACKAGE MANAGERS (30 minutes)
   File: 01_package_managers.txt
   - Install and configure apt/yum/dnf
   - Set up vcpkg for C++ libraries
   - Configure conan package manager
   - Enable required repositories
   - Configure package caching

PHASE 2: C++ DEVELOPMENT TOOLS (1-2 hours)
   File: 02_cpp_dependencies.txt
   - Install GCC 13+ or Clang 17+
   - CMake 3.28+, Ninja build system
   - Boost 1.84+ (with performance patches)
   - spdlog, fmt, abseil-cpp
   - Google Test, Google Benchmark
   - Threading libraries (TBB)

PHASE 3: DATABASE SYSTEMS (2-3 hours)
   File: 03_database_installation.txt
   - PostgreSQL 16 with TimescaleDB
   - Redis 7.x (for caching)
   - ClickHouse 24.x (analytics)
   - Configure for high-performance writes
   - Set up replication and backups

PHASE 4: NETWORKING LIBRARIES (1 hour)
   File: 04_networking_libraries.txt
   - libcurl (HTTP/REST APIs)
   - Boost.Beast (WebSocket support)
   - WebSocket++ (alternative)
   - libquic (QUIC protocol)
   - Zero-copy networking libraries

PHASE 5: MONITORING INFRASTRUCTURE (2-3 hours)
   File: 05_monitoring_stack.txt
   - Prometheus (metrics collection)
   - Grafana (visualization)
   - ELK Stack (logging/analytics)
   - Node Exporter, Process Exporter
   - Custom HFT dashboards

PHASE 6: EXCHANGE CONNECTIVITY (1-2 hours)
   File: 06_exchange_sdks.txt
   - Binance C++ SDK
   - Coinbase Advanced Trade API
   - Kraken WebSocket client
   - FIX protocol libraries
   - Custom exchange adapters

PHASE 7: TROUBLESHOOTING (as needed)
   File: 07_troubleshooting.txt
   - Common installation errors
   - Dependency conflicts resolution
   - Performance verification
   - Network connectivity issues
   - Compilation problems

DIRECTORY STRUCTURE AFTER INSTALLATION
---------------------------------------
/opt/hft/                          # Main installation directory
├── bin/                           # Compiled executables
├── lib/                           # Shared libraries
├── include/                       # Header files
├── config/                        # Configuration files
├── data/                          # Market data storage
├── logs/                          # Application logs
└── backups/                       # Database backups

/usr/local/                        # System-wide installations
├── bin/                           # cmake, ninja, etc.
├── lib/                           # boost, spdlog, etc.
└── include/                       # C++ headers

/var/lib/                          # Database storage
├── postgresql/
├── redis/
└── clickhouse/

/home/hfttrader/                   # User workspace
├── hft_system/                    # Source code
├── builds/                        # Build artifacts
├── logs/                          # User logs
└── .ssh/                          # SSH keys for APIs

VERIFICATION CHECKLIST
----------------------
After completing installation, verify:

[  ] GCC/Clang version >= 13/17
[  ] CMake version >= 3.28
[  ] Boost version >= 1.84
[  ] PostgreSQL running and accepting connections
[  ] Redis responding to PING
[  ] ClickHouse cluster healthy
[  ] Prometheus scraping metrics
[  ] Grafana accessible on port 3000
[  ] Exchange API connectivity test passed
[  ] Latency to exchange < 1ms (co-location)
[  ] All unit tests passing
[  ] Build system working (Debug/Release)

VERIFICATION COMMANDS
---------------------
# Check compiler versions
g++ --version                      # Should be 13+
clang++ --version                  # Should be 17+

# Check build tools
cmake --version                    # Should be 3.28+
ninja --version                    # Should be 1.11+

# Check libraries
dpkg -l | grep boost              # Ubuntu/Debian
rpm -qa | grep boost              # RHEL/CentOS

# Check databases
sudo systemctl status postgresql
sudo systemctl status redis
sudo systemctl status clickhouse-server

# Check monitoring
curl http://localhost:9090         # Prometheus
curl http://localhost:3000         # Grafana

# Test network latency
ping -c 10 api.binance.com
ping -c 10 api.coinbase.com

ESTIMATED INSTALLATION TIME
---------------------------
Fresh Installation:     6-10 hours
With Cached Packages:   2-4 hours
Expert Install:         1-2 hours

Development Machine:    3-5 hours (no databases/monitoring)
Staging Server:         5-8 hours (partial monitoring)
Production Server:      8-12 hours (full stack + validation)

PREREQUISITE KNOWLEDGE
----------------------
To successfully complete installation, you should understand:

- Linux system administration (intermediate)
- Package management (apt/yum/dnf)
- C++ compilation and linking
- Database administration basics
- Network configuration
- Systemd service management

SUPPORT AND RESOURCES
---------------------
Before asking for help, check:

1. 07_troubleshooting.txt for common issues
2. System logs: journalctl -xe
3. Application logs in /var/log/
4. Database logs in respective data directories
5. Build output for compilation errors

External Resources:
- Boost documentation: https://www.boost.org/doc/
- CMake documentation: https://cmake.org/documentation/
- PostgreSQL documentation: https://www.postgresql.org/docs/
- Prometheus documentation: https://prometheus.io/docs/

SECURITY CONSIDERATIONS
-----------------------
During installation:

1. NEVER run production services as root
2. Use dedicated service accounts
3. Configure firewall before exposing services
4. Use strong passwords for databases
5. Secure API keys in environment variables
6. Enable SSL/TLS for all external connections
7. Review file permissions after installation
8. Keep audit logs of all installations

POST-INSTALLATION STEPS
-----------------------
After completing all installation guides:

1. Run security hardening script
2. Configure backup automation
3. Set up monitoring alerts
4. Perform load testing
5. Document custom configurations
6. Create system snapshots/images
7. Review and update firewall rules
8. Schedule maintenance windows

ROLLBACK PROCEDURES
-------------------
If installation fails:

1. Stop all services:
   sudo systemctl stop postgresql redis clickhouse-server
   sudo systemctl stop prometheus grafana-server

2. Remove packages (Ubuntu):
   sudo apt remove --purge <package-name>
   sudo apt autoremove

3. Remove packages (RHEL):
   sudo yum remove <package-name>

4. Clean build directories:
   rm -rf /opt/hft/*
   rm -rf ~/builds/*

5. Restore from snapshot if available

PERFORMANCE VALIDATION
----------------------
After installation, run these benchmarks:

# CPU performance
sysbench cpu --cpu-max-prime=20000 run

# Memory bandwidth
sysbench memory --memory-block-size=1M --memory-total-size=10G run

# Disk I/O
fio --name=random-write --ioengine=libaio --rw=randwrite --bs=4k \
    --numjobs=4 --size=1g --runtime=60 --time_based

# Network latency
sockperf ping-pong -i <exchange-ip> -p 9999

# Database write performance
pgbench -i -s 50 hft_db
pgbench -c 10 -j 2 -t 10000 hft_db

Expected Results (Production Hardware):
- CPU: 5000+ events/second
- Memory: 10GB/s+ bandwidth
- Disk: 50K+ IOPS, <100us latency
- Network: <500us RTT to exchange
- PostgreSQL: 10K+ TPS

MAINTENANCE SCHEDULE
--------------------
After installation:

Daily:
- Review system logs
- Check disk space usage
- Monitor database sizes
- Verify backup completion

Weekly:
- Update security patches
- Review performance metrics
- Clean old log files
- Test disaster recovery

Monthly:
- Full system updates
- Performance benchmarking
- Capacity planning review
- Security audit

COST CONSIDERATIONS
-------------------
Budget for:

- Co-location: $500-2000/month per server
- Cloud alternatives: $200-1000/month
- Software licenses: Variable (most tools are open-source)
- Exchange API fees: Per exchange
- Monitoring tools: Free (Prometheus/Grafana)
- Database storage: Factor in data growth

NEXT STEPS
----------
1. Review all installation guides in this directory
2. Prepare installation checklist
3. Allocate time for each phase
4. Have rollback plan ready
5. Begin with 01_package_managers.txt

For production deployments, consider:
- Parallel installation on multiple servers
- Infrastructure-as-Code (Ansible/Terraform)
- Automated testing of installations
- Documentation of custom configurations

================================================================================
Last Updated: 2025-11-25
Version: 1.0
Maintainer: HFT System Team
================================================================================
