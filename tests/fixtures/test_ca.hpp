#ifndef THINGER_HTTP_TEST_CA_HPP
#define THINGER_HTTP_TEST_CA_HPP

#include <thinger/asio/ssl/certificate_manager.hpp>
#include <boost/asio/ip/address.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace thinger::http::test {

// Key types of the certificates issued
enum class key_type { ec_p256, rsa2048, rsa3072 };

// A private certificate authority for TLS tests: its certificate (to trust it), server and
// client certificates it issues, and intermediate CAs below it
class test_ca {
public:
    explicit test_ca(const std::string& name = "Thinger Test CA") : test_ca(name, nullptr) {}

    ~test_ca() {
        X509_free(certificate_);
        EVP_PKEY_free(key_);
    }

    test_ca(const test_ca&) = delete;
    test_ca& operator=(const test_ca&) = delete;

    // Certificate of the CA (PEM)
    const std::string& pem() const { return pem_; }

    // A server certificate for `host` (a DNS name or an IP address): certificate and key PEM
    std::pair<std::string, std::string> issue(const std::string& host) const {
        boost::system::error_code ec;
        boost::asio::ip::make_address(host, ec);
        std::string san = (ec ? "DNS:" : "IP:") + host;
        return issue_leaf(host, key_type::ec_p256, {
            {NID_subject_alt_name, san},
            {NID_basic_constraints, "CA:FALSE"},
            {NID_key_usage, "critical,digitalSignature"},
            {NID_ext_key_usage, "serverAuth"},
        });
    }

    // A client certificate (TLS client authentication) with the common name `name`:
    // certificate and key PEM
    std::pair<std::string, std::string> issue_client(const std::string& name, key_type type = key_type::ec_p256) const {
        return issue_leaf(name, type, {
            {NID_basic_constraints, "CA:FALSE"},
            {NID_key_usage, "critical,digitalSignature"},
            {NID_ext_key_usage, "clientAuth"},
        });
    }

    // An intermediate CA issued by this one
    std::unique_ptr<test_ca> issue_intermediate(const std::string& name) const {
        return std::unique_ptr<test_ca>(new test_ca(name, this));
    }

    // The DER encoding of the first certificate of a PEM
    static std::string der_of(const std::string& pem) {
        BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!certificate) return {};
        unsigned char* data = nullptr;
        int size = i2d_X509(certificate, &data);
        std::string der(reinterpret_cast<const char*>(data), static_cast<size_t>(size > 0 ? size : 0));
        OPENSSL_free(data);
        X509_free(certificate);
        return der;
    }

private:
    // A root CA (without issuer), or an intermediate one
    test_ca(const std::string& name, const test_ca* issuer) {
        key_ = EVP_EC_gen("P-256");
        certificate_ = make(name, key_, issuer ? issuer->certificate_ : nullptr, issuer ? issuer->key_ : nullptr, {
            {NID_basic_constraints, "critical,CA:TRUE"},
            {NID_key_usage, "critical,keyCertSign,cRLSign"},
        });
        pem_ = to_pem([&](BIO* bio) { PEM_write_bio_X509(bio, certificate_); });
    }

    static EVP_PKEY* make_key(key_type type) {
        switch (type) {
            case key_type::rsa2048: return EVP_RSA_gen(2048);
            case key_type::rsa3072: return EVP_RSA_gen(3072);
            case key_type::ec_p256: break;
        }
        return EVP_EC_gen("P-256");
    }

    std::pair<std::string, std::string> issue_leaf(const std::string& name, key_type type,
                                                   const std::vector<std::pair<int, std::string>>& extensions) const {
        EVP_PKEY* key = make_key(type);
        X509* certificate = make(name, key, certificate_, key_, extensions);
        auto certificate_pem = to_pem([&](BIO* bio) { PEM_write_bio_X509(bio, certificate); });
        auto key_pem = to_pem([&](BIO* bio) { PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr); });
        X509_free(certificate);
        EVP_PKEY_free(key);
        return {certificate_pem, key_pem};
    }

    template<typename Write>
    static std::string to_pem(Write write) {
        BIO* bio = BIO_new(BIO_s_mem());
        write(bio);
        char* data = nullptr;
        long size = BIO_get_mem_data(bio, &data);
        std::string pem(data, static_cast<size_t>(size));
        BIO_free(bio);
        return pem;
    }

    static X509* make(const std::string& name, EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key,
                      const std::vector<std::pair<int, std::string>>& extensions) {
        static std::atomic<long> serial{0};
        X509* certificate = X509_new();
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), static_cast<long>(std::time(nullptr)) + ++serial);
        X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 24 * 3600);
        X509_set_pubkey(certificate, key);
        X509_NAME* subject = X509_get_subject_name(certificate);
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0);
        X509_set_issuer_name(certificate, issuer ? X509_get_subject_name(issuer) : subject);

        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, issuer ? issuer : certificate, certificate, nullptr, nullptr, 0);
        for (const auto& [nid, value] : extensions) {
            X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
            X509_add_ext(certificate, extension, -1);
            X509_EXTENSION_free(extension);
        }
        X509_sign(certificate, issuer_key ? issuer_key : key, EVP_sha256());
        return certificate;
    }

    EVP_PKEY* key_ = nullptr;
    X509* certificate_ = nullptr;
    std::string pem_;
};

// The certificate the TLS test servers present for a name (SNI) while it exists
struct server_certificate {
    std::string host;

    server_certificate(std::string name, const std::pair<std::string, std::string>& certificate)
        : host(std::move(name)) {
        asio::certificate_manager::instance().set_certificate(host, certificate.first, certificate.second);
    }

    ~server_certificate() {
        asio::certificate_manager::instance().remove_certificate(host);
    }

    server_certificate(const server_certificate&) = delete;
    server_certificate& operator=(const server_certificate&) = delete;
};

// The client certificate mode of a host name (see certificate_manager::set_client_verify())
// while it exists: the manager is shared by the servers of the process
struct client_verify_host {
    std::string host;
    bool set;

    client_verify_host(std::string name, asio::client_verify mode)
        : host(std::move(name)), set(asio::certificate_manager::instance().set_client_verify(host, mode)) {}

    ~client_verify_host() {
        asio::certificate_manager::instance().remove_client_verify(host);
    }

    client_verify_host(const client_verify_host&) = delete;
    client_verify_host& operator=(const client_verify_host&) = delete;
};

} // namespace thinger::http::test

#endif // THINGER_HTTP_TEST_CA_HPP
