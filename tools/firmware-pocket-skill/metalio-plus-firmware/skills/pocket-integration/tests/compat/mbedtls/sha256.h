#pragma once
// Host test adapter only. Firmware uses ESP-IDF PSA; real Windows CNG hashes.
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
struct mbedtls_sha256_context{BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;};
inline void mbedtls_sha256_init(mbedtls_sha256_context* c){*c={};}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c,int){
    return BCryptOpenAlgorithmProvider(&c->alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)||
        BCryptCreateHash(c->alg,&c->hash,nullptr,0,nullptr,0,0)?-1:0;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c,const unsigned char* p,size_t n){
    return BCryptHashData(c->hash,const_cast<PUCHAR>(p),ULONG(n),0)?-1:0;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c,unsigned char* out){return BCryptFinishHash(c->hash,out,32,0)?-1:0;}
inline void mbedtls_sha256_free(mbedtls_sha256_context* c){if(c->hash)BCryptDestroyHash(c->hash);if(c->alg)BCryptCloseAlgorithmProvider(c->alg,0);*c={};}
