#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
#include "sidp_transport_queue.hpp"
#include "sidp_transport_websocket.hpp"
#include "sidp_transport_cdc.hpp"
using namespace sidp;

static std::vector<std::uint8_t> frame(std::uint32_t id, msg_kind_t kind = KIND_REQUEST) {
    std::vector<std::uint8_t> data(sizeof(msg_header_t));
    auto *h = reinterpret_cast<msg_header_t *>(data.data());
    h->version = PROTOCOL_VERSION; h->kind = kind; h->opcode = OP_GET_STATE; h->request_id = id;
    assert(crc32_hasher::set_message_crc(data));
    return data;
}
class test_transport : public packet_queue_transport {
public:
    test_transport() { assert(create_queues() == ESP_OK); }
    ~test_transport() { destroy_queues(); }
    using packet_queue_transport::service_tx;
    using packet_queue_transport::deliver_packet;
    using packet_queue_transport::receive_epoch;
    using packet_queue_transport::timeout_to_ticks;
    void connect() { link_changed(true); }
    void disconnect() { link_changed(false); }
    std::vector<std::uint32_t> sent;
    std::function<bool()> send_hook;
private:
    bool physical_link_open() const noexcept override { return true; }
    bool deliver_tx_frame(std::span<const std::uint8_t> data, bool) noexcept override {
        if (send_hook && !send_hook()) return false;
        sent.push_back(reinterpret_cast<const msg_header_t *>(data.data())->request_id);
        return true;
    }
};
static void expect_empty(transport_intf &t) {
    std::uint8_t *data = nullptr; std::size_t size = 0;
    assert(t.start_read(&data, &size, 0) == ESP_ERR_TIMEOUT);
    assert(data == nullptr && size == 0);
}
int main() {
    test_transport t;
    t.connect();
    assert(!t.is_open());
    assert(t.begin_session() == ESP_OK);
    const auto old_epoch = t.receive_epoch();
    auto request = frame(1);
    t.deliver_packet(request.data(), request.size(), old_epoch);
    assert(t.write_message(frame(2)) == ESP_OK);
    t.disconnect(); t.connect(); // Owner must still see the loss after quick reconnect.
    assert(t.needs_disconnect());
    assert(t.begin_session() == ESP_OK);
    expect_empty(t);
    assert(!t.service_tx());
    t.deliver_packet(request.data(), request.size(), old_epoch); // delayed old callback
    expect_empty(t);

    t.deliver_packet(request.data(), request.size(), t.receive_epoch());
    std::uint8_t *borrowed; std::size_t size;
    assert(t.start_read(&borrowed, &size, 0) == ESP_OK);
    t.disconnect(); t.connect();
    assert(t.begin_session() == ESP_ERR_INVALID_STATE);
    t.end_read(borrowed);
    assert(t.begin_session() == ESP_OK);

    // Logs have a separate queue; controls stay FIFO and always dequeue first.
    assert(t.write_message_log(frame(10, KIND_LOG_STREAM)) == ESP_OK);
    assert(t.write_message(frame(11)) == ESP_OK);
    assert(t.write_message(frame(12)) == ESP_OK);
    assert(t.service_tx() && t.service_tx() && t.service_tx());
    assert((t.sent == std::vector<std::uint32_t>{11,12,10}));
    host_fail_next_send = true;
    assert(t.write_message_log(frame(13, KIND_LOG_STREAM)) == ESP_ERR_NO_MEM);
    assert(t.is_open());
    assert(t.write_message(frame(14)) == ESP_OK);
    t.send_hook = [] { return false; };
    assert(t.service_tx());
    assert(t.needs_disconnect());
    assert(t.flush_write(0) == ESP_ERR_INVALID_STATE);
    assert(t.write_message(frame(15)) == ESP_ERR_INVALID_STATE);
    assert(t.begin_session() == ESP_ERR_INVALID_STATE); // same failed connection
    t.disconnect(); t.connect();
    assert(t.begin_session() == ESP_OK);
    host_fail_next_send = true;
    assert(t.write_message(frame(16)) == ESP_ERR_NO_MEM);
    assert(t.needs_disconnect());
    t.disconnect(); t.connect();
    assert(t.begin_session() == ESP_OK);

    // A blocked send cannot leak into a newly accepted session.
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    t.send_hook = [&] {
        std::unique_lock lock(mutex);
        entered = true; cv.notify_one();
        cv.wait(lock, [&] { return release; });
        return false;
    };
    assert(t.write_message(frame(17)) == ESP_OK);
    std::thread sender([&] { assert(t.service_tx()); });
    { std::unique_lock lock(mutex); cv.wait(lock, [&] { return entered; }); }
    t.disconnect(); t.connect();
    assert(t.begin_session() == ESP_ERR_INVALID_STATE);
    { std::lock_guard lock(mutex); release = true; cv.notify_one(); }
    sender.join();
    assert(t.begin_session() == ESP_OK);
    expect_empty(t);
    assert(!t.service_tx());
    host_fail_next_send = true;
    t.deliver_packet(request.data(), request.size(), t.receive_epoch());
    assert(t.needs_disconnect());
    assert(test_transport::timeout_to_ticks(11) == 2);

    // Exercise the actual WebSocket event registration and decoder.
    auto &ws = websocket_transport::instance();
    assert(ws.init({}) == ESP_OK);
    assert(host_websocket.disable_auto_reconnect);
    assert(!host_websocket.enable_close_reconnect);
    host_ws_event(WEBSOCKET_EVENT_CONNECTED); // null event_data is valid
    assert(ws.begin_session() == ESP_OK);
    esp_websocket_event_data_t event;
    event.payload_len = event.data_len = request.size();
    event.data_ptr = reinterpret_cast<char *>(request.data());
    host_ws_event(WEBSOCKET_EVENT_DATA, &event);
    assert(ws.start_read(&borrowed, &size, 0) == ESP_OK);
    ws.end_read(borrowed);
    host_ws_event(WEBSOCKET_EVENT_DATA, &event); // queued old request
    host_ws_event(WEBSOCKET_EVENT_DISCONNECTED);
    host_ws_event(WEBSOCKET_EVENT_CONNECTED);
    assert(ws.needs_disconnect());
    assert(ws.begin_session() == ESP_OK);
    expect_empty(ws);
    // Old fragmented message cannot continue into a new session.
    event.fin = false;
    host_ws_event(WEBSOCKET_EVENT_DATA, &event);
    ws.close_session();
    assert(ws.begin_session() == ESP_ERR_INVALID_STATE);
    host_ws_event(WEBSOCKET_EVENT_DISCONNECTED);
    host_ws_event(WEBSOCKET_EVENT_CONNECTED);
    assert(ws.begin_session() == ESP_OK);
    event.fin = true; event.op_code = WS_TRANSPORT_OPCODES_CONT;
    host_ws_event(WEBSOCKET_EVENT_DATA, &event);
    expect_empty(ws);

    for (auto closed : {WEBSOCKET_EVENT_CLOSED, WEBSOCKET_EVENT_FINISH, WEBSOCKET_EVENT_ERROR}) {
        host_ws_event(closed);
        assert(ws.needs_disconnect());
        host_ws_event(WEBSOCKET_EVENT_DISCONNECTED);
        assert(ws.reconnect() == ESP_OK); // client already stopped
        host_ws_event(WEBSOCKET_EVENT_CONNECTED);
        assert(ws.begin_session() == ESP_OK);
        expect_empty(ws);
    }

    // CDC DTR and physical-device boundaries both require fresh acceptance.
    auto &cdc = cdc_slip_transport::instance();
    assert(cdc.init(TINYUSB_CDC_ACM_0) == ESP_OK);
    tinyusb_event_t attached{TINYUSB_EVENT_ATTACHED};
    cdc_slip_transport::device_event_callback(&attached, nullptr);
    host_cdc_dtr(true);
    assert(cdc.begin_session() == ESP_OK);
    auto feed = [&] {
        host_cdc_input.push_back(cdc_slip_transport::SLIP_END);
        for (auto b : request) {
            if (b == cdc_slip_transport::SLIP_END || b == cdc_slip_transport::SLIP_ESC) {
                host_cdc_input.push_back(cdc_slip_transport::SLIP_ESC);
                host_cdc_input.push_back(b == cdc_slip_transport::SLIP_END ? cdc_slip_transport::SLIP_ESC_END : cdc_slip_transport::SLIP_ESC_ESC);
            } else host_cdc_input.push_back(b);
        }
        host_cdc_input.push_back(cdc_slip_transport::SLIP_END);
        cdcacm_event_t rx{CDC_EVENT_RX, {}};
        host_cdc_config.callback_rx(0, &rx);
    };
    feed();
    host_cdc_dtr(false); host_cdc_dtr(true);
    assert(cdc.needs_disconnect());
    assert(cdc.begin_session() == ESP_OK);
    expect_empty(cdc);
    feed();
    tinyusb_event_t detached{TINYUSB_EVENT_DETACHED};
    cdc_slip_transport::device_event_callback(&detached, nullptr);
    cdc_slip_transport::device_event_callback(&attached, nullptr);
    host_cdc_dtr(true);
    assert(cdc.needs_disconnect());
    assert(cdc.begin_session() == ESP_OK);
    expect_empty(cdc);
    feed();
    assert(cdc.start_read(&borrowed, &size, 0) == ESP_OK);
    cdc.end_read(borrowed);
    puts("TRANSPORT TESTS PASSED");
}
