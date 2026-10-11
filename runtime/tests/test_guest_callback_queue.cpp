// GuestCallQueue: FIFO order, timeouts, stop, and pushes from the consumer.
#include "net/guest_callbacks.h"

#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static GuestCall call(uint32_t fn, uint32_t a0) {
    GuestCall c;
    c.fn = fn;
    c.a[0] = a0;
    return c;
}

static void five_args_and_buffer() {
    // A matching event: handler(id, event, mac*, optlen, opt*) with MAC at
    // buf+0 and opt data at buf+8 (PPSSPP notifyMatchingHandler layout).
    GuestCall c;
    c.fn = 0x08900000;
    c.a[0] = 1;
    c.a[1] = 7;
    c.a[2] = 0;
    c.a[3] = 2;
    c.a[4] = 8;
    c.buf = {0, 0, 0, 0, 0, 0x0b, 0, 0, 0xAA, 0xBB};
    c.buf_args = (1 << 2) | (1 << 4);
    GuestCallQueue q;
    q.push(c);
    GuestCall out;
    CHECK(q.pop(&out, 10) && out.buf == c.buf && out.a[3] == 2, "buffer and args survive the queue");
    uint32_t args[5];
    guest_call_resolve(out, 0x09F00000u, args);
    CHECK(args[0] == 1 && args[1] == 7 && args[2] == 0x09F00000u && args[3] == 2 && args[4] == 0x09F00008u,
          "buffer address added to the marked args only");
}

static void fifo_order() {
    GuestCallQueue q;
    q.push(call(1, 10));
    q.push(call(2, 20));
    GuestCall c{};
    CHECK(q.pop(&c, 10) && c.fn == 1 && c.a[0] == 10, "first in, first out");
    CHECK(q.pop(&c, 10) && c.fn == 2, "second");
    CHECK(!q.pop(&c, 20), "empty queue times out");
}

static void reentrant_call() {
    // The consumer pushes while handling a call (a handler that raises a new
    // event); the producer keeps pushing at the same time.
    GuestCallQueue q;
    int handled = 0;
    std::thread consumer([&] {
        GuestCall c{};
        while (q.pop(&c, 500)) {
            handled++;
            if (c.a[0] == 1) q.push(call(c.fn, 0));
        }
    });
    for (int i = 0; i < 100; i++) q.push(call(0x08800000u + i, static_cast<uint32_t>(i & 1)));
    consumer.join();
    CHECK(handled == 150, "all calls and re-entrant pushes handled");
}

static void stop_wakes_consumer() {
    GuestCallQueue q;
    bool returned = false;
    std::thread t([&] {
        GuestCall c{};
        returned = !q.pop(&c, 10000);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.stop();
    t.join();
    CHECK(returned, "stop releases a waiting pop");
    q.push(call(1, 0));
    GuestCall c{};
    CHECK(!q.pop(&c, 10), "no pops after stop");
}

int main() {
    fifo_order();
    five_args_and_buffer();
    reentrant_call();
    stop_wakes_consumer();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("guest_callback_queue: all passed\n");
    return 0;
}
