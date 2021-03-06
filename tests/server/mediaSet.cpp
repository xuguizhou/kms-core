/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MediaSet
#include <boost/test/unit_test.hpp>
#include <ModuleManager.hpp>
#include <KurentoException.hpp>
#include <gst/gst.h>
#include <MediaSet.hpp>
#include <ServerManagerImpl.hpp>
#include <ServerInfo.hpp>
#include <ModuleInfo.hpp>
#include <ServerType.hpp>
#include <ObjectCreated.hpp>
#include <ObjectDestroyed.hpp>

#include <config.h>

using namespace kurento;

std::shared_ptr<ServerManagerImpl> serverManager;
std::shared_ptr <ModuleManager> moduleManager;

static void
initialize_test_function ()
{
  moduleManager = std::shared_ptr<ModuleManager> (new ModuleManager() );
  std::vector<std::shared_ptr<ModuleInfo>> modules;

  gst_init (NULL, NULL);

  std::string moduleName = "../../src/server/libkmscoremodule.so";

  moduleManager->loadModule (moduleName);

  for (auto moduleIt : moduleManager->getModules () ) {
    std::vector<std::string> factories;

    for (auto factIt : moduleIt.second->getFactories() ) {
      factories.push_back (factIt.first);
    }

    modules.push_back (std::shared_ptr<ModuleInfo> (new ModuleInfo (
                         moduleIt.second->getVersion(), moduleIt.second->getName(), factories) ) );
  }

  std::shared_ptr<ServerType> type (new ServerType (ServerType::KMS) );
  std::vector<std::string> capabilities;
  capabilities.push_back ("transactions");

  std::shared_ptr<ServerInfo> serverInfo = std::shared_ptr <ServerInfo>
      (new ServerInfo ("", modules,
                       type, capabilities) );

  serverManager =  std::dynamic_pointer_cast <ServerManagerImpl>
                   (MediaSet::getMediaSet ()->ref (new ServerManagerImpl (
                         serverInfo, boost::property_tree::ptree () ) ) );
  MediaSet::getMediaSet ()->setServerManager (std::dynamic_pointer_cast
      <ServerManagerImpl> (serverManager) );
}

void
initialize_test ()
{
  static std::once_flag flag;

  std::call_once (flag, initialize_test_function);
}

BOOST_AUTO_TEST_CASE (release_elements)
{
  std::mutex mtx;
  std::condition_variable cv;
  bool created = false;
  bool destroyed = false;
  std::string watched_object;

  std::shared_ptr<kurento::Factory> mediaPipelineFactory;
  std::shared_ptr<kurento::Factory> passThroughFactory;
  std::string mediaPipelineId;
  std::string passThroughId;
  std::string passThrough2Id;

  initialize_test();

  sigc::connection createdConn = serverManager->signalObjectCreated.connect ([&] (
  ObjectCreated event) {
    std::unique_lock<std::mutex> lck (mtx);
    created = true;
    cv.notify_one();
  });

  sigc::connection destroyedConn =
  serverManager->signalObjectDestroyed.connect ([&] (ObjectDestroyed event) {
    std::unique_lock<std::mutex> lck (mtx);

    if (watched_object == event.getObjectId() ) {
      destroyed = true;
      cv.notify_one();
    }
  });

  mediaPipelineFactory = moduleManager->getFactory ("MediaPipeline");
  passThroughFactory = moduleManager->getFactory ("PassThrough");

  mediaPipelineId = mediaPipelineFactory->createObject (
                      boost::property_tree::ptree(), "session1", Json::Value() )->getId();

  // Wait for creation event
  std::unique_lock<std::mutex> lck (mtx);

  if (!cv.wait_for (lck, std::chrono::seconds (1), [&created] () {
  return created;
}) ) {
    BOOST_FAIL ("Timeout waiting for creationg event");
  }
  createdConn.disconnect();

  watched_object = mediaPipelineId;
  lck.unlock();

  Json::Value params;
  params["mediaPipeline"] = mediaPipelineId;

  passThroughId = passThroughFactory->createObject (
                    boost::property_tree::ptree(), "session1", params )->getId();

  passThrough2Id = passThroughFactory->createObject (
                     boost::property_tree::ptree(), "session1", params )->getId();

  // Ref by other session
  MediaSet::getMediaSet()->ref ("session2", mediaPipelineId);
  MediaSet::getMediaSet()->ref ("session2", passThroughId);


  lck.lock();
  watched_object = passThrough2Id;
  lck.unlock();

  // This should destroy passThrow2 but no other elements
  kurento::MediaSet::getMediaSet()->unref ("session1", mediaPipelineId);

  lck.lock();

  if (!cv.wait_for (lck, std::chrono::seconds (1), [&destroyed] () {
  return destroyed;
}) ) {
    BOOST_FAIL ("Timeout waiting for " + watched_object + " destruction event");
  }
  watched_object = "";
  destroyed = false;
  lck.unlock();

  // Check that objects in session2 were not destroyed
  MediaSet::getMediaSet()->ref ("session3", mediaPipelineId);
  MediaSet::getMediaSet()->ref ("session3", passThroughId);

  // Check that object not in session2 were destroyed
  try {
    MediaSet::getMediaSet()->ref ("session3", passThrough2Id);
    BOOST_FAIL ("This code should not be reached");
  } catch (KurentoException e) {
    BOOST_CHECK (e.getCode() == MEDIA_OBJECT_NOT_FOUND);
  }

  lck.lock();
  watched_object = mediaPipelineId;
  lck.unlock();

  kurento::MediaSet::getMediaSet()->release (mediaPipelineId);

  // Wait for pipeline destruction event
  lck.lock();

  if (!cv.wait_for (lck, std::chrono::seconds (1), [&destroyed] () {
  return destroyed;
}) ) {
    BOOST_FAIL ("Timeout waiting for " + watched_object + " destruction event");
  }

  destroyedConn.disconnect();
}
