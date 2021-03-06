/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "message/messaging_service.hh"
#include "core/distributed.hh"
#include "core/sleep.hh"
#include "gms/failure_detector.hh"
#include "gms/gossiper.hh"
#include "gms/gossip_digest_syn.hh"
#include "gms/gossip_digest_ack.hh"
#include "gms/gossip_digest_ack2.hh"
#include "gms/gossiper.hh"
#include "rpc/rpc.hh"
#include "config.hh"
#include "digest_algorithm.hh"
#include "idl/gossip_digest.dist.hh"
#include "serializer_impl.hh"
#include "serialization_visitors.hh"
#include "idl/gossip_digest.dist.impl.hh"
#include "rpc/lz4_compressor.hh"
#include "rpc/multi_algo_compressor_factory.hh"

namespace net {

// thunk from rpc serializers to generate serializers
template <typename T, typename Output>
void write(serializer, Output& out, const T& data) {
    ser::serialize(out, data);
}
template <typename T, typename Input>
T read(serializer, Input& in, boost::type<T> type) {
    return ser::deserialize(in, type);
}

template <typename Output, typename T>
void write(serializer s, Output& out, const foreign_ptr<T>& v) {
    return write(s, out, *v);
}
template <typename Input, typename T>
foreign_ptr<T> read(serializer s, Input& in, boost::type<foreign_ptr<T>>) {
    return make_foreign(read(s, in, boost::type<T>()));
}

template <typename Output, typename T>
void write(serializer s, Output& out, const lw_shared_ptr<T>& v) {
    return write(s, out, *v);
}
template <typename Input, typename T>
lw_shared_ptr<T> read(serializer s, Input& in, boost::type<lw_shared_ptr<T>>) {
    return make_lw_shared(read(s, in, boost::type<T>()));
}

static logging::logger logger("messaging_service");
static logging::logger rpc_logger("rpc");

using inet_address = gms::inet_address;
using gossip_digest_syn = gms::gossip_digest_syn;
using gossip_digest_ack = gms::gossip_digest_ack;
using gossip_digest_ack2 = gms::gossip_digest_ack2;
using rpc_protocol = rpc::protocol<serializer, messaging_verb>;
using namespace std::chrono_literals;

static rpc::lz4_compressor::factory lz4_compressor_factory;
static rpc::multi_algo_compressor_factory compressor_factory(&lz4_compressor_factory);

struct messaging_service::rpc_protocol_wrapper : public rpc_protocol { using rpc_protocol::rpc_protocol; };

// This wrapper pretends to be rpc_protocol::client, but also handles
// stopping it before destruction, in case it wasn't stopped already.
// This should be integrated into messaging_service proper.
class messaging_service::rpc_protocol_client_wrapper {
    std::unique_ptr<rpc_protocol::client> _p;
public:
    rpc_protocol_client_wrapper(rpc_protocol& proto, rpc::client_options opts, ipv4_addr addr, ipv4_addr local = ipv4_addr())
            : _p(std::make_unique<rpc_protocol::client>(proto, std::move(opts), addr, local)) {
    }
    rpc_protocol_client_wrapper(rpc_protocol& proto, rpc::client_options opts, ipv4_addr addr, ipv4_addr local, ::shared_ptr<seastar::tls::server_credentials> c)
            : _p(std::make_unique<rpc_protocol::client>(proto, std::move(opts), seastar::tls::socket(c), addr, local))
    {}
    auto get_stats() const { return _p->get_stats(); }
    future<> stop() { return _p->stop(); }
    bool error() {
        return _p->error();
    }
    operator rpc_protocol::client&() { return *_p; }
};

struct messaging_service::rpc_protocol_server_wrapper : public rpc_protocol::server { using rpc_protocol::server::server; };

constexpr int32_t messaging_service::current_version;

distributed<messaging_service> _the_messaging_service;

bool operator==(const msg_addr& x, const msg_addr& y) {
    // Ignore cpu id for now since we do not really support shard to shard connections
    return x.addr == y.addr;
}

bool operator<(const msg_addr& x, const msg_addr& y) {
    // Ignore cpu id for now since we do not really support shard to shard connections
    if (x.addr < y.addr) {
        return true;
    } else {
        return false;
    }
}

std::ostream& operator<<(std::ostream& os, const msg_addr& x) {
    return os << x.addr << ":" << x.cpu_id;
}

size_t msg_addr::hash::operator()(const msg_addr& id) const {
    // Ignore cpu id for now since we do not really support // shard to shard connections
    return std::hash<uint32_t>()(id.addr.raw_addr());
}

messaging_service::shard_info::shard_info(shared_ptr<rpc_protocol_client_wrapper>&& client)
    : rpc_client(std::move(client)) {
}

rpc::stats messaging_service::shard_info::get_stats() const {
    return rpc_client->get_stats();
}

void messaging_service::foreach_client(std::function<void(const msg_addr& id, const shard_info& info)> f) const {
    for (unsigned idx = 0; idx < _clients.size(); idx ++) {
        for (auto i = _clients[idx].cbegin(); i != _clients[idx].cend(); i++) {
            f(i->first, i->second);
        }
    }
}

void messaging_service::foreach_server_connection_stats(std::function<void(const rpc::client_info&, const rpc::stats&)>&& f) const {
    for (auto&& s : _server) {
        if (s) {
            s->foreach_connection([f](const rpc_protocol::server::connection& c) {
                f(c.info(), c.get_stats());
            });
        }
    }
}

void messaging_service::increment_dropped_messages(messaging_verb verb) {
    _dropped_messages[static_cast<int32_t>(verb)]++;
}

uint64_t messaging_service::get_dropped_messages(messaging_verb verb) const {
    return _dropped_messages[static_cast<int32_t>(verb)];
}

const uint64_t* messaging_service::get_dropped_messages() const {
    return _dropped_messages;
}

int32_t messaging_service::get_raw_version(const gms::inet_address& endpoint) const {
    // FIXME: messaging service versioning
    return current_version;
}

bool messaging_service::knows_version(const gms::inet_address& endpoint) const {
    // FIXME: messaging service versioning
    return true;
}

// Register a handler (a callback lambda) for verb
template <typename Func>
void register_handler(messaging_service* ms, messaging_verb verb, Func&& func) {
    ms->rpc()->register_handler(verb, std::move(func));
}

messaging_service::messaging_service(gms::inet_address ip, uint16_t port, bool listen_now)
    : messaging_service(std::move(ip), port, encrypt_what::none, compress_what::none, 0, nullptr, false, listen_now)
{}

static
rpc::resource_limits
rpc_resource_limits() {
    rpc::resource_limits limits;
    limits.bloat_factor = 3;
    limits.basic_request_size = 1000;
    limits.max_memory = std::max<size_t>(0.08 * memory::stats().total_memory(), 1'000'000);
    return limits;
}

void messaging_service::start_listen() {
    bool listen_to_bc = _should_listen_to_broadcast_address && _listen_address != utils::fb_utilities::get_broadcast_address();
    rpc::server_options so;
    if (_compress_what != compress_what::none) {
        so.compressor_factory = &compressor_factory;
    }
    if (!_server[0]) {
        auto listen = [&] (const gms::inet_address& a) {
            auto addr = ipv4_addr{a.raw_addr(), _port};
            return std::unique_ptr<rpc_protocol_server_wrapper>(new rpc_protocol_server_wrapper(*_rpc,
                    so, addr, rpc_resource_limits()));
        };
        _server[0] = listen(_listen_address);
        if (listen_to_bc) {
            _server[1] = listen(utils::fb_utilities::get_broadcast_address());
        }
    }

    if (!_server_tls[0]) {
        auto listen = [&] (const gms::inet_address& a) {
            return std::unique_ptr<rpc_protocol_server_wrapper>(
                    [this, &so, &a] () -> std::unique_ptr<rpc_protocol_server_wrapper>{
                if (_encrypt_what == encrypt_what::none) {
                    return nullptr;
                }
                listen_options lo;
                lo.reuse_address = true;
                auto addr = make_ipv4_address(ipv4_addr{a.raw_addr(), _ssl_port});
                return std::make_unique<rpc_protocol_server_wrapper>(*_rpc,
                        so, seastar::tls::listen(_credentials, addr, lo));
            }());
        };
        _server_tls[0] = listen(_listen_address);
        if (listen_to_bc) {
            _server_tls[1] = listen(utils::fb_utilities::get_broadcast_address());
        }
    }
    // Do this on just cpu 0, to avoid duplicate logs.
    if (engine().cpu_id() == 0) {
        if (_server_tls[0]) {
            logger.info("Starting Encrypted Messaging Service on SSL port {}", _ssl_port);
        }
        logger.info("Starting Messaging Service on port {}", _port);
    }
}

messaging_service::messaging_service(gms::inet_address ip
        , uint16_t port
        , encrypt_what ew
        , compress_what cw
        , uint16_t ssl_port
        , std::shared_ptr<seastar::tls::credentials_builder> credentials
        , bool sltba
        , bool listen_now)
    : _listen_address(ip)
    , _port(port)
    , _ssl_port(ssl_port)
    , _encrypt_what(ew)
    , _compress_what(cw)
    , _should_listen_to_broadcast_address(sltba)
    , _rpc(new rpc_protocol_wrapper(serializer { }))
    , _credentials(credentials ? credentials->build_server_credentials() : nullptr)
{
    _rpc->set_logger([] (const sstring& log) {
            rpc_logger.info("{}", log);
    });
    /*
    register_handler(this, messaging_verb::CLIENT_ID, [] (rpc::client_info& ci, gms::inet_address broadcast_address, uint32_t src_cpu_id, rpc::optional<uint64_t> max_result_size) {
        ci.attach_auxiliary("baddr", broadcast_address);
        ci.attach_auxiliary("src_cpu_id", src_cpu_id);
        ci.attach_auxiliary("max_result_size", max_result_size.value_or(query::result_memory_limiter::maximum_result_size));
        return rpc::no_wait;
    });
    */
    if (listen_now) {
        start_listen();
    }
}

msg_addr messaging_service::get_source(const rpc::client_info& cinfo) {
    return msg_addr{
        cinfo.retrieve_auxiliary<gms::inet_address>("baddr"),
        cinfo.retrieve_auxiliary<uint32_t>("src_cpu_id")
    };
}

messaging_service::~messaging_service() = default;

uint16_t messaging_service::port() {
    return _port;
}

gms::inet_address messaging_service::listen_address() {
    return _listen_address;
}

future<> messaging_service::stop_tls_server() {
    for (auto&& s : _server_tls) {
        if (s) {
            return s->stop();
        }
    }
    return make_ready_future<>();
}

future<> messaging_service::stop_nontls_server() {
    for (auto&& s : _server) {
        if (s) {
            return s->stop();
        }
    }
    return make_ready_future<>();
}

future<> messaging_service::stop_client() {
    return parallel_for_each(_clients, [] (auto& m) {
        return parallel_for_each(m, [] (std::pair<const msg_addr, shard_info>& c) {
            return c.second.rpc_client->stop();
        });
    });
}

future<> messaging_service::stop() {
    _stopping = true;
    return when_all(stop_nontls_server(), stop_tls_server(), stop_client()).discard_result();
}

rpc::no_wait_type messaging_service::no_wait() {
    return rpc::no_wait;
}

static unsigned get_rpc_client_idx(messaging_verb verb) {
    unsigned idx = 0;
    // GET_SCHEMA_VERSION is sent from read/mutate verbs so should be
    // sent on a different connection to avoid potential deadlocks
    // as well as reduce latency as there are potentially many requests
    // blocked on schema version request.
    if (verb == messaging_verb::GOSSIP_DIGEST_SYN ||
        verb == messaging_verb::GOSSIP_DIGEST_ACK2 ||
        verb == messaging_verb::GOSSIP_SHUTDOWN ||
        verb == messaging_verb::GOSSIP_ECHO) {
        idx = 1;
    }
    return idx;
}

/**
 * Get an IP for a given endpoint to connect to
 *
 * @param ep endpoint to check
 *
 * @return preferred IP (local) for the given endpoint if exists and if the
 *         given endpoint resides in the same data center with the current Node.
 *         Otherwise 'ep' itself is returned.
 */
gms::inet_address messaging_service::get_preferred_ip(gms::inet_address ep) {
/*
    auto it = _preferred_ip_cache.find(ep);

    if (it != _preferred_ip_cache.end()) {
        auto& snitch_ptr = locator::i_endpoint_snitch::get_local_snitch_ptr();
        auto my_addr = utils::fb_utilities::get_broadcast_address();

        if (snitch_ptr->get_datacenter(ep) == snitch_ptr->get_datacenter(my_addr)) {
            return it->second;
        }
    }
*/
    // If cache doesn't have an entry for this endpoint - return endpoint itself
    return ep;
}

future<> messaging_service::init_local_preferred_ip_cache() {
/*
    return db::system_keyspace::get_preferred_ips().then([this] (auto ips_cache) {
        _preferred_ip_cache = ips_cache;
        //
        // Reset the connections to the endpoints that have entries in
        // _preferred_ip_cache so that they reopen with the preferred IPs we've
        // just read.
        //
        for (auto& p : _preferred_ip_cache) {
            msg_addr id = {
                .addr = p.first
            };

            this->remove_rpc_client(id);
        }
    });
*/
    return make_ready_future<>();
}

void messaging_service::cache_preferred_ip(gms::inet_address ep, gms::inet_address ip) {
    _preferred_ip_cache[ep] = ip;
}

shared_ptr<messaging_service::rpc_protocol_client_wrapper> messaging_service::get_rpc_client(messaging_verb verb, msg_addr id) {
    assert(!_stopping);
    auto idx = get_rpc_client_idx(verb);
    auto it = _clients[idx].find(id);

    if (it != _clients[idx].end()) {
        auto c = it->second.rpc_client;
        if (!c->error()) {
            return c;
        }
        remove_error_rpc_client(verb, id);
    }

    auto must_encrypt = [&id, this] {
/*
        if (_encrypt_what == encrypt_what::none) {
            return false;
        }
        if (_encrypt_what == encrypt_what::all) {
            return true;
        }

        auto& snitch_ptr = locator::i_endpoint_snitch::get_local_snitch_ptr();

        if (_encrypt_what == encrypt_what::dc) {
            return snitch_ptr->get_datacenter(id.addr)
                            != snitch_ptr->get_datacenter(utils::fb_utilities::get_broadcast_address());
        }
        return snitch_ptr->get_rack(id.addr)
                        != snitch_ptr->get_rack(utils::fb_utilities::get_broadcast_address());
*/
        return false;
    }();

    auto must_compress = [&id, this] {
        if (_compress_what == compress_what::none) {
            return false;
        }

        if (_compress_what == compress_what::dc) {
/*
            auto& snitch_ptr = locator::i_endpoint_snitch::get_local_snitch_ptr();
            return snitch_ptr->get_datacenter(id.addr)
                            != snitch_ptr->get_datacenter(utils::fb_utilities::get_broadcast_address());
*/
        }

        return true;
    }();

    auto remote_addr = ipv4_addr(get_preferred_ip(id.addr).raw_addr(), must_encrypt ? _ssl_port : _port);
    auto local_addr = ipv4_addr{_listen_address.raw_addr(), 0};

    rpc::client_options opts;
    // send keepalive messages each minute if connection is idle, drop connection after 10 failures
    opts.keepalive = std::experimental::optional<net::tcp_keepalive_params>({60s, 60s, 10});
    if (must_compress) {
        opts.compressor_factory = &compressor_factory;
    }

    auto client = must_encrypt ?
                    ::make_shared<rpc_protocol_client_wrapper>(*_rpc, std::move(opts),
                                    remote_addr, local_addr, _credentials) :
                    ::make_shared<rpc_protocol_client_wrapper>(*_rpc, std::move(opts),
                                    remote_addr, local_addr);

    it = _clients[idx].emplace(id, shard_info(std::move(client))).first;
/*
    uint32_t src_cpu_id = engine().cpu_id();
    _rpc->make_client<rpc::no_wait_type(gms::inet_address, uint32_t, uint64_t)>(messaging_verb::CLIENT_ID)(*it->second.rpc_client, utils::fb_utilities::get_broadcast_address(), src_cpu_id,
                                                                                                           query::result_memory_limiter::maximum_result_size);
*/
    return it->second.rpc_client;
}

void messaging_service::remove_rpc_client_one(clients_map& clients, msg_addr id, bool dead_only) {
    if (_stopping) {
        // if messaging service is in a processed of been stopped no need to
        // stop and remove connection here since they are being stopped already
        // and we'll just interfere
        return;
    }

    auto it = clients.find(id);
    if (it != clients.end() && (!dead_only || it->second.rpc_client->error())) {
        auto client = std::move(it->second.rpc_client);
        clients.erase(it);
        //
        // Explicitly call rpc_protocol_client_wrapper::stop() for the erased
        // item and hold the messaging_service shared pointer till it's over.
        // This will make sure messaging_service::stop() blocks until
        // client->stop() is over.
        //
        client->stop().finally([id, client, ms = shared_from_this()] {
            logger.debug("dropped connection to {}", id.addr);
        }).discard_result();
    }
}

void messaging_service::remove_error_rpc_client(messaging_verb verb, msg_addr id) {
    remove_rpc_client_one(_clients[get_rpc_client_idx(verb)], id, true);
}

void messaging_service::remove_rpc_client(msg_addr id) {
    for (auto& c : _clients) {
        remove_rpc_client_one(c, id, false);
    }
}

std::unique_ptr<messaging_service::rpc_protocol_wrapper>& messaging_service::rpc() {
    return _rpc;
}

// Send a message for verb
template <typename MsgIn, typename... MsgOut>
auto send_message(messaging_service* ms, messaging_verb verb, msg_addr id, MsgOut&&... msg) {
    auto rpc_handler = ms->rpc()->make_client<MsgIn(MsgOut...)>(verb);
    if (ms->is_stopping()) {
        using futurator = futurize<std::result_of_t<decltype(rpc_handler)(rpc_protocol::client&, MsgOut...)>>;
        return futurator::make_exception_future(rpc::closed_error());
    }
    auto rpc_client_ptr = ms->get_rpc_client(verb, id);
    auto& rpc_client = *rpc_client_ptr;
    return rpc_handler(rpc_client, std::forward<MsgOut>(msg)...).then_wrapped([ms = ms->shared_from_this(), id, verb, rpc_client_ptr = std::move(rpc_client_ptr)] (auto&& f) {
        try {
            if (f.failed()) {
                ms->increment_dropped_messages(verb);
                f.get();
                assert(false); // never reached
            }
            return std::move(f);
        } catch (rpc::closed_error) {
            // This is a transport error
            ms->remove_error_rpc_client(verb, id);
            throw;
        } catch (...) {
            // This is expected to be a rpc server error, e.g., the rpc handler throws a std::runtime_error.
            throw;
        }
    });
}

// TODO: Remove duplicated code in send_message
template <typename MsgIn, typename Timeout, typename... MsgOut>
auto send_message_timeout(messaging_service* ms, messaging_verb verb, msg_addr id, Timeout timeout, MsgOut&&... msg) {
    auto rpc_handler = ms->rpc()->make_client<MsgIn(MsgOut...)>(verb);
    if (ms->is_stopping()) {
        using futurator = futurize<std::result_of_t<decltype(rpc_handler)(rpc_protocol::client&, MsgOut...)>>;
        return futurator::make_exception_future(rpc::closed_error());
    }
    auto rpc_client_ptr = ms->get_rpc_client(verb, id);
    auto& rpc_client = *rpc_client_ptr;
    return rpc_handler(rpc_client, timeout, std::forward<MsgOut>(msg)...).then_wrapped([ms = ms->shared_from_this(), id, verb, rpc_client_ptr = std::move(rpc_client_ptr)] (auto&& f) {
        try {
            if (f.failed()) {
                ms->increment_dropped_messages(verb);
                f.get();
                assert(false); // never reached
            }
            return std::move(f);
        } catch (rpc::closed_error) {
            // This is a transport error
            ms->remove_error_rpc_client(verb, id);
            throw;
        } catch (...) {
            // This is expected to be a rpc server error, e.g., the rpc handler throws a std::runtime_error.
            throw;
        }
    });
}

template <typename MsgIn, typename... MsgOut>
auto send_message_timeout_and_retry(messaging_service* ms, messaging_verb verb, msg_addr id,
        std::chrono::seconds timeout, int nr_retry, std::chrono::seconds wait, MsgOut... msg) {
    namespace stdx = std::experimental;
    using MsgInTuple = typename futurize_t<MsgIn>::value_type;
    return do_with(int(nr_retry), std::move(msg)..., [ms, verb, id, timeout, wait, nr_retry] (auto& retry, const auto&... messages) {
        return repeat_until_value([ms, verb, id, timeout, wait, nr_retry, &retry, &messages...] {
            return send_message_timeout<MsgIn>(ms, verb, id, timeout, messages...).then_wrapped(
                    [ms, verb, id, timeout, wait, nr_retry, &retry] (auto&& f) mutable {
                auto vb = int(verb);
                try {
                    MsgInTuple ret = f.get();
                    if (retry != nr_retry) {
                        logger.info("Retry verb={} to {}, retry={}: OK", vb, id, retry);
                    }
                    return make_ready_future<stdx::optional<MsgInTuple>>(std::move(ret));
                } catch (rpc::timeout_error) {
                    logger.info("Retry verb={} to {}, retry={}: timeout in {} seconds", vb, id, retry, timeout.count());
                    throw;
                } catch (rpc::closed_error) {
                    logger.info("Retry verb={} to {}, retry={}: {}", vb, id, retry, std::current_exception());
                    // Stop retrying if retry reaches 0 or message service is shutdown
                    // or the remote node is removed from gossip (on_remove())
                    retry--;
                    if (retry == 0) {
                        logger.debug("Retry verb={} to {}, retry={}: stop retrying: retry == 0", vb, id, retry);
                        throw;
                    }
                    if (ms->is_stopping()) {
                        logger.debug("Retry verb={} to {}, retry={}: stop retrying: messaging_service is stopped",
                                     vb, id, retry);
                        throw;
                    }
                    if (!gms::get_local_gossiper().is_known_endpoint(id.addr)) {
                        logger.debug("Retry verb={} to {}, retry={}: stop retrying: node is removed from the cluster",
                                     vb, id, retry);
                        throw;
                    }
                    return sleep_abortable(wait).then([] {
                        return make_ready_future<stdx::optional<MsgInTuple>>(stdx::nullopt);
                    }).handle_exception([vb, id, retry] (std::exception_ptr ep) {
                        logger.debug("Retry verb={} to {}, retry={}: stop retrying: {}", vb, id, retry, ep);
                        return make_exception_future<stdx::optional<MsgInTuple>>(ep);
                    });
                } catch (...) {
                    throw;
                }
            });
        }).then([ms = ms->shared_from_this()] (MsgInTuple result) {
            return futurize<MsgIn>::from_tuple(std::move(result));
        });
    });
}

// Send one way message for verb
template <typename... MsgOut>
auto send_message_oneway(messaging_service* ms, messaging_verb verb, msg_addr id, MsgOut&&... msg) {
    return send_message<rpc::no_wait_type>(ms, std::move(verb), std::move(id), std::forward<MsgOut>(msg)...);
}

// Send one way message for verb
template <typename Timeout, typename... MsgOut>
auto send_message_oneway_timeout(messaging_service* ms, Timeout timeout, messaging_verb verb, msg_addr id, MsgOut&&... msg) {
    return send_message_timeout<rpc::no_wait_type>(ms, std::move(verb), std::move(id), timeout, std::forward<MsgOut>(msg)...);
}

// Wrappers for verbs

// Retransmission parameters for streaming verbs.
// A stream plan gives up retrying in 10*30 + 10*60 seconds (15 minutes) at
// most, 10*30 seconds (5 minutes) at least.
static constexpr int streaming_nr_retry = 10;
static constexpr std::chrono::seconds streaming_timeout{10*60};
static constexpr std::chrono::seconds streaming_wait_before_retry{30};

void messaging_service::register_gossip_echo(std::function<future<> ()>&& func) {
    register_handler(this, messaging_verb::GOSSIP_ECHO, std::move(func));
}
void messaging_service::unregister_gossip_echo() {
    _rpc->unregister_handler(net::messaging_verb::GOSSIP_ECHO);
}
future<> messaging_service::send_gossip_echo(msg_addr id) {
    return send_message_timeout<void>(this, messaging_verb::GOSSIP_ECHO, std::move(id), 3000ms);
}

void messaging_service::register_gossip_shutdown(std::function<rpc::no_wait_type (inet_address from)>&& func) {
    register_handler(this, messaging_verb::GOSSIP_SHUTDOWN, std::move(func));
}
void messaging_service::unregister_gossip_shutdown() {
    _rpc->unregister_handler(net::messaging_verb::GOSSIP_SHUTDOWN);
}
future<> messaging_service::send_gossip_shutdown(msg_addr id, inet_address from) {
    return send_message_oneway(this, messaging_verb::GOSSIP_SHUTDOWN, std::move(id), std::move(from));
}

// gossip syn
void messaging_service::register_gossip_digest_syn(std::function<rpc::no_wait_type (const rpc::client_info& cinfo, gossip_digest_syn)>&& func) {
    register_handler(this, messaging_verb::GOSSIP_DIGEST_SYN, std::move(func));
}
void messaging_service::unregister_gossip_digest_syn() {
    _rpc->unregister_handler(net::messaging_verb::GOSSIP_DIGEST_SYN);
}
future<> messaging_service::send_gossip_digest_syn(msg_addr id, gossip_digest_syn msg) {
    return send_message_oneway(this, messaging_verb::GOSSIP_DIGEST_SYN, std::move(id), std::move(msg));
}

// gossip ack
void messaging_service::register_gossip_digest_ack(std::function<rpc::no_wait_type (const rpc::client_info& cinfo, gossip_digest_ack)>&& func) {
    register_handler(this, messaging_verb::GOSSIP_DIGEST_ACK, std::move(func));
}
void messaging_service::unregister_gossip_digest_ack() {
    _rpc->unregister_handler(net::messaging_verb::GOSSIP_DIGEST_ACK);
}
future<> messaging_service::send_gossip_digest_ack(msg_addr id, gossip_digest_ack msg) {
    return send_message_oneway(this, messaging_verb::GOSSIP_DIGEST_ACK, std::move(id), std::move(msg));
}

// gossip ack2
void messaging_service::register_gossip_digest_ack2(std::function<rpc::no_wait_type (gossip_digest_ack2)>&& func) {
    register_handler(this, messaging_verb::GOSSIP_DIGEST_ACK2, std::move(func));
}
void messaging_service::unregister_gossip_digest_ack2() {
    _rpc->unregister_handler(net::messaging_verb::GOSSIP_DIGEST_ACK2);
}
future<> messaging_service::send_gossip_digest_ack2(msg_addr id, gossip_digest_ack2 msg) {
    return send_message_oneway(this, messaging_verb::GOSSIP_DIGEST_ACK2, std::move(id), std::move(msg));
}
}
