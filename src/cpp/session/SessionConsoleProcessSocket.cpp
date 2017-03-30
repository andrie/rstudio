/*
 * SessionConsoleProcessSocket.cpp
 *
 * Copyright (C) 2009-17 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <session/SessionConsoleProcessSocket.hpp>

#include <core/FilePath.hpp>
#include <core/json/Json.hpp>


namespace rstudio {
namespace session {
namespace console_process {

using namespace rstudio::core;

namespace {

// rapid reseeding via srand(time) causes same "random" sequence to be
// returned by rand
bool s_didSeedRand = false;


// number of seconds to keep socket open before first connection has been made
int s_joinSeconds = 180;

// number of seconds to wait before shutting down socket when all connections
// have closedj
int s_leaveSeconds = 10;

// the number of active connections, and a timer that shuts us down when we've
// been without connections for too long
int s_activeConnections = 0;
boost::asio::io_service s_ioService;
boost::asio::deadline_timer s_shutdownTimer(s_ioService);

} // anonymous namespace

ConsoleProcessSocket::ConsoleProcessSocket()
   :
     port_(0),
     serverRunning_(false)
{
}

ConsoleProcessSocket::~ConsoleProcessSocket()
{
   try
   {
      stopAllConnections();
      stopServer();
   }
   catch (...)
   {
   }
}

Error ConsoleProcessSocket::ensureServerRunning(
      const ConsoleProcessSocketCallbacks& callbacks)
{
   callbacks_ = callbacks;

   if (serverRunning_)
      return Success();

   long port = 0;
   unsigned portRetries = 0;

   // initialize seed for random port selection
   if (!s_didSeedRand)
   {
      srand(time(NULL));
      s_didSeedRand = true;
   }

   // no user-specified port; pick a random port
   port = 3000 + (rand() % 5000);

   try
   {
      wsServer_.set_access_channels(websocketpp::log::alevel::none);
      wsServer_.init_asio();

      wsServer_.set_message_handler(
               boost::bind(&ConsoleProcessSocket::onMessage, this, &wsServer_, _1, _2));
      wsServer_.set_http_handler(
               boost::bind(&ConsoleProcessSocket::onHttp, this, &wsServer_, _1));
      wsServer_.set_close_handler(
               boost::bind(&ConsoleProcessSocket::onClose, this, &wsServer_, _1));
      wsServer_.set_open_handler(
               boost::bind(&ConsoleProcessSocket::onOpen, this, &wsServer_, _1));

      // try to bind to the given port
      do
      {
         try
         {
            if (core::FilePath("/proc/net/if_inet6").exists())
            {
               // listen will fail without ipv6 support on the machine so we
               // only use it for machines with a ipv6 stack
               // TODO (gary) need a cross-platform way to do this
               wsServer_.listen(port);
            }
            else
            {
               // no ipv6 support, fall back to ipv4
               wsServer_.listen(boost::asio::ip::tcp::v4(), port);
            }

            wsServer_.start_accept();
            break;
         }
         catch (websocketpp::exception const& e)
         {
            // fail if this isn't the code we're expecting
            // (we're only trying to deal with address in use errors here)
            if (e.code() != websocketpp::transport::asio::error::pass_through)
            {
               return systemError(boost::system::errc::invalid_argument,
                                  e.what(), ERROR_LOCATION);
            }

            // try another random port
            port = 3000 + (rand() % 5000);
         }
      }
      while (++portRetries < 20);

      // if we used up all our retries, abort now
      if (portRetries == 20)
      {
         return systemError(boost::system::errc::not_supported,
                            "Couldn't find an available port",
                            ERROR_LOCATION);
      }

      // TODO (gary) Should we have a shutdown timer if there are no
      // connections after a certain period of time?

      // start server
      core::thread::safeLaunchThread(
               boost::bind(&ConsoleProcessSocket::watchSocket, this),
               &websocketThread_);

      port_ = port;
      serverRunning_ = true;
   }
   catch (websocketpp::exception const& e)
   {
      LOG_ERROR_MESSAGE(e.what());
      return systemError(boost::system::errc::invalid_argument,
                            e.what(), ERROR_LOCATION);
   }
   catch (...)
   {
      LOG_ERROR_MESSAGE("Unknown exception starting up terminal websocket");
      return systemError(boost::system::errc::invalid_argument,
                         "Unknown exception", ERROR_LOCATION);
   }
   return Success();
}

void ConsoleProcessSocket::stopServer()
{
   try
   {
      if (serverRunning_)
      {
         stopAllConnections();
         wsServer_.stop();
         serverRunning_ = false;
         port_ = 0;
         websocketThread_.join();
      }
   }
   catch (websocketpp::exception const& e)
   {
      LOG_ERROR_MESSAGE(e.what());
   }
   catch (...)
   {
      LOG_ERROR_MESSAGE("Unknown exception stopping terminal websocket server");
   }
}

Error ConsoleProcessSocket::listen(
      const std::string& terminalHandle,
      const ConsoleProcessSocketConnectionCallbacks& connectionCallbacks)
{
   if (!serverRunning_)
   {
      return systemError(boost::system::errc::not_connected,
                         terminalHandle,
                         ERROR_LOCATION);
   }

   ConsoleProcessSocketConnectionDetails details = connections_.get(terminalHandle);
   details.handle_ = terminalHandle;
   details.connectionCallbacks_ = connectionCallbacks;
   connections_.set(terminalHandle, details);
   return Success();
}

Error ConsoleProcessSocket::stopListening(const std::string& terminalHandle)
{
   ConsoleProcessSocketConnectionDetails details = connections_.get(terminalHandle);
   if (details.handle_.compare(terminalHandle))
   {
      return systemError(boost::system::errc::invalid_argument,
                         terminalHandle,
                         ERROR_LOCATION);
   }

   details = connections_.collect(terminalHandle);
   return Success();
}

Error ConsoleProcessSocket::sendText(const std::string& terminalHandle,
                                     const std::string& message)
{
   // do we know about this handle?
   ConsoleProcessSocketConnectionDetails details = connections_.get(terminalHandle);
   if (details.handle_.compare(terminalHandle))
   {
      std::string msg = "Unknown handle: \"" + terminalHandle + "\"";
      return systemError(boost::system::errc::not_connected, msg, ERROR_LOCATION);
   }

   // make sure this handle still refers to a connection before we try to
   // send data over it
   websocketpp::lib::error_code ec;
   wsServer_.get_con_from_hdl(details.hdl_, ec);
   if (ec.value() > 0)
   {
      return systemError(boost::system::errc::not_connected,
                         ec.message(), ERROR_LOCATION);
   }

   wsServer_.send(details.hdl_, message, websocketpp::frame::opcode::text, ec);
   if (ec)
   {
      return systemError(boost::system::errc::bad_message,
                         ec.message(), ERROR_LOCATION);
   }
   return Success();
}

void ConsoleProcessSocket::stopAllConnections()
{
   connections_.clear();
}

int ConsoleProcessSocket::port() const
{
   return port_;
}

void ConsoleProcessSocket::watchSocket()
{
   wsServer_.run();
}

void ConsoleProcessSocket::onMessage(terminalServer* s,
                                     websocketpp::connection_hdl hdl,
                                     terminalMessage_ptr msg)
{
   std::string handle = getHandle(s, hdl);
   if (handle.empty())
      return;
   ConsoleProcessSocketConnectionDetails details = connections_.get(handle);

   std::string message = msg->get_payload();
   if (details.connectionCallbacks_.onReceivedInput)
      details.connectionCallbacks_.onReceivedInput(message);
}

void ConsoleProcessSocket::onClose(terminalServer* s, websocketpp::connection_hdl hdl)
{
   std::string handle = getHandle(s, hdl);
   if (handle.empty())
      return;

   s_activeConnections--;

   ConsoleProcessSocketConnectionDetails details = connections_.get(handle);

   if (details.connectionCallbacks_.onConnectionClosed)
      details.connectionCallbacks_.onConnectionClosed();
}

void ConsoleProcessSocket::onOpen(terminalServer* s, websocketpp::connection_hdl hdl)
{
   std::string handle = getHandle(s, hdl);
   if (handle.empty())
      return;

   s_activeConnections++;

   // add/update in connections map
   ConsoleProcessSocketConnectionDetails details = connections_.get(handle);
   details.handle_ = handle;
   details.hdl_ = hdl;
   connections_.set(handle, details);

   // notify the specific connection, if available
   if (details.connectionCallbacks_.onConnectionOpened)
      details.connectionCallbacks_.onConnectionOpened();
}

void ConsoleProcessSocket::onHttp(terminalServer* s, websocketpp::connection_hdl hdl)
{
   // We could return diagnostics here if we had some, but for now just 404
   terminalServer::connection_ptr con = s->get_con_from_hdl(hdl);
   con->set_status(websocketpp::http::status_code::not_found);
}

std::string ConsoleProcessSocket::getHandle(terminalServer* s, websocketpp::connection_hdl hdl)
{
   // determine handle from last part of url, e.g. xxxxx from "terminal/xxxxx/"
   terminalServer::connection_ptr con = s->get_con_from_hdl(hdl);
   std::string resource = con->get_resource();
   if (resource.empty() || resource[resource.length() - 1] != '/')
      return std::string();

   // remove mandatory trailing slash
   resource.resize(resource.length() - 1);

   // everything after remaining final slash is the handle
   size_t lastSlash = resource.find_last_of('/');
   if (lastSlash == std::string::npos || lastSlash + 1 >= resource.length())
      return std::string();
   return resource.substr(lastSlash + 1);
}

} // namespace console_process
} // namespace session
} // namespace rstudio
