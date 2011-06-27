/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * SlpThread.cpp
 * Copyright (C) 2011 Simon Newton
 */

#include <slp.h>
#include <ola/Logging.h>
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include "tools/e133/SlpThread.h"


using std::pair;
using std::string;
using std::vector;

// The service name we use for SLP
const char SlpThread::SERVICE_NAME[] = "service:e133.esta";
const unsigned short SlpThread::MIN_LIFETIME = 5;


void RegisterCallback(SLPHandle slp_handle, SLPError errcode, void* cookie) {
  SLPError *error = static_cast<SLPError*>(cookie);
  *error = errcode;
  (void) slp_handle;
}


/**
 * This is what we pass as a void* to the slp callback
 */
typedef struct {
  SLPError error;
  vector<pair<string, unsigned short> > urls;
} slp_cookie;


/*
 * This is called by the SLP library.
 */
SLPBoolean ServiceCallback(SLPHandle slp_handle,
                           const char* srvurl,
                           unsigned short lifetime,
                           SLPError errcode,
                           void *raw_cookie) {
  slp_cookie *cookie = static_cast<slp_cookie*>(raw_cookie);

  if (errcode == SLP_OK) {
    pair<string, unsigned short> p(srvurl, lifetime);
    cookie->urls.push_back(p);
    cookie->error = SLP_OK;
  } else if (errcode == SLP_LAST_CALL) {
    cookie->error = SLP_OK;
  } else {
    cookie->error = errcode;
  }

  return SLP_TRUE;
  (void) slp_handle;
}


/**
 * Create a new resolver thread. This doesn't actually start it.
 * @param ss the select server to use to handle the callbacks.
 */
SlpThread::SlpThread(ola::network::SelectServer *ss,
                     slp_discovery_callback *discovery_callback,
                     unsigned int refresh_time)
    : OlaThread(),
      m_main_ss(ss),
      m_init_ok(false),
      m_refresh_time(refresh_time),
      m_discovery_callback(discovery_callback),
      m_discovery_timeout(ola::network::INVALID_TIMEOUT) {
  m_incoming_socket.SetOnData(ola::NewCallback(this, &SlpThread::NewRequest));
  m_outgoing_socket.SetOnData(ola::NewCallback(this,
                                               &SlpThread::RequestComplete));
  pthread_mutex_init(&m_incoming_mutex, NULL);
  pthread_mutex_init(&m_outgoing_mutex, NULL);
}


/**
 * Clean up
 */
SlpThread::~SlpThread() {
  Cleanup();
  pthread_mutex_destroy(&m_incoming_mutex);
  pthread_mutex_destroy(&m_outgoing_mutex);
}


/**
 * Setup the SLP Thread
 */
bool SlpThread::Init() {
  if (m_init_ok)
    return true;

  if (!m_incoming_socket.Init())
    return false;

  if (!m_outgoing_socket.Init()) {
    m_incoming_socket.Close();
    return false;
  }

  SLPError err = SLPOpen("en", SLP_FALSE, &m_slp_handle);
  if (err != SLP_OK) {
    OLA_INFO << "Error opening slp handle " << err;
    m_incoming_socket.Close();
    m_outgoing_socket.Close();
    return false;
  }
  m_ss.AddSocket(&m_incoming_socket);
  m_main_ss->AddSocket(&m_outgoing_socket);
  m_init_ok = true;
  return true;
}


/**
 * Start the slp resolver thread
 */
bool SlpThread::Start() {
  if (!m_init_ok)
    return false;
  return OlaThread::Start();
}


/**
 * Stop the resolver thread
 */
bool SlpThread::Join(void *ptr) {
  m_ss.Terminate();
  // kick the select server so the wake up is immediate
  WakeUpSocket(&m_incoming_socket);
  return OlaThread::Join(ptr);
}


/**
 * Clean up
 */
void SlpThread::Cleanup() {
  if (m_incoming_socket.ReadDescriptor() !=
      ola::network::Socket::CLOSED_SOCKET) {
    m_ss.RemoveSocket(&m_incoming_socket);
    m_incoming_socket.Close();
  }


  if (m_outgoing_socket.ReadDescriptor() !=
      ola::network::Socket::CLOSED_SOCKET) {
    m_main_ss->RemoveSocket(&m_outgoing_socket);
    m_outgoing_socket.Close();
  }

  if (m_init_ok)
    SLPClose(m_slp_handle);
  m_init_ok = false;
}


/**
 * Trigger discovery.
 * @param urls a pointer to a vector which will be populated with the urls we
 * find.
 *
 * This call returns immediately. Upon completion, the callback will be invoked
 * in the thread running the select server passed to SlpThread's constructor.
 */
bool SlpThread::Discover() {
  if (m_discovery_callback == NULL) {
    OLA_WARN <<
        "Attempted to run discovery but no callback was passed to "
        "SlpThread(), this is a programming error.";
    return false;
  }

  ola::SingleUseCallback0<void> *callback = ola::NewSingleCallback(
      this,
      &SlpThread::DiscoveryRequest);
  AddToIncomingQueue(callback);
  WakeUpSocket(&m_incoming_socket);
  return true;
}


/**
 * Register a url.
 * This registers a url with slpd, which either registers it with the DA if
 * there is one, or responds to Findsrv requests as a SA.
 * @param callback the callback to run when the registration is complete
 * @param url the url to register
 * @param lifetime the time in seconds for this registration to remain valid
 */
void SlpThread::Register(slp_registration_callback *on_complete,
                         const string &url,
                         unsigned short lifetime) {
  if (lifetime <= SLPD_AGING_TIME_S * 2) {
    OLA_WARN << "Lifetime of " << url <<
      " has been set to less than twice the slpd aging lifetime of " <<
      SLPD_AGING_TIME_S << ", forcing to " << 2 * SLPD_AGING_TIME_S;
    lifetime = 2 * SLPD_AGING_TIME_S;
  }
  ola::SingleUseCallback0<void> *callback = ola::NewSingleCallback(
      this,
      &SlpThread::RegisterRequest,
      on_complete,
      url,
      lifetime);
  AddToIncomingQueue(callback);
  WakeUpSocket(&m_incoming_socket);
}


/**
 * DeRegister a url.
 * This deregisters a url with slpd, which either deregisters it with the DA if
 * there is one, or stops responding to Findsrv requests as a SA.
 * @param callback the callback to run when the registration is complete
 * @param url the url to register
 */
void SlpThread::DeRegister(slp_registration_callback *on_complete,
                           const string &url) {
  ola::SingleUseCallback0<void> *callback = ola::NewSingleCallback(
      this,
      &SlpThread::DeregisterRequest,
      on_complete,
      url);
  AddToIncomingQueue(callback);
  WakeUpSocket(&m_incoming_socket);
}


/**
 * Entry point to the new thread
 */
void *SlpThread::Run() {
  m_ss.Run();
  return NULL;
}


/**
 * Called when a new request arrives
 */
void SlpThread::NewRequest() {
  EmptySocket(&m_incoming_socket);

  while (true) {
    pthread_mutex_lock(&m_incoming_mutex);
    if (m_incoming_queue.empty()) {
      pthread_mutex_unlock(&m_incoming_mutex);
      return;
    }

    ola::BaseCallback0<void> *callback = m_incoming_queue.front();
    m_incoming_queue.pop();
    pthread_mutex_unlock(&m_incoming_mutex);
    callback->Run();
  }
}


/**
 * Called in the main thread when a request completes
 */
void SlpThread::RequestComplete() {
  EmptySocket(&m_outgoing_socket);

  while (true) {
    pthread_mutex_lock(&m_outgoing_mutex);
    if (m_outgoing_queue.empty()) {
      pthread_mutex_unlock(&m_outgoing_mutex);
      return;
    }

    ola::BaseCallback0<void> *callback = m_outgoing_queue.front();
    m_outgoing_queue.pop();
    pthread_mutex_unlock(&m_outgoing_mutex);
    callback->Run();
  }
}


/**
 * Wake up the select server on the other end of this socket by sending some
 * data.
 */
void SlpThread::WakeUpSocket(ola::network::LoopbackSocket *socket) {
  uint8_t wake_up = 'a';
  socket->Send(&wake_up, sizeof(wake_up));
}


/**
 * Read (and discard) any pending data on a socket
 */
void SlpThread::EmptySocket(ola::network::LoopbackSocket *socket) {
  while (socket->DataRemaining()) {
    uint8_t message;
    unsigned int size;
    socket->Receive(&message, sizeof(message), size);
  }
}


/**
 * Add a callback to the incoming queue
 */
void SlpThread::AddToIncomingQueue(ola::SingleUseCallback0<void> *callback) {
  pthread_mutex_lock(&m_incoming_mutex);
  m_incoming_queue.push(callback);
  pthread_mutex_unlock(&m_incoming_mutex);
}


/**
 * Add a callback to the outgoing queue, this also writes to the socket to wake
 * up the remote end
 */
void SlpThread::AddToOutgoingQueue(ola::BaseCallback0<void> *callback) {
  pthread_mutex_lock(&m_outgoing_mutex);
  m_outgoing_queue.push(callback);
  pthread_mutex_unlock(&m_outgoing_mutex);
  WakeUpSocket(&m_outgoing_socket);
}


/**
 * Perform the discovery routine.
 */
void SlpThread::DiscoveryRequest() {
  if (m_discovery_timeout != ola::network::INVALID_TIMEOUT) {
    m_ss.RemoveTimeout(m_discovery_timeout);
  }

  slp_cookie cookie;
  SLPError err = SLPFindSrvs(m_slp_handle,
                             SERVICE_NAME,
                             0,  // use configured scopes
                             0,  // no attr filter
                             ServiceCallback,
                             &cookie);
  bool ok = true;

  if ((err != SLP_OK)) {
    OLA_INFO << "Error finding service with slp " << err;
    ok = false;
  }

  if (cookie.error != SLP_OK) {
    OLA_INFO << "Error finding service with slp " << cookie.error;
    ok = false;
  }

  // figure out the lowest expiry time and use that as the refresh timer.
  // This will cause issues in large networks as all SLP clients will
  // syncronize.
  // TODO(simon): Add jitter here.
  unsigned short next_discovery_duration = m_refresh_time;
  if (ok) {
    vector<pair<string, unsigned short> >::const_iterator iter;
    for (iter = cookie.urls.begin(); iter != cookie.urls.end(); ++iter) {
      next_discovery_duration = std::min(iter->second,
                                         next_discovery_duration);
    }
  }

  url_vector *urls = new url_vector;
  // we lack select1st so we can't use transform
  urls->reserve(cookie.urls.size());
  vector<pair<string, unsigned short> >::const_iterator iter;
  for (iter = cookie.urls.begin(); iter != cookie.urls.end(); ++iter)
    urls->push_back(iter->first);

  OLA_INFO << "next discovery time is " << next_discovery_duration;
  m_discovery_timeout = m_ss.RegisterSingleTimeout(
      next_discovery_duration * 1000,
      ola::NewSingleCallback(this,
                             &SlpThread::DiscoveryTriggered));

  AddToOutgoingQueue(
      NewSingleCallback(this,
                        &SlpThread::DiscoveryActionComplete,
                        ok,
                        urls));
}


/**
 * Register a url with slp
 */
void SlpThread::RegisterRequest(slp_registration_callback *callback,
                                const string url,
                                unsigned short lifetime) {
  lifetime = std::max(lifetime, MIN_LIFETIME);
  unsigned short min_lifetime = SLPGetRefreshInterval();
  OLA_INFO << "min interval from DA is " << min_lifetime;

  if (min_lifetime != 0 && lifetime < min_lifetime)
    lifetime = min_lifetime;

  url_state_map::iterator iter = m_url_map.find(url);
  if (iter != m_url_map.end()) {
    if (iter->second.lifetime == lifetime) {
      OLA_INFO << "New lifetime of " << url << " matches current registration,"
        << " ignoring update";
      AddToOutgoingQueue(
          NewSingleCallback(this,
                            &SlpThread::SimpleActionComplete,
                            callback,
                            true));
      return;
    }
    iter->second.lifetime = lifetime;
    if (iter->second.timeout != ola::network::INVALID_TIMEOUT)
      m_ss.RemoveTimeout(iter->second.timeout);
  } else {
    url_registration_state url_state = {
      lifetime,
      ola::network::INVALID_TIMEOUT
    };
    pair <string, url_registration_state> p(url, url_state);
    iter = m_url_map.insert(p).first;
  }

  bool ok = PerformRegistration(url, lifetime, &(iter->second.timeout));

  // mark as done
  AddToOutgoingQueue(
      NewSingleCallback(this,
                        &SlpThread::SimpleActionComplete,
                        callback,
                        ok));
}


/**
 * Do the actual SLP registration
 */
bool SlpThread::PerformRegistration(const string &url,
                                    unsigned short lifetime,
                                    ola::network::timeout_id *timeout) {
  std::stringstream str;
  str << SERVICE_NAME << "://" << url;
  SLPError callbackerr;
  SLPError err = SLPReg(m_slp_handle,
                        str.str().c_str(),
                        lifetime,
                        0,
                        "",
                        SLP_TRUE,
                        RegisterCallback,
                        &callbackerr);

  bool ok = true;
  if ((err != SLP_OK)) {
    OLA_INFO << "Error registering service with slp " << err;
    ok = false;
  }

  if (callbackerr != SLP_OK) {
    OLA_INFO << "Error registering service with slp " << callbackerr;
    ok = false;
  }

  // schedule timeout
  lifetime = lifetime - SLPD_AGING_TIME_S;
  OLA_INFO << "next registration for " << url << " in " <<
    lifetime;
  *timeout = m_ss.RegisterSingleTimeout(
      (lifetime - 1) * 1000,
      ola::NewSingleCallback(this,
                             &SlpThread::RegistrationTriggered,
                             url));
  return ok;
}


void SlpThread::DeregisterRequest(slp_registration_callback *callback,
                                  const string url) {
  url_state_map::iterator iter = m_url_map.find(url);
  if (iter != m_url_map.end()) {
    OLA_INFO << "erasing " << url << " from map";
    if (iter->second.timeout != ola::network::INVALID_TIMEOUT)
      m_ss.RemoveTimeout(iter->second.timeout);
    m_url_map.erase(iter);
  }

  std::stringstream str;
  str << SERVICE_NAME << "://" << url;
  SLPError callbackerr;
  SLPError err = SLPDereg(m_slp_handle,
                          str.str().c_str(),
                          RegisterCallback,
                          &callbackerr);

  bool ok = true;
  if ((err != SLP_OK)) {
    OLA_INFO << "Error deregistering service with slp " << err;
    ok = false;
  }

  if (callbackerr != SLP_OK) {
    OLA_INFO << "Error deregistering service with slp " << callbackerr;
    ok = false;
  }

  AddToOutgoingQueue(
      NewSingleCallback(this,
                        &SlpThread::SimpleActionComplete,
                        callback,
                        ok));
}


void SlpThread::DiscoveryActionComplete(bool ok, url_vector *urls) {
  m_discovery_callback->Run(ok, *urls);
  delete urls;
}

void SlpThread::SimpleActionComplete(slp_registration_callback *callback,
                                     bool ok) {
  callback->Run(ok);
}


/**
 * Called when the discovery timer triggers. This usually means that the
 * lifetime of one of the discovered urls has expired and we need to check if
 * it's still active.
 */
void SlpThread::DiscoveryTriggered() {
  OLA_INFO << "scheduled next discovery run";
  // set the discovery_timeout to invalid so we don't try and remove it while
  // it's running.
  m_discovery_timeout = ola::network::INVALID_TIMEOUT;
  DiscoveryRequest();
}


/**
 * Called when the lifetime for a service has expired and we need to register
 * it again.
 */
void SlpThread::RegistrationTriggered(string url) {
  OLA_INFO << "register " << url << " again";
  url_state_map::iterator iter = m_url_map.find(url);
  if (iter != m_url_map.end())
    PerformRegistration(url, iter->second.lifetime, &(iter->second.timeout));
}