#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <cstdio>
#include <string>
#include <memory>

/* True iff e is an error that means a read/write operation can be retried. */
#define SOCKET_ERR_RW_RETRIABLE(e) \
        ((e) == EINTR || (e) == EAGAIN)
/* True iff e is an error that means an connect can be retried. */
#define SOCKET_ERR_CONNECT_RETRIABLE(e) \
        ((e) == EINTR || (e) == EINPROGRESS)
/* True iff e is an error that means a accept can be retried. */
#define SOCKET_ERR_ACCEPT_RETRIABLE(e) \
        ((e) == EINTR || (e) == EAGAIN || (e) == ECONNABORTED)
/* True iff e is an error that means the connection was refused */
#define SOCKET_ERR_CONNECT_REFUSED(e) \
        ((e) == ECONNREFUSED)

class SocketTcpClient
{
public:
  bool init(const std::string& server_ip, uint16_t server_port, const std::string& interface_name,
            uint32_t recv_buf_size = 4096) {
    interface_name_ = interface_name;
    server_ip_ = server_ip;
    server_port_ = server_port;
    RecvBufSize = recv_buf_size;
    recvbuf_.reset(new char[RecvBufSize]);
    return true;
  }

  bool init(const std::string& server_ip, uint16_t server_port, uint16_t local_port, const std::string& interface_name = "",
            uint32_t recv_buf_size = 4096) {
    interface_name_ = interface_name;
    server_ip_ = server_ip;
    server_port_ = server_port;
    local_port_ =local_port;
    RecvBufSize = recv_buf_size;
    recvbuf_.reset(new char[RecvBufSize]);
    return true;
  }

  bool init(int fd, const std::string& client_ip, uint16_t client_port, uint32_t recv_buf_size = 4096) {
    RecvBufSize = recv_buf_size;
    server_ip_ = client_ip;
    server_port_ = client_port;
    recvbuf_.reset(new char[RecvBufSize]);

    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) {
      saveError("setsockopt TCP_NODELAY error");
      ::close(fd);
      return false;
    }

    head_ = tail_ = 0;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    fd_ = fd;
    return true;
  }

  ~SocketTcpClient() { close("destruct"); }

  const std::string& getLastError() { return last_error_; };

  void setDomain(const char* data) {domain_ = data;}
  const std::string& getServer() { return server_ip_; }
  uint16_t getServerPort() { return server_port_; }
  
  std::string getLocalIP() {
      struct sockaddr_in local_addr{};
      socklen_t addr_len = sizeof(local_addr);

      if (getsockname(fd_, (struct sockaddr*)&local_addr, &addr_len) == -1) {
          perror("getsockname");
          return "";
      }

      return inet_ntoa(local_addr.sin_addr);
}

  bool isConnected() { return fd_ >= 0; }

  bool connect() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      saveError("socket error");
      return false;
    }
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) {
      saveError("setsockopt TCP_NODELAY error");
      ::close(fd);
      return false;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
      saveError("setsockopt SO_REUSEADDR error");
      ::close(fd);
      return false;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) < 0) {
      saveError("setsockopt SO_KEEPALIVE error");
      ::close(fd);
      return false;
    }

    int idle = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0) {
      saveError("setsockopt TCP_KEEPIDLE error");
      ::close(fd);
      return false;
    }

    int interval = 2;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0) {
      saveError("setsockopt TCP_KEEPINTVL error");
      ::close(fd);
      return false;
    }

    int retry = 2;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &retry, sizeof(retry)) < 0) {
      saveError("setsockopt TCP_KEEPCNT error");
      ::close(fd);
      return false;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(0);
    if (interface_name_.size() > 0 && interface_name_[0] >= '0' && interface_name_[0] <= '9')
    {
        inet_pton(AF_INET, interface_name_.data(), &(local_addr.sin_addr));
    }
    if (local_port_ > 0) {
        local_addr.sin_port = htons(local_port_);
    }
    bind(fd, (struct sockaddr*)&local_addr, sizeof(local_addr));
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip_.data(), &(server_addr.sin_addr));
    server_addr.sin_port = htons(server_port_);
    bzero(&(server_addr.sin_zero), 8);
    int rc = ::connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (rc != 0) {
      int serrno = errno;
      if (SOCKET_ERR_CONNECT_RETRIABLE(serrno)) {
        //wait for writable
        fd_set wset={0};
        struct timeval tval={0};

        FD_SET(fd, &wset);
        tval.tv_sec = idle;

        if(select(fd + 1, NULL, &wset, NULL, &tval) > 0 && FD_ISSET(fd, &wset)){
          serrno = 0;
          socklen_t len = sizeof(len);
          getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&serrno, (socklen_t*)&len);
          if(0 == serrno)
            rc = 0;
        }
      }
    }
    if (rc != 0) {
      saveError("connect error");
      ::close(fd);
      return false;
    }
    head_ = tail_ = 0;
    fd_ = fd;
    return true;
  }

  void close(const char* reason) {
    if (fd_ >= 0) {
      saveError(reason);
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool write(const char* data, uint32_t size, bool more = false, int id = 0) {
    int retry_times = 0;
    int loop_times = 0;
    int flags = MSG_NOSIGNAL;
    if (more) flags |= MSG_MORE;
    do {
      int sent = ::send(fd_, data, size, flags);
      if (sent < 0) {
        int serrno = errno;
        if (serrno != EAGAIN) {
          saveError("send error");
          fprintf(stderr, "[SocketTcpClient fd=%d] send failed: %s (errno=%d), size=%u\n",
                  fd_, last_error_.c_str(), serrno, size);
          close("send error");
          return false;
        }
        fprintf(stderr, "[SocketTcpClient fd=%d] send EAGAIN, SNDBUF full, spinning (size=%u) times=%d id=%d\n",
                fd_, size, retry_times, id);
        retry_times++;
        continue;
      }
      retry_times = 0;
      data += sent;
      size -= sent;

      if (loop_times > 0) {
        fprintf(stderr, "[SocketTcpClient fd=%d] loop------ send: id=%d times=%d size=%d sent=%d\n",
                fd_, id, loop_times, size, sent);
      }
      loop_times++;
    } while (size != 0);
    return true;
  }

  // return true if read something
  template<typename Handler>
  bool read(Handler handler) {
    int ret = ::read(fd_, recvbuf_.get() + tail_, RecvBufSize - tail_);
    if (ret <= 0) {
      if (ret < 0 && errno == EAGAIN) return false;
      if (ret < 0) {
        close("read error");
      }
      else {
        close("remote close");
      }
      return false;
    }
    tail_ += ret;

    uint32_t remaining = handler(recvbuf_.get() + head_, tail_ - head_);
    if (remaining == 0) {
      head_ = tail_ = 0;
    }
    else {
      head_ = tail_ - remaining;
      if (head_ >= RecvBufSize / 2) {
        memcpy(recvbuf_.get(), recvbuf_.get() + head_, remaining);
        head_ = 0;
        tail_ = remaining;
      }
      else if (tail_ == RecvBufSize) {
        close("recv buf full");
      }
    }
    return true;
  }

  const std::string& LoginName() const { return login_name_; }
  void SetLoginName(const std::string& name) { login_name_ = name; }

  void SetContext(void* c) { context_ = c; }
  void* Context() { return context_; }

private:
  void saveError(const char* msg) {
    last_error_ = msg;
    last_error_ += " errno: " + std::to_string(errno);
  }

  int fd_ = -1;
  uint32_t RecvBufSize;
  uint32_t head_;
  uint32_t tail_;
  std::unique_ptr<char[]> recvbuf_;

  std::string server_ip_;
  uint16_t server_port_;
  uint16_t local_port_;
  std::string domain_;
  std::string last_error_;
  std::string interface_name_;
  std::string login_name_;
  void* context_ = nullptr;
};

