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

#include "checks/checker_process.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/io.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>
#include <process/time.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/jsonify.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/recordio.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include <stout/os/environment.hpp>
#include <stout/os/killtree.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "internal/evolve.hpp"

#ifdef __linux__
#include "linux/ns.hpp"
#endif

namespace http = process::http;

using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;
using process::Subprocess;

using std::map;
using std::shared_ptr;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {
namespace checks {

#ifndef __WINDOWS__
constexpr char HTTP_CHECK_COMMAND[] = "curl";
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect";
#else
constexpr char HTTP_CHECK_COMMAND[] = "curl.exe";
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect.exe";
#endif // __WINDOWS__

static const string DEFAULT_HTTP_SCHEME = "http";

// Use '127.0.0.1' and '::1' instead of 'localhost', because the
// host file in some container images may not contain 'localhost'.
constexpr char DEFAULT_IPV4_DOMAIN[] = "127.0.0.1";
constexpr char DEFAULT_IPV6_DOMAIN[] = "::1";


#ifdef __linux__
// TODO(alexr): Instead of defining this ad-hoc clone function, provide a
// general solution for entering namespace in child processes, see MESOS-6184.
static pid_t cloneWithSetns(
    const lambda::function<int()>& func,
    const Option<pid_t>& taskPid,
    const vector<string>& namespaces)
{
  auto child = [=]() -> int {
    if (taskPid.isSome()) {
      foreach (const string& ns, namespaces) {
        Try<Nothing> setns = ns::setns(taskPid.get(), ns);
        if (setns.isError()) {
          // This effectively aborts the check.
          LOG(FATAL) << "Failed to enter the " << ns << " namespace of task"
                     << " (pid: " << taskPid.get() << "): " << setns.error();
        }

        VLOG(1) << "Entered the " << ns << " namespace of task"
                << " (pid: " << taskPid.get() << ") successfully";
      }
    }

    return func();
  };

  pid_t pid = ::fork();
  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    // Child.
    ::exit(child());
    UNREACHABLE();
  } else {
    // Parent.
    return pid;
  }
}
#endif


// Reads `ProcessIO::Data` records from a string containing "Record-IO"
// data encoded in protobuf messages, and returns the stdout and stderr.
//
// NOTE: This function ignores any `ProcessIO::Control` records.
//
// TODO(gkleiman): This function is very similar to one in `api_tests.cpp`, we
// should refactor them into a common helper when fixing MESOS-7903.
static Try<tuple<string, string>> decodeProcessIOData(const string& data)
{
  string stdoutReceived;
  string stderrReceived;

  ::recordio::Decoder<v1::agent::ProcessIO> decoder(
      lambda::bind(
          deserialize<v1::agent::ProcessIO>,
          ContentType::PROTOBUF,
          lambda::_1));

  Try<std::deque<Try<v1::agent::ProcessIO>>> records = decoder.decode(data);

  if (records.isError()) {
    return Error(records.error());
  }

  while (!records->empty()) {
    Try<v1::agent::ProcessIO> record = records->front();
    records->pop_front();

    if (record.isError()) {
      return Error(record.error());
    }

    if (record->data().type() == v1::agent::ProcessIO::Data::STDOUT) {
      stdoutReceived += record->data().data();
    } else if (record->data().type() == v1::agent::ProcessIO::Data::STDERR) {
      stderrReceived += record->data().data();
    }
  }

  return std::make_tuple(stdoutReceived, stderrReceived);
}


CheckerProcess::CheckerProcess(
    const CheckInfo& _check,
    const string& _launcherDir,
    const lambda::function<void(const Try<CheckStatusInfo>&)>& _callback,
    const TaskID& _taskId,
    const Option<pid_t>& _taskPid,
    const vector<string>& _namespaces,
    const Option<ContainerID>& _taskContainerId,
    const Option<http::URL>& _agentURL,
    const Option<string>& _authorizationHeader,
    const Option<string>& _scheme,
    const std::string& _name,
    bool _commandCheckViaAgent,
    bool _ipv6)
  : ProcessBase(process::ID::generate("checker")),
    check(_check),
    launcherDir(_launcherDir),
    updateCallback(_callback),
    taskId(_taskId),
    taskPid(_taskPid),
    namespaces(_namespaces),
    taskContainerId(_taskContainerId),
    agentURL(_agentURL),
    authorizationHeader(_authorizationHeader),
    scheme(_scheme),
    name(_name),
    commandCheckViaAgent(_commandCheckViaAgent),
    ipv6(_ipv6),
    paused(false)
{
  Try<Duration> create = Duration::create(check.delay_seconds());
  CHECK_SOME(create);
  checkDelay = create.get();

  create = Duration::create(check.interval_seconds());
  CHECK_SOME(create);
  checkInterval = create.get();

  // Zero value means infinite timeout.
  create = Duration::create(check.timeout_seconds());
  CHECK_SOME(create);
  checkTimeout =
    (create.get() > Duration::zero()) ? create.get() : Duration::max();

#ifdef __linux__
  if (!namespaces.empty()) {
    clone = lambda::bind(&cloneWithSetns, lambda::_1, taskPid, namespaces);
  }
#endif
}


void CheckerProcess::initialize()
{
  scheduleNext(checkDelay);
}


void CheckerProcess::finalize()
{
  LOG(INFO) << "Stopped " << name << " for task '" << taskId << "'";
}


void CheckerProcess::performCheck()
{
  if (paused) {
    return;
  }

  Stopwatch stopwatch;
  stopwatch.start();

  switch (check.type()) {
    case CheckInfo::COMMAND: {
      Future<int> future = commandCheckViaAgent ? nestedCommandCheck()
                                                : commandCheck();
      future.onAny(defer(
          self(),
          &Self::processCommandCheckResult, stopwatch, lambda::_1));
      break;
    }

    case CheckInfo::HTTP: {
      httpCheck().onAny(defer(
          self(),
          &Self::processHttpCheckResult, stopwatch, lambda::_1));
      break;
    }

    case CheckInfo::TCP: {
      tcpCheck().onAny(defer(
          self(),
          &Self::processTcpCheckResult, stopwatch, lambda::_1));
      break;
    }

    case CheckInfo::UNKNOWN: {
      LOG(FATAL) << "Received UNKNOWN check type";
      break;
    }
  }
}


void CheckerProcess::scheduleNext(const Duration& duration)
{
  CHECK(!paused);

  VLOG(1) << "Scheduling " << name << " for task '" << taskId << "' in "
          << duration;

  delay(duration, self(), &Self::performCheck);
}


void CheckerProcess::pause()
{
  if (!paused) {
    VLOG(1) << "Paused " << name << " for task '" << taskId << "'";

    paused = true;
  }
}


void CheckerProcess::resume()
{
  if (paused) {
    VLOG(1) << "Resumed " << name << " for task '" << taskId << "'";

    paused = false;

    // Schedule a check immediately.
    scheduleNext(Duration::zero());
  }
}


void CheckerProcess::processCheckResult(
    const Stopwatch& stopwatch,
    const Result<CheckStatusInfo>& result)
{
  // `Checker` might have been paused while performing the check.
  if (paused) {
    LOG(INFO) << "Ignoring " << name << " result for"
              << " task '" << taskId << "': checking is paused";
    return;
  }

  // `result` will be:
  //
  // 1. `Some(CheckStatusInfo)` if it was possible to perform the check.
  // 2. An `Error` if the check failed due to a non-transient error,
  //    e.g., timed out.
  // 3. `None` if the check failed due to a transient error - this kind of
  //     failure will be silently ignored.
  if (result.isSome()) {
    // It was possible to perform the check.
    VLOG(1) << "Performed " << name << " for task '" << taskId << "' in "
            << stopwatch.elapsed();

    updateCallback(result.get());
  } else if (result.isError()) {
    // The check failed due to a non-transient error.
    updateCallback(Error(result.error()));
  } else {
    // The check failed due to a transient error.
    LOG(INFO) << name << " for task '" << taskId << "' is not available";
  }

  scheduleNext(checkInterval);
}


Future<int> CheckerProcess::commandCheck()
{
  CHECK_EQ(CheckInfo::COMMAND, check.type());
  CHECK(check.has_command());

  const CommandInfo& command = check.command().command();

  map<string, string> environment = os::environment();

  foreach (const Environment::Variable& variable,
           command.environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Launch the subprocess.
  Try<Subprocess> s = Error("Not launched");

  if (command.shell()) {
    // Use the shell variant.
    VLOG(1) << "Launching " << name << " '" << command.value() << "'"
            << " for task '" << taskId << "'";

    s = process::subprocess(
        command.value(),
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        environment,
        clone);
  } else {
    // Use the exec variant.
    vector<string> argv(
        std::begin(command.arguments()), std::end(command.arguments()));

    VLOG(1) << "Launching " << name << " [" << command.value() << ", "
            << strings::join(", ", argv) << "] for task '" << taskId << "'";

    s = process::subprocess(
        command.value(),
        argv,
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        nullptr,
        environment,
        clone);
  }

  if (s.isError()) {
    return Failure("Failed to create subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once it is available.
  const pid_t commandPid = s->pid();
  const string _name = name;
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return s->status()
    .after(
        timeout,
        [timeout, commandPid, _name, _taskId](Future<Option<int>> future)
    {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the external command process.
        VLOG(1) << "Killing the " << _name << " process '" << commandPid
                << "' for task '" << _taskId << "'";

        os::killtree(commandPid, SIGKILL);
      }

      return Failure("Command timed out after " + stringify(timeout));
    })
    .then([](const Option<int>& exitCode) -> Future<int> {
      if (exitCode.isNone()) {
        return Failure("Failed to reap the command process");
      }

      return exitCode.get();
    });
}


Future<int> CheckerProcess::nestedCommandCheck()
{
  CHECK_EQ(CheckInfo::COMMAND, check.type());
  CHECK(check.has_command());
  CHECK_SOME(taskContainerId);
  CHECK_SOME(agentURL);

  VLOG(1) << "Launching " << name << " for task '" << taskId << "'";

  // We don't want recoverable errors, e.g., the agent responding with
  // HTTP status code 503, to trigger a check failure.
  //
  // The future returned by this method represents the result of a
  // check. It will be set to the exit status of the check command if it
  // succeeded, to a `Failure` if there was a non-transient error, and
  // discarded if there was a transient error.
  auto promise = std::make_shared<Promise<int>>();

  if (previousCheckContainerId.isSome()) {
    agent::Call call;
    call.set_type(agent::Call::REMOVE_NESTED_CONTAINER);

    agent::Call::RemoveNestedContainer* removeContainer =
      call.mutable_remove_nested_container();

    removeContainer->mutable_container_id()->CopyFrom(
        previousCheckContainerId.get());

    http::Request request;
    request.method = "POST";
    request.url = agentURL.get();
    request.body = serialize(ContentType::PROTOBUF, evolve(call));
    request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                       {"Content-Type", stringify(ContentType::PROTOBUF)}};

    if (authorizationHeader.isSome()) {
      request.headers["Authorization"] = authorizationHeader.get();
    }

    http::request(request, false)
      .onFailed(defer(self(),
                      [this, promise](const string& failure) {
        LOG(WARNING) << "Connection to remove the nested container '"
                     << previousCheckContainerId.get() << "' used for the "
                     << name << " for task '" << taskId << "' failed: "
                     << failure;

        // Something went wrong while sending the request, we treat this
        // as a transient failure and discard the promise.
        promise->discard();
      }))
      .onReady(defer(self(), [this, promise](const http::Response& response) {
        if (response.code != http::Status::OK) {
          // The agent was unable to remove the check container, we
          // treat this as a transient failure and discard the promise.
          LOG(WARNING) << "Received '" << response.status << "' ("
                       << response.body << ") while removing the nested"
                       << " container '" << previousCheckContainerId.get()
                       << "' used for the " << name << " for task '"
                       << taskId << "'";

          promise->discard();
        }

        previousCheckContainerId = None();
        _nestedCommandCheck(promise);
      }));
  } else {
    _nestedCommandCheck(promise);
  }

  return promise->future();
}


void CheckerProcess::_nestedCommandCheck(shared_ptr<Promise<int>> promise)
{
  // TODO(alexr): Use lambda named captures for
  // these cached values once they are available.
  const TaskID _taskId = taskId;
  const string _name = name;

  http::connect(agentURL.get())
    .onFailed(defer(self(), [_taskId, _name, promise](const string& failure) {
      LOG(WARNING) << "Unable to establish connection with the agent to launch "
                   << _name << " for task '" << _taskId << "'"
                   << ": " << failure;

      // We treat this as a transient failure.
      promise->discard();
    }))
    .onReady(defer(self(), &Self::__nestedCommandCheck, promise, lambda::_1));
}


void CheckerProcess::__nestedCommandCheck(
    shared_ptr<Promise<int>> promise,
    http::Connection connection)
{
  ContainerID checkContainerId;
  checkContainerId.set_value("check-" + id::UUID::random().toString());
  checkContainerId.mutable_parent()->CopyFrom(taskContainerId.get());

  previousCheckContainerId = checkContainerId;

  CommandInfo command(check.command().command());

  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  agent::Call::LaunchNestedContainerSession* launch =
    call.mutable_launch_nested_container_session();

  launch->mutable_container_id()->CopyFrom(checkContainerId);
  launch->mutable_command()->CopyFrom(command);

  http::Request request;
  request.method = "POST";
  request.url = agentURL.get();
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.headers = {{"Accept", stringify(ContentType::RECORDIO)},
                     {"Message-Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  if (authorizationHeader.isSome()) {
    request.headers["Authorization"] = authorizationHeader.get();
  }

  // TODO(alexr): Use a lambda named capture for
  // this cached value once it is available.
  const Duration timeout = checkTimeout;

  auto checkTimedOut = std::make_shared<bool>(false);

  // `LAUNCH_NESTED_CONTAINER_SESSION` returns a streamed response with
  // the output of the container. The agent will close the stream once
  // the container has exited, or kill the container if the client
  // closes the connection.
  //
  // We're calling `Connection::send` with `streamed = false`, so that
  // it returns an HTTP response of type 'BODY' once the entire response
  // is received.
  //
  // This means that this future will not be completed until after the
  // check command has finished or the connection has been closed.
  //
  // TODO(gkleiman): The output of timed-out checks is lost, we'll
  // probably have to call `Connection::send` with `streamed = true`
  // to be able to log it. See MESOS-7903.
  connection.send(request, false)
    .after(checkTimeout,
           defer(self(),
                 [timeout, checkTimedOut](Future<http::Response> future) {
      future.discard();

      *checkTimedOut = true;

      return Failure("Command timed out after " + stringify(timeout));
    }))
    .onFailed(defer(self(),
                    &Self::nestedCommandCheckFailure,
                    promise,
                    connection,
                    checkContainerId,
                    checkTimedOut,
                    lambda::_1))
    .onReady(defer(self(),
                   &Self::___nestedCommandCheck,
                   promise,
                   checkContainerId,
                   lambda::_1));
}


void CheckerProcess::___nestedCommandCheck(
    shared_ptr<Promise<int>> promise,
    const ContainerID& checkContainerId,
    const http::Response& launchResponse)
{
  if (launchResponse.code != http::Status::OK) {
    // The agent was unable to launch the check container,
    // we treat this as a transient failure.
    LOG(WARNING) << "Received '" << launchResponse.status << "' ("
                 << launchResponse.body << ") while launching " << name
                 << " for task '" << taskId << "'";

    promise->discard();
    return;
  }

  Try<tuple<string, string>> checkOutput =
    decodeProcessIOData(launchResponse.body);

  if (checkOutput.isError()) {
    LOG(WARNING) << "Failed to decode the output of the " << name
                 << " for task '" << taskId << "': " << checkOutput.error();
  } else {
    string stdoutReceived;
    string stderrReceived;

    tie(stdoutReceived, stderrReceived) = checkOutput.get();

    LOG(INFO) << "Output of the " << name << " for task '" << taskId
              << "' (stdout):" << std::endl << stdoutReceived;

    LOG(INFO) << "Output of the " << name << " for task '" << taskId
              << "' (stderr):" << std::endl << stderrReceived;
  }

  waitNestedContainer(checkContainerId)
    .onFailed([promise](const string& failure) {
      promise->fail(
          "Unable to get the exit code: " + failure);
    })
    .onReady([promise](const Option<int>& status) -> void {
      if (status.isNone()) {
        promise->fail("Unable to get the exit code");
      // TODO(gkleiman): Make sure that the following block works on Windows.
      } else if (WIFSIGNALED(status.get()) &&
                 WTERMSIG(status.get()) == SIGKILL) {
        // The check container was signaled, probably because the task
        // finished while the check was still in-flight, so we discard
        // the result.
        promise->discard();
      } else {
        promise->set(status.get());
      }
    });
}


void CheckerProcess::nestedCommandCheckFailure(
    shared_ptr<Promise<int>> promise,
    http::Connection connection,
    const ContainerID& checkContainerId,
    shared_ptr<bool> checkTimedOut,
    const string& failure)
{
  if (*checkTimedOut) {
    // The check timed out, closing the connection will make the agent
    // kill the container.
    connection.disconnect();

    // If the check delay interval is zero, we'll try to perform another
    // check right after we finish processing the current timeout.
    //
    // We'll try to remove the container created for the check at the
    // beginning of the next check. In order to prevent a failure, the
    // promise should only be completed once we're sure that the
    // container has terminated.
    waitNestedContainer(checkContainerId)
      .onAny([failure, promise](const Future<Option<int>>&) {
        // We assume that once `WaitNestedContainer` returns,
        // irrespective of whether the response contains a failure, the
        // container will be in a terminal state, and that it will be
        // possible to remove it.
        //
        // This means that we don't need to retry the `WaitNestedContainer`
        // call.
        promise->fail(failure);
      });
  } else {
    // The agent was not able to complete the request, discarding the
    // promise signals the checker that it should retry the check.
    //
    // This will allow us to recover from a blip. The executor will
    // pause the checker when it detects that the agent is not
    // available.
    LOG(WARNING) << "Connection to the agent to launch " << name
                 << " for task '" << taskId << "' failed: " << failure;

    promise->discard();
  }
}


Future<Option<int>> CheckerProcess::waitNestedContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

  agent::Call::WaitNestedContainer* containerWait =
    call.mutable_wait_nested_container();

  containerWait->mutable_container_id()->CopyFrom(containerId);

  http::Request request;
  request.method = "POST";
  request.url = agentURL.get();
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  if (authorizationHeader.isSome()) {
    request.headers["Authorization"] = authorizationHeader.get();
  }

  // TODO(alexr): Use a lambda named capture for
  // this cached value once it is available.
  const string _name = name;

  return http::request(request, false)
    .repair([containerId, _name](const Future<http::Response>& future) {
      return Failure(
          "Connection to wait for " + _name + " container '" +
          stringify(containerId) + "' failed: " + future.failure());
    })
    .then(defer(self(),
                &Self::_waitNestedContainer, containerId, lambda::_1));
}


Future<Option<int>> CheckerProcess::_waitNestedContainer(
    const ContainerID& containerId,
    const http::Response& httpResponse)
{
  if (httpResponse.code != http::Status::OK) {
    return Failure(
        "Received '" + httpResponse.status + "' (" + httpResponse.body +
        ") while waiting on " + name + " container '" +
        stringify(containerId) + "'");
  }

  Try<agent::Response> response =
    deserialize<agent::Response>(ContentType::PROTOBUF, httpResponse.body);
  CHECK_SOME(response);

  CHECK(response->has_wait_nested_container());

  return (
      response->wait_nested_container().has_exit_status()
        ? Option<int>(response->wait_nested_container().exit_status())
        : Option<int>::none());
}


void CheckerProcess::processCommandCheckResult(
    const Stopwatch& stopwatch,
    const Future<int>& future)
{
  CHECK(!future.isPending());

  Result<CheckStatusInfo> result = None();

  // On Posix, `future` corresponds to termination information in the
  // `stat_loc` area. On Windows, `status` is obtained via calling the
  // `GetExitCodeProcess()` function.
  //
  // TODO(alexr): Ensure `WEXITSTATUS` family macros are no-op on Windows,
  // see MESOS-7242.
  if (future.isReady() && WIFEXITED(future.get())) {
    const int exitCode = WEXITSTATUS(future.get());
    LOG(INFO) << name << " for task '" << taskId << "' returned: " << exitCode;

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(check.type());
    checkStatusInfo.mutable_command()->set_exit_code(
        static_cast<int32_t>(exitCode));

    result = Result<CheckStatusInfo>(checkStatusInfo);
  } else if (future.isDiscarded()) {
    // Check's status is currently not available due to a transient error,
    // e.g., due to the agent failover, no `CheckStatusInfo` message should
    // be sent to the callback.
    result = None();
  } else {
    result = Result<CheckStatusInfo>(Error(future.failure()));
  }

  processCheckResult(stopwatch, result);
}


Future<int> CheckerProcess::httpCheck()
{
  CHECK_EQ(CheckInfo::HTTP, check.type());
  CHECK(check.has_http());

  const CheckInfo::Http& http = check.http();

  const string _scheme = scheme.isSome() ? scheme.get() : DEFAULT_HTTP_SCHEME;
  const string path = http.has_path() ? http.path() : "";

  // As per "curl --manual", the square brackets are required to tell curl that
  // it's an IPv6 address, and we need to set "-g" option below to stop curl
  // from interpreting the square brackets as special globbing characters.
  const string domain = ipv6 ?
                        "[" + string(DEFAULT_IPV6_DOMAIN) + "]" :
                        DEFAULT_IPV4_DOMAIN;

  const string url = _scheme + "://" + domain + ":" +
                     stringify(http.port()) + path;

  VLOG(1) << "Launching " << name << " '" << url << "'"
          << " for task '" << taskId << "'";

  const vector<string> argv = {
    HTTP_CHECK_COMMAND,
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follows HTTP 3xx redirects.
    "-k",                 // Ignores SSL validation when scheme is https.
    "-w", "%{http_code}", // Displays HTTP response code on stdout.
    "-o", os::DEV_NULL,   // Ignores output.
    "-g",                 // Switches off the "URL globbing parser".
    url
  };

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = process::subprocess(
      HTTP_CHECK_COMMAND,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      clone);

  if (s.isError()) {
    return Failure(
        "Failed to create the " + string(HTTP_CHECK_COMMAND) +
        " subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once it is available.
  const pid_t curlPid = s->pid();
  const string _name = name;
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout,
        [timeout, curlPid, _name, _taskId](Future<tuple<Future<Option<int>>,
                                                        Future<string>,
                                                        Future<string>>> future)
    {
      future.discard();

      if (curlPid != -1) {
        // Cleanup the HTTP_CHECK_COMMAND process.
        VLOG(1) << "Killing the " << _name << " process " << curlPid
                << " for task '" << _taskId << "'";

        os::killtree(curlPid, SIGKILL);
      }

      return Failure(
          string(HTTP_CHECK_COMMAND) + " timed out after " +
          stringify(timeout));
    })
    .then(defer(self(), &Self::_httpCheck, lambda::_1));
}


Future<int> CheckerProcess::_httpCheck(
    const tuple<Future<Option<int>>, Future<string>, Future<string>>& t)
{
  const Future<Option<int>>& status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the " + string(HTTP_CHECK_COMMAND) +
        " process: " + (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the " + string(HTTP_CHECK_COMMAND) + " process");
  }

  int exitCode = status->get();
  if (exitCode != 0) {
    const Future<string>& commandError = std::get<2>(t);
    if (!commandError.isReady()) {
      return Failure(
          string(HTTP_CHECK_COMMAND) + " " + WSTRINGIFY(exitCode) +
          "; reading stderr failed: " +
          (commandError.isFailed() ? commandError.failure() : "discarded"));
    }

    return Failure(
        string(HTTP_CHECK_COMMAND) + " " + WSTRINGIFY(exitCode) + ": " +
        commandError.get());
  }

  const Future<string>& commandOutput = std::get<1>(t);
  if (!commandOutput.isReady()) {
    return Failure(
        "Failed to read stdout from " + string(HTTP_CHECK_COMMAND) + ": " +
        (commandOutput.isFailed() ? commandOutput.failure() : "discarded"));
  }

  VLOG(1) << "Output of the " << name << " for task '" << taskId
          << "': " << commandOutput.get();

  // Parse the output and get the HTTP status code.
  Try<int> statusCode = numify<int>(commandOutput.get());
  if (statusCode.isError()) {
    return Failure(
        "Unexpected output from " + string(HTTP_CHECK_COMMAND) + ": " +
        commandOutput.get());
  }

  return statusCode.get();
}


void CheckerProcess::processHttpCheckResult(
    const Stopwatch& stopwatch,
    const Future<int>& future)
{
  CHECK(!future.isPending());

  Result<CheckStatusInfo> result = None();

  if (future.isReady()) {
    LOG(INFO) << name << " for task '" << taskId << "'"
              << " returned: " << future.get();

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(check.type());
    checkStatusInfo.mutable_http()->set_status_code(
        static_cast<uint32_t>(future.get()));

    result = Result<CheckStatusInfo>(checkStatusInfo);
  } else if (future.isDiscarded()) {
    // Check's status is currently not available due to a transient error,
    // e.g., due to the agent failover, no `CheckStatusInfo` message should
    // be sent to the callback.
    result = None();
  } else {
    result = Result<CheckStatusInfo>(Error(future.failure()));
  }

  processCheckResult(stopwatch, result);
}


Future<bool> CheckerProcess::tcpCheck()
{
  CHECK_EQ(CheckInfo::TCP, check.type());
  CHECK(check.has_tcp());

  // TCP_CHECK_COMMAND should be reachable.
  CHECK(os::exists(launcherDir));

  const CheckInfo::Tcp& tcp = check.tcp();

  VLOG(1) << "Launching " << name << " for task '" << taskId << "'"
          << " at port " << tcp.port();

  const string command = path::join(launcherDir, TCP_CHECK_COMMAND);
  const string domain = ipv6 ? DEFAULT_IPV6_DOMAIN : DEFAULT_IPV4_DOMAIN;

  const vector<string> argv = {
    command,
    "--ip=" + domain,
    "--port=" + stringify(tcp.port())
  };

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = subprocess(
      command,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      clone);

  if (s.isError()) {
    return Failure(
        "Failed to create the " + command + " subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once they are available.
  pid_t commandPid = s->pid();
  const string _name = name;
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout, [timeout, commandPid, _name, _taskId](
            Future<tuple<Future<Option<int>>,
            Future<string>,
            Future<string>>> future)
    {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the TCP_CHECK_COMMAND process.
        VLOG(1) << "Killing the " << _name << " process " << commandPid
                << " for task '" << _taskId << "'";

        os::killtree(commandPid, SIGKILL);
      }

      return Failure(
          string(TCP_CHECK_COMMAND) + " timed out after " + stringify(timeout));
    })
    .then(defer(self(), &Self::_tcpCheck, lambda::_1));
}


Future<bool> CheckerProcess::_tcpCheck(
    const tuple<Future<Option<int>>, Future<string>, Future<string>>& t)
{
  const Future<Option<int>>& status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the " + string(TCP_CHECK_COMMAND) +
        " process: " + (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the " + string(TCP_CHECK_COMMAND) + " process");
  }

  int exitCode = status->get();

  const Future<string>& commandOutput = std::get<1>(t);
  if (commandOutput.isReady()) {
    VLOG(1) << "Output of the " << name << " for task '" << taskId
            << "': " << commandOutput.get();
  }

  if (exitCode != 0) {
    const Future<string>& commandError = std::get<2>(t);
    if (commandError.isReady()) {
      VLOG(1) << string(TCP_CHECK_COMMAND) << ": " << commandError.get();
    }
  }

  // Non-zero exit code of TCP_CHECK_COMMAND can mean configuration problem
  // (e.g., bad command flag), system error (e.g., a socket cannot be
  // created), or actually a failed connection. We cannot distinguish between
  // these cases, hence treat all of them as connection failure.
  return (exitCode == 0 ? true : false);
}


void CheckerProcess::processTcpCheckResult(
    const Stopwatch& stopwatch,
    const Future<bool>& future)
{
  CHECK(!future.isPending());

  Result<CheckStatusInfo> result = None();

  if (future.isReady()) {
    LOG(INFO) << name << " for task '" << taskId << "'"
              << " returned: " << future.get();

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(check.type());
    checkStatusInfo.mutable_tcp()->set_succeeded(future.get());

    result = Result<CheckStatusInfo>(checkStatusInfo);
  } else if (future.isDiscarded()) {
    // Check's status is currently not available due to a transient error,
    // e.g., due to the agent failover, no `CheckStatusInfo` message should
    // be sent to the callback.
    result = None();
  } else {
    result = Result<CheckStatusInfo>(Error(future.failure()));
  }

  processCheckResult(stopwatch, result);
}

} // namespace checks {
} // namespace internal {
} // namespace mesos {
