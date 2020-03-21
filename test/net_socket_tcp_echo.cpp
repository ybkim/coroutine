/**
 * @author github.com/luncliff (luncliff@gmail.com)
 * @copyright CC BY 4.0
 */
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <thread>

#include <coroutine/net.h>
#include <coroutine/return.h>

#include <socket.hpp>
#if defined(__APPLE__)
#include <latch_darwin.h>
#else
#include <latch.h>
#endif

using namespace std;
using namespace coro;

using io_buffer_reserved_t = array<std::byte, 3900>;
using on_accept_handler = auto (*)(int64_t) -> nullptr_t;

bool is_async_pending(uint32_t ec) noexcept {
    switch (ec) {
#if defined(_WIN32)
    case WSAEWOULDBLOCK:
    case EINPROGRESS:
    case ERROR_IO_PENDING:
    case EWOULDBLOCK:
#else
    case EAGAIN:
    case EINPROGRESS:
#endif
        return true;
    default:
        return false;
    }
}

//  Accept socket connects and invoke designated function
auto accept_until_error(int64_t ln, on_accept_handler service) {
    while (true) {
        // next accept is non-blocking
        socket_set_option_nonblock(ln);
        int64_t cs{};
        socket_accept(ln, cs);
        if (cs < 0) // accept failed
            // auto ec = recent_net_error();
            // system_category().message(ec)
            return;

        socket_set_option_nonblock(cs); // set some options
        socket_set_option_nodelay(cs);
        socket_set_option_recv_timout(cs, 1000);
        socket_set_option_send_timout(cs, 1000);
        // attach(spawn) a service coroutine
        service(cs);
    }
}

// Receive some bytes from the socket and echo back
// continue until EOF
auto tcp_echo_service(int64_t sd) -> nullptr_t {
    auto on_return = gsl::finally([=]() { socket_close(sd); });

    io_work_t work{};
    int64_t rsz = 0, ssz = 0;       // received/sent data size
    io_buffer_t buf{};              // memory view to the `storage`
    io_buffer_reserved_t storage{}; // each coroutine frame contains buffer

RecvData:
    rsz = co_await recv_stream(sd, buf = storage, 0, work);
    if (rsz == 0) // EOF reached
        co_return;
    buf = {storage.data(), rsz}; // buf := [base, rsz)

SendData:
    ssz = co_await send_stream(sd, buf, 0, work);
    if (ssz == 0) // EOF reached
        co_return;
    if (ssz == rsz) // send complete
        goto RecvData;

    // send < recv : need to send more
    rsz -= ssz;
    buf = {storage.data() + ssz, rsz}; // buf := [base + ssz, rsz)
    goto SendData;
}

auto tcp_recv_stream(int64_t sd, io_work_t& work, //
                     int64_t& rsz, latch& wg) -> nullptr_t {

    auto on_return = gsl::finally([&wg]() {
        try {
            wg.count_down();
        } catch (const std::system_error& e) {
            fputs(e.what(), stderr);
        }
    });

    io_buffer_reserved_t storage{}; // each coroutine frame contains buffer

    rsz = co_await recv_stream(sd, storage, 0, work);
    if (auto ec = work.error()) {
        const auto emsg = system_category().message(ec);
        fputs(emsg.c_str(), stderr);
        exit(__LINE__);
    }
    assert(rsz > 0);
}

auto tcp_send_stream(int64_t sd, io_work_t& work, //
                     int64_t& ssz, latch& wg) -> nullptr_t {

    auto on_return = gsl::finally([&wg]() {
        try {
            wg.count_down();
        } catch (const std::system_error& e) {
            fputs(e.what(), stderr);
        }
    });

    io_buffer_reserved_t storage{}; // each coroutine frame contains buffer

    ssz = co_await send_stream(sd, storage, 0, work);
    if (auto ec = work.error()) {
        const auto emsg = system_category().message(ec);
        fputs(emsg.c_str(), stderr);
        exit(__LINE__);
    }
    assert(ssz > 0);
}

int main(int, char*[]) {
    socket_setup();
    auto on_return = gsl::finally([]() { socket_teardown(); });

    // going to handle 4 connections concurrently
    static constexpr auto max_socket_count = 4U;
    static constexpr auto io_coroutine_count = max_socket_count * 2;

    // using IPv4 since CI(or Docker) env doesn't support IPv6
    addrinfo hint{};
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_protocol = IPPROTO_TCP;

    // listener socket
    int64_t ln{};
    if (auto ec = socket_create(hint, ln)) {
        const auto emsg = system_category().message(ec);
        fputs(emsg.c_str(), stderr);
        exit(__LINE__);
    }
    auto on_return2 = gsl::finally([ln]() {
        socket_close(ln);
        // this sleep is for waiting windows completion routines
        std::this_thread::sleep_for(1s);
    });

    sockaddr_in local{}; // local: listening address
    local.sin_family = hint.ai_family;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(3345);
    socket_bind(ln, local);

    socket_set_option(ln, SOL_SOCKET, SO_REUSEADDR, true);
    socket_set_option_nonblock(ln);
    socket_listen(ln);

    array<int64_t, max_socket_count> conns{};
    array<int64_t, max_socket_count> rsz{}, ssz{}; // received/sent

    for (auto& sd : conns) {
        if (auto ec = socket_create(hint, sd)) {
            const auto emsg = system_category().message(ec);
            fputs(emsg.c_str(), stderr);
            exit(__LINE__);
        }
        socket_set_option_nonblock(sd); // non-blocking connect
        socket_set_option_send_timout(sd, 900);
        socket_set_option_recv_timout(sd, 900);

        if (auto ec = socket_connect(sd, local))
            // Allow non-block error
            if (is_async_pending(ec) == false) {
                const auto emsg = system_category().message(ec);
                fputs(emsg.c_str(), stderr);
                exit(__LINE__);
            }
        socket_set_option_nodelay(sd);
    }
    auto on_return3 = gsl::finally([&conns]() {
        for (auto sd : conns)
            socket_close(sd);
    });

    // Accept all in the backlog
    // accepted sockets will be served by the 'tcp_echo_service'
    accept_until_error(ln, tcp_echo_service);

    // We will spawn some coroutines and wait them to return using `latch`.
    // Those coroutines will perform send/recv operation on the socket
    latch wg{2 * max_socket_count};

    // All I/O coroutines will use pre-allocated 'io_work_t'
    // so they can expose their last state which includes I/O error code
    array<io_work_t, io_coroutine_count> works{};

    for (auto i = 0U; i < max_socket_count; ++i) {
        tcp_recv_stream(conns[i], works[2 * i + 0], rsz[i], wg);
        tcp_send_stream(conns[i], works[2 * i + 1], ssz[i], wg);
    }
    // latch will help to sync the fork-join of coroutines
    do {
        // perform APC on Windows,
        // polling in the other platform
        poll_net_tasks(2'000);
    } while (wg.try_wait() == false);

    // This is an echo. so receive/send length must be equal !
    for (auto i = 0U; i < max_socket_count; ++i) {
        assert(ssz[i] != -1);     // no i/o error
        assert(rsz[i] != -1);     //
        assert(ssz[i] == rsz[i]); // sent == received
    }
    return EXIT_SUCCESS;
}
