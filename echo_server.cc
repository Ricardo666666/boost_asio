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

namespace {
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
    std::cout << __FUNCTION__ << std::endl;
  }
  void Start() { 
    socket_->async_receive(boost::asio::buffer(msgbuf_, sizeof(msgbuf_)), boost::bind(&Connection::ReadHandler, shared_from_this(), _1, _2));
  }
  void Close() { // �ظ��ĵ���socket��closeû������, �����ܲ�������close(����Close�ӿڱ�¶���û�,�������������).
    if (status_.exchange(kClosed) != kClosed) { // �����ظ�����socket��close��û�������, ��������Ҳ��֤Closeֻ�ܱ�����һ��.
      boost::lock_guard<boost::mutex> guard(socket_mutex_);
      boost::system::error_code errcode;
      if (socket_->close(errcode)) {
        std::cerr << "Close Connection Error" << std::endl;
      } else {
        std::cerr << "Close Connection Done" << std::endl;
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
      //printf("%.*s", (int)bytes_transferred, msgbuf_);
      // ����չʾһ������ڶ��߳�asio����ȷ��ʹ��async_write����ķ���echo, ���Ҵ�������Ϣ�����Ա���socketʧЧʱ�л��ᷢ����Ϣ�ط�.
      EchoMsg(StringPtr(new std::string(msgbuf_, bytes_transferred)));
    } else if (error == boost::asio::error::operation_aborted) {
      std::cout << "Connection ReadHandler Canceled." << std::endl;
    } else {
      ConnStatus expected = kConnected;
      if (status_.compare_exchange_strong(expected, kError)) {
        std::cout << "ReadHandler Error." << std::endl;
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
      std::cout << "Connection WriteHandler Canceled." << std::endl;
    } else {
      ConnStatus expected = kConnected;
      if (status_.compare_exchange_strong(expected, kError)) {
        std::cout << "WriteHandler Error." << std::endl;
      }
    }
  }
  void EchoMsg(StringPtr msg) {
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
    std::cout << __FUNCTION__ << std::endl;
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
      std::cerr << "Close Acceptor Error" << std::endl;
    }
    stopped_.store(true);
  }
private:
  void AcceptHandler(SocketPtr socket, const boost::system::error_code& error) { // û�в�������
    if (error == boost::asio::error::operation_aborted) { // ��Acceptor���رն�Cancel, ����Ҫ���κ�����.
      std::cout << "Accept Canceled" << std::endl;
      return; // �û������ر���Server, ��˲�����Cancel
    } else if (!error) { // �ɹ�Accept, ����һ���µ�Connection.
      std::cout << "Accept New Connection" << std::endl;
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
      std::cout << "Accept Error" << std::endl;
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
      //std::cout << "case 1" << std::endl;
    } else if (conn->status() == Connection::kError) { // case 2
      //std::cout << "case 2" << std::endl;
      conn->Close();
      conn_set_.erase(conn);
    } else if (conn->status() == Connection::kClosed) {// case 3
      //std::cout << "case 3" << std::endl;
      conn_set_.erase(conn);
    } else {
      //std::cout << "case 4" << std::endl; // case 4
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
      ("help,h", "show how to use this program")
      ("port,p", boost::program_options::value<unsigned short>()->required(), "the tcp port server binds to")
      ("config,c", boost::program_options::value<std::string>(), "read config from file");
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
  SetupSignalHandler();

  IOServicePtr io_service(new boost::asio::io_service());

  unsigned short port = options["port"].as<unsigned short>();

  EchoServerPtr echo_server(new EchoServer(io_service));
  if (!echo_server->Start("0.0.0.0", port)) {
    return -1;
  }
  boost::thread_group asio_threads;
  for (int i = 0; i < 64; ++i) {
    asio_threads.create_thread(boost::bind(AsioThreadMain, io_service));
  }

  while (!g_shutdown_server) {
    sleep(1);
  }
  echo_server->Stop(); // �رռ�����
  echo_server.reset();   // �ͷ����ü���, ��echo_server����.
  asio_threads.join_all(); // �ȴ�asio��Ȼ�˳�
  std::cout << "Stopped.. .." << std::endl;
  return 0;
}
