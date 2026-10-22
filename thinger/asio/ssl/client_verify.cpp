#include "client_verify.hpp"
#include "../../util/logger.hpp"
#include <boost/asio/ssl/context.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <string>

namespace thinger::asio {

namespace {

    // Deferred validation: any chain is accepted here, the application validates it. OpenSSL
    // still checks the signature of the client (CertificateVerify): it holds the key of the leaf
    int accept_chain(int /*preverified*/, X509_STORE_CTX* /*context*/) {
        return 1;
    }

    int policy_index() {
        static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        return index;
    }

    // The trusted CAs of a PEM, read as the CAs of a context (Boost.Asio's reader): none if
    // there is none, or one cannot be read
    X509_STORE* load_store(const std::string& pem) {
        boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);
        try {
            context.add_certificate_authority(boost::asio::buffer(pem));
        } catch (const boost::system::system_error& e) {
            LOG_ERROR("cannot read the trusted CAs of the client certificates: {}", e.code().message());
            return nullptr;
        }
        X509_STORE* store = SSL_CTX_get_cert_store(context.native_handle());
        X509_STORE_up_ref(store);
        return store;
    }

}

client_verify_policy::client_verify_policy(client_verify_options options) : options_(std::move(options)) {
    // What identifies the policy, as the context of its sessions (SHA-256: 32 bytes, the most
    // OpenSSL takes)
    std::string identity = std::to_string(static_cast<int>(options_.mode)) + ":" + std::to_string(options_.max_depth) +
                           ":" + std::to_string(options_.max_cert_list) + ":" + options_.trusted_cas;
    unsigned int size = 0;
    EVP_Digest(identity.data(), identity.size(), session_id_context_.data(), &size, EVP_sha256(), nullptr);
}

client_verify_policy::~client_verify_policy() {
    X509_STORE_free(store_);
}

std::shared_ptr<const client_verify_policy> client_verify_policy::create(const client_verify_options& options) {
    std::shared_ptr<client_verify_policy> policy(new client_verify_policy(options));
    if (options.mode != client_verify::none && !options.trusted_cas.empty()) {
        policy->store_ = load_store(options.trusted_cas);
        if (!policy->store_) return nullptr;
    }
    return policy;
}

const client_verify_policy& client_verify_policy::deferred() {
    static const client_verify_policy policy{client_verify_options{}};
    return policy;
}

void client_verify_policy::apply(SSL* ssl) const {
    if (options_.mode == client_verify::none) return;
    SSL_set_ex_data(ssl, policy_index(), const_cast<client_verify_policy*>(this));
    SSL_set_session_id_context(ssl, session_id_context_.data(), session_id_context_.size());
    set_mode(ssl, options_.mode);
}

void client_verify_policy::set_mode(SSL* ssl, client_verify mode) const {
    if (mode == client_verify::none) {
        SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
        return;
    }
    int flags = SSL_VERIFY_PEER;
    if (mode == client_verify::required) flags |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    SSL_set_verify(ssl, flags, store_ ? nullptr : accept_chain);
    SSL_set_verify_depth(ssl, options_.max_depth);
    SSL_set_max_cert_list(ssl, static_cast<long>(options_.max_cert_list));
    if (store_) SSL_set1_verify_cert_store(ssl, store_);
}

const client_verify_policy* client_verify_policy::of(const SSL* ssl) {
    return static_cast<const client_verify_policy*>(SSL_get_ex_data(ssl, policy_index()));
}

} // namespace thinger::asio
