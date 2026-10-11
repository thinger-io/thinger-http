#ifndef THINGER_HTTP_TEST_CA_HPP
#define THINGER_HTTP_TEST_CA_HPP

#include <thinger/asio/ssl/certificate_manager.hpp>
#include <boost/asio/ip/address.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace thinger::http::test {

// A private certificate authority for TLS tests: its certificate (to trust it) and server
// certificates it issues
class test_ca {
public:
    explicit test_ca(const std::string& name = "Thinger Test CA") {
        key_ = EVP_EC_gen("P-256");
        certificate_ = make(name, key_, nullptr, nullptr, {
            {NID_basic_constraints, "critical,CA:TRUE"},
            {NID_key_usage, "critical,keyCertSign,cRLSign"},
        });
        pem_ = to_pem([&](BIO* bio) { PEM_write_bio_X509(bio, certificate_); });
    }

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
        EVP_PKEY* key = EVP_EC_gen("P-256");
        boost::system::error_code ec;
        boost::asio::ip::make_address(host, ec);
        std::string san = (ec ? "DNS:" : "IP:") + host;
        X509* certificate = make(host, key, certificate_, key_, {
            {NID_subject_alt_name, san},
            {NID_basic_constraints, "CA:FALSE"},
            {NID_key_usage, "critical,digitalSignature"},
            {NID_ext_key_usage, "serverAuth"},
        });
        auto certificate_pem = to_pem([&](BIO* bio) { PEM_write_bio_X509(bio, certificate); });
        auto key_pem = to_pem([&](BIO* bio) { PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr); });
        X509_free(certificate);
        EVP_PKEY_free(key);
        return {certificate_pem, key_pem};
    }

private:
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

} // namespace thinger::http::test

#endif // THINGER_HTTP_TEST_CA_HPP
