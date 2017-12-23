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

#ifndef __COMPOSING_CONTAINERIZER_HPP__
#define __COMPOSING_CONTAINERIZER_HPP__

#include <map>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/containerizer.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class ComposingContainerizerProcess;


class ComposingContainerizer : public Containerizer
{
public:
  static Try<ComposingContainerizer*> create(
      const std::vector<Containerizer*>& containerizers);

  ComposingContainerizer(
      const std::vector<Containerizer*>& containerizers);

  virtual ~ComposingContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<Containerizer::LaunchResult> launch(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig,
      const std::map<std::string, std::string>& environment,
      const Option<std::string>& pidCheckpointPath);

  virtual process::Future<process::http::Connection> attach(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId);

  virtual process::Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId);

  virtual process::Future<bool> destroy(const ContainerID& containerId);

  virtual process::Future<bool> kill(
      const ContainerID& containerId,
      int signal);

  virtual process::Future<hashset<ContainerID>> containers();

  virtual process::Future<Nothing> remove(const ContainerID& containerId);

  virtual process::Future<Nothing> pruneImages(
      const std::vector<Image>& excludedImages);

private:
  ComposingContainerizerProcess* process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __COMPOSING_CONTAINERIZER_HPP__
