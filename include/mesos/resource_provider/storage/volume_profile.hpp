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

#ifndef __MESOS_RESOURCE_PROVIDER_VOLUME_PROFILE_HPP__
#define __MESOS_RESOURCE_PROVIDER_VOLUME_PROFILE_HPP__

#include <memory>
#include <string>
#include <tuple>

#include <process/future.hpp>

#include <stout/hashset.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>

#include <csi/spec.hpp>

namespace mesos {

/**
 * This module is used by Storage Resource Providers to translate the
 * "profile" field of a `Resource::DiskInfo::Source` into fields that a
 * Container Storage Interface (CSI) plugin can potentially
 * understand. This affects the following CSI requests:
 *   * ControllerPublishVolumeRequest
 *   * CreateVolumeRequest
 *   * GetCapacityRequest
 *   * NodePublishVolumeRequest
 *   * ValidateVolumeCapabilitiesRequest
 *
 * This module is not intended to interact with any CSI plugins directly.
 *
 * Documentation for each of the CSI requests can be found at:
 *   https://github.com/container-storage-interface/spec/
 */
class VolumeProfileAdaptor
{
public:
  struct ProfileInfo
  {
    /**
     * Corresponds to the `volume_capability` or `volume_capabilities`
     * fields of the affected CSI requests listed in the module description.
     *
     * NOTE: Some CSI requests take a list of `VolumeCapability` objects.
     * However, because there is no existing way to choose between a list
     * of `VolumeCapability` objects, the module will only allow a single
     * `VolumeCapability`.
     */
    csi::VolumeCapability capability;

    /**
     * Free-form key-value pairs which should be passed into the body
     * of a `CreateVolumeRequest`. Neither Mesos nor this module is expected
     * to act upon these values (except copying them into the request).
     */
    google::protobuf::Map<std::string, std::string> parameters;
  };

  /**
   * Factory method used to create a VolumeProfileAdaptor instance.
   * If the `name` parameter is provided, the module is instantiated
   * using the `ModuleManager`. Otherwise, a "default" volume profile
   * adaptor instance (defined in `src/resource_provider/volume_profile.cpp`)
   * is returned.
   *
   * NOTE: The lifecycle of the returned object is delegated to the caller.
   */
  static Try<VolumeProfileAdaptor*> create(
      const Option<std::string>& name = None());

  /**
   * Global methods for setting and getting a VolumeProfileAdaptor instance.
   *
   * The agent (or test) is expected to create and set the adaptor instance
   * and manage the pointer (this method will only keep a weak pointer).
   * Each component that needs to use the VolumeProfileAdaptor, such as the
   * Storage Local Resource Provider, should call `getAdaptor`.
   */
  static void setAdaptor(const std::shared_ptr<VolumeProfileAdaptor>& adaptor);
  static std::shared_ptr<VolumeProfileAdaptor> getAdaptor();

  virtual ~VolumeProfileAdaptor() {}

  /**
   * Called before a Storage Resource Provider makes an affected CSI request.
   * The caller is responsible for copying the returned values into the request
   * object.
   *
   * This method is expected to return a Failure if a matching "profile"
   * cannot be found or retrieved. The caller should not proceed with
   * any of the affected CSI requests if this method returns a failure.
   *
   * The `csiPluginInfoType` parameter is the `CSIPluginInfo::type` field
   * found inside `ResourceProviderInfo::storage`. This module may choose to
   * filter results based on the type of CSI plugin.
   *
   * NOTE: This module assumes that profiles are immutable after creation.
   * Changing the `VolumeCapability` or Parameters of a profile after creation
   * may result in undefined behavior from the SLRP or CSI plugins.
   */
  virtual process::Future<ProfileInfo> translate(
      const std::string& profile,
      const std::string& csiPluginInfoType) = 0;

  /**
   * Returns a future that will be satisifed iff the set of profiles known
   * by the module differs from the `knownProfiles` parameter.
   *
   * The `csiPluginInfoType` parameter is the `CSIPluginInfo::type` field
   * found inside `ResourceProviderInfo::storage`. This module may choose to
   * filter results based on the type of CSI plugin.
   *
   * NOTE: It is highly recommended for the module to insert a random delay
   * between discovering a different set of profiles and satisfying this
   * future, because the SLRP is expected to update the set of offered
   * resources based on this future. Adding a random delay may prevent
   * a thundering herd of resource updates to the Mesos master.
   */
  virtual process::Future<hashset<std::string>> watch(
      const hashset<std::string>& knownProfiles,
      const std::string& csiPluginInfoType) = 0;

protected:
  VolumeProfileAdaptor() {}
};

} // namespace mesos {

#endif // __MESOS_RESOURCE_PROVIDER_VOLUME_PROFILE_HPP__
