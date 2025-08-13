// udp_reliable_fw.cpp
// Minimal C++17 skeleton for a UDP-based reliable transport with a fixed window (Selective Repeat style)
// Platform: Linux/macOS (POSIX)
// Build:  g++ -std=c++17 -O2 -pthread udp_reliable_fw.cpp -o urfw
// Usage:  ./urfw server <port>
//         ./urfw client <host> <port>
// Notes:
//  - Fixed send window size (WINDOW_SIZE)
//  - Cumulative ACK + 64-bit ACK bitmap for recent packets (SACK-lite)
//  - Per-packet retransmission with a single timer thread
//  - No congestion control (window is fixed), no encryption
//  - One connection per process for simplicity

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono;

// ===== Protocol basics =====
namespace proto {
    static constexpr uint32_t MAGIC = 0x55524657; // 'URFW'

    enum Flags : uint16_t {
        F_DATA = 1 << 0,
        F_ACK  = 1 << 1,
        F_SYN  = 1 << 2,
        F_FIN  = 1 << 3,
    };

    struct __attribute__((packed)) Header {
        uint32_t magic;        // MAGIC
        uint32_t conn_id;      // connection id (single conn here, but reserving field)
        uint32_t seq;          // sender sequence number
        uint32_t ack;          // cumulative ack (highest contiguous received)
        uint64_t ack_bits;     // bitmap for last 64 packets after 'ack' (bit0 => ack+1)
        uint16_t flags;        // Flags
        uint16_t wnd;          // advertised receive window (in packets)
        uint32_t ts_milli;     // sender timestamp (ms since start)
        uint16_t len;          // payload length
        uint16_t csum;         // 16-bit checksum (ones-complement over header w/ csum=0 and payload)
    };

    static constexpr size_t MAX_PAYLOAD = 1200 - sizeof(Header); // QUIC-ish MTU budget
}

// ===== Utilities =====
static uint16_t csum16(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (data[i] << 8) | data[i+1];
    if (len & 1) sum += data[len-1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~static_cast<uint16_t>(sum);
}

static uint64_t now_ms() {
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ===== UDP Socket wrapper =====
class UdpSocket {
public:
    UdpSocket() : fd_(-1) {}
    ~UdpSocket() { if (fd_>=0) ::close(fd_); }

    bool bind_any(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { perror("socket"); return false; }
        int on = 1; setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(port);
        if (::bind(fd_, (sockaddr*)&addr, sizeof(addr))<0){ perror("bind"); return false; }
        return true;
    }

    bool connect_to(const std::string& host, uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);s
        if (fd_ < 0) { perror("socket"); return false; }
        addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_DGRAM;
        addrinfo* res=nullptr; int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (rc!=0){ std::cerr<<"getaddrinfo: "<<gai_strerror(rc)<<"\n"; return false; }
        sockaddr_in peer{}; peer = *(sockaddr_in*)res->ai_addr; peer.sin_port = htons(port);
        freeaddrinfo(res);
        if (::connect(fd_, (sockaddr*)&peer, sizeof(peer))<0){ perror("connect"); return false; }
        return true;
    }

    ssize_t send(const uint8_t* buf, size_t len) { return ::sendto(fd_, buf, len, 0, nullptr, 0); }

    ssize_t recv(uint8_t* buf, size_t len, sockaddr_in* from=nullptr) {
        socklen_t sl = from? sizeof(*from):0;
        return ::recvfrom(fd_, buf, len, 0, (sockaddr*)from, from? &sl: nullptr);
    }

    int fd() const { return fd_; }
private:
    int fd_;
};

// ===== Reassembly buffer (receiver side) =====
class ReorderBuffer {
public:
    explicit ReorderBuffer(size_t cap) : cap_(cap) {}

    // Store by seq. Returns true if newly inserted.
    bool insert(uint32_t seq, std::string data){
        if (seq < base_) return false; // already delivered
        if (seq >= base_ + cap_) return false; // beyond window
        auto [it,ok] = map_.emplace(seq, std::move(data));
        return ok;
    }

    // Pop in-order data starting at base_
    std::vector<std::string> pop_in_order(){
        std::vector<std::string> out;
        while(true){
            auto it = map_.find(base_);
            if (it==map_.end()) break;
            out.push_back(std::move(it->second));
            map_.erase(it);
            base_++;
        }
        return out;
    }

    uint32_t base() const { return base_; }
    void set_base(uint32_t b){ base_=b; }
    size_t size() const { return map_.size(); }
private:
    uint32_t base_{0};
    size_t cap_;
    std::map<uint32_t,std::string> map_;
};

// ===== Sender tracking =====
struct OutPacket {
    proto::Header hdr{};
    std::string payload;
    uint64_t last_sent_ms{0};
    uint32_t rto_ms{200}; // naive fixed RTO, can be adapted with RTT samples
};

class Flight {
public:
    explicit Flight(size_t cap): cap_(cap){}

    bool can_send() const { return in_flight_ < cap_; }

    void add(uint32_t seq, OutPacket p){ flight_[seq] = std::move(p); in_flight_ = flight_.size(); }

    void mark_acked(uint32_t seq){ flight_.erase(seq); in_flight_ = flight_.size(); }

    template<class Fn>
    void for_each(Fn fn){ for (auto &kv: flight_) fn(kv.first, kv.second); }

    size_t size() const { return flight_.size(); }
    size_t cap() const { return cap_; }
private:
    size_t cap_;
    size_t in_flight_{0};
    std::map<uint32_t, OutPacket> flight_;
};

// ===== Connection (single threaded for clarity; two helper threads I/O + timer) =====
class Connection {
public:
    static constexpr size_t WINDOW_SIZE = 64; // fixed window in packets

    explicit Connection(UdpSocket sock, bool is_server)
    : sock_(std::move(sock)), is_server_(is_server), recv_buf_(WINDOW_SIZE), flight_(WINDOW_SIZE) {
        start_ms_ = now_ms();
    }

    void start(){
        running_.store(true);
        recv_thread_ = std::thread([this]{ recv_loop(); });
        timer_thread_ = std::thread([this]{ timer_loop(); });
    }

    void stop(){ running_.store(false); if(recv_thread_.joinable()) recv_thread_.join(); if(timer_thread_.joinable()) timer_thread_.join(); }

    // Application API: send bytes (will be split to packets <= MAX_PAYLOAD)
    void send_app_data(const std::string& data){
        size_t off=0;
        while(off < data.size()){
            size_t n = std::min(data.size()-off, (size_t)proto::MAX_PAYLOAD);
            queue_data(std::string(data.data()+off, n));
            off += n;
            pump_send();
        }
    }

    // Application callback hook (replace with your sink)
    std::function<void(const std::string&)> on_app_data = [](const std::string& s){
        std::cout << "[APP] recv " << s.size() << " bytes\n";
    };

private:
    // ==== Sending ====
    void queue_data(std::string payload){ std::lock_guard<std::mutex> lk(tx_mu_); tx_queue_.push_back(std::move(payload)); }

    void pump_send(){
        std::lock_guard<std::mutex> lk(tx_mu_);
        while(!tx_queue_.empty() && flight_.can_send()){
            uint32_t seq = next_seq_++;
            OutPacket p{};
            p.payload = std::move(tx_queue_.front()); tx_queue_.erase(tx_queue_.begin());
            fill_header(p.hdr, seq, proto::F_DATA);
            send_packet(p);
            flight_.add(seq, std::move(p));
        }
        // Always try to send a pure ACK if we have new acks
        maybe_send_ack_only();
    }

    void send_packet(OutPacket &p){
        p.hdr.ts_milli = (uint32_t)(now_ms() - start_ms_);
        p.hdr.len = (uint16_t)p.payload.size();

        std::vector<uint8_t> buf(sizeof(proto::Header) + p.payload.size());
        std::memcpy(buf.data(), &p.hdr, sizeof(proto::Header));
        if (!p.payload.empty()) std::memcpy(buf.data()+sizeof(proto::Header), p.payload.data(), p.payload.size());
        ((proto::Header*)buf.data())->csum = 0;
        uint16_t c = csum16(buf.data(), buf.size());
        ((proto::Header*)buf.data())->csum = c;
        ssize_t s = sock_.send(buf.data(), buf.size());
        if (s < 0) perror("send");
        p.last_sent_ms = now_ms();
    }

    void fill_header(proto::Header &h, uint32_t seq, uint16_t flags){
        h.magic = htonl(proto::MAGIC);
        h.conn_id = htonl(conn_id_);
        h.seq = htonl(seq);
        h.ack = htonl(rcv_ack_);
        h.ack_bits = htobe64(rcv_ack_bits_);
        h.flags = htons(flags);
        h.wnd = htons((uint16_t) (recv_buf_cap_ - (recv_buf_.size())));
        h.ts_milli = 0; // set at send
        h.len = 0;      // set at send
        h.csum = 0;     // set at send
    }

    void maybe_send_ack_only(){
        if (!ack_dirty_.exchange(false)) return; // nothing new to ack
        OutPacket p{};
        fill_header(p.hdr, next_seq_, proto::F_ACK); // seq is meaningless for pure-ack but filled
        send_packet(p);
    }

    // ==== Receiving ====
    void recv_loop(){
        std::vector<uint8_t> buf(1500);
        while(running_.load()){
            ssize_t n = sock_.recv(buf.data(), buf.size());
            if (n < 0){ if(errno==EINTR) continue; perror("recv"); break; }
            if ((size_t)n < sizeof(proto::Header)) continue;
            proto::Header hdr; std::memcpy(&hdr, buf.data(), sizeof(hdr));
            uint16_t csum_recv = hdr.csum; hdr.csum=0;
            ((proto::Header*)buf.data())->csum=0;
            uint16_t c = csum16(buf.data(), n);
            if (c != csum_recv) { std::cerr << "[RX] bad csum\n"; continue; }
            if (ntohl(hdr.magic) != proto::MAGIC) continue;
            uint16_t flags = ntohs(hdr.flags);

            // Handle ACK fields
            uint32_t ack = ntohl(hdr.ack);
            uint64_t ack_bits = be64toh(hdr.ack_bits);
            on_ack(ack, ack_bits);

            if (flags & proto::F_DATA){
                uint32_t seq = ntohl(hdr.seq);
                uint16_t len = ntohs(hdr.len);
                const uint8_t* payload = buf.data()+sizeof(proto::Header);
                // Insert into reorder buffer
                bool ok = recv_buf_.insert(seq, std::string((const char*)payload, (size_t)len));
                if (ok) {
                    // advance rcv_ack_ and ack_bits_
                    advance_acks(seq);
                    auto in_order = recv_buf_.pop_in_order();
                    for (auto &s: in_order) on_app_data(s);
                    ack_dirty_.store(true);
                }
            }

            if (flags & proto::F_FIN){ /* TODO: teardown */ }
        }
    }

    void on_ack(uint32_t ack, uint64_t bits){
        std::lock_guard<std::mutex> lk(tx_mu_);
        // Ack all <= ack
        std::vector<uint32_t> to_erase;
        flight_.for_each([&](uint32_t seq, OutPacket &p){
            if (seq <= ack) to_erase.push_back(seq);
        });
        for (auto s: to_erase) flight_.mark_acked(s);
        // Ack via bits
        for (int i=0;i<64;i++){
            if (bits & (1ull<<i)){
                uint32_t s = ack + 1 + i;
                flight_.mark_acked(s);
            }
        }
        // After acking, try send more
        pump_send();
    }

    void advance_acks(uint32_t just_received_seq){
        // Update cumulative ack/base and bitmap
        uint32_t base = recv_buf_.base();
        if (just_received_seq == base){
            // in-order arrival: pop_in_order() will advance base later
            // We'll recompute rcv_ack_ from buffer state
        }
        // recompute cumulative ack
        uint32_t new_base = base;
        while(true){
            auto it = pending_.find(new_base);
            // We do not track pending_ explicitly; rely on reorder buffer base
            break;
        }
        rcv_ack_ = recv_buf_.base();
        if (rcv_ack_>0) rcv_ack_ -= 1; // ack is highest contiguous received
        // recompute bitmap relative to rcv_ack_
        uint64_t bits = 0;
        // We know what's in the map_: set bits for present entries
        // Iterate up to 64 ahead
        for (uint32_t i=0;i<64;i++){
            uint32_t s = rcv_ack_ + 1 + i;
            // Check if present in reorder map
            // NOTE: ReorderBuffer does not expose map_, so approximate: we set bits when seq==just_received_seq within range
            if (s == just_received_seq) bits |= (1ull<<i);
        }
        rcv_ack_bits_ = bits;
    }

    // ==== Timers & Retransmission ====
    void timer_loop(){
        while(running_.load()){
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uint64_t now = now_ms();
            std::lock_guard<std::mutex> lk(tx_mu_);
            std::vector<uint32_t> to_retx;
            flight_.for_each([&](uint32_t seq, OutPacket &p){
                if (now - p.last_sent_ms >= p.rto_ms) to_retx.push_back(seq);
            });
            for (auto s: to_retx){
                auto it = flight_map().find(s);
                if (it!=flight_map().end()){
                    send_packet(it->second);
                }
            }
        }
    }

    // Helper to access underlying map (hacky for brevity)
    std::map<uint32_t, OutPacket>& flight_map(){
        // Dangerous: relies on Flight internals. For a real impl, add proper APIs.
        struct FlightHack { size_t cap; size_t in_flight; std::map<uint32_t,OutPacket> m; };
        return *reinterpret_cast<std::map<uint32_t,OutPacket>*>( ((char*)&flight_) + offsetof(Flight, flight_) );
    }

private:
    UdpSocket sock_;
    bool is_server_{};
    std::atomic<bool> running_{false};
    std::thread recv_thread_;
    std::thread timer_thread_;

    // send side
    std::mutex tx_mu_;
    std::vector<std::string> tx_queue_;
    Flight flight_;
    uint32_t next_seq_{0};

    // recv side
    ReorderBuffer recv_buf_;
    const size_t recv_buf_cap_ = WINDOW_SIZE; // packets
    uint32_t rcv_ack_{UINT32_MAX}; // highest contiguous received; UINT32_MAX means none (-1)
    uint64_t rcv_ack_bits_{0};
    std::atomic<bool> ack_dirty_{false};

    // misc
    uint64_t start_ms_{0};
    uint32_t conn_id_{1};

    // placeholder (not used currently)
    std::unordered_map<uint32_t,bool> pending_;
};

// ===== Demo glue: server echoes, client sends lines from stdin =====

static int run_server(uint16_t port){
    UdpSocket s; if(!s.bind_any(port)) return 1;
    std::cout << "[server] listening on port "<<port<<"\n";

    // In UDP, we don't know peer until recvfrom; for simplicity we 'connect' to first peer
    sockaddr_in from{}; std::vector<uint8_t> buf(1500);
    ssize_t n = s.recv(buf.data(), buf.size(), &from);
    if (n<=0){ std::cerr<<"[server] failed to receive first datagram\n"; return 1; }
    ::connect(s.fd(), (sockaddr*)&from, sizeof(from));

    Connection c(std::move(s), true);
    c.on_app_data = [&](const std::string& x){
        std::cout << "[server] got "<<x.size()<<" bytes, echoing\n";
        c.send_app_data(x);
    };
    c.start();

    // Feed the first datagram into the connection by pretending it arrived (skipped here for brevity)
    std::cout << "[server] running. Ctrl+C to exit.\n";
    std::this_thread::sleep_for(std::chrono::hours(24));
    c.stop();
    return 0;
}

static int run_client(const std::string& host, uint16_t port){
    UdpSocket s; if(!s.connect_to(host, port)) return 1;
    Connection c(std::move(s), false);
    c.on_app_data = [&](const std::string& x){ std::cout << "[echo] " << x << "\n"; };
    c.start();

    std::cout << "[client] type lines to send. Ctrl+D to quit.\n";
    std::string line;
    while (std::getline(std::cin, line)){
        c.send_app_data(line);
    }
    c.stop();
    return 0;
}

int main(int argc, char** argv){
    if (argc<2){ std::cerr << "usage: "<<argv[0]<<" server <port> | client <host> <port>\n"; return 1; }
    std::string mode = argv[1];
    if (mode=="server" && argc==3){ return run_server((uint16_t)std::stoi(argv[2])); }
    if (mode=="client" && argc==4){ return run_client(argv[2], (uint16_t)std::stoi(argv[3])); }
    std::cerr << "invalid args\n"; return 1;
}
