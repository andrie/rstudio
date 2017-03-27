/*
 * SessionConsoleProcessSocket.hpp
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
#ifndef SESSION_CONSOLE_PROCESS_SOCKET_HPP
#define SESSION_CONSOLE_PROCESS_SOCKET_HPP

#include <string>

#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>

#include <core/Error.hpp>
#include <core/Thread.hpp>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/frame.hpp>

namespace rstudio {
namespace session {
namespace console_process {

struct ConsoleProcessSocketCallbacks
{
   // invoked when socket closes
   boost::function<void ()> onSocketClosed;
};

struct ConsoleProcessSocketConnectionCallbacks
{
   // invoked when input arrives on the socket
   boost::function<void (const std::string& input)> onReceivedInput;

   // invoked when connection opens
   boost::function<void()> onConnectionOpened;

   // invoked when connection closes
   boost::function<void ()> onConnectionClosed;
};

typedef websocketpp::server<websocketpp::config::asio> terminalServer;
typedef terminalServer::message_ptr terminalMessage_ptr;

// Manages a websocket that channels input and output from client for
// interactive terminals. Terminals are identified via a unique handle.
class ConsoleProcessSocket : boost::noncopyable
{
public:
   ConsoleProcessSocket();
   ~ConsoleProcessSocket();

   // start the websocket servicing thread
   core::Error ensureServerRunning(const ConsoleProcessSocketCallbacks& callbacks);

   // stop the websocket servicing thread, if running
   core::Error stopServer();

   // start receiving callbacks for given connection
   core::Error listen(const std::string& terminalHandle,
                      const ConsoleProcessSocketConnectionCallbacks& callbacks);

   // stop listening to given terminal handle
   core::Error stop(const std::string& terminalHandle);

   // send text to client
   core::Error sendText(const std::string& terminalHandle,
                        const std::string& message);

   // stop listening to all terminals
   core::Error stopAll();

   // number of terminals being monitored
   int connectionCount() const;

   // network port for websocket listener; 0 means no port
   int port() const;

private:
   void watchSocket();

   void onMessage(terminalServer* s, websocketpp::connection_hdl hdl,
                  terminalMessage_ptr msg);
   void onClose(terminalServer* s, websocketpp::connection_hdl hdl);
   void onOpen(terminalServer* s, websocketpp::connection_hdl hdl);
   void onHttp(terminalServer* s, websocketpp::connection_hdl hdl);

private:
   std::string handle_;
   ConsoleProcessSocketCallbacks callbacks_;
   ConsoleProcessSocketConnectionCallbacks connectionCallbacks_;

   int port_;
   boost::thread websocketThread_;
   bool serverRunning_;
   terminalServer wsServer_;
   websocketpp::connection_hdl hdl_;
};

} // namespace console_process
} // namespace session
} // namespace rstudio

#endif // SESSION_CONSOLE_PROCESS_SOCKET_HPP