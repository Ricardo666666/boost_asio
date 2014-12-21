#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <exception>
#include <iostream>
#include <string>
#include <deque>
#include <set>
#include "boost/asio.hpp"
#include "boost/thread.hpp"
#include "boost/bind.hpp"
#include "boost/shared_ptr.hpp"
#include "boost/enable_shared_from_this.hpp"
#include "boost/thread/thread.hpp"
#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/atomic.hpp"
#include "boost/program_options/options_description.hpp"
#include "boost/program_options/parsers.hpp"
#include "boost/program_options/variables_map.hpp"
#include "boost/log/common.hpp"
#include "boost/log/core.hpp"
#include "boost/log/sinks.hpp"
#include "boost/log/attributes.hpp"
#include "boost/log/expressions.hpp"
#include "boost/log/trivial.hpp"
#include "boost/log/utility/setup/common_attributes.hpp"
#include "boost/log/utility/exception_handler.hpp"
#include "boost/log/support/date_time.hpp"

namespace {
BOOST_LOG_INLINE_GLOBAL_LOGGER_DEFAULT(global_logger_src, 
    boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level>);

#define LOG(level) BOOST_LOG_FUNCTION();BOOST_LOG_SEV(global_logger_src::get(), boost::log::trivial::level)

void InitLogging(bool open_debug, const std::string& log_dir) {
  // ���ͨ������(ʱ��,����ID,�߳�ID)
  boost::log::add_common_attributes();
  // ��ȡcore, �Ա�����ע��sink
  boost::shared_ptr<boost::log::core> core = boost::log::core::get();
  // ������������ԣ�������,Դ�ļ���,�к�)
  core->add_global_attribute("Scope", boost::log::attributes::named_scope());
  // ��������log������׳����쳣
  core->set_exception_handler(boost::log::make_exception_suppressor());
  // �رյ�����־
  if (!open_debug) {
    core->set_filter(boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") >= boost::log::trivial::info);
  }

  // ����3��sink:
  // 1,severity<=debug����������sink_trace_debug
  // 2,debug<severity<=warning����sink_info_warning
  // 3,warning<severity<=fatal���������sink_error_fatal
  typedef boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend> sync_sink_frontend;
  
  boost::shared_ptr<boost::log::sinks::text_file_backend> sink_trace_debug_backend =
    boost::make_shared<boost::log::sinks::text_file_backend>(
        boost::log::keywords::file_name = log_dir + "/echo_server.trace_debug.%Y%m%d.%H%M.%N.log",
        boost::log::keywords::rotation_size = 1024 * 1024 * 1024,                                     
        boost::log::keywords::open_mode = std::ios::app,
        boost::log::keywords::auto_flush = true
    );  

  boost::shared_ptr<sync_sink_frontend> sink_trace_debug_frontend(new sync_sink_frontend(sink_trace_debug_backend));
  sink_trace_debug_frontend->set_filter(boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <= boost::log::trivial::debug);
  sink_trace_debug_frontend->set_formatter(boost::log::expressions::stream << "[" <<
          boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S") <<
          "] [" << boost::log::expressions::attr<boost::log::attributes::current_process_id::value_type>("ProcessID") << 
          "-" << boost::log::expressions::attr<boost::log::attributes::current_thread_id::value_type>("ThreadID") << "] [" <<
          boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <<
          "] [" << boost::log::expressions::format_named_scope("Scope", boost::log::keywords::format = "%n[%f:%l]", 
            boost::log::keywords::depth = 1) << "] " << boost::log::expressions::smessage);
  core->add_sink(sink_trace_debug_frontend);
  
  boost::shared_ptr<boost::log::sinks::text_file_backend> sink_info_warning_backend =
    boost::make_shared<boost::log::sinks::text_file_backend>(
        boost::log::keywords::file_name = log_dir + "/echo_server.info_warning.%Y%m%d.%H%M.%N.log", 
        boost::log::keywords::rotation_size = 1024 * 1024 * 1024,                                     
        boost::log::keywords::open_mode = std::ios::app,
        boost::log::keywords::auto_flush = true
    );

  boost::shared_ptr<sync_sink_frontend> sink_info_warning_frontend(new sync_sink_frontend(sink_info_warning_backend));
  sink_info_warning_frontend->set_filter(boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <= 
      boost::log::trivial::warning && boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") > 
    boost::log::trivial::debug);
  sink_info_warning_frontend->set_formatter(boost::log::expressions::stream << "[" <<
          boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S") <<
          "] [" << boost::log::expressions::attr<boost::log::attributes::current_process_id::value_type>("ProcessID") << 
          "-" << boost::log::expressions::attr<boost::log::attributes::current_thread_id::value_type>("ThreadID") << "] [" <<
          boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <<
          "] " << boost::log::expressions::smessage);
  core->add_sink(sink_info_warning_frontend);
  
  boost::shared_ptr<boost::log::sinks::text_file_backend> sink_error_fatal_backend =
    boost::make_shared<boost::log::sinks::text_file_backend>(
        boost::log::keywords::file_name = log_dir + "/echo_server.error_fatal.%Y%m%d.%H%M.%N.log",                                        
        boost::log::keywords::rotation_size = 1024 * 1024 * 1024,                                     
        boost::log::keywords::open_mode = std::ios::app,
        boost::log::keywords::auto_flush = true
    );

  boost::shared_ptr<sync_sink_frontend> sink_error_fatal_frontend(new sync_sink_frontend(sink_info_warning_backend));
  sink_error_fatal_frontend->set_filter(boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <= 
      boost::log::trivial::fatal && boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") > 
    boost::log::trivial::warning);
  sink_error_fatal_frontend->set_formatter(boost::log::expressions::stream << "[" <<
          boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S") <<
          "] [" << boost::log::expressions::attr<boost::log::attributes::current_process_id::value_type>("ProcessID") << 
          "-" << boost::log::expressions::attr<boost::log::attributes::current_thread_id::value_type>("ThreadID") << "] [" <<
          boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <<
          "] " << boost::log::expressions::smessage);
  core->add_sink(sink_error_fatal_frontend);
}

class EchoServer;

typedef boost::shared_ptr<EchoServer> EchoServerPtr;
typedef boost::shared_ptr<boost::asio::io_service> IOServicePtr;
typedef boost::shared_ptr<boost::asio::ip::tcp::socket> SocketPtr;
class Connection;
typedef boost::shared_ptr<Connection> ConnPtr;
typedef boost::shared_ptr<std::string> StringPtr;
typedef boost::shared_ptr<boost::asio::deadline_timer> TimerPtr;

// ׼��1:
// һ��Socket��Զ��Ҫ����async_read/async_write����1��,���Բο�boost doc:
// This operation is implemented in terms of zero or more calls to the stream's async_write_some function, and is known as a composed operation. The program must ensure that the stream performs no other write operations (such as async_write, the stream's async_write_some function, or any other composed operations that perform writes) until this operation completes.
// Ҳ����һ��Ҫǰһ��async��������ٷ�����һ��!!

// ׼��2:
// ����1��socket, �ڶ��߳�������һ��Ҫ��������, һ�Ѵ������һ������, �����÷����Ƿ��̰߳�ȫ��.
// Ҳ����˵ͬ��close/async_read/async_write/async_connect���ĸ��������ü���.

class Connection : public boost::enable_shared_from_this<Connection> {
public:
  enum ConnStatus {
    kConnected = 0,
    kError = 1,
    kClosed = 2,
  };
  Connection(SocketPtr socket) : status_(kConnected), socket_(socket) {
  }
  ~Connection() {
    // ���������ｫwrite_queue�еĴ�����Ϣ�������Ե��߼�����
    LOG(debug) << __FUNCTION__;
  }
  void Start() { 
    socket_->async_receive(boost::asio::buffer(msgbuf_, sizeof(msgbuf_)), boost::bind(&Connection::ReadHandler, shared_from_this(), _1, _2));
  }
  void Close() { // �ظ��ĵ���socket��closeû������, �����ܲ�������close(����Close�ӿڱ�¶���û�,�������������).
    if (status_.exchange(kClosed) != kClosed) { // �����ظ�����socket��close��û�������, ��������Ҳ��֤Closeֻ�ܱ����һ��.
      boost::lock_guard<boost::mutex> guard(socket_mutex_);
      boost::system::error_code errcode;
      if (socket_->close(errcode)) {
        LOG(warning) << "Close Connection Error";
      } else {
        LOG(info) << "Close Connection Done";
      }
    }
  }
  ConnStatus status() { return status_.load(); }
private:
  void ReadHandler(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (!error) { // û�з�������(������ȡ��), ��ô������һ�ζ�ȡ.
      // �ú�������һЩ���ݾͻ᷵��, ���������������echo�߼�. ���ϣ����ȡָ���������ǰ������, ʹ��async_read.
      {
        boost::lock_guard<boost::mutex> guard(socket_mutex_);
        socket_->async_receive(boost::asio::buffer(msgbuf_, sizeof(msgbuf_)), boost::bind(&Connection::ReadHandler, shared_from_this(), _1, _2));
      }
      // ����չʾһ������ڶ��߳�asio����ȷ��ʹ��async_write����ķ���echo, ���Ҵ�������Ϣ�����Ա���socketʧЧʱ�л��ᷢ����Ϣ�ط�.
      EchoMsg(StringPtr(new std::string(msgbuf_, bytes_transferred)));
    } else if (error == boost::asio::error::operation_aborted) {
      LOG(trace) << "Connection ReadHandler Canceled.";
    } else {
      ConnStatus expected = kConnected;
      if (status_.compare_exchange_strong(expected, kError)) {
        LOG(warning) << "ReadHandler Error.";
      }
    }
  }
  void WriteHandler(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (!error) {
      boost::lock_guard<boost::mutex> guard(socket_mutex_);
      write_queue_.pop_front();
      if (write_queue_.size()) {
        StringPtr next_msg = write_queue_.front();
        // async_write��֤����ȫ��д��ص�.
        async_write(*socket_, boost::asio::buffer(*next_msg), boost::bind(&Connection::WriteHandler, shared_from_this(), _1, _2));
      }
    } else if (error == boost::asio::error::operation_aborted) {
      LOG(trace) << "Connection WriteHandler Canceled.";
    } else {
      ConnStatus expected = kConnected;
      if (status_.compare_exchange_strong(expected, kError)) {
        LOG(warning) << "WriteHandler Error.";
      }
    }
  }
  void EchoMsg(StringPtr msg) {
    LOG(debug) << "EchoMsg: " << *msg;
    boost::lock_guard<boost::mutex> guard(socket_mutex_);
    write_queue_.push_back(msg);
    if (write_queue_.size() == 1) {
      async_write(*socket_, boost::asio::buffer(*msg), boost::bind(&Connection::WriteHandler, shared_from_this(), _1, _2));
    }
  }
  std::deque<StringPtr> write_queue_;
  boost::mutex socket_mutex_;
  boost::atomic<ConnStatus> status_;
  char msgbuf_[1024 * 16];
  SocketPtr socket_;
};

class EchoServer : public boost::enable_shared_from_this<EchoServer> {
public:
  EchoServer(IOServicePtr io_service) : stopped_(false), io_service_(io_service), acceptor_(*io_service) {
  }
  ~EchoServer() {
    // ��Stop�����߳��ͷ����ü���, �ȴ�io_service������ʣ���¼�������, ��ʱ�������������Ӽ���,
    // ����Close������Socket���ͷ����ü���.
    LOG(trace) << __FUNCTION__;
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    for (ConnSetIter iter = conn_set_.begin(); iter != conn_set_.end(); ++iter) {
      (*iter)->Close();
    }
  }
  bool Start(const std::string& host, unsigned short port) {
    boost::system::error_code errcode;
    boost::asio::ip::address address = boost::asio::ip::address::from_string(host, errcode);
    if (errcode) {
      return false;
    }
    if (acceptor_.open(boost::asio::ip::tcp::v4(), errcode)) {
      return false;
    }
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    boost::asio::ip::tcp::endpoint endpoint(address, port);
    if (acceptor_.bind(endpoint, errcode) || acceptor_.listen(1024, errcode)) {
      acceptor_.close();
      return false;
    }
    SocketPtr socket(new boost::asio::ip::tcp::socket(*io_service_));
    acceptor_.async_accept(*socket, boost::bind(&EchoServer::AcceptHandler, shared_from_this(), socket, _1));
    return true;
  }
  void Stop() {
    boost::lock_guard<boost::mutex> guard(socket_mutex_);
    boost::system::error_code errcode;
    if (acceptor_.close(errcode)) {
      LOG(warning) << "Close Acceptor Error";
    }
    stopped_.store(true);
  }
private:
  void AcceptHandler(SocketPtr socket, const boost::system::error_code& error) { // û�в�������
    if (error == boost::asio::error::operation_aborted) { // ��Acceptor���رն�Cancel, ����Ҫ���κ�����.
      LOG(trace) << "Accept Canceled";
      return; // �û������ر���Server, ��˲�����Cancel
    } else if (!error) { // �ɹ�Accept, ����һ���µ�Connection.
      LOG(info) << "Accept New Connection";
      ConnPtr new_conn(new Connection(socket));
      new_conn->Start();
      {
        boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
        conn_set_.insert(new_conn);
      }
      TimerPtr socket_timer(new boost::asio::deadline_timer(*io_service_));
      socket_timer->expires_from_now(boost::posix_time::seconds(1));
      socket_timer->async_wait(boost::bind(&EchoServer::CheckSocketStatus, shared_from_this(), new_conn, socket_timer, _1));
    } else {
      LOG(error) << "Accept Error";
    }
    SocketPtr new_socket(new boost::asio::ip::tcp::socket(*io_service_));
    boost::lock_guard<boost::mutex> guard(socket_mutex_);
    acceptor_.async_accept(*new_socket, boost::bind(&EchoServer::AcceptHandler, shared_from_this(), new_socket, _1));
  }
  void CheckSocketStatus(ConnPtr conn, TimerPtr socket_timer, const boost::system::error_code& error) {
    // 1, EchoServer�Ѿ���Stop����, ��ô����ֹͣtimer�ͷŵ���EchoServer�����ü���, ��EchoServer������������
    // 2, �ж�conn->status()==kError��Close���Ӳ���ConnSet���Ƴ�.
    // 3, �ж�conn->status()==kClosed���ConnSet���Ƴ�.(�����û����Ի�ȡSocketPtr����ʱ����Close)
    // 4, ��������, ����������һ��timer.
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    ConnSetIter iter = conn_set_.find(conn);
    assert(iter != conn_set_.end());
    if (stopped_.load()) {
      // case 1
      //LOG(debug) << "case 1";
    } else if (conn->status() == Connection::kError) { // case 2
      //LOG(debug) << "case 2";
      conn->Close();
      conn_set_.erase(conn);
    } else if (conn->status() == Connection::kClosed) {// case 3
      //LOG(debug) << "case 3";
      conn_set_.erase(conn);
    } else {
      //LOG(debug) << "case 4"; // case 4
      socket_timer->expires_from_now(boost::posix_time::seconds(1));
      socket_timer->async_wait(boost::bind(&EchoServer::CheckSocketStatus, shared_from_this(), conn, socket_timer, _1));
    }
  }
  typedef std::set<ConnPtr> ConnSet;
  typedef ConnSet::iterator ConnSetIter;
  boost::atomic<bool> stopped_;
  boost::mutex socket_mutex_;
  boost::mutex conn_set_mutex_;
  ConnSet conn_set_;
  IOServicePtr io_service_;
  boost::asio::ip::tcp::acceptor acceptor_; // auto-close while destructor.
};
volatile sig_atomic_t g_shutdown_server = 0;
void ShutdownServerHandler(int signo) {
  g_shutdown_server = 1;
}
void SetupSignalHandler() {
  sigset_t sigset;
  sigfillset(&sigset);
  sigdelset(&sigset, SIGTERM);
  sigdelset(&sigset, SIGINT);
  sigprocmask(SIG_SETMASK, &sigset, NULL);

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = ShutdownServerHandler;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
}
void AsioThreadMain(IOServicePtr io_service) {
  // ���̵߳������io_service��leader-followerģ��
  // ��ʼ������һ��EchoServer��Acceptor������, ���̵߳���Stop��Reset�ͷ����ú�,
  // io_service�ᴦ����acceptorʣ���¼����ͷ����ü����Ӷ�ʹechoserver����, ��echoserver������
  // �Ὣ�������ߵ�socket����close���ͷ����ü���, ��io_service����������socket��ʣ���¼����ͷ����ü���
  // �Ӷ�ʹ����socket����, ����io_service�Ͻ����κ��¼�, �Զ��˳��߳�.
  io_service->run();
}
bool ParseCommands(int argc, char** argv, boost::program_options::variables_map* options) {
  boost::program_options::options_description desc("Usage");
  desc.add_options()
      ("help", "show how to use this program")
      ("thread,t", boost::program_options::value<uint32_t>()->default_value(12), "number of threads of asio")
      ("port,p", boost::program_options::value<unsigned short>()->required(), "the tcp port server binds to")
      ("config,c", boost::program_options::value<std::string>(), "read config from file")
      ("log,l", boost::program_options::value<std::string>()->default_value("./log"), "the directory to write log")
      ("debug,d", "open debug mode for logging");
  try {
    // ����������
    boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), *options); 
    if (options->count("help")) { 
      std::cerr << desc << std::endl;
      return false;
    }
    if (options->count("config")) { // �����ļ���Ϊ����
      std::string cfile = (*options)["config"].as<std::string>();
      boost::program_options::store(boost::program_options::parse_config_file<char>(cfile.c_str(), desc), *options);
    }
    boost::program_options::notify(*options); // ���մ�������У��
  } catch (std::exception& except) {
    std::cerr << except.what() << std::endl;
    std::cerr << desc << std::endl;
    return false;
  }
  return true;
}
}

int main(int argc, char** argv) {
  boost::program_options::variables_map options;
  if (!ParseCommands(argc, argv, &options)) {
    return -1;
  }
  InitLogging(options.count("debug"), options["log"].as<std::string>());
  
  SetupSignalHandler();

  IOServicePtr io_service(new boost::asio::io_service());

  unsigned short port = options["port"].as<unsigned short>();

  EchoServerPtr echo_server(new EchoServer(io_service));
  if (!echo_server->Start("0.0.0.0", port)) {
    return -1;
  }
  uint32_t thread_num = options["thread"].as<uint32_t>();
  boost::thread_group asio_threads;
  for (uint32_t i = 0; i < thread_num; ++i) {
    asio_threads.create_thread(boost::bind(AsioThreadMain, io_service));
  }

  while (!g_shutdown_server) {
    sleep(1);
  }
  echo_server->Stop(); // �رռ�����
  echo_server.reset();   // �ͷ����ü���, ��echo_server����.
  asio_threads.join_all(); // �ȴ�asio��Ȼ�˳�
  LOG(info) << "Stopped.. ..";
  return 0;
}
