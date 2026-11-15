// A separate process is intentional: an unprotected send must fail this test
// through the default SIGPIPE disposition, not kill a QtTest runner or hide
// the problem behind a process-wide SIG_IGN handler.
#include "snap_msgsock.h"

#include <cerrno>
#include <csignal>
#include <cstdio>

namespace {
class Descriptor {
public:
    explicit Descriptor(int value = -1) : fd(value) {}
    ~Descriptor() { if (fd >= 0) ::close(fd); }
    int get() const { return fd; }
    int release() { const int value = fd; fd = -1; return value; }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
private:
    int fd;
};

int failed(const char *operation)
{
    std::fprintf(stderr, "%s failed (errno=%d)\n", operation, errno);
    return 1;
}
} // namespace

int main()
{
    if (std::signal(SIGPIPE, SIG_DFL) == SIG_ERR)
        return failed("restore default SIGPIPE handler");
    Descriptor listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0)
        return failed("socket listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
#ifdef __APPLE__
    address.sin_len = sizeof(address);
#endif
    if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        return failed("bind loopback ephemeral port");
    if (::listen(listener.get(), 1) != 0)
        return failed("listen");
    socklen_t length = sizeof(address);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address), &length) != 0)
        return failed("getsockname");
    Descriptor peer(::socket(AF_INET, SOCK_STREAM, 0));
    if (peer.get() < 0)
        return failed("socket peer");
    if (::connect(peer.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        return failed("connect loopback peer");
    Descriptor accepted(::accept(listener.get(), nullptr, nullptr));
    if (accepted.get() < 0)
        return failed("accept");

    TMsgSocket socket;
    const int nativeSocket = accepted.get();
    socket.SetSocket(accepted.release()); // the native wrapper now owns this descriptor
    if (socket.LastTcpError != 0)
        return failed("Snap7 SetSocket");
#ifdef SO_NOSIGPIPE
    int noSignal = 0;
    socklen_t optionSize = sizeof(noSignal);
    if (::getsockopt(nativeSocket, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, &optionSize) != 0)
        return failed("get SO_NOSIGPIPE");
    if (noSignal != 1) {
        std::fprintf(stderr, "Snap7 accepted socket did not enable SO_NOSIGPIPE\n");
        return 1;
    }
#endif
    if (::shutdown(nativeSocket, SHUT_WR) != 0)
        return failed("shutdown write side");
    // Skip select() so this deterministically exercises the native send(EPIPE)
    // path instead of possibly returning an earlier readiness timeout.
    socket.SendTimeout = 0;
    char payload = 'x';
    const int result = socket.SendPacket(&payload, 1);
    if (result == 0) {
        std::fprintf(stderr, "A send after SHUT_WR unexpectedly succeeded\n");
        return 1;
    }
    std::printf("Snap7 EPIPE returned an error without SIGPIPE (result=%d, socketError=%d)\n",
                result, socket.LastTcpError);
    return 0;
}
