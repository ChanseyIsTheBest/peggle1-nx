/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

// peggle2-nx (2026-10-13): sockets without a host socket. Peggle 2 starts RakNet while it boots
// (RakPeer::Startup from the constructor of its network manager, sub_822CAF38): socket, setsockopt, bind, then two
// threads, one blocked in recvfrom. On the Switch nothing initialises libnx's socket service (only log_network
// does), so socket() failed, Startup returned an error and the game took a path the Xbox 360 never takes: it
// destroyed the half-built manager and went on using it. One second later the main thread read freed memory (guest
// 0x00750064: the text of XAudio2's device name in the manager's old block). That was the "heap crash" of every
// console boot since 2026-10-05. With real sockets it would still fail there: libnx's sockaddr_in has BSD's
// sin_len / sin_family bytes, and bind is handed Winsock's 16-bit family.
// An offline socket gives the game what a console with a network cable and no traffic gives it: every call
// succeeds, nothing ever arrives from outside, a blocking recvfrom blocks. A datagram sent to a port another
// offline socket of this process is bound to is delivered to it (RakNet wakes its own receive thread that way).
#if defined(__SWITCH__)
#define REX_NET_OFFLINE_SOCKETS_DEFAULT true
#else
#define REX_NET_OFFLINE_SOCKETS_DEFAULT false
#endif
REXCVAR_DEFINE_BOOL(net_offline_sockets, REX_NET_OFFLINE_SOCKETS_DEFAULT, "Net",
                    "Guest sockets have no host socket behind them: every call succeeds, nothing arrives from "
                    "outside, datagrams between the process's own sockets are delivered (the default on the Switch, "
                    "where the socket service is not started; false = host sockets)");
// The console's failure on any machine: with this the PC reproduces what the Switch did before net_offline_sockets.
REXCVAR_DEFINE_BOOL(net_diag_socket_fail, false, "Net",
                    "Test only: every guest socket() fails, as on a Switch whose socket service is not started "
                    "(Peggle 2 then crashes about a second after its network manager's start-up fails)");

namespace rex::system {

namespace {
// Winsock error codes the offline sockets report.
constexpr uint32_t kWsaEIntr = 10004, kWsaEWouldBlock = 10035, kWsaEOpNotSupp = 10045, kWsaENetUnreach = 10051;
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;

// Every open offline socket, for the delivery between them.
std::mutex g_offline_mutex;
std::vector<XSocket*> g_offline_sockets;
std::atomic<uint32_t> g_offline_logged{0};

// The first few events of the offline sockets go to the log: what the game asked of the network while it started.
template <typename... Args>
void OfflineLog(fmt::format_string<Args...> text, Args&&... args) {
  if (g_offline_logged.fetch_add(1, std::memory_order_relaxed) < 24) {
    REXSYS_INFO("[net] offline socket: {}", fmt::format(text, std::forward<Args>(args)...));
  }
}
}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  while (!incoming_packets_.empty()) {
    delete[] incoming_packets_.front();
    incoming_packets_.pop();
  }
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  if (REXCVAR_GET(net_diag_socket_fail)) [[unlikely]] {
    REXSYS_WARN("[net] net_diag_socket_fail: socket({}, {}, {}) fails", uint32_t(af), uint32_t(type), uint32_t(proto));
    return X_STATUS_UNSUCCESSFUL;
  }
  if (REXCVAR_GET(net_offline_sockets)) {
    offline_ = true;
    {
      std::lock_guard<std::mutex> lock(g_offline_mutex);
      g_offline_sockets.push_back(this);
    }
    OfflineLog("created (family {}, type {}, protocol {})", uint32_t(af), uint32_t(type), uint32_t(proto_));
    return X_STATUS_SUCCESS;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    REXSYS_WARN("[net] host socket({}, {}, {}) failed (errno {}); net_offline_sockets = true gives the game "
                "sockets without a network",
                uint32_t(af), uint32_t(type), uint32_t(proto), errno);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (offline_) {
    {
      std::lock_guard<std::mutex> lock(g_offline_mutex);
      std::erase(g_offline_sockets, this);
    }
    {
      std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
      closed_ = true;
    }
    incoming_cv_.notify_all();  // a thread blocked in RecvFrom returns
    return X_STATUS_SUCCESS;
  }

  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  if (offline_) {
    if (level == 0xFFFF && optname == 0x0020) broadcast_socket_ = true;  // SO_BROADCAST
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  if (offline_) {
    if (cmd == kFionbio && arg_ptr) {
      std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
      nonblocking_ = memory::load_and_swap<uint32_t>(arg_ptr) != 0;
      OfflineLog("{} mode", nonblocking_ ? "non-blocking" : "blocking");
    } else if (cmd == kFionread && arg_ptr) {
      std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
      const uint32_t waiting =
          incoming_packets_.empty() ? 0 : reinterpret_cast<packet*>(incoming_packets_.front())->data_len;
      memory::store_and_swap<uint32_t>(arg_ptr, waiting);
    }
    return X_STATUS_SUCCESS;
  }

  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  if (offline_) {
    last_error_ = kWsaENetUnreach;  // nobody to connect to
    return X_STATUS_UNSUCCESSFUL;
  }

  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (offline_) {
    std::lock_guard<std::mutex> lock(g_offline_mutex);  // SendTo reads the port under it
    bound_ = true;
    bound_port_ = name->sin_port;
    OfflineLog("bound to port {}", bound_port_);
    return X_STATUS_SUCCESS;
  }

  int ret = bind(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  if (offline_) {
    return X_STATUS_SUCCESS;  // nobody will connect
  }

  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  if (offline_) {
    last_error_ = kWsaEWouldBlock;  // no connection is ever pending
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  if (offline_) {
    return 0;
  }
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (offline_) {
    return RecvFrom(buf, buf_len, flags, nullptr, nullptr);
  }
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  if (offline_) {
    // A datagram one of the process's own sockets sent here, else what a socket with no traffic does: a
    // non-blocking one reports WSAEWOULDBLOCK, a blocking one waits (until the socket is closed).
    std::unique_lock<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.empty() && !closed_) {
      if (nonblocking_) {
        last_error_ = kWsaEWouldBlock;
        return -1;
      }
      OfflineLog("a blocking recvfrom waits (nothing arrives from outside)");
      incoming_cv_.wait(lock, [this] { return !incoming_packets_.empty() || closed_; });
    }
    if (incoming_packets_.empty()) {
      last_error_ = kWsaEIntr;  // closed while it waited
      return -1;
    }
    uint8_t* raw = incoming_packets_.front();
    incoming_packets_.pop();
    const packet* pkt = reinterpret_cast<const packet*>(raw);
    const uint32_t data_len = pkt->data_len;
    std::memcpy(buf, pkt->data, std::min(data_len, buf_len));
    if (from) {
      from->sin_family = 2;  // AF_INET
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
    }
    if (from_len) {
      *from_len = sizeof(XSOCKADDR_IN);
    }
    delete[] raw;
    return int(std::min(data_len, buf_len));
  }

  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = nfrom.sin_port;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (offline_) {
    return int(buf_len);  // never connected: sent to nobody
  }
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  if (offline_) {
    // Sent, as far as the game can tell. Delivered when one of the process's own sockets is bound to the port
    // (whatever the address: there is no other machine); lost otherwise, as on a network with nobody listening.
    bool delivered = false;
    uint16_t port = 0;
    if (to) {
      port = to->sin_port;
      std::lock_guard<std::mutex> lock(g_offline_mutex);
      for (XSocket* target : g_offline_sockets) {
        if (target->bound_ && target->bound_port_ == port && target->type_ == type_) {
          delivered = target->QueuePacket(0x7F000001u, bound_port_, buf, buf_len);  // from 127.0.0.1
          break;
        }
      }
    }
    OfflineLog("sendto of {} bytes to port {}: {}", buf_len, port,
               delivered ? "delivered to this process's own socket" : "nobody listens there");
    return int(buf_len);
  }

  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  sockaddr_in nto;
  if (to) {
    nto.sin_addr.s_addr = to->sin_addr;
    nto.sin_family = to->sin_family;
    nto.sin_port = to->sin_port;
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to_len);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    incoming_packets_.push((uint8_t*)pkt);
  }
  incoming_cv_.notify_one();  // an offline socket's blocked RecvFrom

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
