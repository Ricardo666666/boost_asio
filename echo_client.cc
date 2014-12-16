#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <exception>
#include <sstream>
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
class EchoClient;

typedef boost::shared_ptr<EchoClient> EchoClientPtr;
typedef boost::shared_ptr<boost::asio::io_service> IOServicePtr;
typedef boost::shared_ptr<boost::asio::ip::tcp::socket> SocketPtr;
typedef boost::shared_ptr<boost::asio::ip::tcp::resolver> ResolverPtr;
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
    kResolving = 0, // �첽��������
    kResolveError, // �첽��������ʧ��
    kConnecting, // �첽��������
    kConnected,
    kError,
    kClosed,
  };
  Connection(IOServicePtr io_service, const std::string& host, unsigned short port)
    : status_(kResolving), io_service_(io_service), host_(host), port_(port) {
  }
  ~Connection() {
    // ���������ｫwrite_queue�еĴ�����Ϣ�������Ե��߼�����
    std::cout << __FUNCTION__ << std::endl;
  }
  void Start() {
    std::ostringstream str_port;
    str_port << port_;
    boost::asio::ip::tcp::resolver::query query(boost::asio::ip::tcp::v4(), host_, str_port.str());
    resolver_.reset(new boost::asio::ip::tcp::resolver(*io_service_));
    resolver_->async_resolve(query, boost::bind(&Connection::ResolveHandler, shared_from_this(), _1, _2));
  }
  void Close() { // �ظ��ĵ���socket��closeû������, �����ܲ�������close(����Close�ӿڱ�¶���û�,�������������).
    ConnStatus cur_status = status_.exchange(kClosed);
    if (cur_status != kClosed) { // �����ظ�����socket��close��û�������, ��������Ҳ��֤Closeֻ�ܱ�����һ��.
      if (cur_status != kResolving && cur_status != kResolveError) { // ���˽�������״̬��, ����״̬��socket���Ѿ�open��.
        boost::lock_guard<boost::mutex> guard(socket_mutex_);
        boost::system::error_code errcode;
        assert(socket_->close(errcode) == boost::system::errc::success);
      }
    }
  }
  void EchoMsg(StringPtr msg) {
    boost::lock_guard<boost::mutex> guard(socket_mutex_);
    write_queue_.push_back(msg);
    if (write_queue_.size() == 1 && status_.load() == kConnected) {
      async_write(*socket_, boost::asio::buffer(*msg), boost::bind(&Connection::WriteHandler, shared_from_this(), _1, _2));
    }
  }
  ConnStatus status() { return status_.load(); }
private:
  void ResolveHandler(const boost::system::error_code& error, boost::asio::ip::tcp::resolver::iterator iterator) {
    // Resolved
    std::cout << __FUNCTION__ << std::endl;
    if (!error) { // �����ɹ�, ѡ��һ��IP����async_connect.
      boost::asio::ip::tcp::resolver::iterator end;
      if (iterator != end) {
        const boost::asio::ip::tcp::endpoint& endpoint = *iterator;
        socket_.reset(new boost::asio::ip::tcp::socket(*io_service_));
        boost::lock_guard<boost::mutex> guard(socket_mutex_);
        ConnStatus expected = kResolving;
        if (!status_.compare_exchange_strong(expected, kConnecting)) {
          std::cout << "ResolveHandler, Status Is Not Resolving(always kClosed) While Resolved." << std::endl;
          return;
        }
        socket_->async_connect(endpoint, boost::bind(&Connection::ConnectHandler, shared_from_this(), _1));
        return;
      }
    } else if (error == boost::asio::error::operation_aborted) { // ��Ȼ�����ﲻ��cancel resolver, ������ʵ������߼��Ա������ϲ�Գ�ʱ��������cancel.
      std::cout << "Connection ResolveHandler Canceled." << std::endl;
      return;
    }
    // û�н�����IP���߽��������˴���, ��������Ϊ����״̬.
    ConnStatus expected = kResolving;
    if (status_.compare_exchange_strong(expected, kResolveError)) {
      std::cout << "ResolveHandler Error." << std::endl;
    }
  }
  void ConnectHandler(const boost::system::error_code& error) {
    if (!error) { // ���ӳɹ�, �����ȡ
      boost::lock_guard<boost::mutex> guard(socket_mutex_);
      ConnStatus expected = kConnecting;
      if (!status_.compare_exchange_strong(expected, kConnected)) {
        std::cout << "ConnectHandler, Status Is Not Connecting(always kClosed) While Connected." << std::endl;
        return;
      }
      socket_->async_receive(boost::asio::buffer(msgbuf_, sizeof(msgbuf_)), boost::bind(&Connection::ReadHandler, shared_from_this(), _1, _2));
      if (write_queue_.size()) {
        StringPtr next_msg = write_queue_.front();
        // async_write��֤����ȫ��д��ص�.
        async_write(*socket_, boost::asio::buffer(*next_msg), boost::bind(&Connection::WriteHandler, shared_from_this(), _1, _2));
      }
    } else if (error == boost::asio::error::operation_aborted) {
      std::cout << "Connection ConnectHandler Canceled." << std::endl;
    } else {
      ConnStatus expected = kConnecting;
      if (status_.compare_exchange_strong(expected, kError)) {
        std::cout << "ConnectHandler Error." << std::endl;
      }
    }
  }
  void ReadHandler(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (!error) { // û�з�������(������ȡ��), ��ô������һ�ζ�ȡ.
      // �ú�������һЩ���ݾͻ᷵��, ���������������echo�߼�. ���ϣ����ȡָ���������ǰ������, ʹ��async_read.
      {
        boost::lock_guard<boost::mutex> guard(socket_mutex_);
        socket_->async_receive(boost::asio::buffer(msgbuf_, sizeof(msgbuf_)), boost::bind(&Connection::ReadHandler, shared_from_this(), _1, _2));
      }
      // printf("%.*s", (int)bytes_transferred, msgbuf_);
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
  std::deque<StringPtr> write_queue_;
  boost::mutex socket_mutex_;
  boost::atomic<ConnStatus> status_;
  char msgbuf_[1024 * 16];
  SocketPtr socket_;
  IOServicePtr io_service_;
  ResolverPtr resolver_;
  std::string host_;
  unsigned short port_;
};

class EchoClient : public boost::enable_shared_from_this<EchoClient> {
public:
  EchoClient(IOServicePtr io_service, const std::string& host, unsigned short port, uint32_t concurrent)
    : host_(host), port_(port), concurrent_(concurrent), stopped_(false), io_service_(io_service) {
  }
  ~EchoClient() {
    // ��Stop�����߳��ͷ����ü���, �ȴ�io_service������ʣ���¼�������, ����Close������Socket���ͷ����ü���.
    std::cout << __FUNCTION__ << std::endl;
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    for (ConnSetIter iter = conn_set_.begin(); iter != conn_set_.end(); ++iter) {
      (*iter)->Close();
    }
  }
  bool Start() {
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    for (uint32_t i = 0; i < concurrent_; ++i) {
      ConnPtr cli_conn = AddNewConnection(host_, port_);
      TimerPtr socket_timer(new boost::asio::deadline_timer(*io_service_));
      socket_timer->expires_from_now(boost::posix_time::seconds(1));
      socket_timer->async_wait(boost::bind(&EchoClient::CheckSocketStatus, shared_from_this(), cli_conn, socket_timer, _1));
    }
    return true;
  }
  void Stop() {
    stopped_.store(true);
  }
private:
  ConnPtr AddNewConnection(const std::string& host, unsigned short port) {
    ConnPtr new_conn(new Connection(io_service_, host, port));
    new_conn->Start();
    new_conn->EchoMsg(StringPtr(new std::string("Hello Asio.\n")));
    conn_set_.insert(new_conn);
    return new_conn;
  }
  void CheckSocketStatus(ConnPtr conn, TimerPtr socket_timer, const boost::system::error_code& error) {
    // 1, EchoClient�Ѿ���Stop����, ��ô����ֹͣtimer�ͷŵ���EchoClient�����ü���, ��EchoClient������������
    // 2, �ж�conn->status()==kError/kResolveError��Close���Ӳ���ConnSet���Ƴ�, ���´���������.
    // 3, �ж�conn->status()==kClosed���ConnSet���Ƴ�.(�����û����Ի�ȡSocketPtr����ʱ����Close)
    // 4, ��������, ����������һ��timer.
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    ConnSetIter iter = conn_set_.find(conn);
    assert(iter != conn_set_.end());
    if (stopped_.load()) {
      // case 1
      //std::cout << "case 1" << std::endl;
      return;
    } else if (conn->status() == Connection::kError || conn->status() == Connection::kResolveError) { // case 2
      //std::cout << "case 2" << std::endl;
      conn->Close();
      conn_set_.erase(conn); // TODO:
      conn = AddNewConnection(host_, port_);
    } else if (conn->status() == Connection::kClosed) {// case 3
      //std::cout << "case 3" << std::endl;
      conn_set_.erase(conn); // TODO:
      conn = AddNewConnection(host_, port_);
    }
    //std::cout << "case 4" << std::endl; // case 4
    socket_timer->expires_from_now(boost::posix_time::seconds(1));
    socket_timer->async_wait(boost::bind(&EchoClient::CheckSocketStatus, shared_from_this(), conn, socket_timer, _1));
  }
  typedef std::set<ConnPtr> ConnSet;
  typedef ConnSet::iterator ConnSetIter;
  std::string host_;
  unsigned short port_;
  uint32_t concurrent_;
  boost::atomic<bool> stopped_;
  boost::mutex conn_set_mutex_;
  ConnSet conn_set_;
  IOServicePtr io_service_;
};

volatile sig_atomic_t g_shutdown_client = 0;
void ShutdownClientHandler(int signo) {
  g_shutdown_client = 1;
}
void SetupSignalHandler() {
  sigset_t sigset;
  sigfillset(&sigset);
  sigdelset(&sigset, SIGTERM);
  sigdelset(&sigset, SIGINT);
  sigprocmask(SIG_SETMASK, &sigset, NULL);

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = ShutdownClientHandler;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
}
void AsioThreadMain(IOServicePtr io_service) {
  // ���̵߳������io_service��leader-followerģ��
  io_service->run();
}
bool ParseCommands(int argc, char** argv, boost::program_options::variables_map* options) {
  boost::program_options::options_description desc("Usage");
  desc.add_options()
      ("help", "show how to use this program")
      ("thread,t", boost::program_options::value<uint32_t>()->default_value(12), "number of threads of asio")
      ("host,h", boost::program_options::value<std::string>()->required(), "the tcp host client connects to")
      ("port,p", boost::program_options::value<unsigned short>()->required(), "the tcp port client connects to")
      ("concurrent,n", boost::program_options::value<uint32_t>()->default_value(1), "the number of connections to server")
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

  std::string host = options["host"].as<std::string>();
  unsigned short port = options["port"].as<unsigned short>();
  uint32_t concurrent = options["concurrent"].as<uint32_t>();

  EchoClientPtr echo_client(new EchoClient(io_service, host, port, concurrent));
  if (!echo_client->Start()) {
    return -1;
  }
  uint32_t thread_num = options["thread"].as<uint32_t>();
  boost::thread_group asio_threads;
  for (uint32_t i = 0; i < thread_num; ++i) {
    asio_threads.create_thread(boost::bind(AsioThreadMain, io_service));
  }

  while (!g_shutdown_client) {
    sleep(1);
  }
  echo_client->Stop(); // �رտͻ���
  echo_client.reset();   // �ͷ����ü���, ��echo_client����.
  asio_threads.join_all(); // �ȴ�asio��Ȼ�˳�
  std::cout << "Stopped.. .." << std::endl;
  return 0;
}
