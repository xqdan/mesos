// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#include <signal.h>
#include <stdlib.h> // For random().

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <mesos/type_utils.hpp>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/module/authenticatee.hpp>

#ifdef ENABLE_GRPC
#include <mesos/resource_provider/storage/volume_profile.hpp>
#endif

#include <process/after.hpp>
#include <process/async.hpp>
#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/reap.hpp>
#include <process/time.hpp>

#include <process/ssl/flags.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/fs.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include <stout/os/realpath.hpp>

#include "authentication/cram_md5/authenticatee.hpp"

#include "common/build.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"
#include "common/status_utils.hpp"
#include "common/validation.hpp"

#include "credentials/credentials.hpp"

#include "hook/manager.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

#include "logging/logging.hpp"

#include "master/detector/standalone.hpp"

#include "module/manager.hpp"

#include "slave/compatibility.hpp"
#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/task_status_update_manager.hpp"

#ifdef __WINDOWS__
// Used to install a Windows console ctrl handler.
// https://msdn.microsoft.com/en-us/library/windows/desktop/ms682066(v=vs.85).aspx
#include <slave/windows_ctrlhandler.hpp>
#else
// Used to install a handler for POSIX signal.
// http://pubs.opengroup.org/onlinepubs/009695399/functions/sigaction.html
#include <slave/posix_signalhandler.hpp>
#endif // __WINDOWS__

namespace http = process::http;

using google::protobuf::RepeatedPtrField;

using mesos::SecretGenerator;

using mesos::authorization::createSubject;

using mesos::executor::Call;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerTermination;
using mesos::slave::QoSController;
using mesos::slave::QoSCorrection;
using mesos::slave::ResourceEstimator;

using std::find;
using std::list;
using std::map;
using std::ostream;
using std::ostringstream;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using process::after;
using process::async;
using process::wait; // Necessary on some OS's to disambiguate.
using process::Break;
using process::Clock;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::Time;
using process::UPID;

using process::http::authentication::Principal;

#ifdef __WINDOWS__
constexpr char MESOS_EXECUTOR[] = "mesos-executor.exe";
#else
constexpr char MESOS_EXECUTOR[] = "mesos-executor";
#endif // __WINDOWS__

namespace mesos {
namespace internal {
namespace slave {

using namespace state;

// Forward declarations.

// Needed for logging task/task group.
static string taskOrTaskGroup(
    const Option<TaskInfo>& task,
    const Option<TaskGroupInfo>& taskGroup);


// Returns the command info for default executor.
static CommandInfo defaultExecutorCommandInfo(
    const std::string& launcherDir,
    const Option<std::string>& user);


Slave::Slave(const string& id,
             const slave::Flags& _flags,
             MasterDetector* _detector,
             Containerizer* _containerizer,
             Files* _files,
             GarbageCollector* _gc,
             TaskStatusUpdateManager* _taskStatusUpdateManager,
             ResourceEstimator* _resourceEstimator,
             QoSController* _qosController,
             SecretGenerator* _secretGenerator,
             const Option<Authorizer*>& _authorizer)
  : ProcessBase(id),
    state(RECOVERING),
    flags(_flags),
    http(this),
    capabilities(
        _flags.agent_features.isNone()
          ? protobuf::slave::Capabilities(AGENT_CAPABILITIES())
          : protobuf::slave::Capabilities(
                _flags.agent_features->capabilities())),
    completedFrameworks(MAX_COMPLETED_FRAMEWORKS),
    detector(_detector),
    containerizer(_containerizer),
    files(_files),
    metrics(*this),
    gc(_gc),
    taskStatusUpdateManager(_taskStatusUpdateManager),
    masterPingTimeout(DEFAULT_MASTER_PING_TIMEOUT()),
    metaDir(paths::getMetaRootDir(flags.work_dir)),
    recoveryErrors(0),
    credential(None()),
    authenticatee(nullptr),
    authenticating(None()),
    authenticated(false),
    reauthenticate(false),
    failedAuthentications(0),
    executorDirectoryMaxAllowedAge(age(0)),
    resourceEstimator(_resourceEstimator),
    qosController(_qosController),
    secretGenerator(_secretGenerator),
    authorizer(_authorizer),
    resourceVersion(id::UUID::random()) {}


Slave::~Slave()
{
  // TODO(benh): Shut down frameworks?

  // TODO(benh): Shut down executors? The executor should get an "exited"
  // event and initiate a shut down itself.

  foreachvalue (Framework* framework, frameworks) {
    delete framework;
  }

  delete authenticatee;
}


void Slave::signaled(int signal, int uid)
{
  if (signal == SIGUSR1) {
    Result<string> user = os::user(uid);

    shutdown(
        UPID(),
        "Received SIGUSR1 signal" +
        (user.isSome() ? " from user " + user.get() : ""));
  }
}


void Slave::initialize()
{
  LOG(INFO) << "Mesos agent started on " << string(self()).substr(5);
  LOG(INFO) << "Flags at startup: " << flags;

  if (self().address.ip.isLoopback()) {
    LOG(WARNING) << "\n**************************************************\n"
                 << "Agent bound to loopback interface!"
                 << " Cannot communicate with remote master(s)."
                 << " You might want to set '--ip' flag to a routable"
                 << " IP address.\n"
                 << "**************************************************";
  }

  if (flags.registration_backoff_factor > REGISTER_RETRY_INTERVAL_MAX) {
    EXIT(EXIT_FAILURE)
      << "Invalid value '" << flags.registration_backoff_factor << "'"
      << " for --registration_backoff_factor:"
      << " Must be less than " << REGISTER_RETRY_INTERVAL_MAX;
  }

  authenticateeName = flags.authenticatee;

  // Load credential for agent authentication with the master.
  if (flags.credential.isSome()) {
    Result<Credential> _credential =
      credentials::readCredential(flags.credential.get());
    if (_credential.isError()) {
      EXIT(EXIT_FAILURE) << _credential.error() << " (see --credential flag)";
    } else if (_credential.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Empty credential file '" << flags.credential.get() << "'"
        << " (see --credential flag)";
    } else {
      credential = _credential.get();
      LOG(INFO) << "Agent using credential for: "
                << credential->principal();
    }
  }

  Option<Credentials> httpCredentials;
  if (flags.http_credentials.isSome()) {
    Result<Credentials> credentials =
      credentials::read(flags.http_credentials.get());
    if (credentials.isError()) {
       EXIT(EXIT_FAILURE)
         << credentials.error() << " (see --http_credentials flag)";
    } else if (credentials.isNone()) {
       EXIT(EXIT_FAILURE)
         << "Credentials file must contain at least one credential"
         << " (see --http_credentials flag)";
    }
    httpCredentials = credentials.get();
  }

  string httpAuthenticators;
  if (flags.http_authenticators.isSome()) {
    httpAuthenticators = flags.http_authenticators.get();
#ifdef USE_SSL_SOCKET
  } else if (flags.authenticate_http_executors) {
    httpAuthenticators =
      string(DEFAULT_BASIC_HTTP_AUTHENTICATOR) + "," +
      string(DEFAULT_JWT_HTTP_AUTHENTICATOR);
#endif // USE_SSL_SOCKET
  } else {
    httpAuthenticators = DEFAULT_BASIC_HTTP_AUTHENTICATOR;
  }

  Option<string> jwtSecretKey;
#ifdef USE_SSL_SOCKET
  if (flags.jwt_secret_key.isSome()) {
    Try<string> jwtSecretKey_ = os::read(flags.jwt_secret_key.get());
    if (jwtSecretKey_.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to read the file specified by "
                         << "--jwt_secret_key";
    }

    // TODO(greggomann): Factor the following code out into a common helper,
    // since we also do this when loading credentials.
    Try<os::Permissions> permissions =
      os::permissions(flags.jwt_secret_key.get());
    if (permissions.isError()) {
      LOG(WARNING) << "Failed to stat jwt secret key file '"
                   << flags.jwt_secret_key.get()
                   << "': " << permissions.error();
    } else if (permissions.get().others.rwx) {
      LOG(WARNING) << "Permissions on executor secret key file '"
                   << flags.jwt_secret_key.get()
                   << "' are too open; it is recommended that your"
                   << " key file is NOT accessible by others";
    }

    jwtSecretKey = jwtSecretKey_.get();
  }

  if (flags.authenticate_http_executors) {
    if (flags.jwt_secret_key.isNone()) {
      EXIT(EXIT_FAILURE) << "--jwt_secret_key must be specified when "
                         << "--authenticate_http_executors is set to true";
    }

    Try<Nothing> result = initializeHttpAuthenticators(
        EXECUTOR_HTTP_AUTHENTICATION_REALM,
        strings::split(httpAuthenticators, ","),
        httpCredentials,
        jwtSecretKey);

    if (result.isError()) {
      EXIT(EXIT_FAILURE) << result.error();
    }
  }
#endif // USE_SSL_SOCKET

  if (flags.authenticate_http_readonly) {
    Try<Nothing> result = initializeHttpAuthenticators(
        READONLY_HTTP_AUTHENTICATION_REALM,
        strings::split(httpAuthenticators, ","),
        httpCredentials,
        jwtSecretKey);

    if (result.isError()) {
      EXIT(EXIT_FAILURE) << result.error();
    }
  }

  if (flags.authenticate_http_readwrite) {
    Try<Nothing> result = initializeHttpAuthenticators(
        READWRITE_HTTP_AUTHENTICATION_REALM,
        strings::split(httpAuthenticators, ","),
        httpCredentials,
        jwtSecretKey);

    if (result.isError()) {
      EXIT(EXIT_FAILURE) << result.error();
    }
  }

  if ((flags.gc_disk_headroom < 0) || (flags.gc_disk_headroom > 1)) {
    EXIT(EXIT_FAILURE)
      << "Invalid value '" << flags.gc_disk_headroom << "'"
      << " for --gc_disk_headroom. Must be between 0.0 and 1.0";
  }

  Try<Nothing> initialize =
    resourceEstimator->initialize(defer(self(), &Self::usage));

  if (initialize.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to initialize the resource estimator: " << initialize.error();
  }

  initialize = qosController->initialize(defer(self(), &Self::usage));

  if (initialize.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to initialize the QoS Controller: " << initialize.error();
  }

  // Ensure slave work directory exists.
  Try<Nothing> mkdir = os::mkdir(flags.work_dir);
  if (mkdir.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create agent work directory '" << flags.work_dir << "': "
      << mkdir.error();
  }

#ifdef ENABLE_GRPC
  // Create the VolumeProfileAdaptor module and set it globally so
  // any component that needs the module can share this instance.
  Try<VolumeProfileAdaptor*> _volumeProfileAdaptor =
    VolumeProfileAdaptor::create(flags.volume_profile_adaptor);

  if (_volumeProfileAdaptor.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create volume profile adaptor: "
      << _volumeProfileAdaptor.error();
  }

  volumeProfileAdaptor =
    shared_ptr<VolumeProfileAdaptor>(_volumeProfileAdaptor.get());

  VolumeProfileAdaptor::setAdaptor(volumeProfileAdaptor);
#endif

  string scheme = "http";

#ifdef USE_SSL_SOCKET
  if (process::network::openssl::flags().enabled) {
    scheme = "https";
  }
#endif

  http::URL localResourceProviderURL(
      scheme,
      self().address.ip,
      self().address.port,
      self().id + "/api/v1/resource_provider");

  Try<Owned<LocalResourceProviderDaemon>> _localResourceProviderDaemon =
    LocalResourceProviderDaemon::create(
        localResourceProviderURL,
        flags,
        secretGenerator);

  if (_localResourceProviderDaemon.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create local resource provider daemon: "
      << _localResourceProviderDaemon.error();
  }

  localResourceProviderDaemon = std::move(_localResourceProviderDaemon.get());

  Try<Resources> resources = Containerizer::resources(flags);
  if (resources.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to determine agent resources: " << resources.error();
  }

  // Ensure disk `source`s are accessible.
  foreach (
      const Resource& resource,
      resources->filter([](const Resource& _resource) {
        return _resource.has_disk() && _resource.disk().has_source();
      })) {
    const Resource::DiskInfo::Source& source = resource.disk().source();
    switch (source.type()) {
      case Resource::DiskInfo::Source::PATH: {
        // For `PATH` sources we create them if they do not exist.
        CHECK(source.has_path());

        if (!source.path().has_root()) {
          EXIT(EXIT_FAILURE)
            << "PATH disk root directory is not specified "
            << "'" << resource << "'";
        }

        Try<Nothing> mkdir = os::mkdir(source.path().root(), true);

        if (mkdir.isError()) {
          EXIT(EXIT_FAILURE)
            << "Failed to create DiskInfo path directory "
            << "'" << source.path().root() << "': " << mkdir.error();
        }
        break;
      }
      case Resource::DiskInfo::Source::MOUNT: {
        CHECK(source.has_mount());

        if (!source.mount().has_root()) {
          EXIT(EXIT_FAILURE)
            << "MOUNT disk root directory is not specified "
            << "'" << resource << "'";
        }

        // For `MOUNT` sources we fail if they don't exist.
        // On Linux we test the mount table for existence.
#ifdef __linux__
        // Get the `realpath` of the `root` to verify it against the
        // mount table entries.
        // TODO(jmlvanre): Consider enforcing allowing only real paths
        // as opposed to symlinks. This would prevent the ability for
        // an operator to change the underlying data while the slave
        // checkpointed `root` had the same value. We could also check
        // the UUID of the underlying block device to catch this case.
        Result<string> realpath = os::realpath(source.mount().root());

        if (!realpath.isSome()) {
          EXIT(EXIT_FAILURE)
            << "Failed to determine `realpath` for DiskInfo mount in resource '"
            << resource << "' with path '" << source.mount().root() << "': "
            << (realpath.isError() ? realpath.error() : "no such path");
        }

        // TODO(jmlvanre): Consider moving this out of the for loop.
        Try<fs::MountTable> mountTable = fs::MountTable::read("/proc/mounts");
        if (mountTable.isError()) {
          EXIT(EXIT_FAILURE)
            << "Failed to open mount table to verify mounts: "
            << mountTable.error();
        }

        bool foundEntry = false;
        foreach (const fs::MountTable::Entry& entry, mountTable->entries) {
          if (entry.dir == realpath.get()) {
            foundEntry = true;
            break;
          }
        }

        if (!foundEntry) {
          EXIT(EXIT_FAILURE)
            << "Failed to find mount '" << realpath.get()
            << "' in /proc/mounts";
        }
#else // __linux__
        // On other platforms we test whether that provided `root` exists.
        if (!os::exists(source.mount().root())) {
          EXIT(EXIT_FAILURE)
            << "Failed to find mount point '" << source.mount().root() << "'";
        }
#endif // __linux__
        break;
      }
      case Resource::DiskInfo::Source::BLOCK:
      case Resource::DiskInfo::Source::RAW:
      case Resource::DiskInfo::Source::UNKNOWN: {
        EXIT(EXIT_FAILURE)
          << "Unsupported 'DiskInfo.Source.Type' in '" << resource << "'";
      }
    }
  }

  Attributes attributes;
  if (flags.attributes.isSome()) {
    attributes = Attributes::parse(flags.attributes.get());
  }

  // Determine our hostname or use the hostname provided.
  string hostname;

  if (flags.hostname.isNone()) {
    if (flags.hostname_lookup) {
      Try<string> result = net::getHostname(self().address.ip);

      if (result.isError()) {
        EXIT(EXIT_FAILURE) << "Failed to get hostname: " << result.error();
      }

      hostname = result.get();
    } else {
      // We use the IP address for hostname if the user requested us
      // NOT to look it up, and it wasn't explicitly set via --hostname:
      hostname = stringify(self().address.ip);
    }
  } else {
    hostname = flags.hostname.get();
  }

  // Initialize slave info.
  info.set_hostname(hostname);
  info.set_port(self().address.port);

  info.mutable_resources()->CopyFrom(resources.get());
  if (HookManager::hooksAvailable()) {
    info.mutable_resources()->CopyFrom(
        HookManager::slaveResourcesDecorator(info));
  }

  // Initialize `totalResources` with `info.resources`, checkpointed
  // resources will be applied later during recovery.
  totalResources = info.resources();

  LOG(INFO) << "Agent resources: " << info.resources();

  info.mutable_attributes()->CopyFrom(attributes);
  if (HookManager::hooksAvailable()) {
    info.mutable_attributes()->CopyFrom(
        HookManager::slaveAttributesDecorator(info));
  }

  LOG(INFO) << "Agent attributes: " << info.attributes();

  // Checkpointing of slaves is always enabled.
  info.set_checkpoint(true);

  if (flags.domain.isSome()) {
    info.mutable_domain()->CopyFrom(flags.domain.get());
  }

  LOG(INFO) << "Agent hostname: " << info.hostname();

  taskStatusUpdateManager->initialize(defer(self(), &Slave::forward, lambda::_1)
    .operator std::function<void(StatusUpdate)>());

  // We pause the task status update manager so that it doesn't forward any
  // updates while the slave is still recovering. It is unpaused/resumed when
  // the slave (re-)registers with the master.
  taskStatusUpdateManager->pause();

  // Start disk monitoring.
  // NOTE: We send a delayed message here instead of directly calling
  // checkDiskUsage, to make disabling this feature easy (e.g by specifying
  // a very large disk_watch_interval).
  delay(flags.disk_watch_interval, self(), &Slave::checkDiskUsage);

  // Start image store disk monitoring. Please note that image layers
  // garbage collection is only enabled if the agent flag `--image_gc_config`
  // is set.
  // TODO(gilbert): Consider move the image auto GC logic to containerizers
  // respectively. For now, it is only enabled for the Mesos Containerizer.
  if (flags.image_gc_config.isSome() &&
      flags.image_providers.isSome() &&
      strings::contains(flags.containerizers, "mesos")) {
    delay(
        Nanoseconds(
            flags.image_gc_config->image_disk_watch_interval().nanoseconds()),
        self(),
        &Slave::checkImageDiskUsage);
  }

  startTime = Clock::now();

  // Install protobuf handlers.
  install<SlaveRegisteredMessage>(
      &Slave::registered,
      &SlaveRegisteredMessage::slave_id,
      &SlaveRegisteredMessage::connection);

  install<SlaveReregisteredMessage>(
      &Slave::reregistered,
      &SlaveReregisteredMessage::slave_id,
      &SlaveReregisteredMessage::reconciliations,
      &SlaveReregisteredMessage::connection);

  install<RunTaskMessage>(
      &Slave::runTask,
      &RunTaskMessage::framework,
      &RunTaskMessage::framework_id,
      &RunTaskMessage::pid,
      &RunTaskMessage::task,
      &RunTaskMessage::resource_version_uuids);

  install<RunTaskGroupMessage>(
      &Slave::runTaskGroup,
      &RunTaskGroupMessage::framework,
      &RunTaskGroupMessage::executor,
      &RunTaskGroupMessage::task_group,
      &RunTaskGroupMessage::resource_version_uuids);

  install<KillTaskMessage>(
      &Slave::killTask);

  install<ShutdownExecutorMessage>(
      &Slave::shutdownExecutor,
      &ShutdownExecutorMessage::framework_id,
      &ShutdownExecutorMessage::executor_id);

  install<ShutdownFrameworkMessage>(
      &Slave::shutdownFramework,
      &ShutdownFrameworkMessage::framework_id);

  install<FrameworkToExecutorMessage>(
      &Slave::schedulerMessage,
      &FrameworkToExecutorMessage::slave_id,
      &FrameworkToExecutorMessage::framework_id,
      &FrameworkToExecutorMessage::executor_id,
      &FrameworkToExecutorMessage::data);

  install<UpdateFrameworkMessage>(
      &Slave::updateFramework);

  install<CheckpointResourcesMessage>(
      &Slave::checkpointResourcesMessage,
      &CheckpointResourcesMessage::resources);

  install<ApplyOperationMessage>(
      &Slave::applyOperation);

  install<ReconcileOperationsMessage>(
      &Slave::reconcileOperations);

  install<StatusUpdateAcknowledgementMessage>(
      &Slave::statusUpdateAcknowledgement,
      &StatusUpdateAcknowledgementMessage::slave_id,
      &StatusUpdateAcknowledgementMessage::framework_id,
      &StatusUpdateAcknowledgementMessage::task_id,
      &StatusUpdateAcknowledgementMessage::uuid);

  install<AcknowledgeOperationStatusMessage>(
      &Slave::operationStatusAcknowledgement);

  install<RegisterExecutorMessage>(
      &Slave::registerExecutor,
      &RegisterExecutorMessage::framework_id,
      &RegisterExecutorMessage::executor_id);

  install<ReregisterExecutorMessage>(
      &Slave::reregisterExecutor,
      &ReregisterExecutorMessage::framework_id,
      &ReregisterExecutorMessage::executor_id,
      &ReregisterExecutorMessage::tasks,
      &ReregisterExecutorMessage::updates);

  install<StatusUpdateMessage>(
      &Slave::statusUpdate,
      &StatusUpdateMessage::update,
      &StatusUpdateMessage::pid);

  install<ExecutorToFrameworkMessage>(
      &Slave::executorMessage,
      &ExecutorToFrameworkMessage::slave_id,
      &ExecutorToFrameworkMessage::framework_id,
      &ExecutorToFrameworkMessage::executor_id,
      &ExecutorToFrameworkMessage::data);

  install<ShutdownMessage>(
      &Slave::shutdown,
      &ShutdownMessage::message);

  install<PingSlaveMessage>(
      &Slave::ping,
      &PingSlaveMessage::connected);

  // Setup the '/api/v1' handler for streaming requests.
  RouteOptions options;
  options.requestStreaming = true;
  route("/api/v1",
        // TODO(benh): Is this authentication realm sufficient or do
        // we need some kind of hybrid if we expect both executors
        // and operators/tooling to use this endpoint?
        READWRITE_HTTP_AUTHENTICATION_REALM,
        Http::API_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.api(request, principal);
        },
        options);

  route("/api/v1/executor",
        EXECUTOR_HTTP_AUTHENTICATION_REALM,
        Http::EXECUTOR_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.executor(request, principal);
        });

  route("/api/v1/resource_provider",
        READWRITE_HTTP_AUTHENTICATION_REALM,
        Http::RESOURCE_PROVIDER_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return resourceProviderManager.api(request, principal);
        });

  // TODO(ijimenez): Remove this endpoint at the end of the
  // deprecation cycle on 0.26.
  route("/state.json",
        READONLY_HTTP_AUTHENTICATION_REALM,
        Http::STATE_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.state(request, principal);
        });
  route("/state",
        READONLY_HTTP_AUTHENTICATION_REALM,
        Http::STATE_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.state(request, principal);
        });
  route("/flags",
        READONLY_HTTP_AUTHENTICATION_REALM,
        Http::FLAGS_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.flags(request, principal);
        });
  route("/health",
        Http::HEALTH_HELP(),
        [this](const http::Request& request) {
          return http.health(request);
        });
  route("/monitor/statistics",
        READONLY_HTTP_AUTHENTICATION_REALM,
        Http::STATISTICS_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.statistics(request, principal);
        });
  // TODO(ijimenez): Remove this endpoint at the end of the
  // deprecation cycle on 0.26.
  route("/monitor/statistics.json",
        READONLY_HTTP_AUTHENTICATION_REALM,
        Http::STATISTICS_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.statistics(request, principal);
        });
  route("/containers",
        READONLY_HTTP_AUTHENTICATION_REALM,
        Http::CONTAINERS_HELP(),
        [this](const http::Request& request,
               const Option<Principal>& principal) {
          logRequest(request);
          return http.containers(request, principal);
        });

  const PID<Slave> slavePid = self();

  auto authorize = [slavePid](const Option<Principal>& principal) {
    return dispatch(
        slavePid,
        &Slave::authorizeLogAccess,
        principal);
  };

  // Expose the log file for the webui. Fall back to 'log_dir' if
  // an explicit file was not specified.
  if (flags.external_log_file.isSome()) {
    files->attach(
        flags.external_log_file.get(), AGENT_LOG_VIRTUAL_PATH, authorize)
      .onAny(defer(self(),
                   &Self::fileAttached,
                   lambda::_1,
                   flags.external_log_file.get(),
                   AGENT_LOG_VIRTUAL_PATH));
  } else if (flags.log_dir.isSome()) {
    Try<string> log =
      logging::getLogFile(logging::getLogSeverity(flags.logging_level));

    if (log.isError()) {
      LOG(ERROR) << "Agent log file cannot be found: " << log.error();
    } else {
      files->attach(log.get(), AGENT_LOG_VIRTUAL_PATH, authorize)
        .onAny(defer(self(),
                     &Self::fileAttached,
                     lambda::_1,
                     log.get(),
                     AGENT_LOG_VIRTUAL_PATH));
    }
  }

  // Check that the reconfiguration_policy flag is valid.
  if (flags.reconfiguration_policy != "equal" &&
      flags.reconfiguration_policy != "additive") {
    EXIT(EXIT_FAILURE)
      << "Unknown option for 'reconfiguration_policy' flag "
      << flags.reconfiguration_policy << "."
      << " Please run the agent with '--help' to see the valid options.";
  }

  // Check that the recover flag is valid.
  if (flags.recover != "reconnect" && flags.recover != "cleanup") {
    EXIT(EXIT_FAILURE)
      << "Unknown option for 'recover' flag " << flags.recover << "."
      << " Please run the agent with '--help' to see the valid options";
  }

  auto signalHandler = defer(self(), &Slave::signaled, lambda::_1, lambda::_2)
    .operator std::function<void(int, int)>();

#ifdef __WINDOWS__
  if (!os::internal::installCtrlHandler(&signalHandler)) {
    EXIT(EXIT_FAILURE)
      << "Failed to configure console handlers: " << WindowsError().message;
  }
#else
  if (os::internal::configureSignal(&signalHandler) < 0) {
    EXIT(EXIT_FAILURE)
      << "Failed to configure signal handlers: " << os::strerror(errno);
  }
#endif  // __WINDOWS__

  // Do recovery.
  async(&state::recover, metaDir, flags.strict)
    .then(defer(self(), &Slave::recover, lambda::_1))
    .then(defer(self(), &Slave::_recover))
    .onAny(defer(self(), &Slave::__recover, lambda::_1));
}


void Slave::finalize()
{
  LOG(INFO) << "Agent terminating";

  // NOTE: We use 'frameworks.keys()' here because 'shutdownFramework'
  // can potentially remove a framework from 'frameworks'.
  foreach (const FrameworkID& frameworkId, frameworks.keys()) {
    // TODO(benh): Because a shut down isn't instantaneous (but has
    // a shut down/kill phases) we might not actually propagate all
    // the status updates appropriately here. Consider providing
    // an alternative function which skips the shut down phase and
    // simply does a kill (sending all status updates
    // immediately). Of course, this still isn't sufficient
    // because those status updates might get lost and we won't
    // resend them unless we build that into the system.
    // NOTE: We shut down the framework only if it has disabled
    // checkpointing. This is because slave recovery tests terminate
    // the slave to simulate slave restart.
    if (!frameworks[frameworkId]->info.checkpoint()) {
      shutdownFramework(UPID(), frameworkId);
    }
  }
}


void Slave::shutdown(const UPID& from, const string& message)
{
  if (from && master != from) {
    LOG(WARNING) << "Ignoring shutdown message from " << from
                 << " because it is not from the registered master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  if (from) {
    LOG(INFO) << "Agent asked to shut down by " << from
              << (message.empty() ? "" : " because '" + message + "'");
  } else if (info.has_id()) {
    if (message.empty()) {
      LOG(INFO) << "Unregistering and shutting down";
    } else {
      LOG(INFO) << message << "; unregistering and shutting down";
    }

    UnregisterSlaveMessage message_;
    message_.mutable_slave_id()->MergeFrom(info.id());
    send(master.get(), message_);
  } else {
    if (message.empty()) {
      LOG(INFO) << "Shutting down";
    } else {
      LOG(INFO) << message << "; shutting down";
    }
  }

  state = TERMINATING;

  if (frameworks.empty()) { // Terminate slave if there are no frameworks.
    terminate(self());
  } else {
    // NOTE: The slave will terminate after all the executors have
    // terminated.
    // NOTE: We use 'frameworks.keys()' here because 'shutdownFramework'
    // can potentially remove a framework from 'frameworks'.
    foreach (const FrameworkID& frameworkId, frameworks.keys()) {
      shutdownFramework(from, frameworkId);
    }
  }
}


void Slave::fileAttached(
    const Future<Nothing>& result,
    const string& path,
    const string& virtualPath)
{
  if (result.isReady()) {
    VLOG(1) << "Successfully attached '" << path << "'"
            << " to virtual path '" << virtualPath << "'";
  } else {
    LOG(ERROR) << "Failed to attach '" << path << "'"
               << " to virtual path '" << virtualPath << "': "
               << (result.isFailed() ? result.failure() : "discarded");
  }
}


// TODO(vinod/bmahler): Get rid of this helper.
Nothing Slave::detachFile(const string& path)
{
  files->detach(path);
  return Nothing();
}


void Slave::detected(const Future<Option<MasterInfo>>& _master)
{
  CHECK(state == DISCONNECTED ||
        state == RUNNING ||
        state == TERMINATING) << state;

  if (state != TERMINATING) {
    state = DISCONNECTED;
  }

  // Pause the status updates.
  taskStatusUpdateManager->pause();

  if (_master.isFailed()) {
    EXIT(EXIT_FAILURE) << "Failed to detect a master: " << _master.failure();
  }

  Option<MasterInfo> latest;

  if (_master.isDiscarded()) {
    LOG(INFO) << "Re-detecting master";
    latest = None();
    master = None();
  } else if (_master->isNone()) {
    LOG(INFO) << "Lost leading master";
    latest = None();
    master = None();
  } else {
    latest = _master.get();
    master = UPID(latest->pid());

    LOG(INFO) << "New master detected at " << master.get();

    // Cancel the pending registration timer to avoid spurious attempts
    // at reregistration. `Clock::cancel` is idempotent, so this call
    // is safe even if no timer is active or pending.
    Clock::cancel(agentRegistrationTimer);

    if (state == TERMINATING) {
      LOG(INFO) << "Skipping registration because agent is terminating";
      return;
    }

    if (requiredMasterCapabilities.agentUpdate) {
      protobuf::master::Capabilities masterCapabilities(
          latest->capabilities());

      if (!masterCapabilities.agentUpdate) {
        EXIT(EXIT_FAILURE) <<
          "Agent state changed on restart, but the detected master lacks the "
          "AGENT_UPDATE capability. Refusing to connect.";
        return;
      }

      if (dynamic_cast<mesos::master::detector::StandaloneMasterDetector*>(
          detector)) {
        LOG(WARNING) <<
          "The AGENT_UPDATE master capability is required, "
          "but the StandaloneMasterDetector does not have the ability to read "
          "master capabilities.";
      }
    }

    // Wait for a random amount of time before authentication or
    // registration.
    Duration duration =
      flags.registration_backoff_factor * ((double) os::random() / RAND_MAX);

    if (credential.isSome()) {
      // Authenticate with the master.
      // TODO(vinod): Consider adding an "AUTHENTICATED" state to the
      // slave instead of "authenticate" variable.
      delay(duration, self(), &Slave::authenticate);
    } else {
      // Proceed with registration without authentication.
      LOG(INFO) << "No credentials provided."
                << " Attempting to register without authentication";

      delay(duration,
            self(),
            &Slave::doReliableRegistration,
            flags.registration_backoff_factor * 2); // Backoff.
    }
  }

  // Keep detecting masters.
  LOG(INFO) << "Detecting new master";
  detection = detector->detect(latest)
    .onAny(defer(self(), &Slave::detected, lambda::_1));
}


void Slave::authenticate()
{
  authenticated = false;

  if (master.isNone()) {
    return;
  }

  if (authenticating.isSome()) {
    // Authentication is in progress. Try to cancel it.
    // Note that it is possible that 'authenticating' is ready
    // and the dispatch to '_authenticate' is enqueued when we
    // are here, making the 'discard' here a no-op. This is ok
    // because we set 'reauthenticate' here which enforces a retry
    // in '_authenticate'.
    Future<bool> authenticating_ = authenticating.get();
    authenticating_.discard();
    reauthenticate = true;
    return;
  }

  LOG(INFO) << "Authenticating with master " << master.get();

  // Ensure there is a link to the master before we start
  // communicating with it.
  link(master.get());

  CHECK(authenticatee == nullptr);

  if (authenticateeName == DEFAULT_AUTHENTICATEE) {
    LOG(INFO) << "Using default CRAM-MD5 authenticatee";
    authenticatee = new cram_md5::CRAMMD5Authenticatee();
  }

  if (authenticatee == nullptr) {
    Try<Authenticatee*> module =
      modules::ModuleManager::create<Authenticatee>(authenticateeName);
    if (module.isError()) {
      EXIT(EXIT_FAILURE)
        << "Could not create authenticatee module '"
        << authenticateeName << "': " << module.error();
    }
    LOG(INFO) << "Using '" << authenticateeName << "' authenticatee";
    authenticatee = module.get();
  }

  CHECK_SOME(credential);

  authenticating =
    authenticatee->authenticate(master.get(), self(), credential.get())
      .onAny(defer(self(), &Self::_authenticate));

  delay(Seconds(5), self(), &Self::authenticationTimeout, authenticating.get());
}


void Slave::_authenticate()
{
  delete CHECK_NOTNULL(authenticatee);
  authenticatee = nullptr;

  CHECK_SOME(authenticating);
  const Future<bool>& future = authenticating.get();

  if (master.isNone()) {
    LOG(INFO) << "Ignoring _authenticate because the master is lost";
    authenticating = None();
    // Set it to false because we do not want further retries until
    // a new master is detected.
    // We obviously do not need to reauthenticate either even if
    // 'reauthenticate' is currently true because the master is
    // lost.
    reauthenticate = false;
    return;
  }

  if (reauthenticate || !future.isReady()) {
    LOG(WARNING)
      << "Failed to authenticate with master " << master.get() << ": "
      << (reauthenticate ? "master changed" :
         (future.isFailed() ? future.failure() : "future discarded"));

    authenticating = None();
    reauthenticate = false;

    ++failedAuthentications;

    // Backoff.
    // The backoff is a random duration in the interval [0, b * 2^N)
    // where `b = authentication_backoff_factor` and `N` the number
    // of failed authentication attempts. It is capped by
    // `REGISTER_RETRY_INTERVAL_MAX`.
    Duration backoff =
      flags.authentication_backoff_factor * std::pow(2, failedAuthentications);
    backoff = std::min(backoff, AUTHENTICATION_RETRY_INTERVAL_MAX);

    // Determine the delay for next attempt by picking a random
    // duration between 0 and 'maxBackoff'.
    // TODO(vinod): Use random numbers from <random> header.
    backoff *= double(os::random()) / RAND_MAX;

    // TODO(vinod): Add a limit on number of retries.
    delay(backoff, self(), &Self::authenticate); // Retry.
    return;
  }

  if (!future.get()) {
    // For refused authentication, we exit instead of doing a shutdown
    // to keep possibly active executors running.
    EXIT(EXIT_FAILURE)
      << "Master " << master.get() << " refused authentication";
  }

  LOG(INFO) << "Successfully authenticated with master " << master.get();

  authenticated = true;
  authenticating = None();

  failedAuthentications = 0;

  // Proceed with registration.
  doReliableRegistration(flags.registration_backoff_factor * 2);
}


void Slave::authenticationTimeout(Future<bool> future)
{
  // NOTE: Discarded future results in a retry in '_authenticate()'.
  // Also note that a 'discard' here is safe even if another
  // authenticator is in progress because this copy of the future
  // corresponds to the original authenticator that started the timer.
  if (future.discard()) { // This is a no-op if the future is already ready.
    LOG(WARNING) << "Authentication timed out";
  }
}


void Slave::registered(
    const UPID& from,
    const SlaveID& slaveId,
    const MasterSlaveConnection& connection)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring registration message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  CHECK_SOME(master);

  if (connection.has_total_ping_timeout_seconds()) {
    masterPingTimeout = Seconds(connection.total_ping_timeout_seconds());
  } else {
    masterPingTimeout = DEFAULT_MASTER_PING_TIMEOUT();
  }

  switch (state) {
    case DISCONNECTED: {
      LOG(INFO) << "Registered with master " << master.get()
                << "; given agent ID " << slaveId;

      state = RUNNING;

      // Cancel the pending registration timer to avoid spurious attempts
      // at reregistration. `Clock::cancel` is idempotent, so this call
      // is safe even if no timer is active or pending.
      Clock::cancel(agentRegistrationTimer);

      taskStatusUpdateManager->resume(); // Resume status updates.

      info.mutable_id()->CopyFrom(slaveId); // Store the slave id.

      // Create the slave meta directory.
      paths::createSlaveDirectory(metaDir, slaveId);

      // Checkpoint slave info.
      const string path = paths::getSlaveInfoPath(metaDir, slaveId);

      VLOG(1) << "Checkpointing SlaveInfo to '" << path << "'";

      {
        // The `SlaveInfo.resources` does not include dynamic reservations,
        // which means it cannot contain reservation refinements, so
        // `downgradeResources` should always succeed.
        SlaveInfo info_ = info;

        Try<Nothing> result = downgradeResources(info_.mutable_resources());
        CHECK_SOME(result);

        CHECK_SOME(state::checkpoint(path, info_));
      }

      // We start the local resource providers daemon once the agent is
      // running, so the resource providers can use the agent API.
      localResourceProviderDaemon->start(info.id());

      // Setup a timer so that the agent attempts to re-register if it
      // doesn't receive a ping from the master for an extended period
      // of time. This needs to be done once registered, in case we
      // never receive an initial ping.
      Clock::cancel(pingTimer);

      pingTimer = delay(
          masterPingTimeout,
          self(),
          &Slave::pingTimeout,
          detection);

      break;
    }
    case RUNNING:
      // Already registered!
      if (info.id() != slaveId) {
       EXIT(EXIT_FAILURE)
         << "Registered but got wrong id: " << slaveId
         << " (expected: " << info.id() << "). Committing suicide";
      }
      LOG(WARNING) << "Already registered with master " << master.get();

      break;
    case TERMINATING:
      LOG(WARNING) << "Ignoring registration because agent is terminating";
      break;
    case RECOVERING:
    default:
      LOG(FATAL) << "Unexpected agent state " << state;
      break;
  }

  // If this agent can support resource providers or has had any oversubscribed
  // resources set, send an `UpdateSlaveMessage` to the master to inform it of a
  // possible changes between completion of recovery and agent registration.
  if (capabilities.resourceProvider || oversubscribedResources.isSome()) {
    UpdateSlaveMessage message = generateUpdateSlaveMessage();

    LOG(INFO) << "Forwarding agent update " << JSON::protobuf(message);

    send(master.get(), message);
  }
}


void Slave::reregistered(
    const UPID& from,
    const SlaveID& slaveId,
    const vector<ReconcileTasksMessage>& reconciliations,
    const MasterSlaveConnection& connection)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring re-registration message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  CHECK_SOME(master);

  if (info.id() != slaveId) {
    EXIT(EXIT_FAILURE)
      << "Re-registered but got wrong id: " << slaveId
      << " (expected: " << info.id() << "). Committing suicide";
  }

  if (connection.has_total_ping_timeout_seconds()) {
    masterPingTimeout = Seconds(connection.total_ping_timeout_seconds());
  } else {
    masterPingTimeout = DEFAULT_MASTER_PING_TIMEOUT();
  }

  switch (state) {
    case DISCONNECTED:
      LOG(INFO) << "Re-registered with master " << master.get();
      state = RUNNING;
      taskStatusUpdateManager->resume(); // Resume status updates.

      // We start the local resource providers daemon once the agent is
      // running, so the resource providers can use the agent API.
      localResourceProviderDaemon->start(info.id());

      // Setup a timer so that the agent attempts to re-register if it
      // doesn't receive a ping from the master for an extended period
      // of time. This needs to be done once re-registered, in case we
      // never receive an initial ping.
      Clock::cancel(pingTimer);

      pingTimer = delay(
          masterPingTimeout,
          self(),
          &Slave::pingTimeout,
          detection);

      break;
    case RUNNING:
      LOG(WARNING) << "Already re-registered with master " << master.get();
      break;
    case TERMINATING:
      LOG(WARNING) << "Ignoring re-registration because agent is terminating";
      return;
    case RECOVERING:
      // It's possible to receive a message intended for the previous
      // run of the slave here. Short term we can leave this as is and
      // crash in this case. Ideally responses can be tied to a
      // particular run of the slave, see:
      // https://issues.apache.org/jira/browse/MESOS-676
      // https://issues.apache.org/jira/browse/MESOS-677
    default:
      LOG(FATAL) << "Unexpected agent state " << state;
      return;
  }

  // If this agent can support resource providers or has had any oversubscribed
  // resources set, send an `UpdateSlaveMessage` to the master to inform it of a
  // possible changes between completion of recovery and agent registration.
  if (capabilities.resourceProvider || oversubscribedResources.isSome()) {
    UpdateSlaveMessage message = generateUpdateSlaveMessage();

    LOG(INFO) << "Forwarding agent update " << JSON::protobuf(message);
    send(master.get(), message);
  }

  // Reconcile any tasks per the master's request.
  foreach (const ReconcileTasksMessage& reconcile, reconciliations) {
    Framework* framework = getFramework(reconcile.framework_id());

    foreach (const TaskStatus& status, reconcile.statuses()) {
      const TaskID& taskId = status.task_id();

      bool known = false;
      if (framework != nullptr) {
        known = framework->hasTask(taskId);
      }

      // Send a terminal status update for each task that is known to
      // the master but not known to the agent. This ensures that the
      // master will cleanup any state associated with the task, which
      // is not running. We send TASK_DROPPED to partition-aware
      // frameworks; frameworks that are not partition-aware are sent
      // TASK_LOST for backward compatibility.
      //
      // If the task is known to the agent, we don't need to send a
      // status update to the master: because the master already knows
      // about the task, any subsequent status updates will be
      // propagated correctly.
      if (!known) {
        // NOTE: The `framework` field of the `ReconcileTasksMessage`
        // is only set by masters running Mesos 1.1.0 or later. If the
        // field is unset, we assume the framework is not partition-aware.
        mesos::TaskState taskState = TASK_LOST;

        if (reconcile.has_framework() &&
            protobuf::frameworkHasCapability(
                reconcile.framework(),
                FrameworkInfo::Capability::PARTITION_AWARE)) {
          taskState = TASK_DROPPED;
        }

        LOG(WARNING) << "Agent reconciling task " << taskId
                     << " of framework " << reconcile.framework_id()
                     << " in state " << taskState
                     << ": task unknown to the agent";

        const StatusUpdate update = protobuf::createStatusUpdate(
            reconcile.framework_id(),
            info.id(),
            taskId,
            taskState,
            TaskStatus::SOURCE_SLAVE,
            id::UUID::random(),
            "Reconciliation: task unknown to the agent",
            TaskStatus::REASON_RECONCILIATION);

        // NOTE: We can't use statusUpdate() here because it drops
        // updates for unknown frameworks.
        taskStatusUpdateManager->update(update, info.id())
          .onAny(defer(self(),
                       &Slave::___statusUpdate,
                       lambda::_1,
                       update,
                       UPID()));
      }
    }
  }
}


void Slave::doReliableRegistration(Duration maxBackoff)
{
  if (master.isNone()) {
    LOG(INFO) << "Skipping registration because no master present";
    return;
  }

  if (credential.isSome() && !authenticated) {
    LOG(INFO) << "Skipping registration because not authenticated";
    return;
  }

  if (state == RUNNING) { // Slave (re-)registered with the master.
    return;
  }

  if (state == TERMINATING) {
    LOG(INFO) << "Skipping registration because agent is terminating";
    return;
  }

  CHECK(state == DISCONNECTED) << state;

  CHECK_NE("cleanup", flags.recover);

  // Ensure there is a link to the master before we start
  // communicating with it. We want to link after the initial
  // registration backoff in order to avoid all of the agents
  // establishing connections with the master at once.
  // See MESOS-5330.
  link(master.get());

  SlaveInfo slaveInfo = info;

  // The `SlaveInfo.resources` does not include dynamic reservations,
  // which means it cannot contain reservation refinements, so
  // `downgradeResources` should always succeed.
  Try<Nothing> result = downgradeResources(slaveInfo.mutable_resources());
  CHECK_SOME(result);

  RepeatedPtrField<Resource> checkpointedResources_ = checkpointedResources;

  // If the checkpointed resources don't have reservation refinements,
  // send them to the master in "pre-reservation-refinement" format
  // for backward compatibility with old masters. If downgrading is
  // not possible without losing information, send the resources in
  // the "post-reservation-refinement" format. We ignore the return
  // value of `downgradeResources` because for now, we send the result
  // either way.
  //
  // TODO(mpark): Do something smarter with the result once something
  // like a master capability is introduced.
  downgradeResources(&checkpointedResources_);

  if (!slaveInfo.has_id()) {
    // Registering for the first time.
    RegisterSlaveMessage message;
    message.set_version(MESOS_VERSION);
    message.mutable_slave()->CopyFrom(slaveInfo);

    message.mutable_agent_capabilities()->CopyFrom(
        capabilities.toRepeatedPtrField());

    // Include checkpointed resources.
    message.mutable_checkpointed_resources()->CopyFrom(checkpointedResources_);

    message.mutable_resource_version_uuid()->set_value(
        resourceVersion.toBytes());

    send(master.get(), message);
  } else {
    // Re-registering, so send tasks running.
    ReregisterSlaveMessage message;
    message.set_version(MESOS_VERSION);

    message.mutable_agent_capabilities()->CopyFrom(
        capabilities.toRepeatedPtrField());

    // Include checkpointed resources.
    message.mutable_checkpointed_resources()->CopyFrom(checkpointedResources_);

    message.mutable_resource_version_uuid()->set_value(
        resourceVersion.toBytes());

    message.mutable_slave()->CopyFrom(slaveInfo);

    foreachvalue (Framework* framework, frameworks) {
      message.add_frameworks()->CopyFrom(framework->info);

      // TODO(bmahler): We need to send the executors for these
      // pending tasks, and we need to send exited events if they
      // cannot be launched, see MESOS-1715, MESOS-1720, MESOS-1800.
      typedef hashmap<TaskID, TaskInfo> TaskMap;
      foreachvalue (const TaskMap& tasks, framework->pendingTasks) {
        foreachvalue (const TaskInfo& task, tasks) {
          message.add_tasks()->CopyFrom(protobuf::createTask(
              task, TASK_STAGING, framework->id()));
        }
      }

      foreachvalue (Executor* executor, framework->executors) {
        // Add launched, terminated, and queued tasks.
        // Note that terminated executors will only have terminated
        // unacknowledged tasks.
        // Note that for each task the latest state and status update
        // state (if any) is also included.
        foreachvalue (Task* task, executor->launchedTasks) {
          message.add_tasks()->CopyFrom(*task);
        }

        foreachvalue (Task* task, executor->terminatedTasks) {
          message.add_tasks()->CopyFrom(*task);
        }

        foreachvalue (const TaskInfo& task, executor->queuedTasks) {
          message.add_tasks()->CopyFrom(protobuf::createTask(
              task, TASK_STAGING, framework->id()));
        }

        // Do not re-register with Command (or Docker) Executors
        // because the master doesn't store them; they are generated
        // by the slave.
        if (!executor->isGeneratedForCommandTask()) {
          // Ignore terminated executors because they do not consume
          // any resources.
          if (executor->state != Executor::TERMINATED) {
            ExecutorInfo* executorInfo = message.add_executor_infos();
            executorInfo->MergeFrom(executor->info);

            // Scheduler Driver will ensure the framework id is set in
            // ExecutorInfo, effectively making it a required field.
            CHECK(executorInfo->has_framework_id());
          }
        }
      }
    }

    // Add completed frameworks.
    foreachvalue (const Owned<Framework>& completedFramework,
                  completedFrameworks) {
      VLOG(1) << "Reregistering completed framework "
                << completedFramework->id();

      Archive::Framework* completedFramework_ =
        message.add_completed_frameworks();

      completedFramework_->mutable_framework_info()->CopyFrom(
          completedFramework->info);

      if (completedFramework->pid.isSome()) {
        completedFramework_->set_pid(completedFramework->pid.get());
      }

      foreach (const Owned<Executor>& executor,
               completedFramework->completedExecutors) {
        VLOG(2) << "Reregistering completed executor '" << executor->id
                << "' with " << executor->terminatedTasks.size()
                << " terminated tasks, " << executor->completedTasks.size()
                << " completed tasks";

        foreachvalue (const Task* task, executor->terminatedTasks) {
          VLOG(2) << "Reregistering terminated task " << task->task_id();
          completedFramework_->add_tasks()->CopyFrom(*task);
        }

        foreach (const shared_ptr<Task>& task, executor->completedTasks) {
          VLOG(2) << "Reregistering completed task " << task->task_id();
          completedFramework_->add_tasks()->CopyFrom(*task);
        }
      }
    }

    // If the resources don't have reservation refinements, send them
    // to the master in "pre-reservation-refinement" format for backward
    // compatibility with old masters. If downgrading is not possible
    // without losing information, send the resources in the
    // "post-reservation-refinement" format. We ignore the return value of
    // `downgradeResources` because for now, we send the result either way.
    //
    // TODO(mpark): Do something smarter with the result once something
    // like a master capability is introduced.
    foreach (Task& task, *message.mutable_tasks()) {
      downgradeResources(task.mutable_resources());
    }

    foreach (ExecutorInfo& executor, *message.mutable_executor_infos()) {
      downgradeResources(executor.mutable_resources());
    }

    foreach (Archive::Framework& completedFramework,
             *message.mutable_completed_frameworks()) {
      foreach (Task& task, *completedFramework.mutable_tasks()) {
        downgradeResources(task.mutable_resources());
      }
    }

    CHECK_SOME(master);
    send(master.get(), message);
  }

  // Bound the maximum backoff by 'REGISTER_RETRY_INTERVAL_MAX'.
  maxBackoff = std::min(maxBackoff, REGISTER_RETRY_INTERVAL_MAX);

  // Determine the delay for next attempt by picking a random
  // duration between 0 and 'maxBackoff'.
  Duration delay = maxBackoff * ((double) os::random() / RAND_MAX);

  VLOG(1) << "Will retry registration in " << delay << " if necessary";

  // Backoff.
  agentRegistrationTimer = process::delay(
      delay,
      self(),
      &Slave::doReliableRegistration,
      maxBackoff * 2);
}


// TODO(vinod): Instead of crashing the slave on checkpoint errors,
// send TASK_LOST to the framework.
void Slave::runTask(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId,
    const UPID& pid,
    const TaskInfo& task,
    const vector<ResourceVersionUUID>& resourceVersionUuids)
{
  CHECK_NE(task.has_executor(), task.has_command())
    << "Task " << task.task_id()
    << " should have either CommandInfo or ExecutorInfo set but not both";

  if (master != from) {
    LOG(WARNING) << "Ignoring run task message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  if (!frameworkInfo.has_id()) {
    LOG(ERROR) << "Ignoring run task message from " << from
               << " because it does not have a framework ID";
    return;
  }

  const ExecutorInfo executorInfo = getExecutorInfo(frameworkInfo, task);

  run(frameworkInfo, executorInfo, task, None(), resourceVersionUuids, pid);
}


void Slave::run(
    const FrameworkInfo& frameworkInfo,
    ExecutorInfo executorInfo,
    Option<TaskInfo> task,
    Option<TaskGroupInfo> taskGroup,
    const vector<ResourceVersionUUID>& resourceVersionUuids,
    const UPID& pid)
{
  CHECK_NE(task.isSome(), taskGroup.isSome())
    << "Either task or task group should be set but not both";

  auto injectAllocationInfo = [](
      RepeatedPtrField<Resource>* resources,
      const FrameworkInfo& frameworkInfo) {
    set<string> roles = protobuf::framework::getRoles(frameworkInfo);

    foreach (Resource& resource, *resources) {
      if (!resource.has_allocation_info()) {
        if (roles.size() != 1) {
          LOG(FATAL) << "Missing 'Resource.AllocationInfo' for resources"
                     << " allocated to MULTI_ROLE framework"
                     << " '" << frameworkInfo.name() << "'";
        }

        resource.mutable_allocation_info()->set_role(*roles.begin());
      }
    }
  };

  injectAllocationInfo(executorInfo.mutable_resources(), frameworkInfo);
  convertResourceFormat(
      executorInfo.mutable_resources(),
      POST_RESERVATION_REFINEMENT);

  if (task.isSome()) {
    injectAllocationInfo(task->mutable_resources(), frameworkInfo);
    convertResourceFormat(
        task->mutable_resources(),
        POST_RESERVATION_REFINEMENT);

    if (task->has_executor()) {
      injectAllocationInfo(
          task->mutable_executor()->mutable_resources(),
          frameworkInfo);
      convertResourceFormat(
          task->mutable_executor()->mutable_resources(),
          POST_RESERVATION_REFINEMENT);
    }
  }

  if (taskGroup.isSome()) {
    foreach (TaskInfo& task, *taskGroup->mutable_tasks()) {
      injectAllocationInfo(task.mutable_resources(), frameworkInfo);
      convertResourceFormat(
          task.mutable_resources(),
          POST_RESERVATION_REFINEMENT);

      if (task.has_executor()) {
        injectAllocationInfo(
            task.mutable_executor()->mutable_resources(),
            frameworkInfo);
        convertResourceFormat(
            task.mutable_executor()->mutable_resources(),
            POST_RESERVATION_REFINEMENT);
      }
    }
  }

  vector<TaskInfo> tasks;
  if (task.isSome()) {
    tasks.push_back(task.get());
  } else {
    foreach (const TaskInfo& task, taskGroup->tasks()) {
      tasks.push_back(task);
    }
  }

  const FrameworkID& frameworkId = frameworkInfo.id();

  LOG(INFO) << "Got assigned " << taskOrTaskGroup(task, taskGroup)
            << " for framework " << frameworkId;

  foreach (const TaskInfo& _task, tasks) {
    if (_task.slave_id() != info.id()) {
      LOG(WARNING)
        << "Agent " << info.id() << " ignoring running "
        << taskOrTaskGroup(_task, taskGroup) << " because "
        << "it was intended for old agent " << _task.slave_id();
      return;
    }
  }

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  // TODO(bmahler): Also ignore if we're DISCONNECTED.
  if (state == RECOVERING || state == TERMINATING) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " because the agent is " << state;

    // TODO(vinod): Consider sending a TASK_LOST here.
    // Currently it is tricky because 'statusUpdate()'
    // ignores updates for unknown frameworks.
    return;
  }

  list<Future<bool>> unschedules;

  // If we are about to create a new framework, unschedule the work
  // and meta directories from getting gc'ed.
  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    // Unschedule framework work directory.
    string path = paths::getFrameworkPath(
        flags.work_dir, info.id(), frameworkId);

    if (os::exists(path)) {
      unschedules.push_back(gc->unschedule(path));
    }

    // Unschedule framework meta directory.
    path = paths::getFrameworkPath(metaDir, info.id(), frameworkId);
    if (os::exists(path)) {
      unschedules.push_back(gc->unschedule(path));
    }

    Option<UPID> frameworkPid = None();

    if (pid != UPID()) {
      frameworkPid = pid;
    }

    framework = new Framework(
        this,
        flags,
        frameworkInfo,
        frameworkPid);

    frameworks[frameworkId] = framework;
    if (frameworkInfo.checkpoint()) {
      framework->checkpointFramework();
    }

    // Does this framework ID already exist in `completedFrameworks`?
    // If so, move the completed executors of the old framework to
    // this new framework and remove the old completed framework.
    if (completedFrameworks.contains(frameworkId)) {
      Owned<Framework>& completedFramework =
        completedFrameworks.at(frameworkId);

      framework->completedExecutors = completedFramework->completedExecutors;
      completedFrameworks.erase(frameworkId);
    }
  }

  const ExecutorID& executorId = executorInfo.executor_id();

  if (HookManager::hooksAvailable()) {
    // Set task labels from run task label decorator.
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
      (*it).mutable_labels()->CopyFrom(
          HookManager::slaveRunTaskLabelDecorator(
              (*it), executorInfo, frameworkInfo, info));
    }

    // Update `task`/`taskGroup` to reflect the task label updates.
    if (task.isSome()) {
      CHECK_EQ(1u, tasks.size());
      task->mutable_labels()->CopyFrom(tasks[0].labels());
    } else {
      for (int i = 0; i < taskGroup->tasks().size(); ++i) {
        taskGroup->mutable_tasks(i)->mutable_labels()->
          CopyFrom(tasks[i].labels());
      }
    }
  }

  CHECK_NOTNULL(framework);

  // Track the pending task / task group to ensure the framework is
  // not removed and the framework and top level executor directories
  // are not scheduled for deletion before '_run()' is called.
  //
  // TODO(bmahler): Can we instead track pending tasks within the
  // `Executor` struct by creating it earlier?
  if (task.isSome()) {
    framework->addPendingTask(executorId, task.get());
  } else {
    framework->addPendingTaskGroup(executorId, taskGroup.get());
  }

  // If we are about to create a new executor, unschedule the top
  // level work and meta directories from getting gc'ed.
  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    // Unschedule executor work directory.
    string path = paths::getExecutorPath(
        flags.work_dir, info.id(), frameworkId, executorId);

    if (os::exists(path)) {
      unschedules.push_back(gc->unschedule(path));
    }

    // Unschedule executor meta directory.
    path = paths::getExecutorPath(metaDir, info.id(), frameworkId, executorId);

    if (os::exists(path)) {
      unschedules.push_back(gc->unschedule(path));
    }
  }

  // Run the task after the unschedules are done.
  collect(unschedules)
    .onAny(defer(self(),
                 &Self::_run,
                 lambda::_1,
                 frameworkInfo,
                 executorInfo,
                 task,
                 taskGroup,
                 resourceVersionUuids));
}


void Slave::_run(
    const Future<list<bool>>& unschedules,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const Option<TaskInfo>& task,
    const Option<TaskGroupInfo>& taskGroup,
    const std::vector<ResourceVersionUUID>& resourceVersionUuids)
{
  // TODO(anindya_sinha): Consider refactoring the initial steps common
  // to `_run()` and `__run()`.
  CHECK_NE(task.isSome(), taskGroup.isSome())
    << "Either task or task group should be set but not both";

  vector<TaskInfo> tasks;
  if (task.isSome()) {
    tasks.push_back(task.get());
  } else {
    foreach (const TaskInfo& _task, taskGroup->tasks()) {
      tasks.push_back(_task);
    }
  }

  const FrameworkID& frameworkId = frameworkInfo.id();
  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " because the framework " << frameworkId
                 << " does not exist";
    return;
  }

  // We don't send a status update here because a terminating
  // framework cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " of framework " << frameworkId
                 << " because the framework is terminating";

    // Although we cannot send a status update in this case, we remove
    // the affected tasks from the pending tasks.
    foreach (const TaskInfo& _task, tasks) {
      framework->removePendingTask(_task.task_id());
    }

    if (framework->idle()) {
      removeFramework(framework);
    }

    return;
  }

  // Ignore the launch if killed in the interim. The invariant here
  // is that all tasks in the group are still pending, or all were
  // removed due to a kill arriving for one of the tasks in the group.
  bool allPending = true;
  bool allRemoved = true;
  foreach (const TaskInfo& _task, tasks) {
    if (framework->isPending(_task.task_id())) {
      allRemoved = false;
    } else {
      allPending = false;
    }
  }

  CHECK(allPending != allRemoved)
    << "BUG: The task group " << taskOrTaskGroup(task, taskGroup)
    << " was killed partially";

  if (allRemoved) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " of framework " << frameworkId
                 << " because it has been killed in the meantime";
    return;
  }

  CHECK(!unschedules.isDiscarded());

  if (!unschedules.isReady()) {
    LOG(ERROR) << "Failed to unschedule directories scheduled for gc: "
               << (unschedules.isFailed() ?
                   unschedules.failure() : "future discarded");

    // We report TASK_DROPPED to the framework because the task was
    // never launched. For non-partition-aware frameworks, we report
    // TASK_LOST for backward compatibility.
    mesos::TaskState taskState = TASK_DROPPED;
    if (!protobuf::frameworkHasCapability(
            frameworkInfo, FrameworkInfo::Capability::PARTITION_AWARE)) {
      taskState = TASK_LOST;
    }

    foreach (const TaskInfo& _task, tasks) {
      framework->removePendingTask(_task.task_id());

      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          _task.task_id(),
          taskState,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random(),
          "Could not launch the task because we failed to unschedule"
          " directories scheduled for gc",
          TaskStatus::REASON_GC_ERROR);

      // TODO(vinod): Ensure that the task status update manager
      // reliably delivers this update. Currently, we don't guarantee
      // this because removal of the framework causes the status
      // update manager to stop retrying for its un-acked updates.
      statusUpdate(update, UPID());
    }

    if (framework->idle()) {
      removeFramework(framework);
    }

    return;
  }

  // Authorize the task or tasks (as in a task group) to ensure that the
  // task user is allowed to launch tasks on the agent. If authorization
  // fails, the task (or all tasks in a task group) are not launched.
  list<Future<bool>> authorizations;

  LOG(INFO) << "Authorizing " << taskOrTaskGroup(task, taskGroup)
            << " for framework " << frameworkId;

  foreach (const TaskInfo& _task, tasks) {
    authorizations.push_back(authorizeTask(_task, frameworkInfo));
  }

  collect(authorizations)
    .onAny(defer(self(),
                 &Self::__run,
                 lambda::_1,
                 frameworkInfo,
                 executorInfo,
                 task,
                 taskGroup,
                 resourceVersionUuids));
}


void Slave::__run(
    const Future<list<bool>>& future,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const Option<TaskInfo>& task,
    const Option<TaskGroupInfo>& taskGroup,
    const vector<ResourceVersionUUID>& resourceVersionUuids)
{
  CHECK_NE(task.isSome(), taskGroup.isSome())
    << "Either task or task group should be set but not both";

  vector<TaskInfo> tasks;
  if (task.isSome()) {
    tasks.push_back(task.get());
  } else {
    foreach (const TaskInfo& _task, taskGroup->tasks()) {
      tasks.push_back(_task);
    }
  }

  const FrameworkID& frameworkId = frameworkInfo.id();
  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " because the framework " << frameworkId
                 << " does not exist";
    return;
  }

  const ExecutorID& executorId = executorInfo.executor_id();

  // We don't send a status update here because a terminating
  // framework cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " of framework " << frameworkId
                 << " because the framework is terminating";

    // Although we cannot send a status update in this case, we remove
    // the affected tasks from the list of pending tasks.
    foreach (const TaskInfo& _task, tasks) {
      framework->removePendingTask(_task.task_id());
    }

    if (framework->idle()) {
      removeFramework(framework);
    }

    return;
  }

  // Ignore the launch if killed in the interim. The invariant here
  // is that all tasks in the group are still pending, or all were
  // removed due to a kill arriving for one of the tasks in the group.
  bool allPending = true;
  bool allRemoved = true;
  foreach (const TaskInfo& _task, tasks) {
    if (framework->isPending(_task.task_id())) {
      allRemoved = false;
    } else {
      allPending = false;
    }
  }

  CHECK(allPending != allRemoved)
    << "BUG: The task group " << taskOrTaskGroup(task, taskGroup)
    << " was killed partially";

  if (allRemoved) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " of framework " << frameworkId
                 << " because it has been killed in the meantime";
    return;
  }

  foreach (const TaskInfo& _task, tasks) {
    CHECK(framework->removePendingTask(_task.task_id()));
  }

  CHECK(!future.isDiscarded());

  // Validate that the task (or tasks in case of task group) are authorized
  // to be run on this agent.
  Option<Error> error = None();
  if (!future.isReady()) {
    error = Error("Failed to authorize " + taskOrTaskGroup(task, taskGroup) +
                  ": " + future.failure());
  }

  if (error.isNone()) {
    list<bool> authorizations = future.get();

    foreach (const TaskInfo& _task, tasks) {
      bool authorized = authorizations.front();
      authorizations.pop_front();

      // If authorization for this task fails, we fail all tasks (in case of
      // a task group) with this specific error.
      if (!authorized) {
        string user = frameworkInfo.user();

        if (_task.has_command() && _task.command().has_user()) {
          user = _task.command().user();
        } else if (executorInfo.has_command() &&
                   executorInfo.command().has_user()) {
          user = executorInfo.command().user();
        }

        error = Error("Task '" + stringify(_task.task_id()) + "'"
                      " is not authorized to launch as"
                      " user '" + user + "'");

        break;
      }
    }
  }

  // For failed authorization, we send a TASK_ERROR status update for
  // all tasks.
  if (error.isSome()) {
    const TaskStatus::Reason reason = task.isSome()
      ? TaskStatus::REASON_TASK_UNAUTHORIZED
      : TaskStatus::REASON_TASK_GROUP_UNAUTHORIZED;

    LOG(ERROR) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
               << " of framework " << frameworkId
               << ": " << error->message;

    foreach (const TaskInfo& _task, tasks) {
      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          _task.task_id(),
          TASK_ERROR,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random(),
          error->message,
          reason);

      statusUpdate(update, UPID());
    }

    // Refer to the comment after 'framework->removePendingTask' above
    // for why we need this.
    if (framework->idle()) {
      removeFramework(framework);
    }

    return;
  }

  // Check task invariants.
  //
  // TODO(bbannier): Instead of copy-pasting identical code to deal
  // with cases where tasks need to be terminated, consolidate code
  // below to decouple checking from terminating.

  // If the master sent resource versions, perform a best-effort check
  // that they are consistent with the resources the task uses.
  //
  // TODO(bbannier): Also check executor resources.
  bool kill = false;
  if (!resourceVersionUuids.empty()) {
    hashset<Option<ResourceProviderID>> usedResourceProviderIds;
    foreach (const TaskInfo& _task, tasks) {
      foreach (const Resource& resource, _task.resources()) {
        usedResourceProviderIds.insert(resource.has_provider_id()
           ? Option<ResourceProviderID>(resource.provider_id())
           : None());
      }
    }

    const hashmap<Option<ResourceProviderID>, id::UUID>
      receivedResourceVersions = protobuf::parseResourceVersions(
          {resourceVersionUuids.begin(), resourceVersionUuids.end()});

    foreach (const Option<ResourceProviderID>& resourceProviderId,
             usedResourceProviderIds) {
      if (resourceProviderId.isNone()) {
        CHECK(receivedResourceVersions.contains(None()));

        if (resourceVersion != receivedResourceVersions.at(None())) {
          kill = true;
        }
      } else {
        ResourceProvider* resourceProvider =
          getResourceProvider(resourceProviderId.get());

        if (resourceProvider == nullptr ||
            resourceProvider->resourceVersion !=
              receivedResourceVersions.at(resourceProviderId.get())) {
          kill = true;
        }
      }
    }
  }

  if (kill) {
    // We report TASK_DROPPED to the framework because the task was
    // never launched. For non-partition-aware frameworks, we report
    // TASK_LOST for backward compatibility.
    mesos::TaskState taskState = TASK_DROPPED;
    if (!protobuf::frameworkHasCapability(
            frameworkInfo, FrameworkInfo::Capability::PARTITION_AWARE)) {
      taskState = TASK_LOST;
    }

    foreach (const TaskInfo& _task, tasks) {
      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          _task.task_id(),
          taskState,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random(),
          "Tasks assumes outdated resource state",
          TaskStatus::REASON_INVALID_OFFERS,
          executorId);

      statusUpdate(update, UPID());
    }

    // Refer to the comment after 'framework->removePendingTask' above
    // for why we need this.
    if (framework->idle()) {
      removeFramework(framework);
    }

    return;
  }

  auto unallocated = [](const Resources& resources) {
    Resources result = resources;
    result.unallocate();
    return result;
  };

  CHECK_EQ(kill, false);

  // NOTE: If the task/task group or executor uses resources that are
  // checkpointed on the slave (e.g. persistent volumes), we should
  // already know about it. If the slave doesn't know about them (e.g.
  // CheckpointResourcesMessage was dropped or came out of order), we
  // send TASK_DROPPED status updates here since restarting the task
  // may succeed in the event that CheckpointResourcesMessage arrives
  // out of order.
  foreach (const TaskInfo& _task, tasks) {
    // We must unallocate the resources to check whether they are
    // contained in the unallocated total checkpointed resources.
    Resources checkpointedTaskResources =
      unallocated(_task.resources()).filter(needCheckpointing);

    foreach (const Resource& resource, checkpointedTaskResources) {
      if (!checkpointedResources.contains(resource)) {
        LOG(WARNING) << "Unknown checkpointed resource " << resource
                     << " for task " << _task
                     << " of framework " << frameworkId;

        kill = true;
        break;
      }
    }
  }

  if (kill) {
    // We report TASK_DROPPED to the framework because the task was
    // never launched. For non-partition-aware frameworks, we report
    // TASK_LOST for backward compatibility.
    mesos::TaskState taskState = TASK_DROPPED;
    if (!protobuf::frameworkHasCapability(
            frameworkInfo, FrameworkInfo::Capability::PARTITION_AWARE)) {
      taskState = TASK_LOST;
    }

    foreach (const TaskInfo& _task, tasks) {
      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          _task.task_id(),
          taskState,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random(),
          "The checkpointed resources being used by the task or task group are "
          "unknown to the agent",
          TaskStatus::REASON_RESOURCES_UNKNOWN);

      statusUpdate(update, UPID());
    }

    // Refer to the comment after 'framework->removePendingTask' above
    // for why we need this.
    if (framework->idle()) {
      removeFramework(framework);
    }

    return;
  }

  CHECK_EQ(kill, false);

  // Refer to the comment above when looping across tasks on
  // why we need to unallocate resources.
  Resources checkpointedExecutorResources =
    unallocated(executorInfo.resources()).filter(needCheckpointing);

  foreach (const Resource& resource, checkpointedExecutorResources) {
    if (!checkpointedResources.contains(resource)) {
      LOG(WARNING) << "Unknown checkpointed resource " << resource
                   << " for executor '" << executorId
                   << "' of framework " << frameworkId;

      kill = true;
      break;
    }
  }

  if (kill) {
    // We report TASK_DROPPED to the framework because the task was
    // never launched. For non-partition-aware frameworks, we report
    // TASK_LOST for backward compatibility.
    mesos::TaskState taskState = TASK_DROPPED;
    if (!protobuf::frameworkHasCapability(
            frameworkInfo, FrameworkInfo::Capability::PARTITION_AWARE)) {
      taskState = TASK_LOST;
    }

    foreach (const TaskInfo& _task, tasks) {
      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          _task.task_id(),
          taskState,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random(),
          "The checkpointed resources being used by the executor are unknown "
          "to the agent",
          TaskStatus::REASON_RESOURCES_UNKNOWN,
          executorId);

      statusUpdate(update, UPID());
    }

    // Refer to the comment after 'framework->removePendingTask' above
    // for why we need this.
    if (framework->idle()) {
      removeFramework(framework);
    }

    return;
  }

  // NOTE: The slave cannot be in 'RECOVERING' because the task would
  // have been rejected in 'run()' in that case.
  CHECK(state == DISCONNECTED || state == RUNNING || state == TERMINATING)
    << state;

  if (state == TERMINATING) {
    LOG(WARNING) << "Ignoring running " << taskOrTaskGroup(task, taskGroup)
                 << " of framework " << frameworkId
                 << " because the agent is terminating";

    // Refer to the comment after 'framework->removePendingTask' above
    // for why we need this.
    if (framework->idle()) {
      removeFramework(framework);
    }

    // We don't send a TASK_LOST here because the slave is
    // terminating.
    return;
  }

  CHECK(framework->state == Framework::RUNNING) << framework->state;

  LOG(INFO) << "Launching " << taskOrTaskGroup(task, taskGroup)
            << " for framework " << frameworkId;

  // Either send the task/task group to an executor or start a new executor
  // and queue it until the executor has started.
  Executor* executor = framework->getExecutor(executorId);

  if (executor == nullptr) {
    executor = framework->addExecutor(executorInfo);

    if (secretGenerator) {
      generateSecret(framework->id(), executor->id, executor->containerId)
        .onAny(defer(
            self(),
            &Self::launchExecutor,
            lambda::_1,
            frameworkId,
            executorId,
            taskGroup.isNone() ? task.get() : Option<TaskInfo>::none()));
    } else {
      launchExecutor(
          None(),
          frameworkId,
          executorId,
          taskGroup.isNone() ? task.get() : Option<TaskInfo>::none());
    }
  }

  CHECK_NOTNULL(executor);

  switch (executor->state) {
    case Executor::TERMINATING:
    case Executor::TERMINATED: {
      string executorState;

      if (executor->state == Executor::TERMINATING) {
        executorState = "terminating";
      } else {
        executorState = "terminated";
      }

      LOG(WARNING) << "Asked to run " << taskOrTaskGroup(task, taskGroup)
                   << "' for framework " << frameworkId
                   << " with executor '" << executorId
                   << "' which is " << executorState;

      // We report TASK_DROPPED to the framework because the task was
      // never launched. For non-partition-aware frameworks, we report
      // TASK_LOST for backward compatibility.
      mesos::TaskState taskState = TASK_DROPPED;
      if (!protobuf::frameworkHasCapability(
              frameworkInfo, FrameworkInfo::Capability::PARTITION_AWARE)) {
        taskState = TASK_LOST;
      }

      foreach (const TaskInfo& _task, tasks) {
        const StatusUpdate update = protobuf::createStatusUpdate(
            frameworkId,
            info.id(),
            _task.task_id(),
            taskState,
            TaskStatus::SOURCE_SLAVE,
            id::UUID::random(),
            "Executor " + executorState,
            TaskStatus::REASON_EXECUTOR_TERMINATED);

        statusUpdate(update, UPID());
      }

      break;
    }
    case Executor::REGISTERING:
      if (executor->checkpoint) {
        foreach (const TaskInfo& _task, tasks) {
          executor->checkpointTask(_task);
        }
      }

      if (taskGroup.isSome()) {
        executor->enqueueTaskGroup(taskGroup.get());
      } else {
        foreach (const TaskInfo& _task, tasks) {
          executor->enqueueTask(_task);
        }
      }

      LOG(INFO) << "Queued " << taskOrTaskGroup(task, taskGroup)
                << " for executor " << *executor;

      break;
    case Executor::RUNNING: {
      if (executor->checkpoint) {
        foreach (const TaskInfo& _task, tasks) {
          executor->checkpointTask(_task);
        }
      }

      // Queue tasks until the containerizer is updated
      // with new resource limits (MESOS-998).
      if (taskGroup.isSome()) {
        executor->enqueueTaskGroup(taskGroup.get());
      } else {
        foreach (const TaskInfo& _task, tasks) {
          executor->enqueueTask(_task);
        }
      }

      LOG(INFO) << "Queued " << taskOrTaskGroup(task, taskGroup)
                << " for executor " << *executor;

      publishResources()
        .then(defer(self(), [=] {
          return containerizer->update(
              executor->containerId,
              executor->allocatedResources());
        }))
        .onAny(defer(self(),
                     &Self::___run,
                     lambda::_1,
                     frameworkId,
                     executorId,
                     executor->containerId,
                     task.isSome() ? list<TaskInfo>({task.get()})
                                   : list<TaskInfo>(),
                     taskGroup.isSome() ? list<TaskGroupInfo>({taskGroup.get()})
                                        : list<TaskGroupInfo>()));

      break;
    }
    default:
      LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                 << executor->state;
      break;
  }

  // We don't perform the checks for 'removeFramework' here since
  // we're guaranteed by 'addExecutor' that 'framework->executors'
  // will be non-empty.
  CHECK(!framework->executors.empty());
}


void Slave::___run(
    const Future<Nothing>& future,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const list<TaskInfo>& tasks,
    const list<TaskGroupInfo>& taskGroups)
{
  if (!future.isReady()) {
    LOG(ERROR) << "Failed to update resources for container " << containerId
               << " of executor '" << executorId
               << "' of framework " << frameworkId
               << ", destroying container: "
               << (future.isFailed() ? future.failure() : "discarded");

    containerizer->destroy(containerId);

    Executor* executor = getExecutor(frameworkId, executorId);
    if (executor != nullptr) {
      Framework* framework = getFramework(frameworkId);
      CHECK_NOTNULL(framework);

      // Send TASK_GONE because the task was started but has now
      // been terminated. If the framework is not partition-aware,
      // we send TASK_LOST instead for backward compatibility.
      mesos::TaskState taskState = TASK_GONE;
      if (!framework->capabilities.partitionAware) {
        taskState = TASK_LOST;
      }

      ContainerTermination termination;
      termination.set_state(taskState);
      termination.set_reason(TaskStatus::REASON_CONTAINER_UPDATE_FAILED);
      termination.set_message(
          "Failed to update resources for container: " +
          (future.isFailed() ? future.failure() : "discarded"));

      executor->pendingTermination = termination;

      // TODO(jieyu): Set executor->state to be TERMINATING.
    }

    return;
  }

  // Needed for logging.
  auto tasksAndTaskGroups = [&tasks, &taskGroups]() {
    ostringstream out;
    if (!tasks.empty()) {
      vector<TaskID> taskIds;
      foreach (const TaskInfo& task, tasks) {
        taskIds.push_back(task.task_id());
      }
      out << "tasks " << stringify(taskIds);
    }

    if (!taskGroups.empty()) {
      if (!tasks.empty()) {
        out << " and ";
      }

      out << "task groups ";

      vector<vector<TaskID>> taskIds;
      for (auto it = taskGroups.begin(); it != taskGroups.end(); it++) {
        vector<TaskID> taskIds_;
        foreach (const TaskInfo& task, (*it).tasks()) {
          taskIds_.push_back(task.task_id());
        }
        taskIds.push_back(taskIds_);
      }

      out << stringify(taskIds);
    }

    return out.str();
  };

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Ignoring sending queued " << tasksAndTaskGroups()
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework does not exist";
    return;
  }

  // No need to send the task to the executor because the framework is
  // being shutdown. No need to send status update for the task as
  // well because the framework is terminating!
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring sending queued " << tasksAndTaskGroups()
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework is terminating";
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    LOG(WARNING) << "Ignoring sending queued " << tasksAndTaskGroups()
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the executor does not exist";
    return;
  }

  // This is the case where the original instance of the executor has
  // been shutdown and a new instance is brought up. No need to send
  // status update as well because it should have already been sent
  // when the original instance of the executor was shutting down.
  if (executor->containerId != containerId) {
    LOG(WARNING) << "Ignoring sending queued " << tasksAndTaskGroups()
                 << "' to executor " << *executor
                 << " because the target container " << containerId
                 << " has exited";
    return;
  }

  CHECK(executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  // No need to send the task to the executor because the executor is
  // terminating or has been terminated. No need to send status update
  // for the task as well because it will be properly handled by
  // 'executorTerminated'.
  if (executor->state != Executor::RUNNING) {
    LOG(WARNING) << "Ignoring sending queued " << tasksAndTaskGroups()
                 << " to executor " << *executor
                 << " because the executor is in "
                 << executor->state << " state";
    return;
  }

  foreach (const TaskInfo& task, tasks) {
    // This is the case where the task is killed. No need to send
    // status update because it should be handled in 'killTask'.
    if (!executor->queuedTasks.contains(task.task_id())) {
      LOG(WARNING) << "Ignoring sending queued task '" << task.task_id()
                   << "' to executor " << *executor
                   << " because the task has been killed";
      continue;
    }

    CHECK_SOME(executor->dequeueTask(task.task_id()));
    executor->addLaunchedTask(task);

    LOG(INFO) << "Sending queued task '" << task.task_id()
              << "' to executor " << *executor;

    RunTaskMessage message;
    message.mutable_framework()->MergeFrom(framework->info);
    message.mutable_task()->MergeFrom(task);

    // Note that 0.23.x executors require the 'pid' to be set
    // to decode the message, but do not use the field.
    message.set_pid(framework->pid.getOrElse(UPID()));

    executor->send(message);
  }

  foreach (const TaskGroupInfo& taskGroup, taskGroups) {
    // The invariant here is that all queued tasks in the group
    // are still queued, or all were removed due to a kill arriving
    // for one of the tasks in the group.
    bool allQueued = true;
    bool allRemoved = true;
    foreach (const TaskInfo& task, taskGroup.tasks()) {
      if (executor->queuedTasks.contains(task.task_id())) {
        allRemoved = false;
      } else {
        allQueued = false;
      }
    }

    CHECK(allQueued != allRemoved)
      << "BUG: The task group " << taskOrTaskGroup(None(), taskGroup)
      << " was killed partially";

    if (allRemoved) {
      // This is the case where the task group is killed. No need to send
      // status update because it should be handled in 'killTask'.
      LOG(WARNING) << "Ignoring sending queued task group "
                   << taskOrTaskGroup(None(), taskGroup) << " to executor "
                   << *executor << " because the task group has been killed";
      continue;
    }

    LOG(INFO) << "Sending queued task group"
              << " " << taskOrTaskGroup(None(), taskGroup)
              << " to executor " << *executor;

    foreach (const TaskInfo& task, taskGroup.tasks()) {
      CHECK_SOME(executor->dequeueTask(task.task_id()));
      executor->addLaunchedTask(task);
    }

    executor::Event event;
    event.set_type(executor::Event::LAUNCH_GROUP);

    executor::Event::LaunchGroup* launchGroup = event.mutable_launch_group();
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    executor->send(event);
  }
}


// Generates a secret for executor authentication.
Future<Secret> Slave::generateSecret(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  Principal principal(
      Option<string>::none(),
      {
        {"fid", frameworkId.value()},
        {"eid", executorId.value()},
        {"cid", containerId.value()}
      });

  return secretGenerator->generate(principal)
    .then([](const Secret& secret) -> Future<Secret> {
      Option<Error> error = common::validation::validateSecret(secret);

      if (error.isSome()) {
        return Failure(
            "Failed to validate generated secret: " + error->message);
      } else if (secret.type() != Secret::VALUE) {
        return Failure(
            "Expecting generated secret to be of VALUE type instead of " +
            stringify(secret.type()) + " type; " +
            "only VALUE type secrets are supported at this time");
      }

      return secret;
    });
}


// Launches an executor which was previously created.
void Slave::launchExecutor(
    const Option<Future<Secret>>& future,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Option<TaskInfo>& taskInfo)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Ignoring launching executor '" << executorId
                 << "' because the framework " << frameworkId
                 << " does not exist";
    return;
  }

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring launching executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework is terminating";
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    LOG(WARNING) << "Ignoring launching executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the executor does not exist";
    return;
  }

  if (executor->state == Executor::TERMINATING ||
      executor->state == Executor::TERMINATED) {
    string executorState;
    if (executor->state == Executor::TERMINATING) {
      executorState = "terminating";
    } else {
      executorState = "terminated";
    }

    LOG(WARNING) << "Ignoring launching executor " << *executor
                 << " in container " << executor->containerId
                 << " because the executor is " << executorState;

    // The framework may have shutdown this executor already, transitioning it
    // to the TERMINATING/TERMINATED state. However, the executor still exists
    // in the agent's map, so we must send status updates for any queued tasks
    // and perform cleanup via `executorTerminated`.
    ContainerTermination termination;
    termination.set_state(TASK_FAILED);
    termination.set_reason(TaskStatus::REASON_CONTAINER_LAUNCH_FAILED);
    termination.set_message("Executor " + executorState);

    executorTerminated(frameworkId, executorId, termination);

    return;
  }

  CHECK_EQ(Executor::REGISTERING, executor->state);

  Option<Secret> authenticationToken;

  if (future.isSome()) {
    if (!future->isReady()) {
      LOG(ERROR) << "Failed to launch executor " << *executor
                 << " in container " << executor->containerId
                 << " because secret generation failed: "
                 << (future->isFailed() ? future->failure() : "discarded");

      ContainerTermination termination;
      termination.set_state(TASK_FAILED);
      termination.set_reason(TaskStatus::REASON_CONTAINER_LAUNCH_FAILED);
      termination.set_message(
          "Secret generation failed: " +
          (future->isFailed() ? future->failure() : "discarded"));

      executorTerminated(frameworkId, executorId, termination);

      return;
    }

    authenticationToken = future->get();
  }

  // Tell the containerizer to launch the executor.
  // NOTE: We make a copy of the executor info because we may mutate
  // it with some default fields and resources.
  ExecutorInfo executorInfo_ = executor->info;

  // Populate the command info for default executor. We modify the ExecutorInfo
  // to avoid resetting command info upon re-registering with the master since
  // the master doesn't store them; they are generated by the slave.
  if (executorInfo_.has_type() &&
      executorInfo_.type() == ExecutorInfo::DEFAULT) {
    CHECK(!executorInfo_.has_command());

    executorInfo_.mutable_command()->CopyFrom(
        defaultExecutorCommandInfo(flags.launcher_dir, executor->user));
  }

  Resources resources = executorInfo_.resources();

  // NOTE: We modify the ExecutorInfo to include the task's
  // resources when launching the executor so that the containerizer
  // has non-zero resources to work with when the executor has
  // no resources. This should be revisited after MESOS-600.
  if (taskInfo.isSome()) {
    resources += taskInfo->resources();
  }

  executorInfo_.mutable_resources()->CopyFrom(resources);

  // Add the default container info to the executor info.
  // TODO(jieyu): Rename the flag to be default_mesos_container_info.
  if (!executorInfo_.has_container() &&
      flags.default_container_info.isSome()) {
    executorInfo_.mutable_container()->CopyFrom(
        flags.default_container_info.get());
  }

  // Bundle all the container launch fields together.
  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo_);
  containerConfig.mutable_command_info()->CopyFrom(executorInfo_.command());
  containerConfig.mutable_resources()->CopyFrom(executorInfo_.resources());
  containerConfig.set_directory(executor->directory);

  if (executor->user.isSome()) {
    containerConfig.set_user(executor->user.get());
  }

  // For both of the following cases, `ExecutorInfo.container` is what
  // we want to tell the containerizer about the container to be
  // launched:
  // (1) If this is a command task case (i.e., the framework specifies
  //     the `TaskInfo` but not `ExecutorInfo`), the
  //     `ExecutorInfo.container` is already copied from
  //     `TaskInfo.container` in `Slave::getExecutorInfo`. As a
  //     result, we should just inform the containerizer about
  //     `ExecutorInfo.container`.
  // (2) If this is a non command task (e.g., default executor, custom
  //     executor), the `ExecutorInfo.container` is what we want to
  //     tell the containerizer anyway.
  if (executorInfo_.has_container()) {
    containerConfig.mutable_container_info()
      ->CopyFrom(executorInfo_.container());
  }

  if (executor->isGeneratedForCommandTask()) {
    CHECK_SOME(taskInfo)
      << "Command (or Docker) executor does not support task group";

    containerConfig.mutable_task_info()->CopyFrom(taskInfo.get());
  }

  // Prepare environment variables for the executor.
  map<string, string> environment = executorEnvironment(
      flags,
      executorInfo_,
      executor->directory,
      info.id(),
      self(),
      authenticationToken,
      framework->info.checkpoint());

  // Prepare the filename of the pidfile, for checkpoint-enabled frameworks.
  Option<string> pidCheckpointPath = None();
  if (framework->info.checkpoint()){
    pidCheckpointPath = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(flags.work_dir),
        info.id(),
        framework->id(),
        executor->id,
        executor->containerId);
  }

  LOG(INFO) << "Launching container " << executor->containerId
            << " for executor '" << executor->id
            << "' of framework " << framework->id();

  // Launch the container.
  // NOTE: Since we modify the ExecutorInfo to include the task's
  // resources when launching the executor, these resources need to be
  // published before the containerizer preparing them. This should be
  // revisited after MESOS-600.
  publishResources(
      taskInfo.isSome() ? taskInfo->resources() : Option<Resources>::none())
    .then(defer(self(), [=] {
      return containerizer->launch(
          executor->containerId,
          containerConfig,
          environment,
          pidCheckpointPath);
    }))
    .onAny(defer(self(),
                 &Self::executorLaunched,
                 frameworkId,
                 executor->id,
                 executor->containerId,
                 lambda::_1));

  // Make sure the executor registers within the given timeout.
  delay(flags.executor_registration_timeout,
        self(),
        &Self::registerExecutorTimeout,
        frameworkId,
        executor->id,
        executor->containerId);

  return;
}


void Slave::runTaskGroup(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const TaskGroupInfo& taskGroupInfo,
    const vector<ResourceVersionUUID>& resourceVersionUuids)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring run task group message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  if (!frameworkInfo.has_id()) {
    LOG(ERROR) << "Ignoring run task group message from " << from
               << " because it does not have a framework ID";
    return;
  }

  if (taskGroupInfo.tasks().empty()) {
    LOG(ERROR) << "Ignoring run task group message from " << from
               << " for framework " << frameworkInfo.id()
               << " because it has no tasks";
    return;
  }

  run(
      frameworkInfo,
      executorInfo,
      None(),
      taskGroupInfo,
      resourceVersionUuids,
      UPID());
}


void Slave::killTask(
    const UPID& from,
    const KillTaskMessage& killTaskMessage)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring kill task message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  const FrameworkID& frameworkId = killTaskMessage.framework_id();
  const TaskID& taskId = killTaskMessage.task_id();

  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  // TODO(bmahler): Also ignore if we're DISCONNECTED.
  if (state == RECOVERING || state == TERMINATING) {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because the agent is " << state;
    // TODO(vinod): Consider sending a TASK_LOST here.
    // Currently it is tricky because 'statusUpdate()'
    // ignores updates for unknown frameworks.
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Ignoring kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such framework is running";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // We don't send a status update here because a terminating
  // framework cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring kill task " << taskId
                 << " of framework " << frameworkId
                 << " because the framework is terminating";
    return;
  }

  // If the task is pending, we send a TASK_KILLED immediately.
  // This will trigger a synchronous removal of the pending task,
  // which prevents it from being launched.
  if (framework->isPending(taskId)) {
    LOG(WARNING) << "Killing task " << taskId
                 << " of framework " << frameworkId
                 << " before it was launched";

    Option<TaskGroupInfo> taskGroup =
      framework->getTaskGroupForPendingTask(taskId);

    list<StatusUpdate> updates;
    if (taskGroup.isSome()) {
      foreach (const TaskInfo& task, taskGroup->tasks()) {
        updates.push_back(protobuf::createStatusUpdate(
            frameworkId,
            info.id(),
            task.task_id(),
            TASK_KILLED,
            TaskStatus::SOURCE_SLAVE,
            id::UUID::random(),
            "A task within the task group was killed before"
            " delivery to the executor",
            TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH,
            CHECK_NOTNONE(
                framework->getExecutorIdForPendingTask(task.task_id()))));
      }
    } else {
      updates.push_back(protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          taskId,
          TASK_KILLED,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random(),
          "Killed before delivery to the executor",
          TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH,
          CHECK_NOTNONE(
              framework->getExecutorIdForPendingTask(taskId))));
    }

    foreach (const StatusUpdate& update, updates) {
      // NOTE: Sending a terminal update (TASK_KILLED) synchronously
      // removes the task/task group from 'framework->pendingTasks'
      // and 'framework->pendingTaskGroups', so that it will not be
      // launched.
      statusUpdate(update, UPID());
    }

    return;
  }

  Executor* executor = framework->getExecutor(taskId);
  if (executor == nullptr) {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no corresponding executor is running";

    // We send a TASK_DROPPED update because this task has never been
    // launched on this slave. If the framework is not partition-aware,
    // we send TASK_LOST for backward compatibility.
    mesos::TaskState taskState = TASK_DROPPED;
    if (!framework->capabilities.partitionAware) {
      taskState = TASK_LOST;
    }

    const StatusUpdate update = protobuf::createStatusUpdate(
        frameworkId,
        info.id(),
        taskId,
        taskState,
        TaskStatus::SOURCE_SLAVE,
        id::UUID::random(),
        "Cannot find executor",
        TaskStatus::REASON_EXECUTOR_TERMINATED);

    statusUpdate(update, UPID());
    return;
  }

  switch (executor->state) {
    case Executor::REGISTERING: {
      LOG(WARNING) << "Transitioning the state of task " << taskId
                   << " of framework " << frameworkId
                   << " to TASK_KILLED because the executor is not registered";

      // This task might be part of a task group. If so, we need to
      // send a TASK_KILLED update for all tasks in the group.
      Option<TaskGroupInfo> taskGroup = executor->getQueuedTaskGroup(taskId);

      list<StatusUpdate> updates;
      if (taskGroup.isSome()) {
        foreach (const TaskInfo& task, taskGroup->tasks()) {
          updates.push_back(protobuf::createStatusUpdate(
              frameworkId,
              info.id(),
              task.task_id(),
              TASK_KILLED,
              TaskStatus::SOURCE_SLAVE,
              id::UUID::random(),
              "A task within the task group was killed before"
              " delivery to the executor",
              TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH,
              executor->id));
        }
      } else {
        updates.push_back(protobuf::createStatusUpdate(
            frameworkId,
            info.id(),
            taskId,
            TASK_KILLED,
            TaskStatus::SOURCE_SLAVE,
            id::UUID::random(),
            "Killed before delivery to the executor",
            TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH,
            executor->id));
      }

      foreach (const StatusUpdate& update, updates) {
        // NOTE: Sending a terminal update (TASK_KILLED) removes the
        // task/task group from 'executor->queuedTasks' and
        // 'executor->queuedTaskGroup', so that if the executor registers at
        // a later point in time, it won't get this task or task group.
        statusUpdate(update, UPID());
      }

      break;
    }
    case Executor::TERMINATING:
      LOG(WARNING) << "Ignoring kill task " << taskId
                   << " because the executor " << *executor
                   << " is terminating";
      break;
    case Executor::TERMINATED:
      LOG(WARNING) << "Ignoring kill task " << taskId
                   << " because the executor " << *executor
                   << " is terminated";
      break;
    case Executor::RUNNING: {
      if (executor->queuedTasks.contains(taskId)) {
        // This is the case where the task has not yet been sent to
        // the executor (e.g., waiting for containerizer update to
        // finish).

        // This task might be part of a task group. If so, we need to
        // send a TASK_KILLED update for all the other tasks.
        Option<TaskGroupInfo> taskGroup = executor->getQueuedTaskGroup(taskId);

        list<StatusUpdate> updates;
        if (taskGroup.isSome()) {
          foreach (const TaskInfo& task, taskGroup->tasks()) {
            updates.push_back(protobuf::createStatusUpdate(
                frameworkId,
                info.id(),
                task.task_id(),
                TASK_KILLED,
                TaskStatus::SOURCE_SLAVE,
                id::UUID::random(),
                "Killed before delivery to the executor",
                TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH,
                executor->id));
          }
        } else {
          updates.push_back(protobuf::createStatusUpdate(
              frameworkId,
              info.id(),
              taskId,
              TASK_KILLED,
              TaskStatus::SOURCE_SLAVE,
              id::UUID::random(),
              "Killed before delivery to the executor",
              TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH,
              executor->id));
        }

        foreach (const StatusUpdate& update, updates) {
          // NOTE: Sending a terminal update (TASK_KILLED) removes the
          // task/task group from 'executor->queuedTasks' and
          // 'executor->queuedTaskGroup', so that if the executor registers at
          // a later point in time, it won't get this task.
          statusUpdate(update, UPID());
        }
      } else {
        // Send a message to the executor and wait for
        // it to send us a status update.
        KillTaskMessage message;
        message.mutable_framework_id()->MergeFrom(frameworkId);
        message.mutable_task_id()->MergeFrom(taskId);
        if (killTaskMessage.has_kill_policy()) {
          message.mutable_kill_policy()->MergeFrom(
              killTaskMessage.kill_policy());
        }

        executor->send(message);
      }
      break;
    }
    default:
      LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                 << executor->state;
      break;
  }
}


// TODO(benh): Consider sending a boolean that specifies if the
// shut down should be graceful or immediate. Likewise, consider
// sending back a shut down acknowledgement, because otherwise you
// could get into a state where a shut down was sent, dropped, and
// therefore never processed.
void Slave::shutdownFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  // Allow shutdownFramework() only if
  // its called directly (e.g. Slave::finalize()) or
  // its a message from the currently registered master.
  if (from && master != from) {
    LOG(WARNING) << "Ignoring shutdown framework message for " << frameworkId
                 << " from " << from
                 << " because it is not from the registered master ("
                 << (master.isSome() ? stringify(master.get()) : "None") << ")";
    return;
  }

  VLOG(1) << "Asked to shut down framework " << frameworkId
          << " by " << from;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING || state == DISCONNECTED) {
    LOG(WARNING) << "Ignoring shutdown framework message for " << frameworkId
                 << " because the agent has not yet registered with the master";
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    VLOG(1) << "Cannot shut down unknown framework " << frameworkId;
    return;
  }

  switch (framework->state) {
    case Framework::TERMINATING:
      LOG(WARNING) << "Ignoring shutdown framework " << framework->id()
                   << " because it is terminating";
      break;
    case Framework::RUNNING:
      LOG(INFO) << "Shutting down framework " << framework->id();

      framework->state = Framework::TERMINATING;

      // Shut down all executors of this framework.
      // NOTE: We use 'executors.keys()' here because 'shutdownExecutor'
      // and 'removeExecutor' can remove an executor from 'executors'.
      foreach (const ExecutorID& executorId, framework->executors.keys()) {
        Executor* executor = framework->executors[executorId];
        CHECK(executor->state == Executor::REGISTERING ||
              executor->state == Executor::RUNNING ||
              executor->state == Executor::TERMINATING ||
              executor->state == Executor::TERMINATED)
          << executor->state;

        if (executor->state == Executor::REGISTERING ||
            executor->state == Executor::RUNNING) {
          _shutdownExecutor(framework, executor);
        } else if (executor->state == Executor::TERMINATED) {
          // NOTE: We call remove here to ensure we can remove an
          // executor (of a terminating framework) that is terminated
          // but waiting for acknowledgements.
          removeExecutor(framework, executor);
        } else {
          // Executor is terminating. Ignore.
        }
      }

      // Remove this framework if it has no pending executors and tasks.
      if (framework->idle()) {
        removeFramework(framework);
      }
      break;
    default:
      LOG(FATAL) << "Framework " << frameworkId
                 << " is in unexpected state " << framework->state;
      break;
  }
}


void Slave::schedulerMessage(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping message from framework " << frameworkId
                 << " because the agent is in " << state << " state";
    metrics.invalid_framework_messages++;
    return;
  }


  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Dropping message from framework " << frameworkId
                 << " because framework does not exist";
    metrics.invalid_framework_messages++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Dropping message from framework " << frameworkId
                 << " because framework is terminating";
    metrics.invalid_framework_messages++;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    LOG(WARNING) << "Dropping message for executor " << executorId
                 << " because executor does not exist";
    metrics.invalid_framework_messages++;
    return;
  }

  switch (executor->state) {
    case Executor::REGISTERING:
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // TODO(*): If executor is not yet registered, queue framework
      // message? It's probably okay to just drop it since frameworks
      // can have the executor send a message to the master to say when
      // it's ready.
      LOG(WARNING) << "Dropping message for executor " << *executor
                   << " because executor is not running";
      metrics.invalid_framework_messages++;
      break;
    case Executor::RUNNING: {
      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      executor->send(message);
      metrics.valid_framework_messages++;
      break;
    }
    default:
      LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                 << executor->state;
      break;
  }
}


void Slave::updateFramework(
    const UpdateFrameworkMessage& message)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  const FrameworkID& frameworkId = message.framework_id();
  const UPID& pid = message.pid();

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping updateFramework message for " << frameworkId
                 << " because the agent is in " << state << " state";
    metrics.invalid_framework_messages++;
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Ignoring info update for framework " << frameworkId
                 << " because it does not exist";
    return;
  }

  switch (framework->state) {
    case Framework::TERMINATING:
      LOG(WARNING) << "Ignoring info update for framework " << frameworkId
                   << " because it is terminating";
      break;
    case Framework::RUNNING: {
      LOG(INFO) << "Updating info for framework " << frameworkId
                << (pid != UPID() ? " with pid updated to " + stringify(pid)
                                  : "");

      // The framework info was added in 1.3, so it will not be set
      // if from a master older than 1.3.
      if (message.has_framework_info()) {
        framework->info.CopyFrom(message.framework_info());
        framework->capabilities = message.framework_info().capabilities();
      }

      if (pid == UPID()) {
        framework->pid = None();
      } else {
        framework->pid = pid;
      }

      if (framework->info.checkpoint()) {
        framework->checkpointFramework();
      }

      // Inform task status update manager to immediately resend any pending
      // updates.
      taskStatusUpdateManager->resume();

      break;
    }
    default:
      LOG(FATAL) << "Framework " << framework->id()
                << " is in unexpected state " << framework->state;
      break;
  }
}


void Slave::checkpointResources(
    vector<Resource> _checkpointedResources,
    bool changeTotal)
{
  // TODO(jieyu): Here we assume that CheckpointResourcesMessages are
  // ordered (i.e., slave receives them in the same order master sends
  // them). This should be true in most of the cases because TCP
  // enforces in order delivery per connection. However, the ordering
  // is technically not guaranteed because master creates multiple
  // connections to the slave in some cases (e.g., persistent socket
  // to slave breaks and master uses ephemeral socket). This could
  // potentially be solved by using a version number and rejecting
  // stale messages according to the version number.
  //
  // If CheckpointResourcesMessages are delivered out-of-order, there
  // are two cases to consider:
  //  (1) If master does not fail over, it will reconcile the state
  //      with the slave if the framework later changes the
  //      checkpointed resources. Since master is the source of truth
  //      for reservations, the inconsistency is not exposed to
  //      frameworks.
  //  (2) If master does fail over, the slave will inform the new
  //      master about the incorrect checkpointed resources. When that
  //      happens, we expect framework to reconcile based on the
  //      offers they get.

  // An agent with resource providers requires an operation feedback protocol
  // instead of simply checkpointing results by the master. Fail hard here
  // instead of applying an incompatible message.
  const bool checkpointingResourceProviderResources = std::any_of(
      _checkpointedResources.begin(),
      _checkpointedResources.end(),
      [](const Resource& resource) { return resource.has_provider_id(); });

  CHECK(!checkpointingResourceProviderResources)
    << "Resource providers must perform their own checkpointing";

  convertResourceFormat(&_checkpointedResources, POST_RESERVATION_REFINEMENT);

  Resources newCheckpointedResources = _checkpointedResources;

  if (newCheckpointedResources == checkpointedResources) {
    VLOG(1) << "Ignoring new checkpointed resources identical to the current "
            << "version: " << checkpointedResources;
    return;
  }

  // This is a sanity check to verify that the new checkpointed
  // resources are compatible with the agent resources specified
  // through the '--resources' command line flag. The resources
  // should be guaranteed compatible by the master.
  Try<Resources> _totalResources = applyCheckpointedResources(
      info.resources(),
      newCheckpointedResources);

  CHECK_SOME(_totalResources)
    << "Failed to apply checkpointed resources "
    << newCheckpointedResources << " to agent's resources "
    << info.resources();

  if (changeTotal) {
    totalResources = _totalResources.get();
  }

  // Store the target checkpoint resources. We commit the checkpoint
  // only after all operations are successful. If any of the operations
  // fail, the agent exits and the update to checkpointed resources
  // is re-attempted after the agent restarts before agent reregistration.
  //
  // Since we commit the checkpoint after all operations are successful,
  // we avoid a case of inconsistency between the master and the agent if
  // the agent restarts during handling of CheckpointResourcesMessage.

  {
    // If the checkpointed resources don't have reservation refinements,
    // checkpoint them on the agent in "pre-reservation-refinement" format
    // for backward compatibility with old agents. If downgrading is
    // not possible without losing information, checkpoint the resources in
    // the "post-reservation-refinement" format. We ignore the return
    // value of `downgradeResources` because for now, we checkpoint the result
    // either way.
    //
    // TODO(mpark): Do something smarter with the result once something
    // like agent capability requirements is introduced.
    RepeatedPtrField<Resource> newCheckpointedResources_ =
      newCheckpointedResources;

    downgradeResources(&newCheckpointedResources_);

    CHECK_SOME(state::checkpoint(
        paths::getResourcesTargetPath(metaDir),
        newCheckpointedResources_))
      << "Failed to checkpoint resources target " << newCheckpointedResources_;
  }

  Try<Nothing> syncResult = syncCheckpointedResources(
      newCheckpointedResources);

  if (syncResult.isError()) {
    // Exit the agent (without committing the checkpoint) on failure.
    EXIT(EXIT_FAILURE)
      << "Failed to sync checkpointed resources: "
      << syncResult.error();
  }

  // Rename the target checkpoint to the committed checkpoint.
  Try<Nothing> renameResult = os::rename(
      paths::getResourcesTargetPath(metaDir),
      paths::getResourcesInfoPath(metaDir));

  if (renameResult.isError()) {
    // Exit the agent since the checkpoint could not be committed.
    EXIT(EXIT_FAILURE)
      << "Failed to checkpoint resources " << newCheckpointedResources
      << ": " << renameResult.error();
  }

  LOG(INFO) << "Updated checkpointed resources from "
            << checkpointedResources << " to "
            << newCheckpointedResources;

  checkpointedResources = newCheckpointedResources;
}


void Slave::checkpointResourcesMessage(
    const vector<Resource>& checkpointedResources)
{
  checkpointResources(checkpointedResources, true);
}


Try<Nothing> Slave::syncCheckpointedResources(
    const Resources& newCheckpointedResources)
{
  Resources oldVolumes = checkpointedResources.persistentVolumes();
  Resources newVolumes = newCheckpointedResources.persistentVolumes();

  // Create persistent volumes that do not already exist.
  //
  // TODO(jieyu): Consider introducing a volume manager once we start
  // to support multiple disks, or raw disks. Depending on the
  // DiskInfo, we may want to create either directories under a root
  // directory, or LVM volumes from a given device.
  foreach (const Resource& volume, newVolumes) {
    // This is validated in master.
    CHECK(Resources::isReserved(volume));

    if (oldVolumes.contains(volume)) {
      continue;
    }

    string path = paths::getPersistentVolumePath(flags.work_dir, volume);

    // If creation of persistent volume fails, the agent exits.
    string volumeDescription = "persistent volume " +
      volume.disk().persistence().id() + " at '" + path + "'";

    // We don't take any action if the directory already exists.
    // If the volume is on a MOUNT disk then the directory would
    // be a mount point that already exists. Otherwise it is possible
    // that pre-existing data exists at this path before it's managed
    // by Mesos agent. In any case because we make sure volume destroy
    // is retried until successful, here we are not concerned about
    // them being leaked from previous persistent volumes.
    if (!os::exists(path)) {
      // If the directory does not exist, we should proceed only if the
      // target directory is successfully created.
      Try<Nothing> result = os::mkdir(path, true);
      if (result.isError()) {
        return Error("Failed to create the " +
            volumeDescription + ": " + result.error());
      }
    }
  }

  // If a persistent volume that in the slave's previous checkpointed
  // resources doesn't appear in the new checkpointed resources, this
  // implies the volume has been explicitly destroyed. We immediately
  // remove the filesystem objects for the removed volume. Note that
  // for MOUNT disks, we don't remove the root directory (mount point)
  // of the volume.
  foreach (const Resource& volume, oldVolumes) {
    if (newVolumes.contains(volume)) {
      continue;
    }

    string path = paths::getPersistentVolumePath(flags.work_dir, volume);

    LOG(INFO) << "Deleting persistent volume '"
              << volume.disk().persistence().id()
              << "' at '" << path << "'";

    if (!os::exists(path)) {
      LOG(WARNING) << "Failed to find persistent volume '"
                   << volume.disk().persistence().id()
                   << "' at '" << path << "'";
    } else {
      const Resource::DiskInfo::Source& source = volume.disk().source();

      bool removeRoot = true;
      if (source.type() == Resource::DiskInfo::Source::MOUNT) {
        removeRoot = false;
      }

      // We should proceed only if the directory is removed.
      Try<Nothing> result = os::rmdir(path, true, removeRoot);

      if (result.isError()) {
        return Error(
            "Failed to remove persistent volume '" +
            stringify(volume.disk().persistence().id()) +
            "' at '" + path + "': " + result.error());
      }
    }
  }

  return Nothing();
}


void Slave::applyOperation(const ApplyOperationMessage& message)
{
  // The operation might be from an operator API call, thus the framework ID
  // here is optional.
  Option<FrameworkID> frameworkId = message.has_framework_id()
    ? message.framework_id()
    : Option<FrameworkID>::none();

  Try<id::UUID> uuid = id::UUID::fromBytes(message.operation_uuid().value());
  if (uuid.isError()) {
    LOG(ERROR) << "Failed to parse operation UUID for operation "
               << "'" << message.operation_info().id() << "' from "
               << (frameworkId.isSome()
                     ? "framework " + stringify(frameworkId.get())
                     : "an operator API call")
               << ": " << uuid.error();
    return;
  }

  Result<ResourceProviderID> resourceProviderId =
    getResourceProviderId(message.operation_info());

  if (resourceProviderId.isError()) {
    LOG(ERROR) << "Failed to get the resource provider ID of operation "
               << "'" << message.operation_info().id() << "' "
               << "(uuid: " << uuid->toString() << ") from "
               << (frameworkId.isSome()
                     ? "framework " + stringify(frameworkId.get())
                     : "an operator API call")
               << ": " << resourceProviderId.error();
    return;
  }

  Operation* operation = new Operation(
      protobuf::createOperation(
          message.operation_info(),
          protobuf::createOperationStatus(OPERATION_PENDING),
          frameworkId,
          info.id(),
          uuid.get()));

  addOperation(operation);

  if (protobuf::isSpeculativeOperation(message.operation_info())) {
    apply(operation);
  }

  if (resourceProviderId.isSome()) {
    resourceProviderManager.applyOperation(message);
    return;
  }

  CHECK(protobuf::isSpeculativeOperation(message.operation_info()));

  // TODO(jieyu): We should drop the operation if the resource version
  // uuid in the operation does not match that of the agent. This is
  // currently not possible because if any speculative operation for
  // agent default resources fails, the agent will crash. We might
  // want to change that behavior in the future. Revisit this once we
  // change that behavior.
  Resources _checkpointedResources = totalResources.filter(needCheckpointing);

  // TODO(nfnt): Have this function return a `Result`.
  checkpointResources(
      {_checkpointedResources.begin(), _checkpointedResources.end()},
      false);

  UpdateOperationStatusMessage update =
    protobuf::createUpdateOperationStatusMessage(
        uuid.get(),
        protobuf::createOperationStatus(OPERATION_FINISHED),
        None(),
        frameworkId,
        info.id());

  updateOperation(operation, update);

  removeOperation(operation);

  // TODO(nfnt): Use the status update manager to reliably send
  // this message to the master.
  send(master.get(), update);
}


void Slave::reconcileOperations(const ReconcileOperationsMessage& message)
{
  bool containsResourceProviderOperations = false;

  foreach (
      const ReconcileOperationsMessage::Operation& operation,
      message.operations()) {
    if (operation.has_resource_provider_id()) {
      containsResourceProviderOperations = true;
      continue;
    }

    Try<id::UUID> operationUuid =
      id::UUID::fromBytes(operation.operation_uuid().value());
    CHECK_SOME(operationUuid);

    // The master reconciles when it notices that an operation is missing from
    // an `UpdateSlaveMessage`. If we cannot find an operation in the agent
    // state, we send an update to inform the master. If we do find the
    // operation, then the master and agent state are consistent and we do not
    // need to do anything.
    Operation* storedOperation = getOperation(operationUuid.get());
    if (storedOperation == nullptr) {
      // For agent default resources, we send best-effort operation status
      // updates to the master. This is satisfactory because a dropped message
      // would imply a subsequent agent reregistration, after which an
      // `UpdateSlaveMessage` would be sent with pending operations.
      UpdateOperationStatusMessage update =
        protobuf::createUpdateOperationStatusMessage(
            operationUuid.get(),
            protobuf::createOperationStatus(OPERATION_DROPPED),
            None(),
            None(),
            info.id());

      send(master.get(), update);
    }
  }

  if (containsResourceProviderOperations) {
    resourceProviderManager.reconcileOperations(message);
  }
}


void Slave::statusUpdateAcknowledgement(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const TaskID& taskId,
    const string& uuid)
{
  // Originally, all status update acknowledgements were sent from the
  // scheduler driver. We'd like to have all acknowledgements sent by
  // the master instead. See: MESOS-1389.
  // For now, we handle acknowledgements from the leading master and
  // from the scheduler driver, for backwards compatibility.
  // TODO(bmahler): Aim to have the scheduler driver no longer
  // sending acknowledgements in 0.20.0. Stop handling those messages
  // here in 0.21.0.
  // NOTE: We must reject those acknowledgements coming from
  // non-leading masters because we may have already sent the terminal
  // un-acknowledged task to the leading master! Unfortunately, the
  // master's pid will not change across runs on the same machine, so
  // we may process a message from the old master on the same machine,
  // but this is a more general problem!
  if (strings::startsWith(from.id, "master")) {
    if (state != RUNNING) {
      LOG(WARNING) << "Dropping status update acknowledgement message for "
                   << frameworkId << " because the agent is in "
                   << state << " state";
      return;
    }

    if (master != from) {
      LOG(WARNING) << "Ignoring status update acknowledgement message from "
                   << from << " because it is not the expected master: "
                   << (master.isSome() ? stringify(master.get()) : "None");
      return;
    }
  }

  taskStatusUpdateManager->acknowledgement(
      taskId, frameworkId, id::UUID::fromBytes(uuid).get())
    .onAny(defer(self(),
                 &Slave::_statusUpdateAcknowledgement,
                 lambda::_1,
                 taskId,
                 frameworkId,
                 id::UUID::fromBytes(uuid).get()));
}


void Slave::_statusUpdateAcknowledgement(
    const Future<bool>& future,
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const id::UUID& uuid)
{
  // The future could fail if this is a duplicate status update acknowledgement.
  if (!future.isReady()) {
    LOG(ERROR) << "Failed to handle status update acknowledgement (UUID: "
               << uuid << ") for task " << taskId
               << " of framework " << frameworkId << ": "
               << (future.isFailed() ? future.failure() : "future discarded");
    return;
  }

  VLOG(1) << "Task status update manager successfully handled status update"
          << " acknowledgement (UUID: " << uuid
          << ") for task " << taskId
          << " of framework " << frameworkId;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(ERROR) << "Status update acknowledgement (UUID: " << uuid
               << ") for task " << taskId
               << " of unknown framework " << frameworkId;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // Find the executor that has this update.
  Executor* executor = framework->getExecutor(taskId);
  if (executor == nullptr) {
    LOG(ERROR) << "Status update acknowledgement (UUID: " << uuid
               << ") for task " << taskId
               << " of unknown executor";
    return;
  }

  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  // If the task has reached terminal state and all its updates have
  // been acknowledged, mark it completed.
  if (executor->terminatedTasks.contains(taskId) && !future.get()) {
    executor->completeTask(taskId);
  }

  // Remove the executor if it has terminated and there are no more
  // incomplete tasks.
  if (executor->state == Executor::TERMINATED && !executor->incompleteTasks()) {
    removeExecutor(framework, executor);
  }

  // Remove this framework if it has no pending executors and tasks.
  if (framework->idle()) {
    removeFramework(framework);
  }
}


void Slave::operationStatusAcknowledgement(
    const UPID& from,
    const AcknowledgeOperationStatusMessage& acknowledgement)
{
  Try<id::UUID> operationUuid =
    id::UUID::fromBytes(acknowledgement.operation_uuid().value());
  CHECK_SOME(operationUuid);

  Operation* operation = getOperation(operationUuid.get());
  if (operation != nullptr) {
    resourceProviderManager.acknowledgeOperationStatus(acknowledgement);

    CHECK(operation->statuses_size() > 0);
    if (protobuf::isTerminalState(
            operation->statuses(operation->statuses_size() - 1).state())) {
      // Note that if this acknowledgement is dropped due to resource provider
      // disconnection, the resource provider will inform the agent about the
      // operation via an UPDATE_STATE call after it reregisters, which will
      // cause the agent to add the operation back.
      removeOperation(operation);
    }
  } else {
    LOG(WARNING) << "Dropping operation update acknowledgement with"
                 << " status_uuid " << acknowledgement.status_uuid() << " and"
                 << " operation_uuid " << acknowledgement.operation_uuid()
                 << " because the operation was not found";
  }
}


void Slave::subscribe(
    HttpConnection http,
    const Call::Subscribe& subscribe,
    Framework* framework,
    Executor* executor)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(executor);

  LOG(INFO) << "Received Subscribe request for HTTP executor " << *executor;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == TERMINATING) {
    LOG(WARNING) << "Shutting down executor " << *executor << " as the agent "
                 << "is terminating";
    http.send(ShutdownExecutorMessage());
    http.close();
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Shutting down executor " << *executor << " as the "
                 << "framework is terminating";
    http.send(ShutdownExecutorMessage());
    http.close();
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // TERMINATED is possible if the executor forks, the parent process
      // terminates and the child process (driver) tries to register!
      LOG(WARNING) << "Shutting down executor " << *executor
                   << " because it is in unexpected state " << executor->state;
      http.send(ShutdownExecutorMessage());
      http.close();
      break;
    case Executor::RUNNING:
    case Executor::REGISTERING: {
      // Close the earlier connection if one existed. This can even
      // be a retried Subscribe request from an already connected
      // executor.
      if (executor->http.isSome()) {
        LOG(WARNING) << "Closing already existing HTTP connection from "
                     << "executor " << *executor;
        executor->http->close();
      }

      executor->state = Executor::RUNNING;

      // Save the connection for the executor.
      executor->http = http;
      executor->pid = None();

      if (framework->info.checkpoint()) {
        // Write a marker file to indicate that this executor
        // is HTTP based.
        const string path = paths::getExecutorHttpMarkerPath(
            metaDir,
            info.id(),
            framework->id(),
            executor->id,
            executor->containerId);

        LOG(INFO) << "Creating a marker file for HTTP based executor "
                  << *executor << " at path '" << path << "'";
        CHECK_SOME(os::touch(path));
      }

      // Here, we kill the executor if it no longer has any task or task group
      // to run (e.g., framework sent a `killTask()`). This is a workaround for
      // those executors (e.g., command executor, default executor) that do not
      // have a proper self terminating logic when they haven't received the
      // task or task group within a timeout.
      if (state != RECOVERING &&
          executor->queuedTasks.empty() &&
          executor->queuedTaskGroups.empty()) {
        CHECK(executor->launchedTasks.empty())
            << " Newly registered executor '" << executor->id
            << "' has launched tasks";

        LOG(WARNING) << "Shutting down the executor " << *executor
                     << " because it has no tasks to run";

        _shutdownExecutor(framework, executor);

        return;
      }

      // Tell executor it's registered and give it any queued tasks
      // or task groups.
      executor::Event event;
      event.set_type(executor::Event::SUBSCRIBED);

      executor::Event::Subscribed* subscribed = event.mutable_subscribed();
      subscribed->mutable_executor_info()->CopyFrom(executor->info);
      subscribed->mutable_framework_info()->MergeFrom(framework->info);
      subscribed->mutable_slave_info()->CopyFrom(info);
      subscribed->mutable_container_id()->CopyFrom(executor->containerId);

      executor->send(event);

      // Handle all the pending updates.
      // The task status update manager might have already checkpointed
      // some of these pending updates (for example, if the slave died
      // right after it checkpointed the update but before it could send
      // the ACK to the executor). This is ok because the status update
      // manager correctly handles duplicate updates.
      foreach (const Call::Update& update, subscribe.unacknowledged_updates()) {
        // NOTE: This also updates the executor's resources!
        statusUpdate(protobuf::createStatusUpdate(
            framework->id(),
            update.status(),
            info.id()),
            None());
      }

      // Split the queued tasks between the task groups and tasks.
      LinkedHashMap<TaskID, TaskInfo> queuedTasks = executor->queuedTasks;

      foreach (const TaskGroupInfo& taskGroup, executor->queuedTaskGroups) {
        foreach (const TaskInfo& task, taskGroup.tasks()) {
          queuedTasks.erase(task.task_id());
        }
      }

      publishResources()
        .then(defer(self(), [=] {
          return containerizer->update(
              executor->containerId,
              executor->allocatedResources());
        }))
        .onAny(defer(self(),
                     &Self::___run,
                     lambda::_1,
                     framework->id(),
                     executor->id,
                     executor->containerId,
                     queuedTasks.values(),
                     executor->queuedTaskGroups));

      hashmap<TaskID, TaskInfo> unackedTasks;
      foreach (const TaskInfo& task, subscribe.unacknowledged_tasks()) {
        unackedTasks[task.task_id()] = task;
      }

      // Now, if there is any task still in STAGING state and not in
      // unacknowledged 'tasks' known to the executor, the slave must
      // have died before the executor received the task! We should
      // transition it to TASK_DROPPED. We only consider/store
      // unacknowledged 'tasks' at the executor driver because if a
      // task has been acknowledged, the slave must have received an
      // update for that task and transitioned it out of STAGING!
      //
      // TODO(vinod): Consider checkpointing 'TaskInfo' instead of
      // 'Task' so that we can relaunch such tasks! Currently we don't
      // do it because 'TaskInfo.data' could be huge.
      foreachvalue (Task* task, executor->launchedTasks) {
        if (task->state() == TASK_STAGING &&
            !unackedTasks.contains(task->task_id())) {
          mesos::TaskState newTaskState = TASK_DROPPED;
          if (!protobuf::frameworkHasCapability(
                  framework->info,
                  FrameworkInfo::Capability::PARTITION_AWARE)) {
            newTaskState = TASK_LOST;
          }

          LOG(INFO) << "Transitioning STAGED task " << task->task_id()
                    << " to " << newTaskState
                    << " because it is unknown to the executor "
                    << executor->id;

          const StatusUpdate update = protobuf::createStatusUpdate(
              framework->id(),
              info.id(),
              task->task_id(),
              newTaskState,
              TaskStatus::SOURCE_SLAVE,
              id::UUID::random(),
              "Task launched during agent restart",
              TaskStatus::REASON_SLAVE_RESTARTED,
              executor->id);

          statusUpdate(update, UPID());
        }
      }

      break;
    }
    default:
      LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                 << executor->state;
      break;
  }
}


void Slave::registerExecutor(
    const UPID& from,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  LOG(INFO) << "Got registration for executor '" << executorId
            << "' of framework " << frameworkId << " from "
            << stringify(from);

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING) {
    LOG(WARNING) << "Shutting down executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the agent is still recovering";
    reply(ShutdownExecutorMessage());
    return;
  }

  if (state == TERMINATING) {
    LOG(WARNING) << "Shutting down executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the agent is terminating";
    reply(ShutdownExecutorMessage());
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Shutting down executor '" << executorId
                 << "' as the framework " << frameworkId
                 << " does not exist";

    reply(ShutdownExecutorMessage());
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Shutting down executor '" << executorId
                 << "' as the framework " << frameworkId
                 << " is terminating";

    reply(ShutdownExecutorMessage());
    return;
  }

  Executor* executor = framework->getExecutor(executorId);

  // Check the status of the executor.
  if (executor == nullptr) {
    LOG(WARNING) << "Unexpected executor '" << executorId
                 << "' registering for framework " << frameworkId;
    reply(ShutdownExecutorMessage());
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // TERMINATED is possible if the executor forks, the parent process
      // terminates and the child process (driver) tries to register!
    case Executor::RUNNING:
      LOG(WARNING) << "Shutting down executor " << *executor
                   << " because it is in unexpected state " << executor->state;
      reply(ShutdownExecutorMessage());
      break;
    case Executor::REGISTERING: {
      executor->state = Executor::RUNNING;

      // Save the pid for the executor.
      executor->pid = from;
      link(from);

      if (framework->info.checkpoint()) {
        // TODO(vinod): This checkpointing should be done
        // asynchronously as it is in the fast path of the slave!

        // Checkpoint the libprocess pid.
        string path = paths::getLibprocessPidPath(
            metaDir,
            info.id(),
            executor->frameworkId,
            executor->id,
            executor->containerId);

        VLOG(1) << "Checkpointing executor pid '"
                << executor->pid.get() << "' to '" << path << "'";
        CHECK_SOME(state::checkpoint(path, executor->pid.get()));
      }

      // Here, we kill the executor if it no longer has any task to run
      // (e.g., framework sent a `killTask()`). This is a workaround for those
      // single task executors (e.g., command executor) that do not have a
      // proper self terminating logic when they haven't received the task
      // within a timeout. Also note even if the agent restarts before sending
      // this shutdown message, it is safe because the executor driver shuts
      // down the executor if it gets disconnected from the agent before
      // registration.
      if (executor->queuedTasks.empty()) {
        CHECK(executor->launchedTasks.empty())
            << " Newly registered executor '" << executor->id
            << "' has launched tasks";

        LOG(WARNING) << "Shutting down the executor " << *executor
                     << " because it has no tasks to run";

        _shutdownExecutor(framework, executor);

        return;
      }

      // Tell executor it's registered and give it any queued tasks
      // or task groups.
      ExecutorRegisteredMessage message;
      message.mutable_executor_info()->MergeFrom(executor->info);
      message.mutable_framework_id()->MergeFrom(framework->id());
      message.mutable_framework_info()->MergeFrom(framework->info);
      message.mutable_slave_id()->MergeFrom(info.id());
      message.mutable_slave_info()->MergeFrom(info);
      executor->send(message);

      // Split the queued tasks between the task groups and tasks.
      LinkedHashMap<TaskID, TaskInfo> queuedTasks = executor->queuedTasks;

      foreach (const TaskGroupInfo& taskGroup, executor->queuedTaskGroups) {
        foreach (const TaskInfo& task, taskGroup.tasks()) {
          queuedTasks.erase(task.task_id());
        }
      }

      publishResources()
        .then(defer(self(), [=] {
          return containerizer->update(
              executor->containerId,
              executor->allocatedResources());
        }))
        .onAny(defer(self(),
                     &Self::___run,
                     lambda::_1,
                     frameworkId,
                     executorId,
                     executor->containerId,
                     queuedTasks.values(),
                     executor->queuedTaskGroups));

      break;
    }
    default:
      LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                 << executor->state;
      break;
  }
}


void Slave::reregisterExecutor(
    const UPID& from,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const vector<TaskInfo>& tasks,
    const vector<StatusUpdate>& updates)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  LOG(INFO) << "Received re-registration message from"
            << " executor '" << executorId << "'"
            << " of framework " << frameworkId;

  if (state == TERMINATING) {
    LOG(WARNING) << "Shutting down executor '" << executorId << "'"
                 << " of framework " << frameworkId
                 << " because the agent is terminating";

    reply(ShutdownExecutorMessage());
    return;
  }

  if (!frameworks.contains(frameworkId)) {
    LOG(WARNING) << "Shutting down executor '" << executorId << "'"
                 << " of framework " << frameworkId
                 << " because the framework is unknown";

    reply(ShutdownExecutorMessage());
    return;
  }

  Framework* framework = frameworks.at(frameworkId);

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Shutting down executor '" << executorId << "'"
                 << " of framework " << frameworkId
                 << " because the framework is terminating";

    reply(ShutdownExecutorMessage());
    return;
  }

  Executor* executor = framework->getExecutor(executorId);

  if (executor == nullptr) {
    LOG(WARNING) << "Shutting down unknown executor '" << executorId << "'"
                 << " of framework " << frameworkId;

    reply(ShutdownExecutorMessage());
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // TERMINATED is possible if the executor forks, the parent process
      // terminates and the child process (driver) tries to register!
      LOG(WARNING) << "Shutting down executor " << *executor
                   << " because it is in unexpected state " << executor->state;
      reply(ShutdownExecutorMessage());
      break;

    case Executor::RUNNING:
      if (flags.executor_reregistration_retry_interval.isNone()) {
        // Previously, when an executor sends a re-registration while
        // in the RUNNING state, we would shut the executor down. We
        // preserve that behavior when the optional reconnect retry
        // is not enabled.
        LOG(WARNING) << "Shutting down executor " << *executor
                     << " because it is in unexpected state "
                     << executor->state;
        reply(ShutdownExecutorMessage());
      } else {
        // When the agent is configured to retry the reconnect requests
        // to executors, we ignore any further re-registrations. This
        // is because we can't easily handle re-registering libprocess
        // based executors in the steady state, and we plan to move to
        // only allowing v1 HTTP executors (where re-subscription in
        // the steady state is supported). Also, ignoring this message
        // ensures that any executors mimicking the libprocess protocol
        // do not have any illusion of being able to re-register without
        // an agent restart (hopefully they will commit suicide if they
        // fail to re-register).
        LOG(WARNING) << "Ignoring executor re-registration message from "
                     << *executor << " because it is already registered";
      }
      break;

    case Executor::REGISTERING: {
      executor->state = Executor::RUNNING;

      executor->pid = from; // Update the pid.
      link(from);

      // Send re-registration message to the executor.
      ExecutorReregisteredMessage message;
      message.mutable_slave_id()->MergeFrom(info.id());
      message.mutable_slave_info()->MergeFrom(info);
      send(executor->pid.get(), message);

      // Handle all the pending updates.
      // The task status update manager might have already checkpointed
      // some of these pending updates (for example, if the slave died
      // right after it checkpointed the update but before it could send
      // the ACK to the executor). This is ok because the status update
      // manager correctly handles duplicate updates.
      foreach (const StatusUpdate& update, updates) {
        // NOTE: This also updates the executor's resources!
        statusUpdate(update, executor->pid.get());
      }

      // Tell the containerizer to update the resources.
      containerizer->update(
          executor->containerId,
          executor->allocatedResources())
        .onAny(defer(self(),
                     &Self::_reregisterExecutor,
                     lambda::_1,
                     frameworkId,
                     executorId,
                     executor->containerId));

      hashmap<TaskID, TaskInfo> unackedTasks;
      foreach (const TaskInfo& task, tasks) {
        unackedTasks[task.task_id()] = task;
      }

      // Now, if there is any task still in STAGING state and not in
      // unacknowledged 'tasks' known to the executor, the slave must
      // have died before the executor received the task! We should
      // transition it to TASK_DROPPED. We only consider/store
      // unacknowledged 'tasks' at the executor driver because if a
      // task has been acknowledged, the slave must have received
      // an update for that task and transitioned it out of STAGING!
      //
      // TODO(vinod): Consider checkpointing 'TaskInfo' instead of
      // 'Task' so that we can relaunch such tasks! Currently we
      // don't do it because 'TaskInfo.data' could be huge.
      foreachvalue (Task* task, executor->launchedTasks) {
        if (task->state() == TASK_STAGING &&
            !unackedTasks.contains(task->task_id())) {
          mesos::TaskState newTaskState = TASK_DROPPED;
          if (!protobuf::frameworkHasCapability(
                  framework->info,
                  FrameworkInfo::Capability::PARTITION_AWARE)) {
            newTaskState = TASK_LOST;
          }

          LOG(INFO) << "Transitioning STAGED task " << task->task_id()
                    << " to " << newTaskState
                    << " because it is unknown to the executor '"
                    << executorId << "'";

          const StatusUpdate update = protobuf::createStatusUpdate(
              frameworkId,
              info.id(),
              task->task_id(),
              newTaskState,
              TaskStatus::SOURCE_SLAVE,
              id::UUID::random(),
              "Task launched during agent restart",
              TaskStatus::REASON_SLAVE_RESTARTED,
              executorId);

          statusUpdate(update, UPID());
        }
      }

      // TODO(vinod): Similar to what we do in `registerExecutor()` the executor
      // should be shutdown if it hasn't received any tasks.
      break;
    }
  }
}


void Slave::_reregisterExecutor(
    const Future<Nothing>& future,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  if (!future.isReady()) {
    LOG(ERROR) << "Failed to update resources for container " << containerId
               << " of executor '" << executorId
               << "' of framework " << frameworkId
               << ", destroying container: "
               << (future.isFailed() ? future.failure() : "discarded");

    containerizer->destroy(containerId);

    Executor* executor = getExecutor(frameworkId, executorId);
    if (executor != nullptr) {
      Framework* framework = getFramework(frameworkId);
      CHECK_NOTNULL(framework);

      // Send TASK_GONE because the task was started but has now
      // been terminated. If the framework is not partition-aware,
      // we send TASK_LOST instead for backward compatibility.
      mesos::TaskState taskState = TASK_GONE;
      if (!framework->capabilities.partitionAware) {
        taskState = TASK_LOST;
      }

      ContainerTermination termination;
      termination.set_state(taskState);
      termination.set_reason(TaskStatus::REASON_CONTAINER_UPDATE_FAILED);
      termination.set_message(
          "Failed to update resources for container: " +
          (future.isFailed() ? future.failure() : "discarded"));

      executor->pendingTermination = termination;

      // TODO(jieyu): Set executor->state to be TERMINATING.
    }
  }
}


void Slave::reregisterExecutorTimeout()
{
  CHECK(state == RECOVERING || state == TERMINATING) << state;

  LOG(INFO) << "Cleaning up un-reregistered executors";

  foreachvalue (Framework* framework, frameworks) {
    CHECK(framework->state == Framework::RUNNING ||
          framework->state == Framework::TERMINATING)
      << framework->state;

    foreachvalue (Executor* executor, framework->executors) {
      switch (executor->state) {
        case Executor::RUNNING:     // Executor re-registered.
        case Executor::TERMINATING:
        case Executor::TERMINATED:
          break;
        case Executor::REGISTERING: {
          // If we are here, the executor must have been hung and not
          // exited! This is because if the executor properly exited,
          // it should have already been identified by the isolator
          // (via the reaper) and cleaned up!
          LOG(INFO) << "Killing un-reregistered executor " << *executor;

          containerizer->destroy(executor->containerId);

          executor->state = Executor::TERMINATING;

          // Send TASK_GONE because the task was started but has now
          // been terminated. If the framework is not partition-aware,
          // we send TASK_LOST instead for backward compatibility.
          mesos::TaskState taskState = TASK_GONE;
          if (!protobuf::frameworkHasCapability(
                  framework->info,
                  FrameworkInfo::Capability::PARTITION_AWARE)) {
            taskState = TASK_LOST;
          }

          ContainerTermination termination;
          termination.set_state(taskState);
          termination.set_reason(
              TaskStatus::REASON_EXECUTOR_REREGISTRATION_TIMEOUT);
          termination.set_message(
              "Executor did not re-register within " +
              stringify(flags.executor_reregistration_timeout));

          executor->pendingTermination = termination;
          break;
        }
        default:
          LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                     << executor->state;
          break;
      }
    }
  }

  // Signal the end of recovery.
  // TODO(greggomann): Allow the agent to complete recovery before the executor
  // re-registration timeout has elapsed. See MESOS-7539
  recoveryInfo.recovered.set(Nothing());
}


// This can be called in two ways:
// 1) When a status update from the executor is received.
// 2) When slave generates task updates (e.g LOST/KILLED/FAILED).
// NOTE: We set the pid in 'Slave::___statusUpdate()' to 'pid' so that
// whoever sent this update will get an ACK. This is important because
// we allow executors to send updates for tasks that belong to other
// executors. Currently we allow this because we cannot guarantee
// reliable delivery of status updates. Since executor driver caches
// unacked updates it is important that whoever sent the update gets
// acknowledgement for it.
void Slave::statusUpdate(StatusUpdate update, const Option<UPID>& pid)
{
  LOG(INFO) << "Handling status update " << update
            << (pid.isSome() ? " from " + stringify(pid.get()) : "");

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (!update.has_uuid()) {
    LOG(WARNING) << "Ignoring status update " << update << " without 'uuid'";
    metrics.invalid_status_updates++;
    return;
  }

  if (update.slave_id() != info.id()) {
    LOG(WARNING) << "Ignoring status update " << update << " due to "
                 << "Slave ID mismatch; expected '" << info.id()
                 << "', received '" << update.slave_id() << "'";
    metrics.invalid_status_updates++;
    return;
  }

  if (update.status().slave_id() != info.id()) {
    LOG(WARNING) << "Ignoring status update " << update << " due to "
                 << "Slave ID mismatch; expected '" << info.id()
                 << "', received '" << update.status().slave_id() << "'";
    metrics.invalid_status_updates++;
    return;
  }

  // TODO(bmahler): With the HTTP API, we must validate the UUID
  // inside the TaskStatus. For now, we ensure that the uuid of task
  // status matches the update's uuid, in case the executor is using
  // pre 0.23.x driver.
  update.mutable_status()->set_uuid(update.uuid());

  // Set the source and UUID before forwarding the status update.
  update.mutable_status()->set_source(
      pid == UPID() ? TaskStatus::SOURCE_SLAVE : TaskStatus::SOURCE_EXECUTOR);

  // Set TaskStatus.executor_id if not already set; overwrite existing
  // value if already set.
  if (update.has_executor_id()) {
    if (update.status().has_executor_id() &&
        update.status().executor_id() != update.executor_id()) {
      LOG(WARNING) << "Executor ID mismatch in status update"
                   << (pid.isSome() ? " from " + stringify(pid.get()) : "")
                   << "; overwriting received '"
                   << update.status().executor_id() << "' with expected'"
                   << update.executor_id() << "'";
    }
    update.mutable_status()->mutable_executor_id()->CopyFrom(
        update.executor_id());
  }

  Framework* framework = getFramework(update.framework_id());
  if (framework == nullptr) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " for unknown framework " << update.framework_id();
    metrics.invalid_status_updates++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // We don't send update when a framework is terminating because
  // it cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " for terminating framework " << framework->id();
    metrics.invalid_status_updates++;
    return;
  }

  if (HookManager::hooksAvailable()) {
    // Even though the hook(s) return a TaskStatus, we only use two fields:
    // container_status and labels. Remaining fields are discarded.
    TaskStatus statusFromHooks =
      HookManager::slaveTaskStatusDecorator(
          update.framework_id(), update.status());
    if (statusFromHooks.has_labels()) {
      update.mutable_status()->mutable_labels()->CopyFrom(
          statusFromHooks.labels());
    }

    if (statusFromHooks.has_container_status()) {
      update.mutable_status()->mutable_container_status()->CopyFrom(
          statusFromHooks.container_status());
    }
  }

  const TaskStatus& status = update.status();

  // For pending tasks, we must synchronously remove them
  // to guarantee that the launch is prevented.
  //
  // TODO(bmahler): Ideally we store this task as terminated
  // but with unacknowledged updates (same as the `Executor`
  // struct does).
  if (framework->isPending(status.task_id())) {
    CHECK(framework->removePendingTask(status.task_id()));

    if (framework->idle()) {
      removeFramework(framework);
    }

    metrics.valid_status_updates++;

    taskStatusUpdateManager->update(update, info.id())
      .onAny(defer(self(), &Slave::___statusUpdate, lambda::_1, update, pid));
  }

  Executor* executor = framework->getExecutor(status.task_id());
  if (executor == nullptr) {
    LOG(WARNING) << "Could not find the executor for "
                 << "status update " << update;
    metrics.valid_status_updates++;

    // NOTE: We forward the update here because this update could be
    // generated by the slave when the executor is unknown to it
    // (e.g., killTask(), _run()) or sent by an executor for a
    // task that belongs to another executor.
    // We also end up here if 1) the previous slave died after
    // checkpointing a _terminal_ update but before it could send an
    // ACK to the executor AND 2) after recovery the status update
    // manager successfully retried the update, got the ACK from the
    // scheduler and cleaned up the stream before the executor
    // re-registered. In this case, the slave cannot find the executor
    // corresponding to this task because the task has been moved to
    // 'Executor::completedTasks'.
    //
    // NOTE: We do not set the `ContainerStatus` (including the
    // `NetworkInfo` within the `ContainerStatus)  for this case,
    // because the container is unknown. We cannot use the slave IP
    // address here (for the `NetworkInfo`) since we do not know the
    // type of network isolation used for this container.
    taskStatusUpdateManager->update(update, info.id())
      .onAny(defer(self(), &Slave::___statusUpdate, lambda::_1, update, pid));

    return;
  }

  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  // Failing this validation on the executor driver used to cause the
  // driver to abort. Now that the validation is done by the slave, it
  // should shutdown the executor to be consistent.
  //
  // TODO(arojas): Once the HTTP API is the default, return a
  // 400 Bad Request response, indicating the reason in the body.
  if (status.source() == TaskStatus::SOURCE_EXECUTOR &&
      status.state() == TASK_STAGING) {
    LOG(ERROR) << "Received TASK_STAGING from executor " << *executor
               << " which is not allowed. Shutting down the executor";

    _shutdownExecutor(framework, executor);
    return;
  }

  // TODO(vinod): Revisit these semantics when we disallow executors
  // from sending updates for tasks that belong to other executors.
  if (pid.isSome() &&
      pid != UPID() &&
      executor->pid.isSome() &&
      executor->pid != pid) {
    LOG(WARNING) << "Received status update " << update << " from " << pid.get()
                 << " on behalf of a different executor '" << executor->id
                 << "' (" << executor->pid.get() << ")";
  }

  metrics.valid_status_updates++;

  // Before sending update, we need to retrieve the container status
  // if the task reached the executor. For tasks that are queued, we
  // do not need to send the container status and we must
  // synchronously transition the task to ensure that it is removed
  // from the queued tasks before the run task path continues.
  if (executor->queuedTasks.contains(status.task_id())) {
    CHECK(protobuf::isTerminalState(status.state()))
        << "Queued tasks can only be transitioned to terminal states";

    _statusUpdate(update, pid, executor->id, None());
  } else {
    // NOTE: If the executor sets the ContainerID inside the
    // ContainerStatus, that indicates that the Task this status update
    // is associated with is tied to that container (could be nested).
    // Therefore, we need to get the status of that container, instead
    // of the top level executor container.
    ContainerID containerId = executor->containerId;
    if (update.status().has_container_status() &&
        update.status().container_status().has_container_id()) {
      containerId = update.status().container_status().container_id();
    }

    containerizer->status(containerId)
      .onAny(defer(self(),
                   &Slave::_statusUpdate,
                   update,
                   pid,
                   executor->id,
                   lambda::_1));
  }
}


void Slave::_statusUpdate(
    StatusUpdate update,
    const Option<process::UPID>& pid,
    const ExecutorID& executorId,
    const Option<Future<ContainerStatus>>& containerStatus)
{
  // There can be cases where a container is already removed from the
  // containerizer before the `status` call is dispatched to the
  // containerizer, leading to the failure of the returned `Future`.
  // In such a case we should simply not update the `ContainerStatus`
  // with the return `Future` but continue processing the
  // `StatusUpdate`.
  if (containerStatus.isSome() && containerStatus->isReady()) {
    ContainerStatus* status =
      update.mutable_status()->mutable_container_status();

    status->MergeFrom(containerStatus->get());

    // Fill in the container IP address with the IP from the agent
    // PID, if not already filled in.
    //
    // TODO(karya): Fill in the IP address by looking up the executor PID.
    if (status->network_infos().size() == 0) {
      NetworkInfo* networkInfo = status->add_network_infos();
      NetworkInfo::IPAddress* ipAddress = networkInfo->add_ip_addresses();

      // Set up IPv4 address.
      //
      // NOTE: By default the protocol is set to IPv4 and therefore we
      // don't explicitly set the protocol here.
      ipAddress->set_ip_address(stringify(self().address.ip));

      // Set up IPv6 address.
      if (self().addresses.v6.isSome()) {
        ipAddress = networkInfo->add_ip_addresses();
        ipAddress->set_ip_address(stringify(self().addresses.v6->ip));
        ipAddress->set_protocol(NetworkInfo::IPv6);
      }
    }
  }


  const TaskStatus& status = update.status();

  Executor* executor = getExecutor(update.framework_id(), executorId);
  if (executor == nullptr) {
    LOG(WARNING) << "Ignoring container status update for framework "
                 << update.framework_id()
                 << "for a non-existent executor";
    return;
  }

  // We set the latest state of the task here so that the slave can
  // inform the master about the latest state (via status update or
  // ReregisterSlaveMessage message) as soon as possible. Master can use
  // this information, for example, to release resources as soon as the
  // latest state of the task reaches a terminal state. This is
  // important because task status update manager queues updates and
  // only sends one update per task at a time; the next update for a
  // task is sent only after the acknowledgement for the previous one is
  // received, which could take a long time if the framework is backed
  // up or is down.
  Try<Nothing> updated = executor->updateTaskState(status);

  // If we fail to update the task state, drop the update. Note that
  // we have to acknowledge the executor so that it does not retry.
  if (updated.isError()) {
    LOG(ERROR) << "Failed to update state of task '" << status.task_id() << "'"
               << " to " << status.state() << ": " << updated.error();

    // NOTE: This may lead to out-of-order acknowledgements since other
    // update acknowledgements may be waiting for the containerizer or
    // task status update manager.
    ___statusUpdate(Nothing(), update, pid);
    return;
  }

  if (protobuf::isTerminalState(status.state())) {
    // If the task terminated, wait until the container's resources
    // have been updated before sending the status update. Note that
    // duplicate terminal updates are not possible here because they
    // lead to an error from `Executor::updateTaskState`.
    containerizer->update(executor->containerId, executor->allocatedResources())
      .onAny(defer(self(),
                   &Slave::__statusUpdate,
                   lambda::_1,
                   update,
                   pid,
                   executor->id,
                   executor->containerId,
                   executor->checkpoint));
  } else {
    // Immediately send the status update.
    __statusUpdate(None(),
                   update,
                   pid,
                   executor->id,
                   executor->containerId,
                   executor->checkpoint);
  }
}


void Slave::__statusUpdate(
    const Option<Future<Nothing>>& future,
    const StatusUpdate& update,
    const Option<UPID>& pid,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    bool checkpoint)
{
  if (future.isSome() && !future->isReady()) {
    LOG(ERROR) << "Failed to update resources for container " << containerId
               << " of executor '" << executorId
               << "' running task " << update.status().task_id()
               << " on status update for terminal task, destroying container: "
               << (future->isFailed() ? future->failure() : "discarded");

    containerizer->destroy(containerId);

    Executor* executor = getExecutor(update.framework_id(), executorId);
    if (executor != nullptr) {
      Framework* framework = getFramework(update.framework_id());
      CHECK_NOTNULL(framework);

      // Send TASK_GONE because the task was started but has now
      // been terminated. If the framework is not partition-aware,
      // we send TASK_LOST instead for backward compatibility.
      mesos::TaskState taskState = TASK_GONE;
      if (!framework->capabilities.partitionAware) {
        taskState = TASK_LOST;
      }

      ContainerTermination termination;
      termination.set_state(taskState);
      termination.set_reason(TaskStatus::REASON_CONTAINER_UPDATE_FAILED);
      termination.set_message(
          "Failed to update resources for container: " +
          (future->isFailed() ? future->failure() : "discarded"));

      executor->pendingTermination = termination;

      // TODO(jieyu): Set executor->state to be TERMINATING.
    }
  }

  if (checkpoint) {
    // Ask the task status update manager to checkpoint and reliably send the
    // update.
    taskStatusUpdateManager->update(update, info.id(), executorId, containerId)
      .onAny(defer(self(), &Slave::___statusUpdate, lambda::_1, update, pid));
  } else {
    // Ask the task status update manager to just retry the update.
    taskStatusUpdateManager->update(update, info.id())
      .onAny(defer(self(), &Slave::___statusUpdate, lambda::_1, update, pid));
  }
}


void Slave::___statusUpdate(
    const Future<Nothing>& future,
    const StatusUpdate& update,
    const Option<UPID>& pid)
{
  CHECK_READY(future) << "Failed to handle status update " << update;

  VLOG(1) << "Task status update manager successfully handled status update "
          << update;

  if (pid == UPID()) {
    return;
  }

  StatusUpdateAcknowledgementMessage message;
  message.mutable_framework_id()->MergeFrom(update.framework_id());
  message.mutable_slave_id()->MergeFrom(update.slave_id());
  message.mutable_task_id()->MergeFrom(update.status().task_id());
  message.set_uuid(update.uuid());

  // Task status update manager successfully handled the status update.
  // Acknowledge the executor, if we have a valid pid.
  if (pid.isSome()) {
    LOG(INFO) << "Sending acknowledgement for status update " << update
              << " to " << pid.get();

    send(pid.get(), message);
  } else {
    // Acknowledge the HTTP based executor.
    Framework* framework = getFramework(update.framework_id());
    if (framework == nullptr) {
      LOG(WARNING) << "Ignoring sending acknowledgement for status update "
                   << update << " of unknown framework";
      return;
    }

    Executor* executor = framework->getExecutor(update.status().task_id());
    if (executor == nullptr) {
      // Refer to the comments in 'statusUpdate()' on when this can
      // happen.
      LOG(WARNING) << "Ignoring sending acknowledgement for status update "
                   << update << " of unknown executor";
      return;
    }

    executor->send(message);
  }
}


// NOTE: An acknowledgement for this update might have already been
// processed by the slave but not the task status update manager.
void Slave::forward(StatusUpdate update)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping status update " << update
                 << " sent by task status update manager because the agent"
                 << " is in " << state << " state";
    return;
  }

  // Ensure that task status uuid is set even if this update was sent by
  // the task status update manager after recovering a pre 0.23.x
  // slave/executor driver's updates. This allows us to simplify the
  // master code (in >= 0.27.0) to assume the uuid is always set for
  // retryable updates.
  CHECK(update.has_uuid())
    << "Expecting updates without 'uuid' to have been rejected";

  update.mutable_status()->set_uuid(update.uuid());

  // Update the status update state of the task and include the latest
  // state of the task in the status update.
  Framework* framework = getFramework(update.framework_id());
  if (framework != nullptr) {
    const TaskID& taskId = update.status().task_id();
    Executor* executor = framework->getExecutor(taskId);
    if (executor != nullptr) {
      // NOTE: We do not look for the task in queued tasks because
      // no update is expected for it until it's launched. Similarly,
      // we do not look for completed tasks because the state for a
      // completed task shouldn't be changed.
      Task* task = nullptr;
      if (executor->launchedTasks.contains(taskId)) {
        task = executor->launchedTasks[taskId];
      } else if (executor->terminatedTasks.contains(taskId)) {
        task = executor->terminatedTasks[taskId];
      }

      if (task != nullptr) {
        // We set the status update state of the task here because in
        // steady state master updates the status update state of the
        // task when it receives this update. If the master fails over,
        // slave re-registers with this task in this status update
        // state. Note that an acknowledgement for this update might be
        // enqueued on task status update manager when we are here. But
        // that is ok because the status update state will be updated
        // when the next update is forwarded to the slave.
        task->set_status_update_state(update.status().state());
        task->set_status_update_uuid(update.uuid());

        // Include the latest state of task in the update. See the
        // comments in 'statusUpdate()' on why informing the master
        // about the latest state of the task is important.
        update.set_latest_state(task->state());
      }
    }
  }

  CHECK_SOME(master);
  LOG(INFO) << "Forwarding the update " << update << " to " << master.get();

  // NOTE: We forward the update even if framework/executor/task doesn't
  // exist because the task status update manager will be expecting an
  // acknowledgement for the update. This could happen for example if
  // this is a retried terminal update and before we are here the slave
  // has already processed the acknowledgement of the original update
  // and removed the framework/executor/task. Also, slave
  // re-registration can generate updates when framework/executor/task
  // are unknown.

  // Forward the update to master.
  StatusUpdateMessage message;
  message.mutable_update()->MergeFrom(update);
  message.set_pid(self()); // The ACK will be first received by the slave.

  send(master.get(), message);
}


void Slave::executorMessage(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping framework message from executor '"
                 << executorId << "' to framework " << frameworkId
                 << " because the agent is in " << state << " state";
    metrics.invalid_framework_messages++;
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Cannot send framework message from executor '"
                 << executorId << "' to framework " << frameworkId
                 << " because framework does not exist";
    metrics.invalid_framework_messages++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring framework message from executor '"
                 << executorId << "' to framework " << frameworkId
                 << " because framework is terminating";
    metrics.invalid_framework_messages++;
    return;
  }

  ExecutorToFrameworkMessage message;
  message.mutable_slave_id()->MergeFrom(slaveId);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_data(data);

  CHECK_SOME(master);

  if (framework->pid.isSome()) {
    LOG(INFO) << "Sending message for framework " << frameworkId
              << " to " << framework->pid.get();
    send(framework->pid.get(), message);
  } else {
    LOG(INFO) << "Sending message for framework " << frameworkId
              << " through the master " << master.get();
    send(master.get(), message);
  }

  metrics.valid_framework_messages++;
}


// NOTE: The agent will respond to pings from the master even if it is
// not in the RUNNING state. This is because agent recovery might take
// longer than the master's ping timeout. We don't want to cause
// cluster churn by marking such agents unreachable. If the master
// sees a broken agent socket, it waits `agent_reregister_timeout` for
// the agent to re-register, which implies that recovery should finish
// within that (more generous) timeout.
void Slave::ping(const UPID& from, bool connected)
{
  VLOG(2) << "Received ping from " << from;

  if (!connected && state == RUNNING) {
    // This could happen if there is a one-way partition between
    // the master and slave, causing the master to get an exited
    // event and marking the slave disconnected but the slave
    // thinking it is still connected. Force a re-registration with
    // the master to reconcile.
    LOG(INFO) << "Master marked the agent as disconnected but the agent"
              << " considers itself registered! Forcing re-registration.";
    detection.discard();
  }

  // We just received a ping from the master, so reset the ping timer.
  Clock::cancel(pingTimer);

  pingTimer = delay(
      masterPingTimeout,
      self(),
      &Slave::pingTimeout,
      detection);

  send(from, PongSlaveMessage());
}


void Slave::pingTimeout(Future<Option<MasterInfo>> future)
{
  // It's possible that a new ping arrived since the timeout fired
  // and we were unable to cancel this timeout. If this occurs, don't
  // bother trying to re-detect.
  if (pingTimer.timeout().expired()) {
    LOG(INFO) << "No pings from master received within "
              << masterPingTimeout;

    future.discard();
  }
}


void Slave::exited(const UPID& pid)
{
  LOG(INFO) << "Got exited event for " << pid;

  if (master.isNone() || master.get() == pid) {
    // TODO(neilc): Try to re-link to the master (MESOS-1963).
    // TODO(benh): After so long waiting for a master, commit suicide.
    LOG(WARNING) << "Master disconnected!"
                 << " Waiting for a new master to be elected";
  }
}


Framework* Slave::getFramework(const FrameworkID& frameworkId) const
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks.at(frameworkId);
  }

  return nullptr;
}


Executor* Slave::getExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId) const
{
  Framework* framework = getFramework(frameworkId);
  if (framework != nullptr) {
    return framework->getExecutor(executorId);
  }

  return nullptr;
}


Executor* Slave::getExecutor(const ContainerID& containerId) const
{
  const ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

  // Locate the executor (for now we just loop since we don't
  // index based on container id and this likely won't have a
  // significant performance impact due to the low number of
  // executors per-agent).
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      if (rootContainerId == executor->containerId) {
        return executor;
      }
    }
  }

  return nullptr;
}


ExecutorInfo Slave::getExecutorInfo(
    const FrameworkInfo& frameworkInfo,
    const TaskInfo& task) const
{
  // In the case of tasks launched as part of a task group, the task group's
  // ExecutorInfo is injected into each TaskInfo by the master and we return
  // it here.
  if (task.has_executor()) {
    return task.executor();
  }

  ExecutorInfo executor;

  // Command executors share the same id as the task.
  executor.mutable_executor_id()->set_value(task.task_id().value());
  executor.mutable_framework_id()->CopyFrom(frameworkInfo.id());

  if (task.has_container()) {
    // Store the container info in the executor info so it will
    // be checkpointed. This allows the correct containerizer to
    // recover this task on restart.
    executor.mutable_container()->CopyFrom(task.container());
  }

  // Prepare an executor name which includes information on the
  // command being launched.
  string name = "(Task: " + task.task_id().value() + ") ";

  if (task.command().shell()) {
    if (!task.command().has_value()) {
      name += "(Command: NO COMMAND)";
    } else {
      name += "(Command: sh -c '";
      if (task.command().value().length() > 15) {
        name += task.command().value().substr(0, 12) + "...')";
      } else {
        name += task.command().value() + "')";
      }
    }
  } else {
    if (!task.command().has_value()) {
      name += "(Command: NO EXECUTABLE)";
    } else {
      string args =
        task.command().value() + ", " +
        strings::join(", ", task.command().arguments());

      if (args.length() > 15) {
        name += "(Command: [" + args.substr(0, 12) + "...])";
      } else {
        name += "(Command: [" + args + "])";
      }
    }
  }

  executor.set_name("Command Executor " + name);
  executor.set_source(task.task_id().value());

  // Copy the [uris, environment, container, user] fields from the
  // CommandInfo to get the URIs we need to download, the
  // environment variables that should get set, the necessary
  // container information, and the user to run the executor as but
  // nothing else because we need to set up the rest of the executor
  // command ourselves in order to invoke 'mesos-executor'.
  executor.mutable_command()->mutable_uris()->MergeFrom(
      task.command().uris());

  if (task.command().has_environment()) {
    executor.mutable_command()->mutable_environment()->MergeFrom(
        task.command().environment());
  }

  // Add fields which can be relevant (depending on Authorizer) for
  // authorization.

  if (task.has_labels()) {
    executor.mutable_labels()->MergeFrom(task.labels());
  }

  if (task.has_discovery()) {
    executor.mutable_discovery()->MergeFrom(task.discovery());
  }

  // Adjust the executor shutdown grace period if the kill policy is
  // set. We add a small buffer of time to avoid destroying the
  // container before `TASK_KILLED` is sent by the executor.
  //
  // TODO(alexr): Remove `MAX_REAP_INTERVAL` once the reaper signals
  // immediately after the watched process has exited.
  if (task.has_kill_policy() &&
      task.kill_policy().has_grace_period()) {
    Duration gracePeriod =
      Nanoseconds(task.kill_policy().grace_period().nanoseconds()) +
      process::MAX_REAP_INTERVAL() +
      Seconds(1);

    executor.mutable_shutdown_grace_period()->set_nanoseconds(
        gracePeriod.ns());
  }

  if (task.command().has_user()) {
    executor.mutable_command()->set_user(task.command().user());
  }

  Result<string> path = os::realpath(
      path::join(flags.launcher_dir, MESOS_EXECUTOR));

  if (path.isSome()) {
    executor.mutable_command()->set_shell(false);
    executor.mutable_command()->set_value(path.get());
    executor.mutable_command()->add_arguments(MESOS_EXECUTOR);
    executor.mutable_command()->add_arguments(
        "--launcher_dir=" + flags.launcher_dir);

    // TODO(jieyu): We should move those Mesos containerizer specific
    // logic (e.g., 'hasRootfs') to Mesos containerizer.
    bool hasRootfs = task.has_container() &&
                     task.container().type() == ContainerInfo::MESOS &&
                     task.container().mesos().has_image();

    if (hasRootfs) {
      executor.mutable_command()->add_arguments(
          "--sandbox_directory=" + flags.sandbox_directory);

#ifndef __WINDOWS__
      // NOTE: if switch_user flag is false and the slave runs under
      // a non-root user, the task will be rejected by the Posix
      // filesystem isolator. Linux filesystem isolator requires slave
      // to have root permission.
      if (flags.switch_user) {
        string user;
        if (task.command().has_user()) {
          user = task.command().user();
        } else {
          user = frameworkInfo.user();
        }

        executor.mutable_command()->add_arguments("--user=" + user);
      }
#endif // __WINDOWS__
    }
  } else {
    executor.mutable_command()->set_shell(true);
    executor.mutable_command()->set_value(
        "echo '" +
        (path.isError() ? path.error() : "No such file or directory") +
        "'; exit 1");
  }

  // Add an allowance for the command (or docker) executor. This does
  // lead to a small overcommit of resources.
  //
  // NOTE: The size of the memory is truncated here to preserve the
  // existing behavior for backward compatibility.
  // TODO(vinod): If a task is using revocable resources, mark the
  // corresponding executor resource (e.g., cpus) to be also
  // revocable. Currently, it is OK because the containerizer is
  // given task + executor resources on task launch resulting in
  // the container being correctly marked as revocable.
  Resources executorOverhead = Resources::parse(
      "cpus:" + stringify(DEFAULT_EXECUTOR_CPUS) + ";" +
      "mem:" + stringify(
          DEFAULT_EXECUTOR_MEM.bytes() / Bytes::MEGABYTES)).get();

  // If the task has an allocation role, we inject it into
  // the executor as well. Note that old masters will not
  // ensure the allocation info is set, and the agent will
  // inject this later, when storing the task/executor.
  Option<string> role = None();
  foreach (const Resource& resource, task.resources()) {
    if (role.isNone() && resource.has_allocation_info()) {
      role = resource.allocation_info().role();
    }

    // Check that the roles are consistent.
    Option<string> otherRole = resource.has_allocation_info()
        ? Option<string>(resource.allocation_info().role()) : None();

    CHECK(role == otherRole)
      << (role.isSome() ? role.get() : "None")
      << " vs " << (otherRole.isSome() ? otherRole.get() : "None");
  }

  if (role.isSome()) {
    executorOverhead.allocate(role.get());
  }

  executor.mutable_resources()->CopyFrom(executorOverhead);

  return executor;
}


void Slave::executorLaunched(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const Future<Containerizer::LaunchResult>& future)
{
  // Set up callback for executor termination. Note that we do this
  // regardless of whether or not we have successfully launched the
  // executor because even if we failed to launch the executor the
  // result of calling 'wait' will make sure everything gets properly
  // cleaned up. Note that we do this here instead of where we do
  // Containerizer::launch because we want to guarantee the contract
  // with the Containerizer that we won't call 'wait' until after the
  // launch has completed.
  containerizer->wait(containerId)
    .onAny(defer(self(),
                 &Self::executorTerminated,
                 frameworkId,
                 executorId,
                 lambda::_1));

  if (!future.isReady()) {
    LOG(ERROR) << "Container '" << containerId
               << "' for executor '" << executorId
               << "' of framework " << frameworkId
               << " failed to start: "
               << (future.isFailed() ? future.failure() : "future discarded");

    ++metrics.container_launch_errors;

    containerizer->destroy(containerId);

    Executor* executor = getExecutor(frameworkId, executorId);
    if (executor != nullptr) {
      ContainerTermination termination;
      termination.set_state(TASK_FAILED);
      termination.set_reason(TaskStatus::REASON_CONTAINER_LAUNCH_FAILED);
      termination.set_message(
          "Failed to launch container: " +
          (future.isFailed() ? future.failure() : "discarded"));

      executor->pendingTermination = termination;

      // TODO(jieyu): Set executor->state to be TERMINATING.
    }

    return;
  } else if (future.get() == Containerizer::LaunchResult::NOT_SUPPORTED) {
    LOG(ERROR) << "Container '" << containerId
               << "' for executor '" << executorId
               << "' of framework " << frameworkId
               << " failed to start: None of the enabled containerizers ("
               << flags.containerizers << ") could create a container for the "
               << "provided TaskInfo/ExecutorInfo message";

    ++metrics.container_launch_errors;
    return;
  } else if (future.get() == Containerizer::LaunchResult::ALREADY_LAUNCHED) {
    // This should be extremely rare, as the user would need to launch a
    // standalone container with a user-specified UUID that happens to
    // collide with the Agent-generated ContainerID for this launch.
    LOG(ERROR) << "Container '" << containerId
               << "' for executor '" << executorId
               << "' of framework " << frameworkId
               << " has already been launched.";
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Framework '" << frameworkId
                 << "' for executor '" << executorId
                 << "' is no longer valid";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Killing executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework is terminating";
    containerizer->destroy(containerId);
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    LOG(WARNING) << "Killing unknown executor '" << executorId
                 << "' of framework " << frameworkId;
    containerizer->destroy(containerId);
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATING:
      LOG(WARNING) << "Killing executor " << *executor
                   << " because the executor is terminating";

      containerizer->destroy(containerId);
      break;
    case Executor::REGISTERING:
    case Executor::RUNNING:
      break;
    case Executor::TERMINATED:
    default:
      LOG(FATAL) << "Executor " << *executor << " is in an unexpected state "
                 << executor->state;

      break;
  }
}


// Called by the isolator when an executor process terminates, and by
// `Slave::launchExecutor` when executor secret generation fails.
void Slave::executorTerminated(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Future<Option<ContainerTermination>>& termination)
{
  int status;
  // A termination failure indicates the containerizer could not destroy a
  // container.
  // TODO(idownes): This is a serious error so consider aborting the slave if
  // this occurs.
  if (!termination.isReady()) {
    LOG(ERROR) << "Termination of executor '" << executorId
               << "' of framework " << frameworkId
               << " failed: "
               << (termination.isFailed()
                   ? termination.failure()
                   : "discarded");
    // Set a special status for failure.
    status = -1;
  } else if (termination->isNone()) {
    LOG(ERROR) << "Termination of executor '" << executorId
               << "' of framework " << frameworkId
               << " failed: unknown container";
    // Set a special status for failure.
    status = -1;
  } else if (!termination->get().has_status()) {
    LOG(INFO) << "Executor '" << executorId
              << "' of framework " << frameworkId
              << " has terminated with unknown status";
    // Set a special status for None.
    status = -1;
  } else {
    status = termination->get().status();
    LOG(INFO) << "Executor '" << executorId
              << "' of framework " << frameworkId << " "
              << WSTRINGIFY(status);
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Framework " << frameworkId
                 << " for executor '" << executorId
                 << "' does not exist";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    LOG(WARNING) << "Executor '" << executorId
                 << "' of framework " << frameworkId
                 << " does not exist";
    return;
  }

  switch (executor->state) {
    case Executor::REGISTERING:
    case Executor::RUNNING:
    case Executor::TERMINATING: {
      ++metrics.executors_terminated;

      executor->state = Executor::TERMINATED;

      // Transition all live tasks to TASK_GONE/TASK_FAILED.
      // If the containerizer killed the executor (e.g., due to OOM event)
      // or if this is a command executor, we send TASK_FAILED status updates
      // instead of TASK_GONE.
      // NOTE: We don't send updates if the framework is terminating because we
      // don't want the task status update manager to keep retrying these
      // updates since it won't receive ACKs from the scheduler.  Also, the task
      // status update manager should have already cleaned up all the status
      // update streams for a framework that is terminating.
      if (framework->state != Framework::TERMINATING) {
        // Transition all live launched tasks. Note that the map is
        // removed from within the loop due terminal status updates.
        foreach (const TaskID& taskId, executor->launchedTasks.keys()) {
          Task* task = executor->launchedTasks.at(taskId);

          if (!protobuf::isTerminalState(task->state())) {
            sendExecutorTerminatedStatusUpdate(
                taskId, termination, frameworkId, executor);
          }
        }

        // Transition all queued tasks. Note that the map is removed
        // from within the loop due terminal status updates.
        foreach (const TaskID& taskId, executor->queuedTasks.keys()) {
          sendExecutorTerminatedStatusUpdate(
              taskId, termination, frameworkId, executor);
        }
      }

      // Only send ExitedExecutorMessage if it is not a Command (or
      // Docker) Executor because the master doesn't store them; they
      // are generated by the slave.
      // TODO(vinod): Reliably forward this message to the master.
      if (!executor->isGeneratedForCommandTask()) {
        ExitedExecutorMessage message;
        message.mutable_slave_id()->MergeFrom(info.id());
        message.mutable_framework_id()->MergeFrom(frameworkId);
        message.mutable_executor_id()->MergeFrom(executorId);
        message.set_status(status);

        if (master.isSome()) { send(master.get(), message); }
      }

      // Remove the executor if either the slave or framework is
      // terminating or there are no incomplete tasks.
      if (state == TERMINATING ||
          framework->state == Framework::TERMINATING ||
          !executor->incompleteTasks()) {
        removeExecutor(framework, executor);
      }

      // Remove this framework if it has no pending executors and tasks.
      if (framework->idle()) {
        removeFramework(framework);
      }
      break;
    }
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id()
                 << " in unexpected state " << executor->state;
      break;
  }
}


void Slave::removeExecutor(Framework* framework, Executor* executor)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(executor);

  LOG(INFO) << "Cleaning up executor " << *executor;

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // Check that this executor has terminated.
  CHECK(executor->state == Executor::TERMINATED) << executor->state;

  // Check that either 1) the executor has no tasks with pending
  // updates or 2) the slave/framework is terminating, because no
  // acknowledgements might be received.
  CHECK(!executor->incompleteTasks() ||
        state == TERMINATING ||
        framework->state == Framework::TERMINATING);

  // Write a sentinel file to indicate that this executor
  // is completed.
  if (executor->checkpoint) {
    const string path = paths::getExecutorSentinelPath(
        metaDir,
        info.id(),
        framework->id(),
        executor->id,
        executor->containerId);
    CHECK_SOME(os::touch(path));
  }

  // TODO(vinod): Move the responsibility of gc'ing to the
  // Executor struct.

  // Schedule the executor run work directory to get garbage collected.
  const string path = paths::getExecutorRunPath(
      flags.work_dir,
      info.id(),
      framework->id(),
      executor->id,
      executor->containerId);

  os::utime(path); // Update the modification time.
  garbageCollect(path)
    .then(defer(self(), &Self::detachFile, path));

  // Schedule the top level executor work directory, only if the
  // framework doesn't have any 'pending' tasks for this executor.
  if (!framework->pendingTasks.contains(executor->id)) {
    const string path = paths::getExecutorPath(
        flags.work_dir, info.id(), framework->id(), executor->id);

    // Make sure we detach both real and virtual paths for "latest"
    // symlink. We prefer users to use the virtual paths because
    // they do not expose the `work_dir` and agent ID, but the real
    // paths remains for compatibility reason.
    const string latestPath = paths::getExecutorLatestRunPath(
        flags.work_dir,
        info.id(),
        framework->id(),
        executor->id);

    const string virtualLatestPath = paths::getExecutorVirtualPath(
        framework->id(),
        executor->id);

    os::utime(path); // Update the modification time.
    garbageCollect(path)
      .then(defer(self(), [=]() {
        detachFile(latestPath);
        detachFile(virtualLatestPath);
        return Nothing();
      }));
  }

  if (executor->checkpoint) {
    // Schedule the executor run meta directory to get garbage collected.
    const string path = paths::getExecutorRunPath(
        metaDir,
        info.id(),
        framework->id(),
        executor->id,
        executor->containerId);

    os::utime(path); // Update the modification time.
    garbageCollect(path);

    // Schedule the top level executor meta directory, only if the
    // framework doesn't have any 'pending' tasks for this executor.
    if (!framework->pendingTasks.contains(executor->id)) {
      const string path = paths::getExecutorPath(
          metaDir, info.id(), framework->id(), executor->id);

      os::utime(path); // Update the modification time.
      garbageCollect(path);
    }
  }

  if (HookManager::hooksAvailable()) {
    HookManager::slaveRemoveExecutorHook(framework->info, executor->info);
  }

  framework->destroyExecutor(executor->id);
}


void Slave::removeFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO)<< "Cleaning up framework " << framework->id();

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING);

  // We only remove frameworks once they become idle.
  CHECK(framework->idle());

  // Close all task status update streams for this framework.
  taskStatusUpdateManager->cleanup(framework->id());

  // Schedule the framework work and meta directories for garbage
  // collection.
  // TODO(vinod): Move the responsibility of gc'ing to the
  // Framework struct.

  const string path = paths::getFrameworkPath(
      flags.work_dir, info.id(), framework->id());

  os::utime(path); // Update the modification time.
  garbageCollect(path);

  if (framework->info.checkpoint()) {
    // Schedule the framework meta directory to get garbage collected.
    const string path = paths::getFrameworkPath(
        metaDir, info.id(), framework->id());

    os::utime(path); // Update the modification time.
    garbageCollect(path);
  }

  frameworks.erase(framework->id());

  // Pass ownership of the framework pointer.
  completedFrameworks.set(framework->id(), Owned<Framework>(framework));

  if (state == TERMINATING && frameworks.empty()) {
    terminate(self());
  }
}


void Slave::shutdownExecutor(
    const UPID& from,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (from && master != from) {
    LOG(WARNING) << "Ignoring shutdown executor message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " from " << from << " because it is not from the"
                 << " registered master ("
                 << (master.isSome() ? stringify(master.get()) : "None") << ")";
    return;
  }

  LOG(INFO) << "Asked to shut down executor '" << executorId
            << "' of framework " << frameworkId << " by " << from;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING || state == DISCONNECTED) {
    LOG(WARNING) << "Ignoring shutdown executor message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because the agent has not yet registered with the master";
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(WARNING) << "Cannot shut down executor '" << executorId
                 << "' of unknown framework " << frameworkId;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring shutdown executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework is terminating";
    return;
  }

  if (!framework->executors.contains(executorId)) {
    LOG(WARNING) << "Ignoring shutdown of unknown executor '" << executorId
                 << "' of framework " << frameworkId;
    return;
  }

  Executor* executor = framework->executors[executorId];
  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  if (executor->state == Executor::TERMINATING) {
    LOG(WARNING) << "Ignoring shutdown executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the executor is terminating";
    return;
  }

  if (executor->state == Executor::TERMINATED) {
    LOG(WARNING) << "Ignoring shutdown executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the executor is terminated";
    return;
  }

  _shutdownExecutor(framework, executor);
}


void Slave::_shutdownExecutor(Framework* framework, Executor* executor)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(executor);

  LOG(INFO) << "Shutting down executor " << *executor;

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING)
    << executor->state;

  executor->state = Executor::TERMINATING;

  // If the executor hasn't yet registered, this message
  // will be dropped to the floor!
  executor->send(ShutdownExecutorMessage());

  // If the executor specifies shutdown grace period,
  // pass it instead of the default.
  Duration shutdownTimeout = flags.executor_shutdown_grace_period;
  if (executor->info.has_shutdown_grace_period()) {
    shutdownTimeout = Nanoseconds(
        executor->info.shutdown_grace_period().nanoseconds());
  }

  // Prepare for sending a kill if the executor doesn't comply.
  delay(shutdownTimeout,
        self(),
        &Slave::shutdownExecutorTimeout,
        framework->id(),
        executor->id,
        executor->containerId);
}


void Slave::shutdownExecutorTimeout(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(INFO) << "Framework " << frameworkId
              << " seems to have exited. Ignoring shutdown timeout"
              << " for executor '" << executorId << "'";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    VLOG(1) << "Executor '" << executorId
            << "' of framework " << frameworkId
            << " seems to have exited. Ignoring its shutdown timeout";
    return;
  }

  // Make sure this timeout is valid.
  if (executor->containerId != containerId) {
    LOG(INFO) << "A new executor " << *executor
              << " with run " << executor->containerId
              << " seems to be active. Ignoring the shutdown timeout"
              << " for the old executor run " << containerId;
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATED:
      LOG(INFO) << "Executor " << *executor << " has already terminated";
      break;
    case Executor::TERMINATING:
      LOG(INFO) << "Killing executor " << *executor;

      containerizer->destroy(executor->containerId);
      break;
    default:
      LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                 << executor->state;
      break;
  }
}


void Slave::registerExecutorTimeout(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == nullptr) {
    LOG(INFO) << "Framework " << frameworkId
              << " seems to have exited. Ignoring registration timeout"
              << " for executor '" << executorId << "'";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(INFO) << "Ignoring registration timeout for executor '" << executorId
              << "' because the  framework " << frameworkId
              << " is terminating";
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == nullptr) {
    VLOG(1) << "Executor '" << executorId
            << "' of framework " << frameworkId
            << " seems to have exited. Ignoring its registration timeout";
    return;
  }

  if (executor->containerId != containerId) {
    LOG(INFO) << "A new executor " << *executor
              << " with run " << executor->containerId
              << " seems to be active. Ignoring the registration timeout"
              << " for the old executor run " << containerId;
    return;
  }

  switch (executor->state) {
    case Executor::RUNNING:
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // Ignore the registration timeout.
      break;
    case Executor::REGISTERING: {
      LOG(INFO) << "Terminating executor " << *executor
                << " because it did not register within "
                << flags.executor_registration_timeout;

      // Immediately kill the executor.
      containerizer->destroy(containerId);

      executor->state = Executor::TERMINATING;

      ContainerTermination termination;
      termination.set_state(TASK_FAILED);
      termination.set_reason(TaskStatus::REASON_EXECUTOR_REGISTRATION_TIMEOUT);
      termination.set_message(
          "Executor did not register within " +
          stringify(flags.executor_registration_timeout));

      executor->pendingTermination = termination;
      break;
    }
    default:
      LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                 << executor->state;
      break;
  }
}


// TODO(vinod): Figure out a way to express this function via cmd line.
Duration Slave::age(double usage)
{
  return flags.gc_delay * std::max(0.0, (1.0 - flags.gc_disk_headroom - usage));
}


void Slave::checkDiskUsage()
{
  // TODO(vinod): We are making usage a Future, so that we can plug in
  // fs::usage() into async.
  // NOTE: We calculate disk usage of the file system on which the
  // slave work directory is mounted.
  Future<double>(::fs::usage(flags.work_dir))
    .onAny(defer(self(), &Slave::_checkDiskUsage, lambda::_1));
}


void Slave::_checkDiskUsage(const Future<double>& usage)
{
  if (!usage.isReady()) {
    LOG(ERROR) << "Failed to get disk usage: "
               << (usage.isFailed() ? usage.failure() : "future discarded");
  } else {
    executorDirectoryMaxAllowedAge = age(usage.get());
    LOG(INFO) << "Current disk usage " << std::setiosflags(std::ios::fixed)
              << std::setprecision(2) << 100 * usage.get() << "%."
              << " Max allowed age: " << executorDirectoryMaxAllowedAge;

    // We prune all directories whose deletion time is within
    // the next 'gc_delay - age'. Since a directory is always
    // scheduled for deletion 'gc_delay' into the future, only directories
    // that are at least 'age' old are deleted.
    gc->prune(flags.gc_delay - executorDirectoryMaxAllowedAge);
  }
  delay(flags.disk_watch_interval, self(), &Slave::checkDiskUsage);
}


Try<Nothing> Slave::compatible(
  const SlaveInfo& previous,
  const SlaveInfo& current) const
{
  // TODO(vinod): Also check for version compatibility.

  if (flags.reconfiguration_policy == "equal") {
    return compatibility::equal(previous, current);
  }

  if (flags.reconfiguration_policy == "additive") {
    return compatibility::additive(previous, current);
  }

  // Should have been validated during startup.
  UNREACHABLE();
}


// TODO(gilbert): Consider to read the Image GC config dynamically.
// For now, the Image GC config can only be updated after the agent
// restarts.
void Slave::checkImageDiskUsage()
{
  // TODO(gilbert): Container image gc is supported for docker image
  // in Mesos Containerizer for now. Add more image store gc supports
  // if necessary.
  Future<double>(::fs::usage(flags.docker_store_dir))
    .onAny(defer(self(), &Slave::_checkImageDiskUsage, lambda::_1));
}


void Slave::_checkImageDiskUsage(const Future<double>& usage)
{
  CHECK(flags.image_gc_config.isSome());

  if (!usage.isReady()) {
    LOG(ERROR) << "Failed to get image store disk usage: "
               << (usage.isFailed() ? usage.failure() : "future discarded");
  } else {
    LOG(INFO) << "Current docker image store disk usage: "
              << std::setiosflags(std::ios::fixed) << std::setprecision(2)
              << 100 * usage.get() << "%.";

    if ((flags.image_gc_config->image_disk_headroom() + usage.get()) > 1.0) {
      LOG(INFO) << "Image store disk usage exceeds the threshold '"
                << 100 * (1.0 - flags.image_gc_config->image_disk_headroom())
                << "%'. Container Image GC is triggered.";

      vector<Image> excludedImages(
          flags.image_gc_config->excluded_images().begin(),
          flags.image_gc_config->excluded_images().end());

      containerizer->pruneImages(excludedImages);
    }
  }

  delay(
      Nanoseconds(
          flags.image_gc_config->image_disk_watch_interval().nanoseconds()),
      self(),
      &Slave::checkImageDiskUsage);
}


Future<Nothing> Slave::recover(const Try<state::State>& state)
{
  if (state.isError()) {
    return Failure(state.error());
  }

  Option<ResourcesState> resourcesState = state->resources;
  Option<SlaveState> slaveState = state->slave;

  // With the addition of frameworks with multiple roles, we
  // need to inject the allocated role into each allocated
  // `Resource` object that we've persisted. Note that we
  // also must do this for MULTI_ROLE frameworks since they
  // may have tasks that were present before the framework
  // upgraded into MULTI_ROLE.
  auto injectAllocationInfo = [](
      RepeatedPtrField<Resource>* resources,
      const FrameworkInfo& frameworkInfo) {
    set<string> roles = protobuf::framework::getRoles(frameworkInfo);

    bool injectedAllocationInfo = false;
    foreach (Resource& resource, *resources) {
      if (!resource.has_allocation_info()) {
        if (roles.size() != 1) {
          LOG(FATAL) << "Missing 'Resource.AllocationInfo' for resources"
                     << " allocated to MULTI_ROLE framework"
                     << " '" << frameworkInfo.name() << "'";
        }

        resource.mutable_allocation_info()->set_role(*roles.begin());
        injectedAllocationInfo = true;
      }
    }

    return injectedAllocationInfo;
  };

  // In order to allow frameworks to change their role(s), we need to keep
  // track of the fact that the resources used to be implicitly allocated to
  // `FrameworkInfo.role` before the agent upgrade. To this end, we inject
  // the `AllocationInfo` to the resources in `ExecutorState` and `TaskState`,
  // and re-checkpoint them if necessary.

  hashset<ExecutorID> injectedExecutors;
  hashmap<ExecutorID, hashset<TaskID>> injectedTasks;

  if (slaveState.isSome()) {
    foreachvalue (FrameworkState& frameworkState, slaveState->frameworks) {
      if (!frameworkState.info.isSome()) {
        continue;
      }

      foreachvalue (ExecutorState& executorState, frameworkState.executors) {
        if (!executorState.info.isSome()) {
          continue;
        }

        if (injectAllocationInfo(
                executorState.info->mutable_resources(),
                frameworkState.info.get())) {
          injectedExecutors.insert(executorState.id);
        }

        foreachvalue (RunState& runState, executorState.runs) {
          foreachvalue (TaskState& taskState, runState.tasks) {
            if (!taskState.info.isSome()) {
              continue;
            }

            if (injectAllocationInfo(
                    taskState.info->mutable_resources(),
                    frameworkState.info.get())) {
              injectedTasks[executorState.id].insert(taskState.id);
            }
          }
        }
      }
    }
  }

  // Recover checkpointed resources.
  // NOTE: 'resourcesState' is None if the slave rootDir does not
  // exist or the resources checkpoint file cannot be found.
  if (resourcesState.isSome()) {
    if (resourcesState->errors > 0) {
      LOG(WARNING) << "Errors encountered during resources recovery: "
                   << resourcesState->errors;

      metrics.recovery_errors += resourcesState->errors;
    }

    checkpointedResources = resourcesState->resources;

    if (resourcesState->target.isSome()) {
      Resources targetResources = resourcesState->target.get();

      // Sync the checkpointed resources from the target (which was
      // only created when there are pending changes in the
      // checkpointed resources). If there is any failure, the
      // checkpoint is not committed and the agent exits. In that
      // case, sync of checkpoints will be reattempted on the next
      // agent restart (before agent reregistration).
      Try<Nothing> syncResult = syncCheckpointedResources(targetResources);

      if (syncResult.isError()) {
        return Failure(
            "Target checkpointed resources " +
            stringify(targetResources) +
            " failed to sync from current checkpointed resources " +
            stringify(checkpointedResources) + ": " +
            syncResult.error());
      }

      // Rename the target checkpoint to the committed checkpoint.
      Try<Nothing> renameResult = os::rename(
          paths::getResourcesTargetPath(metaDir),
          paths::getResourcesInfoPath(metaDir));

      if (renameResult.isError()) {
        return Failure(
            "Failed to checkpoint resources " +
            stringify(targetResources) + ": " +
            renameResult.error());
      }

      // Since we synced the target resources to the committed resources, we
      // check resource compatibility with `--resources` command line flag
      // based on the committed checkpoint.
      checkpointedResources = targetResources;
    }

    // This is to verify that the checkpointed resources are
    // compatible with the agent resources specified through the
    // '--resources' command line flag. The compatibility has been
    // verified by the old agent but the flag may have changed during
    // agent restart in an incompatible way and the operator may need
    // to either fix the flag or the checkpointed resources.
    Try<Resources> _totalResources = applyCheckpointedResources(
        info.resources(), checkpointedResources);

    if (_totalResources.isError()) {
      return Failure(
          "Checkpointed resources " +
          stringify(checkpointedResources) +
          " are incompatible with agent resources " +
          stringify(info.resources()) + ": " +
          _totalResources.error());
    }

    totalResources = _totalResources.get();
  }

  if (slaveState.isSome() && slaveState->info.isSome()) {
    if (slaveState->errors > 0) {
      LOG(WARNING) << "Errors encountered during agent recovery: "
                   << slaveState->errors;

      metrics.recovery_errors += slaveState->errors;
    }

    // Save the previous id into the current `SlaveInfo`, so we can compare
    // both of them for equality. This is safe because if it turned out that
    // we can not reuse the id, we will either crash or erase it again.
    info.mutable_id()->CopyFrom(slaveState->info->id());

    // Check for SlaveInfo compatibility.
    Try<Nothing> _compatible =
      compatible(slaveState->info.get(), info);

    if (_compatible.isSome()) {
      // Permitted change, so we reuse the recovered agent id and reconnect
      // to running executors.

      // Prior to Mesos 1.5, the master expected that an agent would never
      // change its `SlaveInfo` and keep the same slave id, and therefore would
      // not update it's internal data structures on agent re-registration.
      if (!(slaveState->info.get() == info)) {
        requiredMasterCapabilities.agentUpdate = true;
      }

      // Recover the frameworks.
      foreachvalue (const FrameworkState& frameworkState,
                    slaveState->frameworks) {
        recoverFramework(frameworkState, injectedExecutors, injectedTasks);
      }
    } else if (state->rebooted) {
      // Prior to Mesos 1.4 we directly bypass the state recovery and
      // start as a new agent upon reboot (introduced in MESOS-844).
      // This unncessarily discards the existing agent ID (MESOS-6223).
      // Starting in Mesos 1.4 we'll attempt to recover the slave state
      // even after reboot but in case of an incompatible slave info change
      // we'll fall back to recovering as a new agent (existing behavior).
      // Prior to Mesos 1.5, an incompatible change would be any slave info
      // mismatch.
      // This prevents the agent from flapping if the slave info (resources,
      // attributes, etc.) change is due to host maintenance associated
      // with the reboot.

      LOG(WARNING) << "Falling back to recover as a new agent due to error: "
                   << _compatible.error();

      // Cleaning up the slave state to avoid any state recovery for the
      // old agent.
      info.clear_id();
      slaveState = None();

      // Remove the "latest" symlink if it exists to "checkpoint" the
      // decision to recover as a new agent.
      const string& latest = paths::getLatestSlavePath(metaDir);
      if (os::exists(latest)) {
        CHECK_SOME(os::rm(latest))
          << "Failed to remove latest symlink '" << latest << "'";
      }
    } else {
      return Failure(_compatible.error());
    }
  }

  return taskStatusUpdateManager->recover(metaDir, slaveState)
    .then(defer(self(), &Slave::_recoverContainerizer, slaveState));
}


Future<Nothing> Slave::_recoverContainerizer(
    const Option<state::SlaveState>& state)
{
  return containerizer->recover(state);
}


Future<Nothing> Slave::_recover()
{
  // Alow HTTP based executors to subscribe after the
  // containerizer recovery is complete.
  recoveryInfo.reconnect = true;

  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      // Set up callback for executor termination.
      containerizer->wait(executor->containerId)
        .onAny(defer(self(),
                     &Self::executorTerminated,
                     framework->id(),
                     executor->id,
                     lambda::_1));

      if (flags.recover == "reconnect") {
        // We send a reconnect message for PID based executors
        // as we can initiate communication with them. Recovered
        // HTTP executors, on the other hand, are responsible for
        // subscribing back with the agent using a retry interval.
        // Note that recovered http executors are marked with
        // http.isNone and pid.isNone (see comments in the header).
        if (executor->pid.isSome() && executor->pid.get()) {
          LOG(INFO)
            << "Sending reconnect request to executor " << *executor;

          ReconnectExecutorMessage message;
          message.mutable_slave_id()->MergeFrom(info.id());
          send(executor->pid.get(), message);

          // PID-based executors using Mesos libraries >= 1.1.2 always
          // re-link with the agent upon receiving the reconnect message.
          // This avoids the executor replying on a half-open TCP
          // connection to the old agent (possible if netfilter is
          // dropping packets, see: MESOS-7057). However, PID-based
          // executors using Mesos libraries < 1.1.2 do not re-link
          // and are therefore prone to replying on a half-open connection
          // after the agent restarts. If we only send a single reconnect
          // message, these "old" executors will reply on their half-open
          // connection and receive a RST; without any retries, they will
          // fail to reconnect and be killed by the agent once the executor
          // re-registration timeout elapses. To ensure these "old"
          // executors can reconnect in the presence of netfilter dropping
          // packets, we introduced optional retries of the reconnect
          // message. This results in "old" executors correctly establishing
          // a link when processing the second reconnect message.
          if (flags.executor_reregistration_retry_interval.isSome()) {
            const Duration& retryInterval =
              flags.executor_reregistration_retry_interval.get();

            const FrameworkID& frameworkId = framework->id();
            const ExecutorID& executorId = executor->id;

            process::loop(
                self(),
                [retryInterval]() {
                  return after(retryInterval);
                },
                [this, frameworkId, executorId, message](Nothing)
                    -> ControlFlow<Nothing> {
                  if (state != RECOVERING) {
                    return Break();
                  }

                  Framework* framework = getFramework(frameworkId);
                  if (framework == nullptr) {
                    return Break();
                  }

                  Executor* executor = framework->getExecutor(executorId);
                  if (executor == nullptr) {
                    return Break();
                  }

                  if (executor->state != Executor::REGISTERING) {
                    return Break();
                  }

                  LOG(INFO) << "Re-sending reconnect request to executor "
                            << *executor;

                  send(executor->pid.get(), message);
                  return Continue();
                });
          }
        } else if (executor->pid.isNone()) {
          LOG(INFO) << "Waiting for executor " << *executor
                    << " to subscribe";
        } else {
          LOG(INFO) << "Unable to reconnect to executor " << *executor
                    << " because no pid or http checkpoint file was found";
        }
      } else {
        // For PID-based executors, we ask the executor to shut
        // down and give it time to terminate. For HTTP executors,
        // we do the same, however, the shutdown will only be sent
        // when the executor subscribes.
        if ((executor->pid.isSome() && executor->pid.get()) ||
            executor->pid.isNone()) {
          LOG(INFO) << "Sending shutdown to executor " << *executor;
          _shutdownExecutor(framework, executor);
        } else {
          LOG(INFO) << "Killing executor " << *executor
                    << " because no pid or http checkpoint file was found";

          containerizer->destroy(executor->containerId);
        }
      }
    }
  }

  if (!frameworks.empty() && flags.recover == "reconnect") {
    // Cleanup unregistered executors after a delay.
    delay(flags.executor_reregistration_timeout,
          self(),
          &Slave::reregisterExecutorTimeout);

    // We set 'recovered' flag inside reregisterExecutorTimeout(),
    // so that when the slave re-registers with master it can
    // correctly inform the master about the launched tasks.
    return recoveryInfo.recovered.future();
  }

  return Nothing();
}


void Slave::__recover(const Future<Nothing>& future)
{
  if (!future.isReady()) {
    EXIT(EXIT_FAILURE)
      << "Failed to perform recovery: "
      << (future.isFailed() ? future.failure() : "future discarded") << "\n"
      << "If recovery failed due to a change in configuration and you want to\n"
      << "keep the current agent id, you might want to change the\n"
      << "`--reconfiguration_policy` flag to a more permissive value.\n"
      << "\n"
      << "To restart this agent with a new agent id instead, do as follows:\n"
      << "rm -f " << paths::getLatestSlavePath(metaDir) << "\n"
      << "This ensures that the agent does not recover old live executors.\n"
      << "\n"
      << "If you use the Docker containerizer and think that the Docker\n"
      << "daemon state is broken, you can try to clear it. But be careful:\n"
      << "these commands will erase all containers and images from this host,\n"
      << "not just those started by Mesos!\n"
      << "docker kill $(docker ps -q)\n"
      << "docker rm $(docker ps -a -q)\n"
      << "docker rmi $(docker images -q)\n"
      << "\n"
      << "Finally, restart the agent.";
  }

  LOG(INFO) << "Finished recovery";

  CHECK_EQ(RECOVERING, state);

  // Checkpoint boot ID.
  Try<string> bootId = os::bootId();
  if (bootId.isError()) {
    LOG(ERROR) << "Could not retrieve boot id: " << bootId.error();
  } else {
    const string path = paths::getBootIdPath(metaDir);
    CHECK_SOME(state::checkpoint(path, bootId.get()));
  }

  // Schedule all old slave directories for garbage collection.
  // TODO(vinod): Do this as part of recovery. This needs a fix
  // in the recovery code, to recover all slaves instead of only
  // the latest slave.
  const string directory = path::join(flags.work_dir, "slaves");
  Try<list<string>> entries = os::ls(directory);
  if (entries.isSome()) {
    foreach (const string& entry, entries.get()) {
      string path = path::join(directory, entry);
      // Ignore non-directory entries.
      if (!os::stat::isdir(path)) {
        continue;
      }

      // We garbage collect a directory if either the slave has not
      // recovered its id (hence going to get a new id when it
      // registers with the master) or if it is an old work directory.
      SlaveID slaveId;
      slaveId.set_value(entry);
      if (!info.has_id() || slaveId != info.id()) {
        LOG(INFO) << "Garbage collecting old agent " << slaveId;

        // NOTE: We update the modification time of the slave work/meta
        // directories even though these are old because these
        // directories might not have been scheduled for gc before.

        // GC the slave work directory.
        os::utime(path); // Update the modification time.
        garbageCollect(path);

        // GC the slave meta directory.
        path = paths::getSlavePath(metaDir, slaveId);
        if (os::exists(path)) {
          os::utime(path); // Update the modification time.
          garbageCollect(path);
        }
      }
    }
  }

  if (flags.recover == "reconnect") {
    state = DISCONNECTED;

    // Start detecting masters.
    detection = detector->detect()
      .onAny(defer(self(), &Slave::detected, lambda::_1));

    if (capabilities.resourceProvider) {
      // Start listening for messages from the resource provider manager.
      resourceProviderManager.messages().get().onAny(
          defer(self(), &Self::handleResourceProviderMessage, lambda::_1));
    }

    // Forward oversubscribed resources.
    forwardOversubscribed();

    // Start acting on correction from QoS Controller.
    qosCorrections();
  } else {
    // Slave started in cleanup mode.
    CHECK_EQ("cleanup", flags.recover);
    state = TERMINATING;

    if (frameworks.empty()) {
      terminate(self());
    }

    // If there are active executors/frameworks, the slave will
    // shutdown when all the executors are terminated. Note that
    // the executors are guaranteed to terminate because they
    // are sent shutdown signal in '_recover()' which results in
    // 'Containerizer::destroy()' being called if the termination
    // doesn't happen within a timeout.
  }

  recoveryInfo.recovered.set(Nothing()); // Signal recovery.
}


void Slave::recoverFramework(
    const FrameworkState& state,
    const hashset<ExecutorID>& executorsToRecheckpoint,
    const hashmap<ExecutorID, hashset<TaskID>>& tasksToRecheckpoint)
{
  LOG(INFO) << "Recovering framework " << state.id;

  if (state.executors.empty()) {
    // GC the framework work directory.
    garbageCollect(
        paths::getFrameworkPath(flags.work_dir, info.id(), state.id));

    // GC the framework meta directory.
    garbageCollect(
        paths::getFrameworkPath(metaDir, info.id(), state.id));

    return;
  }

  CHECK(!frameworks.contains(state.id));

  CHECK_SOME(state.info);
  FrameworkInfo frameworkInfo = state.info.get();

  // Mesos 0.22 and earlier didn't write the FrameworkID into the FrameworkInfo.
  // In this case, we we update FrameworkInfo.framework_id from directory name,
  // and rewrite the new format when we are done.
  bool recheckpoint = false;
  if (!frameworkInfo.has_id()) {
    frameworkInfo.mutable_id()->CopyFrom(state.id);
    recheckpoint = true;
  }

  CHECK(frameworkInfo.has_id());
  CHECK(frameworkInfo.checkpoint());

  // In 0.24.0, HTTP schedulers are supported and these do not
  // have a 'pid'. In this case, the slave will checkpoint UPID().
  CHECK_SOME(state.pid);

  Option<UPID> pid = state.pid.get();

  if (pid.get() == UPID()) {
    pid = None();
  }

  Framework* framework = new Framework(
      this, flags, frameworkInfo, pid);

  frameworks[framework->id()] = framework;

  if (recheckpoint) {
    framework->checkpointFramework();
  }

  // Now recover the executors for this framework.
  foreachvalue (const ExecutorState& executorState, state.executors) {
    framework->recoverExecutor(
        executorState,
        executorsToRecheckpoint.contains(executorState.id),
        tasksToRecheckpoint.contains(executorState.id)
            ? tasksToRecheckpoint.at(executorState.id)
            : hashset<TaskID>{});
  }

  // Remove the framework in case we didn't recover any executors.
  if (framework->executors.empty()) {
    removeFramework(framework);
  }
}


Future<Nothing> Slave::garbageCollect(const string& path)
{
  Try<long> mtime = os::stat::mtime(path);
  if (mtime.isError()) {
    LOG(ERROR) << "Failed to find the mtime of '" << path
               << "': " << mtime.error();
    return Failure(mtime.error());
  }

  // It is unsafe for testing to use unix time directly, we must use
  // Time::create to convert into a Time object that reflects the
  // possibly advanced state of the libprocess Clock.
  Try<Time> time = Time::create(mtime.get());
  CHECK_SOME(time);

  // GC based on the modification time.
  Duration delay = flags.gc_delay - (Clock::now() - time.get());

  return gc->schedule(delay, path);
}


void Slave::forwardOversubscribed()
{
  VLOG(2) << "Querying resource estimator for oversubscribable resources";

  resourceEstimator->oversubscribable()
    .onAny(defer(self(), &Self::_forwardOversubscribed, lambda::_1));
}


void Slave::_forwardOversubscribed(const Future<Resources>& oversubscribable)
{
  if (!oversubscribable.isReady()) {
    LOG(ERROR) << "Failed to get oversubscribable resources: "
               << (oversubscribable.isFailed()
                   ? oversubscribable.failure() : "future discarded");
  } else {
    VLOG(2) << "Received oversubscribable resources "
            << oversubscribable.get() << " from the resource estimator";

    // Oversubscribable resources must be tagged as revocable.
    //
    // TODO(bmahler): Consider tagging input as revocable
    // rather than rejecting and crashing here.
    CHECK_EQ(oversubscribable.get(), oversubscribable->revocable());

    auto unallocated = [](const Resources& resources) {
      Resources result = resources;
      result.unallocate();
      return result;
    };

    // Calculate the latest allocation of oversubscribed resources.
    // Note that this allocation value might be different from the
    // master's view because new task/executor might be in flight from
    // the master or pending on the slave etc. This is ok because the
    // allocator only considers the slave's view of allocation when
    // calculating the available oversubscribed resources to offer.
    Resources oversubscribed;
    foreachvalue (Framework* framework, frameworks) {
      oversubscribed += unallocated(
          framework->allocatedResources().revocable());
    }

    // Add oversubscribable resources to the total.
    oversubscribed += oversubscribable.get();

    // Only forward the estimate if it's different from the previous
    // estimate. We also send this whenever we get (re-)registered
    // (i.e. whenever we transition into the RUNNING state).
    if (state == RUNNING && oversubscribedResources != oversubscribed) {
      LOG(INFO) << "Forwarding total oversubscribed resources "
                << oversubscribed;

      // We do not update the agent's resource version since
      // oversubscribed resources cannot be used for any operations
      // but launches. Since oversubscription is run at regular
      // intervals updating the version could cause a lot of
      // operation churn.
      //
      // TODO(bbannier): Revisit this if we modify the operations
      // possible on oversubscribed resources.

      UpdateSlaveMessage message;
      message.mutable_slave_id()->CopyFrom(info.id());
      message.set_update_oversubscribed_resources(true);
      message.mutable_oversubscribed_resources()->CopyFrom(oversubscribed);

      CHECK_SOME(master);
      send(master.get(), message);
    }

    // Update the estimate.
    oversubscribedResources = oversubscribed;
  }

  delay(flags.oversubscribed_resources_interval,
        self(),
        &Self::forwardOversubscribed);
}


UpdateSlaveMessage Slave::generateResourceProviderUpdate() const
{
  // Agent information (total resources, operations, resource
  // versions) is not passed as part of some `ResourceProvider`, but
  // globally in `UpdateStateMessage`.
  //
  // TODO(bbannier): Pass agent information as a resource provider.
  UpdateSlaveMessage message;
  message.mutable_slave_id()->CopyFrom(info.id());
  message.set_update_oversubscribed_resources(false);
  message.mutable_resource_version_uuid()->set_value(resourceVersion.toBytes());
  message.mutable_operations();

  foreachvalue (const Operation* operation, operations) {
    Result<ResourceProviderID> resourceProviderId =
      getResourceProviderId(operation->info());

    if (resourceProviderId.isNone()) {
      message.mutable_operations()
        ->add_operations()->CopyFrom(*operation);
    }
  }

  foreachvalue (ResourceProvider* resourceProvider, resourceProviders) {
    UpdateSlaveMessage::ResourceProvider* provider =
      message.mutable_resource_providers()->add_providers();

    provider->mutable_info()->CopyFrom(
        resourceProvider->info);
    provider->mutable_total_resources()->CopyFrom(
        resourceProvider->totalResources);
    provider->mutable_resource_version_uuid()->set_value(
        resourceProvider->resourceVersion.toBytes());

    provider->mutable_operations();

    foreachvalue (const Operation* operation,
                  resourceProvider->operations) {
      provider->mutable_operations()
        ->add_operations()->CopyFrom(*operation);
    }
  }

  return message;
}


UpdateSlaveMessage Slave::generateUpdateSlaveMessage() const
{
  UpdateSlaveMessage message = generateResourceProviderUpdate();

  if (oversubscribedResources.isSome()) {
    message.set_update_oversubscribed_resources(true);
    message.mutable_oversubscribed_resources()->CopyFrom(
        oversubscribedResources.get());
  }

  return message;
}


void Slave::handleResourceProviderMessage(
    const Future<ResourceProviderMessage>& message)
{
  // Ignore terminal messages which are not ready. These
  // can arise e.g., if the `Future` was discarded.
  if (!message.isReady()) {
    LOG(ERROR) << "Last resource provider message became terminal before "
                  "becoming ready: "
               << (message.isFailed() ? message.failure() : "future discarded");

    // Wait for the next message.
    resourceProviderManager.messages().get()
      .onAny(defer(self(), &Self::handleResourceProviderMessage, lambda::_1));

    return;
  }

  LOG(INFO) << "Handling resource provider message '" << message.get() << "'";

  switch(message->type) {
    case ResourceProviderMessage::Type::UPDATE_STATE: {
      CHECK_SOME(message->updateState);

      const ResourceProviderMessage::UpdateState& updateState =
        message->updateState.get();

      CHECK(updateState.info.has_id());
      const ResourceProviderID& resourceProviderId = updateState.info.id();

      ResourceProvider* resourceProvider =
        getResourceProvider(resourceProviderId);

      if (resourceProvider == nullptr) {
        resourceProvider = new ResourceProvider(
            updateState.info,
            updateState.totalResources,
            updateState.resourceVersion);

        addResourceProvider(resourceProvider);

        foreachvalue (const Operation& operation,
                      updateState.operations) {
          addOperation(new Operation(operation));
        }

        // Update the 'total' in the Slave.
        totalResources += updateState.totalResources;
      } else {
        // Always update the resource provider info.
        resourceProvider->info = updateState.info;

        if (resourceProvider->totalResources != updateState.totalResources) {
          // Update the 'total' in the Slave.
          CHECK(totalResources.contains(resourceProvider->totalResources));
          totalResources -= resourceProvider->totalResources;
          totalResources += updateState.totalResources;

          // Update the 'total' in the resource provider.
          resourceProvider->totalResources = updateState.totalResources;
        }

        // Update operation state.
        //
        // We only update operations which are not contained in both
        // the known and just received sets. All other operations will
        // be updated via relayed operation status updates.
        const hashset<id::UUID> knownUuids =
          resourceProvider->operations.keys();

        const hashset<id::UUID> receivedUuids = updateState.operations.keys();

        // Handle operations known to the agent but not reported by
        // the resource provider. These could be operations where the
        // agent has started tracking an operation, but the resource
        // provider failed over before it could bookkeep the
        // operation.
        //
        // NOTE: We do not mutate operations statuses here; this would
        // be the responsibility of a operation status update handler.
        hashset<id::UUID> disappearedOperations;
        std::set_difference(
            knownUuids.begin(),
            knownUuids.end(),
            receivedUuids.begin(),
            receivedUuids.end(),
            std::inserter(
                disappearedOperations, disappearedOperations.begin()));

        foreach (const id::UUID& uuid, disappearedOperations) {
          // TODO(bbannier): Instead of simply dropping an operation
          // with `removeOperation` here we should instead send a
          // `Reconcile` message with a failed state to the resource
          // provider so its status update manager can reliably
          // deliver the operation status to the framework.
          CHECK(resourceProvider->operations.contains(uuid));
          removeOperation(resourceProvider->operations.at(uuid));
        }

        // Handle operations known to the resource provider but not
        // the agent. This can happen if the agent failed over and the
        // resource provider reregistered.
        hashset<id::UUID> reappearedOperations;
        std::set_difference(
            receivedUuids.begin(),
            receivedUuids.end(),
            knownUuids.begin(),
            knownUuids.end(),
            std::inserter(reappearedOperations, reappearedOperations.begin()));

        foreach (const id::UUID& uuid, reappearedOperations) {
          // Start tracking this operation.
          //
          // NOTE: We do not need to update total resources here as its
          // state was sync explicitly with the received total above.
          CHECK(updateState.operations.contains(uuid));
          addOperation(new Operation(updateState.operations.at(uuid)));
        }

        // Update resource version of this resource provider.
        resourceProvider->resourceVersion = updateState.resourceVersion;
      }

      // Send the updated resources to the master if the agent is running. Note
      // that since we have already updated our copy of the latest resource
      // provider resources, it is safe to consume this message and wait for the
      // next one; even if we do not send the update to the master right now, an
      // update will be send once the agent reregisters.
      switch (state) {
        case RECOVERING:
        case DISCONNECTED:
        case TERMINATING: {
          break;
        }
        case RUNNING: {
          LOG(INFO) << "Forwarding new total resources " << totalResources;

          // Inform the master about the update from the resource provider.
          send(master.get(), generateResourceProviderUpdate());

          break;
        }
      }
      break;
    }
    case ResourceProviderMessage::Type::UPDATE_OPERATION_STATUS: {
      CHECK_SOME(message->updateOperationStatus);

      const UpdateOperationStatusMessage& update =
        message->updateOperationStatus->update;

      Try<id::UUID> operationUUID =
        id::UUID::fromBytes(update.operation_uuid().value());
      CHECK_SOME(operationUUID);

      Operation* operation = getOperation(operationUUID.get());

      if (operation != nullptr) {
        // The agent might not know about the operation in the
        // following cases:
        //
        // Case 1:
        // (1) The agent sends to a resource provder an ACK for a
        //     terminal operation status update and removes the
        //     operation.
        // (2) The resource provider doesn't get the ACK.
        // (3) The resource provider's status update manager resends
        //     the operation status update.
        //
        // Case 2:
        // (1) The master knows an operation that the agent doesn't
        //     know, because an ApplyOperationMessage was dropped.
        // (2) The master sends a ReconcileOperationsMessage
        //     message to the agent, who forwards it to a resource
        //     provider.
        // (3) The resource provider doesn't know the operation, so it
        //     sends an operation status update with the state
        //     OPERATION_DROPPED.
        //
        // In both cases the agent should not update it's internal
        // state, but it should still forward the operation status
        // update.
        updateOperation(operation, update);
      }

      switch (state) {
        case RECOVERING:
        case DISCONNECTED:
        case TERMINATING: {
          LOG(WARNING) << "Dropping status update of operation '"
                       << update.status().operation_id() << "' (uuid: "
                       << operationUUID->toString() << ") for framework "
                       << update.framework_id() << " because agent is in "
                       << state << " state";
          break;
        }
        case RUNNING: {
          LOG(INFO) << "Forwarding status update of "
                    << (operation == nullptr ? "unknown " : "")
                    << "operation '" << update.status().operation_id()
                    << "' (uuid: " << operationUUID->toString()
                    << ") for framework " << update.framework_id();

          // The status update from the resource provider didn't
          // provide the agent ID (because the resource provider
          // doesn't know it), hence we inject it here.
          UpdateOperationStatusMessage _update;
          _update.CopyFrom(update);
          _update.mutable_slave_id()->CopyFrom(info.id());

          send(master.get(), _update);
          break;
        }
      }
      break;
    }
    case ResourceProviderMessage::Type::DISCONNECT: {
      CHECK_SOME(message->disconnect);

      const ResourceProviderID& resourceProviderId =
        message->disconnect->resourceProviderId;

      ResourceProvider* resourceProvider =
        getResourceProvider(resourceProviderId);

      if (resourceProvider == nullptr) {
        LOG(ERROR) << "Failed to find the disconnected resource provider "
                   << resourceProviderId << ", ignoring the message";
        break;
      }

      // A disconnected resource provider effectively results in its
      // total resources to be set to empty. This will cause offers to
      // be rescinded so that no operation or task can be launched
      // using resources from the disconnected resource provider. If
      // later, the resource provider reconnects, it'll result in
      // an `UpdateSlaveMessage` so that its resources can be offered
      // again by the master.
      CHECK(totalResources.contains(resourceProvider->totalResources));
      totalResources -= resourceProvider->totalResources;

      resourceProvider->totalResources = Resources();

      // Send the updated resources to the master if the agent is running. Note
      // that since we have already updated our copy of the latest resource
      // provider resources, it is safe to consume this message and wait for the
      // next one; even if we do not send the update to the master right now, an
      // update will be send once the agent reregisters.
      switch (state) {
        case RECOVERING:
        case DISCONNECTED:
        case TERMINATING: {
          break;
        }
        case RUNNING: {
          LOG(INFO) << "Forwarding new total resources " << totalResources;

          // Inform the master about the update from the resource provider.
          send(master.get(), generateResourceProviderUpdate());

          break;
        }
      }
      break;
    }
  }

  // Wait for the next message.
  resourceProviderManager.messages().get()
    .onAny(defer(self(), &Self::handleResourceProviderMessage, lambda::_1));
}


void Slave::addOperation(Operation* operation)
{
  Try<id::UUID> uuid = id::UUID::fromBytes(operation->uuid().value());
  CHECK_SOME(uuid);

  operations.put(uuid.get(), operation);

  Result<ResourceProviderID> resourceProviderId =
    getResourceProviderId(operation->info());

  CHECK(!resourceProviderId.isError())
    << "Failed to get resource provider ID: "
    << resourceProviderId.error();

  if (resourceProviderId.isSome()) {
    ResourceProvider* resourceProvider =
      getResourceProvider(resourceProviderId.get());

    CHECK_NOTNULL(resourceProvider);

    resourceProvider->addOperation(operation);
  }
}


void Slave::updateOperation(
    Operation* operation,
    const UpdateOperationStatusMessage& update)
{
  CHECK_NOTNULL(operation);

  const OperationStatus& status = update.status();

  Option<OperationStatus> latestStatus;
  if (update.has_latest_status()) {
    latestStatus = update.latest_status();
  }

  // Whether the operation has just become terminated.
  Option<bool> terminated;

  if (latestStatus.isSome()) {
    terminated =
      !protobuf::isTerminalState(operation->latest_status().state()) &&
      protobuf::isTerminalState(latestStatus->state());

    // If the operation has already transitioned to a terminal state,
    // do not update its state.
    if (!protobuf::isTerminalState(operation->latest_status().state())) {
      operation->mutable_latest_status()->CopyFrom(latestStatus.get());
    }
  } else {
    terminated =
      !protobuf::isTerminalState(operation->latest_status().state()) &&
      protobuf::isTerminalState(status.state());

    if (!protobuf::isTerminalState(operation->latest_status().state())) {
      operation->mutable_latest_status()->CopyFrom(status);
    }
  }

  // Adding the update's status to the stored operation below is the one place
  // in this function where we mutate the operation state irrespective of the
  // value of `terminated`. We check to see if this status update is a retry;
  // if so, we do nothing.
  bool isRetry = false;
  if (status.has_uuid()) {
    foreach (const OperationStatus& storedStatus, operation->statuses()) {
      if (storedStatus.has_uuid() && storedStatus.uuid() == status.uuid()) {
        isRetry = true;
        break;
      }
    }
  }

  if (!isRetry) {
    operation->add_statuses()->CopyFrom(status);
  }

  LOG(INFO) << "Updating the state of operation '"
            << operation->info().id()
            << "' (uuid: " << operation->uuid()
            << ") of framework " << operation->framework_id()
            << " (latest state: " << operation->latest_status().state()
            << ", status update state: " << status.state() << ")";

  CHECK_SOME(terminated);

  if (!terminated.get()) {
    return;
  }

  if (protobuf::isSpeculativeOperation(operation->info())) {
    return;
  }

  switch (update.latest_status().state()) {
    // Terminal state, and the conversion is successful.
    case OPERATION_FINISHED: {
      apply(operation);
      break;
    }

    // Terminal state, and the conversion has failed.
    case OPERATION_FAILED:
    case OPERATION_ERROR:
    case OPERATION_DROPPED: {
      break;
    }

    // Non-terminal. This shouldn't happen.
    case OPERATION_PENDING:
    case OPERATION_UNSUPPORTED: {
      LOG(FATAL) << "Unexpected operation state "
                 << operation->latest_status().state();
    }
  }
}


void Slave::removeOperation(Operation* operation)
{
  Try<id::UUID> uuid = id::UUID::fromBytes(operation->uuid().value());
  CHECK_SOME(uuid);

  Result<ResourceProviderID> resourceProviderId =
    getResourceProviderId(operation->info());

  CHECK(!resourceProviderId.isError())
    << "Failed to get resource provider ID: "
    << resourceProviderId.error();

  if (resourceProviderId.isSome()) {
    ResourceProvider* resourceProvider =
      getResourceProvider(resourceProviderId.get());

    CHECK_NOTNULL(resourceProvider);

    resourceProvider->removeOperation(operation);
  }

  CHECK(operations.contains(uuid.get()))
    << "Unknown operation (uuid: " << uuid->toString() << ")";

  operations.erase(uuid.get());
  delete operation;
}


Operation* Slave::getOperation(const id::UUID& uuid) const
{
  if (operations.contains(uuid)) {
    return operations.at(uuid);
  }
  return nullptr;
}


void Slave::addResourceProvider(ResourceProvider* resourceProvider)
{
  CHECK(resourceProvider->info.has_id());
  CHECK(!resourceProviders.contains(resourceProvider->info.id()));

  resourceProviders.put(
      resourceProvider->info.id(),
      resourceProvider);
}


ResourceProvider* Slave::getResourceProvider(const ResourceProviderID& id) const
{
  if (resourceProviders.contains(id)) {
    return resourceProviders.at(id);
  }
  return nullptr;
}


void Slave::apply(Operation* operation)
{
  vector<ResourceConversion> conversions;

  // NOTE: 'totalResources' don't have allocations set, we need to
  // remove them from the conversions.

  if (protobuf::isSpeculativeOperation(operation->info())) {
    Offer::Operation strippedOperation = operation->info();
    protobuf::stripAllocationInfo(&strippedOperation);

    Try<vector<ResourceConversion>> _conversions =
      getResourceConversions(strippedOperation);

    CHECK_SOME(_conversions);

    conversions = _conversions.get();
  } else {
    // For non-speculative operations, we only apply the conversion
    // once it becomes terminal. Before that, we don't know the
    // converted resources of the conversion.
    CHECK_EQ(OPERATION_FINISHED, operation->latest_status().state());

    Try<Resources> consumed = protobuf::getConsumedResources(operation->info());
    CHECK_SOME(consumed);

    Resources converted = operation->latest_status().converted_resources();

    consumed->unallocate();
    converted.unallocate();

    conversions.emplace_back(consumed.get(), converted);
  }

  // Now, actually apply the operation.
  Try<Resources> resources = totalResources.apply(conversions);
  CHECK_SOME(resources);

  totalResources = resources.get();

  Result<ResourceProviderID> resourceProviderId =
    getResourceProviderId(operation->info());

  CHECK(!resourceProviderId.isError())
    << "Failed to get resource provider ID: "
    << resourceProviderId.error();

  // Besides updating the agent's `totalResources`, we also need to
  // update the resource provider's `totalResources`.
  if (resourceProviderId.isSome()) {
    ResourceProvider* resourceProvider =
      getResourceProvider(resourceProviderId.get());

    CHECK_NOTNULL(resourceProvider);

    Try<Resources> resources =
      resourceProvider->totalResources.apply(conversions);

    CHECK_SOME(resources);

    resourceProvider->totalResources = resources.get();
  }
}


Future<Nothing> Slave::publishResources(
    const Option<Resources>& additionalResources)
{
  Resources resources;

  // NOTE: For resources providers that serve quantity-based resources
  // without any identifiers (such as memory), it is very hard to keep
  // track of published resources. So instead of implementing diff-based
  // resource publishing, we implement an "ensure-all" semantics, and
  // always calculate the total resources that need to remain published.
  foreachvalue (const Framework* framework, frameworks) {
    // NOTE: We do not call `framework->allocatedResource()` here
    // because we do not want to publsh resources for pending tasks that
    // have not been authorized yet.
    foreachvalue (const Executor* executor, framework->executors) {
      resources += executor->allocatedResources();
    }
  }

  if (additionalResources.isSome()) {
    resources += additionalResources.get();
  }

  return resourceProviderManager.publishResources(resources);
}


void Slave::qosCorrections()
{
  qosController->corrections()
    .onAny(defer(self(), &Self::_qosCorrections, lambda::_1));
}


void Slave::_qosCorrections(const Future<list<QoSCorrection>>& future)
{
  // Make sure correction handler is scheduled again.
  delay(flags.qos_correction_interval_min, self(), &Self::qosCorrections);

  // Verify slave state.
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING || state == TERMINATING) {
    LOG(WARNING) << "Cannot perform QoS corrections because the agent is "
                 << state;
    return;
  }

  if (!future.isReady()) {
    LOG(WARNING) << "Failed to get corrections from QoS Controller: "
                  << (future.isFailed() ? future.failure() : "discarded");
    return;
  }

  const list<QoSCorrection>& corrections = future.get();

  VLOG(1) << "Received " << corrections.size() << " QoS corrections";

  foreach (const QoSCorrection& correction, corrections) {
    // TODO(nnielsen): Print correction, once the operator overload
    // for QoSCorrection has been implemented.
    if (correction.type() == QoSCorrection::KILL) {
      const QoSCorrection::Kill& kill = correction.kill();

      if (!kill.has_framework_id()) {
        LOG(WARNING) << "Ignoring QoS correction KILL: "
                     << "framework id not specified.";
        continue;
      }

      const FrameworkID& frameworkId = kill.framework_id();

      if (!kill.has_executor_id()) {
        // TODO(nnielsen): For now, only executor killing is supported. Check
        // can be removed when task killing is supported as well.
        LOG(WARNING) << "Ignoring QoS correction KILL on framework "
                     << frameworkId << ": executor id not specified";
        continue;
      }

      const ExecutorID& executorId = kill.executor_id();

      Framework* framework = getFramework(frameworkId);
      if (framework == nullptr) {
        LOG(WARNING) << "Ignoring QoS correction KILL on framework "
                     << frameworkId << ": framework cannot be found";
        continue;
      }

      // Verify framework state.
      CHECK(framework->state == Framework::RUNNING ||
            framework->state == Framework::TERMINATING)
        << framework->state;

      if (framework->state == Framework::TERMINATING) {
        LOG(WARNING) << "Ignoring QoS correction KILL on framework "
                     << frameworkId << ": framework is terminating.";
        continue;
      }

      Executor* executor = framework->getExecutor(executorId);
      if (executor == nullptr) {
        LOG(WARNING) << "Ignoring QoS correction KILL on executor '"
                     << executorId << "' of framework " << frameworkId
                     << ": executor cannot be found";
        continue;
      }

      const ContainerID containerId =
          kill.has_container_id() ? kill.container_id() : executor->containerId;
      if (containerId != executor->containerId) {
        LOG(WARNING) << "Ignoring QoS correction KILL on container '"
                     << containerId << "' for executor " << *executor
                     << ": container cannot be found";
        continue;
      }

      switch (executor->state) {
        case Executor::REGISTERING:
        case Executor::RUNNING: {
          LOG(INFO) << "Killing container '" << containerId
                    << "' for executor " << *executor
                    << " as QoS correction";

          containerizer->destroy(containerId);

          // TODO(nnielsen): We should ensure that we are addressing
          // the _container_ which the QoS controller intended to
          // kill. Without this check, we may run into a scenario
          // where the executor has terminated and one with the same
          // id has started in the interim i.e. running in a different
          // container than the one the QoS controller targeted
          // (MESOS-2875).
          executor->state = Executor::TERMINATING;

          // Send TASK_GONE because the task was started but has now
          // been terminated. If the framework is not partition-aware,
          // we send TASK_LOST instead for backward compatibility.
          mesos::TaskState taskState = TASK_GONE;
          if (!protobuf::frameworkHasCapability(
                  framework->info,
                  FrameworkInfo::Capability::PARTITION_AWARE)) {
            taskState = TASK_LOST;
          }

          ContainerTermination termination;
          termination.set_state(taskState);
          termination.set_reason(TaskStatus::REASON_CONTAINER_PREEMPTED);
          termination.set_message("Container preempted by QoS correction");

          executor->pendingTermination = termination;

          ++metrics.executors_preempted;
          break;
        }
        case Executor::TERMINATING:
        case Executor::TERMINATED:
          LOG(WARNING) << "Ignoring QoS correction KILL on executor "
                       << *executor << " because the executor is in "
                       << executor->state << " state";
          break;
        default:
          LOG(FATAL) << "Executor " << *executor << " is in unexpected state "
                     << executor->state;
          break;
      }
    } else {
      LOG(WARNING) << "QoS correction type " << correction.type()
                   << " is not supported";
    }
  }
}


Future<ResourceUsage> Slave::usage()
{
  // NOTE: We use 'Owned' here trying to avoid the expensive copy.
  // C++11 lambda only supports capturing variables that have copy
  // constructors. Revisit once we remove the copy constructor for
  // Owned (or C++14 lambda generalized capture is supported).
  Owned<ResourceUsage> usage(new ResourceUsage());
  list<Future<ResourceStatistics>> futures;

  foreachvalue (const Framework* framework, frameworks) {
    foreachvalue (const Executor* executor, framework->executors) {
      // No need to get statistics and status if we know that the
      // executor has already terminated.
      if (executor->state == Executor::TERMINATED) {
        continue;
      }

      ResourceUsage::Executor* entry = usage->add_executors();
      entry->mutable_executor_info()->CopyFrom(executor->info);
      entry->mutable_allocated()->CopyFrom(executor->allocatedResources());
      entry->mutable_container_id()->CopyFrom(executor->containerId);

      // We include non-terminal tasks in ResourceUsage.
      foreachvalue (const Task* task, executor->launchedTasks) {
        ResourceUsage::Executor::Task* t = entry->add_tasks();
        t->set_name(task->name());
        t->mutable_id()->CopyFrom(task->task_id());
        t->mutable_resources()->CopyFrom(task->resources());

        if (task->has_labels()) {
          t->mutable_labels()->CopyFrom(task->labels());
        }
      }

      futures.push_back(containerizer->usage(executor->containerId));
    }
  }

  usage->mutable_total()->CopyFrom(totalResources);

  return await(futures).then(
      [usage](const list<Future<ResourceStatistics>>& futures) {
        // NOTE: We add ResourceUsage::Executor to 'usage' the same
        // order as we push future to 'futures'. So the variables
        // 'future' and 'executor' below should be in sync.
        CHECK_EQ(futures.size(), (size_t) usage->executors_size());

        size_t i = 0;
        foreach (const Future<ResourceStatistics>& future, futures) {
          ResourceUsage::Executor* executor = usage->mutable_executors(i++);

          if (future.isReady()) {
            executor->mutable_statistics()->CopyFrom(future.get());
          } else {
            LOG(WARNING) << "Failed to get resource statistics for executor '"
                         << executor->executor_info().executor_id() << "'"
                         << " of framework "
                         << executor->executor_info().framework_id() << ": "
                         << (future.isFailed() ? future.failure()
                                               : "discarded");
          }
        }

        return Future<ResourceUsage>(*usage);
      });
}


// As a principle, we do not need to re-authorize actions that have already
// been authorized by the master. However, we re-authorize the RUN_TASK action
// on the agent even though the master has already authorized it because:
// a) in cases where hosts have heterogeneous user-account configurations,
//    it makes sense to set the ACL on the agent instead of on the master
// b) compared to other actions such as killing a task and shutting down a
//    framework, it's a greater security risk if malicious tasks are launched
//    as a superuser on the agent.
Future<bool> Slave::authorizeTask(
    const TaskInfo& task,
    const FrameworkInfo& frameworkInfo)
{
  if (authorizer.isNone()) {
    return true;
  }

  // Authorize the task.
  authorization::Request request;

  if (frameworkInfo.has_principal()) {
    request.mutable_subject()->set_value(frameworkInfo.principal());
  }

  request.set_action(authorization::RUN_TASK);

  authorization::Object* object = request.mutable_object();

  object->mutable_task_info()->CopyFrom(task);
  object->mutable_framework_info()->CopyFrom(frameworkInfo);

  LOG(INFO)
    << "Authorizing framework principal '"
    << (frameworkInfo.has_principal() ? frameworkInfo.principal() : "ANY")
    << "' to launch task " << task.task_id();

  return authorizer.get()->authorized(request);
}


Future<bool> Slave::authorizeLogAccess(const Option<Principal>& principal)
{
  if (authorizer.isNone()) {
    return true;
  }

  authorization::Request request;
  request.set_action(authorization::ACCESS_MESOS_LOG);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    request.mutable_subject()->CopyFrom(subject.get());
  }

  return authorizer.get()->authorized(request);
}


Future<bool> Slave::authorizeSandboxAccess(
    const Option<Principal>& principal,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (authorizer.isNone()) {
    return true;
  }

  // Set authorization subject.
  Option<authorization::Subject> subject = createSubject(principal);

  Future<Owned<ObjectApprover>> sandboxApprover =
    authorizer.get()->getObjectApprover(subject, authorization::ACCESS_SANDBOX);

  return sandboxApprover
    .then(defer(
        self(),
        [this, frameworkId, executorId](
            const Owned<ObjectApprover>& sandboxApprover) -> Future<bool> {
          // Construct authorization object.
          ObjectApprover::Object object;

          if (frameworks.contains(frameworkId)) {
            Framework* framework = frameworks.get(frameworkId).get();

            object.framework_info = &(framework->info);

            if (framework->executors.contains(executorId)) {
              object.executor_info =
                &(framework->executors.get(executorId).get()->info);
            }
          }

          Try<bool> approved = sandboxApprover->approved(object);

          if (approved.isError()) {
            return Failure(approved.error());
          }

          return approved.get();
        }));
}


void Slave::sendExecutorTerminatedStatusUpdate(
    const TaskID& taskId,
    const Future<Option<ContainerTermination>>& termination,
    const FrameworkID& frameworkId,
    const Executor* executor)
{
  CHECK_NOTNULL(executor);

  mesos::TaskState state;
  TaskStatus::Reason reason;
  string message;

  const bool haveTermination = termination.isReady() && termination->isSome();

  // Determine the task state for the status update.
  if (haveTermination && termination->get().has_state()) {
    state = termination->get().state();
  } else if (executor->pendingTermination.isSome() &&
             executor->pendingTermination->has_state()) {
    state = executor->pendingTermination->state();
  } else {
    state = TASK_FAILED;
  }

  // Determine the task reason for the status update.
  if (haveTermination && termination->get().has_reason()) {
    reason = termination->get().reason();
  } else if (executor->pendingTermination.isSome() &&
             executor->pendingTermination->has_reason()) {
    reason = executor->pendingTermination->reason();
  } else {
    reason = TaskStatus::REASON_EXECUTOR_TERMINATED;
  }

  // Determine the message for the status update.
  vector<string> messages;

  if (executor->pendingTermination.isSome() &&
      executor->pendingTermination->has_message()) {
    messages.push_back(executor->pendingTermination->message());
  }

  if (!termination.isReady()) {
    messages.push_back(
        "Abnormal executor termination: " +
        (termination.isFailed() ? termination.failure() : "discarded future"));
  } else if (termination->isNone()) {
    messages.push_back("Abnormal executor termination: unknown container");
  } else if (termination->get().has_message()) {
    messages.push_back(termination->get().message());
  }

  if (messages.empty()) {
    message = "Executor terminated";
  } else {
    message = strings::join("; ", messages);
  }

  Option<Resources> limitedResources;

  if (haveTermination && !termination->get().limited_resources().empty()) {
    limitedResources = termination->get().limited_resources();
  }

  statusUpdate(
      protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          taskId,
          state,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random(),
          message,
          reason,
          executor->id,
          None(),
          None(),
          None(),
          None(),
          None(),
          limitedResources),
      UPID());
}


// TODO(dhamon): Move these to their own metrics.hpp|cpp.
double Slave::_tasks_staging()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    typedef hashmap<TaskID, TaskInfo> TaskMap;
    foreachvalue (const TaskMap& tasks, framework->pendingTasks) {
      count += tasks.size();
    }

    foreachvalue (Executor* executor, framework->executors) {
      count += executor->queuedTasks.size();

      foreachvalue (Task* task, executor->launchedTasks) {
        if (task->state() == TASK_STAGING) {
          count++;
        }
      }
    }
  }
  return count;
}


double Slave::_tasks_starting()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreachvalue (Task* task, executor->launchedTasks) {
        if (task->state() == TASK_STARTING) {
          count++;
        }
      }
    }
  }
  return count;
}


double Slave::_tasks_running()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreachvalue (Task* task, executor->launchedTasks) {
        if (task->state() == TASK_RUNNING) {
          count++;
        }
      }
    }
  }
  return count;
}


double Slave::_tasks_killing()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreachvalue (Task* task, executor->launchedTasks) {
        if (task->state() == TASK_KILLING) {
          count++;
        }
      }
    }
  }
  return count;
}


double Slave::_executors_registering()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      if (executor->state == Executor::REGISTERING) {
        count++;
      }
    }
  }
  return count;
}


double Slave::_executors_running()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      if (executor->state == Executor::RUNNING) {
        count++;
      }
    }
  }
  return count;
}


double Slave::_executors_terminating()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      if (executor->state == Executor::TERMINATING) {
        count++;
      }
    }
  }
  return count;
}


double Slave::_executor_directory_max_allowed_age_secs()
{
  return executorDirectoryMaxAllowedAge.secs();
}


double Slave::_resources_total(const string& name)
{
  double total = 0.0;

  foreach (const Resource& resource, info.resources()) {
    if (resource.name() == name && resource.type() == Value::SCALAR) {
      total += resource.scalar().value();
    }
  }

  return total;
}


double Slave::_resources_used(const string& name)
{
  // We use `Resources` arithmetic to accummulate the resources since the
  // `+=` operator de-duplicates the same shared resources across executors.
  Resources used;

  foreachvalue (Framework* framework, frameworks) {
    used += framework->allocatedResources().nonRevocable();
  }

  return used.get<Value::Scalar>(name).getOrElse(Value::Scalar()).value();
}


double Slave::_resources_percent(const string& name)
{
  double total = _resources_total(name);

  if (total == 0.0) {
    return 0.0;
  }

  return _resources_used(name) / total;
}


double Slave::_resources_revocable_total(const string& name)
{
  double total = 0.0;

  if (oversubscribedResources.isSome()) {
    foreach (const Resource& resource, oversubscribedResources.get()) {
      if (resource.name() == name && resource.type() == Value::SCALAR) {
        total += resource.scalar().value();
      }
    }
  }

  return total;
}


double Slave::_resources_revocable_used(const string& name)
{
  // We use `Resources` arithmetic to accummulate the resources since the
  // `+=` operator de-duplicates the same shared resources across executors.
  Resources used;

  foreachvalue (Framework* framework, frameworks) {
    used += framework->allocatedResources().revocable();
  }

  return used.get<Value::Scalar>(name).getOrElse(Value::Scalar()).value();
}


double Slave::_resources_revocable_percent(const string& name)
{
  double total = _resources_revocable_total(name);

  if (total == 0.0) {
    return 0.0;
  }

  return _resources_revocable_used(name) / total;
}


Framework::Framework(
    Slave* _slave,
    const Flags& slaveFlags,
    const FrameworkInfo& _info,
    const Option<UPID>& _pid)
  : state(RUNNING),
    slave(_slave),
    info(_info),
    capabilities(_info.capabilities()),
    pid(_pid),
    completedExecutors(slaveFlags.max_completed_executors_per_framework) {}


Framework::~Framework()
{
  // We own the non-completed executor pointers, so they need to be deleted.
  foreachvalue (Executor* executor, executors) {
    delete executor;
  }
}


bool Framework::idle() const
{
  return executors.empty() && pendingTasks.empty();
}


void Framework::checkpointFramework() const
{
  // Checkpoint the framework info.
  string path = paths::getFrameworkInfoPath(
      slave->metaDir, slave->info.id(), id());

  VLOG(1) << "Checkpointing FrameworkInfo to '" << path << "'";

  CHECK_SOME(state::checkpoint(path, info));

  // Checkpoint the framework pid, note that we checkpoint a
  // UPID() when it is None (for HTTP schedulers) because
  // 0.23.x slaves consider a missing pid file to be an
  // error.
  path = paths::getFrameworkPidPath(
      slave->metaDir, slave->info.id(), id());

  VLOG(1) << "Checkpointing framework pid"
          << " '" << pid.getOrElse(UPID()) << "'"
          << " to '" << path << "'";

  CHECK_SOME(state::checkpoint(path, pid.getOrElse(UPID())));
}


Executor* Framework::addExecutor(const ExecutorInfo& executorInfo)
{
  // Verify that Resource.AllocationInfo is set, if coming
  // from a MULTI_ROLE master this will be set, otherwise
  // the agent will inject it when receiving the executor.
  foreach (const Resource& resource, executorInfo.resources()) {
    CHECK(resource.has_allocation_info());
  }

  // Generate an ID for the executor's container.
  // TODO(idownes) This should be done by the containerizer but we need the
  // ContainerID to create the executor's directory and generate the secret.
  // Consider fixing this since 'launchExecutor()' is handled asynchronously.
  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Option<string> user = None();
#ifndef __WINDOWS__
  if (slave->flags.switch_user) {
    // The command (either in form of task or executor command) can
    // define a specific user to run as. If present, this precedes the
    // framework user value. The selected user will have been verified by
    // the master at this point through the active ACLs.
    // NOTE: The global invariant is that the executor info at this
    // point is (1) the user provided task.executor() or (2) a command
    // executor constructed by the slave from the task.command().
    // If this changes, we need to check the user in both
    // task.command() and task.executor().command() below.
    user = info.user();
    if (executorInfo.command().has_user()) {
      user = executorInfo.command().user();
    }
  }
#endif // __WINDOWS__

  // Create a directory for the executor.
  const string directory = paths::createExecutorDirectory(
      slave->flags.work_dir,
      slave->info.id(),
      id(),
      executorInfo.executor_id(),
      containerId,
      user);

  Executor* executor = new Executor(
      slave,
      id(),
      executorInfo,
      containerId,
      directory,
      user,
      info.checkpoint());

  if (executor->checkpoint) {
    executor->checkpointExecutor();
  }

  CHECK(!executors.contains(executorInfo.executor_id()))
    << "Unknown executor '" << executorInfo.executor_id() << "'";

  executors[executorInfo.executor_id()] = executor;

  LOG(INFO) << "Launching executor '" << executorInfo.executor_id()
            << "' of framework " << id()
            << " with resources " << executorInfo.resources()
            << " in work directory '" << directory << "'";

  const ExecutorID& executorId = executorInfo.executor_id();
  FrameworkID frameworkId = id();

  const PID<Slave> slavePid = slave->self();

  auto authorize =
    [slavePid, executorId, frameworkId](const Option<Principal>& principal) {
      return dispatch(
          slavePid,
          &Slave::authorizeSandboxAccess,
          principal,
          frameworkId,
          executorId);
    };

  // We expose the executor's sandbox in the /files endpoints
  // via the following paths:
  //
  //  (1) /agent_workdir/frameworks/FID/executors/EID/runs/CID
  //  (2) /agent_workdir/frameworks/FID/executors/EID/runs/latest
  //  (3) /frameworks/FID/executors/EID/runs/latest
  //
  // Originally we just exposed the real path (1) and later
  // exposed the 'latest' symlink (2) since it's not easy for
  // users to know the run's container ID. We deprecated
  // (1) and (2) by exposing a virtual path (3) since we do not
  // want to expose the agent's work directory and it's not
  // something users care about in this context.
  //
  // TODO(zhitao): Remove (1) and (2) per MESOS-7960 once we
  // pass 2.0. They remain now for backwards compatibility.
  const string latestPath = paths::getExecutorLatestRunPath(
      slave->flags.work_dir,
      slave->info.id(),
      id(),
      executorInfo.executor_id());

  const string virtualLatestPath = paths::getExecutorVirtualPath(
      id(),
      executorInfo.executor_id());

  slave->files->attach(executor->directory, latestPath, authorize)
    .onAny(defer(
        slave,
        &Slave::fileAttached,
        lambda::_1,
        executor->directory,
        latestPath));

  slave->files->attach(executor->directory, virtualLatestPath, authorize)
    .onAny(defer(
        slave,
        &Slave::fileAttached,
        lambda::_1,
        executor->directory,
        virtualLatestPath));

  slave->files->attach(executor->directory, executor->directory, authorize)
    .onAny(defer(
        slave,
        &Slave::fileAttached,
        lambda::_1,
        executor->directory,
        executor->directory));

  return executor;
}


Executor* Framework::getExecutor(const ExecutorID& executorId) const
{
  if (executors.contains(executorId)) {
    return executors.at(executorId);
  }

  return nullptr;
}


Executor* Framework::getExecutor(const TaskID& taskId) const
{
  foreachvalue (Executor* executor, executors) {
    if (executor->queuedTasks.contains(taskId) ||
        executor->launchedTasks.contains(taskId) ||
        executor->terminatedTasks.contains(taskId)) {
      return executor;
    }
  }
  return nullptr;
}


void Framework::destroyExecutor(const ExecutorID& executorId)
{
  if (executors.contains(executorId)) {
    Executor* executor = executors[executorId];
    executors.erase(executorId);

    // Pass ownership of the executor pointer.
    completedExecutors.push_back(Owned<Executor>(executor));
  }
}


void Framework::recoverExecutor(
    const ExecutorState& state,
    bool recheckpointExecutor,
    const hashset<TaskID>& tasksToRecheckpoint)
{
  LOG(INFO) << "Recovering executor '" << state.id
            << "' of framework " << id();

  CHECK_NOTNULL(slave);

  if (state.runs.empty() || state.latest.isNone() || state.info.isNone()) {
    LOG(WARNING) << "Skipping recovery of executor '" << state.id
                 << "' of framework " << id()
                 << " because its latest run or executor info"
                 << " cannot be recovered";

    // GC the top level executor work directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->flags.work_dir, slave->info.id(), id(), state.id));

    // GC the top level executor meta directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->metaDir, slave->info.id(), id(), state.id));

    return;
  }

  // Verify that Resource.AllocationInfo is set, this should
  // be injected by the agent when recovering.
  foreach (const Resource& resource, state.info->resources()) {
    CHECK(resource.has_allocation_info());
  }

  // We are only interested in the latest run of the executor!
  // So, we GC all the old runs.
  // NOTE: We don't schedule the top level executor work and meta
  // directories for GC here, because they will be scheduled when
  // the latest executor run terminates.
  const ContainerID& latest = state.latest.get();
  foreachvalue (const RunState& run, state.runs) {
    CHECK_SOME(run.id);
    const ContainerID& runId = run.id.get();
    if (latest != runId) {
      // GC the executor run's work directory.
      // TODO(vinod): Expose this directory to webui by recovering the
      // tasks and doing a 'files->attach()'.
      slave->garbageCollect(paths::getExecutorRunPath(
          slave->flags.work_dir, slave->info.id(), id(), state.id, runId));

      // GC the executor run's meta directory.
      slave->garbageCollect(paths::getExecutorRunPath(
          slave->metaDir, slave->info.id(), id(), state.id, runId));
    }
  }

  Option<RunState> run = state.runs.get(latest);
  CHECK_SOME(run)
      << "Cannot find latest run " << latest << " for executor " << state.id
      << " of framework " << id();

  // Create executor.
  const string directory = paths::getExecutorRunPath(
      slave->flags.work_dir, slave->info.id(), id(), state.id, latest);

  Executor* executor = new Executor(
      slave,
      id(),
      state.info.get(),
      latest,
      directory,
      info.user(),
      info.checkpoint());

  // Recover the libprocess PID if possible for PID based executors.
  if (run->http.isSome()) {
    if (!run->http.get()) {
      // When recovering in non-strict mode, the assumption is that the
      // slave can die after checkpointing the forked pid but before the
      // libprocess pid. So, it is not possible for the libprocess pid
      // to exist but not the forked pid. If so, it is a really bad
      // situation (e.g., disk corruption).
      CHECK_SOME(run->forkedPid)
        << "Failed to get forked pid for executor " << state.id
        << " of framework " << id();

      executor->pid = run->libprocessPid.get();
    } else {
      // We set the PID to None() to signify that this is a HTTP based
      // executor.
      executor->pid = None();
    }
  } else {
    // We set the PID to UPID() to signify that the connection type for this
    // executor is unknown.
    executor->pid = UPID();
  }

  // And finally recover all the executor's tasks.
  foreachvalue (const TaskState& taskState, run->tasks) {
    executor->recoverTask(
        taskState,
        tasksToRecheckpoint.contains(taskState.id));
  }

  ExecutorID executorId = state.id;
  FrameworkID frameworkId = id();

  const PID<Slave> slavePid = slave->self();

  auto authorize =
    [slavePid, executorId, frameworkId](const Option<Principal>& principal) {
      return dispatch(
          slavePid,
          &Slave::authorizeSandboxAccess,
          principal,
          frameworkId,
          executorId);
    };

  // We expose the executor's sandbox in the /files endpoints
  // via the following paths:
  //
  //  (1) /agent_workdir/frameworks/FID/executors/EID/runs/CID
  //  (2) /agent_workdir/frameworks/FID/executors/EID/runs/latest
  //  (3) /frameworks/FID/executors/EID/runs/latest
  //
  // Originally we just exposed the real path (1) and later
  // exposed the 'latest' symlink (2) since it's not easy for
  // users to know the run's container ID. We deprecated
  // (1) and (2) by exposing a virtual path (3) since we do not
  // want to expose the agent's work directory and it's not
  // something users care about in this context.
  //
  // TODO(zhitao): Remove (1) and (2) per MESOS-7960 once we
  // pass 2.0. They remain now for backwards compatibility.
  const string latestPath = paths::getExecutorLatestRunPath(
      slave->flags.work_dir,
      slave->info.id(),
      id(),
      state.id);

  const string virtualLatestPath = paths::getExecutorVirtualPath(
      id(),
      state.id);

  slave->files->attach(executor->directory, latestPath, authorize)
    .onAny(defer(
        slave,
        &Slave::fileAttached,
        lambda::_1,
        executor->directory,
        latestPath));

  slave->files->attach(executor->directory, virtualLatestPath, authorize)
    .onAny(defer(
        slave,
        &Slave::fileAttached,
        lambda::_1,
        executor->directory,
        virtualLatestPath));

  // Expose the executor's files.
  slave->files->attach(executor->directory, executor->directory, authorize)
    .onAny(defer(
        slave,
        &Slave::fileAttached,
        lambda::_1,
        executor->directory,
        executor->directory));

  // Add the executor to the framework.
  executors[executor->id] = executor;
  if (recheckpointExecutor) {
    executor->checkpointExecutor();
  }

  // If the latest run of the executor was completed (i.e., terminated
  // and all updates are acknowledged) in the previous run, we
  // transition its state to 'TERMINATED' and gc the directories.
  if (run->completed) {
    ++slave->metrics.executors_terminated;

    executor->state = Executor::TERMINATED;

    CHECK_SOME(run->id);
    const ContainerID& runId = run->id.get();

    // GC the executor run's work directory.
    const string path = paths::getExecutorRunPath(
        slave->flags.work_dir, slave->info.id(), id(), state.id, runId);

    slave->garbageCollect(path)
       .then(defer(slave, &Slave::detachFile, path));

    // GC the executor run's meta directory.
    slave->garbageCollect(paths::getExecutorRunPath(
        slave->metaDir, slave->info.id(), id(), state.id, runId));

    // GC the top level executor work directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->flags.work_dir, slave->info.id(), id(), state.id))
        .then(defer(slave, &Slave::detachFile, latestPath));

    // GC the top level executor meta directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->metaDir, slave->info.id(), id(), state.id));

    // Move the executor to 'completedExecutors'.
    destroyExecutor(executor->id);
  }
}


void Framework::addPendingTask(
    const ExecutorID& executorId,
    const TaskInfo& task)
{
  pendingTasks[executorId][task.task_id()] = task;
}


void Framework::addPendingTaskGroup(
    const ExecutorID& executorId,
    const TaskGroupInfo& taskGroup)
{
  foreach (const TaskInfo& task, taskGroup.tasks()) {
    pendingTasks[executorId][task.task_id()] = task;
  }

  pendingTaskGroups.push_back(taskGroup);
}


bool Framework::hasTask(const TaskID& taskId) const
{
  foreachkey (const ExecutorID& executorId, pendingTasks) {
    if (pendingTasks.at(executorId).contains(taskId)) {
      return true;
    }
  }

  foreachvalue (Executor* executor, executors) {
    if (executor->queuedTasks.contains(taskId) ||
        executor->launchedTasks.contains(taskId) ||
        executor->terminatedTasks.contains(taskId)) {
      return true;
    }
  }

  return false;
}


bool Framework::isPending(const TaskID& taskId) const
{
  foreachkey (const ExecutorID& executorId, pendingTasks) {
    if (pendingTasks.at(executorId).contains(taskId)) {
      return true;
    }
  }

  return false;
}


Option<TaskGroupInfo> Framework::getTaskGroupForPendingTask(
    const TaskID& taskId)
{
  foreach (const TaskGroupInfo& taskGroup, pendingTaskGroups) {
    foreach (const TaskInfo& taskInfo, taskGroup.tasks()) {
      if (taskInfo.task_id() == taskId) {
        return taskGroup;
      }
    }
  }

  return None();
}


bool Framework::removePendingTask(const TaskID& taskId)
{
  bool removed = false;

  foreachkey (const ExecutorID& executorId, pendingTasks) {
    if (pendingTasks.at(executorId).contains(taskId)) {
      pendingTasks.at(executorId).erase(taskId);
      if (pendingTasks.at(executorId).empty()) {
        pendingTasks.erase(executorId);
      }

      removed = true;
      break;
    }
  }

  // We also remove the pending task group if all of its
  // tasks have been removed.
  for (auto it = pendingTaskGroups.begin();
       it != pendingTaskGroups.end();
       ++it) {
    foreach (const TaskInfo& t, it->tasks()) {
      if (t.task_id() == taskId) {
        // Found its task group, check if all tasks within
        // the group have been removed.
        bool allRemoved = true;

        foreach (const TaskInfo& t_, it->tasks()) {
          if (hasTask(t_.task_id())) {
            allRemoved = false;
            break;
          }
        }

        if (allRemoved) {
          pendingTaskGroups.erase(it);
        }

        return removed;
      }
    }
  }

  return removed;
}


Option<ExecutorID> Framework::getExecutorIdForPendingTask(
    const TaskID& taskId) const
{
  foreachkey (const ExecutorID& executorId, pendingTasks) {
    if (pendingTasks.at(executorId).contains(taskId)) {
      return executorId;
    }
  }

  return None();
}


Resources Framework::allocatedResources() const
{
  Resources allocated;

  foreachvalue (const Executor* executor, executors) {
    allocated += executor->allocatedResources();
  }

  hashset<ExecutorID> pendingExecutors;

  typedef hashmap<TaskID, TaskInfo> TaskMap;
  foreachvalue (const TaskMap& pendingTasks, pendingTasks) {
    foreachvalue (const TaskInfo& task, pendingTasks) {
      allocated += task.resources();

      ExecutorInfo executorInfo = slave->getExecutorInfo(info, task);
      const ExecutorID& executorId = executorInfo.executor_id();

      if (!executors.contains(executorId) &&
          !pendingExecutors.contains(executorId)) {
        allocated += executorInfo.resources();
        pendingExecutors.insert(executorId);
      }
    }
  }

  return allocated;
}


Executor::Executor(
    Slave* _slave,
    const FrameworkID& _frameworkId,
    const ExecutorInfo& _info,
    const ContainerID& _containerId,
    const string& _directory,
    const Option<string>& _user,
    bool _checkpoint)
  : state(REGISTERING),
    slave(_slave),
    id(_info.executor_id()),
    info(_info),
    frameworkId(_frameworkId),
    containerId(_containerId),
    directory(_directory),
    user(_user),
    checkpoint(_checkpoint),
    http(None()),
    pid(None()),
    completedTasks(MAX_COMPLETED_TASKS_PER_EXECUTOR)
{
  CHECK_NOTNULL(slave);

  // TODO(jieyu): The way we determine if an executor is generated for
  // a command task (either command or docker executor) is really
  // hacky. We rely on the fact that docker executor launch command is
  // set in the docker containerizer so that this check is still valid
  // in the slave.
  Result<string> executorPath =
    os::realpath(path::join(slave->flags.launcher_dir, MESOS_EXECUTOR));

  if (executorPath.isSome()) {
    isGeneratedForCommandTask_ =
      strings::contains(info.command().value(), executorPath.get());
  }
}


Executor::~Executor()
{
  if (http.isSome()) {
    closeHttpConnection();
  }

  // Delete the tasks.
  foreachvalue (Task* task, launchedTasks) {
    delete task;
  }
  foreachvalue (Task* task, terminatedTasks) {
    delete task;
  }
}


void Executor::enqueueTask(const TaskInfo& task)
{
  queuedTasks[task.task_id()] = task;
}


void Executor::enqueueTaskGroup(const TaskGroupInfo& taskGroup)
{
  foreach (const TaskInfo& task, taskGroup.tasks()) {
    queuedTasks[task.task_id()] = task;
  }

  queuedTaskGroups.push_back(taskGroup);
}


Option<TaskInfo> Executor::dequeueTask(const TaskID& taskId)
{
  Option<TaskInfo> taskInfo = queuedTasks.get(taskId);

  queuedTasks.erase(taskId);

  // Remove the task group if all of its tasks have been dequeued.
  for (auto it = queuedTaskGroups.begin(); it != queuedTaskGroups.end(); ++it) {
    foreach (const TaskInfo& t, it->tasks()) {
      if (t.task_id() == taskId) {
        // Found its task group, check if all tasks within
        // the group have been removed.
        bool allRemoved = true;

        foreach (const TaskInfo& t_, it->tasks()) {
          if (queuedTasks.contains(t_.task_id())) {
            allRemoved = false;
            break;
          }
        }

        if (allRemoved) {
          queuedTaskGroups.erase(it);
        }

        return taskInfo;
      }
    }
  }

  return taskInfo;
}


Task* Executor::addLaunchedTask(const TaskInfo& task)
{
  CHECK(!queuedTasks.contains(task.task_id()))
    << "Task " << task.task_id() << " was not dequeued";

  // The master should enforce unique task IDs, but just in case
  // maybe we shouldn't make this a fatal error.
  CHECK(!launchedTasks.contains(task.task_id()))
    << "Duplicate task " << task.task_id();

  // Verify that Resource.AllocationInfo is set, if coming
  // from a MULTI_ROLE master this will be set, otherwise
  // the agent will inject it when receiving the task.
  foreach (const Resource& resource, task.resources()) {
    CHECK(resource.has_allocation_info());
  }

  Task* t = new Task(protobuf::createTask(task, TASK_STAGING, frameworkId));

  launchedTasks[task.task_id()] = t;

  return t;
}


void Executor::completeTask(const TaskID& taskId)
{
  VLOG(1) << "Completing task " << taskId;

  CHECK(terminatedTasks.contains(taskId))
    << "Failed to find terminated task " << taskId;

  Task* task = terminatedTasks[taskId];
  completedTasks.push_back(shared_ptr<Task>(task));
  terminatedTasks.erase(taskId);
}


void Executor::checkpointExecutor()
{
  CHECK(checkpoint);

  // Checkpoint the executor info.
  const string path = paths::getExecutorInfoPath(
      slave->metaDir, slave->info.id(), frameworkId, id);

  VLOG(1) << "Checkpointing ExecutorInfo to '" << path << "'";

  {
    // If the checkpointed resources don't have reservation refinements,
    // checkpoint them on the agent in "pre-reservation-refinement" format
    // for backward compatibility with old agents. If downgrading is
    // not possible without losing information, checkpoint the resources in
    // the "post-reservation-refinement" format. We ignore the return
    // value of `downgradeResources` because for now, we checkpoint the result
    // either way.
    //
    // TODO(mpark): Do something smarter with the result once something
    // like agent capability requirements is introduced.
    ExecutorInfo info_ = info;
    downgradeResources(info_.mutable_resources());
    CHECK_SOME(state::checkpoint(path, info_));
  }

  // Create the meta executor directory.
  // NOTE: This creates the 'latest' symlink in the meta directory.
  paths::createExecutorDirectory(
      slave->metaDir, slave->info.id(), frameworkId, id, containerId);
}


void Executor::checkpointTask(const TaskInfo& task)
{
  checkpointTask(protobuf::createTask(task, TASK_STAGING, frameworkId));
}


void Executor::checkpointTask(const Task& task)
{
  CHECK(checkpoint);

  const string path = paths::getTaskInfoPath(
      slave->metaDir,
      slave->info.id(),
      frameworkId,
      id,
      containerId,
      task.task_id());

  VLOG(1) << "Checkpointing TaskInfo to '" << path << "'";

  {
    // If the checkpointed resources don't have reservation refinements,
    // checkpoint them on the agent in "pre-reservation-refinement" format
    // for backward compatibility with old agents. If downgrading is
    // not possible without losing information, checkpoint the resources in
    // the "post-reservation-refinement" format. We ignore the return
    // value of `downgradeResources` because for now, we checkpoint the result
    // either way.
    //
    // TODO(mpark): Do something smarter with the result once something
    // like agent capability requirements is introduced.
    Task task_ = task;
    downgradeResources(task_.mutable_resources());
    CHECK_SOME(state::checkpoint(path, task_));
  }
}


void Executor::recoverTask(const TaskState& state, bool recheckpointTask)
{
  if (state.info.isNone()) {
    LOG(WARNING) << "Skipping recovery of task " << state.id
                 << " because its info cannot be recovered";
    return;
  }

  // Verify that Resource.AllocationInfo is set, the agent
  // should inject it during recovery.
  foreach (const Resource& resource, state.info->resources()) {
    CHECK(resource.has_allocation_info());
  }

  Task* task = new Task(state.info.get());
  if (recheckpointTask) {
    checkpointTask(*task);
  }

  launchedTasks[state.id] = task;

  // Read updates to get the latest state of the task.
  foreach (const StatusUpdate& update, state.updates) {
    Try<Nothing> updated = updateTaskState(update.status());

    // TODO(bmahler): We only log this error because we used to
    // allow multiple terminal updates and so we may encounter
    // this when recovering an old executor. We can hard-CHECK
    // this 6 months from 1.1.0.
    if (updated.isError()) {
      LOG(ERROR) << "Failed to update state of recovered task"
                 << " '" << state.id << "' to " << update.status().state()
                 << ": " << updated.error();

      // The only case that should be possible here is when the
      // task had multiple terminal updates persisted.
      continue;
    }

    // Complete the task if it is terminal and
    // the update has been acknowledged.
    if (protobuf::isTerminalState(update.status().state())) {
      CHECK(update.has_uuid())
        << "Expecting updates without 'uuid' to have been rejected";

      if (state.acks.contains(id::UUID::fromBytes(update.uuid()).get())) {
        completeTask(state.id);
      }
      break;
    }
  }
}


Try<Nothing> Executor::updateTaskState(const TaskStatus& status)
{
  bool terminal = protobuf::isTerminalState(status.state());

  const TaskID& taskId = status.task_id();

  Task* task = nullptr;

  if (queuedTasks.contains(taskId)) {
    if (!terminal) {
      return Error("Cannot send non-terminal update for queued task");
    }

    TaskInfo taskInfo = CHECK_NOTNONE(dequeueTask(taskId));

    task = new Task(protobuf::createTask(
        taskInfo,
        status.state(),
        frameworkId));
  } else if (launchedTasks.contains(taskId)) {
    task = launchedTasks.at(status.task_id());

    if (terminal) {
      launchedTasks.erase(taskId);
    }
  } else if (terminatedTasks.contains(taskId)) {
    return Error("Task is already terminated with state"
                 " " + stringify(terminatedTasks.at(taskId)->state()));
  } else {
    return Error("Task is unknown");
  }

  CHECK_NOTNULL(task);

  // TODO(brenden): Consider wiping the `data` and `message` fields?
  if (task->statuses_size() > 0 &&
      task->statuses(task->statuses_size() - 1).state() == status.state()) {
    task->mutable_statuses()->RemoveLast();
  }

  task->add_statuses()->CopyFrom(status);
  task->set_state(status.state());

  // TODO(bmahler): This only increments the state when the update
  // can be handled. Should we always increment the state?
  if (terminal) {
    terminatedTasks[task->task_id()] = task;

    switch (status.state()) {
      case TASK_FINISHED: ++slave->metrics.tasks_finished; break;
      case TASK_FAILED:   ++slave->metrics.tasks_failed;   break;
      case TASK_KILLED:   ++slave->metrics.tasks_killed;   break;
      case TASK_LOST:     ++slave->metrics.tasks_lost;     break;
      case TASK_GONE:     ++slave->metrics.tasks_gone;     break;
      default:
        LOG(ERROR) << "Unexpected terminal task state " << status.state();
        break;
    }
  }

  return Nothing();
}


bool Executor::incompleteTasks()
{
  return !queuedTasks.empty() ||
         !launchedTasks.empty() ||
         !terminatedTasks.empty();
}


bool Executor::isGeneratedForCommandTask() const
{
  return isGeneratedForCommandTask_;
}


void Executor::closeHttpConnection()
{
  CHECK_SOME(http);

  if (!http->close()) {
    LOG(WARNING) << "Failed to close HTTP pipe for " << *this;
  }

  http = None();
}


Option<TaskGroupInfo> Executor::getQueuedTaskGroup(const TaskID& taskId)
{
  foreach (const TaskGroupInfo& taskGroup, queuedTaskGroups) {
    foreach (const TaskInfo& taskInfo, taskGroup.tasks()) {
      if (taskInfo.task_id() == taskId) {
        return taskGroup;
      }
    }
  }

  return None();
}


Resources Executor::allocatedResources() const
{
  Resources allocatedResources = info.resources();

  foreachvalue (const TaskInfo& task, queuedTasks) {
    allocatedResources += task.resources();
  }

  foreachvalue (const Task* task, launchedTasks) {
    allocatedResources += task->resources();
  }

  return allocatedResources;
}


void ResourceProvider::addOperation(Operation* operation)
{
  Try<id::UUID> uuid = id::UUID::fromBytes(operation->uuid().value());
  CHECK_SOME(uuid);

  CHECK(!operations.contains(uuid.get()))
    << "Operation (uuid: " << uuid->toString() << ") already exists";

  operations.put(uuid.get(), operation);
}


void ResourceProvider::removeOperation(Operation* operation)
{
  Try<id::UUID> uuid = id::UUID::fromBytes(operation->uuid().value());
  CHECK_SOME(uuid);

  CHECK(operations.contains(uuid.get()))
    << "Unknown operation (uuid: " << uuid->toString() << ")";

  operations.erase(uuid.get());
}


map<string, string> executorEnvironment(
    const Flags& flags,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    const Option<Secret>& authenticationToken,
    bool checkpoint)
{
  map<string, string> environment;

  // In cases where DNS is not available on the slave, the absence of
  // LIBPROCESS_IP in the executor's environment will cause an error when the
  // new executor process attempts a hostname lookup. Thus, we pass the slave's
  // LIBPROCESS_IP through here, even if the executor environment is specified
  // explicitly. Note that a LIBPROCESS_IP present in the provided flags will
  // override this value.
  Option<string> libprocessIP = os::getenv("LIBPROCESS_IP");
  if (libprocessIP.isSome()) {
    environment["LIBPROCESS_IP"] = libprocessIP.get();
  }

  if (flags.executor_environment_variables.isSome()) {
    foreachpair (const string& key,
                 const JSON::Value& value,
                 flags.executor_environment_variables->values) {
      // See slave/flags.cpp where we validate each value is a string.
      CHECK(value.is<JSON::String>());
      environment[key] = value.as<JSON::String>().value;
    }
  }

  // Set LIBPROCESS_PORT so that we bind to a random free port (since
  // this might have been set via --port option). We do this before
  // the environment variables below in case it is included.
  environment["LIBPROCESS_PORT"] = "0";

  // Also add MESOS_NATIVE_JAVA_LIBRARY if it's not already present (and
  // like above, we do this before the environment variables below in
  // case the framework wants to override).
  // TODO(tillt): Adapt library towards JNI specific name once libmesos
  // has been split.
  if (environment.count("MESOS_NATIVE_JAVA_LIBRARY") == 0) {
    string path =
#ifdef __APPLE__
      LIBDIR "/libmesos-" VERSION ".dylib";
#else
      LIBDIR "/libmesos-" VERSION ".so";
#endif
    if (os::exists(path)) {
      environment["MESOS_NATIVE_JAVA_LIBRARY"] = path;
    }
  }

  // Also add MESOS_NATIVE_LIBRARY if it's not already present.
  // This environment variable is kept for offering non JVM-based
  // frameworks a more compact and JNI independent library.
  if (environment.count("MESOS_NATIVE_LIBRARY") == 0) {
    string path =
#ifdef __APPLE__
      LIBDIR "/libmesos-" VERSION ".dylib";
#else
      LIBDIR "/libmesos-" VERSION ".so";
#endif
    if (os::exists(path)) {
      environment["MESOS_NATIVE_LIBRARY"] = path;
    }
  }

  environment["MESOS_FRAMEWORK_ID"] = executorInfo.framework_id().value();
  environment["MESOS_EXECUTOR_ID"] = executorInfo.executor_id().value();
  environment["MESOS_DIRECTORY"] = directory;
  environment["MESOS_SLAVE_ID"] = slaveId.value();
  environment["MESOS_SLAVE_PID"] = stringify(slavePid);
  environment["MESOS_AGENT_ENDPOINT"] = stringify(slavePid.address);
  environment["MESOS_CHECKPOINT"] = checkpoint ? "1" : "0";
  environment["MESOS_HTTP_COMMAND_EXECUTOR"] =
    flags.http_command_executor ? "1" : "0";

  // Set executor's shutdown grace period. If set, the customized value
  // from `ExecutorInfo` overrides the default from agent flags.
  Duration executorShutdownGracePeriod = flags.executor_shutdown_grace_period;
  if (executorInfo.has_shutdown_grace_period()) {
    executorShutdownGracePeriod =
      Nanoseconds(executorInfo.shutdown_grace_period().nanoseconds());
  }

  environment["MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD"] =
    stringify(executorShutdownGracePeriod);

  if (checkpoint) {
    environment["MESOS_RECOVERY_TIMEOUT"] = stringify(flags.recovery_timeout);

    // The maximum backoff duration to be used by an executor between two
    // retries when disconnected.
    environment["MESOS_SUBSCRIPTION_BACKOFF_MAX"] =
      stringify(flags.executor_reregistration_timeout);
  }

  if (authenticationToken.isSome()) {
    CHECK(authenticationToken->has_value());

    environment["MESOS_EXECUTOR_AUTHENTICATION_TOKEN"] =
      authenticationToken->value().data();
  }

  if (HookManager::hooksAvailable()) {
    // Include any environment variables from Hooks.
    // TODO(karya): Call environment decorator hook _after_ putting all
    // variables from executorInfo into 'env'. This would prevent the
    // ones provided by hooks from being overwritten by the ones in
    // executorInfo in case of a conflict. The overwriting takes places
    // at the callsites of executorEnvironment (e.g., ___launch function
    // in src/slave/containerizer/docker.cpp)
    // TODO(karya): Provide a mechanism to pass the new environment
    // variables created above (MESOS_*) on to the hook modules.
    const Environment& hooksEnvironment =
      HookManager::slaveExecutorEnvironmentDecorator(executorInfo);

    foreach (const Environment::Variable& variable,
             hooksEnvironment.variables()) {
      environment[variable.name()] = variable.value();
    }
  }

  return environment;
}


ostream& operator<<(ostream& stream, const Executor& executor)
{
  stream << "'" << executor.id << "' of framework " << executor.frameworkId;

  if (executor.pid.isSome() && executor.pid.get()) {
    stream << " at " << executor.pid.get();
  } else if (executor.http.isSome() ||
             (executor.slave->state == Slave::RECOVERING &&
              executor.state == Executor::REGISTERING &&
              executor.http.isNone() && executor.pid.isNone())) {
    stream << " (via HTTP)";
  }

  return stream;
}


ostream& operator<<(ostream& stream, Executor::State state)
{
  switch (state) {
    case Executor::REGISTERING: return stream << "REGISTERING";
    case Executor::RUNNING:     return stream << "RUNNING";
    case Executor::TERMINATING: return stream << "TERMINATING";
    case Executor::TERMINATED:  return stream << "TERMINATED";
    default:                    return stream << "UNKNOWN";
  }
}


ostream& operator<<(ostream& stream, Framework::State state)
{
  switch (state) {
    case Framework::RUNNING:     return stream << "RUNNING";
    case Framework::TERMINATING: return stream << "TERMINATING";
    default:                     return stream << "UNKNOWN";
  }
}


ostream& operator<<(ostream& stream, Slave::State state)
{
  switch (state) {
    case Slave::RECOVERING:   return stream << "RECOVERING";
    case Slave::DISCONNECTED: return stream << "DISCONNECTED";
    case Slave::RUNNING:      return stream << "RUNNING";
    case Slave::TERMINATING:  return stream << "TERMINATING";
    default:                  return stream << "UNKNOWN";
  }
}


static string taskOrTaskGroup(
    const Option<TaskInfo>& task,
    const Option<TaskGroupInfo>& taskGroup)
{
  ostringstream out;
  if (task.isSome()) {
    out << "task '" << task->task_id() << "'";
  } else {
    CHECK_SOME(taskGroup);

    vector<TaskID> taskIds;
    foreach (const TaskInfo& task, taskGroup->tasks()) {
      taskIds.push_back(task.task_id());
    }
     out << "task group containing tasks " << taskIds;
  }

  return out.str();
}


static CommandInfo defaultExecutorCommandInfo(
    const string& launcherDir,
    const Option<string>& user)
{
  Result<string> path = os::realpath(
      path::join(launcherDir, MESOS_DEFAULT_EXECUTOR));

  CommandInfo commandInfo;
  if (path.isSome()) {
    commandInfo.set_shell(false);
    commandInfo.set_value(path.get());
    commandInfo.add_arguments(MESOS_DEFAULT_EXECUTOR);
    commandInfo.add_arguments("--launcher_dir=" + launcherDir);
  } else {
    commandInfo.set_shell(true);
    commandInfo.set_value(
        "echo '" +
        (path.isError() ? path.error() : "No such file or directory") +
        "'; exit 1");
  }

  if (user.isSome()) {
    commandInfo.set_user(user.get());
  }

  return commandInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
