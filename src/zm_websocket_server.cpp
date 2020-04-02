#include "zm_websocket_server.h"
#include "zm_logger.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

WebSocket_Server::WebSocket_Server(Monitor *p_monitor, uint16_t p_port) : monitor(p_monitor), m_port(p_port) {
  // Initialize Asio Transport
  m_server.init_asio();

  // Register handler callbacks

  m_server.set_open_handler(bind(&WebSocket_Server::on_open, this, ::_1));
  m_server.set_close_handler(bind(&WebSocket_Server::on_close, this, ::_1));
  m_server.set_message_handler(bind(&WebSocket_Server::on_message, this, ::_1, ::_2));
  m_server.set_tls_init_handler(bind(&WebSocket_Server::on_tls_init, this, MOZILLA_INTERMEDIATE, ::_1));

} // end WebSocket_Server::WebSocket_Server()

void WebSocket_Server::start() {
    // Start a thread to run the processing loop
    thread *message_processor_thread = new thread(bind(&WebSocket_Server::process_messages, &server_instance));

    websocket_thread = new thread(&WebSocket_Server::run, &server_instance);
}

void WebSocket_Server::run() {
  Debug(1, "Listening on port %d", m_port);
  // listen on specified port
  m_server.listen(m_port);

  Debug(1, "Start accept");
  // Start the server accept loop
  m_server.start_accept();

  // Start the ASIO io_service run loop
  try {
    Debug(1, "Server run");
    m_server.run();
    Debug(1, "Server run");
  } catch (const std::exception & e) {
    Error("Exception %s", e.what());
  } catch (...) {
    Error("Exception");
  }
} // end WebSocket_Server::run()

void WebSocket_Server::stop() {
   websocketpp::lib::error_code ec;
    server.stop_listening(ec);
    if (ec) {
        // Failed to stop listening. Log reason using ec.message().
        return;
    }
    // Close all existing websocket connections.
    string data = "Terminating connection...";
    map<string, connection_hdl>::iterator it;
    for (it = websockets.begin(); it != websockets.end(); ++it) {
        websocketpp::lib::error_code ec;
        server.close(it->second, websocketpp::close::status::normal, data, ec); // send text message.
        if (ec) { // we got an error
            // Error closing websocket. Log reason using ec.message().
        }
    }
     
    // Stop the endpoint.
    server.stop();
} // end WebSocket_Server::stop

std::string WebSocket_Server::get_password() {
    return "";
}

context_ptr WebSocket_Server::on_tls_init(tls_mode mode, websocketpp::connection_hdl hdl) {
  namespace asio = websocketpp::lib::asio;

  Debug(1, "on_tls_init called with hdl: using TLS mode: %s",
      (mode == MOZILLA_MODERN ? "Mozilla Modern" : "Mozilla Intermediate"));

  context_ptr ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);

  try {
    if ( mode == MOZILLA_MODERN ) {
      // Modern disables TLSv1
      ctx->set_options(asio::ssl::context::default_workarounds |
          asio::ssl::context::no_sslv2 |
          asio::ssl::context::no_sslv3 |
          asio::ssl::context::no_tlsv1 |
          asio::ssl::context::single_dh_use);
    } else {
      ctx->set_options(asio::ssl::context::default_workarounds |
          asio::ssl::context::no_sslv2 |
          asio::ssl::context::no_sslv3 |
          asio::ssl::context::single_dh_use);
    }
    ctx->set_password_callback(bind(&WebSocket_Server::get_password, this));
    ctx->use_certificate_chain_file("/etc/letsencrypt/live/pseudo.connortechnology.com/fullchain.pem");
    ctx->use_private_key_file("/etc/letsencrypt/live/pseudo.connortechnology.com/privkey.pem", asio::ssl::context::pem);

    // Example method of generating this file:
    // `openssl dhparam -out dh.pem 2048`
    // Mozilla Intermediate suggests 1024 as the minimum size to use
    // Mozilla Modern suggests 2048 as the minimum size to use.
    ctx->use_tmp_dh_file("/etc/zm/dh.pem");

    std::string ciphers;

    if (mode == MOZILLA_MODERN) {
      ciphers = "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:DHE-RSA-AES256-SHA:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!3DES:!MD5:!PSK";
    } else {
      ciphers = "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:DHE-DSS-AES128-GCM-SHA256:kEDH+AESGCM:ECDHE-RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:ECDHE-RSA-AES256-SHA:ECDHE-ECDSA-AES256-SHA:DHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA256:DHE-RSA-AES256-SHA256:DHE-DSS-AES256-SHA:DHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA256:AES256-SHA256:AES128-SHA:AES256-SHA:AES:CAMELLIA:DES-CBC3-SHA:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!aECDH:!EDH-DSS-DES-CBC3-SHA:!EDH-RSA-DES-CBC3-SHA:!KRB5-DES-CBC3-SHA";
    }

    if (SSL_CTX_set_cipher_list(ctx->native_handle() , ciphers.c_str()) != 1) {
      Error("Error setting cipher list");
    }
  } catch (std::exception& e) {
    Error("Exception: %s", e.what());
  }
  return ctx;
} // end on_tls_init

void WebSocket_Server::on_open(connection_hdl hdl) {
  Debug(1, "on_open");
  {
    lock_guard<mutex> guard(m_action_lock);
    //std::cout << "on_open" << std::endl;
    m_actions.push(action(SUBSCRIBE, hdl));
  }
  m_action_cond.notify_one();
} // end WebSocket_Server::on_open(hdl)

void WebSocket_Server::on_close(connection_hdl hdl) {
  Debug(1, "on_close");
  {
    lock_guard<mutex> guard(m_action_lock);
    //std::cout << "on_close" << std::endl;
    m_actions.push(action(UNSUBSCRIBE,hdl));
  }
  m_action_cond.notify_one();
} // end WebSocket_Server::on_close

void WebSocket_Server::on_message(connection_hdl hdl, server::message_ptr msg) {
  // queue message up for sending by processing thread
  {
    lock_guard<mutex> guard(m_action_lock);
    //std::cout << "on_message" << std::endl;
    Debug(1, "got message: %s", msg->get_payload().c_str());
    json j = json::parse(msg->get_payload().c_str());
    auto type = j["type"].get<std::string>();
    if ( type == "get" ) {
      auto what = j["what"].get<std::string>();
      if ( what == "status" ) {
        Debug(1, "Have status request");
        json status;
        status["status"]["fps"] = monitor->GetFPS();
        status["status"]["state"] = monitor->GetState();
//status["status"]["level"] = monitor->GetLevel();
//
        //server::message_ptr msg = hdl->get_data_message();
//msg->reset(message::frame::opcode::TEXT); // or message::frame::opcode::BINARY
//msg->set_payload(status.dump());
//m_server.send(hdl, msg);
        m_server.send(hdl, status.dump(), websocketpp::frame::opcode::text);
        //, server::message::frame::opcode::TEXT);
        //m_actions.push(action(MESSAGE, hdl, 
      } else {
        Debug(1, "Unknown what %s", what.c_str());
      }
    } else {
      Debug(1, "Unknown type %s", type.c_str());
    }
    //m_actions.push(action(MESSAGE, hdl, msg));
  }
  m_action_cond.notify_one();
} // end WebSocket_Server::on_message

void WebSocket_Server::process_messages() {
  while ( 1 ) {
    unique_lock<mutex> lock(m_action_lock);

    while ( m_actions.empty() ) {
      m_action_cond.wait(lock);
    }

    action a = m_actions.front();
    m_actions.pop();

    lock.unlock();

    if ( a.type == SUBSCRIBE ) {
      lock_guard<mutex> guard(m_connection_lock);
      m_connections.insert(a.hdl);
      Debug(1, "subscribe");
    } else if ( a.type == UNSUBSCRIBE ) {
      lock_guard<mutex> guard(m_connection_lock);
      m_connections.erase(a.hdl);
      Debug(1, "unsubscribe");
    } else if ( a.type == MESSAGE ) {
      lock_guard<mutex> guard(m_connection_lock);

      con_list::iterator it;
      for ( it = m_connections.begin(); it != m_connections.end(); ++it ) {
        Debug(1, "message: %s", a.msg->get_payload().c_str());
        m_server.send(*it, a.msg);
      }
    } else if ( a.type == QUIT ) {
      return;
    } else {
      // undefined.
    }
  } // end while 1
} // end WebSocket_Server::process_messages

void WebSocket_Server::broadcast(std::string message) {
  m_actions.push(action(MESSAGE, hdl, message));
} // end void WebSocket_Server::broadcast(std::string message)
