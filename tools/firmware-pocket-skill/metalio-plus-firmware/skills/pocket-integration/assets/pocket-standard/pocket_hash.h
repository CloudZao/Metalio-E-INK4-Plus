#pragma once
#include <cstddef>
#include <cstdint>

// ESP-IDF 6 uses PSA Crypto; the host-only path preserves the existing parser
// tests' OpenSSL-backed mbedTLS adapters. Neither path replaces image parsing.
#ifdef ESP_PLATFORM
#include <psa/crypto.h>
#else
#include <mbedtls/sha256.h>
#include <mbedtls/md5.h>
#endif

namespace pocket {
class Sha256 {
public:
    Sha256() {
#ifdef ESP_PLATFORM
        valid_ = psa_crypto_init() == PSA_SUCCESS &&
                 psa_hash_setup(&operation_, PSA_ALG_SHA_256) == PSA_SUCCESS;
#else
        mbedtls_sha256_init(&operation_);
        valid_ = mbedtls_sha256_starts(&operation_, 0) == 0;
#endif
    }
    ~Sha256() {
#ifdef ESP_PLATFORM
        (void)psa_hash_abort(&operation_);
#else
        mbedtls_sha256_free(&operation_);
#endif
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool Add(const void* bytes, std::size_t length) {
        if (!valid_) return false;
#ifdef ESP_PLATFORM
        valid_ = psa_hash_update(&operation_, static_cast<const uint8_t*>(bytes), length) == PSA_SUCCESS;
#else
        valid_ = mbedtls_sha256_update(&operation_, static_cast<const unsigned char*>(bytes), length) == 0;
#endif
        return valid_;
    }
    bool Finish(unsigned char* output) {
        if (!valid_) return false;
#ifdef ESP_PLATFORM
        std::size_t length = 0;
        const bool ok = psa_hash_finish(&operation_, output, 32, &length) == PSA_SUCCESS && length == 32;
#else
        const bool ok = mbedtls_sha256_finish(&operation_, output) == 0;
#endif
        valid_ = false;
        return ok;
    }
private:
#ifdef ESP_PLATFORM
    psa_hash_operation_t operation_ = PSA_HASH_OPERATION_INIT;
#else
    mbedtls_sha256_context operation_{};
#endif
    bool valid_ = false;
};
inline bool PartitionMd5(const unsigned char* bytes, std::size_t length, unsigned char* output) {
#ifdef ESP_PLATFORM
    std::size_t result_length = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_compute(PSA_ALG_MD5, bytes, length, output, 16, &result_length) == PSA_SUCCESS && result_length == 16;
#else
    return mbedtls_md5(bytes, length, output) == 0;
#endif
}
} // namespace pocket
