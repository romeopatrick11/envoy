#include "server/server.h"

#include <signal.h>

#include <cstdint>
#include <functional>
#include <string>

#include "envoy/event/dispatcher.h"
#include "envoy/event/signal.h"
#include "envoy/event/timer.h"
#include "envoy/network/dns.h"
#include "envoy/network/listener.h"
#include "envoy/server/options.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/api/api_impl.h"
#include "common/common/utility.h"
#include "common/common/version.h"
#include "common/memory/stats.h"
#include "common/network/address_impl.h"
#include "common/network/utility.h"
#include "common/runtime/runtime_impl.h"
#include "common/stats/statsd.h"

#include "server/configuration_impl.h"
#include "server/guarddog_impl.h"
#include "server/test_hooks.h"
#include "server/worker.h"

#include "spdlog/spdlog.h"

namespace Envoy {
namespace Server {

void InitManagerImpl::initialize(std::function<void()> callback) {
  ASSERT(state_ == State::NotInitialized);
  if (targets_.empty()) {
    callback();
    state_ = State::Initialized;
  } else {
    callback_ = callback;
    state_ = State::Initializing;
    for (auto target : targets_) {
      target->initialize([this, target]() -> void {
        ASSERT(std::find(targets_.begin(), targets_.end(), target) != targets_.end());
        targets_.remove(target);
        if (targets_.empty()) {
          state_ = State::Initialized;
          callback_();
        }
      });
    }
  }
}

void InitManagerImpl::registerTarget(Init::Target& target) {
  ASSERT(state_ == State::NotInitialized);
  targets_.push_back(&target);
}

InstanceImpl::InstanceImpl(Options& options, TestHooks& hooks, HotRestart& restarter,
                           Stats::StoreRoot& store, Thread::BasicLockable& access_log_lock,
                           ComponentFactory& component_factory,
                           const LocalInfo::LocalInfo& local_info)
    : options_(options), restarter_(restarter), start_time_(time(nullptr)),
      original_start_time_(start_time_), stats_store_(store),
      server_stats_{ALL_SERVER_STATS(POOL_GAUGE_PREFIX(stats_store_, "server."))},
      handler_(log(), Api::ApiPtr{new Api::Impl(options.fileFlushIntervalMsec())}),
      dns_resolver_(handler_.dispatcher().createDnsResolver({})), local_info_(local_info),
      access_log_manager_(handler_.api(), handler_.dispatcher(), access_log_lock, store) {

  failHealthcheck(false);

  uint64_t version_int;
  if (!StringUtil::atoul(VersionInfo::revision().substr(0, 6).c_str(), version_int, 16)) {
    throw EnvoyException("compiled GIT SHA is invalid. Invalid build.");
  }
  server_stats_.version_.set(version_int);

  restarter_.initialize(handler_.dispatcher(), *this);
  drain_manager_ = component_factory.createDrainManager(*this);

  try {
    initialize(options, hooks, component_factory);
  } catch (const EnvoyException& e) {
    log().critical("error initializing configuration '{}': {}", options.configPath(), e.what());
    thread_local_.shutdownThread();
    exit(1);
  }
}

InstanceImpl::~InstanceImpl() { restarter_.shutdown(); }

Upstream::ClusterManager& InstanceImpl::clusterManager() { return config_->clusterManager(); }

Tracing::HttpTracer& InstanceImpl::httpTracer() { return config_->httpTracer(); }

void InstanceImpl::drainListeners() {
  log().warn("closing and draining listeners");
  for (const auto& worker : workers_) {
    Worker& worker_ref = *worker;
    worker->dispatcher().post([&worker_ref]() -> void { worker_ref.handler()->closeListeners(); });
  }

  drain_manager_->startDrainSequence();
}

void InstanceImpl::failHealthcheck(bool fail) {
  // We keep liveness state in shared memory so the parent process sees the same state.
  server_stats_.live_.set(!fail);
}

void InstanceImpl::flushStats() {
  log_debug("flushing stats");
  HotRestart::GetParentStatsInfo info;
  restarter_.getParentStats(info);
  server_stats_.uptime_.set(time(nullptr) - original_start_time_);
  server_stats_.memory_allocated_.set(Memory::Stats::totalCurrentlyAllocated() +
                                      info.memory_allocated_);
  server_stats_.memory_heap_size_.set(Memory::Stats::totalCurrentlyReserved());
  server_stats_.parent_connections_.set(info.num_connections_);
  server_stats_.total_connections_.set(numConnections() + info.num_connections_);
  server_stats_.days_until_first_cert_expiring_.set(
      sslContextManager().daysUntilFirstCertExpires());

  for (const Stats::CounterSharedPtr& counter : stats_store_.counters()) {
    uint64_t delta = counter->latch();
    if (counter->used()) {
      for (const auto& sink : stat_sinks_) {
        sink->flushCounter(counter->name(), delta);
      }
    }
  }

  for (const Stats::GaugeSharedPtr& gauge : stats_store_.gauges()) {
    if (gauge->used()) {
      for (const auto& sink : stat_sinks_) {
        sink->flushGauge(gauge->name(), gauge->value());
      }
    }
  }

  stat_flush_timer_->enableTimer(config_->statsFlushInterval());
}

int InstanceImpl::getListenSocketFd(const std::string& address) {
  Network::Address::InstanceConstSharedPtr addr = Network::Utility::resolveUrl(address);
  for (const auto& entry : socket_map_) {
    if (entry.second->localAddress()->asString() == addr->asString()) {
      return entry.second->fd();
    }
  }

  return -1;
}

Network::ListenSocket* InstanceImpl::getListenSocketByIndex(uint32_t index) {
  if (index < config_->listeners().size()) {
    auto it = std::next(config_->listeners().begin(), index);
    return socket_map_[it->get()].get();
  }
  return nullptr;
}

void InstanceImpl::getParentStats(HotRestart::GetParentStatsInfo& info) {
  info.memory_allocated_ = Memory::Stats::totalCurrentlyAllocated();
  info.num_connections_ = numConnections();
}

bool InstanceImpl::healthCheckFailed() { return server_stats_.live_.value() == 0; }

void InstanceImpl::initialize(Options& options, TestHooks& hooks,
                              ComponentFactory& component_factory) {
  log().warn("initializing epoch {} (hot restart version={})", options.restartEpoch(),
             restarter_.version());

  // Handle configuration that needs to take place prior to the main configuration load.
  Json::ObjectSharedPtr config_json = Json::Factory::loadFromFile(options.configPath());
  Configuration::InitialImpl initial_config(*config_json);
  log().info("admin address: {}", initial_config.admin().address()->asString());

  HotRestart::ShutdownParentAdminInfo info;
  info.original_start_time_ = original_start_time_;
  restarter_.shutdownParentAdmin(info);
  original_start_time_ = info.original_start_time_;
  admin_.reset(new AdminImpl(initial_config.admin().accessLogPath(),
                             initial_config.admin().profilePath(), options.adminAddressPath(),
                             initial_config.admin().address(), *this));

  admin_scope_ = stats_store_.createScope("listener.admin.");
  handler_.addListener(*admin_, admin_->mutable_socket(), *admin_scope_,
                       Network::ListenerOptions::listenerOptionsWithBindToPort());

  loadServerFlags(initial_config.flagsPath());

  // Workers get created first so they register for thread local updates.
  for (uint32_t i = 0; i < std::max(1U, options.concurrency()); i++) {
    workers_.emplace_back(new Worker(thread_local_, options.fileFlushIntervalMsec()));
  }

  // The main thread is also registered for thread local updates so that code that does not care
  // whether it runs on the main thread or on workers can still use TLS.
  thread_local_.registerThread(handler_.dispatcher(), true);

  // We can now initialize stats for threading.
  stats_store_.initializeThreading(handler_.dispatcher(), thread_local_);

  // Runtime gets initialized before the main configuration since during main configuration
  // load things may grab a reference to the loader for later use.
  runtime_loader_ = component_factory.createRuntime(*this, initial_config);

  // Once we have runtime we can initialize the SSL context manager.
  ssl_context_manager_.reset(new Ssl::ContextManagerImpl(*runtime_loader_));

  cluster_manager_factory_.reset(new Upstream::ProdClusterManagerFactory(
      runtime(), stats(), threadLocal(), random(), dnsResolver(), sslContextManager(), dispatcher(),
      localInfo()));

  // Now the configuration gets parsed. The configuration may start setting thread local data
  // per above. See MainImpl::initialize() for why we do this pointer dance.
  Configuration::MainImpl* main_config =
      new Configuration::MainImpl(*this, *cluster_manager_factory_);
  config_.reset(main_config);
  main_config->initialize(*config_json);

  for (const Configuration::ListenerPtr& listener : config_->listeners()) {
    // For each listener config we share a single TcpListenSocket among all threaded listeners.
    // UdsListenerSockets are not managed and do not participate in hot restart as they are only
    // used for testing.

    // First we try to get the socket from our parent if applicable.

    ASSERT(listener->address()->type() == Network::Address::Type::Ip);
    std::string addr = fmt::format("tcp://{}", listener->address()->asString());
    int fd = restarter_.duplicateParentListenSocket(addr);
    if (fd != -1) {
      log().info("obtained socket for address {} from parent", addr);
      socket_map_[listener.get()].reset(new Network::TcpListenSocket(fd, listener->address()));
    } else {
      socket_map_[listener.get()].reset(
          new Network::TcpListenSocket(listener->address(), listener->bindToPort()));
    }
  }

  // Setup signals.
  sigterm_ = handler_.dispatcher().listenForSignal(SIGTERM, [this]() -> void {
    log().warn("caught SIGTERM");
    restarter_.terminateParent();
    handler_.dispatcher().exit();
  });

  sig_usr_1_ = handler_.dispatcher().listenForSignal(SIGUSR1, [this]() -> void {
    log().warn("caught SIGUSR1");
    access_log_manager_.reopen();
  });

  sig_hup_ = handler_.dispatcher().listenForSignal(SIGHUP, []() -> void {
    log().warn("caught and eating SIGHUP. See documentation for how to hot restart.");
  });

  initializeStatSinks();

  // Some of the stat sinks may need dispatcher support so don't flush until the main loop starts.
  // Just setup the timer.
  stat_flush_timer_ = handler_.dispatcher().createTimer([this]() -> void { flushStats(); });
  stat_flush_timer_->enableTimer(config_->statsFlushInterval());

  // GuardDog (deadlock detection) object and thread setup before workers are
  // started and before our own run() loop runs.
  guard_dog_.reset(
      new Server::GuardDogImpl(*admin_scope_, *config_, ProdMonotonicTimeSource::instance_));

  // Register for cluster manager init notification. We don't start serving worker traffic until
  // upstream clusters are initialized which may involve running the event loop. Note however that
  // this can fire immediately if all clusters have already initialized.
  clusterManager().setInitializedCb([this, &hooks]() -> void {
    log().warn("all clusters initialized. initializing init manager");
    init_manager_.initialize([this, &hooks]() -> void { startWorkers(hooks); });
  });
}

void InstanceImpl::startWorkers(TestHooks& hooks) {
  log().warn("all dependencies initialized. starting workers");
  for (const WorkerPtr& worker : workers_) {
    try {
      worker->initializeConfiguration(*config_, socket_map_, *guard_dog_);
    } catch (const Network::CreateListenerException& e) {
      // It is possible that we fail to start listening on a port, even though we were able to
      // bind to it above. This happens when there is a race between two applications to listen
      // on the same port. In general if we can't initialize the worker configuration just print
      // the error and exit cleanly without crashing.
      log().critical("shutting down due to error initializing worker configuration: {}", e.what());
      shutdown();
    }
  }

  // At this point we are ready to take traffic and all listening ports are up. Notify our parent
  // if applicable that they can stop listening and drain.
  restarter_.drainParentListeners();
  drain_manager_->startParentShutdownSequence();
  hooks.onServerInitialized();
}

Runtime::LoaderPtr InstanceUtil::createRuntime(Instance& server,
                                               Server::Configuration::Initial& config) {
  if (config.runtime()) {
    log().info("runtime symlink: {}", config.runtime()->symlinkRoot());
    log().info("runtime subdirectory: {}", config.runtime()->subdirectory());

    std::string override_subdirectory =
        config.runtime()->overrideSubdirectory() + "/" + server.localInfo().clusterName();
    log().info("runtime override subdirectory: {}", override_subdirectory);

    return Runtime::LoaderPtr{new Runtime::LoaderImpl(
        server.dispatcher(), server.threadLocal(), config.runtime()->symlinkRoot(),
        config.runtime()->subdirectory(), override_subdirectory, server.stats(), server.random())};
  } else {
    return Runtime::LoaderPtr{new Runtime::NullLoaderImpl(server.random())};
  }
}

void InstanceImpl::initializeStatSinks() {
  if (config_->statsdUdpIpAddress().valid()) {
    log().info("statsd UDP ip address: {}", config_->statsdUdpIpAddress().value());
    stat_sinks_.emplace_back(new Stats::Statsd::UdpStatsdSink(
        thread_local_,
        Network::Utility::parseInternetAddressAndPort(config_->statsdUdpIpAddress().value())));
    stats_store_.addSink(*stat_sinks_.back());
  } else if (config_->statsdUdpPort().valid()) {
    // TODO(hennna): DEPRECATED - statsdUdpPort will be removed in 1.4.0.
    log().warn("statsd_local_udp_port has been DEPRECATED and will be removed in 1.4.0. "
               "Consider setting statsd_udp_ip_address instead.");
    log().info("statsd UDP port: {}", config_->statsdUdpPort().value());
    Network::Address::InstanceConstSharedPtr address(
        new Network::Address::Ipv4Instance(config_->statsdUdpPort().value()));
    stat_sinks_.emplace_back(new Stats::Statsd::UdpStatsdSink(thread_local_, address));
    stats_store_.addSink(*stat_sinks_.back());
  }

  if (config_->statsdTcpClusterName().valid()) {
    log().info("statsd TCP cluster: {}", config_->statsdTcpClusterName().value());
    stat_sinks_.emplace_back(
        new Stats::Statsd::TcpStatsdSink(local_info_, config_->statsdTcpClusterName().value(),
                                         thread_local_, config_->clusterManager(), stats_store_));
    stats_store_.addSink(*stat_sinks_.back());
  }
}

void InstanceImpl::loadServerFlags(const Optional<std::string>& flags_path) {
  if (!flags_path.valid()) {
    return;
  }

  log().info("server flags path: {}", flags_path.value());
  if (handler_.api().fileExists(flags_path.value() + "/drain")) {
    log().warn("starting server in drain mode");
    failHealthcheck(true);
  }
}

uint64_t InstanceImpl::numConnections() {
  uint64_t num_connections = 0;
  for (const auto& worker : workers_) {
    if (worker->handler()) {
      num_connections += worker->handler()->numConnections();
    }
  }

  return num_connections;
}

void InstanceImpl::run() {
  // Run the main dispatch loop waiting to exit.
  log().warn("starting main dispatch loop");
  auto watchdog = guard_dog_->createWatchDog(Thread::Thread::currentThreadId());
  watchdog->startWatchdog(handler_.dispatcher());
  handler_.dispatcher().run(Event::Dispatcher::RunType::Block);
  log().warn("main dispatch loop exited");
  guard_dog_->stopWatching(watchdog);
  watchdog.reset();

  // Before the workers start exiting we should disable stat threading.
  stats_store_.shutdownThreading();

  // Shutdown all the listeners now that the main dispatch loop is done.
  for (const WorkerPtr& worker : workers_) {
    worker->exit();
  }

  // Only flush if we have not been hot restarted.
  if (stat_flush_timer_) {
    flushStats();
  }

  config_->clusterManager().shutdown();
  handler_.closeConnections();
  thread_local_.shutdownThread();
  log().warn("exiting");
  log().flush();
}

Runtime::Loader& InstanceImpl::runtime() { return *runtime_loader_; }

void InstanceImpl::shutdown() {
  log().warn("shutdown invoked. sending SIGTERM to self");
  kill(getpid(), SIGTERM);
}

void InstanceImpl::shutdownAdmin() {
  log().warn("shutting down admin due to child startup");
  stat_flush_timer_.reset();
  handler_.closeListeners();
  admin_->mutable_socket().close();

  log().warn("terminating parent process");
  restarter_.terminateParent();
}

} // Server
} // Envoy
